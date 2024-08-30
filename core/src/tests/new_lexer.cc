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
  lex.append_source(lex::source{
      .path = "string",
      .data = "",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(QuotedString, simple)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
"Hallo"
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(
      lex.next_token(),
      ::testing::Field(&lex::token::type, lex::token_type::QuotedString));
  EXPECT_EQ(lex.buffer, "Hallo");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(QuotedString, Continuation)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
"Hallo"
"Hallo"
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(
      lex.next_token(),
      ::testing::Field(&lex::token::type, lex::token_type::QuotedString));
  EXPECT_EQ(lex.buffer, "HalloHallo");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(QuotedString, NonContinuation)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
"Hallo"
1234
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(
      lex.next_token(),
      ::testing::Field(&lex::token::type, lex::token_type::QuotedString));
  EXPECT_EQ(lex.buffer, "Hallo");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::Number));
  EXPECT_EQ(lex.buffer, "1234");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(Number, SimpleDecimal)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
1234
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::Number));
  EXPECT_EQ(lex.buffer, "1234");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(Include, Number)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
@include/Number.inc
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::Number));
  EXPECT_EQ(lex.buffer, "1234");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

TEST(QuotedInclude, Number)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
@"include/Number.inc"
)MULTILINE",
  });

  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::Number));
  EXPECT_EQ(lex.buffer, "1234");
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::LineEnd));
  EXPECT_THAT(lex.next_token(),
              ::testing::Field(&lex::token::type, lex::token_type::FileEnd));
  EXPECT_TRUE(lex.finished());
}

static void parse_all(lex::lexer& lex)
{
  while (!lex.finished()) { (void)lex.next_token(); }
}

TEST(Comment, simple)
{
  lex::lexer lex;
  auto path = "string";
  auto data = R"MULTILINE(
Hallo
1234
)MULTILINE";
  lex.append_source(lex::source{
      .path = path,
      .data = data,
  });

  parse_all(lex);

  auto start = size_t{0};
  auto end = lex.current_global_offset;

  lex::source_location all = {start, end};

  std::string_view comment = "something went wrong";


  auto str = lex.format_comment(all, comment);

  std::cout << str << std::endl;

  // TODO: think about how to test this reliably
  EXPECT_EQ(1, 0);
}

TEST(Comment, include)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data =
          R"MULTILINE(
Hallo
@"include/Number.inc"
1234
)MULTILINE",
  });

  parse_all(lex);

  auto start = size_t{0};
  auto end = lex.current_global_offset;

  lex::source_location all = {start, end};

  auto str = lex.format_comment(all, "something went wrong");

  std::cout << str << std::endl;

  // TODO: think about how to test this reliably
  EXPECT_EQ(1, 0);
}
