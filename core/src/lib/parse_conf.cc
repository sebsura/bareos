/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2025 Bareos GmbH & Co. KG

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
#include <string_view>

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
    gsl::span<const ResourceTable> resource_definitions,
    const char* config_default_filename,
    const char* config_include_dir,
    void (*ParseConfigBeforeCb)(ConfigurationParser&),
    void (*ParseConfigReadyCb)(ConfigurationParser&),
    SaveResourceCb_t SaveResourceCb,
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
  resource_definitions_ = resource_definitions;
  config_default_filename_
      = config_default_filename == nullptr ? "" : config_default_filename;
  config_include_dir_ = config_include_dir == nullptr ? "" : config_include_dir;
  ParseConfigBeforeCb_ = ParseConfigBeforeCb;
  ParseConfigReadyCb_ = ParseConfigReadyCb;
  ASSERT(SaveResourceCb);
  ASSERT(DumpResourceCb);
  ASSERT(FreeResourceCb);
  SaveResourceCb_ = SaveResourceCb;
  DumpResourceCb_ = DumpResourceCb;
  FreeResourceCb_ = FreeResourceCb;

  // config resources container needs to access our members, so this
  // needs to always happen after we initialised everything else
  config_resources_container_
      = std::make_shared<ConfigResourcesContainer>(this);
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

