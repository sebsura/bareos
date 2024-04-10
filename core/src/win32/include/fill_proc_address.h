/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2022-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_WIN32_INCLUDE_FILL_PROC_ADDRESS_H_
#define BAREOS_WIN32_INCLUDE_FILL_PROC_ADDRESS_H_

#include <windows.h>

/*
 * wrap GetProcAddress so that the type of the pointer is deduced and the
 * compiler warning is suppressed. We also return the pointer, so the caller
 * can simply do the following:
 *
 * WinAPI my_func
 * if (!BareosFillProcAddress(my_func, my_module, "my_function_name")) {
 *   // error handling
 * }
 */

template <typename T>
T BareosFillProcAddress(T& func_ptr, HMODULE hModule, LPCSTR lpProcName)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
  func_ptr = reinterpret_cast<T>(GetProcAddress(hModule, lpProcName));
#pragma GCC diagnostic pop
  return func_ptr;
}

#define DEFINE_DYNAMIC_FUNCTION(Name)     \
  struct Name {                           \
    static constexpr char name[] = #Name; \
    using type = decltype(::Name);        \
  }
DEFINE_DYNAMIC_FUNCTION(
    RtlTest);  // -> struct RtlTest { static constexpr char name[] = "RtlTest";
               // using type = decltype(::RtlTest); };
struct DebugHelp : function_bundle<RtlTest> {};

Debughelp.load(library);
DebugHelp.call<Fun1>(Arg1);
DebugHelp.call<Fun2>(Brg1, Brg2);

#endif  // BAREOS_WIN32_INCLUDE_FILL_PROC_ADDRESS_H_
