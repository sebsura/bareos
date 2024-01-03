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

#ifndef BAREOS_STORED_BACKENDS_UTIL_H_
#define BAREOS_STORED_BACKENDS_UTIL_H_

#include <string_view>
#include <string>
#include <unordered_map>
#include <variant>

namespace util::options {
using options = std::unordered_map<std::string_view, std::string_view>;
using error = std::string;

std::variant<options, error> parse_options(std::string_view v);
};  // namespace util::options

#endif  // BAREOS_STORED_BACKENDS_UTIL_H_
