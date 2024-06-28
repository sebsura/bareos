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

#ifndef BAREOS_PLUGINS_PYTHON_COMMON_H_
#define BAREOS_PLUGINS_PYTHON_COMMON_H_

#include <Python.h>

template <typename T> PyObject* PyValue(const T& val)
{
  if constexpr (std::is_convertible_v<T, long>) {
    return PyLong_FromLong(static_cast<long>(val));
  } else if constexpr (std::is_convertible_v<T, const char*>) {
    return PyBytes_FromString(static_cast<const char*>(val));
  } else {
    static_assert(false, "Not implemented yet.");
  }
}

template <typename T>
bool AddDictValue(PyObject* dict, const char* str, const T& val)
{
  auto* obj = PyValue(val);

  if (!obj) { return false; }

  auto res = PyDict_SetItemString(dict, str, obj);
  Py_DECREF(obj);

  return res == 0;
}

PyObject* bRC_dict();
PyObject* JobMessageType_dict();

struct module_dict {
  const char* name;
  PyObject* obj;

  ~module_dict() { Py_XDECREF(obj); }
};


bool Plugin_AddDict(PyObject* module, const char* name, PyObject* dict);

#define PYTHON_INIT(name) PyMODINIT_FUNC PyInit_##name(void)

#endif  // BAREOS_PLUGINS_PYTHON_COMMON_H_
