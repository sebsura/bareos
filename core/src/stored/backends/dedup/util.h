/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

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

#ifndef BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_
#define BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_

#include <vector>
namespace dedup {
class read_buffer {
 public:
  read_buffer(const char* begin, std::size_t size)
      : begin{begin}, current{begin}, end{begin + size}
  {
  }

  template <typename T = char> const T* read_array(std::size_t num)
  {
    if (current + num * sizeof(T) >= end) { return nullptr; }
    auto* value = reinterpret_cast<const T*>(current);
    current += num * sizeof(T);
    return value;
  }
  template <typename T> const T* read() { return read_array<T>(1); }

  ssize_t bytes_read() { return current - begin; }

  ssize_t bytes_to_go() { return end - current; }

 private:
  const char* begin;
  const char* current;
  const char* end;
};

struct write_buffer {
 public:
  write_buffer(char* begin, std::size_t size)
      : begin{begin}, current{begin}, end{begin + size}
  {
  }

  template <typename T = char> bool write_array(std::size_t num, const T* vals)
  {
    if (current + num * sizeof(T) >= end) { return false; }

    std::memcpy(current, vals, num * sizeof(T));
    current += num * sizeof(T);

    return true;
  }

  template <typename T> bool write(const T& val)
  {
    return write_array<T>(1, &val);
  }

  ssize_t bytes_written() { return current - begin; }

  ssize_t bytes_free() { return end - current; }

  char* reserve(std::size_t size)
  {
    if (current + size >= end) {
      return nullptr;
    } else {
      char* reserved = current;
      current += size;
      return reserved;
    }
  }

 private:
  char* begin;
  char* current;
  char* end;
};

class fd {
 public:
  enum class open_flags
  {
    CREATE_READ_WRITE,
    READ_WRITE,
    WRITE_ONLY,
    READ_ONLY,
  };

  fd(const char* path, open_flags flags, int mode)
      : fd{::open(path, to_system(flags), mode)}, path
  {
    path
  }
  bool read(void* data, ssize_t num_bytes)
  {
    return ::read(fd, data, num_bytes) == num_bytes;
  }

  bool write(const void* data, ssize_t num_bytes);
  {
    return ::write(fd, data, num_bytes) == num_bytes;
  }

  bool delete()
  {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }

    return 0 == unlink(path.c_str());
  }

  bool flush() { return 0 == fsync(fd); }

  bool goto_end() { return lseek(fd, 0, SEEK_END) >= 0; }

  bool goto_begin() { return lseek(fd, 0, SEEK_SET) >= 0; }

  bool truncate(std::size_t size) { return 0 == ftruncate(fd, size); }

  bool is_ok() { return !error; }

 private:
  int fd;
  std::string path;
  bool error{false};

  static constexpr int to_system(open_flags flags) { return 0; }
};

template <typename T> class file_based_vector {
 public:
  file_based_vector(char* path)
      : file(path, fd::open_flags::READ_WRITE), size{0}, capacity{0}
  {
  }

  bool reserve(std::size_t new_cap)
  {
    if (capacity < new_cap) {
      capacity = new_cap;
      file.truncate(capacity * sizeof(T));
    }
  }

  bool reserve_extra(std::size_t extra) { return reserve(size + extra); }

  bool push_back(const T&) { return push_back_array(&T, 1); }

  bool push_back_array(const T* arr, std::size_t count)
  {
    write_head += count;
    if (write_head > capacity) {
      do {
        capacity += allocation_amount;
      } while (write_head > capacity);
      file.truncate(capacity * sizeof(T));
    }
    fd.write(arr, count * sizeof(T));
    size = write_head;
  }

  std::vector<T> pop_front_array(std::size_t count)
  {
    std::size_t actual_count = std::min(count, size - write_head);
    std::vector<T> array;
    array.resize(actual_count);
    if (!fill_array(array.data(), actual_count)) { return {}; }

    return array;
  }

  std::optional<T> pop_front()
  {
    T val;
    if (!fill_array(&val, 1)) { return std::nullopt; }
    return val;
  }

  std::size_t size() const { return size; }

  std::size_t capacity() const { return capacity; }

  bool is_ok() const { return !error; }

 private:
  std::size_t size;
  std::size_t capacity;
  // reads and writes are done from this position
  std::size_t write_head{0};
  fd file;
  bool error{false};

  constexpr static allocation_size = 1 * 1024 * 1024 * 1024 / sizeof(T);

  bool fill_array(T* arr, std::size_t count)
  {
    if (write_head + count > size) return false;
    if (!file.read(arr, count * sizeof(T))) { return false; }

    if (result < 0) { return false; }

    write_head += count;

    return true;
  }
};
} /* namespace dedup */

#endif  // BAREOS_STORED_BACKENDS_DEDUP_UTIL_H_
