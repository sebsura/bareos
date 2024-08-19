/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.
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
// Kern Sibbald, March MMIII
/*
 * @file
 * Configuration file parser for new and old Include and
 * Exclude records
 */

#include "include/bareos.h"
#include "dird.h"
#include "dird/dird_globals.h"
#include "dird/director_jcr_impl.h"
#include "findlib/match.h"
#include "lib/parse_conf.h"
#include "include/compiler_macro.h"

#ifndef HAVE_REGEX_H
#  include "lib/bregex.h"
#else
#  include <regex.h>
#endif
#include "findlib/find.h"

#include "inc_conf.h"
#include "lib/edit.h"

#include <cassert>

namespace directordaemon {

#define PERMITTED_VERIFY_OPTIONS (const char*)"ipnugsamcd51"
#define PERMITTED_ACCURATE_OPTIONS (const char*)"ipnugsamcd51A"
#define PERMITTED_BASEJOB_OPTIONS (const char*)"ipnugsamcd51"

typedef struct {
  bool configured;
  std::string default_value;
} options_default_value_s;


// Define FileSet KeyWord values
enum
{
  INC_KW_NONE,
  INC_KW_COMPRESSION,
  INC_KW_DIGEST,
  INC_KW_ENCRYPTION,
  INC_KW_VERIFY,
  INC_KW_BASEJOB,
  INC_KW_ACCURATE,
  INC_KW_ONEFS,
  INC_KW_RECURSE,
  INC_KW_SPARSE,
  INC_KW_HARDLINK,
  INC_KW_REPLACE,  /* restore options */
  INC_KW_READFIFO, /* Causes fifo data to be read */
  INC_KW_PORTABLE,
  INC_KW_MTIMEONLY,
  INC_KW_KEEPATIME,
  INC_KW_EXCLUDE,
  INC_KW_ACL,
  INC_KW_IGNORECASE,
  INC_KW_HFSPLUS,
  INC_KW_NOATIME,
  INC_KW_ENHANCEDWILD,
  INC_KW_CHKCHANGES,
  INC_KW_STRIPPATH,
  INC_KW_HONOR_NODUMP,
  INC_KW_XATTR,
  INC_KW_SIZE,
  INC_KW_SHADOWING,
  INC_KW_AUTO_EXCLUDE,
  INC_KW_FORCE_ENCRYPTION
};

/*
 * This is the list of options that can be stored by store_opts
 * Note, now that the old style Include/Exclude code is gone,
 * the INC_KW code could be put into the "code" field of the
 * options given above.
 */

// Options for FileSet keywords
struct s_fs_opt {
  const char* name;
  int keyword;
  const char* option;
};

/*
 * Options permitted for each keyword and resulting value.
 * The output goes into opts, which are then transmitted to
 * the FD for application as options to the following list of
 * included files.
 */
static struct s_fs_opt FS_options[]
    = {{"md5", INC_KW_DIGEST, "M"},
       {"sha1", INC_KW_DIGEST, "S"},
       {"sha256", INC_KW_DIGEST, "S2"},
       {"sha512", INC_KW_DIGEST, "S3"},
       {"xxh128", INC_KW_DIGEST, "S4"},
       {"gzip", INC_KW_COMPRESSION, "Z6"},
       {"gzip1", INC_KW_COMPRESSION, "Z1"},
       {"gzip2", INC_KW_COMPRESSION, "Z2"},
       {"gzip3", INC_KW_COMPRESSION, "Z3"},
       {"gzip4", INC_KW_COMPRESSION, "Z4"},
       {"gzip5", INC_KW_COMPRESSION, "Z5"},
       {"gzip6", INC_KW_COMPRESSION, "Z6"},
       {"gzip7", INC_KW_COMPRESSION, "Z7"},
       {"gzip8", INC_KW_COMPRESSION, "Z8"},
       {"gzip9", INC_KW_COMPRESSION, "Z9"},
       {"lzo", INC_KW_COMPRESSION, "Zo"},
       {"lzfast", INC_KW_COMPRESSION, "Zff"},
       {"lz4", INC_KW_COMPRESSION, "Zf4"},
       {"lz4hc", INC_KW_COMPRESSION, "Zfh"},
       {"blowfish", INC_KW_ENCRYPTION, "Eb"},
       {"3des", INC_KW_ENCRYPTION, "E3"},
       {"aes128", INC_KW_ENCRYPTION, "Ea1"},
       {"aes192", INC_KW_ENCRYPTION, "Ea2"},
       {"aes256", INC_KW_ENCRYPTION, "Ea3"},
       {"camellia128", INC_KW_ENCRYPTION, "Ec1"},
       {"camellia192", INC_KW_ENCRYPTION, "Ec2"},
       {"camellia256", INC_KW_ENCRYPTION, "Ec3"},
       {"aes128hmacsha1", INC_KW_ENCRYPTION, "Eh1"},
       {"aes256hmacsha1", INC_KW_ENCRYPTION, "Eh2"},
       {"yes", INC_KW_ONEFS, "0"},
       {"no", INC_KW_ONEFS, "f"},
       {"yes", INC_KW_RECURSE, "0"},
       {"no", INC_KW_RECURSE, "h"},
       {"yes", INC_KW_SPARSE, "s"},
       {"no", INC_KW_SPARSE, "0"},
       {"yes", INC_KW_HARDLINK, "0"},
       {"no", INC_KW_HARDLINK, "H"},
       {"always", INC_KW_REPLACE, "a"},
       {"ifnewer", INC_KW_REPLACE, "w"},
       {"never", INC_KW_REPLACE, "n"},
       {"yes", INC_KW_READFIFO, "r"},
       {"no", INC_KW_READFIFO, "0"},
       {"yes", INC_KW_PORTABLE, "p"},
       {"no", INC_KW_PORTABLE, "0"},
       {"yes", INC_KW_MTIMEONLY, "m"},
       {"no", INC_KW_MTIMEONLY, "0"},
       {"yes", INC_KW_KEEPATIME, "k"},
       {"no", INC_KW_KEEPATIME, "0"},
       {"yes", INC_KW_EXCLUDE, "e"},
       {"no", INC_KW_EXCLUDE, "0"},
       {"yes", INC_KW_ACL, "A"},
       {"no", INC_KW_ACL, "0"},
       {"yes", INC_KW_IGNORECASE, "i"},
       {"no", INC_KW_IGNORECASE, "0"},
       {"yes", INC_KW_HFSPLUS, "R"}, /* "R" for resource fork */
       {"no", INC_KW_HFSPLUS, "0"},
       {"yes", INC_KW_NOATIME, "K"},
       {"no", INC_KW_NOATIME, "0"},
       {"yes", INC_KW_ENHANCEDWILD, "K"},
       {"no", INC_KW_ENHANCEDWILD, "0"},
       {"yes", INC_KW_CHKCHANGES, "c"},
       {"no", INC_KW_CHKCHANGES, "0"},
       {"yes", INC_KW_HONOR_NODUMP, "N"},
       {"no", INC_KW_HONOR_NODUMP, "0"},
       {"yes", INC_KW_XATTR, "X"},
       {"no", INC_KW_XATTR, "0"},
       {"localwarn", INC_KW_SHADOWING, "d1"},
       {"localremove", INC_KW_SHADOWING, "d2"},
       {"globalwarn", INC_KW_SHADOWING, "d3"},
       {"globalremove", INC_KW_SHADOWING, "d4"},
       {"none", INC_KW_SHADOWING, "0"},
       {"yes", INC_KW_AUTO_EXCLUDE, "0"},
       {"no", INC_KW_AUTO_EXCLUDE, "x"},
       {"yes", INC_KW_FORCE_ENCRYPTION, "Ef"},
       {"no", INC_KW_FORCE_ENCRYPTION, "0"},
       {NULL, 0, 0}};

// Imported subroutines
extern void StoreInc(LEX* lc, ResourceItem* item, int index, int pass);

/* clang-format off */

/* new Include/Exclude items
 * name handler value code flags default_value */
ResourceItem newinc_items[] = {
  { "File", CFG_TYPE_FNAME, ITEM(IncludeExcludeItem, name_list), 0, 0, NULL, NULL, NULL },
  { "Plugin", CFG_TYPE_PLUGINNAME, ITEM(IncludeExcludeItem, plugin_list), 0, 0, NULL, NULL, NULL },
  { "ExcludeDirContaining", CFG_TYPE_ALIST_NAME, ITEM(IncludeExcludeItem, ignoredir), 0, 0, NULL, NULL, NULL },
  { "Options", CFG_TYPE_OPTIONS, ITEM(IncludeExcludeItem, file_options_list), 0, CFG_ITEM_NO_EQUALS, NULL, NULL, NULL },
  {}
};

ResourceItem newexc_items[] = {
  { "File", CFG_TYPE_FNAME, ITEM(IncludeExcludeItem, name_list), 0, 0, NULL, NULL, NULL },
  {}
};

/* Items that are valid in an Options resource
 * name handler value code flags default_value */

ResourceItem options_items[] = {
  { "Accurate", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_ACCURATE, 0, NULL, NULL, NULL },
  { "AclSupport", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_ACL, 0, NULL, NULL, NULL },
  { "AutoExclude", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_AUTO_EXCLUDE, 0, NULL, NULL, NULL },
  { "BaseJob", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_BASEJOB, CFG_ITEM_DEPRECATED, NULL, NULL, NULL },
  { "CheckFileChanges", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_CHKCHANGES, 0, NULL, NULL, NULL },
  { "Compression", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_COMPRESSION, 0, NULL, NULL, NULL },
  /* MARKER */ // missing encryption ?
  { "Encryption", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_ENCRYPTION, 0, NULL, NULL, NULL },
  { "EnhancedWild", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_ENHANCEDWILD, 0, NULL, NULL, NULL },
  { "Exclude", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_EXCLUDE, 0, NULL, NULL, NULL },
  { "ForceEncryption", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_FORCE_ENCRYPTION, 0, NULL, NULL, NULL },
  { "HardLinks", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_HARDLINK, 0, NULL, NULL, NULL },
  { "HfsPlusSupport", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_HFSPLUS, 0, NULL, NULL, NULL },
  { "HonornoDumpFlag", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_HONOR_NODUMP, 0, NULL, NULL, NULL },
  { "IgnoreCase", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_IGNORECASE, 0, NULL, NULL, NULL },
  { "KeepAtime", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_KEEPATIME, 0, NULL, NULL, NULL },
  { "MtimeOnly", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_MTIMEONLY, 0, NULL, NULL, NULL },
  { "NoAtime", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_NOATIME, 0, NULL, NULL, NULL },
  { "OneFs", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_ONEFS, 0, NULL, NULL, NULL },
  { "Portable", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_PORTABLE, 0, NULL, NULL, NULL },
  { "ReadFifo", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_READFIFO, 0, NULL, NULL, NULL },
  { "Recurse", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_RECURSE, 0, NULL, NULL, NULL },
  { "Replace", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_REPLACE, 0, NULL, NULL, NULL },
  { "Shadowing", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_SHADOWING, 0, NULL, NULL, NULL },
  { "Signature", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_DIGEST, 0, NULL, NULL, NULL },
  { "Size", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_SIZE, 0, NULL, NULL, NULL },
  { "Sparse", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_SPARSE, 0, NULL, NULL, NULL },
  { "StripPath", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_STRIPPATH, 0, NULL, NULL, NULL },
  { "Verify", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_VERIFY, 0, NULL, NULL, NULL },
  { "XAttrSupport", CFG_TYPE_OPTION, ITEM(FileOptions, opts), INC_KW_XATTR, 0, NULL, NULL, NULL },

  { "Regex", CFG_TYPE_REGEX, ITEM(FileOptions, regex), 0, 0, NULL, NULL, NULL },
  { "RegexDir", CFG_TYPE_REGEX, ITEM(FileOptions, regexdir), 1, 0, NULL, NULL, NULL },
  { "RegexFile", CFG_TYPE_REGEX, ITEM(FileOptions, regexfile), 2, 0, NULL, NULL, NULL },
  /* MARKER */
  // find out if Base is WildBase or Base
  { "Base", CFG_TYPE_ALIST_NAME, ITEM(FileOptions, base), 0, CFG_ITEM_DEPRECATED, NULL, NULL, NULL },
  { "Wild", CFG_TYPE_WILD, ITEM(FileOptions, wild), 0, 0, NULL, NULL, NULL },
  { "WildDir", CFG_TYPE_WILD, ITEM(FileOptions, wilddir), 1, 0, NULL, NULL, NULL },
  { "WildFile", CFG_TYPE_WILD, ITEM(FileOptions, wildfile), 2, 0, NULL, NULL, NULL },
  { "Plugin", CFG_TYPE_ALIST_NAME, ITEM(FileOptions, plugin), 0, 0, NULL, NULL, NULL },
  { "FsType", CFG_TYPE_FSTYPE, ITEM(FileOptions, fstype), 0, 0, NULL, NULL, NULL },
  { "DriveType", CFG_TYPE_DRIVETYPE, ITEM(FileOptions, Drivetype), 0, 0, NULL, NULL, NULL },
  { "Meta", CFG_TYPE_META, ITEM(FileOptions, meta), 0, 0, 0, NULL, NULL },
  {}
};

/* clang-format on */


struct OptionsDefaultValues {
  std::map<int, options_default_value_s> option_default_values
      = {{INC_KW_ACL, {false, "A"}},
         {INC_KW_HARDLINK, {false, "H"}},
         {INC_KW_XATTR, {false, "X"}}};
};

// determine used compression algorithms
// returns if compression is used at all.
bool FindUsedCompressalgos(PoolMem* compressalgos, JobControlRecord* jcr)
{
  int cnt = 0;
  IncludeExcludeItem* inc;
  FileOptions* fopts;
  FilesetResource* fs;
  struct s_fs_opt* fs_opt;

  if (!jcr->dir_impl->res.job || !jcr->dir_impl->res.job->fileset) {
    return false;
  }

  fs = jcr->dir_impl->res.job->fileset;
  for (std::size_t i = 0; i < fs->include_items.size(); i++) {
    inc = fs->include_items[i];

    for (std::size_t j = 0; j < inc->file_options_list.size(); j++) {
      fopts = inc->file_options_list[j];

      for (char* k = fopts->opts; *k; k++) { /* Try to find one request */
        switch (*k) {
          case 'Z': /* Compression */
            for (fs_opt = FS_options; fs_opt->name; fs_opt++) {
              if (fs_opt->keyword != INC_KW_COMPRESSION) { continue; }

              if (bstrncmp(k, fs_opt->option, strlen(fs_opt->option))) {
                if (cnt > 0) {
                  compressalgos->strcat(",");
                } else {
                  compressalgos->strcat(" (");
                }
                compressalgos->strcat(fs_opt->name);
                k += strlen(fs_opt->option) - 1;
                cnt++;
                continue;
              }
            }
            break;
          default:
            break;
        }
      }
    }
  }

  if (cnt > 0) { compressalgos->strcat(")"); }

  return cnt > 0;
}

// Check if the configured options are valid.
static inline void IsInPermittedSet(LEX* lc,
                                    const char* SetType,
                                    const char* permitted_set)
{
  const char *p, *q;
  bool found;

  for (p = lc->str; *p; p++) {
    found = false;
    for (q = permitted_set; *q; q++) {
      if (*p == *q) {
        found = true;
        break;
      }
    }

    if (!found) {
      scan_err3(lc, T_("Illegal %s option %c, got option string: %s:"), SetType,
                *p, lc->str);
    }
  }
}

/**
 * Scan for right hand side of Include options (keyword=option) is
 * converted into one or two characters. Verifyopts=xxxx is Vxxxx:
 * Whatever is found is concatenated to the opts string.
 *
 * This code is also used inside an Options resource.
 */
static void ScanIncludeOptions(LEX* lc, int keyword, char* opts, int optlen)
{
  int i;
  char option[64];
  int lcopts = lc->options;
  struct s_sz_matching size_matching;

  memset(option, 0, sizeof(option));
  lc->options |= LOPT_STRING;     /* force string */
  LexGetToken(lc, BCT_STRING);    /* expect at least one option */
  if (keyword == INC_KW_VERIFY) { /* special case */
    IsInPermittedSet(lc, T_("verify"), PERMITTED_VERIFY_OPTIONS);
    bstrncat(opts, "V", optlen); /* indicate Verify */
    bstrncat(opts, lc->str, optlen);
    bstrncat(opts, ":", optlen); /* Terminate it */
    Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
  } else if (keyword == INC_KW_ACCURATE) { /* special case */
    IsInPermittedSet(lc, T_("accurate"), PERMITTED_ACCURATE_OPTIONS);
    bstrncat(opts, "C", optlen); /* indicate Accurate */
    bstrncat(opts, lc->str, optlen);
    bstrncat(opts, ":", optlen); /* Terminate it */
    Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
  } else if (keyword == INC_KW_BASEJOB) { /* special case */
    IsInPermittedSet(lc, T_("base job"), PERMITTED_BASEJOB_OPTIONS);
    bstrncat(opts, "J", optlen); /* indicate BaseJob */
    bstrncat(opts, lc->str, optlen);
    bstrncat(opts, ":", optlen); /* Terminate it */
    Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
  } else if (keyword == INC_KW_STRIPPATH) { /* special case */
    if (!IsAnInteger(lc->str)) {
      scan_err1(lc, T_("Expected a strip path positive integer, got: %s:"),
                lc->str);
      return;
    }
    bstrncat(opts, "P", optlen); /* indicate strip path */
    bstrncat(opts, lc->str, optlen);
    bstrncat(opts, ":", optlen); /* Terminate it */
    Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
  } else if (keyword == INC_KW_SIZE) { /* special case */
    if (!ParseSizeMatch(lc->str, &size_matching)) {
      scan_err1(lc, T_("Expected a parseable size, got: %s:"), lc->str);
      return;
    }
    bstrncat(opts, "z", optlen); /* indicate size */
    bstrncat(opts, lc->str, optlen);
    bstrncat(opts, ":", optlen); /* Terminate it */
    Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
  } else {
    // Standard keyword options for Include/Exclude
    for (i = 0; FS_options[i].name; i++) {
      if (FS_options[i].keyword == keyword
          && Bstrcasecmp(lc->str, FS_options[i].name)) {
        bstrncpy(option, FS_options[i].option, sizeof(option));
        i = 0;
        break;
      }
    }
    if (i != 0) {
      scan_err1(lc, T_("Expected a FileSet option keyword, got: %s:"), lc->str);
      return;
    } else { /* add option */
      bstrncat(opts, option, optlen);
      Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
    }
  }
  lc->options = lcopts;

  // If option terminated by comma, eat it
  if (lc->ch == ',') { LexGetToken(lc, BCT_ALL); /* yes, eat comma */ }
}

// Store regex info
static void StoreRegex(ConfigurationParser*,
                       BareosResource* res,
                       LEX* lc,
                       ResourceItem* item,
                       int index)
{
  int rc;
  regex_t preg{};
  char prbuf[500];

  auto* regex_loc = GetItemVariablePointer<alist<char*>**>(res, *item);
  if (!*regex_loc) { *regex_loc = new alist<char*>(10, not_owned_by_alist); }
  auto* regex = *regex_loc;

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup regex string
   */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      rc = regcomp(&preg, lc->str, REG_EXTENDED);
      if (rc != 0) {
        regerror(rc, &preg, prbuf, sizeof(prbuf));
        regfree(&preg);
        scan_err1(lc, T_("Regex compile error. ERR=%s\n"), prbuf);
        return;
      }
      regfree(&preg);
      regex->append(strdup(lc->str));
      Dmsg4(900, "set %s (%p) size=%d %s\n", item->name, regex_loc,
            regex->size(), lc->str);
      break;
    default:
      scan_err1(lc, T_("Expected a regex string, got: %s\n"), lc->str);
      return;
  }
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

// Store Wild-card info
static void StoreWild(ConfigurationParser*,
                      BareosResource* res,
                      LEX* lc,
                      ResourceItem* item,
                      int index)
{
  auto* wild_loc = GetItemVariablePointer<alist<const char*>**>(res, *item);
  if (!*wild_loc) { *wild_loc = new alist<const char*>(10, owned_by_alist); }

  auto* wild = *wild_loc;

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  // Pickup Wild-card string
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      wild->append(strdup(lc->str));
      Dmsg4(9, "set %s (%p) size=%d %s\n", item->name, wild_loc, wild->size(),
            lc->str);
      break;
    default:
      scan_err1(lc, T_("Expected a wild-card string, got: %s\n"), lc->str);
      return;
  }
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

// Store fstype info
static void StoreFstype(ConfigurationParser*,
                        BareosResource* res,
                        LEX* lc,
                        ResourceItem* item,
                        int index)
{
  auto* fstype_loc = GetItemVariablePointer<alist<const char*>**>(res, *item);
  if (!*fstype_loc) {
    *fstype_loc = new alist<const char*>(10, owned_by_alist);
  }

  auto* fstype = *fstype_loc;

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup fstype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      fstype->append(strdup(lc->str));
      Dmsg3(900, "set fstype %s %p size=%d %s\n", item->name, fstype_loc,
            fstype->size(), lc->str);
      break;
    default:
      scan_err1(lc, T_("Expected a fstype string, got: %s\n"), lc->str);
      return;
  }
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

// Store Drivetype info
static void StoreDrivetype(ConfigurationParser*,
                           BareosResource* res,
                           LEX* lc,
                           ResourceItem* item,
                           int index)
{
  auto* drivetype_loc
      = GetItemVariablePointer<alist<const char*>**>(res, *item);
  if (!*drivetype_loc) {
    *drivetype_loc = new alist<const char*>(10, owned_by_alist);
  }

  auto* drivetype = *drivetype_loc;

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup Drivetype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      drivetype->append(strdup(lc->str));
      Dmsg3(900, "set Drivetype %s (%p) size=%d %s\n", item->name,
            drivetype_loc, drivetype->size(), lc->str);
      break;
    default:
      scan_err1(lc, T_("Expected a Drivetype string, got: %s\n"), lc->str);
      return;
  }
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

static void StoreMeta(ConfigurationParser*,
                      BareosResource* res,
                      LEX* lc,
                      ResourceItem* item,
                      int index)
{
  auto* meta_loc = GetItemVariablePointer<alist<const char*>**>(res, *item);
  if (!*meta_loc) { *meta_loc = new alist<const char*>(10, owned_by_alist); }

  auto* meta = *meta_loc;

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup Drivetype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      meta->append(strdup(lc->str));
      Dmsg3(900, "set meta %s (%p) size=%d %s\n", item->name, meta_loc,
            meta->size(), lc->str);
      break;
    default:
      scan_err1(lc, T_("Expected a meta string, got: %s\n"), lc->str);
      return;
  }
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

// New style options come here
static void StoreOption(
    ConfigurationParser*,
    BareosResource* res,
    LEX* lc,
    ResourceItem*
        item /*,
               std::map<int, options_default_value_s>& option_default_values */
    ,
    int index)
{
  auto* opts = GetItemVariablePointer<FileOptions::options*>(res, *item);

  // FileOptions::options inc_opts = {};

  int keyword = item->code;

  // if (option_default_values.find(keyword) != option_default_values.end()) {
  //   option_default_values[keyword].configured = true;
  // }


  // Now scan for the value
  ScanIncludeOptions(lc, keyword, *opts, sizeof(*opts));
  Dmsg2(900, "new incexe opts=%s\n", opts);

  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

#if 0
/* MARKER */
// If current_opts not defined, create first entry
static void SetupCurrentOpts(void)
{
  FileOptions* fo = new FileOptions;
  fo->regex.init(1, true);
  fo->regexdir.init(1, true);
  fo->regexfile.init(1, true);
  fo->wild.init(1, true);
  fo->wilddir.init(1, true);
  fo->wildfile.init(1, true);
  fo->wildbase.init(1, true);
  fo->base.init(1, true);
  fo->fstype.init(1, true);
  fo->Drivetype.init(1, true);
  fo->meta.init(1, true);
  res_incexe->current_opts = fo;
  res_incexe->file_options_list.push_back(fo);
}

static void ApplyDefaultValuesForUnsetOptions(
    OptionsDefaultValues default_values)
{
  for (auto const& option : default_values.option_default_values) {
    int keyword_id = option.first;
    bool was_set_in_config = option.second.configured;
    std::string default_value = option.second.default_value;
    if (!was_set_in_config) {
      bstrncat(res_incexe->current_opts->opts, default_value.c_str(),
               MAX_FOPTS);
      Dmsg2(900, "setting default value for keyword-id=%d, %s\n", keyword_id,
            default_value.c_str());
    }
  }
}

static void StoreDefaultOptions()
{
  SetupCurrentOpts();
  ApplyDefaultValuesForUnsetOptions(OptionsDefaultValues{});
}
#endif

static void ParseConfigCb(ConfigurationParser* p,
                          BareosResource* res,
                          LEX* lc,
                          ResourceItem* item,
                          int index);

// Come here when Options seen in Include/Exclude
static void StoreOptionsRes(ConfigurationParser* p,
                            BareosResource* res,
                            LEX* lc,
                            ResourceItem* item,
                            int index)
{
#if 1
  auto& opts_loc = GetItemVariable<std::vector<FileOptions*>&>(res, *item);

  auto* new_opt = new FileOptions;

  auto parse_res = p->ParseResource(new_opt, options_items, lc, ParseConfigCb);
  if (!parse_res) {
    // error
    delete new_opt;
    return;
  }

  opts_loc.push_back(new_opt);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
#else
  int token;
  OptionsDefaultValues default_values;

  token = LexGetToken(lc, BCT_SKIP_EOL);
  if (token != BCT_BOB) {
    scan_err1(lc, T_("Expecting open brace. Got %s"), lc->str);
    return;
  }

  if (pass == 1) { SetupCurrentOpts(); }

  while ((token = LexGetToken(lc, BCT_ALL)) != BCT_EOF) {
    if (token == BCT_EOL) { continue; }
    if (token == BCT_EOB) { break; }
    if (token != BCT_IDENTIFIER) {
      scan_err1(lc, T_("Expecting keyword, got: %s\n"), lc->str);
      return;
    }
    bool found = false;
    for (int i = 0; options_items[i].name; i++) {
      if (Bstrcasecmp(options_items[i].name, lc->str)) {
        token = LexGetToken(lc, BCT_SKIP_EOL);
        if (token != BCT_EQUALS) {
          scan_err1(lc, T_("expected an equals, got: %s"), lc->str);
          return;
        }
        /* Call item handler */
        switch (options_items[i].type) {
          case CFG_TYPE_OPTION:
            StoreOption(lc, &options_items[i], pass,
                        default_values.option_default_values);
            break;
          case CFG_TYPE_REGEX:
            StoreRegex(lc, &options_items[i], pass);
            break;
          case CFG_TYPE_WILD:
            StoreWild(lc, &options_items[i], pass);
            break;
          case CFG_TYPE_FSTYPE:
            StoreFstype(lc, &options_items[i], pass);
            break;
          case CFG_TYPE_DRIVETYPE:
            StoreDrivetype(lc, &options_items[i], pass);
            break;
          case CFG_TYPE_META:
            StoreMeta(lc, &options_items[i], pass);
            break;
          default:
            break;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      scan_err1(lc, T_("Keyword %s not permitted in this resource"), lc->str);
      return;
    }
  }

  if (pass == 1) { ApplyDefaultValuesForUnsetOptions(default_values); }
#endif
}

/**
 * Store Filename info. Note, for minor efficiency reasons, we
 * always increase the name buffer by 10 items because we expect
 * to add more entries.
 */
static void StoreFname(ConfigurationParser*,
                       BareosResource* res,
                       LEX* lc,
                       ResourceItem* item,
                       int index)
{
  int token = LexGetToken(lc, BCT_SKIP_EOL);

  auto* fname_loc = GetItemVariablePointer<alist<const char*>**>(res, *item);

  if (!*fname_loc) { *fname_loc = new alist<const char*>(10, owned_by_alist); }

  auto* fname = *fname_loc;

  /* Pickup Filename string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
      if (strchr(lc->str, '\\')) {
        scan_err1(lc,
                  T_("Backslash found. Use forward slashes or quote the "
                     "string.: %s\n"),
                  lc->str);
        return;
      }
      [[fallthrough]];
    case BCT_QUOTED_STRING: {
      /* MARKER */
      // if (res_fs->have_MD5) {
      //   IGNORE_DEPRECATED_ON;
      //   MD5_Update(&res_fs->md5c, (unsigned char*)lc->str, lc->str_len);
      //   IGNORE_DEPRECATED_OFF;
      // }

      fname->append(lc->str);

      Dmsg1(900, "Add to name_list %s\n", lc->str);
      break;
    }
    default:
      scan_err1(lc, T_("Expected a filename, got: %s"), lc->str);
      return;
  }
  ScanToEol(lc);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
}

/**
 * Store Filename info. Note, for minor efficiency reasons, we
 * always increase the name buffer by 10 items because we expect
 * to add more entries.
 */
static void StorePluginName(ConfigurationParser*,
                            BareosResource* res,
                            LEX* lc,
                            ResourceItem* item,
                            int index)
{
#if 1
  int token = LexGetToken(lc, BCT_SKIP_EOL);

  auto* plugin_name_loc
      = GetItemVariablePointer<alist<const char*>**>(res, *item);

  if (!*plugin_name_loc) {
    *plugin_name_loc = new alist<const char*>(10, owned_by_alist);
  }

  auto* plugin_name = *plugin_name_loc;

  /* Pickup Filename string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
      if (strchr(lc->str, '\\')) {
        scan_err1(lc,
                  T_("Backslash found. Use forward slashes or quote the "
                     "string.: %s\n"),
                  lc->str);
        return;
      }
      [[fallthrough]];
    case BCT_QUOTED_STRING: {
      /* MARKER */
      // if (res_fs->have_MD5) {
      //   IGNORE_DEPRECATED_ON;
      //   MD5_Update(&res_fs->md5c, (unsigned char*)lc->str, lc->str_len);
      //   IGNORE_DEPRECATED_OFF;
      // }

      plugin_name->append(lc->str);

      Dmsg1(900, "Add to name_list %s\n", lc->str);
      break;
    }
    default:
      scan_err1(lc, T_("Expected a filename, got: %s"), lc->str);
      return;
  }
  ScanToEol(lc);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
#else
  int token;