bool ConfigurationParser::ParseConfigFile(const char* config_file_name,
                                          void* caller_ctx,
                                          LEX_ERROR_HANDLER* scan_error,
                                          LEX_WARNING_HANDLER* scan_warning)
{
  Dmsg1(900, "Enter ParseConfigFile(%s)\n", config_file_name);

  for (int parser_pass = 1; parser_pass <= 2; ++parser_pass) {
    // --- init parser pass ---
    Dmsg1(900, "ParseConfig parser_pass_number_ %d\n", parser_pass);

    struct LexCloser {
      void operator()(LEX* lex) const
      {
        while (lex) { lex = LexCloseFile(lex); }
      }
    };

    std::unique_ptr<LEX, LexCloser> lexical_parser{
        lex_open_file(nullptr, config_file_name, scan_error, scan_warning)};
    if (!lexical_parser) {
      lex_error(config_file_name, scan_error, scan_warning);
      // scan_err0(lexical_parser, T_("ParseAllTokens failed."));
      return false;
    }

    LexSetErrorHandlerErrorType(lexical_parser.get(), err_type_);

    lexical_parser->error_counter = 0;
    lexical_parser->caller_ctx = caller_ctx;
    // --- init parser pass end ---

    // --- parse all tokens ---
    {
      struct CurrentResource {
        FreeResourceCb_t free_res;

        CurrentResource(FreeResourceCb_t free_res_) : free_res{free_res_} {}
        CurrentResource(const CurrentResource& free_res_) = delete;
        CurrentResource& operator=(const CurrentResource& free_res_) = delete;
        CurrentResource(CurrentResource&& free_res_) = delete;
        CurrentResource& operator=(CurrentResource&& free_res_) = delete;

        void set(BareosResource* res_, const ResourceTable* tbl_)
        {
          res = res_;
          tbl = tbl_;
        }

        void release()
        {
          res = {};
          tbl = {};
        }

        void reset()
        {
          if (res) { free_res(res, tbl->rcode); }
          res = {};
          tbl = {};
        }

        operator bool() const { return res != nullptr; }

        ~CurrentResource() { reset(); }

        BareosResource* res{};
        const ResourceTable* tbl{};
      };

      CurrentResource current{FreeResourceCb_};
      int token;
      size_t config_level = 0;

      while ((token = LexGetToken(lexical_parser.get(), BCT_ALL)) != BCT_EOF) {
        Dmsg3(900, "parse resource=%p parser_pass_number_=%d got token=%s\n",
              current.res, parser_pass, lex_tok_to_str(token));

        auto parsed_text = lexical_parser->str;
        if (!current) {
          switch (token) {
            case BCT_EOL:
            case BCT_UTF8_BOM: {
              continue;
            }
            case BCT_UTF16_BOM: {
              scan_err0(lexical_parser.get(),
                        T_("Currently we cannot handle UTF-16 source files. "
                           "Please convert the conf file to UTF-8\n"));
              return false;
            }
            case BCT_IDENTIFIER: {
              // intentionally left blank; continues after switch
            } break;
            default: {
              scan_err1(lexical_parser.get(),
                        T_("Expected a Resource name identifier, got: %s"),
                        parsed_text);
              return false;
            }
          }

          ASSERT(token == BCT_IDENTIFIER);

          auto* resource_table = GetResourceTable(parsed_text);
          if (!resource_table) {
            scan_err1(lexical_parser.get(),
                      T_("Expected a Resource name identifier, got: %s"),
                      parsed_text);
            return false;
          }

          // TODO: this should just be a static assert
          if (resource_table->items.empty()) {
            scan_err1(
                lexical_parser.get(),
                T_("Internal parse error at %s.  No Resource Items found.\n"),
                parsed_text);
            return false;
          }

          current.set(resource_table->create_resource(), resource_table);

          if (!current) {
            scan_err1(lexical_parser.get(),
                      T_("Expected a Resource name identifier, got: %s"),
                      parsed_text);
            return false;
          }

          SetAllResourceDefaultsByParserPass(current.res, current.tbl->rcode,
                                             current.tbl->items, parser_pass);
        } else {
          switch (token) {
            case BCT_BOB: {
              config_level += 1;
            } break;
            case BCT_IDENTIFIER: {
              if (config_level != 1) {
                scan_err1(lexical_parser.get(),
                          T_("not in resource definition: %s"),
                          lexical_parser->str);
                return false;
              }

              int resource_item_index = GetResourceItemIndex(
                  current.tbl->items, lexical_parser->str);

              if (resource_item_index >= 0
                  && (size_t)resource_item_index < current.tbl->items.size()) {
                const ResourceItem* item = nullptr;
                item = &current.tbl->items[resource_item_index];
                if (!item->has_no_eq()) {
                  token = LexGetToken(lexical_parser.get(), BCT_SKIP_EOL);
                  Dmsg1(900, "in BCT_IDENT got token=%s\n",
                        lex_tok_to_str(token));
                  if (token != BCT_EQUALS) {
                    scan_err1(lexical_parser.get(),
                              T_("expected an equals, got: %s"),
                              lexical_parser->str);
                    return false;
                  }
                }

                if (parser_pass == 1 && item->is_deprecated()) {
                  AddWarning(std::string("using deprecated keyword ")
                             + item->name + " on line "
                             + std::to_string(lexical_parser->line_no)
                             + " of file " + lexical_parser->fname);
                }

                Dmsg1(800, "calling handler for %s\n", item->name);

                if (!StoreResource(this, current.res, item->type,
                                   lexical_parser.get(), item, parser_pass)) {
                  if (store_res_) {
                    store_res_(
                        this, current.res, lexical_parser.get(), item,
                        parser_pass,
                        config_resources_container_->configuration_resources_);
                  }
                }
              } else {
                Dmsg2(900, "config_level_=%d id=%s\n", config_level,
                      lexical_parser->str);
                Dmsg1(900, "Keyword = %s\n", lexical_parser->str);
                scan_err1(lexical_parser.get(),
                          T_("Keyword \"%s\" not permitted in this resource.\n"
                             "Perhaps you left the trailing brace off of the "
                             "previous resource."),
                          lexical_parser->str);
                return false;
              }
            } break;
            case BCT_EOB: {
              config_level -= 1;
              Dmsg0(900, "BCT_EOB => define new resource\n");
              if (!current.res->resource_name_) {
                scan_err0(lexical_parser.get(),
                          T_("Name not specified for resource"));
                return false;
              }

              /* save resource */
              if (!SaveResourceCb_(current.res, *current.tbl, parser_pass)) {
                scan_err0(lexical_parser.get(), T_("SaveResource failed"));
                return false;
              }

              if (parser_pass == 1) {
                // dont free the resource from pass 1, as thats the one
                // that gets saved
                current.release();
              } else {
                // free the resource from pass 2 as its not used anymore
                if (current.res) free(current.res->resource_name_);
                delete current.res;
                current.release();
              }
            } break;
            case BCT_EOL: {
              // intentionally left blank
            } break;

            default: {
              scan_err2(lexical_parser.get(),
                        T_("unexpected token %d %s in resource definition"),
                        token, lex_tok_to_str(token));
              return false;
            }
          }
        }
      }

      if (current) {
        scan_err0(lexical_parser.get(),
                  T_("End of conf file reached with unclosed resource."));
        return false;
      }
    }

    // --- parse all tokens end ---

    // --- get parse error ---

    if (lexical_parser->error_counter > 0) {
      scan_err0(lexical_parser.get(), T_("Parser Error occurred."));
      return false;
    }

    // --- get parse error end ---
  }

  if (debug_level >= 900) {
    for (size_t i = 0;
         i < config_resources_container_->configuration_resources_.size();
         i++) {
      DumpResourceCb_(i,
                      config_resources_container_->configuration_resources_[i],
                      PrintMessage, nullptr, false, false);
    }
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
  for (size_t i = 0; i < resource_definitions_.size(); i++) {
    if (Bstrcasecmp(resource_definitions_[i].name, resource_type_name)) {
      return i;
    }
    if (const auto& alias = resource_definitions_[i].alias) {
      if (Bstrcasecmp(alias->name, resource_type_name)) {
        std::string warning
            = "Found resource alias usage \"" + std::string(alias->name)
              + "\" in configuration which is discouraged, consider using \""
              + resource_definitions_[i].name + "\" instead.";
        if (std::find(warnings_.begin(), warnings_.end(), warning)
            == warnings_.end()) {
          AddWarning(warning);
        }
        return i;
      }
    }
  }

  return -1;
}

int ConfigurationParser::GetResourceCode(const char* resource_type_name)
{
  int index = GetResourceTableIndex(resource_type_name);
  if (index >= 0) { return resource_definitions_[index].rcode; }
  return -1;
}

const ResourceTable* ConfigurationParser::GetResourceTable(
    const char* resource_type_name)
{
  int res_table_index = GetResourceTableIndex(resource_type_name);
  if (res_table_index == -1) { return nullptr; }
  return &resource_definitions_[res_table_index];
}

int ConfigurationParser::GetResourceItemIndex(
    gsl::span<const ResourceItem> resource_items,
    const char* item_name)
{
  auto is_name_for_item
      = [this](const ResourceItem& item, const char* name) -> bool {
    if (Bstrcasecmp(item.name, name)) { return true; }

    if (item.alias && Bstrcasecmp(item.alias, name)) {
      std::string warning
          = "Found alias usage \"" + std::string{item.alias}
            + "\" in configuration which is discouraged, consider using \""
            + item.name + "\" instead.";
      if (std::find(warnings_.begin(), warnings_.end(), warning)
          == warnings_.end()) {
        AddWarning(warning);
      }
      return true;
    }

    return false;
  };
  if (auto found = std::find_if(resource_items.begin(), resource_items.end(),
                                [item_name, &is_name_for_item](auto& item) {
                                  return is_name_for_item(item, item_name);
                                });
      found != resource_items.end()) {
    return found - resource_items.begin();
  }

  return -1;
}

const ResourceItem* ConfigurationParser::GetResourceItem(
    gsl::span<const ResourceItem> resource_items,
    const char* item_name)
{
  auto idx = GetResourceItemIndex(resource_items, item_name);
  if (idx < 0) { return nullptr; }
  return &resource_items[idx];
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
  for (size_t i = 0; i < resource_definitions_.size(); i++) {
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
