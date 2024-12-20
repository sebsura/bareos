/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2024 Bareos GmbH & Co. KG

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

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "lib/kv_store.cc"

TEST(kv_store, CreateAndDestroy)
{
  auto store = kv_store<int>::create(100);

  ASSERT_NE(store, std::nullopt);

  std::string path = store->path();
  std::cout << "Opened " << path << std::endl;
  store.reset();  // destroy the file
  struct stat s;
  EXPECT_NE(stat(path.c_str(), &s), 0)  // the file should not exist anymore
      << "file " << path << " still exists!";
}

TEST(kv_store, InsertAndRetrieveInt)
{
  auto store = kv_store<int>::create(100'000'000);

  ASSERT_NE(store, std::nullopt);

  std::string path = store->path();
  std::cout << "Using " << path << std::endl;
  std::cout << "  map size = " << store->capacity() << std::endl;

  ASSERT_TRUE(store->store(1, 2));
  ASSERT_THAT(store->retrieve(1), testing::Optional(std::make_optional(2)));
}
