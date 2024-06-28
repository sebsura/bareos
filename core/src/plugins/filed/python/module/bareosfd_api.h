/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can modify it under the terms of
   version three of the GNU Affero General Public License as published by the
   Free Software Foundation, which is listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#ifndef BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_API_H_
#define BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_API_H_

#include "filed/fd_plugins.h"

namespace filedaemon {
struct bareosfd_capi {
  bRC (*PyParsePluginDefinition)(PluginContext* bareos_plugin_ctx, void* value);
  bRC (*PyGetPluginValue)(PluginContext* bareos_plugin_ctx,
                          pVariable var,
                          void* value);
  bRC (*PySetPluginValue)(PluginContext* bareos_plugin_ctx,
                          pVariable var,
                          void* value);
  bRC (*PyHandlePluginEvent)(PluginContext* bareos_plugin_ctx,
                             bEvent* event,
                             void* value);
  bRC (*PyStartBackupFile)(PluginContext* bareos_plugin_ctx, save_pkt* sp);
  bRC (*PyEndBackupFile)(PluginContext* bareos_plugin_ctx);
  bRC (*PyPluginIO)(PluginContext* bareos_plugin_ctx, io_pkt* io);
  bRC (*PyStartRestoreFile)(PluginContext* bareos_plugin_ctx, const char* cmd);
  bRC (*PyEndRestoreFile)(PluginContext* bareos_plugin_ctx);
  bRC (*PyCreateFile)(PluginContext* bareos_plugin_ctx, restore_pkt* rp);
  bRC (*PySetFileAttributes)(PluginContext* bareos_plugin_ctx, restore_pkt* rp);
  bRC (*PyCheckFile)(PluginContext* bareos_plugin_ctx, char* fname);
  bRC (*PyGetAcl)(PluginContext* bareos_plugin_ctx, acl_pkt* ap);
  bRC (*PySetAcl)(PluginContext* bareos_plugin_ctx, acl_pkt* ap);
  bRC (*PyGetXattr)(PluginContext* bareos_plugin_ctx, xattr_pkt* xp);
  bRC (*PySetXattr)(PluginContext* bareos_plugin_ctx, xattr_pkt* xp);
  bRC (*PyRestoreObjectData)(PluginContext* bareos_plugin_ctx,
                             restore_object_pkt* rop);
  bRC (*PyHandleBackupFile)(PluginContext* bareos_plugin_ctx, save_pkt* sp);
  bRC (*set_bareos_core_functions)(CoreFunctions* new_bareos_core_functions);
  bRC (*set_plugin_context)(PluginContext* new_plugin_context);
};
}  // namespace filedaemon

#endif  // BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_API_H_
