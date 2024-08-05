/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_DIRD_CONNECTION_PLUGIN_CONFIG_H_
#define BAREOS_DIRD_CONNECTION_PLUGIN_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

enum bareos_job_type : uint32_t
{
  BJT_BACKUP = 'B',       /**< Backup Job */
  BJT_MIGRATED_JOB = 'M', /**< A previous backup job that was migrated */
  BJT_VERIFY = 'V',       /**< Verify Job */
  BJT_RESTORE = 'R',      /**< Restore Job */
  BJT_CONSOLE = 'U',      /**< console program */
  BJT_SYSTEM = 'I',       /**< internal system "job" */
  BJT_ADMIN = 'D',        /**< admin job */
  BJT_ARCHIVE = 'A',      /**< Archive Job */
  BJT_JOB_COPY = 'C',     /**< Copy of a Job */
  BJT_COPY = 'c',         /**< Copy Job */
  BJT_MIGRATE = 'g',      /**< Migration Job */
  BJT_SCAN = 'S',         /**< Scan Job */
  BJT_CONSOLIDATE = 'O'   /**< Always Incremental Consolidate Job */
};


enum bareos_job_level : uint32_t
{
  BJL_NONE = 0,
  BJL_FULL = 'F',         /* Full backup */
  BJL_INCREMENTAL = 'I',  /* since last backup */
  BJL_DIFFERENTIAL = 'D', /* since last full backup */
};

enum bareos_resource_type
{
  BRT_DIRECTOR,
  BRT_CLIENT,
  BRT_JOBDEFS,
  BRT_JOB,
  BRT_STORAGE,
  BRT_CATALOG,
  BRT_SCHEDULE,
  BRT_FILESET,
  BRT_POOL,
  BRT_MSGS,
  BRT_COUNTER,
  BRT_PROFILE,
  BRT_CONSOLE,
  BRT_USER,
  BRT_GRPC,
};

struct bareos_config_catalog {
  const char* name;
  const char* db_name;
};

struct bareos_config_job {
  const char* name;
  enum bareos_job_type type;
  enum bareos_job_level level;
};

struct bareos_config_client {
  const char* name;
  const char* address;
};

enum bareos_config_schema_base_type
{
  BCSBT_STRING,
  BCSBT_ENUM,
  BCSBT_BOOL,
  BCSBT_POS_INT,  // [1, ...]
  BCSBT_NAT_INT,  // [0, ...]
};

struct bareos_config_schema_type {
  enum bareos_config_schema_base_type base_type;
  bool allow_multiple;
  size_t enum_value_count;
  const char* const* enum_values;
};

struct bareos_config_schema_entry {
  struct bareos_config_schema_type type;

  const char* name;
  const char* default_value;
  const char* description;

  bool required;
  bool deprecated;
};

typedef bool(config_client_callback)(void* user,
                                     const struct bareos_config_client* data);
typedef bool(config_catalog_callback)(void* user,
                                      const struct bareos_config_catalog* data);
typedef bool(config_job_callback)(void* user,
                                  const struct bareos_config_job* data);
typedef bool(config_schema_callback)(
    void* user,
    const struct bareos_config_schema_entry entry);

typedef bool(ConfigListClients_t)(config_client_callback* cb, void* user);
typedef bool(ConfigListJobs_t)(config_job_callback* cb, void* user);
typedef bool(ConfigListCatalogs_t)(config_catalog_callback* cb, void* user);

typedef bool(ConfigSchema_t)(bareos_resource_type type,
                             config_schema_callback* cb,
                             void* user);

struct config_capability {
  ConfigSchema_t* config_schema;

  ConfigListClients_t* list_clients;
  ConfigListJobs_t* list_jobs;
  ConfigListCatalogs_t* list_catalogs;
};

#ifdef __cplusplus
}
#endif

#endif  // BAREOS_DIRD_CONNECTION_PLUGIN_CONFIG_H_
