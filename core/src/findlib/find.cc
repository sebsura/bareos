/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2026 Bareos GmbH & Co. KG

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
// Kern E. Sibbald, MM
/**
 * @file
 * Main routine for finding files on a file system.
 * The heart of the work to find the files on the
 * system is done in find_one.c. Here we have the
 * higher level control as well as the matching
 * routines for the new syntax Options resource.
 */

#if !defined(HAVE_MSVC)
#  include <unistd.h>
#endif
#include "include/bareos.h"
#include "include/filetypes.h"
#include "include/jcr.h"
#include "find.h"
#include "findlib/find_one.h"
#include "lib/util.h"
#include <string>

static void join(std::string& s, const char* sep, const char* app)
{
  if (s.size()) { s += sep; }
  s += app;
}

static std::string fopts_as_str(char (&flags)[FOPTS_BYTES])
{
  std::string s;
  const char* sep = "|";

  if (BitIsSet(FO_PORTABLE_DATA, flags)) { join(s, sep, "PORTABLE"); }
  if (BitIsSet(FO_MD5, flags)) { join(s, sep, "MD5"); }
  if (BitIsSet(FO_COMPRESS, flags)) { join(s, sep, ""); }
  if (BitIsSet(FO_NO_RECURSION, flags)) { join(s, sep, "NO_RECURSION"); }
  if (BitIsSet(FO_MULTIFS, flags)) { join(s, sep, "MULTIFS"); }
  if (BitIsSet(FO_SPARSE, flags)) { join(s, sep, "SPARSE"); }
  if (BitIsSet(FO_IF_NEWER, flags)) { join(s, sep, "IF_NEWER"); }
  if (BitIsSet(FO_NOREPLACE, flags)) { join(s, sep, "NOREPLACE"); }
  if (BitIsSet(FO_READFIFO, flags)) { join(s, sep, "READFIFO"); }
  if (BitIsSet(FO_SHA1, flags)) { join(s, sep, "SHA1"); }
  if (BitIsSet(FO_PORTABLE, flags)) { join(s, sep, "PORTABLE"); }
  if (BitIsSet(FO_MTIMEONLY, flags)) { join(s, sep, "MTIMEONLY"); }
  if (BitIsSet(FO_KEEPATIME, flags)) { join(s, sep, "KEEPATIME"); }
  if (BitIsSet(FO_EXCLUDE, flags)) { join(s, sep, "EXCLUDE"); }
  if (BitIsSet(FO_ACL, flags)) { join(s, sep, "ACL"); }
  if (BitIsSet(FO_NO_HARDLINK, flags)) { join(s, sep, "NO_HARDLINK"); }
  if (BitIsSet(FO_IGNORECASE, flags)) { join(s, sep, "IGNORECASE"); }
  if (BitIsSet(FO_HFSPLUS, flags)) { join(s, sep, "HFSPLUS"); }
  if (BitIsSet(FO_WIN32DECOMP, flags)) { join(s, sep, "WIN32DECOMP"); }
  if (BitIsSet(FO_SHA256, flags)) { join(s, sep, "SHA256"); }
  if (BitIsSet(FO_SHA512, flags)) { join(s, sep, "SHA512"); }
  if (BitIsSet(FO_ENCRYPT, flags)) { join(s, sep, "ENCRYPT"); }
  if (BitIsSet(FO_NOATIME, flags)) { join(s, sep, "NOATIME"); }
  if (BitIsSet(FO_ENHANCEDWILD, flags)) { join(s, sep, "ENHANCEDWILD"); }
  if (BitIsSet(FO_CHKCHANGES, flags)) { join(s, sep, "CHKCHANGES"); }
  if (BitIsSet(FO_STRIPPATH, flags)) { join(s, sep, "STRIPPATH"); }
  if (BitIsSet(FO_HONOR_NODUMP, flags)) { join(s, sep, "HONOR_NODUMP"); }
  if (BitIsSet(FO_XATTR, flags)) { join(s, sep, "XATTR"); }
  if (BitIsSet(FO_DELTA, flags)) { join(s, sep, "DELTA"); }
  if (BitIsSet(FO_PLUGIN, flags)) { join(s, sep, "PLUGIN"); }
  if (BitIsSet(FO_OFFSETS, flags)) { join(s, sep, "OFFSETS"); }
  if (BitIsSet(FO_NO_AUTOEXCL, flags)) { join(s, sep, "NO_AUTOEXCL"); }
  if (BitIsSet(FO_FORCE_ENCRYPT, flags)) { join(s, sep, "FORCE_ENCRYPT"); }
  if (BitIsSet(FO_XXH128, flags)) { join(s, sep, "XXH128"); }

  return s;
}

