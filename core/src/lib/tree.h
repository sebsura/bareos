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

struct node_index {
  std::size_t num;

  bool operator!=(const node_index& other) { return num != other.num; }
};

class tree {
 public:
  struct node {
    struct sibling_iter {
      tree* source;
      node_index current;

     public:
      sibling_iter(tree& source, node_index start)
          : source{&source}, current{start}
      {
      }

      void next() { current = source->nodes[current.num].end; }

      node& operator*() { return source->nodes[current.num]; }
      sibling_iter& operator++()
      {
        next();
        return *this;
      }
      bool operator!=(const sibling_iter& other)
      {
        return source != other.source || current != other.current;
      }
    };

    struct siblings {
      tree* source;
      node_index s, e;

      sibling_iter begin() { return sibling_iter{*source, s}; }
      sibling_iter end() { return sibling_iter{*source, e}; }
    };

    bool has_children() { return false; }
    siblings children() { return siblings{nullptr, end, end}; }

    tree* root;
    node_index index;
    node_index end;

    JobId_t jobid() const { return JobId; }
    std::int32_t findex() const { return FileIndex; }
    bool marked() const { return extract_dir || extract; }
    bool markedf() const { return extract; }
    bool markedd() const { return extract_dir; }

    const char* name() const { return fname; }
    std::string fullpath() const { return ""; }

    node* parent() const { return parent_; }

    uint64_t fh_info() const { return fhinfo; }
    uint64_t fh_node() const { return fhnode; }

    node_type type() const { return t; }

    int32_t dseq() const { return delta_seq; }

    struct delta_list* dlist() const { return delta_list; }

    bool was_inserted() const { return inserted; }

    bool is_hl() const { return hard_link; }
    bool is_sl() const { return soft_link; }

    // only used for building
    void set_fh(uint64_t info, uint64_t node)
    {
      fhinfo = info;
      fhnode = node;
    }

    void do_extract(bool d = true) { extract = d; }
    void do_extract_dir(bool d = true) { extract_dir = d; }

    void set_hard_link(bool hard_link) { this->hard_link = hard_link; }
    void set_findex(int32_t findex) { this->FileIndex = findex; }
    void set_jobid(JobId_t jobid) { this->JobId = jobid; }
    void set_type(node_type type) { this->t = type; }
    void set_soft_link(bool soft_link) { this->soft_link = soft_link; }
    void set_dseq(int32_t dseq) { this->delta_seq = dseq; }

   private:
    int32_t FileIndex;
    node_type t;
    bool extract;
    bool extract_dir;
    char* fname;
    node* parent_;
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
  std::unordered_map<std::uint64_t, node_index> hardlinks;

 public:
  class iter {
    tree* source;
    node_index current;

   public:
    iter(tree& source, node_index start) : source{&source}, current{start} {}

    void next() { current.num += 1; }
    void next_sibling() { current = source->nodes[current.num].end; }

    node& operator*() { return source->nodes[current.num]; }
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

  constexpr node_index root() const { return node_index{0}; }

  constexpr node_index invalid() const { return node_index{(std::size_t)-1}; }


  node_index insert_node(const char* path,
                         const char* fname,
                         node_type type,
                         node_index parent);

  node* find(char* path, node* from) const;

  void add_delta_part(node_index node, JobId_t jobid, std::int32_t findex);

  std::string path_to(node_index node) const;

  void insert_hl(JobId_t jobid, std::int32_t findex, node_index index);
  node_index lookup_hl(JobId_t jobid, std::int32_t findex);

  iter begin();
  iter end();

  void MarkSubTree(node_index node);
  void MarkNode(node_index node);

  tree::node* at(node_index idx) { return &nodes[idx.num]; }

  ~tree();
};

using TREE_ROOT = tree;
// todo: should hardlinks even be part of the tree itself
//       or just part of the tree context ?
using HL_ENTRY = tree::node;
using node_ptr = tree::node*;

/* External interface */
TREE_ROOT* new_tree(int count);
HL_ENTRY* LookupHardlink(TREE_ROOT* root, JobId_t jobid, std::int32_t findex);
void FreeTree(TREE_ROOT* root);

// only used during creation
void TreeAddDeltaPart(TREE_ROOT* root,
                      node_ptr node,
                      JobId_t JobId,
                      int32_t FileIndex);
node_ptr insert_tree_node(char* path,
                          char* fname,
                          node_type type,
                          TREE_ROOT* root,
                          node_ptr node);
void TreeRemoveNode(TREE_ROOT* root, node_ptr node);

void InsertHardlink(TREE_ROOT* root,
                    JobId_t jobid,
                    std::int32_t findex,
                    node_ptr node);

#endif  // BAREOS_LIB_TREE_H_
