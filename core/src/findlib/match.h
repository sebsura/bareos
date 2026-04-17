/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2026 Bareos GmbH & Co. KG

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
#ifndef BAREOS_FINDLIB_MATCH_H_
#define BAREOS_FINDLIB_MATCH_H_

#include "include/fileopts.h"
#include "findlib/shadow.h"

#include <vector>
#include <string>
#include <cstdint>

struct included_file {
  char options[FOPTS_BYTES]; /**< Backup options */
  uint32_t cipher;           /**< Encryption cipher forced by fileset */
  uint32_t algo; /**< Compression algorithm. 4 letters stored as an integer */
  int level;     /**< Compression level */
  int pattern;   /**< Set if wild card pattern */
  struct s_sz_matching* size_match;  /**< Perform size matching ? */
  b_fileset_shadow_type shadow_type; /**< Perform fileset shadowing check ? */
  char VerifyOpts[20];               /**< Options for verify */
  std::string fname;
};

struct excluded_file {
  std::string fname;
};

struct file_filter {
  std::vector<included_file> included_files;
  std::vector<excluded_file> excluded_files;
  std::vector<excluded_file> excluded_paths;
};

void AddFnameToIncludeList(file_filter& ff, int prefixed, const char* fname);
void AddFnameToExcludeList(file_filter& ff, const char* fname);
bool FileIsExcluded(file_filter& ff, const char* file);
bool FileIsIncluded(file_filter& ff, const char* file);

bool ParseSizeMatch(const char* size_match_pattern,
                    struct s_sz_matching* size_matching);

#endif  // BAREOS_FINDLIB_MATCH_H_
