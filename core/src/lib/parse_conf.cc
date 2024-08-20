/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Master Configuration routines.
 *
 * This file contains the common parts of the BAREOS configuration routines.
 *
 * Note, the configuration file parser consists of four parts
 *
 * 1. The generic lexical scanner in lib/lex.c and lib/lex.h
 *
 * 2. The generic config scanner in lib/parse_conf.c and lib/parse_conf.h.
 *    These files contain the parser code, some utility routines,
 *
 * 3. The generic resource functions in lib/res.c
 *    Which form the common store routines (name, int, string, time,
 *    int64, size, ...).
 *
 * 4. The daemon specific file, which contains the Resource definitions
 *    as well as any specific store routines for the resource records.
 *
 * N.B. This is a two pass parser, so if you malloc() a string in a "store"
 * routine, you must ensure to do it during only one of the two passes, or to
 * free it between.
 *
 * Also, note that the resource record is malloced and saved in SaveResource()
 * during pass 1. Anything that you want saved after pass two (e.g. resource
 * pointers) must explicitly be done in SaveResource. Take a look at the Job
 * resource in src/dird/dird_conf.c to see how it is done.
 *
 * Kern Sibbald, January MM
 */

#include <algorithm>

#include "include/bareos.h"
#include "include/jcr.h"
#include "include/exit_codes.h"
#include "lib/address_conf.h"
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "lib/qualified_resource_name_type_converter.h"
#include "lib/bstringlist.h"
#include "lib/ascii_control_characters.h"
#include "lib/messages_resource.h"
#include "lib/resource_item.h"
#include "lib/berrno.h"
#include "lib/util.h"

bool PrintMessage(void*, const char* fmt, ...)
{
  va_list arg_ptr;

  va_start(arg_ptr, fmt);
  vfprintf(stdout, fmt, arg_ptr);
  va_end(arg_ptr);

  return true;
}

ConfigurationParser::ConfigurationParser() = default;
ConfigurationParser::~ConfigurationParser() = default;

ConfigurationParser::ConfigurationParser(
    const char* cf,
    LEX_ERROR_HANDLER* ScanError,
    LEX_WARNING_HANDLER* scan_warning,
    INIT_RES_HANDLER* init_res,
    STORE_RES_HANDLER* store_res,
    PRINT_RES_HANDLER* print_res,
    int32_t err_type,
    int32_t r_num,
    ResourceTable* resource_definitions,
    const char* config_default_filename,
    const char* config_include_dir,
    void (*ParseConfigBeforeCb)(ConfigurationParser&),
    void (*ParseConfigReadyCb)(ConfigurationParser&),
    DumpResourceCb_t DumpResourceCb,
    FreeResourceCb_t FreeResourceCb)
    : ConfigurationParser()
{
  cf_ = cf == nullptr ? "" : cf;
  use_config_include_dir_ = false;
  config_include_naming_format_ = "%s/%s/%s.conf";
  scan_error_ = ScanError;
  scan_warning_ = scan_warning;
  init_res_ = init_res;
  store_res_ = store_res;
  print_res_ = print_res;
  err_type_ = err_type;
  r_num_ = r_num;
  resource_definitions_ = resource_definitions;
  config_resources_container_.reset(new ConfigResourcesContainer(this));
  config_default_filename_
      = config_default_filename == nullptr ? "" : config_default_filename;
  config_include_dir_ = config_include_dir == nullptr ? "" : config_include_dir;
  ParseConfigBeforeCb_ = ParseConfigBeforeCb;
  ParseConfigReadyCb_ = ParseConfigReadyCb;
  ASSERT(DumpResourceCb);
  ASSERT(FreeResourceCb);
  DumpResourceCb_ = DumpResourceCb;
  FreeResourceCb_ = FreeResourceCb;
}

void ConfigurationParser::InitializeQualifiedResourceNameTypeConverter(
    const std::map<int, std::string>& map)
{
  qualified_resource_name_type_converter_.reset(
      new QualifiedResourceNameTypeConverter(map));
}

std::string ConfigurationParser::CreateOwnQualifiedNameForNetworkDump() const
{
  std::string qualified_name;

  if (own_resource_ && qualified_resource_name_type_converter_) {
    if (qualified_resource_name_type_converter_->ResourceToString(
            own_resource_->resource_name_, own_resource_->rcode_,
            "::", qualified_name)) {
      return qualified_name;
    }
  }
  return qualified_name;
}
void ConfigurationParser::ParseConfigOrExit()
{
  if (!ParseConfig()) {
    fprintf(stderr, "Configuration parsing error\n");
    exit(BEXIT_CONFIG_ERROR);
  }
}

