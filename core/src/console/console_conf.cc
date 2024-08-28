/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2009 Free Software Foundation Europe e.V.
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
// Kern Sibbald, January MM, September MM
#define NEED_JANSSON_NAMESPACE 1
#include "include/bareos.h"
#include "console/console_globals.h"
#include "console/console_conf.h"
#include "lib/alist.h"
#include "lib/resource_item.h"
#include "lib/tls_resource_items.h"
#include "lib/output_formatter.h"
#include "lib/output_formatter_resource.h"
#include "lib/version.h"

#include <cassert>

namespace console {

static void FreeResource(BareosResource* sres, int type);
static void DumpResource(int type,
                         BareosResource* reshdr,
                         bool sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose);

/* clang-format off */

static ResourceItem cons_items[] = {
  { "NAME", CFG_TYPE_NAME, ITEM(ConsoleResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, "The name of this resource." },
  { "Description", CFG_TYPE_STR, ITEM(ConsoleResource, description_), 0, 0, NULL, NULL, NULL },
  { "RcFile", CFG_TYPE_DIR, ITEM(ConsoleResource, rc_file), 0, 0, NULL, NULL, NULL },
  { "HistoryFile", CFG_TYPE_DIR, ITEM(ConsoleResource, history_file), 0, 0, NULL, NULL, NULL },
  { "HistoryLength", CFG_TYPE_PINT32, ITEM(ConsoleResource, history_length), 0, CFG_ITEM_DEFAULT, "100", NULL, NULL },
  { "Password", CFG_TYPE_MD5PASSWORD, ITEM(ConsoleResource, password_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "Director", CFG_TYPE_STR, ITEM(ConsoleResource, director), 0, 0, NULL, NULL, NULL },
  { "HeartbeatInterval", CFG_TYPE_TIME, ITEM(ConsoleResource, heartbeat_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL },
  TLS_COMMON_CONFIG(ConsoleResource),
  TLS_CERT_CONFIG(ConsoleResource),
  {}
};

static ResourceItem dir_items[] = {
  { "Name", CFG_TYPE_NAME, ITEM(DirectorResource, resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "Description", CFG_TYPE_STR, ITEM(DirectorResource, description_), 0, 0, NULL, NULL, NULL },
  { "DirPort", CFG_TYPE_PINT32, ITEM(DirectorResource, DIRport), 0, CFG_ITEM_DEFAULT, DIR_DEFAULT_PORT, NULL, NULL },
  { "Address", CFG_TYPE_STR, ITEM(DirectorResource, address), 0, 0, NULL, NULL, NULL },
  { "Password", CFG_TYPE_MD5PASSWORD, ITEM(DirectorResource, password_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL },
  { "HeartbeatInterval", CFG_TYPE_TIME, ITEM(DirectorResource, heartbeat_interval), 0, CFG_ITEM_DEFAULT, "0", NULL, NULL },
  TLS_COMMON_CONFIG(DirectorResource),
  TLS_CERT_CONFIG(DirectorResource),
  {}
};

static ResourceTable resources[] = {
  { "Console", "Consoles", cons_items, R_CONSOLE, false, ResourceFactory<ConsoleResource> },
  { "Director", "Directors", dir_items, R_DIRECTOR, false, ResourceFactory<DirectorResource> },
  {}
};

/* clang-format on */


static void DumpResource(int type,
                         BareosResource* res,
                         bool sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose)
{
  PoolMem buf;
  bool recurse = true;
  OutputFormatter output_formatter
      = OutputFormatter(sendit, sock, nullptr, nullptr);
  OutputFormatterResource output_formatter_resource
      = OutputFormatterResource(&output_formatter);

  if (!res) {
    sendit(sock, T_("Warning: no \"%s\" resource (%d) defined.\n"),
           my_config->ResToStr(type), type);
    return;
  }
  if (type < 0) {  // no recursion
    type = -type;
    recurse = false;
  }

  switch (type) {
    default:
      res->PrintConfig(output_formatter_resource, *my_config,
                       hide_sensitive_data, verbose);
      break;
  }

  if (recurse && res->next_) {
    DumpResource(type, res->next_, sendit, sock, hide_sensitive_data, verbose);
  }
}

static void FreeResource(BareosResource* res, int type)
{
  if (!res) return;

  BareosResource* next_resource = (BareosResource*)res->next_;

  if (res->resource_name_) {
    free(res->resource_name_);
    res->resource_name_ = nullptr;
  }
  if (res->description_) {
    free(res->description_);
    res->description_ = nullptr;
  }

  switch (type) {
    case R_CONSOLE: {
      ConsoleResource* p = dynamic_cast<ConsoleResource*>(res);
      assert(p);
      if (p->rc_file) { free(p->rc_file); }
      if (p->history_file) { free(p->history_file); }
      if (p->password_.value) { free(p->password_.value); }
      if (p->director) { free(p->director); }
      delete p;
      break;
    }
    case R_DIRECTOR: {
      DirectorResource* p = dynamic_cast<DirectorResource*>(res);
      assert(p);
      if (p->address) { free(p->address); }
      if (p->password_.value) { free(p->password_.value); }
      delete p;
      break;
    }
    default:
      printf(T_("Unknown resource type %d\n"), type);
      break;
  }
  if (next_resource) { FreeResource(next_resource, type); }
}

static void ConfigBeforeCallback(ConfigurationParser& t_config)
{
  std::map<int, std::string> map{{R_DIRECTOR, "R_DIRECTOR"},
                                 {R_CONSOLE, "R_CONSOLE"}};
  t_config.InitializeQualifiedResourceNameTypeConverter(map);
}

static void ConfigReadyCallback(ConfigurationParser&) {}

ConfigurationParser* InitConsConfig(const char* configfile, int exit_code)
{
  ConfigurationParser* config = new ConfigurationParser(
      configfile, nullptr, nullptr, nullptr, nullptr, nullptr, exit_code, R_NUM,
      resources, default_config_filename.c_str(), "bconsole.d",
      ConfigBeforeCallback, ConfigReadyCallback, DumpResource, FreeResource);
  if (config) { config->r_own_ = R_CONSOLE; }
  return config;
}

#ifdef HAVE_JANSSON
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  json_t* json = json_object();
  json_object_set_new(json, "format-version", json_integer(2));
  json_object_set_new(json, "component", json_string("bconsole"));
  json_object_set_new(json, "version", json_string(kBareosVersionStrings.Full));

  json_t* json_resource_object = json_object();
  json_object_set_new(json, "resource", json_resource_object);
  json_t* bconsole = json_object();
  json_object_set_new(json_resource_object, "bconsole", bconsole);

  ResourceTable* resource_definition = my_config->resource_definitions_;
  for (; resource_definition->name; ++resource_definition) {
    json_object_set_new(bconsole, resource_definition->name,
                        json_items(resource_definition->items));
  }

  char* const json_str = json_dumps(json, JSON_INDENT(2));
  PmStrcat(buffer, json_str);
  free(json_str);
  json_decref(json);

  return true;
}
#else
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  PmStrcat(buffer, "{ \"success\": false, \"message\": \"not available\" }");
  return false;
}
#endif
} /* namespace console */