#if defined(HAVE_DARWIN_OS)
/* the MacOS linker wants symbols for the destructors of these two types, so we
 * have to force template instantiation. */
template class alist<findFOPTS*>;
template class dlist<dlistString>;
#endif

static const int debuglevel = 450;

int32_t name_max; /* filename max length */
int32_t path_max; /* path name max length */

static int OurCallback(JobControlRecord* jcr,
                       FindFilesPacket* ff,
                       bool top_level);

static const int fnmode = 0;

// Initialize the find files "global" variables
FindFilesPacket* init_find_files()
{
  FindFilesPacket* ff;

  ff = (FindFilesPacket*)malloc(sizeof(FindFilesPacket));
  FindFilesPacket empty_ff;
  *ff = empty_ff;

  ff->sys_fname = GetPoolMemory(PM_FNAME);

  /* Get system path and filename maximum lengths */
  path_max = pathconf(".", _PC_PATH_MAX);
  if (path_max < 2048) { path_max = 2048; }

  name_max = pathconf(".", _PC_NAME_MAX);
  if (name_max < 2048) { name_max = 2048; }
  path_max++; /* add for EOS */
  name_max++; /* add for EOS */

  Dmsg1(debuglevel, "init_find_files ff=%p\n", ff);
  return ff;
}

/**
 * Set FindFiles options. For the moment, we only
 * provide for full/incremental saves, and setting
 * of save_time. For additional options, see above
 */
void SetFindOptions(FindFilesPacket* ff, bool incremental, time_t save_time)
{
  Dmsg0(debuglevel, "Enter SetFindOptions()\n");
  ff->incremental = incremental;
  ff->save_time = save_time;
  Dmsg0(debuglevel, "Leave SetFindOptions()\n");
}

void SetFindChangedFunction(FindFilesPacket* ff,
                            bool CheckFct(JobControlRecord* jcr,
                                          FindFilesPacket* ff))
{
  Dmsg0(debuglevel, "Enter SetFindChangedFunction()\n");
  ff->CheckFct = CheckFct;
}

/**
 * Call this subroutine with a callback subroutine as the first
 * argument and a packet as the second argument, this packet
 * will be passed back to the callback subroutine as the last
 * argument.
 */
