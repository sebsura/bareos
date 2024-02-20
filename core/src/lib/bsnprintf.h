/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2021-2024 Bareos GmbH & Co. KG

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
#ifndef BAREOS_LIB_BSNPRINTF_H_
#define BAREOS_LIB_BSNPRINTF_H_
#include <stdarg.h>
#include <stdint.h>

int Bsnprintf(char* str, int32_t size, const char* format, ...)
    __attribute__((format(printf, 3, 4)));
int Bvsnprintf(char* str, int32_t size, const char* format, va_list ap)
    __attribute__((format(printf, 3, 0)));
#endif  // BAREOS_LIB_BSNPRINTF_H_
