/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_DIRD_RESTORE_H_
#define BAREOS_DIRD_RESTORE_H_

#include <variant>
#include <sstream>
#include <optional>
#include <memory>
#include <cstdint>

#include "include/baconfig.h"
#include "dird_conf.h"
#include "bsr.h"

namespace directordaemon {

bool DoNativeRestore(JobControlRecord* jcr);
bool DoNativeRestoreInit(JobControlRecord* jcr);
void NativeRestoreCleanup(JobControlRecord* jcr, int TermCode);
void GenerateRestoreSummary(JobControlRecord* jcr,
                            int msg_type,
                            const char* TermMsg);

// struct enumeration {
//   std::vector<std::string> names;
// };

// using job_base_value = std::variant<std::string,
//                                     enumeration,
//                                     JobResource*,
//                                     ClientResource*,
//                                     CatalogResource*>;

// using job_value = std::variant<job_base_value,
//   std::optional<job_base_value>>;

// struct job_option {
//   const char* Name;
//   std::optional<job_value> Value;

//   enum class flag {
//     Required = (1 << 0),
//   };

//   std::uint32_t Flags;
// };


struct RestoreOptions {
  template <typename T> using required = T;

  struct ndmp_data {
    required<std::string> JobIds;
    required<TREE_ROOT*> restore_tree;
  };

  struct native_data {
    required<std::string> BootStrapPath;
    required<std::uint32_t> expected_file_count;
    required<bool> unlink_bsr;
  };

  struct regex_where : std::string {};
  struct where : std::string {};

  required<JobResource*> job{nullptr};
  required<std::variant<ndmp_data, native_data>> data;
  required<ClientResource*> restore_client{nullptr};
  required<CatalogResource*> catalog{nullptr};

  std::optional<std::variant<regex_where, where>> location;
  std::optional<replace_option> replace{};

  std::optional<std::string> backup_format;
  std::optional<std::string> plugin_options;
  std::optional<std::string> comment;

  std::optional<std::string> file_regex;
};

struct TreeArgs {
  enum class selection
  {
    None,
    All,
  };

  std::unordered_set<JobId_t> jobids;
  std::size_t estimated_size;
  selection initial_selection;
};

struct InsertTreeContext {
  std::size_t TotalCount;
  TREE_ROOT* root;

  std::optional<std::string> error;
  bool mark_on_create;
};

InsertTreeContext BuildDirectoryTree(BareosDb* db, TreeArgs args);

} /* namespace directordaemon */
#endif  // BAREOS_DIRD_RESTORE_H_