int FindFiles(JobControlRecord* jcr,
              FindFilesPacket* ff,
              int FileSave(JobControlRecord* jcr,
                           FindFilesPacket* ff_pkt,
                           bool top_level),
              int PluginSave(JobControlRecord* jcr,
                             FindFilesPacket* ff_pkt,
                             bool top_level))
{
  ff->FileSave = FileSave;

  /* This is the new way */
  findFILESET* fileset = ff->fileset;
  if (fileset) {
    /* TODO: We probably need be move the initialization in the fileset loop,
     * at this place flags options are "concatenated" across Include {} blocks
     * (not only Options{} blocks inside a Include{}) */
    ClearAllBits(FO_MAX, ff->flags);
    for (auto& incexe : fileset->include_list) {
      dlistString* node;
      fileset->incexe = &incexe;

      // Here, we reset some values between two different Include{}
      strcpy(ff->VerifyOpts, "V");
      strcpy(ff->AccurateOpts, "Cmcs");  /* mtime+ctime+size by default */
      strcpy(ff->BaseJobOpts, "Jspug5"); /* size+perm+user+group+chk  */
      ff->plugin = NULL;
      ff->opt_plugin = false;

      /* By setting all options, we in effect OR the global options which is
       * what we want. */
      for (auto& option_block : incexe.opts_list) {
        CopyBits(FO_MAX, option_block.flags, ff->flags);
        ff->Compress_algo = option_block.Compress_algo;
        ff->Compress_level = option_block.Compress_level;
        ff->StripPath = option_block.StripPath;
        ff->size_match = option_block.size_match.get();
        ff->fstypes = option_block.fstype;
        ff->drivetypes = option_block.Drivetype;
        if (option_block.plugin) {
          ff->plugin = option_block.plugin
                           ->c_str(); /* TODO: generate a plugin event ? */
          ff->opt_plugin = true;
        }
        bstrncat(ff->VerifyOpts, option_block.VerifyOpts,
                 sizeof(ff->VerifyOpts)); /* TODO: Concat or replace? */
        if (option_block.AccurateOpts[0]) {
          bstrncpy(ff->AccurateOpts, option_block.AccurateOpts,
                   sizeof(ff->AccurateOpts));
        }
      }

      Dmsg4(50, "Verify=<%s> Accurate=<%s> BaseJob=<%s> flags=<%s>\n",
            ff->VerifyOpts, ff->AccurateOpts, ff->BaseJobOpts,
            fopts_as_str(ff->flags).c_str());

      foreach_dlist (node, &incexe.name_list) {
        char* fname = node->c_str();

        Dmsg1(debuglevel, "F %s\n", fname);
        ff->top_fname = fname;
        if (FindOneFile(jcr, ff, OurCallback, ff->top_fname, (dev_t)-1, true)
            == 0) {
          return 0; /* error return */
        }
        if (jcr->IsJobCanceled()) { return 0; }
      }

      foreach_dlist (node, &incexe.plugin_list) {
        char* fname = node->c_str();

        if (!PluginSave) {
          Jmsg(jcr, M_FATAL, 0, T_("Plugin: \"%s\" not found.\n"), fname);
          return 0;
        }
        Dmsg1(debuglevel, "PluginCommand: %s\n", fname);
        ff->top_fname = fname;
        ff->cmd_plugin = true;
        if (!PluginSave(jcr, ff, true)) { return 0; }
        ff->cmd_plugin = false;
        if (jcr->IsJobCanceled()) { return 0; }
      }
    }
  }
  return 1;
}

/**
 * Test if the currently selected directory (in ff->fname) is
 * explicitly in the Include list or explicitly in the Exclude list.
 */
bool IsInFileset(FindFilesPacket* ff)
{
  dlistString* node;
  findFILESET* fileset = ff->fileset;

  if (fileset) {
    for (auto& incexe : fileset->include_list) {
      foreach_dlist (node, &incexe.name_list) {
        const char* fname = node->c_str();
        Dmsg2(debuglevel, "Inc fname=%s ff->fname=%s\n", fname, ff->fname);
        if (bstrcmp(fname, ff->fname)) { return true; }
      }
    }
    for (auto& incexe : fileset->exclude_list) {
      foreach_dlist (node, &incexe.name_list) {
        const char* fname = node->c_str();
        Dmsg2(debuglevel, "Exc fname=%s ff->fname=%s\n", fname, ff->fname);
        if (bstrcmp(fname, ff->fname)) { return true; }
      }
    }
  }

  return false;
}

