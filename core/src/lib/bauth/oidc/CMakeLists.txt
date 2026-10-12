#   BAREOS® - Backup Archiving REcovery Open Sourced
#
#   Copyright (C) 2026-2026 Bareos GmbH & Co. KG
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

find_package(CURL REQUIRED)

add_library(oidc_common OBJECT)
target_sources(oidc_common PRIVATE common.cc)

add_library(bauth_oidc_verifier OBJECT)
target_sources(bauth_oidc_verifier PRIVATE verifier.cc)
target_link_libraries(
  bauth_oidc_verifier PRIVATE CURL::libcurl Bareos::Lib Jansson::Jansson
                              oidc_common
)

add_library(bauth_oidc_prover OBJECT)
target_sources(bauth_oidc_prover PRIVATE prover.cc)
target_link_libraries(
  bauth_oidc_prover PRIVATE CURL::libcurl Bareos::Lib Jansson::Jansson
                            oidc_common
)

define_auth_plugin(
  bauth_oidc VERIFIER bauth_oidc_verifier PROVER bauth_oidc_prover
)
