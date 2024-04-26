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

#include <string_view>
#include <utility>
#include <memory>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>
#include <optional>

#include "directory_search_tree.h"
#include "lmdb/lmdb.h"
#include "include/baconfig.h"

// this data structure is a map path -> unique idx
struct directory_map {
  std::unordered_map<std::string, std::size_t> m;

  std::pair<std::size_t, bool> insert(std::string_view dir)
  {
    auto [it, inserted] = m.emplace(dir, m.size());
    return {it->second, inserted};
  }

  std::optional<std::size_t> find(std::string_view dir)
  {
    // this is so dumb
    std::string s{dir};
    if (auto found = m.find(s); found != m.end()) { return found->second; }
    return std::nullopt;
  }
};

struct file_storage {
  MDB_env* env{nullptr};
  MDB_txn* txn{nullptr};
  MDB_dbi dbi{};

  bool ok{false};
  file_storage()
  {
    char tmpname[L_tmpnam];
    constexpr unsigned flags = MDB_NOMEMINIT | MDB_NOLOCK | MDB_NOSUBDIR;
    constexpr mdb_mode_t mode = 0600;
    if (mdb_env_create(&env)) { return; }
    if (mdb_env_set_maxreaders(env, 1)) { return; }
    if (mdb_env_set_mapsize(env, 1024 * 1024 * 1024)) { return; }
    if (mdb_env_open(env, std::tmpnam(tmpname), flags, mode)) { return; }

    if (mdb_env_get_maxkeysize(env)
        < (int)sizeof(std::size_t) + MAX_NAME_LENGTH) {
      return;
    }

    if (mdb_txn_begin(env, nullptr, 0, &txn)) { return; }

    if (mdb_dbi_open(txn, nullptr, MDB_CREATE, &dbi)) { return; }

    ok = true;
  }

  ~file_storage()
  {
    mdb_dbi_close(env, dbi);
    if (txn) { mdb_txn_abort(txn); }
    mdb_env_close(env);
    // unlink(name.c_str());
  }

  bool CommitAndReopen()
  {
    if (mdb_txn_commit(txn)) { return false; }
    if (mdb_txn_begin(env, nullptr, 0, &txn)) { return false; }
    return true;
  }

  bool Enlarge()
  {
    // if (mdb_txn_commit(txn)) { return false; }
    // mdb_dbi_close(env, dbi);
    // mdb_env_close(env);
    // if (mdb_env_set_mapsize(env, size * 2)) { return false; }
    return false;
  }

  bool insert(std::size_t idx,
              std::string_view name,
              const void* data,
              std::size_t datasize)
  {
    char key[MAX_NAME_LENGTH + sizeof(std::size_t)];

    if (name.size() > MAX_NAME_LENGTH) { return false; }

    std::memcpy(key, &idx, sizeof(std::size_t));
    std::memcpy(key + sizeof(size_t), name.data(), name.size());

    MDB_val mkey
        = {.mv_size = sizeof(std::size_t) + name.size(), .mv_data = key};
    MDB_val mdata = {.mv_size = datasize, .mv_data = (void*)data};

    bool try_again = true;
    while (try_again) {
      auto res = mdb_put(txn, dbi, &mkey, &mdata, MDB_NOOVERWRITE);
      switch (res) {
        case 0: {
          return true;
        }
        case MDB_KEYEXIST: {
          try_again = false;
        } break;
        case MDB_TXN_FULL: {
          try_again = CommitAndReopen();
        } break;
        case MDB_MAP_FULL: {
          try_again = Enlarge();
        } break;
      }
    }
    return false;
  }

  std::pair<void*, bool> find(std::size_t idx, std::string_view name)
  {
    char key[MAX_NAME_LENGTH + sizeof(std::size_t)];

    if (name.size() > MAX_NAME_LENGTH) { return {nullptr, false}; }

    std::memcpy(key, &idx, sizeof(std::size_t));
    std::memcpy(key + sizeof(size_t), name.data(), name.size());

    MDB_val mkey
        = {.mv_size = sizeof(std::size_t) + name.size(), .mv_data = key};
    MDB_val mdata = {};

    if (mdb_get(txn, dbi, &mkey, &mdata)) { return {nullptr, false}; }

    return {mdata.mv_data, true};
  }
};

namespace search {

struct file_tree_impl {
  file_tree_impl(std::size_t size_of_element) : element_size{size_of_element} {}

  std::size_t element_size;
  directory_map dirs;
  file_storage storage;
};

file_tree_impl* create_tree(std::size_t element_size)
{
  return new file_tree_impl(element_size);
}
void destroy_tree(file_tree_impl* tree) { delete tree; }

std::pair<void*, bool> tree_insert(file_tree_impl* tree,
                                   std::string_view dir,
                                   std::string_view name,
                                   const void* data)
{
  auto [idx, inserted] = tree->dirs.insert(dir);

  if (inserted) {
    // if the directory did not exist before then the file definitely also
    // does not exist

    tree->storage.insert(idx, name, data, tree->element_size);

    return {nullptr, true};
  }

  return {nullptr, tree->storage.insert(idx, name, data, tree->element_size)};
}

std::pair<void*, bool> tree_find(file_tree_impl* tree,
                                 std::string_view dir,
                                 std::string_view file)
{
  if (auto found = tree->dirs.find(dir)) {
    return tree->storage.find(*found, file);
  }

  return {nullptr, false};
}

}  // namespace search
