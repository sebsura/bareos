/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2008 Free Software Foundation Europe e.V.
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
// Kern E. Sibbald, December MMI
/**
 * @file
 * Old style
 *
 * Routines used to keep and match include and exclude
 * filename/pathname patterns.
 *
 * Note, this file is used for the old style include and
 * excludes, so is deprecated. The new style code is found in
 * src/filed/fileset.c.
 *
 * This code is still used for lists in testls and bextract.
 */

#include "include/bareos.h"
#include "include/jcr.h"
#include "find.h"
#include "include/ch.h"

#include <sys/types.h>
#include "findlib/match.h"
#include "findlib/find_one.h"
#include "lib/edit.h"
#include "lib/crypto.h"

#ifndef FNM_LEADING_DIR
#  define FNM_LEADING_DIR 0
#endif

/* Fold case in fnmatch() on Win32 */
#ifdef HAVE_WIN32
static const int fnmode = FNM_CASEFOLD;
#else
static const int fnmode = 0;
#endif

// Add a filename to list of included files
void AddFnameToIncludeList(file_filter& ff, const char* fname)
{
  auto& inc = ff.included_files.emplace_back();

  const char* rp = fname;

  inc.fname = rp;
  /* Zap trailing slashes.  */
  while (!inc.fname.empty() && IsPathSeparator(inc.fname.back())) {
    inc.fname.pop_back();
  }
  /* Check for wild cards */
  inc.pattern = inc.fname.find_first_of("*[?") != inc.fname.npos;

#if defined(HAVE_WIN32)
  /* Convert any \'s into /'s */
  for (auto& c : inc.fname) {
    if (c == '\\') { c = '/'; }
  }
#endif
  Dmsg4(100, "add_fname_to_include fname=%s\n", inc.fname.c_str());
}

/**
 * We add an exclude name to either the exclude path
 *  list or the exclude filename list.
 */
void AddFnameToExcludeList(file_filter& ff, const char* fname)
{
  Dmsg1(20, "Add name to exclude: %s\n", fname);

  auto& exc = [&]() -> excluded_file& {
    if (first_path_separator(fname) != NULL) {
      return ff.excluded_paths.emplace_back();
    } else {
      return ff.excluded_files.emplace_back();
    }
  }();

  exc.fname = fname;
#if defined(HAVE_WIN32)
  /* Convert any \'s into /'s */
  for (auto& c : exc->fname) {
    if (c == '\\') { c = '/'; }
  }
#endif
}

/**
 * Walk through the included list to see if this
 *  file is included possibly with wild-cards.
 */
bool FileIsIncluded(file_filter& ff, const char* file)
{
  auto& list = ff.included_files;

  for (auto& inc : list) {
    if (inc.pattern) {
      if (fnmatch(inc.fname.c_str(), file, fnmode | FNM_LEADING_DIR) == 0) {
        return true;
      }
      continue;
    }
    /* No wild cards. We accept a match to the
     *  end of any component. */
    Dmsg2(900, "pat=%s file=%s\n", inc.fname.c_str(), file);
    size_t len = strlen(file);
    if (inc.fname.size() == len && bstrcmp(inc.fname.c_str(), file)) {
      return true;
    }

    /* this doesnt make much sense to me:
     *  | file = /a/b/c/
     *  | inc.fname = /a
     *  => file is included
     *  | file = /a/b/c
     *  | inc.fname = /a
     *  => file is _not_ included
     * but maybe there is a reason for this.
     * Im guessing there was supposed to be a "inc.fname" is a directory
     * check here "inc.fname ends in /", but somebody typoed "file ends in /"
     * instead.
     */

    if (inc.fname.size() < len && IsPathSeparator(file[inc.fname.size()])
        && bstrncmp(inc.fname.c_str(), file, inc.fname.size())) {
      return true;
    }
    if (inc.fname.size() == 1 && IsPathSeparator(inc.fname[0])) { return true; }
  }
  return false;
}

/**
 * This is the workhorse of excluded_file().
 * Determine if the file is excluded or not.
 */
static bool FileInExcludedList(std::span<excluded_file> list, const char* file)
{
  for (auto& exc : list) {
    if (fnmatch(exc.fname.c_str(), file, fnmode | FNM_PATHNAME) == 0) {
      Dmsg2(900, "Match exc pat=%s: file=%s:\n", exc.fname.c_str(), file);
      return true;
    }
    Dmsg2(900, "No match exc pat=%s: file=%s:\n", exc.fname.c_str(), file);
  }
  return false;
}

/**
 * Walk through the excluded lists to see if this
 *  file is excluded, or if it matches a component
 *  of an excluded directory.
 */
bool FileIsExcluded(file_filter& ff, const char* file)
{
  const char* p;

#if defined(HAVE_WIN32)
  /*  ***NB*** this removes the drive from the exclude
   *  rule.  Why????? */
  if (file[1] == ':') { file += 2; }
#endif

  if (FileInExcludedList(ff.excluded_paths, file)) { return true; }

  /* Try each component */
  for (p = file; *p; p++) {
    /* Match from the beginning of a component only */
    if ((p == file || (!IsPathSeparator(*p) && IsPathSeparator(p[-1])))
        && FileInExcludedList(ff.excluded_files, p)) {
      return true;
    }
  }
  return false;
}

// Parse a size matching fileset option.
bool ParseSizeMatch(const char* size_match_pattern,
                    struct s_sz_matching* size_matching)
{
  bool retval = false;
  char *private_copy, *bp;

  /* Make a private copy of the input string.
   * As we manipulate the input and size_to_uint64
   * eats its input. */
  private_copy = strdup(size_match_pattern);

  // Empty the matching arguments.
  *size_matching = s_sz_matching{};

  /* See if the size is a range e.g. there is a - in the
   * match pattern. As a size of a file can never be negative
   * this is a workable solution. */
  if ((bp = strchr(private_copy, '-')) != NULL) {
    *bp++ = '\0';
    size_matching->type = size_match_range;
    if (!size_to_uint64(private_copy, &size_matching->begin_size)) {
      goto bail_out;
    }
    if (!size_to_uint64(bp, &size_matching->end_size)) { goto bail_out; }
  } else {
    switch (*private_copy) {
      case '<':
        size_matching->type = size_match_smaller;
        if (!size_to_uint64(private_copy + 1, &size_matching->begin_size)) {
          goto bail_out;
        }
        break;
      case '>':
        size_matching->type = size_match_greater;
        if (!size_to_uint64(private_copy + 1, &size_matching->begin_size)) {
          goto bail_out;
        }
        break;
      default:
        size_matching->type = size_match_approx;
        if (!size_to_uint64(private_copy, &size_matching->begin_size)) {
          goto bail_out;
        }
        break;
    }
  }

  retval = true;

bail_out:
  free(private_copy);
  return retval;
}
