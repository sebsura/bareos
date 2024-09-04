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

#ifndef BAREOS_LIB_PARSE_ERR_H_
#define BAREOS_LIB_PARSE_ERR_H_

#include <cstdarg>
#include <exception>
#include <string>
#include <vector>

struct parse_error : std::exception {
  std::string error;

  parse_error(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
  {
    int size = 0;

    {
      va_list args;
      va_start(args, fmt);

      size = vsnprintf(nullptr, 0, fmt, args);

      va_end(args);
    }

    if (size < 0) { error = "encountered error while creating error object"; }

    error.resize(size);

    {
      va_list args;
      va_start(args, fmt);
      vsnprintf(error.data(), size, fmt, args);
      va_end(args);
    }
  }

  const char* what() const noexcept override { return error.c_str(); }

  parse_error&& add_context(const char* fmt, ...)
      __attribute__((format(printf, 2, 3)))
  {
    int size = 0;

    {
      va_list args;
      va_start(args, fmt);

      size = vsnprintf(nullptr, 0, fmt, args);

      va_end(args);
    }

    if (size < 0) { error = "encountered error while creating error object"; }

    error += "\n";

    auto old_size = error.size();
    error.resize(old_size + size);

    {
      va_list args;
      va_start(args, fmt);
      vsnprintf(error.data() + old_size, size, fmt, args);
      va_end(args);
    }

    return std::move(*this);
  }
};

#endif  // BAREOS_LIB_PARSE_ERR_H_
