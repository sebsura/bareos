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

parse_result ConfigParserStateMachine::ParseResource(BareosResource* res,
                                                     ResourceItem* items,
                                                     LEX* lex,
                                                     size_t pass)
{
  int open_blocks = 0;
  for (;;) {
    int token = LexGetToken(lex, BCT_ALL);
    switch (token) {
      case BCT_BOB: {
        open_blocks += 1;
      } break;
      case BCT_EOB: {
        open_blocks -= 1;

        if (open_blocks == 0) {
          return {};
        } else if (open_blocks < 0) {
          return parse_result("unexpected end of block");
        }
      } break;
      case BCT_IDENTIFIER: {
        int resource_item_index
            = my_config_->GetResourceItemIndex(items, lex->str);

        if (resource_item_index < 0) {
          Dmsg2(900, "config_level_=%d id=%s\n", open_blocks, lex->str);
          Dmsg1(900, "Keyword = %s\n", lex->str);

          PoolMem errmsg;
          Mmsg(errmsg,
               "Keyword \"%s\" not permitted in this resource.\n"
               "Perhaps you left the trailing brace off of the "
               "previous resource.",
               lex->str);
          return parse_result(errmsg.c_str());
        }

        ResourceItem* item = &items[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lex, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            PoolMem errmsg;
            Mmsg(errmsg, "expected an equals, got: %s", lex->str);
            return parse_result(errmsg.c_str());
          }
        }

        if (pass == 1 && item->flags & CFG_ITEM_DEPRECATED) {
          my_config_->AddWarning(std::string("using deprecated keyword ")
                                 + item->name + " on line "
                                 + std::to_string(lex->line_no) + " of file "
                                 + lex->fname);
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_->StoreResource(res, item->type, lex, item,
                                       resource_item_index, pass)) {
          if (my_config_->store_res_) {
            my_config_->store_res_(res, lex, item, resource_item_index, pass,
                                   my_config_->config_resources_container_
                                       ->configuration_resources_.get());
          }
        }
      } break;
      case BCT_EOL: {
        // continue on
      } break;
      case BCT_EOF: {
        return parse_result("End of conf file reached with unclosed resource.");
      } break;

      default: {
        PoolMem errmsg;
        Mmsg(errmsg, "unexpected token %d %s in resource definition", token,
             lex_tok_to_str(token));
        return parse_result(errmsg.c_str());
      } break;
    }
  }
}
