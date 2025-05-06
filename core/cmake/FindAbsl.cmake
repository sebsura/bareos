#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2017-2025 Bareos GmbH & Co. KG
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

# SUSE abseil
find_package(absl REQUIRED)
set(absl_LIBRARIES
    absl::algorithm
    absl::base
    absl::debugging
    absl::flat_hash_map
    absl::memory
    absl::meta
    absl::numeric
    absl::str_format
    absl::strings
    absl::synchronization
    absl::time
    absl::log_internal_check_op
)
message(STATUS "Using absl version ${absl_VERSION}")
message(STATUS "absl lib: ${absl_LIBRARIES}")
