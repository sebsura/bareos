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
  // we currently want at least vista
  OSVERSIONINFOEXW required = {};
  required.dwOSVersionInfoSize = sizeof(required);
  required.dwPlatformId = VER_PLATFORM_WIN32_NT;
  required.dwMajorVersion = 6;

  ASSERT(VerifyVersionInfoW(
      &required, VER_MAJORVERSION | VER_PLATFORMID,
      VerSetConditionMask(
          VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL),
          VER_PLATFORMID, VER_EQUAL)));
}
