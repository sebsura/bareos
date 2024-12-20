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
#ifndef BAREOS_LIB_KV_STORE_H_
#define BAREOS_LIB_KV_STORE_H_

#include <unistd.h>
#include "lmdb.h"

#include <cstring>
#include <optional>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <type_traits>

template <typename T,
          typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
struct kv_store {
 public:
  using key_type = size_t;
  using value_type = T;

  static std::optional<kv_store> create(std::size_t capacity)
  {
    try {
      return kv_store(capacity);
    } catch (const kv_error& ex) {
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  kv_store(const kv_store&) = delete;
  kv_store& operator=(const kv_store&) = delete;
  kv_store(kv_store&& other)
  {
    env = other.env;
    dbi = other.dbi;
    cap = other.cap;
    txn = other.txn;
    other.env = nullptr;
  }
  kv_store& operator=(kv_store&& other)
  {
    env = other.env;
    dbi = other.dbi;
    cap = other.cap;
    txn = other.txn;
    other.env = nullptr;
    return *this;
  }

  ~kv_store()
  {
    if (env) {
      if (!commit_txn()) {
        mdb_txn_abort(txn);
        txn = nullptr;
      }

      mdb_dbi_close(env, dbi);

      const char* path{nullptr};
      mdb_env_get_path(env, &path);

      if (path != nullptr) { unlink(path); }

      mdb_env_close(env);
    }
  }

  const char* path() const
  {
    const char* path{nullptr};
    mdb_env_get_path(env, &path);
    return path;
  }

  bool store(key_type k, const value_type& v)
  {
    auto key = mdb_val_of(k);
    auto val = mdb_val_of(v);

    for (;;) {
      // we cannot handle MAP_FULL currently if we want to insert
      // multiple elements per transaction, because once you receive
      // that error, you need to abort the transaction ...
      // fix: cache k,v pairs of current transaction in a vector,
      //      so you can retry the operations after growing the db

      if (!ensure_write_txn()) { return false; }

      auto res = mdb_put(txn, dbi, &key, &val, 0);
      switch (res) {
        case MDB_SUCCESS: {
          return true;
        }
        case MDB_MAP_FULL: {
          if (!grow_db()) { return false; }
        } break;
        case MDB_TXN_FULL: {
          if (!commit_txn()) { return false; }
        } break;
        default: {
          return false;
        }
      }
    }
  }

  std::optional<value_type> retrieve(key_type k)
  {
    if (!ensure_read_txn()) { return std::nullopt; }

    auto key = mdb_val_of(k);
    MDB_val val;

    auto res = mdb_get(txn, dbi, &key, &val);
    switch (res) {
      case MDB_SUCCESS: {
        value_type v;
        std::memcpy(&v, val.mv_data, sizeof(value_type));
        return v;
      }
      case MDB_NOTFOUND: {
        return std::nullopt;
      }
      default: {
        return std::nullopt;
      }
    }
  }

  size_t capacity() const
  {
    MDB_envinfo info;

    if (mdb_env_info(env, &info) != MDB_SUCCESS) { return 0; }

    return info.me_mapsize;
  }

 private:
  struct kv_error : std::runtime_error {
    template <typename... Args>
    kv_error(Args&&... args) : std::runtime_error{std::forward<Args>(args)...}
    {
    }
  };

  struct transaction {
    MDB_txn* txn{nullptr};

    void commit() &&
    {
      if (mdb_txn_commit(txn) != MDB_SUCCESS) {
        throw kv_error("could not commit");
      }
      txn = nullptr;
    }

    ~transaction() { mdb_txn_abort(txn); }
  };

  kv_store(std::size_t capacity) : cap{capacity}
  {
    if (mdb_env_create(&env) != MDB_SUCCESS) {
      throw kv_error("could not create lmdb environment");
    }
    auto path = std::filesystem::temp_directory_path();
    std::string file = path / "bareos-kv-lmdb-XXXXXX";

    if (mkstemp(file.data()) < 0) {
      throw kv_error("could not create temporary file name");
    }

    if (mdb_env_set_mapsize(env, cap * sizeof(T)) != MDB_SUCCESS) {
      throw kv_error("could not set initial map size");
    }

    if (mdb_env_open(env, file.c_str(),
                     MDB_NOSUBDIR
                         | MDB_NOSYNC  // we dont care about persistance
                         | MDB_WRITEMAP | MDB_NOLOCK,
                     0664)
        != MDB_SUCCESS) {
      throw kv_error("could not open lmdb environment");
    }


    int fd;
    if (mdb_env_get_fd(env, &fd) != MDB_SUCCESS) {
      throw kv_error("could not get fd");
    }

    MDB_txn* create_db_txn;
    if (mdb_txn_begin(env, NULL, 0, &create_db_txn) != MDB_SUCCESS) {
      throw kv_error("could not create transaction");
    }

    transaction db_txn{create_db_txn};

    if (mdb_dbi_open(create_db_txn, NULL, MDB_INTEGERDUP | MDB_DUPFIXED, &dbi)
        != MDB_SUCCESS) {
      throw kv_error("could not create db");
    }

    std::move(db_txn).commit();
  }

  bool ensure_rdwr_txn()
  {
    if (txn != nullptr) {
      // invariant: txn is always read/write
      return true;
    }

    if (mdb_txn_begin(env, NULL, 0, &txn) != MDB_SUCCESS) { return false; }

    return true;
  }
  bool ensure_write_txn() { return ensure_rdwr_txn(); }
  bool ensure_read_txn() { return ensure_rdwr_txn(); }
  bool grow_db()
  {
    if (!commit_txn()) { return false; }

    cap = cap + (cap >> 2);
    if (mdb_env_set_mapsize(env, cap * sizeof(value_type)) != MDB_SUCCESS) {
      return false;
    }
    return true;
  }

  bool commit_txn()
  {
    if (txn == nullptr) { return true; }
    if (mdb_txn_commit(txn) != MDB_SUCCESS) { return false; }
    txn = nullptr;
    return true;
  }

  template <typename I,
            typename = std::enable_if_t<
                std::is_trivially_copyable_v<std::remove_reference_t<I>>>>
  MDB_val mdb_val_of(I&& x)
  {
    using InternalType = std::remove_cv_t<std::remove_reference_t<I>>;
    return MDB_val{
        .mv_size = sizeof(I),
        .mv_data = const_cast<InternalType*>(&x),
    };
  }


  size_t cap{};
  MDB_env* env{nullptr};
  MDB_dbi dbi{};
  MDB_txn* txn{nullptr};
};

#endif
