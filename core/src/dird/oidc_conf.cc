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
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "dird/auth_conf.h"

bool token_type_from_int(int type, token_type* out)
{
  switch (type) {
    case BCT_EOF: {
      *out = token_type::FileEnd;
    } break;
    case BCT_NUMBER: {
      *out = token_type::Number;
    } break;
    case BCT_IPADDR: {
      *out = token_type::IpAddr;
    } break;
    case BCT_IDENTIFIER: {
      *out = token_type::Identifier;
    } break;
    case BCT_UNQUOTED_STRING: {
      *out = token_type::String;
    } break;
    case BCT_QUOTED_STRING: {
      *out = token_type::String;
    } break;
    case BCT_BOB: {
      *out = token_type::BlockStart;
    } break;
    case BCT_EOB: {
      *out = token_type::BlockEnd;
    } break;
    case BCT_EQUALS: {
      *out = token_type::Equals;
    } break;
    case BCT_COMMA: {
      *out = token_type::Comma;
    } break;
    case BCT_EOL: {
      *out = token_type::LineEnd;
    } break;
    case BCT_ERROR: {
      *out = token_type::Error;
    } break;
    case BCT_UTF8_BOM: {
      *out = token_type::Utf8Bom;
    } break;
    case BCT_UTF16_BOM: {
      *out = token_type::Utf16Bom;
    } break;
    default: {
      return false;
    } break;
  }
  return true;
}

std::string_view token_type_to_str(token_type type)
{
  switch (type) {
    case token_type::FileEnd: {
      return "EOF";
    } break;
    case token_type::Number: {
      return "NUMBER";
    } break;
    case token_type::IpAddr: {
      return "IPADDR";
    } break;
    case token_type::Identifier: {
      return "IDENTIFIER";
    } break;
    case token_type::String: {
      return "STRING";
    } break;
    case token_type::BlockStart: {
      return "BOB";
    } break;
    case token_type::BlockEnd: {
      return "EOB";
    } break;
    case token_type::Equals: {
      return "EQUALS";
    } break;
    case token_type::Comma: {
      return "COMMA";
    } break;
    case token_type::LineEnd: {
      return "EOL";
    } break;
    case token_type::Error: {
      return "ERROR";
    } break;
    case token_type::Utf8Bom: {
      return "UTF8_BOM";
    } break;
    case token_type::Utf16Bom: {
      return "UTF16_BOM";
    } break;
  }
  return "unknown";
}


std::string ParsedBlock::text(const token& token) const
{
  switch (token.type) {
    case token_type::FileEnd: {
      return {};
    }
    case token_type::String: {
      auto val = value(token);
      std::string quoted;
      quoted.push_back('"');
      quoted.append(val);
      quoted.push_back('"');
      return quoted;
    } break;
    case token_type::LineEnd: {
      return "\n";
    }
    case token_type::Error: {
      return "< parse error >";
    }
    case token_type::Utf8Bom: {
      // TODO: insert bom
      return {};
    }
    case token_type::Utf16Bom: {
      // TODO: insert bom
      return {};
    }
    default: {
      ASSERT(has_value(token));
      return std::string{value(token)};
    } break;
  }
}

std::string_view ParsedBlock::value(const token& token) const
{
  ASSERT(has_value(token));

  auto start = token.content_start;
  auto end = token.content_end;
  auto len = end - start;

  return std::string_view{content.data() + start, len};
}

bool ParsedBlock::has_value(const token& token) const
{
  auto type = token.type;
  switch (type) {
    case token_type::Number:
      [[fallthrough]];
    case token_type::IpAddr:
      [[fallthrough]];
    case token_type::Identifier:
      [[fallthrough]];
    case token_type::String:
      [[fallthrough]];
    case token_type::BlockStart:
      [[fallthrough]];
    case token_type::BlockEnd:
      [[fallthrough]];
    case token_type::Equals:
      [[fallthrough]];
    case token_type::Comma:
      return true;
    default:
      return false;
  }
  return false;
}

