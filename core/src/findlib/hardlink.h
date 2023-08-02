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
#ifndef BAREOS_FINDLIB_HARDLINK_H_
#define BAREOS_FINDLIB_HARDLINK_H_

#include <unordered_map>
#include <vector>
#include <string>
#include <type_traits>

/**
 * Structure for keeping track of hard linked files, we
 * keep an entry for each hardlinked file that we save,
 * which is the first one found. For all the other files that
 * are linked to this one, we save only the directory
 * entry so we can link it.
 */
struct CurLink {
  uint32_t FileIndex{0};      /**< Bareos FileIndex of this file */
  int32_t digest_stream{0};   /**< Digest type if needed */
  std::vector<char> digest{}; /**< Checksum of the file if needed */
  std::string name;           /**< The name */

  CurLink(const char* fname) : name{fname} {}
  void set_digest(int32_t new_digest_stream,
                  const char* new_digest,
                  uint32_t len)
  {
    if (digest.empty()) {
      digest.assign(new_digest, new_digest + len);
      digest_stream = new_digest_stream;
    }
  }
};

// workaround for the win32 compat layer defining struct stat
// without dev_t/ino_t
using our_dev_t = decltype(static_cast<struct stat*>(nullptr)->st_dev);
using our_ino_t = decltype(static_cast<struct stat*>(nullptr)->st_ino);

// sometimes decltype is weird.  Make sure
// that we ended up with T and not T&
static_assert(!std::is_reference_v<our_dev_t>);
static_assert(!std::is_reference_v<our_ino_t>);

struct Hardlink {
  our_dev_t dev; /**< Device */
  our_ino_t ino; /**< Inode with device is unique */

  friend bool operator==(const Hardlink& l, const Hardlink& r)
  {
    return l.dev == r.dev && l.ino == r.ino;
  }
};

template <> struct std::hash<Hardlink> {
  std::size_t operator()(const Hardlink& link) const
  {
    auto hash1 = std::hash<decltype(link.dev)>{}(link.dev);
    auto hash2 = std::hash<decltype(link.ino)>{}(link.ino);

    // change this when N3876 (std::hash_combine) or something similar
    // is finally implemented.
    return hash1 + 67 * hash2;
  }
};

using LinkHash = std::unordered_map<Hardlink, CurLink>;

#endif  // BAREOS_FINDLIB_HARDLINK_H_