bool ConfigurationParser::ParseConfig()
{
  int errstat;
  PoolMem config_path;

  if (ParseConfigBeforeCb_) ParseConfigBeforeCb_(*this);

  if (parser_first_run_ && (errstat = RwlInit(&res_lock_)) != 0) {
    BErrNo be;
    Jmsg1(nullptr, M_ABORT, 0,
          T_("Unable to initialize resource lock. ERR=%s\n"),
          be.bstrerror(errstat));
  }
  parser_first_run_ = false;

  if (!FindConfigPath(config_path)) {
    Jmsg0(nullptr, M_CONFIG_ERROR, 0, T_("Failed to find config filename.\n"));
  }
  used_config_path_ = config_path.c_str();
  Dmsg1(100, "config file = %s\n", used_config_path_.c_str());
  bool success = ParseConfigFile(config_path.c_str(), nullptr, scan_error_,
                                 scan_warning_);
  if (success && ParseConfigReadyCb_) { ParseConfigReadyCb_(*this); }

  config_resources_container_->SetTimestampToNow();

  return success;
}

void ConfigurationParser::lex_error(const char* cf,
                                    LEX_ERROR_HANDLER* ScanError,
                                    LEX_WARNING_HANDLER* scan_warning) const
{
  // We must create a lex packet to print the error
  LEX* lexical_parser_ = (LEX*)malloc(sizeof(LEX));
  memset(lexical_parser_, 0, sizeof(LEX));

  if (ScanError) {
    lexical_parser_->ScanError = ScanError;
  } else {
    LexSetDefaultErrorHandler(lexical_parser_);
  }

  if (scan_warning) {
    lexical_parser_->scan_warning = scan_warning;
  } else {
    LexSetDefaultWarningHandler(lexical_parser_);
  }

  LexSetErrorHandlerErrorType(lexical_parser_, err_type_);
  BErrNo be;
  scan_err2(lexical_parser_, T_("Cannot open config file \"%s\": %s\n"), cf,
            be.bstrerror());
  free(lexical_parser_);
}

parsable_resource ConfigurationParser::make_resource(const char* name)
{
  auto* table = GetResourceTable(name);
  if (!table || !table->items) { return {}; }

  BareosResource* res = table->make();

  ASSERT(res);

  InitResource(table->rcode, table->items, res);

  res->rcode_str_
      = GetQualifiedResourceNameTypeConverter()->ResourceTypeToString(
          table->rcode);

  return {res, table->items, (int)table->rcode};
}

bool ConfigurationParser::ParseConfigFile(const char* config_file_name,
                                          void* caller_ctx,
                                          LEX_ERROR_HANDLER* scan_error,
                                          LEX_WARNING_HANDLER* scan_warning)
{
  Dmsg1(900, "Enter ParseConfigFile(%s)\n", config_file_name);

  lex_ptr lexer = LexFile(config_file_name, caller_ctx, err_type_, scan_error,
                          scan_warning);

  if (!ParsingPass(lexer.get()) || !FixupPass() || !VerifyPass()) {
    return false;
  }


  Dmsg0(900, "Leave ParseConfigFile()\n");
  return true;
}

bool ConfigurationParser::AppendToResourcesChain(BareosResource* new_resource,
                                                 int rcode)
{
  int rindex = rcode;

  if (!new_resource->resource_name_) {
    Emsg1(M_ERROR, 0,
          T_("Name item is required in %s resource, but not found.\n"),
          resource_definitions_[rindex].name);
    return false;
  }

  if (!config_resources_container_->configuration_resources_[rindex]) {
    config_resources_container_->configuration_resources_[rindex]
        = new_resource;
    Dmsg3(900, "Inserting first %s res: %s index=%d\n", ResToStr(rcode),
          new_resource->resource_name_, rindex);
  } else {  // append
    BareosResource* last = nullptr;
    BareosResource* current
        = config_resources_container_->configuration_resources_[rindex];
    do {
      if (bstrcmp(current->resource_name_, new_resource->resource_name_)) {
        Emsg2(M_ERROR, 0,
              T_("Attempt to define second %s resource named \"%s\" is not "
                 "permitted.\n"),
              resource_definitions_[rindex].name, new_resource->resource_name_);
        return false;
      }
      last = current;
      current = last->next_;
    } while (current);
    last->next_ = new_resource;
    Dmsg3(900, T_("Inserting %s res: %s index=%d\n"), ResToStr(rcode),
          new_resource->resource_name_, rindex);
  }
  return true;
}