bool AcceptFile(FindFilesPacket* ff)
{
  int fnm_flags;
  const char* basename;
  findFILESET* fileset = ff->fileset;
  findIncludeExcludeItem* incexe = fileset->incexe;
  int (*match_func)(const char* pattern, const char* string, int flags);

  Dmsg1(debuglevel, "enter AcceptFile: fname=%s\n", ff->fname);
  if (BitIsSet(FO_ENHANCEDWILD, ff->flags)) {
    match_func = fnmatch;
    if ((basename = last_path_separator(ff->fname)) != NULL)
      basename++;
    else
      basename = ff->fname;
  } else {
    match_func = fnmatch;
    basename = ff->fname;
  }

  for (auto& option_block : incexe->opts_list) {
    CopyBits(FO_MAX, option_block.flags, ff->flags);
    ff->Compress_algo = option_block.Compress_algo;
    ff->Compress_level = option_block.Compress_level;
    ff->fstypes = option_block.fstype;
    ff->drivetypes = option_block.Drivetype;

    fnm_flags = BitIsSet(FO_IGNORECASE, ff->flags) ? FNM_CASEFOLD : 0;
    fnm_flags |= BitIsSet(FO_ENHANCEDWILD, ff->flags) ? FNM_PATHNAME : 0;

    if (S_ISDIR(ff->statp.st_mode)) {
      for (const auto& expr : option_block.wilddir) {
        if (match_func(expr.c_str(), ff->fname, fnmode | fnm_flags) == 0) {
          if (BitIsSet(FO_EXCLUDE, ff->flags)) {
            Dmsg2(debuglevel, "Exclude wilddir: %s file=%s\n", expr.c_str(),
                  ff->fname);
            return false; /* reject dir */
          }
          return true; /* accept dir */
        }
      }
    } else {
      for (const auto& expr : option_block.wildfile) {
        if (match_func(expr.c_str(), ff->fname, fnmode | fnm_flags) == 0) {
          if (BitIsSet(FO_EXCLUDE, ff->flags)) {
            Dmsg2(debuglevel, "Exclude wildfile: %s file=%s\n", expr.c_str(),
                  ff->fname);
            return false; /* reject file */
          }
          return true; /* accept file */
        }
      }

      for (const auto& expr : option_block.wildbase) {
        if (match_func(expr.c_str(), basename, fnmode | fnm_flags) == 0) {
          if (BitIsSet(FO_EXCLUDE, ff->flags)) {
            Dmsg2(debuglevel, "Exclude wildbase: %s file=%s\n", expr.c_str(),
                  basename);
            return false; /* reject file */
          }
          return true; /* accept file */
        }
      }
    }

    for (const auto& expr : option_block.wild) {
      if (match_func(expr.c_str(), ff->fname, fnmode | fnm_flags) == 0) {
        if (BitIsSet(FO_EXCLUDE, ff->flags)) {
          Dmsg2(debuglevel, "Exclude wild: %s file=%s\n", expr.c_str(),
                ff->fname);
          return false; /* reject file */
        }
        return true; /* accept file */
      }
    }

    if (S_ISDIR(ff->statp.st_mode)) {
      for (const auto& expr : option_block.regexdir) {
        if (regexec(expr.as_ptr(), ff->fname, 0, NULL, 0) == 0) {
          if (BitIsSet(FO_EXCLUDE, ff->flags)) {
            return false; /* reject file */
          }
          return true; /* accept file */
        }
      }
    } else {
      for (const auto& expr : option_block.regexfile) {
        if (regexec(expr.as_ptr(), ff->fname, 0, NULL, 0) == 0) {
          if (BitIsSet(FO_EXCLUDE, ff->flags)) {
            return false; /* reject file */
          }
          return true; /* accept file */
        }
      }
    }

    for (const auto& expr : option_block.regex) {
      if (regexec(expr.as_ptr(), ff->fname, 0, NULL, 0) == 0) {
        if (BitIsSet(FO_EXCLUDE, ff->flags)) { return false; /* reject file */ }
        return true; /* accept file */
      }
    }

    // If we have an empty Options clause with exclude, then exclude the file
    if (BitIsSet(FO_EXCLUDE, ff->flags) && option_block.regex.size() == 0
        && option_block.wild.size() == 0 && option_block.regexdir.size() == 0
        && option_block.wilddir.size() == 0
        && option_block.regexfile.size() == 0
        && option_block.wildfile.size() == 0
        && option_block.wildbase.size() == 0) {
      Dmsg1(debuglevel, "Empty options, rejecting: %s\n", ff->fname);
      return false; /* reject file */
    }
  }

  // Now apply the Exclude { } directive
  for (auto& exclude_item : fileset->exclude_list) {
    dlistString* node;

    for (auto& option_block : exclude_item.opts_list) {
      fnm_flags
          = BitIsSet(FO_IGNORECASE, option_block.flags) ? FNM_CASEFOLD : 0;
      for (const auto& expr : option_block.wild) {
        if (fnmatch(expr.c_str(), ff->fname, fnmode | fnm_flags) == 0) {
          Dmsg1(debuglevel, "Reject wild1: %s\n", ff->fname);
          return false; /* reject file */
        }
      }
    }
    fnm_flags = (exclude_item.current_opts != NULL
                 && BitIsSet(FO_IGNORECASE, exclude_item.current_opts->flags))
                    ? FNM_CASEFOLD
                    : 0;
    foreach_dlist (node, &exclude_item.name_list) {
      char* fname = node->c_str();

      if (fnmatch(fname, ff->fname, fnmode | fnm_flags) == 0) {
        Dmsg1(debuglevel, "Reject wild2: %s\n", ff->fname);
        return false; /* reject file */
      }
    }
  }

  return true;
}

