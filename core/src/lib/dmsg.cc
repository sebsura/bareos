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
#include "lib/dmsg.h"
#include <sys/mman.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <optional>
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

static std::atomic<bool> quit_writer = false;

static void fut_wait_for(uint32_t* addr, uint32_t value, timespec* spec)
{
  syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, value, spec, NULL, 0);
}

static void fut_notify_one(uint32_t* addr)
{
  syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void fut_notify_all(uint32_t* addr)
{
  syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, std::numeric_limits<int>::max(),
          NULL, NULL, 0);
}

static void full_write(int fd, void* data, size_t len)
{
  size_t todo = len;

  while (todo > 0) {
    auto res = write(fd, data, len);
    if (res < 0) { break; }

    todo -= res;
  }
}

void write(fast_atomic* atmc, uint32_t value)
{
  __atomic_store_n(&atmc->state, value, __ATOMIC_RELEASE);

  fut_notify_all(&atmc->state);
}

uint32_t read(fast_atomic* atmc)
{
  uint32_t ret;

  __atomic_load(&atmc->state, &ret, __ATOMIC_ACQUIRE);

  return ret;
}

static void write_to_disk(int fd,
                          size_t num_pages,
                          char* base,
                          size_t page_size,
                          fast_atomic* start,
                          fast_atomic* end)
{
  size_t current_page = read(start);


  for (;;) {
    ASSERT(current_page < num_pages);


    size_t read_to = read(end);

    if (read_to == current_page) {
      if (quit_writer.load()) { break; }
      printf("[WRITER] wait on %zu\n", current_page);
      timespec s{};
      s.tv_nsec = size_t{5} * size_t{1000} * size_t{1000};  // 5ms
      fut_wait_for(&end->state, read_to, &s);
      continue;
    }

    printf("[WRITER] Writing pages %zu - %zu\n", current_page, read_to);

    // read_to might be bigger than num_pages!
    if (read_to < current_page) { read_to += num_pages; }
    ASSERT(read_to > current_page);

    auto diff_size = (read_to - current_page) * page_size;

    char* page = base + page_size * current_page;

    // base is part of a map-extended circular buffer, so this is fine!
    full_write(fd, page, diff_size);

    printf("[WRITER] Wrote pages %zu - %zu\n", current_page, read_to);

    current_page = read_to;

    if (current_page >= num_pages) { current_page -= num_pages; }

    write(start, current_page);
  }
}

ring_allocator dmsg::alloc{};
std::string dmsg::directory{};
int dmsg::current_file{};

uint32_t dmsg::old_read_start{};
fast_atomic dmsg::read_start{};
fast_atomic dmsg::read_end{};

std::array<fast_atomic, dmsg::num_pages> dmsg::page_writers{};

std::thread dmsg::writer;

void dmsg::init(const char* dir)
{
  alloc = *ring_allocator::try_create(page_size, 32);

  directory = dir;

  constexpr std::string_view suffix = ".log";

  std::string file_template = directory + "/dmsg_XXXXXX";
  file_template += suffix;

  current_file = mkstemps(file_template.data(), suffix.size());

  ASSERT(current_file >= 0);

  writer = std::thread{
      write_to_disk, current_file, num_pages, std::get<1>(alloc.debug()),
      page_size,     &read_start,  &read_end};
}

static bool between(uint32_t start, uint32_t end, uint32_t val)
{
  if (start < end) {
    return (start <= val) && (val < end);
  } else {
    return (end <= val) && (val < start);
  }
}

void dmsg::msg(std::string_view msg)
{
  char* mem = reinterpret_cast<char*>(alloc.queue(msg.size()));

  if (!mem) { return; }

  auto* base = std::get<1>(alloc.debug());

  uintptr_t b = reinterpret_cast<uintptr_t>(base);
  uintptr_t m = reinterpret_cast<uintptr_t>(mem);

  auto diff = m - b;

  auto start = diff / page_size;
  auto end = (diff + msg.size()) / page_size;
  auto one_past_end = (diff + msg.size() + 1) / page_size;

  printf("Wanting to write to %zu - %zu (-> %zu - %zu)\n", diff,
         diff + msg.size(), start, end);

  auto current_read_end = read(&read_end);

  for (;;) {
    auto rs = read(&read_start);

    if (rs != old_read_start) {
      size_t num_free_pages = 0;
      if (old_read_start < rs) {
        num_free_pages = rs - old_read_start;
      } else {
        num_free_pages = (rs - 0) + (num_pages - old_read_start);
      }
      alloc.dequeue(page_size * num_free_pages);
      old_read_start = rs;
    }
    if (!between(rs, current_read_end, end)) { break; }
    timespec wait;
    wait.tv_nsec = size_t{5} * size_t{1000} * size_t{1000};  // 5ms
    fut_wait_for(&read_start.state, rs, &wait);
  }

  memcpy(mem, msg.data(), msg.size());

  printf("Wrote %zu - %zu (-> %zu - %zu)\n", diff, diff + msg.size(), start,
         one_past_end);

  write(&read_end, one_past_end);
}

void do_dmsg_flush()
{
  auto rs = read(&dmsg::read_start);

  if (rs != dmsg::old_read_start) {
    size_t num_free_pages = 0;
    if (dmsg::old_read_start < rs) {
      num_free_pages = rs - dmsg::old_read_start;
    } else {
      num_free_pages = (rs - 0) + (dmsg::num_pages - dmsg::old_read_start);
    }
    dmsg::alloc.dequeue(dmsg::page_size * num_free_pages);
    dmsg::old_read_start = rs;
  }

  // dump everything that was allocated

  auto allocated = dmsg::alloc.map1.size - dmsg::alloc.free;
  char* value = dmsg::alloc.map1.base + dmsg::alloc.head;

  full_write(dmsg::current_file, value, allocated);
}

void dmsg::deinit()
{
  // todo: we should flush everything

  quit_writer = true;
  fut_notify_one(&read_end.state);

  writer.join();

  do_dmsg_flush();

  fsync(current_file);
  close(current_file);
  alloc = {};
}

// #endif