int ConfigurationParser::GetResourceTableIndex(const char* resource_type_name)
{
  for (int i = 0; resource_definitions_[i].name; i++) {
    if (Bstrcasecmp(resource_definitions_[i].name, resource_type_name)) {
      return i;
    }
  }

  return -1;
}

int ConfigurationParser::GetResourceCode(const char* resource_type_name)
{
  for (int i = 0; resource_definitions_[i].name; i++) {
    if (Bstrcasecmp(resource_definitions_[i].name, resource_type_name)) {
      return resource_definitions_[i].rcode;
    }
  }

  return -1;
}

ResourceTable* ConfigurationParser::GetResourceTable(
    const char* resource_type_name)
{
  int res_table_index = GetResourceTableIndex(resource_type_name);
  return &resource_definitions_[res_table_index];
}

int ConfigurationParser::GetResourceItemIndex(ResourceItem* resource_items_,
                                              const char* item)
{
  int result = -1;
  int i;

  for (i = 0; resource_items_[i].name; i++) {
    if (Bstrcasecmp(resource_items_[i].name, item)) {
      result = i;
      break;
    }
  }

  return result;
}

ResourceItem* ConfigurationParser::GetResourceItem(
    ResourceItem* resource_items_,
    const char* item)
{
  ResourceItem* result = nullptr;
  int i = -1;

  if (resource_items_) {
    i = GetResourceItemIndex(resource_items_, item);
    if (i >= 0) { result = &resource_items_[i]; }
  }

  return result;
}

#if defined(HAVE_WIN32)
struct default_config_dir {
  std::string path{DEFAULT_CONFIGDIR};

  default_config_dir()
  {
    if (dyn::SHGetKnownFolderPath) {
      PWSTR known_path{nullptr};
      HRESULT hr = dyn::SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr,
                                             &known_path);

      if (SUCCEEDED(hr)) {
        PoolMem utf8;
        if (int len = wchar_2_UTF8(utf8.addr(), known_path); len > 0) {
          if (utf8.c_str()[len - 1] == 0) {
            // do not copy zero terminator
            len -= 1;
          }
          path.assign(utf8.c_str(), len);
          path += "\\Bareos";
        }
      }

      CoTaskMemFree(known_path);
    }
  }
};
#endif

const char* ConfigurationParser::GetDefaultConfigDir()
{
#if defined(HAVE_WIN32)
  static default_config_dir def{};

  return def.path.c_str();
#else
  return CONFDIR;
#endif
}

bool ConfigurationParser::GetConfigFile(PoolMem& full_path,
                                        const char* config_dir,
                                        const char* config_filename)
{
  bool found = false;

  if (!PathIsDirectory(config_dir)) { return false; }

  if (config_filename) {
    full_path.strcpy(config_dir);
    if (PathAppend(full_path, config_filename)) {
      if (PathExists(full_path)) {
        config_dir_ = config_dir;
        found = true;
      }
    }
  }

  return found;
}

bool ConfigurationParser::GetConfigIncludePath(PoolMem& full_path,
                                               const char* config_dir)
{
  bool found = false;

  if (!config_include_dir_.empty()) {
    /* Set full_path to the initial part of the include path,
     * so it can be used as result, even on errors.
     * On success, full_path will be overwritten with the full path. */
    full_path.strcpy(config_dir);
    PathAppend(full_path, config_include_dir_.c_str());
    if (PathIsDirectory(full_path)) {
      config_dir_ = config_dir;
      // Set full_path to wildcard path.
      if (GetPathOfResource(full_path, nullptr, nullptr, nullptr, true)) {
        use_config_include_dir_ = true;
        found = true;
      }
    }
  }

  return found;
}

/*
 * Returns false on error
 *         true  on OK, with full_path set to where config file should be
 */
