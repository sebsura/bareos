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

#ifndef BAREOS_LIB_MESSAGE_BUFFER_H_
#define BAREOS_LIB_MESSAGE_BUFFER_H_

#include <fmt/format.h>
#include <gsl/narrow>
#include <cstring>

#include "bsock.h"

struct MessageStream {
  // this ends a message, and begins the next one
 public:
  MessageStream() { insert_placeholder(); }

  inline constexpr size_t used_size() const noexcept
  {
    return data.size() - sizeof(int32_t);
  }

  std::span<char> allocate(std::size_t size) noexcept
  {
    std::size_t current_size = data.size();
    data.resize(current_size + size);
    return {data.data() + current_size, size};
  }

  void end_message() noexcept
  {
    message_ends.push_back(gsl::narrow_cast<int32_t>(data.size()));
    insert_placeholder();
  }

  template <typename... Args>
  void print(fmt::format_string<Args...> fmt, Args&&... args) noexcept
  {
    fmt::format_to(std::back_inserter(data), fmt, std::forward<Args>(args)...);
  }

  // WARNING: if you did not terminate your previous message,
  //          you will extend & terminate it with this call
  template <typename... Args>
  void printm(fmt::format_string<Args...> fmt, Args&&... args) noexcept
  {
    print(fmt, std::forward<Args>(args)...);
    end_message();
  }

  bool write_into(BareosSocket* s) noexcept
  {
    if (!prepare_message_for_sending()) { return false; }

    std::size_t togo = used_size();
    const char* head = data.data();

    while (togo > 0) {
      int32_t bytes_sent = s->write_nbytes(head, togo);

      if (bytes_sent <= 0) { return false; }

      togo -= bytes_sent;
      head += bytes_sent;
    }

    return true;
  }

  void append_signal(int32_t signal) noexcept
  {
    ASSERT(signal < 0);
    // either there is no message, or it was ended just now
    // otherwise the signal will end up being part of the message
    ASSERT((used_size() == 0)
           || (!message_ends.empty()
               && (message_ends.back()
                   == gsl::narrow_cast<int32_t>(used_size()))));

    // we always use the "message start" placeholder that is placed
    // after every message
    // that this placeholder exists, is asserted above
    std::memcpy(data.data() + used_size(), &signal, sizeof(signal));

    // now we need to create a new placeholder
    insert_placeholder();
  }

 private:
  std::vector<std::int32_t> message_ends;
  std::vector<char> data;

  bool prepare_message_for_sending() noexcept
  {
    // we have to fill all the placeholders in the current message
    // with their respective values


    std::size_t current = 0;
    std::size_t message_idx = 0;

    // ignore the empty placeholder thats guaranteed to be at the end
    while (current < used_size()) {
      if (current + sizeof(int32_t) > data.size()) { return false; }

      char* ptr = data.data() + current;
      // its a placeholder if its all zeroes
      int32_t value;
      memcpy(&value, ptr, sizeof(value));

      current += sizeof(value);

      if (value != 0) {
        // this is a signal, skip
        int32_t network = htonl(value);
        memcpy(ptr, &network, sizeof(network));
        continue;
      }

      // ptr now points to a placeholder
      // now we need to determine the message size

      if (message_idx == message_ends.size()) { return false; }

      auto message_end = message_ends[message_idx++];

      auto message_start = gsl::narrow<int32_t>(current);

      int32_t message_size = message_end - message_start;

      if (message_size <= 0) { return false; }

      int32_t network = htonl(message_size);

      std::memcpy(ptr, &network, sizeof(network));

      current = message_end;
    }

    if (current != data.size() - sizeof(int32_t)) { return false; }

    return true;
  }

  void insert_placeholder() noexcept { (void)allocate(sizeof(int32_t)); }
};

#endif  // BAREOS_LIB_MESSAGE_BUFFER_H_
