/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2023 Bareos GmbH & Co. KG

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

#ifndef BAREOS_DIRD_AUTHENTICATE_CONSOLE_H_
#define BAREOS_DIRD_AUTHENTICATE_CONSOLE_H_

#include <atomic>

namespace directordaemon {

class UaContext;


class authentication {
 public:
  ~authentication();

  authentication(const authentication&) = delete;
  authentication& operator=(const authentication&) = delete;

  authentication(authentication&& other) { *this = std::move(other); }

  authentication& operator=(authentication&& other)
  {
    std::swap(incremented_counter, other.incremented_counter);
    std::swap(num_cons, other.num_cons);
    std::swap(max_cons, other.max_cons);
    return *this;
  }

  std::size_t connection_count() const { return num_cons; }

  std::size_t max_connections() const { return max_cons; }

  bool is_ok() const { return incremented_counter; }

  friend authentication AuthenticateConsole(UaContext* ua);

 private:
  authentication() = default;
  authentication(std::size_t max_console_connections);

  bool incremented_counter{false};
  std::size_t num_cons{0};
  std::size_t max_cons{0};
};

[[nodiscard]] authentication AuthenticateConsole(UaContext* ua);

} /* namespace directordaemon */
#endif  // BAREOS_DIRD_AUTHENTICATE_CONSOLE_H_
