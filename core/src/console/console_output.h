/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_CONSOLE_CONSOLE_OUTPUT_H_
#define BAREOS_CONSOLE_CONSOLE_OUTPUT_H_

#include <cstdio>

void ConsoleOutputFormat(const char* fmt, ...)
    __attribute__((format(printf, 1, 2)));
void ConsoleOutput(const char* buf);
void EnableTeeOut();
void DisableTeeOut();
void SetTeeFile(FILE* f);
void CloseTeeFile();

#endif  // BAREOS_CONSOLE_CONSOLE_OUTPUT_H_
