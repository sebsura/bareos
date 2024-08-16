/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2004-2011 Free Software Foundation Europe e.V.
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
/*
 * Main configuration file parser for Bareos Tray Monitor.
 * Adapted from dird_conf.c
 *
 * Note, the configuration file parser consists of three parts
 *
 * 1. The generic lexical scanner in lib/lex.c and lib/lex.h
 *
 * 2. The generic config  scanner in lib/parse_config.c and
 *    lib/parse_config.h. These files contain the parser code,
 *    some utility routines, and the common store routines
 *    (name, int, string).
 *
 * 3. The daemon specific file, which contains the Resource
 *    definitions as well as any specific store routines
 *    for the resource records.
 *
 * Nicolas Boichat, August MMIV
 */

#include "include/bareos.h"
#define NEED_JANSSON_NAMESPACE 1
#include "lib/output_formatter.h"
#include "tray_conf.h"

#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "lib/tls_resource_items.h"
#include "lib/output_formatter.h"
#include "lib/output_formatter_resource.h"
#include "lib/version.h"

#include <cassert>

static const std::string default_config_filename("tray-monitor.conf");

static void FreeResource(BareosResource* sres, int type);
static void DumpResource(int type,
                         BareosResource* reshdr,
                         bool sendit(void* sock, const char* fmt, ...),
                         void* sock,
                         bool hide_sensitive_data,
                         bool verbose);

/* clang-format off */

/*
 * Monitor Resource
 *
 * name handler value code flags default_value
 */
static ResourceItem mon_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(MonitorResource,resource_name_), 0, CFG_ITEM_REQUIRED, 0, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(MonitorResource,description_), 0, 0, 0, NULL, NULL},
  {"Password", CFG_TYPE_MD5PASSWORD, ITEM(MonitorResource,password), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"RefreshInterval", CFG_TYPE_TIME, ITEM(MonitorResource,RefreshInterval), 0, CFG_ITEM_DEFAULT, "60", NULL, NULL},
  {"FdConnectTimeout", CFG_TYPE_TIME, ITEM(MonitorResource,FDConnectTimeout), 0, CFG_ITEM_DEFAULT, "10", NULL, NULL},
  {"SdConnectTimeout", CFG_TYPE_TIME, ITEM(MonitorResource,SDConnectTimeout), 0, CFG_ITEM_DEFAULT, "10", NULL, NULL},
  {"DirConnectTimeout", CFG_TYPE_TIME, ITEM(MonitorResource,DIRConnectTimeout), 0, CFG_ITEM_DEFAULT, "10", NULL, NULL},
    TLS_COMMON_CONFIG(MonitorResource),
    TLS_CERT_CONFIG(MonitorResource),
    {}
};

/*
 * Director's that we can contact
 *
 * name handler value code flags default_value
 */
static ResourceItem dir_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(DirectorResource,resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(DirectorResource,description_), 0, 0, NULL, NULL, NULL},
  {"DirPort", CFG_TYPE_PINT32, ITEM(DirectorResource,DIRport), 0, CFG_ITEM_DEFAULT, DIR_DEFAULT_PORT, NULL, NULL},
  {"Address", CFG_TYPE_STR, ITEM(DirectorResource,address), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
    TLS_COMMON_CONFIG(DirectorResource),
    TLS_CERT_CONFIG(DirectorResource),
    {}
};

/*
 * Client or File daemon resource
 *
 * name handler value code flags default_value
 */
static ResourceItem client_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(ClientResource,resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(ClientResource,description_), 0, 0, NULL, NULL, NULL},
  {"Address", CFG_TYPE_STR, ITEM(ClientResource,address), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"FdPort", CFG_TYPE_PINT32, ITEM(ClientResource,FDport), 0, CFG_ITEM_DEFAULT, FD_DEFAULT_PORT, NULL, NULL},
  {"Password", CFG_TYPE_MD5PASSWORD, ITEM(ClientResource,password), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
    TLS_COMMON_CONFIG(ClientResource),
    TLS_CERT_CONFIG(ClientResource),
    {}
};

/*
 * Storage daemon resource
 *
 * name handler value code flags default_value
 */
static ResourceItem store_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(StorageResource,resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(StorageResource,description_), 0, 0, NULL, NULL, NULL},
  {"SdPort", CFG_TYPE_PINT32, ITEM(StorageResource,SDport), 0, CFG_ITEM_DEFAULT, SD_DEFAULT_PORT, NULL, NULL},
  {"Address", CFG_TYPE_STR, ITEM(StorageResource,address), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"SdAddress", CFG_TYPE_STR, ITEM(StorageResource,address), 0, 0, NULL, NULL, NULL},
  {"Password", CFG_TYPE_MD5PASSWORD, ITEM(StorageResource,password), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"SdPassword", CFG_TYPE_MD5PASSWORD, ITEM(StorageResource,password), 0, 0, NULL, NULL, NULL},
    TLS_COMMON_CONFIG(StorageResource),
    TLS_CERT_CONFIG(StorageResource),
  {}
};

/*
 * Font resource
 *
 * name handler value code flags default_value
 */
