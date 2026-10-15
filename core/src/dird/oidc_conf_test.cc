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

#include "dird/auth_conf.h"

#include <iostream>
#include <sys/mman.h>
#include <unistd.h>


void lex_err(const char* file, int line, lexer* lc, const char* msg, ...) {};
void lex_warn(const char* file, int line, lexer* lc, const char* msg, ...) {};

ParsedBlock scan_from_text(std::string_view text)
{
  auto d = memfd_create("input_text", 0);
  if (d < 0) {
    std::cerr << "could not create fd: " << strerror(errno) << std::endl;
    ASSERT(0);
  }

  write(d, text.data(), std::size(text));

  char path[128];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", d);
  auto* lexer = lex_open_file(nullptr, path, lex_err, lex_warn);

  auto block = ScanBlock(lexer);

  close(d);

  return block;
}

void PrintOidc(const OidcConfig& c)
{
  std::printf(
      "name        = %s\n"
      "url         = %s\n"
      "audience    = %s\n"
      "token_scope = %s\n"
      "api_scope   = %s\n"
      "scope_name  = %s\n"
      "roles_name  = %s\n",
      c.name.value.c_str(), c.url.value.c_str(), c.audience.value.c_str(),
      c.token_scope.value.c_str(), c.api_scope.value.c_str(),
      c.scope_name.value.c_str(), c.roles_name.value.c_str());
}

int main()
{
  {
    char text[] =
        R"(
{
  a = 123
  a = test
  b = "allo"
  c = "ex" "ey"
  d = "dd"
      "2d"

  long name = 1 2 short 3
{




}
}
       )";

    printf("---\n%s\n---\n", text);

    auto block = scan_from_text(text);

    for (auto& token : block.tokens) {
      auto token_type = token.type;
      std::cout << token_type_to_str(token_type) << "["
                << static_cast<int>(token_type) << "]";

      if (block.has_value(token)) {
        auto token_text = block.value(token);
        std::cout << "\t(" << token_text << ")";
      }

      std::cout << "\n";
    }

    auto formatted = format(block);

    printf("---\n%s\n---\n", formatted.c_str());

    auto block2 = scan_from_text(formatted);

    printf("---\n%s\n---\n", format(block2).c_str());

    bool content_same = block.content == block2.content;
    if (!content_same) { printf("diff content\n"); }
    bool amount_same = block.tokens.size() == block2.tokens.size();
    if (!amount_same) { printf("diff token count\n"); }
    bool token_same = true;
    for (size_t i = 0; i < std::min(block.tokens.size(), block2.tokens.size());
         ++i) {
      if (block.tokens[i] != block2.tokens[i]) {
        printf("token %zu different\n", i);
      }
    }
  }
  {
    char cfg_txt[] = R"(
{
  Name        = "test"
  url         = http://my-site
  Token Scope = console
}
                      )";

    ParsedBlock block = scan_from_text(cfg_txt);

    OidcConfig cfg;
    cfg.block = std::move(block);

    if (!Parse(cfg)) {
      printf("bad\n");
    } else {
      PrintOidc(cfg);
    }
  }
}
