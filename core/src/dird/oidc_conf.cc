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
  u32 col_start;
  u32 col_end;

  u32 content_start;
  u32 content_end;

  int type;
};

struct token_line {
  // we are on line 'line' before token 'before_token'
  u32 before_token;

  u32 file;
  u32 line;
};

using LineMap = std::vector<token_line>;

struct ParsedBlock {
  LineMap line_map;
  std::vector<std::string> files;
  std::vector<char> content;
  std::vector<token> tokens;


  size_t line_start(size_t token_idx)
  {
    for (size_t idx = 1; idx < line_map.size(); ++idx) {
      if (line_map[idx].before_token > token_idx) {
        return line_map[idx - 1].line;
      }
    }

    return 0;
  }
  size_t col_start(size_t token_idx) { return tokens[token_idx].col_start; }
  size_t line_end(size_t token_idx)
  {
    for (size_t idx = 1; idx < line_map.size(); ++idx) {
      if (line_map[idx].before_token > token_idx) { return line_map[idx].line; }
    }

    return 0;
  }
  size_t col_end(size_t token_idx) { return tokens[token_idx].col_end; }

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
  LineMap line_map;
  line_map.push_back({.before_token = 0, .file = 0, .line = current_line});

  std::vector<char> content;

  std::vector<token> tokens;


  for (;;) {
    if (current_file != lc->fname) {
      files.emplace_back(lc->fname);
      current_file = lc->fname;
      current_line = lc->line_no;
      line_map.push_back({
          .before_token = tokens.size(),
          .file = files.size() - 1,
          .line = current_line,
      });
    } else if (current_line != lc->line_no) {
      current_line = lc->line_no;
      line_map.push_back({
          .before_token = tokens.size(),
          .file = files.size() - 1,
          .line = current_line,
      });
    }

    u32 col_start = lc->col_no;
    int token = LexGetToken(lc, LC_ALL);
    u32 col_end = lc->col_no;

    if (token == BCT_EOF || token == BCT_ERROR) { break; }

    u32 content_start = content.size();

    content.insert(content.end(), lc->str, lc->str + lc->str_len);

    u32 content_end = content.size();

    tokens.push_back({.col_start = col_start,
                      .col_end = col_end,
                      .content_start = content_start,
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
      .line_map = std::move(line_map),
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

std::string_view read_text(std::string_view input,
                           u32 ls, u32 le, u32 cs, u32 ce)
{
}

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
    auto line_start = block.line_start(i);
    auto line_end = block.line_end(i);
    auto col_start = block.col_start(i);
    auto col_end = block.col_end(i);
    auto token_text = block.token_text(i);
    auto token_type = block.token_type(i);
    std::cout << line_start << ":" << col_start << "\t -> \t" << line_end << ":"
              << col_end << "\t" << token_type;

    if (block.has_value(i)) {
      std::cout << " (" << token_text << " vs" << read_text(text, line_start, line_end, col_start, col_end) << ")";
    }

    std::cout << "\n";
  }
}
