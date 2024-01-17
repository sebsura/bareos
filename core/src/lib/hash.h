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
#ifndef BAREOS_LIB_HASH_H_
#define BAREOS_LIB_HASH_H_

#include <cstdlib>

// see N3876 / boost::hash_combine
inline std::size_t hash_combine(std::size_t seed, std::size_t hash)
{
  // 0x9e3779b9 is approximately 2^32 / phi (where phi is the golden ratio)
  std::size_t changed = hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed ^ changed;
}


#endif  // BAREOS_LIB_HASH_H_
