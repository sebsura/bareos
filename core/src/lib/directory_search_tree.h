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
#ifndef BAREOS_LIB_DIRECTORY_SEARCH_TREE_H_
#define BAREOS_LIB_DIRECTORY_SEARCH_TREE_H_

#include <string_view>
#include <utility>
#include <memory>
#include <cstddef>

namespace search {

struct file_tree_impl;

file_tree_impl* create_tree(std::size_t element_size);
void destroy_tree(file_tree_impl* impl);

std::pair<void*, bool> tree_insert(file_tree_impl* tree,
                                   std::string_view dir,
                                   std::string_view file,
                                   const void* data);
std::pair<void*, bool> tree_find(file_tree_impl* tree,
                                 std::string_view dir,
                                 std::string_view file);

template <typename T> class file_tree {
 public:
  struct iter {
    iter() {}
    iter(T* ptr_) : ptr{ptr_} {}

    T* ptr{nullptr};
  };

  std::pair<iter, bool> insert(std::string_view path, const T& value)
  {
    auto idx = path.find_last_of("/");

    std::string_view dir{}, name{};

    if (idx == path.npos) {
      dir = path;
    } else {
      dir = path.substr(0, idx);
      name = path.substr(idx + 1);
    }

    auto [ptr, inserted] = tree_insert(impl, dir, name, &value);
    return {{reinterpret_cast<T*>(ptr)}, inserted};
  }

  std::pair<iter, bool> find(std::string_view path)
  {
    auto idx = path.find_last_of("/");

    std::string_view dir{}, name{};

    if (idx == path.npos) {
      dir = path;
    } else {
      dir = path.substr(0, idx);
      name = path.substr(idx + 1);
    }

    auto [ptr, found] = tree_find(impl, dir, name);
    return {{reinterpret_cast<T*>(ptr)}, found};
  }

  file_tree() : impl{create_tree(sizeof(T))} {}

  ~file_tree() { destroy_tree(impl); }

 private:
  file_tree_impl* impl{nullptr};
};

};      // namespace search
#endif  // BAREOS_LIB_DIRECTORY_SEARCH_TREE_H_