  token = LexGetToken(lc, BCT_SKIP_EOL);
  if (pass == 1) {
    // Pickup Filename string
    switch (token) {
      case BCT_IDENTIFIER:
      case BCT_UNQUOTED_STRING:
        if (strchr(lc->str, '\\')) {
          scan_err1(lc,
                    T_("Backslash found. Use forward slashes or quote the "
                       "string.: %s\n"),
                    lc->str);
          return;
        }
        FALLTHROUGH_INTENDED;
      case BCT_QUOTED_STRING: {
        if (res_fs->have_MD5) {
          IGNORE_DEPRECATED_ON;
          MD5_Update(&res_fs->md5c, (unsigned char*)lc->str, lc->str_len);
          IGNORE_DEPRECATED_OFF;
        }
        if (res_incexe->plugin_list.size() == 0) {
          res_incexe->plugin_list.init(10, true);
        }
        res_incexe->plugin_list.append(strdup(lc->str));
        Dmsg1(900, "Add to plugin_list %s\n", lc->str);
        break;
      }
      default:
        scan_err1(lc, T_("Expected a filename, got: %s"), lc->str);
        return;
    }
  }
  ScanToEol(lc);
#endif
}

static void ParseConfigCb(ConfigurationParser* p,
                          BareosResource* res,
                          LEX* lc,
                          ResourceItem* item,
                          int index)
{
  switch (item->type) {
    case CFG_TYPE_FNAME:
      StoreFname(p, res, lc, item, index);
      break;
    case CFG_TYPE_PLUGINNAME:
      StorePluginName(p, res, lc, item, index);
      break;
    case CFG_TYPE_OPTIONS:
      StoreOptionsRes(p, res, lc, item, index);
      break;
    case CFG_TYPE_OPTION:
      StoreOption(p, res, lc, item, index);
      break;
    case CFG_TYPE_REGEX:
      StoreRegex(p, res, lc, item, index);
      break;
    case CFG_TYPE_WILD:
      StoreWild(p, res, lc, item, index);
      break;
    case CFG_TYPE_FSTYPE:
      StoreFstype(p, res, lc, item, index);
      break;
    case CFG_TYPE_DRIVETYPE:
      StoreDrivetype(p, res, lc, item, index);
      break;
    case CFG_TYPE_META:
      StoreMeta(p, res, lc, item, index);
      break;
    default:
      break;
  }
}

