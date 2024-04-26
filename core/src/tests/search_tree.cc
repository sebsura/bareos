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
#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#endif

#include "gtest/gtest.h"
#include "lib/directory_search_tree.h"

TEST(SearchTree, Test)
{
  try {
    search::file_tree<int> f;
    EXPECT_TRUE(f.insert("/a/b/c", 1).second);
    EXPECT_FALSE(f.insert("/a/b/c", -1).second);
    EXPECT_TRUE(f.insert("/a/b/d", 2).second);
    EXPECT_TRUE(f.insert("/a", 3).second);

    {
      auto [iter, found] = f.find("/a/b/c");
      EXPECT_TRUE(found);
      EXPECT_EQ(*iter.ptr, 1);
    }

    {
      auto [iter, found] = f.find("/a/b/d");
      EXPECT_TRUE(found);
      EXPECT_EQ(*iter.ptr, 2);
    }

    {
      auto [iter, found] = f.find("/a");
      EXPECT_TRUE(found);
      EXPECT_EQ(*iter.ptr, 3);
    }

  } catch (const std::system_error& ec) {
    FAIL() << "Error: " << ec.code() << " - " << ec.what() << "\n";
  } catch (const std::exception& ec) {
    FAIL() << "Error: " << ec.what() << "\n";
  }
}