static ResourceItem con_font_items[] = {
  {"Name", CFG_TYPE_NAME, ITEM(ConsoleFontResource,resource_name_), 0, CFG_ITEM_REQUIRED, NULL, NULL, NULL},
  {"Description", CFG_TYPE_STR, ITEM(ConsoleFontResource,description_), 0, 0, NULL, NULL, NULL},
  {"Font", CFG_TYPE_STR, ITEM(ConsoleFontResource,fontface), 0, 0, NULL, NULL, NULL},
  {}
};

/*
 * This is the master resource definition.
 * It must have one item for each of the resource_definitions.
 *
 * NOTE!!! keep it in the same order as the R_codes
 *   or eliminate all resource_definitions[rindex].name
 *
 *  name items rcode configuration_resources
 */
static ResourceTable resource_definitions[] = {
  {"Monitor", "Monitors", mon_items, R_MONITOR, ResourceFactory<MonitorResource> },
  {"Director", "Directors", dir_items, R_DIRECTOR, ResourceFactory<DirectorResource> },
  {"Client", "Clients", client_items, R_CLIENT, ResourceFactory<ClientResource> },
  {"Storage", "Storages", store_items, R_STORAGE, ResourceFactory<StorageResource> },
  {"ConsoleFont", "ConsoleFonts", con_font_items, R_CONSOLE_FONT, ResourceFactory<ConsoleFontResource> },
  {}
};

/* clang-format on */

// Dump contents of resource
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

  if (res == NULL) {
    sendit(sock, T_("Warning: no \"%s\" resource (%d) defined.\n"),
           my_config->ResToStr(type), type);
    return;
  }
  if (type < 0) { /* no recursion */
    type = -type;
    recurse = false;
  }
  switch (type) {
    default:
      res->PrintConfig(output_formatter_resource, *my_config,
                       hide_sensitive_data, verbose);
      break;
  }
  sendit(sock, "%s", buf.c_str());

  if (recurse && res->next_) {
    DumpResource(type, res->next_, sendit, sock, hide_sensitive_data, verbose);
  }
}

static void FreeResource(BareosResource* res, int type)
{
  if (res == NULL) return;

  BareosResource* next_resource = (BareosResource*)res->next_;

  if (res->resource_name_) { free(res->resource_name_); }
  if (res->description_) { free(res->description_); }

  switch (type) {
    case R_MONITOR:
      break;
    case R_DIRECTOR: {
      DirectorResource* p = dynamic_cast<DirectorResource*>(res);
      assert(p);
      if (p->address) { free(p->address); }
      break;
    }
    case R_CLIENT: {
      ClientResource* p = dynamic_cast<ClientResource*>(res);
      assert(p);
      if (p->address) { free(p->address); }
      if (p->password.value) { free(p->password.value); }
      break;
    }
    case R_STORAGE: {
      StorageResource* p = dynamic_cast<StorageResource*>(res);
      assert(p);
      if (p->address) { free(p->address); }
      if (p->password.value) { free(p->password.value); }
      break;
    }
    case R_CONSOLE_FONT: {
      ConsoleFontResource* p = dynamic_cast<ConsoleFontResource*>(res);
      assert(p);
      if (p->fontface) { free(p->fontface); }
      break;
    }
    default:
      printf(T_("Unknown resource type %d in FreeResource.\n"), type);
      break;
  }

  if (next_resource) { FreeResource(next_resource, type); }
}

static void ConfigBeforeCallback(ConfigurationParser& config)
{
  std::map<int, std::string> map{
      {R_MONITOR, "R_MONITOR"}, {R_DIRECTOR, "R_DIRECTOR"},
      {R_CLIENT, "R_CLIENT"},   {R_STORAGE, "R_STORAGE"},
      {R_CONSOLE, "R_CONSOLE"}, {R_CONSOLE_FONT, "R_CONSOLE_FONT"}};
  config.InitializeQualifiedResourceNameTypeConverter(map);
}

static void ConfigReadyCallback(ConfigurationParser&) {}

ConfigurationParser* InitTmonConfig(const char* configfile, int exit_code)
{
  ConfigurationParser* config = new ConfigurationParser(
      configfile, nullptr, nullptr, nullptr, nullptr, nullptr, exit_code, R_NUM,
      resource_definitions, default_config_filename.c_str(), "tray-monitor.d",
      ConfigBeforeCallback, ConfigReadyCallback, DumpResource, FreeResource);
  if (config) { config->r_own_ = R_MONITOR; }
  return config;
}

// Print configuration file schema in json format
#ifdef HAVE_JANSSON
bool PrintConfigSchemaJson(PoolMem& buffer)
{
  json_t* json = json_object();
  json_object_set_new(json, "format-version", json_integer(2));
  json_object_set_new(json, "component", json_string("bareos-tray-monitor"));
  json_object_set_new(json, "version", json_string(kBareosVersionStrings.Full));

  // Resources
  json_t* resource = json_object();
  json_object_set_new(json, "resource", resource);
  json_t* bareos_tray_monitor = json_object();
  json_object_set_new(resource, "bareos-tray-monitor", bareos_tray_monitor);

  for (int r = 0; my_config->resource_definitions_[r].name; r++) {
    ResourceTable& resource_table = my_config->resource_definitions_[r];
    json_object_set_new(bareos_tray_monitor, resource_table.name,
                        json_items(resource_table.items));
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
