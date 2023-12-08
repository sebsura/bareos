/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2009 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2023 Bareos GmbH & Co. KG

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
// Kern Sibbald, June MMII
/**
 * @file
 * Directory tree build/traverse routines
 */

#ifndef BAREOS_LIB_TREE_H_
#define BAREOS_LIB_TREE_H_

#include "include/config.h"

#include <cstdint>
#include <vector>

struct delta_list {
  delta_list* next;
  JobId_t JobId;
  int32_t FileIndex;
};

enum class node_type : int
{
  Root = 1,   /* root node */
  NewDir = 2, /* created directory to fill path */
  Dir = 3,    /* directory entry */
  DirNls = 4, /* directory -- no leading slash -- win32 */
  File = 5,   /* file entry */
};

#define TN_ROOT ((int)node_type::Root)
#define TN_NEWDIR ((int)node_type::NewDir)
#define TN_DIR ((int)node_type::Dir)
#define TN_DIR_NLS ((int)node_type::DirNls)
#define TN_FILE ((int)node_type::File)

struct node_index {
  std::size_t num;

  bool operator!=(const node_index& other) { return num != other.num; }
};

class tree {
 public:
  struct node {
    tree* root;
    node_index index;
    node_index end;
    int32_t FileIndex;
    int type;
    bool extract;
    bool extract_dir;
    char* fname;
    node* parent;
    uint64_t fhinfo, fhnode;
    JobId_t JobId;
    struct delta_list* delta_list;
    int delta_seq;
    bool inserted;
    bool hard_link;
    bool soft_link;
  };

 private:
  std::vector<bool> marked;
  std::vector<node> nodes;

 public:
  class iter {
    tree* source;
    node_index current;

   public:
    iter(tree& source, node_index start) : source{&source}, current{start} {}

    void next() { current.num += 1; }
    void next_sibling() { current = source->nodes[current.num].end; }

    node_index operator*() { return current; }
    iter& operator++()
    {
      next();
      return *this;
    }
    bool operator!=(const iter& other)
    {
      return source != other.source || current != other.current;
    }
  };

  node_index root() const;
  node_index insert_node(const char* path,
                         const char* fname,
                         node_type type,
                         node_index parent);

  node_index find(char* path, node_index from) const;

  void add_delta_part(node_index node, JobId_t jobid, std::int32_t findex);

  std::string path_to(node_index node) const;

  void insert_original_hl(JobId_t jobid, std::int32_t findex, node_index index);
  void insert_hl(JobId_t jobid, std::int32_t findex, node_index index);

  iter begin();
  iter end();

  void MarkSubTree(node_index node);
  void MarkNode(node_index node);

  ~tree();
};

using TREE_ROOT = tree;
using TREE_NODE = tree::node;

/* External interface */
TREE_ROOT* new_tree(int count);
TREE_NODE* insert_tree_node(char* path,
                            char* fname,
                            int type,
                            TREE_ROOT* root,
                            TREE_NODE* parent);
TREE_NODE* tree_cwd(char* path, TREE_ROOT* root, TREE_NODE* node);
void TreeAddDeltaPart(TREE_ROOT* root,
                      TREE_NODE* node,
                      JobId_t JobId,
                      int32_t FileIndex);
void FreeTree(TREE_ROOT* root);
POOLMEM* tree_getpath(TREE_NODE* node);
void TreeRemoveNode(TREE_ROOT* root, TREE_NODE* node);

struct HL_ENTRY {
  TREE_NODE* node;
};

void InsertHardlink(TREE_ROOT* root,
                    JobId_t jobid,
                    std::int32_t findex,
                    TREE_NODE* node);
HL_ENTRY* LookupHardlink(TREE_ROOT* root, JobId_t jobid, std::int32_t findex);

/**
 * Use the following for traversing the whole tree. It will be
 *   traversed in the order the entries were inserted into the
 *   tree.
 */

TREE_NODE* FirstTreeNode(TREE_ROOT* root);
TREE_NODE* NextTreeNode(TREE_NODE* node);

#define foreach_child(var, list) for ((var) = NULL; (var);)

#define TreeNodeHasChild(node) (((void)node), false)

#define first_child(node) (((void)node), (TREE_NODE*)(nullptr))


#endif  // BAREOS_LIB_TREE_H_
