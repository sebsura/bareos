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
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/**
 * @file
 * Directory tree load/save routines
 */

#ifndef BAREOS_LIB_TREE_SAVE_H_
#define BAREOS_LIB_TREE_SAVE_H_

#include "lib/tree.h"

#include <memory>

class job_tree_builder;

struct DeleteTreeBuilder {
  void operator()(job_tree_builder*);
};
std::unique_ptr<job_tree_builder, DeleteTreeBuilder> MakeTreeBuilder(std::size_t capacity = 0);

int job_tree_builder_cb(void* tree_builder, int num_cols, char** row);

std::size_t num_nodes(job_tree_builder* builder);

bool SaveTree(const char *path, TREE_ROOT* root);
TREE_ROOT* LoadTree(const char *path, std::size_t* size, bool mark_on_load);

struct job_node_data
{
  int32_t findex;
  std::size_t stat_idx;
  int32_t delta_seq;
  int32_t fhinfo;
  int32_t fhnode;
  int32_t linkfi;
};

struct my_data {
  std::uint32_t jobid;
  std::vector<job_node_data> nodes;
  std::vector<std::string> names;
  std::vector<struct stat> stats;
};

void insert(void* datap, int, char **row);
void finish(my_data& data);

#endif  // BAREOS_LIB_TREE_SAVE_H_
