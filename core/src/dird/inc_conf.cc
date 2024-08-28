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

#include <charconv>
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

template <typename T> struct fsopt {
  T type;
  const char* name;
  const char* code;
};

template <typename T> struct fsopts {};

template <typename T, size_t N> constexpr auto array_of(const T (&arr)[N])
{
  std::array<T, N> res{};
  for (size_t i = 0; i < N; ++i) { res[i] = arr[i]; }
  return res;
}

template <typename T> constexpr auto fsopts_v = array_of(fsopts<T>::value);

template <> struct fsopts<compression_type> {
  static constexpr fsopt<compression_type> value[] = {
      {compression_type::None, "none", ""},
      {compression_type::Gzip6, "gzip", "Z6"},
      {compression_type::Gzip1, "gzip1", "Z1"},
      {compression_type::Gzip2, "gzip2", "Z2"},
      {compression_type::Gzip3, "gzip3", "Z3"},
      {compression_type::Gzip4, "gzip4", "Z4"},
      {compression_type::Gzip5, "gzip5", "Z5"},
      {compression_type::Gzip6, "gzip6", "Z6"},
      {compression_type::Gzip7, "gzip7", "Z7"},
      {compression_type::Gzip8, "gzip8", "Z8"},
      {compression_type::Gzip9, "gzip9", "Z9"},
      {compression_type::Lzo, "lzo", "Zo"},
      {compression_type::Lzfast, "lzfast", "Zff"},
      {compression_type::Lz4, "lz4", "Zf4"},
      {compression_type::Lz4hc, "lz4hc", "Zfh"},
  };
};

template <> struct fsopts<encryption_type> {
  static constexpr fsopt<encryption_type> value[] = {
      {encryption_type::None, "none", ""},
      {encryption_type::Blowfish, "blowfish", "Eb"},
      {encryption_type::TDes, "3des", "E3"},
      {encryption_type::Aes128, "aes128", "Ea1"},
      {encryption_type::Aes192, "aes192", "Ea2"},
      {encryption_type::Aes256, "aes256", "Ea3"},
      {encryption_type::Camellia128, "camellia128", "Ec1"},
      {encryption_type::Camellia192, "camellia192", "Ec2"},
      {encryption_type::Camellia256, "camellia256", "Ec3"},
      {encryption_type::Aes128hmacsha1, "aes128hmacsha1", "Eh1"},
      {encryption_type::Aes256hmacsha1, "aes256hmacsha1", "Eh2"},
  };
};

template <> struct fsopts<checksum_type> {
  static constexpr fsopt<checksum_type> value[] = {
      {checksum_type::None, "none", ""},
      {checksum_type::Md5, "md5", "M"},
      {checksum_type::Sha1, "sha1", "S"},
      {checksum_type::Sha256, "sha256", "S2"},
      {checksum_type::Sha512, "sha512", "S3"},
      {checksum_type::XxHash128, "xxh128", "S4"},
  };
};

template <> struct fsopts<shadowing_option> {
  static constexpr fsopt<shadowing_option> value[] = {
      {shadowing_option::None, "none", ""},
      {shadowing_option::WarnLocally, "localwarn", "d1"},
      {shadowing_option::RemoveLocally, "localremove", "d2"},
      {shadowing_option::WarnGlobally, "globalwarn", "d3"},
      {shadowing_option::RemoveGlobally, "globalremove", "d4"},
  };
};

template <typename T> const fsopt<T>& option_def(T opt)
{
  auto res = std::find_if(std::begin(fsopts_v<T>), std::end(fsopts_v<T>),
                          [opt](const auto l) { return l.type == opt; });

  ASSERT(res != std::end(fsopts_v<T>));

  return *res;
}

/*
 * Options permitted for each keyword and resulting value.
 * The output goes into opts, which are then transmitted to
 * the FD for application as options to the following list of
 * included files.
 */
