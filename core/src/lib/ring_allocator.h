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

#ifndef BAREOS_LIB_RING_ALLOCATOR_H_
#define BAREOS_LIB_RING_ALLOCATOR_H_

#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <array>

struct mapped_memory {
  mapped_memory() = default;
  mapped_memory(size_t s, char* mem) : size{s}, base{mem} {}
  ~mapped_memory();

  mapped_memory(const mapped_memory&) = delete;
  mapped_memory& operator=(const mapped_memory&) = delete;

  mapped_memory(mapped_memory&& m) { *this = std::move(m); }
  mapped_memory& operator=(mapped_memory&& m)
  {
    size = m.size;
    base = m.base;

    m.size = 0;
    m.base = nullptr;

    return *this;
  }

  std::size_t size{};
  char* base{};
};

struct ring_allocator {
  void* queue(size_t);
  void dequeue(size_t);

  // returns size of mappings,
  // mapping ptr 1, mapping ptr 2
  std::tuple<size_t, char*, char*> debug();

  static std::optional<ring_allocator> try_create(size_t page_size,
                                                  size_t num_pages);
  ring_allocator() = default;

 private:
  ring_allocator(mapped_memory m1, mapped_memory m2);

  size_t head{};
  size_t free{};

  mapped_memory map1{};
  mapped_memory map2{};

  friend void do_dmsg_flush();
};

#endif  // BAREOS_LIB_RING_ALLOCATOR_H_
