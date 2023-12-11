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
  std::size_t num{(std::size_t)-1};

  bool operator==(const node_index& other) { return num == other.num; }
  bool operator!=(const node_index& other) { return num != other.num; }

  bool valid() { return num != (std::size_t)-1; }
};


class tree {
 protected:
  struct node {
    node_type t;
    node_index end;
    int32_t FileIndex;
    char* fname;
    uint64_t fhinfo, fhnode;
    JobId_t JobId;
    struct delta_list* delta_list;
    int delta_seq;
    bool inserted;
    bool hard_link;
    bool soft_link;
  };

  struct marked {
    bool extract;
    bool extract_dir;
  };
  std::vector<marked> status;
  std::vector<node> nodes;
  std::unordered_map<std::uint64_t, node_index> hardlinks;

  const node& at(node_index idx) const { return nodes[idx.num]; }
  const marked& marked_at(node_index idx) const { return status[idx.num]; }

 public:
  struct node_ptr {
    struct sibling_iter {
      const tree* source;
      node_index current;

     public:
      sibling_iter(const tree& source, node_index start)
          : source{&source}, current{start}
      {
      }

      void next() { current = source->at(current).end; }

      node_ptr operator*() { return node_ptr{source, current}; }
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
      const tree* source;
      node_index s, e;

      sibling_iter begin() { return sibling_iter{*source, s}; }
      sibling_iter end() { return sibling_iter{*source, e}; }
    };

    class subtree_iter {
      const tree* source;
      node_index current;

     public:
      subtree_iter(const tree& source, node_index start)
          : source{&source}, current{start}
      {
      }

      void next() { current.num += 1; }

      node_ptr operator*() { return {source, current}; }
      subtree_iter& operator++()
      {
        next();
        return *this;
      }

      bool operator!=(const subtree_iter& other)
      {
        return source != other.source || current != other.current;
      }
    };

    struct subtree_entries {
      const tree* source;
      node_index s, e;

      subtree_iter begin() { return subtree_iter{*source, s}; }
      subtree_iter end() { return subtree_iter{*source, e}; }
    };

    bool has_children()
    {
      node_index start{index.num + 1};
      node_index end = root->at(index).end;
      return start != end;
    }

    siblings children()
    {
      node_index start = index;
      node_index end = root->at(index).end;
      return siblings{root, start, end};
    }

    subtree_entries subtree()
    {
      node_index start{index.num + 1};
      node_index end = root->at(index).end;
      return subtree_entries{root, start, end};
    }


    JobId_t jobid() const { return me().JobId; }
    std::int32_t findex() const { return me().FileIndex; }
    bool marked() const { return markedf() || markedd(); }
    bool markedf() const { return status().extract; }
    bool markedd() const { return status().extract_dir; }

    const char* name() const { return me().fname; }
    std::string fullpath() const { return root->path_to(idx()); }

    node_ptr parent() const
    {
      auto idx = index;
      // go backwards until we find the parent
      while (idx.num > 0) {
        idx.num -= 1;
        if (root->at(idx).end.num > index.num) { return node_ptr{root, idx}; }
      }
      return root->root();
    }

    uint64_t fh_info() const { return me().fhinfo; }
    uint64_t fh_node() const { return me().fhnode; }

    node_type type() const { return me().t; }

    int32_t dseq() const { return me().delta_seq; }

    struct delta_list* dlist() const { return me().delta_list; }

    bool was_inserted() const { return me().inserted; }

    bool is_hl() const { return me().hard_link; }
    bool is_sl() const { return me().soft_link; }

    // only used for building
    void set_fh(uint64_t info, uint64_t node)
    {
      auto& m = const_cast<tree::node&>(me());
      m.fhinfo = info;
      m.fhnode = node;
    }

    void do_extract(bool d = true)
    {
      auto& s = const_cast<tree::marked&>(status());
      s.extract = d;
    }
    void do_extract_dir(bool d = true)
    {
      auto& s = const_cast<tree::marked&>(status());
      s.extract_dir = d;
    }

    void set_hard_link(bool hard_link)
    {
      auto& m = const_cast<tree::node&>(me());
      m.hard_link = hard_link;
    }
    void set_findex(int32_t findex)
    {
      auto& m = const_cast<tree::node&>(me());
      m.FileIndex = findex;
    }
    void set_jobid(JobId_t jobid)
    {
      auto& m = const_cast<tree::node&>(me());
      m.JobId = jobid;
    }
    void set_type(node_type type)
    {
      auto& m = const_cast<tree::node&>(me());
      m.t = type;
    }
    void set_soft_link(bool soft_link)
    {
      auto& m = const_cast<tree::node&>(me());
      m.soft_link = soft_link;
    }
    void set_dseq(int32_t dseq)
    {
      auto& m = const_cast<tree::node&>(me());
      m.delta_seq = dseq;
    }

    operator bool() { return root && index.valid(); }

    node_index idx() const { return index; }

    node_ptr() {}
    node_ptr(const tree* root) : root{root} {}
    node_ptr(const tree* root, node_index index) : root{root}, index{index} {}

   private:
    const tree* root{nullptr};
    node_index index{};

    const tree::node& me() { return root->at(index); }
    const tree::marked& status() { return root->marked_at(index); }

    const tree::node& me() const { return root->at(index); }
    const tree::marked& status() const { return root->marked_at(index); }
  };

 private:
 public:
  node_ptr root() const { return node_ptr{this, node_index{0}}; }

  node_ptr invalid() const { return node_ptr{this}; }


  node_index insert_node(const char* path,
                         const char* fname,
                         node_type type,
                         node_index parent);

  node_ptr find(std::string_view path, node_ptr from) const;

  void add_delta_part(node_index node, JobId_t jobid, std::int32_t findex);

  void insert_hl(JobId_t jobid, std::int32_t findex, node_index index);
  node_ptr lookup_hl(JobId_t jobid, std::int32_t findex) const;

  std::string path_to(node_index node) const;

  void MarkSubTree(node_index node);
  void MarkNode(node_index node);

  ~tree();
};

using TREE_ROOT = tree;
// todo: should hardlinks even be part of the tree itself
//       or just part of the tree context ?
using node_ptr = tree::node_ptr;

/* External interface */
TREE_ROOT* new_tree(int count);
node_ptr LookupHardlink(TREE_ROOT* root, JobId_t jobid, std::int32_t findex);
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
