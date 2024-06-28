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
#include "lib/plugins.h"

template <typename T> PyObject* PyValue(const T& val)
{
  if constexpr (std::is_convertible_v<T, long>) {
    return PyLong_FromLong(static_cast<long>(val));
  } else {
    static_assert(false, "Not implemented yet.");
  }
}

template <typename T>
bool AddDefinition(PyObject* dict, const char* str, const T& val)
{
  auto* obj = PyValue(val);

  if (!obj) { return false; }

  auto res = PyDict_SetItemString(dict, str, obj);
  Py_DECREF(obj);

  return res == 0;
}

#define SET_ENUM_VALUE(dict, val) AddDefinition(dict, #val, val)

PyObject* bRC_Dict()
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

PyObject* JobMessageType_Dict()
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

bool MergeIntoModuleDict(PyObject* module, PyObject* dict)
{
  if (!PyDict_Check(dict)) { return false; }

  Py_ssize_t current = 0;

  PyObject* key = nullptr;
  PyObject* value = nullptr;

  PyObject* module_dict = PyModule_GetDict(module);

  if (!module_dict) { return false; }

  // why would you do this ?
  if (module_dict == dict) { return false; }

  while (PyDict_Next(dict, &current, &key, &value)) {
    if (PyDict_SetItem(module, key, value) < 0) { return false; }
  }

  return true;
}