ParsedBlock ScanBlock(lexer* lc)
{
  int current_level = 0;

  std::vector<char> content;

  std::vector<token> tokens;


  for (;;) {
    int token = LexGetToken(lc, LC_ALL);

    if (token == BCT_EOF || token == BCT_ERROR) { break; }

    u32 content_start = content.size();

    content.insert(content.end(), lc->str, lc->str + lc->str_len);

    u32 content_end = content.size();

    token_type type;
    if (!token_type_from_int(token, &type)) {
      // error
      break;
    }

    tokens.push_back({.content_start = content_start,
                      .content_end = content_end,
                      .type = type});

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
      .content = std::move(content),
      .tokens = std::move(tokens),
  };
}

struct token_stream {
  using iter = const token*;
  iter current;
  iter end;

  const ParsedBlock* parsed;

  token_stream(const ParsedBlock& block)
      : current{block.tokens.data()}
      , end{block.tokens.data() + block.tokens.size()}
      , parsed{&block}
  {
  }

  const token& get()
  {
    ASSERT(!done());
    return *current;
  }
  iter peek() const
  {
    ASSERT(!done());
    return current + 1;
  }

  bool advance()
  {
    ASSERT(!done());
    current = current + 1;
    return done();
  }

  bool done() const { return current == end; }
};

struct Ident {
  std::string value;

  bool operator==(const Ident& s) const { return *this == s.value; }
  bool operator==(const std::string& s) const
  {
    return Bstrcasecmp(value.c_str(), s.c_str());
  }

  bool parse_from(token_stream& stream)
  {
    auto& tk = stream.get();
    if (tk.type != token_type::Identifier) { return false; }

    stream.advance();
    value = stream.parsed->value(tk);

    return IsNameValid(value.c_str());
  }
};

bool Skip(token_stream& stream, token_type ToSkip)
{
  if (stream.get().type != ToSkip) { return false; }
  stream.advance();
  return true;
}

bool SkipEol(token_stream& stream)
{
  auto current_type = stream.get().type;
  if (current_type != token_type::LineEnd
      && current_type != token_type::FileEnd) {
    return false;
  }
  stream.advance();
  return true;
}

bool ParseStringValue(token_stream& stream, config_value<std::string>* out)
{
  auto tk = stream.get();
  switch (tk.type) {
    case token_type::Number:
    case token_type::IpAddr:
    case token_type::Identifier:
    case token_type::String: {
      auto value = stream.parsed->value(tk);
      out->value.assign(value);
      out->set = true;
      stream.advance();
      return true;
    } break;
    default: {
      return false;
    }
  }
}

bool Parse(OidcConfig& cfg)
{
  token_stream stream{cfg.block};

  while (stream.get().type == token_type::LineEnd) { stream.advance(); }

  if (!Skip(stream, token_type::BlockStart)) { return false; }

  for (;;) {
    while (stream.get().type == token_type::LineEnd) { stream.advance(); }

    if (Skip(stream, token_type::BlockEnd)) { break; }


    Ident identifier;
    if (!identifier.parse_from(stream)) { return 0; }

    if (!Skip(stream, token_type::Equals)) { return false; }

    if (identifier == "Name") {
      if (!ParseStringValue(stream, &cfg.name)) { return false; }
    } else if (identifier == "Url") {
      if (!ParseStringValue(stream, &cfg.url)) { return false; }
    } else if (identifier == "Audience") {
      if (!ParseStringValue(stream, &cfg.audience)) { return false; }
    } else if (identifier == "TokenScope") {
      if (!ParseStringValue(stream, &cfg.token_scope)) { return false; }
    } else if (identifier == "ApiScope") {
      if (!ParseStringValue(stream, &cfg.api_scope)) { return false; }
    } else if (identifier == "ScopeName") {
      if (!ParseStringValue(stream, &cfg.scope_name)) { return false; }
    } else if (identifier == "RolesName") {
      if (!ParseStringValue(stream, &cfg.roles_name)) { return false; }
    } else {
      printf("Unknown item '%s'\n", identifier.value.c_str());
      return false;
    }

    if (!SkipEol(stream)) { return false; }

    while (stream.get().type == token_type::LineEnd) { stream.advance(); }
  }

  return stream.done();
}