#if 0
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
#endif

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
  { "Accurate", CFG_TYPE_FILECMP, ITEM(FileOptions, accurate), 0, 0, "mcs", NULL, NULL },
  { "BaseJob", CFG_TYPE_FILECMP, ITEM(FileOptions, basejob), 0, CFG_ITEM_DEPRECATED, NULL, NULL, NULL },
  { "Verify", CFG_TYPE_FILECMP, ITEM(FileOptions, verify), 0, 0, NULL, NULL, NULL },
  { "Size", CFG_TYPE_SZMATCH, ITEM(FileOptions, size), 0, 0, NULL, NULL, NULL },

  { "Compression", CFG_TYPE_COMPRESSION, ITEM(FileOptions, compression), 0, 0, NULL, NULL, NULL },
  /* MARKER */ // missing encryption before ?
  { "Encryption", CFG_TYPE_ENCRYPTION, ITEM(FileOptions, encryption), 0, 0, NULL, NULL, NULL },
  { "Shadowing", CFG_TYPE_SHADOWING, ITEM(FileOptions, shadowing), 0, 0, NULL, NULL, NULL },
  /* MARKER */ // does this make sense here ? Its not documented
  { "Replace", CFG_TYPE_REPLACE, ITEM(FileOptions, replace), 0, 0, NULL, NULL, NULL },
  { "Signature", CFG_TYPE_CHKSUM, ITEM(FileOptions, checksum), 0, 0, NULL, NULL, NULL },
  { "CheckFileChanges", CFG_TYPE_BOOL, ITEM(FileOptions, chkchanges), 0, 0, NULL, NULL, NULL },

  { "AclSupport", CFG_TYPE_BOOL, ITEM(FileOptions, acl), 0, 0, NULL, NULL, NULL },
  { "AutoExclude", CFG_TYPE_BOOL, ITEM(FileOptions, auto_exclude), 0, 0, NULL, NULL, NULL },
  { "EnhancedWild", CFG_TYPE_BOOL, ITEM(FileOptions, enhancedwild), 0, 0, NULL, NULL, NULL },
  { "Exclude", CFG_TYPE_BOOL, ITEM(FileOptions, exclude), 0, 0, NULL, NULL, NULL },
  { "ForceEncryption", CFG_TYPE_BOOL, ITEM(FileOptions, force_encryption), 0, 0, NULL, NULL, NULL },
  { "HardLinks", CFG_TYPE_BOOL, ITEM(FileOptions, hardlink), 0, 0, NULL, NULL, NULL },
  { "HfsPlusSupport", CFG_TYPE_BOOL, ITEM(FileOptions, hfsplus), INC_KW_HFSPLUS, 0, NULL, NULL, NULL },
  { "HonornoDumpFlag", CFG_TYPE_BOOL, ITEM(FileOptions, honor_nodump), INC_KW_HONOR_NODUMP, 0, NULL, NULL, NULL },
  { "IgnoreCase", CFG_TYPE_BOOL, ITEM(FileOptions, ignorecase), INC_KW_IGNORECASE, 0, NULL, NULL, NULL },
  { "KeepAtime", CFG_TYPE_BOOL, ITEM(FileOptions, keepatime), INC_KW_KEEPATIME, 0, NULL, NULL, NULL },
  { "MtimeOnly", CFG_TYPE_BOOL, ITEM(FileOptions, mtimeonly), INC_KW_MTIMEONLY, 0, NULL, NULL, NULL },
  { "NoAtime", CFG_TYPE_BOOL, ITEM(FileOptions, noatime), INC_KW_NOATIME, 0, NULL, NULL, NULL },
  { "OneFs", CFG_TYPE_BOOL, ITEM(FileOptions, onefs), 0, 0, NULL, NULL, NULL },
  { "Portable", CFG_TYPE_BOOL, ITEM(FileOptions, portable), 0, 0, NULL, NULL, NULL },
  { "ReadFifo", CFG_TYPE_BOOL, ITEM(FileOptions, readfifo), 0, 0, NULL, NULL, NULL },
  { "Recurse", CFG_TYPE_BOOL, ITEM(FileOptions, recurse), 0, 0, NULL, NULL, NULL },
  { "Sparse", CFG_TYPE_BOOL, ITEM(FileOptions, sparse), 0, 0, NULL, NULL, NULL },
  { "XAttrSupport", CFG_TYPE_BOOL, ITEM(FileOptions, xattr), 0, 0, NULL, NULL, NULL },

  { "StripPath", CFG_TYPE_PINT32, ITEM(FileOptions, strip_path), 0, 0, NULL, NULL, NULL },

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

  if (!jcr->dir_impl->res.job || !jcr->dir_impl->res.job->fileset) {
    return false;
  }

  fs = jcr->dir_impl->res.job->fileset;
  for (std::size_t i = 0; i < fs->include_items.size(); i++) {
    inc = fs->include_items[i];

    for (std::size_t j = 0; j < inc->file_options_list.size(); j++) {
      fopts = inc->file_options_list[j];

      auto compression = fopts->compression;
      if (compression != compression_type::None) {
        if (cnt > 0) {
          compressalgos->strcat(",");
        } else {
          compressalgos->strcat(" (");
        }
        compressalgos->strcat(option_def(compression).name);
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

// /**
//  * Scan for right hand side of Include options (keyword=option) is
//  * converted into one or two characters. Verifyopts=xxxx is Vxxxx:
//  * Whatever is found is concatenated to the opts string.
//  *
//  * This code is also used inside an Options resource.
//  */
// static void ScanIncludeOptions(LEX* lc, int keyword, char* opts, int optlen)
// {
//   int i;
//   char option[64];
//   int lcopts = lc->options;
//   struct s_sz_matching size_matching;

//   memset(option, 0, sizeof(option));
//   lc->options |= LOPT_STRING;     /* force string */
//   LexGetToken(lc, BCT_STRING);    /* expect at least one option */
//   if (keyword == INC_KW_VERIFY) { /* special case */
//     IsInPermittedSet(lc, T_("verify"), PERMITTED_VERIFY_OPTIONS);
//     bstrncat(opts, "V", optlen); /* indicate Verify */
//     bstrncat(opts, lc->str, optlen);
//     bstrncat(opts, ":", optlen); /* Terminate it */
//     Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//   } else if (keyword == INC_KW_ACCURATE) { /* special case */
//     IsInPermittedSet(lc, T_("accurate"), PERMITTED_ACCURATE_OPTIONS);
//     bstrncat(opts, "C", optlen); /* indicate Accurate */
//     bstrncat(opts, lc->str, optlen);
//     bstrncat(opts, ":", optlen); /* Terminate it */
//     Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//   } else if (keyword == INC_KW_BASEJOB) { /* special case */
//     IsInPermittedSet(lc, T_("base job"), PERMITTED_BASEJOB_OPTIONS);
//     bstrncat(opts, "J", optlen); /* indicate BaseJob */
//     bstrncat(opts, lc->str, optlen);
//     bstrncat(opts, ":", optlen); /* Terminate it */
//     Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//   } else if (keyword == INC_KW_STRIPPATH) { /* special case */
//     if (!IsAnInteger(lc->str)) {
//       scan_err1(lc, T_("Expected a strip path positive integer, got: %s:"),
//                 lc->str);
//       return;
//     }
//     bstrncat(opts, "P", optlen); /* indicate strip path */
//     bstrncat(opts, lc->str, optlen);
//     bstrncat(opts, ":", optlen); /* Terminate it */
//     Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//   } else if (keyword == INC_KW_SIZE) { /* special case */
//     if (!ParseSizeMatch(lc->str, &size_matching)) {
//       scan_err1(lc, T_("Expected a parseable size, got: %s:"), lc->str);
//       return;
//     }
//     bstrncat(opts, "z", optlen); /* indicate size */
//     bstrncat(opts, lc->str, optlen);
//     bstrncat(opts, ":", optlen); /* Terminate it */
//     Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//   } else {
//     // Standard keyword options for Include/Exclude
//     for (i = 0; FS_options[i].name; i++) {
//       if (FS_options[i].keyword == keyword
//           && Bstrcasecmp(lc->str, FS_options[i].name)) {
//         bstrncpy(option, FS_options[i].option, sizeof(option));
//         i = 0;
//         break;
//       }
//     }
//     if (i != 0) {
//       scan_err1(lc, T_("Expected a FileSet option keyword, got: %s:"),
//       lc->str); return;
//     } else { /* add option */
//       bstrncat(opts, option, optlen);
//       Dmsg3(900, "Catopts=%s option=%s optlen=%d\n", opts, option, optlen);
//     }
//   }
//   lc->options = lcopts;

//   // If option terminated by comma, eat it
/* MARKER */  // we still need to do this comma parsing
//   if (lc->ch == ',') { LexGetToken(lc, BCT_ALL); /* yes, eat comma */ }
// }

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

  auto* regex = GetItemVariablePointer<alist<char*>*>(res, *item);

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
      Dmsg4(900, "set %s (%p) size=%d %s\n", item->name, regex, regex->size(),
            lc->str);
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
  auto* wild = GetItemVariablePointer<alist<const char*>*>(res, *item);

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  // Pickup Wild-card string
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      wild->append(strdup(lc->str));
      Dmsg4(9, "set %s (%p) size=%d %s\n", item->name, wild, wild->size(),
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
  auto* fstype = GetItemVariablePointer<alist<const char*>*>(res, *item);

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup fstype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      fstype->append(strdup(lc->str));
      Dmsg3(900, "set fstype %s %p size=%d %s\n", item->name, fstype,
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
  auto* drivetype = GetItemVariablePointer<alist<const char*>*>(res, *item);

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup Drivetype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      drivetype->append(strdup(lc->str));
      Dmsg3(900, "set Drivetype %s (%p) size=%d %s\n", item->name, drivetype,
            drivetype->size(), lc->str);
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
  auto* meta = GetItemVariablePointer<alist<const char*>*>(res, *item);

  int token = LexGetToken(lc, BCT_SKIP_EOL);
  /* Pickup Drivetype string */
  switch (token) {
    case BCT_IDENTIFIER:
    case BCT_UNQUOTED_STRING:
    case BCT_QUOTED_STRING:
      meta->append(strdup(lc->str));
      Dmsg3(900, "set meta %s (%p) size=%d %s\n", item->name, meta,
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

FileOptions::FileOptions()
{
  regex.init(1, true);
  regexdir.init(1, true);
  regexfile.init(1, true);
  wild.init(1, true);
  wilddir.init(1, true);
  wildfile.init(1, true);
  wildbase.init(1, true);
  base.init(1, true);
  fstype.init(1, true);
  Drivetype.init(1, true);
  meta.init(1, true);
}

void FormatFileCmp(std::string& out, const file_compare_options& opt)
{
  if (opt.inodes) { out += 'i'; }
  if (opt.permissions) { out += 'p'; }
  if (opt.num_links) { out += 'n'; }
  if (opt.user_id) { out += 'u'; }
  if (opt.group_id) { out += 'g'; }
  if (opt.size) { out += 's'; }
  if (opt.atime) { out += 'a'; }
  if (opt.mtime) { out += 'm'; }
  if (opt.ctime) { out += 'c'; }
  if (opt.size_decrease) { out += 'd'; }
  if (opt.md5) { out += '5'; }
  if (opt.sha1) { out += '1'; }
  if (opt.always) { out += 'A'; }
}

template <typename Int> static void AppendInt(std::string& s, Int val)
{
  char buffer[100];

  auto res = std::to_chars(std::begin(buffer), std::end(buffer), val);

  ASSERT(res.ec == std::errc());

  std::string_view number_as_str(buffer, res.ptr - &buffer[0]);

  s += number_as_str;
}

std::string FileOptions::format_options()
{
  std::string formatted;
  if (size && size->type != size_match_none) {
    formatted += 'z';

    switch (size->type) {
      case size_match_none: {
      } break;
      case size_match_approx: {
        AppendInt(formatted, size->begin_size);
      } break;
      case size_match_smaller: {
        formatted += "<";
        AppendInt(formatted, size->begin_size);
      } break;
      case size_match_greater: {
        formatted += ">";
        AppendInt(formatted, size->begin_size);
      } break;
      case size_match_range: {
        AppendInt(formatted, size->begin_size);
        formatted += "-";
        AppendInt(formatted, size->end_size);
      } break;
    }

    formatted += ':';
  }

  if (strip_path) {
    formatted += 'P';
    AppendInt(formatted, strip_path);
    formatted += ":";
  }

  if (replace != REPLACE_IFOLDER) {
    // replace already has the right format
    formatted += replace;
  }
  formatted += option_def(compression).code;
  formatted += option_def(encryption).code;
  formatted += option_def(checksum).code;
  formatted += option_def(shadowing).code;

  formatted += 'C';
  FormatFileCmp(formatted, accurate);
  formatted += ':';
  formatted += 'J';
  FormatFileCmp(formatted, basejob);
  formatted += ':';
  formatted += 'V';
  FormatFileCmp(formatted, verify);
  formatted += ':';

  if (!onefs) { formatted += "f"; }
  if (!recurse) { formatted += "h"; }
  if (sparse) { formatted += "s"; }
  if (!hardlink) { formatted += "H"; }
  if (readfifo) { formatted += "r"; }
  if (portable) { formatted += "p"; }
  if (mtimeonly) { formatted += "m"; }
  if (keepatime) { formatted += "k"; }
  if (exclude) { formatted += "e"; }
  if (acl) { formatted += "a"; }
  if (ignorecase) { formatted += "i"; }
  if (hfsplus) { formatted += "R"; }
  if (noatime) { formatted += "K"; }
  if (enhancedwild) { formatted += "K"; }
  if (chkchanges) { formatted += "c"; }
  if (honor_nodump) { formatted += "N"; }
  if (xattr) { formatted += "X"; }
  if (!auto_exclude) { formatted += "x"; }
  if (force_encryption) { formatted += "Ef"; }

  return formatted;
}

#if 0
/* MARKER */
// If current_opts not defined, create first entry
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

  auto* fname = GetItemVariablePointer<alist<const char*>*>(res, *item);

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

      fname->append(strdup(lc->str));

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

  auto* plugin_name = GetItemVariablePointer<alist<const char*>*>(res, *item);

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

      plugin_name->append(strdup(lc->str));

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

static void StoreCompression(ConfigurationParser*,
                             BareosResource* res,
                             LEX* lc,
                             ResourceItem* item,
                             int index)
{
  int lcopts = lc->options;
  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  auto* compression = GetItemVariablePointer<compression_type*>(res, *item);

  bool found = false;
  for (auto& opt : fsopts_v<compression_type>) {
    if (Bstrcasecmp(lc->str, opt.name)) {
      *compression = opt.type;
      found = true;
      break;
    }
  }

  if (!found) {
    scan_err1(lc, T_("Expected a compression type, got: %s:"), lc->str);
    return;
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

static void StoreEncryption(ConfigurationParser*,
                            BareosResource* res,
                            LEX* lc,
                            ResourceItem* item,
                            int index)
{
  int lcopts = lc->options;
  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  auto* encryption = GetItemVariablePointer<encryption_type*>(res, *item);

  bool found = false;
  for (auto& opt : fsopts_v<encryption_type>) {
    if (Bstrcasecmp(lc->str, opt.name)) {
      *encryption = opt.type;
      found = true;
      break;
    }
  }

  if (!found) {
    scan_err1(lc, T_("Expected an encryption type, got: %s:"), lc->str);
    return;
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

static void StoreShadowing(ConfigurationParser*,
                           BareosResource* res,
                           LEX* lc,
                           ResourceItem* item,
                           int index)
{
  int lcopts = lc->options;
  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  auto* shadowing = GetItemVariablePointer<shadowing_option*>(res, *item);


  bool found = false;
  for (auto& opt : fsopts_v<shadowing_option>) {
    if (Bstrcasecmp(lc->str, opt.name)) {
      *shadowing = opt.type;
      found = true;
      break;
    }
  }

  if (!found) {
    scan_err1(lc, T_("Expected a shadowing option, got: %s:"), lc->str);
    return;
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

static void StoreChecksum(ConfigurationParser*,
                          BareosResource* res,
                          LEX* lc,
                          ResourceItem* item,
                          int index)
{
  int lcopts = lc->options;
  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  auto* chksum = GetItemVariablePointer<checksum_type*>(res, *item);

  bool found = false;
  for (auto& opt : fsopts_v<checksum_type>) {
    if (Bstrcasecmp(lc->str, opt.name)) {
      *chksum = opt.type;
      found = true;
      break;
    }
  }

  if (!found) {
    scan_err1(lc, T_("Expected a checksum type, got: %s:"), lc->str);
    return;
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}

static void StoreFileCompare(ConfigurationParser*,
                             BareosResource* res,
                             LEX* lc,
                             ResourceItem* item,
                             int index)
{
  int lcopts = lc->options;
  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  auto* opts = GetItemVariablePointer<file_compare_options*>(res, *item);

  /* MARKER */
  // should we always drop the old value, or just when its the default one ?
  *opts = {};

  for (auto* current = lc->str; *current; current += 1) {
    switch (*current) {
      case 'i': {
        opts->inodes = true;
      } break;
      case 'p': {
        opts->permissions = true;
      } break;
      case 'n': {
        opts->num_links = true;
      } break;
      case 'u': {
        opts->user_id = true;
      } break;
      case 'g': {
        opts->group_id = true;
      } break;
      case 's': {
        opts->size = true;
      } break;
      case 'a': {
        opts->atime = true;
      } break;
      case 'm': {
        opts->mtime = true;
      } break;
      case 'c': {
        opts->ctime = true;
      } break;
      case 'd': {
        opts->size_decrease = true;
      } break;
      case '5': {
        opts->md5 = true;
      } break;
      case '1': {
        opts->sha1 = true;
      } break;
        /* MARKER */  // todo: this is only allowed for accurate resource
        // add a verifier
      case 'A': {
        opts->always = true;
      } break;
      default: {
        scan_err1(lc, T_("expected a file compare option, got: '%c':"),
                  *current);
      } break;
    }
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
}
static void StoreSizeMatch(ConfigurationParser*,
                           BareosResource* res,
                           LEX* lc,
                           ResourceItem* item,
                           int index)
{
  int lcopts = lc->options;
  auto* size = GetItemVariablePointer<s_sz_matching*>(res, *item);

  lc->options |= LOPT_STRING;  /* force string */
  LexGetToken(lc, BCT_STRING); /* expect at least one option */

  if (!ParseSizeMatch(lc->str, size)) {
    scan_err1(lc, T_("Expected a parseable size, got: %s:"), lc->str);
    return;
  }

  lc->options = lcopts;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
  ScanToEol(lc);
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


    case CFG_TYPE_COMPRESSION: {
      StoreCompression(p, res, lc, item, index);
    } break;
    case CFG_TYPE_ENCRYPTION: {
      StoreEncryption(p, res, lc, item, index);
    } break;
    case CFG_TYPE_SHADOWING: {
      StoreShadowing(p, res, lc, item, index);
    } break;
    case CFG_TYPE_CHKSUM: {
      StoreChecksum(p, res, lc, item, index);
    } break;
    case CFG_TYPE_FILECMP: {
      StoreFileCompare(p, res, lc, item, index);
    } break;
    case CFG_TYPE_SZMATCH: {
      StoreSizeMatch(p, res, lc, item, index);
    } break;

    case CFG_TYPE_OPTIONS:
      StoreOptionsRes(p, res, lc, item, index);
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