/**
 * Store new style FileSet Include/Exclude info
 *
 *  Note, when this routine is called, we are inside a FileSet
 *  resource.  We treat the Include/Exclude like a sort of
 *  mini-resource within the FileSet resource.
 */
static void StoreNewinc(ConfigurationParser* p,
                        BareosResource* res,
                        LEX* lc,
                        ResourceItem* item,
                        int index)
{
  auto* list
      = GetItemVariablePointer<std::vector<IncludeExcludeItem*>*>(res, *item);

  IncludeExcludeItem* incexe = new IncludeExcludeItem;

  ResourceItem* items = (item->code) ? newexc_items : newinc_items;

  auto result = p->ParseResource(incexe, items, lc, ParseConfigCb);

  if (!result) {
    scan_err1(lc, T_("Could not parse Include/Exclude block: %s\n"),
              result.strerror());
    return;
  }

  if (!res->IsMemberPresent("Options")) {
    /* MARKER */
    // how to do this ?
    // if (pass == 1) { StoreDefaultOptions(); }
  }

  list->push_back(incexe);
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

/**
 * Store FileSet Include/Exclude info
 *  new style includes are handled in StoreNewinc()
 */
void StoreInc(ConfigurationParser* p,
              BareosResource* res,
              LEX* lc,
              ResourceItem* item,
              int index)
{
  StoreNewinc(p, res, lc, item, index);
  /* MARKER */
  // TODO: can we still check for this ?
  // int token;

  // /* Decide if we are doing a new Include or an old include. The
  //  *  new Include is followed immediately by open brace, whereas the
  //  *  old include has options following the Include. */
  // token = LexGetToken(lc, BCT_SKIP_EOL);
  // if (token == BCT_BOB) {
  //   StoreNewinc(p, res, lc, item, index);
  //   return;
  // }
  // scan_err0(lc, T_("Old style Include/Exclude not supported\n"));
}

json_t* json_incexc(const int type)
{
  return json_datatype(type, newinc_items);
}

json_t* json_options(const int type)
{
  return json_datatype(type, options_items);
}

} /* namespace directordaemon */
