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
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_STORED_BACKENDS_DEDUP_CONFIG_H_
#define BAREOS_STORED_BACKENDS_DEDUP_CONFIG_H_

#include <string>
#include <cstdint>
#include <vector>
#include <cstdlib>

namespace dedup {

struct config {
  struct block_file {
    std::string relpath;
    std::uint64_t Start;
    std::uint64_t End;
    std::uint32_t Idx;
  };

  struct part_file {
    std::string relpath;
    std::uint64_t Start;
    std::uint64_t End;
    std::uint32_t Idx;
  };

  struct data_file {
    std::string relpath;
    std::uint64_t Size;
    std::uint64_t BlockSize;
    std::uint32_t Idx;
    bool ReadOnly;
  };

  std::vector<block_file> bfiles;
  std::vector<part_file> pfiles;
  std::vector<data_file> dfiles;

  static std::vector<char> serialize(const config& conf);
  static config deserialize(const char* data, std::size_t size);
  static config make_default(std::uint64_t BlockSize);
};

};  // namespace dedup

#endif  // BAREOS_STORED_BACKENDS_DEDUP_CONFIG_H_
