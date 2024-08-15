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

#include "lib/parse_conf_state_machine.h"
#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "lib/lex.h"
#include "lib/qualified_resource_name_type_converter.h"

auto ConfigParserStateMachine::NextResourceIdentifier(LEX* lex)
    -> std::variant<done, ident, unexpected_token>
{
  for (;;) {
    auto token = LexGetToken(lex, BCT_ALL);
    switch (token) {
      case BCT_IDENTIFIER: {
        return ident{lex->str};
      } break;
      case BCT_EOL: {
        // continue on
      } break;
      case BCT_EOF: {
        return done{};
      } break;
      default: {
        return unexpected_token{token};
      } break;
    }
  }
}

auto ConfigParserStateMachine::ParseResource(BareosResource* res,
                                             ResourceItem* items,
                                             LEX* lex) -> ParserError
{
  int level = 0;
  for (;;) {
    int token = LexGetToken(lex, BCT_ALL);
    switch (token) {
      case BCT_BOB: {
        level += 1;
      } break;
      case BCT_EOB: {
        level -= 1;

        if (level == 0) {
          return ParserError::kNoError;
        } else if (level < 0) {
          scan_err0(lex, T_("unexpected end of block"));
          return ParserError::kParserError;
        }
      } break;
      case BCT_IDENTIFIER: {
        int resource_item_index
            = my_config_->GetResourceItemIndex(items, lex->str);

        if (resource_item_index < 0) {
          Dmsg2(900, "config_level_=%d id=%s\n", level, lex->str);
          Dmsg1(900, "Keyword = %s\n", lex->str);
          scan_err1(lex,
                    T_("Keyword \"%s\" not permitted in this resource.\n"
                       "Perhaps you left the trailing brace off of the "
                       "previous resource."),
                    lex->str);
          return ParserError::kParserError;
        }

        ResourceItem* item = &items[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lex, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            scan_err1(lex, T_("expected an equals, got: %s"), lex->str);
            return ParserError::kParserError;
          }
        }

        if (parser_pass_number_ == 1 && item->flags & CFG_ITEM_DEPRECATED) {
          my_config_->AddWarning(std::string("using deprecated keyword ")
                                 + item->name + " on line "
                                 + std::to_string(lex->line_no) + " of file "
                                 + lex->fname);
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_->StoreResource(res, item->type, lex, item,
                                       resource_item_index,
                                       parser_pass_number_)) {
          if (my_config_->store_res_) {
            my_config_->store_res_(res, lex, item, resource_item_index,
                                   parser_pass_number_,
                                   my_config_->config_resources_container_
                                       ->configuration_resources_.get());
          }
        }
      } break;
      case BCT_EOL: {
        // continue on
      } break;
      case BCT_EOF: {
        return ParserError::kResourceIncomplete;
      } break;

      default:
        scan_err2(lex, T_("unexpected token %d %s in resource definition"),
                  token, lex_tok_to_str(token));
        return ParserError::kParserError;
    }
  }
}

void ConfigParserStateMachine::DumpResourcesAfterSecondPass()
{
  if (debug_level >= 900 && parser_pass_number_ == 2) {
    for (int i = 0; i <= my_config_->r_num_ - 1; i++) {
      my_config_->DumpResourceCb_(
          i,
          my_config_->config_resources_container_->configuration_resources_[i],
          PrintMessage, nullptr, false, false);
    }
  }
}

lex_ptr LexFile(const char* file,
                void* ctx,
                int err_type,
                LEX_ERROR_HANDLER* err,
                LEX_WARNING_HANDLER* warn)
{
  lex_ptr p{lex_open_file(nullptr, file, err, warn)};

  if (!p) { return p; }

  LexSetErrorHandlerErrorType(p.get(), err_type);
  p->error_counter = 0;
  p->caller_ctx = ctx;

  return p;
}
