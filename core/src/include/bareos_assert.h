/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_INCLUDE_BAREOS_ASSERT_H_
#define BAREOS_INCLUDE_BAREOS_ASSERT_H_

#include "lib/message.h"
#include "include/translate.h"

/**
 * In DEBUG mode an assert that is triggered generates a segmentation
 * fault so we can capture the debug info using btraceback.
 */
#define ASSERT(x)                                                           \
  do {                                                                      \
    if (!(x)) {                                                             \
      e_msg(__FILE__, __LINE__, M_ERROR, 0, T_("Failed ASSERT: %s\n"), #x); \
      p_msg(__FILE__, __LINE__, 000, T_("Failed ASSERT: %s\n"), #x);        \
      abort();                                                              \
    }                                                                       \
  } while (0)

#endif  // BAREOS_INCLUDE_BAREOS_ASSERT_H_
