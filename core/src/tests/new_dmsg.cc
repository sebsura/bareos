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
#  include "gtest/gtest.h"
#else
#  include "gtest/gtest.h"
#  include "include/bareos.h"
#endif

#include "lib/dmsg.h"

TEST(dmsg, ring_buffer)
{
  constexpr size_t page_size = 64 * 1024;
  auto alloc = ring_allocator::try_create(page_size, 20);

  ASSERT_NE(alloc, std::nullopt);

  auto* mem1 = alloc->queue(page_size * 1);
  ASSERT_NE(mem1, nullptr);
  auto* mem2 = alloc->queue(page_size * 1);
  ASSERT_NE(mem2, nullptr);
  alloc->dequeue(page_size * 1);  // dealloc mem1
  auto* mem3 = alloc->queue(page_size * 19);
  ASSERT_NE(mem3, nullptr);

  using page_t = char[page_size];

  auto* page = reinterpret_cast<page_t*>(mem1);
  auto* pages = reinterpret_cast<page_t*>(mem3);

  pages[18][0] = 'c';
  EXPECT_NE(&page[0][0], &pages[18][0]);
  EXPECT_EQ(page[0][0], pages[18][0]);
}

TEST(dmsg, dmsg_some)
{
  dmsg::init("dir");

  std::string quarter_page(dmsg::page_size / 4, '\n');

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);
  dmsg::msg(quarter_page);

  dmsg::deinit();
}

TEST(dmsg, dmsg_much)
{
  dmsg::init("dir");

  char buffer[100];
  for (size_t i = 0; i < 1'000'000; ++i) {
    int len = snprintf(buffer, sizeof(buffer), "%zu\n", i);
    ASSERT_GE(len, 0);
    dmsg::msg(std::string_view{buffer, (size_t)len});
  }

  dmsg::deinit();
}
