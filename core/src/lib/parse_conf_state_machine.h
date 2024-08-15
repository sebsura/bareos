/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_LIB_PARSE_CONF_STATE_MACHINE_H_
#define BAREOS_LIB_PARSE_CONF_STATE_MACHINE_H_

#include <memory>
#include <variant>
#include "lib/lex.h"

struct lex_closer {
  void operator()(LEX* l) const
  {
    auto* ptr = l;

    while (ptr) { ptr = LexCloseFile(ptr); }
  }
};

using lex_ptr = std::unique_ptr<LEX, lex_closer>;

lex_ptr LexFile(const char* file,
                void* ctx,
                int err_type,
                LEX_ERROR_HANDLER* err,
                LEX_WARNING_HANDLER* warn);

class ConfigurationParser;
class BareosResource;
struct ResourceTable;
struct ResourceItem;

struct ident {
  std::string name;
};

struct done {};

struct unexpected_token {
  int value;
};

class ConfigParserStateMachine {
 public:
  ConfigParserStateMachine(ConfigurationParser* my_config, size_t pass)
      : parser_pass_number_{(int)pass}, my_config_{my_config}
  {
  }
  ConfigParserStateMachine(ConfigParserStateMachine& other) = delete;
  ConfigParserStateMachine(ConfigParserStateMachine&& other) = delete;
  ConfigParserStateMachine& operator=(ConfigParserStateMachine& rhs) = delete;
  ConfigParserStateMachine& operator=(ConfigParserStateMachine&& rhs) = delete;

  enum class ParserError
  {
    kNoError,
    kResourceIncomplete,
    kParserError
  };

  void DumpResourcesAfterSecondPass();

  std::variant<done, ident, unexpected_token> NextResourceIdentifier(LEX* lex);

  ParserError ParseResource(BareosResource* res, ResourceItem* items, LEX* lex);

 private:
  int parser_pass_number_ = 0;
  ConfigurationParser* my_config_;
};

#endif  // BAREOS_LIB_PARSE_CONF_STATE_MACHINE_H_
