/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2003-2008 Free Software Foundation Europe e.V.
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

// Kern Sibbald MMIII

/* @file
 * Windows APIs that are different for each system.
 * We use pointers to the entry points so that a
 * single binary will run on all Windows systems.
 */

#include "include/bareos.h"
#include "winapi.h"

void InitWinAPIWrapper()
{
  OSVERSIONINFO osversioninfo = {sizeof(OSVERSIONINFO), 0, 0, 0, 0, 0};

  // Get the current OS version
  if (!GetVersionEx(&osversioninfo)) {
    ASSERT(0);
  } else {
    // Ensure NT kernel (i.e. Win2k+)
    ASSERT(osversioninfo.dwPlatformId == VER_PLATFORM_WIN32_NT);
    // Ensure Vista+
    ASSERT(osversioninfo.dwMajorVersion >= 6);
  }

  dyn::LoadDynamicFunctions();
}

namespace dyn {
dynamic_function::dynamic_function(function_registry& registry, const char* lib)
{
  registry[lib].emplace_back(this);
}

void LoadDynamicFunctions()
{
  for (auto& [lib, funs] : dynamic_functions) {
    auto library = LoadLibraryA(lib.c_str());

    if (!library) continue;

    for (auto* f : funs) { f->load(library); }
  }
}
};  // namespace dyn
