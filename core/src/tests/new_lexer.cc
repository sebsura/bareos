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

#include "lib/parse_err.h"
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

  auto start = lex::lex_point{};
  auto end = lex.current_offset;

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

  auto start = lex::lex_point{};
  auto end = lex.current_offset;

  lex::source_location all = {start, end};

  auto str = lex.format_comment(all, "something went wrong");

  std::cout << str << std::endl;

  // TODO: think about how to test this reliably
  EXPECT_EQ(1, 0);
}

lex::lexer simple_lex(std::string_view v)
{
  lex::lexer lex;
  lex.append_source(lex::source{
      .path = "string",
      .data = std::string{v},
  });

  return lex;
}

template <typename Int> struct SignedFixture : public ::testing::Test {
  using type = Int;
};

template <typename Int> struct UnsignedFixture : public ::testing::Test {
  using type = Int;
};

using SignedTypes = ::testing::Types<int16_t, int32_t, int64_t>;
using UnsignedTypes = ::testing::Types<uint16_t, uint32_t, uint64_t>;

struct IntNames {
  template <typename Int> static std::string GetName(int)
  {
    std::string name;
    if constexpr (std::is_signed_v<Int>) {
      name += "int";
    } else {
      name += "uint";
    }

    name += std::to_string(sizeof(Int) * 4);

    return name;
  }
};

TYPED_TEST_SUITE(SignedFixture, SignedTypes, IntNames);
TYPED_TEST_SUITE(UnsignedFixture, UnsignedTypes, IntNames);

TYPED_TEST(SignedFixture, simple)
{
  {
    auto lex = simple_lex("1234\n");
    EXPECT_EQ(lex::GetValue<typename TestFixture::type>(&lex), 1234);
  }
  {
    auto lex = simple_lex("-1234\n");
    EXPECT_EQ(lex::GetValue<typename TestFixture::type>(&lex), -1234);
  }
}

TYPED_TEST(UnsignedFixture, simple)
{
  {
    auto lex = simple_lex("1234\n");
    EXPECT_EQ(lex::GetValue<typename TestFixture::type>(&lex), 1234);
  }
  {
    auto lex = simple_lex("-1234\n");
    EXPECT_THROW(lex::GetValue<typename TestFixture::type>(&lex), parse_error);
  }
}
