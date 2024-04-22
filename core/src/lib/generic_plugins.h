/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_LIB_GENERIC_PLUGINS_H_
#define BAREOS_LIB_GENERIC_PLUGINS_H_

#include "lib/plugins.h"

/* Functions */
bool LoadPlugins(void* bareos_plugin_interface_version,
                 void* bareos_core_functions,
                 alist<Plugin*>* plugin_list,
                 const char* plugin_dir,
                 alist<const char*>* plugin_names,
                 const char* type,
                 bool IsPluginCompatible(Plugin* plugin));
void UnloadPlugins(alist<Plugin*>* plugin_list);
void UnloadPlugin(alist<Plugin*>* plugin_list, Plugin* plugin, int index);
int ListPlugins(alist<Plugin*>* plugin_list, PoolMem& msg);

#endif  // BAREOS_LIB_GENERIC_PLUGINS_H_
