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

#include "include/baconfig.h"
#include "lib/ring_allocator.h"
#include <sys/mman.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <semaphore.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <thread>

// #if defined(HAVE_LINUX_OS)

mapped_memory::~mapped_memory()
{
  if (base && size) { munmap(base, size); }
}

ring_allocator::ring_allocator(mapped_memory m1, mapped_memory m2)
    : head{0}, free{m1.size}, map1{std::move(m1)}, map2{std::move(m2)}
{
  ASSERT(m1.size == m2.size);
  ASSERT(m1.base + m1.size == m2.base);
}

std::tuple<size_t, char*, char*> ring_allocator::debug()
{
  return {map1.size, map1.base, map2.base};
}

std::optional<ring_allocator> ring_allocator::try_create(size_t page_size,
                                                         size_t num_pages)
{
  size_t num_virtual_pages = num_pages * 2;

  auto fd = memfd_create("dmsg_backing_storage", MFD_CLOEXEC);

  if (fd < 0) {
    perror("memfd creation failed");
    return std::nullopt;
  }

  ftruncate(fd, num_pages * page_size);

  void* addr = mmap(nullptr, num_virtual_pages * page_size, PROT_NONE,
                    MAP_SHARED | MAP_NORESERVE | MAP_POPULATE, fd, 0);

  if (addr == MAP_FAILED) {
    perror("map failed");
    return std::nullopt;
  }

  void* first = mmap(addr, num_pages * page_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, 0);

  if (first == MAP_FAILED) {
    perror("map failed");
    return std::nullopt;
  }

  mapped_memory m1{num_pages * page_size, reinterpret_cast<char*>(first)};

  void* second = mmap((char*)addr + num_pages * page_size,
                      num_pages * page_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE | MAP_FIXED, fd, 0);

  if (second == MAP_FAILED) {
    perror("map failed");
    return std::nullopt;
  }

  mapped_memory m2{num_pages * page_size, reinterpret_cast<char*>(second)};

  return ring_allocator(std::move(m1), std::move(m2));
}


void* ring_allocator::queue(size_t alloc_size)
{
  ASSERT(head < map1.size);

  if (alloc_size <= free) {
    char* allocated = map1.base + head;
    head += alloc_size;

    if (head >= map1.size) { head -= map1.size; }

    free -= alloc_size;

    return allocated;
  }

  return nullptr;
}

void ring_allocator::dequeue(size_t alloc_size)
{
  ASSERT(alloc_size <= map1.size - free);

  free += alloc_size;
}



// #endif