/**
 * The code comes here for each file examined.
 * We filter the files, then call the user's callback if the file is included.
 */
static int OurCallback(JobControlRecord* jcr,
                       FindFilesPacket* ff,
                       bool top_level)
{
  if (top_level) { return ff->FileSave(jcr, ff, top_level); /* accept file */ }
  switch (ff->type) {
    case FT_NOACCESS:
    case FT_NOFOLLOW:
    case FT_NOSTAT:
    case FT_NOCHG:
    case FT_ISARCH:
    case FT_NORECURSE:
    case FT_NOFSCHG:
    case FT_INVALIDFS:
    case FT_INVALIDDT:
    case FT_NOOPEN:
      //    return ff->FileSave(jcr, ff, top_level);

    /* These items can be filtered */
    case FT_LNKSAVED:
    case FT_REGE:
    case FT_REG:
    case FT_LNK:
    case FT_DIRBEGIN:
    case FT_DIREND:
    case FT_RAW:
    case FT_FIFO:
    case FT_SPEC:
    case FT_DIRNOCHG:
    case FT_REPARSE:
    case FT_JUNCTION:
      if (AcceptFile(ff)) {
        return ff->FileSave(jcr, ff, top_level);
      } else {
        Dmsg1(debuglevel, "Skip file %s\n", ff->fname);
        return -1; /* ignore this file */
      }

    default:
      Dmsg1(000, "Unknown FT code %d\n", ff->type);
      return 0;
  }
}

// Terminate FindFiles() and release all allocated memory
void TermFindFiles(FindFilesPacket* ff)
{
  if (ff) {
    FreePoolMemory(ff->sys_fname);
    if (ff->fname_save) { FreePoolMemory(ff->fname_save); }
    if (ff->link_save) { FreePoolMemory(ff->link_save); }
    if (ff->ignoredir_fname) { FreePoolMemory(ff->ignoredir_fname); }
    TermFindOne(ff);
    free(ff);
  }
}

// Define a new Exclude block in the FileSet
findIncludeExcludeItem* new_exclude(findFILESET* fileset)
{
  // New exclude
  fileset->incexe = &fileset->exclude_list.emplace_back();

  return fileset->incexe;
}

// Define a new Include block in the FileSet
findIncludeExcludeItem* new_include(findFILESET* fileset)
{
  // New include
  fileset->incexe = &fileset->include_list.emplace_back();

  return fileset->incexe;
}

/**
 * Define a new preInclude block in the FileSet.
 * That is the include is prepended to the other
 * Includes. This is used for plugin exclusions.
 */
findIncludeExcludeItem* new_preinclude(findFILESET* fileset)
{
  // New pre-include
  fileset->incexe = &fileset->include_list.emplace_front();

  return fileset->incexe;
}

// Used by plugins to define a new options block
void NewOptions(FindFilesPacket* ff, findIncludeExcludeItem* incexe)
{
  ff->fileset->state = state_options;
  incexe->current_opts = &incexe->opts_list.emplace_front();
}

findFOPTS* start_options(FindFilesPacket* ff)
{
  int state = ff->fileset->state;
  findIncludeExcludeItem* incexe = ff->fileset->incexe;

  if (state != state_options) {
    ff->fileset->state = state_options;
    incexe->current_opts = &incexe->opts_list.emplace_back();
  }

  return incexe->current_opts;
}
