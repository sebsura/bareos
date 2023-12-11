/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2023 Bareos GmbH & Co. KG

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

#ifndef BAREOS_DIRD_UA_TREE_H_
#define BAREOS_DIRD_UA_TREE_H_

namespace directordaemon {

bool UserSelectFilesFromTree(TreeContext* tree);
struct tree_insertion_context {
  UaContext* ua{nullptr};
  std::size_t FileCount{0};
  std::size_t InsertionCount{0};
  // emit a '+' every DeltaCount files
  std::size_t DeltaCount{0};
  tree_builder builder;

  tree_insertion_context(UaContext* ua,
                         std::size_t DeltaCount,
                         std::size_t guessed_size)
      : ua{ua}, DeltaCount{DeltaCount}, builder{guessed_size}
  {
  }

  TreeContext to_tree(bool all)
  {
    TreeContext tree;

    tree.root = builder.build(all);
    tree.node = tree.root->root();
    tree.ua = ua;

    return tree;
  }
};
// expects a tree_insertion_context
int InsertTreeHandler(void* ctx, int num_fields, char** row);

} /* namespace directordaemon */
#endif  // BAREOS_DIRD_UA_TREE_H_
