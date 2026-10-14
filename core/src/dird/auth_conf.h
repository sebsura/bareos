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

#ifndef BAREOS_DIRD_AUTH_CONF_H_
#define BAREOS_DIRD_AUTH_CONF_H_

#include "dird/dird_conf.h"

using u32 = uint32_t;

enum class token_type
{
  FileEnd,
  Number,
  IpAddr,
  Identifier,
  String,
  BlockStart,
  BlockEnd,
  Equals,
  Comma,
  LineEnd,
  Error,
  Utf8Bom,
  Utf16Bom,
};

std::string_view token_type_to_str(token_type type);

struct token {
  u32 content_start;
  u32 content_end;

  token_type type;

  auto operator<=>(const token&) const = default;
};

struct ParsedBlock {
  std::vector<char> content;
  std::vector<token> tokens;

  std::string text(const token& token) const;
  std::string_view value(const token& token) const;
  bool has_value(const token& token) const;

  size_t index(const token& token) const { return &token - tokens.data(); }
};

ParsedBlock ScanBlock(lexer* lc);
std::string format(const ParsedBlock& block);

template <typename T> struct config_value {
  T value;

  bool set{false};
  bool required{false};
};

struct OidcConfig {
  config_value<std::string> name;
  config_value<std::string> url;
  config_value<std::string> token_scope;
  config_value<std::string> api_scope;

  config_value<std::string> scope_name;
  config_value<std::string> roles_name;

  ParsedBlock block;
};

struct CramMd5Config {
  config_value<std::string> password;

  ParsedBlock block;
};


bool Parse(OidcConfig& cfg);
bool Parse(CramMd5Config& cfg);

void StoreAuthenticator(lexer* lc,
                        const ResourceItem* item,
                        int,
                        int pass,
                        BareosResource** resources);

#endif  // BAREOS_DIRD_AUTH_CONF_H_
