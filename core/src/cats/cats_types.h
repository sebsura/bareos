/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2025 Bareos GmbH & Co. KG

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

#ifndef BAREOS_CATS_CATS_TYPES_H_
#define BAREOS_CATS_CATS_TYPES_H_

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <bitset>
#include <optional>

typedef char** SQL_ROW;

using DB_RESULT_HANDLER = int(void*, int, char**);

typedef struct sql_field {
  const char* name = nullptr; /* name of column */
  int max_length = 0;         /* max length */
  uint32_t type = 0;          /* type */
  uint32_t flags = 0;         /* flags */
} SQL_FIELD;

enum class query_flag : size_t
{
  DiscardResult,
  Count,
};

struct query_flags {
  std::bitset<static_cast<size_t>(query_flag::Count)> set_flags;

  query_flags() = default;
  constexpr query_flags(std::initializer_list<query_flag> initial_flags)
  {
    for (auto flag : initial_flags) {
      auto pos = static_cast<size_t>(flag);
      set_flags.set(pos);
    }
  }

  template <query_flag Flag> void set()
  {
    constexpr auto pos = static_cast<size_t>(Flag);
    static_assert(pos < static_cast<size_t>(query_flag::Count));
    set_flags.set(pos);
  }

  bool test(query_flag Flag)
  {
    return set_flags.test(static_cast<size_t>(Flag));
  }
};

struct db_command_result {
  static db_command_result Ok() { return {}; }
  static db_command_result Error(std::string msg)
  {
    return db_command_result{std::move(msg)};
  }

  const char* error() const
  {
    if (_error) { return _error->c_str(); }
    return nullptr;
  }

 private:
  db_command_result() = default;
  explicit db_command_result(std::string msg) noexcept : _error{std::move(msg)}
  {
  }

  std::optional<std::string> _error{};
};

struct db_conn;

#endif  // BAREOS_CATS_CATS_TYPES_H_
