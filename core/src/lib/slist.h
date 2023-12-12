/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2021-2023 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
// sorted intrusive list based on a red-black tree

#ifndef BAREOS_LIB_SLIST_H_
#define BAREOS_LIB_SLIST_H_

#include <type_traits>

template <typename T> class sitem {
  template <typename T1, typename C> friend class slist;
  T* parent{nullptr};
  T* left{nullptr};
  T* right{nullptr};
  bool red{false};

  friend T* leftmost(T* start)
  {
    auto* cur = start;
    while (cur->left) { cur = cur->left; }
    return cur;
  }
};

template <typename T, typename Comparator> class slist : Comparator {
 public:
  slist() : Comparator() { static_assert(std::is_base_of_v<sitem<T>, T>); }

  std::pair<T*, bool> add(T* val)
  {
    val->parent = val->left = val->right = nullptr;
    if (head == nullptr) {
      head = val;
      return {val, true};
    }

    auto* cur = head;
    auto* last = cur;
    int comp;  // the last direction taken
    while (cur) {
      last = cur;
      comp = Comparator::operator()(*cur, *val);
      if (comp < 0) {
        cur = cur->left;
      } else if (comp > 0) {
        cur = cur->right;
      } else {
        return {cur, false};
      }
    }

    if (comp < 0) {
      last->left = val;
    } else {
      last->right = val;
    }
    val->red = true;
    val->parent = last;

    balance(val);

    return {val, true};
  }

  struct iter {
    T* cur;

    iter(T* cur) : cur{cur} {}

    iter& operator++()
    {
      if (cur->right) {
        cur = leftmost(cur->right);
      } else {
        while (cur) {
          bool is_left = (cur->parent && cur == cur->parent->left);
          cur = cur->parent;

          if (is_left) { break; }
        }
      }

      return *this;
    }

    T& operator*() { return *cur; }

    bool operator!=(const iter& other) { return cur != other.cur; }
  };

  iter begin()
  {
    if (head) {
      return iter{leftmost(head)};
    } else {
      return iter{nullptr};
    }
  }

  iter end() { return iter{nullptr}; }

 private:
  T* head{nullptr};
  void balance(T* start)
  {
    auto* cur = start;
    while (cur != head && is_red(cur->parent)) {
      /* ASSERT(is_red(cur)); */
      auto* parent = cur->parent;
      auto* grandpa = parent->parent;
      /* ASSERT(is_black(grandpa)); */

      if (!grandpa) {
        // our parent is the root
        parent->red = false;
        return;  // finished
      } else if (parent == grandpa->left) {
        auto* uncle = grandpa->right;
        if (is_black(uncle)) {
          if (cur == parent->right) {
            // cur is an inner node //
            //      G               //
            //     / \              //
            //    P   U             //
            //     \                //
            //      N               //

            RotateLeft(parent);

            // now:                 //
            //      G		    //
            //     / \		    //
            //    N   U		    //
            //   /		    //
            //  P		    //

            // now switch N & P
            cur = parent;
            parent = grandpa->left;
          }
          // cur is an outer node   //
          //      G		    //
          //     / \		    //
          //    P   U		    //
          //   /		    //
          //  N			    //

          RotateRight(grandpa);
          parent->red = false;
          grandpa->red = true;
          return;  // finished
        } else {
          parent->red = false;
          uncle->red = false;
          grandpa->red = true;
          cur = grandpa;
        }
      } else {
        /* ASSERT(parent == grandpa->right); */
        auto* uncle = grandpa->left;
        if (is_black(uncle)) {
          if (cur == parent->left) {
            // cur is an inner node //
            //      G		    //
            //     / \		    //
            //    U   P		    //
            //       /		    //
            //      N		    //

            RotateRight(parent);

            // now:                 //
            //      G		    //
            //     / \		    //
            //    U   N             //
            //         \	    //
            //          P           //

            // now switch N & P
            cur = parent;
            parent = grandpa->right;
          }
          // cur is an outer node   //
          //      G		    //
          //     / \		    //
          //    U   P		    //
          //         \		    //
          //          N		    //
          RotateLeft(grandpa);
          parent->red = false;
          grandpa->red = true;
          return;  // finished
        } else {
          parent->red = false;
          uncle->red = false;
          grandpa->red = true;
          cur = grandpa;
        }
      }
    }
  }
  bool is_black(T* node) { return !node || !node->red; }
  bool is_red(T* node) { return node && node->red; }
  void RotateRight(T* parent)
  {
    auto* grandpa = parent->parent;
    auto* cur = parent->left;

    /* ASSERT(cur != nullptr); */

    auto* child = cur->right;

    //      G     //
    //     /	  //
    //    P	  //
    //   /	  //
    //  *	  //
    //   \	  //
    //    C	  //
    // --->	  //
    //      G	  //
    //     /	  //
    //    *	  //
    //     \	  //
    //      P	  //
    //     /	  //
    //    C	  //

    parent->left = child;
    if (child) child->parent = parent;
    cur->right = parent;
    parent->parent = cur;
    cur->parent = grandpa;

    if (grandpa) {
      if (grandpa->right == parent) {
        grandpa->right = cur;
      } else {
        grandpa->left = cur;
      }
    } else {
      head = cur;
    }
  }
  void RotateLeft(T* parent)
  {
    auto* grandpa = parent->parent;
    auto* cur = parent->right;

    /* ASSERT(cur != nullptr); */

    auto* child = cur->left;

    //      G     //
    //     /	  //
    //    P	  //
    //     \	  //
    //      *	  //
    //     /	  //
    //    C	  //
    // --->	  //
    //      G	  //
    //     /	  //
    //    *	  //
    //   /	  //
    //  P	  //
    //   \	  //
    //    C	  //

    parent->right = child;
    if (child) child->parent = parent;
    cur->left = parent;
    parent->parent = cur;
    cur->parent = grandpa;

    if (grandpa) {
      if (grandpa->right == parent) {
        grandpa->right = cur;
      } else {
        grandpa->left = cur;
      }
    } else {
      head = cur;
    }
  }
};

#endif  // BAREOS_LIB_SLIST_H_
