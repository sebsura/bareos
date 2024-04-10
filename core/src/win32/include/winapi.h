/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2003-2010 Free Software Foundation Europe e.V.
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
 * Windows APIs that are different for each system.
 * We use pointers to the entry points so that a
 * single binary will run on all Windows systems.
 *
 * Kern Sibbald MMIII
 */

#ifndef BAREOS_WIN32_INCLUDE_WINAPI_H_
#define BAREOS_WIN32_INCLUDE_WINAPI_H_

#if defined(HAVE_WIN32)

static_assert(_WIN32_WINNT >= _WIN32_WINNT_VISTA);

/*
 * Commented out native.h include statement, which is not distributed with the
 * free version of VC++, and which is not used in bareos.
 *
 * #if !defined(HAVE_MINGW) // native.h not present on mingw
 * #include <native.h>
 * #endif
 */
#  include <windef.h>
#  include <string>
#  include <unordered_map>
#  include <vector>
#  include <shlobj.h>

#  ifndef POOLMEM
typedef char POOLMEM;
#  endif

// unicode enabling of win 32 needs some defines and functions

// using an average of 3 bytes per character is probably fine in
// practice but I believe that Windows actually uses UTF-16 encoding
// as opposed to UCS2 which means characters 0x10000-0x10ffff are
// valid and result in 4 byte UTF-8 encodings.
#  define MAX_PATH_UTF8 \
    MAX_PATH * 4  // strict upper bound on UTF-16 to UTF-8 conversion

// from
// http://msdn.microsoft.com/library/default.asp?url=/library/en-us/fileio/fs/getfileattributesex.asp
// In the ANSI version of this function, the name is limited to
// MAX_PATH characters. To extend this limit to 32,767 wide
// characters, call the Unicode version of the function and prepend
// "\\?\" to the path. For more information, see Naming a File.
#  define MAX_PATH_W 32767

std::wstring FromUtf8(std::string_view utf8);
std::string FromUtf16(std::wstring_view utf16);
int wchar_2_UTF8(POOLMEM*& pszUTF, const wchar_t* pszUCS);
int UTF8_2_wchar(POOLMEM*& pszUCS, const char* pszUTF);
int wchar_2_UTF8(char* pszUTF, const wchar_t* pszUCS, int cchChar);
BSTR str_2_BSTR(const char* pSrc);
char* BSTR_2_str(const BSTR pSrc);
std::wstring make_win32_path_UTF8_2_wchar(std::string_view utf8);

void InitWinAPIWrapper();

namespace dyn {
class dynamic_function;
using function_registry
    = std::unordered_map<std::string, std::vector<dynamic_function*>>;
class dynamic_function {
 protected:
  dynamic_function(function_registry& reg, const char* lib);
  virtual ~dynamic_function() {}

 public:
  virtual bool load(HMODULE lib) = 0;
};

inline function_registry dynamic_functions{};

#  define DEFINE_DYN_FUNC(Lib, Name)                                     \
    inline struct dyn##Name : dynamic_function {                         \
      static constexpr const char* name = #Name;                         \
      using type = decltype(::Name);                                     \
      type* ptr{nullptr};                                                \
      dyn##Name(function_registry& reg) : dynamic_function(reg, Lib) {}  \
      template <typename... Args> auto operator()(Args... args)          \
      {                                                                  \
        return ptr(std::forward<Args>(args)...);                         \
      }                                                                  \
      operator bool() const { return (ptr != nullptr); }                 \
      bool load(HMODULE lib) override                                    \
      {                                                                  \
        ptr = reinterpret_cast<type*>((void*)GetProcAddress(lib, name)); \
        return (ptr != nullptr);                                         \
      }                                                                  \
    } Name(dynamic_functions);

DEFINE_DYN_FUNC("KERNEL32.DLL", FindFirstFileW);
DEFINE_DYN_FUNC("KERNEL32.DLL", FindFirstFileA);
DEFINE_DYN_FUNC("SHELL32.DLL", SHGetKnownFolderPath);

void LoadDynamicFunctions();

#  undef DEFINE_DYNAMIC_FUNC
};  // namespace dyn

#endif
#endif  // BAREOS_WIN32_INCLUDE_WINAPI_H_
