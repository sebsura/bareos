#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2024-2024 Bareos GmbH & Co. KG
#
#   This program is Free Software; you can redistribute it and/or
#   modify it under the terms of version three of the GNU Affero General Public
#   License as published by the Free Software Foundation and included
#   in the file LICENSE.
#
#   This program is distributed in the hope that it will be useful, but
#   WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#   Affero General Public License for more details.
#
#   You should have received a copy of the GNU Affero General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
#   02110-1301, USA.

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

option(SET_PREFIX_MAP
       "remap absolute debug paths to relative if compiler supports it"
)

if(SET_PREFIX_MAP)
  set(BAREOS_PREFIX_MAP "${CMAKE_SOURCE_DIR}=/usr/src/bareos")

  check_c_compiler_flag(
    -fdebug-prefix-map=${BAREOS_PREFIX_MAP} c_compiler_debug_prefix_map
  )
  check_cxx_compiler_flag(
    -fdebug-prefix-map=${BAREOS_PREFIX_MAP} cxx_compiler_debug_prefix_map
  )
  if(c_compiler_debug_prefix_map AND cxx_compiler_debug_prefix_map)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -fdebug-prefix-map=${BAREOS_PREFIX_MAP}"
    )
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fdebug-prefix-map=${BAREOS_PREFIX_MAP}"
    )
  endif()

  check_c_compiler_flag(
    -fmacro-prefix-map=${BAREOS_PREFIX_MAP} c_compiler_macro_prefix_map
  )
  check_cxx_compiler_flag(
    -fmacro-prefix-map=${BAREOS_PREFIX_MAP} cxx_compiler_macro_prefix_map
  )
  if(c_compiler_macro_prefix_map AND cxx_compiler_macro_prefix_map)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -fmacro-prefix-map=${BAREOS_PREFIX_MAP}"
    )
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -fmacro-prefix-map=${BAREOS_PREFIX_MAP}"
    )
  endif()
endif()
