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

bool ConfigParserStateMachine::ParseAllTokens(LEX* lex)
{
  int token;

  while ((token = LexGetToken(lex, BCT_ALL)) != BCT_EOF) {
    Dmsg3(900, "parse state=%d parser_pass_number_=%d got token=%s\n", state,
          parser_pass_number_, lex_tok_to_str(token));
    switch (state) {
      case ParseState::kInit: {
        if (!ParserInitResource(lex, token)) { return false; }
      } break;
      case ParseState::kResource: {
        if (!ScanResource(lex, token)) {
          // delete the inited resource
          my_config_->FreeResourceCb_(currently_parsed_resource_.resource_,
                                      currently_parsed_resource_.rcode_);
          currently_parsed_resource_.resource_ = nullptr;
          return false;
        }
      } break;
      default: {
        scan_err1(lex, T_("Unknown parser state %d\n"), state);
        return false;
      } break;
    }
  }
  return true;
}

void ConfigParserStateMachine::FreeUnusedMemoryFromPass2()
{
  if (parser_pass_number_ == 2) {
    // free all resource memory from second pass
    if (currently_parsed_resource_.resource_) {
      if (currently_parsed_resource_.resource_->resource_name_) {
        free(currently_parsed_resource_.resource_->resource_name_);
      }
      delete currently_parsed_resource_.resource_;
    }
    currently_parsed_resource_.rcode_ = 0;
    currently_parsed_resource_.items_ = nullptr;
    currently_parsed_resource_.resource_ = nullptr;
  }
}

bool ConfigParserStateMachine::ScanResource(LEX* lex, int token)
{
  switch (token) {
    case BCT_BOB:
      config_level_++;
      return true;
    case BCT_IDENTIFIER: {
      if (config_level_ != 1) {
        scan_err1(lex, T_("not in resource definition: %s"), lex->str);
        return false;
      }

      int resource_item_index = my_config_->GetResourceItemIndex(
          currently_parsed_resource_.items_, lex->str);

      if (resource_item_index >= 0) {
        ResourceItem* item = nullptr;
        item = &currently_parsed_resource_.items_[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lex, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            scan_err1(lex, T_("expected an equals, got: %s"), lex->str);
            return false;
          }
        }

        if (parser_pass_number_ == 1 && item->flags & CFG_ITEM_DEPRECATED) {
          my_config_->AddWarning(std::string("using deprecated keyword ")
                                 + item->name + " on line "
                                 + std::to_string(lex->line_no) + " of file "
                                 + lex->fname);
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_->StoreResource(
                currently_parsed_resource_.resource_, item->type, lex, item,
                resource_item_index, parser_pass_number_)) {
          if (my_config_->store_res_) {
            my_config_->store_res_(currently_parsed_resource_.resource_, lex,
                                   item, resource_item_index,
                                   parser_pass_number_,
                                   my_config_->config_resources_container_
                                       ->configuration_resources_.get());
          }
        }
      } else {
        Dmsg2(900, "config_level_=%d id=%s\n", config_level_, lex->str);
        Dmsg1(900, "Keyword = %s\n", lex->str);
        scan_err1(lex,
                  T_("Keyword \"%s\" not permitted in this resource.\n"
                     "Perhaps you left the trailing brace off of the "
                     "previous resource."),
                  lex->str);
        return false;
      }
      return true;
    }
    case BCT_EOB:
      config_level_--;
      state = ParseState::kInit;
      Dmsg0(900, "BCT_EOB => define new resource\n");
      if (!currently_parsed_resource_.resource_->resource_name_) {
        scan_err0(lex, T_("Name not specified for resource"));
        return false;
      }
      /* save resource */
      if (!my_config_->SaveResourceCb_(currently_parsed_resource_.resource_,
                                       currently_parsed_resource_.rcode_,
                                       currently_parsed_resource_.items_,
                                       parser_pass_number_)) {
        scan_err0(lex, T_("SaveResource failed"));
        return false;
      }

      FreeUnusedMemoryFromPass2();
      return true;

    case BCT_EOL:
      return true;

    default:
      scan_err2(lex, T_("unexpected token %d %s in resource definition"), token,
                lex_tok_to_str(token));
      return true;
  }
  return true;
}

bool ConfigParserStateMachine::ParserInitResource(LEX* lex, int token)
{
  const char* resource_identifier = lex->str;

  switch (token) {
    case BCT_EOL:
    case BCT_UTF8_BOM:
      return true;
    case BCT_UTF16_BOM:
      scan_err0(lex, T_("Currently we cannot handle UTF-16 source files. "
                        "Please convert the conf file to UTF-8\n"));
      return false;
    default:
      if (token != BCT_IDENTIFIER) {
        scan_err1(lex, T_("Expected a Resource name identifier, got: %s"),
                  resource_identifier);
        return false;
      }
      break;
  }

  ResourceTable* resource_table;
  resource_table = my_config_->GetResourceTable(resource_identifier);

  bool init_done = false;

  if (resource_table && resource_table->items) {
    currently_parsed_resource_.rcode_ = resource_table->rcode;
    currently_parsed_resource_.items_ = resource_table->items;

    BareosResource* new_res = resource_table->make();
    ASSERT(new_res);
    my_config_->InitResource(currently_parsed_resource_.rcode_,
                             currently_parsed_resource_.items_,
                             parser_pass_number_, new_res);

    currently_parsed_resource_.resource_ = new_res;

    currently_parsed_resource_.resource_->rcode_str_
        = my_config_->GetQualifiedResourceNameTypeConverter()
              ->ResourceTypeToString(resource_table->rcode);

    state = ParseState::kResource;

    init_done = true;
  }

  if (!init_done) {
    scan_err1(lex, T_("expected resource identifier, got: %s"),
              resource_identifier);
    return false;
  }
  return true;
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

auto ConfigParserStateMachine::GetParseError(LEX* lex) const -> ParserError
{
  // in this order
  if (state != ParseState::kInit) {
    return ParserError::kResourceIncomplete;
  } else if (lex->error_counter > 0) {
    return ParserError::kParserError;
  } else {
    return ParserError::kNoError;
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
