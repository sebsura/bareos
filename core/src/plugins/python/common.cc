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

static inline bRC ConvertPythonRetvalTobRCRetval(PyObject* pRetVal)
{
  return (bRC)PyLong_AsLong(pRetVal);
}

/**
 * Initial load of the Python module.
 *
 * Based on the parsed plugin options we set some prerequisites like the
 * module path and the module to load. We also load the dictionary used
 * for looking up the Python methods.
 */
bRC PyLoadModule(common_private_context* ctx, void* value)
{
  bRC retval = bRC_Error;

  if (ctx->python_loaded) { return bRC_OK; }

  if (!ctx->module_name) { return bRC_Error; }

  /* Extend the Python search path with the given module_path.  */
  if (ctx->module_path) {
    PyObject* sysPath = PySys_GetObject((char*)"path");
    PyObject* mPath = PyUnicode_FromString(ctx->module_path);
    PyList_Insert(sysPath, 0, mPath);
    Py_DECREF(mPath);
  }

  /* Try to load the Python module by name. */
  // Dmsg(plugin_ctx, debuglevel, LOGPREFIX "Trying to load module with name
  // %s\n",
  //      plugin_priv_ctx->module_name);
  ctx->pModule = PyImport_ImportModule(ctx->module_name);

  if (!ctx->pModule) {
    // Dmsg(plugin_ctx, debuglevel,
    //      LOGPREFIX "Failed to load module with name %s\n",
    //      plugin_priv_ctx->module_name);
    return bRC_Error;
  }

  // Dmsg(plugin_ctx, debuglevel,
  //      LOGPREFIX "Successfully loaded module with name %s\n",
  //      plugin_priv_ctx->module_name);

  /* Get the Python dictionary for lookups in the Python namespace.  */
  ctx->pyModuleFunctionsDict
      = PyModule_GetDict(ctx->pModule); /* Borrowed reference */

  /* Lookup the load_bareos_plugin() function in the python module.  */
  PyObject* pFunc
      = PyDict_GetItemString(ctx->pyModuleFunctionsDict,
                             "load_bareos_plugin"); /* Borrowed reference */
  if (pFunc && PyCallable_Check(pFunc)) {
    PyObject* pPluginDefinition = PyUnicode_FromString((char*)value);
    if (!pPluginDefinition) { return bRC_Error; }

    PyObject* pRetVal
        = PyObject_CallFunctionObjArgs(pFunc, pPluginDefinition, NULL);
    Py_DECREF(pPluginDefinition);

    if (!pRetVal) { return bRC_Error; }
    retval = ConvertPythonRetvalTobRCRetval(pRetVal);
    Py_DECREF(pRetVal);
  } else {
    // Dmsg(plugin_ctx, debuglevel,
    //      LOGPREFIX "Failed to find function named load_bareos_plugin()\n");
    return bRC_Error;
  }

  // Keep track we successfully loaded.
  ctx->python_loaded = true;

  return retval;

  // bail_out:
  //   if (PyErr_Occurred()) { PyErrorHandler(plugin_ctx, M_FATAL); }

  //   return retval;
}