bool Parse(CramMd5Config& cfg)
{
  token_stream stream{cfg.block};

  while (stream.get().type == token_type::LineEnd) { stream.advance(); }

  if (!Skip(stream, token_type::BlockStart)) { return false; }

  for (;;) {
    while (stream.get().type == token_type::LineEnd) { stream.advance(); }

    if (Skip(stream, token_type::BlockEnd)) { break; }


    Ident identifier;
    if (!identifier.parse_from(stream)) { return 0; }

    if (!Skip(stream, token_type::Equals)) { return false; }

    if (identifier == "Password") {
      if (!ParseStringValue(stream, &cfg.password)) { return false; }
    } else {
      printf("Unknown item '%s'\n", identifier.value.c_str());
      return false;
    }

    if (!SkipEol(stream)) { return false; }

    while (stream.get().type == token_type::LineEnd) { stream.advance(); }
  }

  return stream.done();
}

bool Validate(const OidcConfig& cfg)
{
  if (cfg.name.required && !cfg.name.set) { return false; }
  if (cfg.url.required && !cfg.url.set) { return false; }
  if (cfg.audience.required && !cfg.audience.set) { return false; }
  if (cfg.token_scope.required && !cfg.token_scope.set) { return false; }
  if (cfg.api_scope.required && !cfg.api_scope.set) { return false; }
  if (cfg.scope_name.required && !cfg.scope_name.set) { return false; }
  if (cfg.roles_name.required && !cfg.roles_name.set) { return false; }

  return true;
}

void StoreAuth(lexer* lc, const ResourceItem* item, int, int pass)
{
  auto block = ScanBlock(lc);

  if (pass == 2) {
    AuthList* list = GetItemVariablePointer<AuthList*>(*item);

    switch (item->code) {
      case CFG_AUTH_OIDC: {
        auto& cfg = list->oidc.emplace_back();

        cfg.block = std::move(block);
        if (!Parse(cfg)) {
          scan_err(lc, "Could not parse oidc auth block");
          list->oidc.pop_back();
          return;
        }

        if (!Validate(cfg)) {
          scan_err(lc, "Could not parse correct oidc auth block");
          list->oidc.pop_back();
          return;
        }
      } break;
      case CFG_AUTH_CRAM_MD5: {
        auto& cfg = list->cram_md5.emplace_back();

        cfg.name.value = "cram md5";
        cfg.block = std::move(block);
        if (!Parse(cfg)) {
          scan_err(lc, "Could not parse legacy auth block");
          list->cram_md5.pop_back();
          return;
        }
      } break;
      default: {
        scan_err(lc, "Unknown auth block.  Cannot continue.");
      }
    }
  }
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

std::string format(const ParsedBlock& block)
{
  size_t i;
  auto peek = [&]() -> std::optional<token_type> {
    if (i + 1 >= block.tokens.size()) { return std::nullopt; }
    return block.tokens[i + 1].type;
  };


  std::string result;
  for (i = 0; i < block.tokens.size(); ++i) {
    auto& token = block.tokens[i];
    auto token_text = block.text(token);
    result += token_text;
    auto next_type = peek();
    if (token.type == token_type::BlockStart
        && next_type == token_type::BlockEnd) {
      // print nothing
    } else if (next_type == token_type::FileEnd
               || next_type == token_type::LineEnd) {
      // print nothing
    } else {
      result += " ";
    }
  }
  return result;
}