bool ConfigurationParser::FindConfigPath(PoolMem& full_path)
{
  bool found = false;
  PoolMem config_dir;
  PoolMem config_path_file;

  if (cf_.empty()) {
    // No path is given, so use the defaults.
    found = GetConfigFile(full_path, GetDefaultConfigDir(),
                          config_default_filename_.c_str());
    if (!found) {
      config_path_file.strcpy(full_path);
      found = GetConfigIncludePath(full_path, GetDefaultConfigDir());
    }
    if (!found) {
      Jmsg2(nullptr, M_ERROR, 0,
            T_("Failed to read config file at the default locations "
               "\"%s\" (config file path) and \"%s\" (config include "
               "directory).\n"),
            config_path_file.c_str(), full_path.c_str());
    }
  } else if (PathExists(cf_.c_str())) {
    // Path is given and exists.
    if (PathIsDirectory(cf_.c_str())) {
      found = GetConfigFile(full_path, cf_.c_str(),
                            config_default_filename_.c_str());
      if (!found) {
        config_path_file.strcpy(full_path);
        found = GetConfigIncludePath(full_path, cf_.c_str());
      }
      if (!found) {
        Jmsg3(nullptr, M_ERROR, 0,
              T_("Failed to find configuration files under directory \"%s\". "
                 "Did look for \"%s\" (config file path) and \"%s\" (config "
                 "include directory).\n"),
              cf_.c_str(), config_path_file.c_str(), full_path.c_str());
      }
    } else {
      full_path.strcpy(cf_.c_str());
      PathGetDirectory(config_dir, full_path);
      config_dir_ = config_dir.c_str();
      found = true;
    }
  } else if (config_default_filename_.empty()) {
    /* Compatibility with older versions.
     * If config_default_filename_ is not set,
     * cf_ may contain what is expected in config_default_filename_. */
    found = GetConfigFile(full_path, GetDefaultConfigDir(), cf_.c_str());
    if (!found) {
      Jmsg2(nullptr, M_ERROR, 0,
            T_("Failed to find configuration files at \"%s\" and \"%s\".\n"),
            cf_.c_str(), full_path.c_str());
    }
  } else {
    Jmsg1(nullptr, M_ERROR, 0, T_("Failed to read config file \"%s\"\n"),
          cf_.c_str());
  }

  return found;
}

void ConfigurationParser::RestoreResourcesContainer(
    std::shared_ptr<ConfigResourcesContainer>&& backup_table)
{
  std::swap(config_resources_container_, backup_table);
  backup_table.reset();
}

std::shared_ptr<ConfigResourcesContainer>
ConfigurationParser::BackupResourcesContainer()
{
  auto backup_table = config_resources_container_;
  config_resources_container_
      = std::make_shared<ConfigResourcesContainer>(this);
  return backup_table;
}

std::shared_ptr<ConfigResourcesContainer>
ConfigurationParser::GetResourcesContainer()
{
  return config_resources_container_;
}


bool ConfigurationParser::RemoveResource(int rcode, const char* name)
{
  int rindex = rcode;
  BareosResource* last;

  /* Remove resource from list.
   *
   * Note: this is intended for removing a resource that has just been added,
   * but proven to be incorrect (added by console command "configure add").
   * For a general approach, a check if this resource is referenced by other
   * resource_definitions must be added. If it is referenced, don't remove it.
   */
  last = nullptr;
  for (BareosResource* res
       = config_resources_container_->configuration_resources_[rindex];
       res; res = res->next_) {
    if (bstrcmp(res->resource_name_, name)) {
      if (!last) {
        Dmsg2(900,
              T_("removing resource %s, name=%s (first resource in list)\n"),
              ResToStr(rcode), name);
        config_resources_container_->configuration_resources_[rindex]
            = res->next_;
      } else {
        Dmsg2(900, T_("removing resource %s, name=%s\n"), ResToStr(rcode),
              name);
        last->next_ = res->next_;
      }
      res->next_ = nullptr;
      FreeResourceCb_(res, rcode);
      return true;
    }
    last = res;
  }

  // Resource with this name not found
  return false;
}

