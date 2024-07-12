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

#include "common.h"
#include "include/baconfig.h"
#include "lib/plugins.h"

#define SET_ENUM_VALUE(dict, val) AddDictValue(dict, #val, val)

PyObject* bRC_dict()
{
  PyObject* dict = PyDict_New();

  if (!dict) { return nullptr; }

  if (!SET_ENUM_VALUE(dict, bRC_OK) || !SET_ENUM_VALUE(dict, bRC_Stop)
      || !SET_ENUM_VALUE(dict, bRC_Error) || !SET_ENUM_VALUE(dict, bRC_More)
      || !SET_ENUM_VALUE(dict, bRC_Term) || !SET_ENUM_VALUE(dict, bRC_Seen)
      || !SET_ENUM_VALUE(dict, bRC_Core) || !SET_ENUM_VALUE(dict, bRC_Skip)
      || !SET_ENUM_VALUE(dict, bRC_Cancel)) {
    Py_DECREF(dict);
    return nullptr;
  }

  return dict;
}

PyObject* JobMessageType_dict()
{
  PyObject* dict = PyDict_New();

  if (!dict) { return nullptr; }

  if (!SET_ENUM_VALUE(dict, M_ABORT) || !SET_ENUM_VALUE(dict, M_DEBUG)
      || !SET_ENUM_VALUE(dict, M_FATAL) || !SET_ENUM_VALUE(dict, M_ERROR)
      || !SET_ENUM_VALUE(dict, M_WARNING) || !SET_ENUM_VALUE(dict, M_INFO)
      || !SET_ENUM_VALUE(dict, M_SAVED) || !SET_ENUM_VALUE(dict, M_NOTSAVED)
      || !SET_ENUM_VALUE(dict, M_SKIPPED) || !SET_ENUM_VALUE(dict, M_MOUNT)
      || !SET_ENUM_VALUE(dict, M_ERROR_TERM) || !SET_ENUM_VALUE(dict, M_TERM)
      || !SET_ENUM_VALUE(dict, M_RESTORED) || !SET_ENUM_VALUE(dict, M_SECURITY)
      || !SET_ENUM_VALUE(dict, M_ALERT) || !SET_ENUM_VALUE(dict, M_VOLMGMT)) {
    Py_DECREF(dict);
    return nullptr;
  }

  return dict;
}

bool Plugin_AddDict(PyObject* module, const char* name, PyObject* dict)
{
  if (!dict) { return false; }
  if (!PyDict_Check(dict)) { return false; }

  PyObject* module_dict = PyModule_GetDict(module);

  if (!module_dict) { return false; }

  /* NOTE: PyDict_Merge does not work as we use byte strings as keys instead
   * of real strings */

  {
    Py_ssize_t pos = 0;
    PyObject* key = nullptr;
    PyObject* value = nullptr;

    while (PyDict_Next(dict, &pos, &key, &value)) {
      if (!PyUnicode_Check(key)) {
        // cannot add bytes keys to module_dict
        continue;
      }

      if (PyDict_SetItem(module_dict, key, value) != 0) { return false; }
    }
  }
  if (PyDict_SetItemString(module_dict, name, dict) != 0) { return false; }
  Py_DECREF(dict);

  return true;
}
