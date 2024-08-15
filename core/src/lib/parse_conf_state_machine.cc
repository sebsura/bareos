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

ConfigParserStateMachine::ConfigParserStateMachine(
    const char* config_file_name,
    void* caller_ctx,
    LEX_ERROR_HANDLER* scan_error,
    LEX_WARNING_HANDLER* scan_warning,
    ConfigurationParser& my_config)
    : config_file_name_(config_file_name)
    , caller_ctx_(caller_ctx)
    , scan_error_(scan_error)
    , scan_warning_(scan_warning)
    , my_config_(my_config)
{
  return;
};

bool ConfigParserStateMachine::ParseAllTokens()
{
  int token;

  while ((token = LexGetToken(lexical_parser_.get(), BCT_ALL)) != BCT_EOF) {
    Dmsg3(900, "parse state=%d parser_pass_number_=%d got token=%s\n", state,
          parser_pass_number_, lex_tok_to_str(token));
    switch (state) {
      case ParseState::kInit: {
        if (!ParserInitResource(token)) { return false; }
      } break;
      case ParseState::kResource: {
        if (!ScanResource(token)) {
          // delete the inited resource
          my_config_.FreeResourceCb_(currently_parsed_resource_.resource_,
                                     currently_parsed_resource_.rcode_);
          currently_parsed_resource_.resource_ = nullptr;
          return false;
        }
      } break;
      default: {
        scan_err1(lexical_parser_.get(), T_("Unknown parser state %d\n"),
                  state);
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

bool ConfigParserStateMachine::ScanResource(int token)
{
  switch (token) {
    case BCT_BOB:
      config_level_++;
      return true;
    case BCT_IDENTIFIER: {
      if (config_level_ != 1) {
        scan_err1(lexical_parser_.get(), T_("not in resource definition: %s"),
                  lexical_parser_->str);
        return false;
      }

      int resource_item_index = my_config_.GetResourceItemIndex(
          currently_parsed_resource_.items_, lexical_parser_->str);

      if (resource_item_index >= 0) {
        ResourceItem* item = nullptr;
        item = &currently_parsed_resource_.items_[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lexical_parser_.get(), BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            scan_err1(lexical_parser_.get(), T_("expected an equals, got: %s"),
                      lexical_parser_->str);
            return false;
          }
        }

        if (parser_pass_number_ == 1 && item->flags & CFG_ITEM_DEPRECATED) {
          my_config_.AddWarning(std::string("using deprecated keyword ")
                                + item->name + " on line "
                                + std::to_string(lexical_parser_->line_no)
                                + " of file " + lexical_parser_->fname);
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_.StoreResource(currently_parsed_resource_.resource_,
                                      item->type, lexical_parser_.get(), item,
                                      resource_item_index,
                                      parser_pass_number_)) {
          if (my_config_.store_res_) {
            my_config_.store_res_(currently_parsed_resource_.resource_,
                                  lexical_parser_.get(), item,
                                  resource_item_index, parser_pass_number_,
                                  my_config_.config_resources_container_
                                      ->configuration_resources_.get());
          }
        }
      } else {
        Dmsg2(900, "config_level_=%d id=%s\n", config_level_,
              lexical_parser_->str);
        Dmsg1(900, "Keyword = %s\n", lexical_parser_->str);
        scan_err1(lexical_parser_.get(),
                  T_("Keyword \"%s\" not permitted in this resource.\n"
                     "Perhaps you left the trailing brace off of the "
                     "previous resource."),
                  lexical_parser_->str);
        return false;
      }
      return true;
    }
    case BCT_EOB:
      config_level_--;
      state = ParseState::kInit;
      Dmsg0(900, "BCT_EOB => define new resource\n");
      if (!currently_parsed_resource_.resource_->resource_name_) {
        scan_err0(lexical_parser_.get(), T_("Name not specified for resource"));
        return false;
      }
      /* save resource */
      if (!my_config_.SaveResourceCb_(currently_parsed_resource_.resource_,
                                      currently_parsed_resource_.rcode_,
                                      currently_parsed_resource_.items_,
                                      parser_pass_number_)) {
        scan_err0(lexical_parser_.get(), T_("SaveResource failed"));
        return false;
      }

      FreeUnusedMemoryFromPass2();
      return true;

    case BCT_EOL:
      return true;

    default:
      scan_err2(lexical_parser_.get(),
                T_("unexpected token %d %s in resource definition"), token,
                lex_tok_to_str(token));
      return true;
  }
  return true;
}

bool ConfigParserStateMachine::ParserInitResource(int token)
{
  const char* resource_identifier = lexical_parser_->str;

  switch (token) {
    case BCT_EOL:
    case BCT_UTF8_BOM:
      return true;
    case BCT_UTF16_BOM:
      scan_err0(lexical_parser_.get(),
                T_("Currently we cannot handle UTF-16 source files. "
                   "Please convert the conf file to UTF-8\n"));
      return false;
    default:
      if (token != BCT_IDENTIFIER) {
        scan_err1(lexical_parser_.get(),
                  T_("Expected a Resource name identifier, got: %s"),
                  resource_identifier);
        return false;
      }
      break;
  }

  ResourceTable* resource_table;
  resource_table = my_config_.GetResourceTable(resource_identifier);

  bool init_done = false;

  if (resource_table && resource_table->items) {
    currently_parsed_resource_.rcode_ = resource_table->rcode;
    currently_parsed_resource_.items_ = resource_table->items;

    BareosResource* new_res = resource_table->make();
    ASSERT(new_res);
    my_config_.InitResource(currently_parsed_resource_.rcode_,
                            currently_parsed_resource_.items_,
                            parser_pass_number_, new_res);

    currently_parsed_resource_.resource_ = new_res;

    currently_parsed_resource_.resource_->rcode_str_
        = my_config_.GetQualifiedResourceNameTypeConverter()
              ->ResourceTypeToString(resource_table->rcode);

    state = ParseState::kResource;

    init_done = true;
  }

  if (!init_done) {
    scan_err1(lexical_parser_.get(),
              T_("expected resource identifier, got: %s"), resource_identifier);
    return false;
  }
  return true;
}

bool ConfigParserStateMachine::InitParserPass()
{
  parser_pass_number_++;
  ASSERT(parser_pass_number_ < 3);

  Dmsg1(900, "ParseConfig parser_pass_number_ %d\n", parser_pass_number_);

  lexical_parser_ = LexFile(config_file_name_.c_str(), caller_ctx_,
                            my_config_.err_type_, scan_error_, scan_warning_);
  if (!lexical_parser_) {
    my_config_.lex_error(config_file_name_.c_str(), scan_error_, scan_warning_);
    return false;
  }

  return true;
}

void ConfigParserStateMachine::DumpResourcesAfterSecondPass()
{
  if (debug_level >= 900 && parser_pass_number_ == 2) {
    for (int i = 0; i <= my_config_.r_num_ - 1; i++) {
      my_config_.DumpResourceCb_(
          i,
          my_config_.config_resources_container_->configuration_resources_[i],
          PrintMessage, nullptr, false, false);
    }
  }
}

ConfigParserStateMachine::ParserError ConfigParserStateMachine::GetParseError()
    const
{
  // in this order
  if (state != ParseState::kInit) {
    return ParserError::kResourceIncomplete;
  } else if (lexical_parser_->error_counter > 0) {
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
