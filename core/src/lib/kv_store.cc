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

#include <optional>
#include <stdexcept>
#include <filesystem>

template <typename T>
struct kv_store {
public:
  std::optional<kv_store> create() {
    try {
      return kv_store();
    } catch (const kv_error& ex) {
      return std::nullopt;
    } catch (...) {
      return std::nullopt;
    }
  }

  kv_store(const kv_store&) = delete;
  kv_store& operator=(const kv_store&) = delete;
  kv_store(kv_store&& other) {
    env = other.env;
    dbi = other.dbi;
    other.env = nullptr;
  }
  kv_store& operator=(kv_store&& other) {
    env = other.env;
    dbi = other.dbi;
    other.env = nullptr;
    return *this;
  }

private:
  struct kv_error : std::runtime_error {};

  struct transaction {
    MDB_txn* txn{nullptr};

    void commit() && {
      if (mdb_txn_commit(txn) < 0) {
        throw kv_error("could not commit");
      }
      txn = nullptr;
    }

    ~transaction() {
      mdb_txn_abort(txn);
    }
  };

  kv_store()
  {
    if (mdb_env_create(&env) < 0) {
      throw kv_error("could not create lmdb environment");
    }
    auto path = std::filesystem::temp_directory_path();
    std::string file = path / "bareos-kv-lmdb-XXXXXX";

    if (mkstemp(file.data()) < 0) {
      throw kv_error("could not create temporary file name");
    }

    if (mdb_env_open(env, file.c_str(),
                     MDB_NOSUBDIR
                     | MDB_NOSYNC // we dont care about persistance
                     | MDB_WRITEMAP
                     | MDB_NOLOCK,
                     0664) < 0) {
      throw kv_error("could not open lmdb environment");
    }

    MDB_txn* create_db_txn;
    if (mdb_txn_begin(env, NULL, 0, &create_db_txn) < 0) {
      throw kv_error("could not create transaction");
    }

    transaction txn{create_db_txn};

    if (mdb_dbi_open(create_db_txn, NULL, 0, &dbi) < 0) {
      throw kv_error("could not create db");
    }

    std::move(txn).commit();
  }

  ~kv_store()
  {
    if (env) {
      mdb_dbi_close(env, dbi);

      const char* path{nullptr};
      mdb_env_get_path(env, &path);

      if (path != nullptr) { unlink(path); }

      mdb_env_close(env);
    }
  }

  MDB_env* env{nullptr};
  MDB_dbi dbi{};
};

#endif
