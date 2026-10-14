/* BAREOS® - Backup Archiving REcovery Open Sourced
 *
 * Copyright (C) 2026-2026 Bareos GmbH & Co. KG
 *
 * This program is Free Software; you can redistribute it and/or
 * modify it under the terms of version three of the GNU Affero General Public
 * License as published by the Free Software Foundation and included
 * in the file LICENSE.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. */

#include <lib/lex.h>

#include <unordered_map>
#include <cstdint>

using u32 = uint32_t;

struct OidcConfig {};

struct token {
  u32 content_start;
  u32 content_end;

  int type;
};

struct ParsedBlock {
  std::vector<std::string> files;
  std::vector<char> content;
  std::vector<token> tokens;

  std::string_view token_text(size_t token_idx)
  {
    auto start = tokens[token_idx].content_start;
    auto end = tokens[token_idx].content_end;
    auto len = end - start;

    return std::string_view{content.data() + start, len};
  }

  std::string_view token_type(size_t token_idx)
  {
    auto type = tokens[token_idx].type;
    switch (type) {
      case BCT_EOF: {
        return "EOF";
      } break;
      case BCT_NUMBER: {
        return "NUMBER";
      } break;
      case BCT_IPADDR: {
        return "IPADDR";
      } break;
      case BCT_IDENTIFIER: {
        return "IDENTIFIER";
      } break;
      case BCT_UNQUOTED_STRING: {
        return "UNQUOTED_STRING";
      } break;
      case BCT_QUOTED_STRING: {
        return "QUOTED_STRING";
      } break;
      case BCT_BOB: {
        return "BOB";
      } break;
      case BCT_EOB: {
        return "EOB";
      } break;
      case BCT_EQUALS: {
        return "EQUALS";
      } break;
      case BCT_COMMA: {
        return "COMMA";
      } break;
      case BCT_EOL: {
        return "EOL";
      } break;
      case BCT_ERROR: {
        return "ERROR";
      } break;
      case BCT_UTF8_BOM: {
        return "UTF8_BOM";
      } break;
      case BCT_UTF16_BOM: {
        return "UTF16_BOM";
      } break;
    }
    return "unknown";
  }

  bool has_value(size_t token_idx)
  {
    auto type = tokens[token_idx].type;
    switch (type) {
      case BCT_NUMBER:
        [[fallthrough]];
      case BCT_IPADDR:
        [[fallthrough]];
      case BCT_IDENTIFIER:
        [[fallthrough]];
      case BCT_UNQUOTED_STRING:
        [[fallthrough]];
      case BCT_QUOTED_STRING:
        [[fallthrough]];
      case BCT_BOB:
        [[fallthrough]];
      case BCT_EOB:
        [[fallthrough]];
      case BCT_EQUALS:
        [[fallthrough]];
      case BCT_COMMA:
        [[fallthrough]];
        return true;
    }
    return false;
  }
};

ParsedBlock ScanBlock(lexer* lc)
{
  int current_level = 0;

  u32 current_line = lc->line_no;
  const char* current_file = lc->fname;

  std::vector<std::string> files;
  files.emplace_back(current_file);

  std::vector<char> content;

  std::vector<token> tokens;


  for (;;) {
    int token = LexGetToken(lc, LC_ALL);

    if (token == BCT_EOF || token == BCT_ERROR) { break; }

    u32 content_start = content.size();

    content.insert(content.end(), lc->str, lc->str + lc->str_len);

    u32 content_end = content.size();

    tokens.push_back({.content_start = content_start,
                      .content_end = content_end,
                      .type = token});

    if (token == BCT_BOB) { current_level += 1; }
    if (token == BCT_EOB) {
      current_level -= 1;
      if (current_level == 0) { break; }
    }
  }

  if (current_level != 0) {
    // error
  }

  return ParsedBlock{
      .files = std::move(files),
      .content = std::move(content),
      .tokens = std::move(tokens),
  };
}

// void StoreOidc(lexer* lc, const ResourceItem* item,
//                int, int pass, BareosResource** resources)
// {
//   auto* config_ptr = GetItemVariablePointer<OidcConfig*>(*item);
//   if (pass == 1) {
//     (void)ScanBlock(lc);
//   } else {
//     auto parsed = ScanBlock(lc);
//   }
// }


#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

void lex_err(const char* file, int line, lexer* lc, const char* msg, ...) {};
void lex_warn(const char* file, int line, lexer* lc, const char* msg, ...) {};

int main()
{
  char text[] =
      R"(
{
  a = 123
  b = "allo"
  c = "ex" "ey"
  d = "dd"
      "2d"

  long name = 1 2 short 3
}
     )";

  auto d = memfd_create("input_text", 0);
  if (d < 0) {
    std::cerr << "could not create fd: " << strerror(errno) << std::endl;
    return -1;
  }

  write(d, text, sizeof(text));

  printf("---\n%s\n---\n", text);

  char path[128];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", d);

  auto* lexer = lex_open_file(nullptr, path, lex_err, lex_warn);

  auto block = ScanBlock(lexer);

  for (size_t i = 0; i < block.tokens.size(); ++i) {
    auto token_text = block.token_text(i);
    auto token_type = block.token_type(i);
    std::cout << token_type;

    if (block.has_value(i)) { std::cout << " (" << token_text << ")"; }

    std::cout << "\n";
  }
}