bool ConfigurationParser::DumpResources(bool sendit(void* sock,
                                                    const char* fmt,
                                                    ...),
                                        void* sock,
                                        const std::string& res_type_name,
                                        const std::string& res_name,
                                        bool hide_sensitive_data)
{
  bool result = false;
  if (res_type_name.empty()) {
    DumpResources(sendit, sock, hide_sensitive_data);
    result = true;
  } else {
    int res_type = GetResourceCode(res_type_name.c_str());
    if (res_type >= 0) {
      BareosResource* res = nullptr;
      if (res_name.empty()) {
        // No name, dump all resources of specified type
        res = GetNextRes(res_type, nullptr);
      } else {
        // Dump a single resource with specified name
        res = GetResWithName(res_type, res_name.c_str());
        res_type = -res_type;
      }
      if (res) { result = true; }
      DumpResourceCb_(res_type, res, sendit, sock, hide_sensitive_data, false);
    }
  }
  return result;
}

void ConfigurationParser::DumpResources(bool sendit(void* sock,
                                                    const char* fmt,
                                                    ...),
                                        void* sock,
                                        bool hide_sensitive_data)
{
  for (int i = 0; i <= r_num_ - 1; i++) {
    if (config_resources_container_->configuration_resources_[i]) {
      DumpResourceCb_(i,
                      config_resources_container_->configuration_resources_[i],
                      sendit, sock, hide_sensitive_data, false);
    }
  }
}

bool ConfigurationParser::GetPathOfResource(PoolMem& path,
                                            const char* component,
                                            const char* resourcetype,
                                            const char* name,
                                            bool set_wildcards)
{
  PoolMem rel_path(PM_FNAME);
  PoolMem directory(PM_FNAME);
  PoolMem resourcetype_lowercase(resourcetype);
  resourcetype_lowercase.toLower();

  if (!component) {
    if (!config_include_dir_.empty()) {
      component = config_include_dir_.c_str();
    } else {
      return false;
    }
  }

  if (resourcetype_lowercase.strlen() <= 0) {
    if (set_wildcards) {
      resourcetype_lowercase.strcpy("*");
    } else {
      return false;
    }
  }

  if (!name) {
    if (set_wildcards) {
      name = "*";
    } else {
      return false;
    }
  }

  path.strcpy(config_dir_.c_str());
  rel_path.bsprintf(config_include_naming_format_.c_str(), component,
                    resourcetype_lowercase.c_str(), name);
  PathAppend(path, rel_path);

  return true;
}

bool ConfigurationParser::GetPathOfNewResource(PoolMem& path,
                                               PoolMem& extramsg,
                                               const char* component,
                                               const char* resourcetype,
                                               const char* name,
                                               bool error_if_exists,
                                               bool create_directories)
{
  PoolMem rel_path(PM_FNAME);
  PoolMem directory(PM_FNAME);
  PoolMem resourcetype_lowercase(resourcetype);
  resourcetype_lowercase.toLower();

  if (!GetPathOfResource(path, component, resourcetype, name, false)) {
    return false;
  }

  PathGetDirectory(directory, path);

  if (create_directories) { PathCreate(directory); }

  if (!PathExists(directory)) {
    extramsg.bsprintf("Resource config directory \"%s\" does not exist.\n",
                      directory.c_str());
    return false;
  }

  /* Store name for temporary file in extramsg.
   * Can be used, if result is true.
   * Otherwise it contains an error message. */
  extramsg.bsprintf("%s.tmp", path.c_str());

  if (!error_if_exists) { return true; }

  // File should not exists, as it is going to be created.
  if (PathExists(path)) {
    extramsg.bsprintf("Resource config file \"%s\" already exists.\n",
                      path.c_str());
    return false;
  }

  if (PathExists(extramsg)) {
    extramsg.bsprintf(
        "Temporary resource config file \"%s.tmp\" already exists.\n",
        path.c_str());
    return false;
  }

  return true;
}

void ConfigurationParser::AddWarning(const std::string& warning)
{
  warnings_ << warning;
}

void ConfigurationParser::ClearWarnings() { warnings_.clear(); }

bool ConfigurationParser::HasWarnings() const { return !warnings_.empty(); }

const BStringList& ConfigurationParser::GetWarnings() const
{
  return warnings_;
}

auto ConfigurationParser::NextResourceIdentifier(LEX* lex)
    -> std::variant<done, ident, unexpected_token>
{
  for (;;) {
    auto token = LexGetToken(lex, BCT_ALL);
    switch (token) {
      case BCT_IDENTIFIER: {
        return ident{lex->str};
      } break;
      case BCT_EOL: {
        // continue on
      } break;
      case BCT_EOF: {
        return done{};
      } break;
      default: {
        return unexpected_token{token};
      } break;
    }
  }
}

