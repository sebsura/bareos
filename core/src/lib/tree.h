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
#include <map>

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


class tree_builder;

class tree {
  friend tree_builder;

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

  node_ptr find(std::string_view path, node_ptr from) const;

  void insert_hl(JobId_t jobid, std::int32_t findex, node_index index);
  node_ptr lookup_hl(JobId_t jobid, std::int32_t findex) const;

  std::string path_to(node_index node) const;

  void MarkSubTree(node_index node);
  void MarkNode(node_index node);

  ~tree();
};

class tree_builder {
 private:
  struct entry;

 public:
  struct iter {
    friend tree_builder;
    void add_delta_part(JobId_t jobid, std::int32_t findex)
    {
      (void)jobid;
      (void)findex;
    }

    tree::node* operator->() { return &source->nodes[origin->node_idx]; }
    iter(const iter&) = default;

   private:
    iter(tree_builder::entry* origin, tree_builder* source)
        : origin{origin}, source{source}
    {
    }

    tree_builder::entry* origin;
    tree_builder* source;
  };
  tree_builder(std::size_t guessed_size);
  std::pair<iter, bool> insert(std::string_view path,
                               std::string_view name,
                               node_type type);

  void insert_original(iter it)
  {
    // it is (jobid, findex)
    auto jobid = it->JobId;
    auto findex = it->FileIndex;
    (void)jobid;
    (void)findex;
  }
  void insert_link(iter it, JobId_t jobid, std::int32_t findex)
  {
    // it links to (jobid, findex)
    (void)it;
    (void)jobid;
    (void)findex;
  }

  void remove(iter it);
  tree* build(bool mark_all);

 private:
  std::vector<tree::node> nodes;

  struct entry {
    std::size_t node_idx{(std::size_t)-1};
    entry* parent{nullptr};
    std::map<std::string, entry> children{};

    entry(entry* parent = nullptr) : parent{parent} {}

    std::pair<entry*, bool> get(std::string_view name)
    {
      auto [it, inserted] = children.emplace(name, this);
      entry* ent = &it->second;
      return {ent, inserted};
    }

    void remove(std::string_view name) { (void)name; }
  };

  entry root{};

  std::string cached_path{"/"};
  entry* cached{&root};

  iter as_iter(entry* ent)
  {
    if (ent->node_idx == (std::size_t)-1) {
      ent->node_idx = nodes.size();
      nodes.emplace_back();
    }
    return iter{ent, this};
  }

  entry* to_entry(iter it) { return it.origin; }

  void add_nodes(entry* entry, std::vector<tree::node>& nodes);
};

using node_ptr = tree::node_ptr;

/* External interface */
tree* new_tree(int count);
// TODO(ssura): should hardlinks even be part of the tree itself
//              or just part of the tree context ?
node_ptr LookupHardlink(tree* root, JobId_t jobid, std::int32_t findex);
void FreeTree(tree* root);

#endif  // BAREOS_LIB_TREE_H_
