/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

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

#ifndef BAREOS_LIB_BAUTH_OIDC_COMMON_H_
#define BAREOS_LIB_BAUTH_OIDC_COMMON_H_

#include "lib/bauth.h"

namespace auth::Oidc {

struct Data {
  std::string url;

  static std::optional<Data> parse(std::span<const char> data)
  {
    std::string result{data.begin(), data.end()};
    return Data{result};
  }

  std::string format() { return url; }
};

};  // namespace auth::Oidc


#endif  // BAREOS_LIB_BAUTH_OIDC_COMMON_H_
