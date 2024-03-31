/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_CORE_SRC_INCLUDE_DLL_EXPORT_IMPORT_H_
#define BAREOS_CORE_SRC_INCLUDE_DLL_EXPORT_IMPORT_H_

#if defined(_MSVC_LANG)
#  define BAREOS_EXPORT __declspec(dllexport) extern
#  define BAREOS_IMPORT __declspec(dllimport) extern
#  if bareossd_EXPORTS
#    define BAREOS_IMPEXP __declspec(dllexport) extern
#  else
#    define BAREOS_IMPEXP __declspec(dllimport)
#  endif
#else
#  define BAREOS_EXPORT extern
#  define BAREOS_IMPORT extern
#  define BAREOS_IMPEXP extern
#endif

#endif  // BAREOS_CORE_SRC_INCLUDE_DLL_EXPORT_IMPORT_H_