parse_result ConfigurationParser::ParseResource(BareosResource* res,
                                                ResourceItem* items,
                                                LEX* lex,
                                                STORE_RES_HANDLER* store)
{
  int open_blocks = 0;
  for (;;) {
    int token = LexGetToken(lex, BCT_ALL);
    switch (token) {
      case BCT_BOB: {
        open_blocks += 1;
      } break;
      case BCT_EOB: {
        open_blocks -= 1;

        if (open_blocks == 0) {
          return {};
        } else if (open_blocks < 0) {
          return parse_result("unexpected end of block");
        }
      } break;
      case BCT_IDENTIFIER: {
        int resource_item_index = GetResourceItemIndex(items, lex->str);

        if (resource_item_index < 0) {
          Dmsg2(900, "config_level_=%d id=%s\n", open_blocks, lex->str);
          Dmsg1(900, "Keyword = %s\n", lex->str);

          PoolMem errmsg;
          Mmsg(errmsg,
               "Keyword \"%s\" not permitted in this resource.\n"
               "Perhaps you left the trailing brace off of the "
               "previous resource.",
               lex->str);
          return parse_result(errmsg.c_str());
        }

        ResourceItem* item = &items[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lex, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            PoolMem errmsg;
            Mmsg(errmsg, "expected an equals, got: %s", lex->str);
            return parse_result(errmsg.c_str());
          }
        }

        if (item->flags & CFG_ITEM_DEPRECATED) {
          AddWarning(std::string("using deprecated keyword ") + item->name
                     + " on line " + std::to_string(lex->line_no) + " of file "
                     + lex->fname);
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!StoreResource(res, item->type, lex, item, resource_item_index)) {
          if (store) { store(this, res, lex, item, resource_item_index); }
        }
      } break;
      case BCT_EOL: {
        // continue on
      } break;
      case BCT_EOF: {
        return parse_result("End of conf file reached with unclosed resource.");
      } break;

      default: {
        PoolMem errmsg;
        Mmsg(errmsg, "unexpected token %d %s in resource definition", token,
             lex_tok_to_str(token));
        return parse_result(errmsg.c_str());
      } break;
    }
  }
}

bool ConfigurationParser::ParsingPass(LEX* lex)
{
  Dmsg1(900, "Enter Parsing Pass\n");

  for (;;) {
    auto res = NextResourceIdentifier(lex);

    if (std::get_if<done>(&res) != nullptr) {
      break;
    } else if (auto* token = std::get_if<unexpected_token>(&res)) {
      scan_err2(lex, T_("Expected a Resource name identifier, got: %d %s"),
                token->value, lex_tok_to_str(token->value));
      return false;
    }

    auto& str = std::get<ident>(res).name;

    Dmsg1(900, "Start Resource(%s)\n", str.c_str());

    auto new_res = make_resource(str.c_str());

    if (!new_res) {
      scan_err1(lex, "Could not allocate %s resource.", str.c_str());
      return false;
    }

    auto parse_res = ParseResource(new_res.res, new_res.items, lex, store_res_);
    if (!parse_res) {
      scan_err1(lex, "%s", parse_res.strerror());
      return false;
    }

    // if (!SaveResourceCb_(new_res.res, new_res.code, new_res.items)) {
    //   scan_err0(lex, "SaveResource failed");
    // }

    AppendToResourcesChain(new_res.res, new_res.code);
  }
  Dmsg1(900, "Leave Parsing Pass\n");
  return true;
}

