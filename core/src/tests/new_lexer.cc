/*
  BAREOS® - Backup Archiving REcovery Open Sourced

  Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "gtest/gtest.h"
#  include "gtest/gtest-matchers.h"
#else
#  include "gtest/gtest.h"
#  include "gtest/gtest-matchers.h"
#  include "gmock/gmock-matchers.h"
#  include "include/bareos.h"
#endif


#include "lib/lex.h"

TEST(lexer, EmptyInput)
{
  lex::lexer lex;
  lex.sources.push_back(lex::source{
      .path = "string",
      .data = "",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  // EXPECT_TRUE(lex.finished());
}

TEST(lexer, QuotedString)
{
  lex::lexer lex;
  lex.sources.push_back(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
"Hallo"
)MULTILINE",
  });

  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(
      lex.next_token(),
      ::testing::Field(&lex::token::type, lex::token_type::QuotedString));
  EXPECT_EQ(lex.buffer, "Hallo");
  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  // EXPECT_TRUE(lex.finished());
}

TEST(lexer, QuotedStringContinuation)
{
  lex::lexer lex;
  lex.sources.push_back(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
"Hallo"
"Hallo"
)MULTILINE",
  });

  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(
      lex.next_token(),
      ::testing::Field(&lex::token::type, lex::token_type::QuotedString));
  EXPECT_EQ(lex.buffer, "HalloHallo");
  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  // EXPECT_TRUE(lex.finished());
}

TEST(lexer, Number)
{
  lex::lexer lex;
  lex.sources.push_back(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
1234
)MULTILINE",
  });

  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::Number));
  EXPECT_EQ(lex.buffer, "1234");
  // EXPECT_THAT(lex.next_token(), ::testing::Field(&lex::token::type,
  // lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  // EXPECT_TRUE(lex.finished());
}
