/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2013-2014 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

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
/**
 * @file
 * This defines the Python types in C++ and the callbacks from Python we
 * support.
 */

#ifndef BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_H_
#define BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_H_

/* common code for all python plugins */
#include "plugins/include/common.h"
#include "plugins/python/common.h"

#include "structmember.h"

/* include automatically generated C API */
#include "c_api/capi_1.inc"
/* This section is used in modules that use bareosfd's API */

static void** Bareosfd_API;

/* include automatically generated C API */
#include "c_api/capi_2.inc"

static int import_bareosfd()
{
  Bareosfd_API = (void**)PyCapsule_Import("bareosfd._C_API", 0);
  return (Bareosfd_API != NULL) ? 0 : -1;
}

#endif  // BAREOS_PLUGINS_FILED_PYTHON_MODULE_BAREOSFD_H_