bool ConfigurationParser::FixupPass()
{
  Dmsg1(900, "Enter Fixup Pass\n");

  for (auto& [tgt, depname] : single_dependencies) {
    auto* destination
        = GetItemVariablePointer<BareosResource**>(tgt.base, *tgt.item);

    if (!destination) {
      // error : tgt.base does not have member tgt.item->name
      return false;
    }

    if (*destination) {
      // error : trying to overwrite defined resource
      return false;
    }

    Dmsg3(900, "Setting %s->%s to %s\n", tgt.base->resource_name_,
          tgt.item->name, depname.c_str());

    auto* dependency = GetResWithName(tgt.item->code, depname.c_str());
    if (!dependency) {
      // error : no such dependency defined
      return false;
    }

    *destination = dependency;
  }

  for (auto& [tgt, deps] : alist_dependencies) {
    auto* destination
        = GetItemVariablePointer<alist<BareosResource*>**>(tgt.base, *tgt.item);
    if (!destination) {
      // error : tgt.base does not have member tgt.item->name
      return false;
    }

    if (!*destination) {
      *destination
          = new alist<BareosResource*>(deps.size(), not_owned_by_alist);
    }

    for (auto& depname : deps) {
      Dmsg3(900, "Appending %s to %s->%s\n", depname.c_str(),
            tgt.base->resource_name_, tgt.item->name);

      auto* dependency = GetResWithName(tgt.item->code, depname.c_str());
      if (!dependency) {
        // error : no such dependency defined
        return false;
      }

      (*destination)->append(dependency);
    }
  }

  for (auto& [tgt, deps] : vector_dependencies) {
    auto* destination = GetItemVariablePointer<std::vector<BareosResource*>*>(
        tgt.base, *tgt.item);

    if (!destination) {
      // error : tgt.base does not have member tgt.item->name
      return false;
    }

    for (auto& depname : deps) {
      Dmsg3(900, "Appending %s to %s->%s\n", depname.c_str(),
            tgt.base->resource_name_, tgt.item->name);

      auto* dependency = GetResWithName(tgt.item->code, depname.c_str());
      if (!dependency) {
        // error
        return false;
      }

      destination->push_back(dependency);
    }
  }

  for (auto* cb : fixup_cbs) {
    if (!(*cb)(this)) { return false; }
  }

  Dmsg1(900, "Leave Fixup Pass\n");
  return true;
}

static bool CheckRequired(BareosResource* res, ResourceTable* tbl)
{
  ResourceItem* current = tbl->items;

  while (current->name) {
    if ((current->flags & CFG_ITEM_REQUIRED) && (!current->IsPresent(res))) {
      /* MARKER */
      // how to do this with scan_err, when we have no lexer ?
      Emsg2(M_ERROR, 0,
            T_("%s item is required in %s resource, but not found (%s).\n"),
            current->name, tbl->name, res->resource_name_);
      return false;
    }

    current += 1;
  }

  return true;
}

bool ConfigurationParser::VerifyPass()
{
  Dmsg1(900, "Enter Verify Pass\n");

  bool ok = true;

  for (int i = 0; i <= r_num_ - 1; i++) {
    BareosResource** current
        = &config_resources_container_->configuration_resources_[i];
    ResourceTable* tbl = &resource_definitions_[i];

    while (*current) {
      // every resource has to pass validation, but we also want to validate
      // all of them before returning
      ok &= CheckRequired(*current, tbl);
      current = &(*current)->next_;
    }
  }

  if (!ok) { return false; }

  // Dump all resources for debugging purposes
  if (debug_level >= 900) {
    for (int i = 0; i <= r_num_ - 1; i++) {
      DumpResourceCb_(i,
                      config_resources_container_->configuration_resources_[i],
                      PrintMessage, nullptr, false, false);
    }
  }
  Dmsg1(900, "Leave Verify Pass\n");
  return true;
}

bool ConfigurationParser::AddDependency(DependencyStorageType type,
                                        BareosResource* res,
                                        ResourceItem* item,
                                        std::string_view referenced_name)
{
  dependency_target tgt{res, item};

  switch (type) {
    case DependencyStorageType::SINGLE: {
      if (auto found = single_dependencies.find(tgt);
          found != single_dependencies.end()) {
        // error
        return false;
      }
      single_dependencies.emplace(tgt, referenced_name);
    } break;
    case DependencyStorageType::ALIST: {
      alist_dependencies[tgt].emplace_back(referenced_name);
    } break;
    case DependencyStorageType::VECTOR: {
      vector_dependencies[tgt].emplace_back(referenced_name);
    } break;
  }

  return true;
}


void ConfigurationParser::InsertResource(int resource_type, BareosResource* res)
{
  // TODO: merge with AppendToResourcesChain
  auto* resources
      = config_resources_container_->configuration_resources_[resource_type];
  if (!resources) {
    config_resources_container_->configuration_resources_[resource_type] = res;

    return;
  }

  while (resources->next_) { resources = resources->next_; }

  resources->next_ = res;
}

void ConfigurationParser::AddFixupCallback(config_fixuper* cb)
{
  fixup_cbs.push_back(cb);
}
