/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

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

#ifndef BAREOS_LIB_BAUTH_OIDC_VERIFIER_H_
#define BAREOS_LIB_BAUTH_OIDC_VERIFIER_H_

#include "lib/bauth.h"

namespace auth::Oidc {

// TODO: document that with entra id you need to set
// api.requestedAccessTokenVersion = 2 in the manifest

struct config {
  // we look at <openid_config_url>/.well-known/openid-configuration
  std::string oidc_url{};
  std::string audience{};
  std::string token_scope{};
  std::string api_scope{};

  std::string scopes_name{};
  std::string roles_name{};
};

struct Verifier : public auth::Verifier {
  using Base = auth::Verifier;

  Verifier(config conf) : cfg{std::move(conf)} {}

  std::string_view name() const override { return "oidc"; };
  std::vector<char> generate_challenge() override;
  auth::Status step(std::vector<char>&,
                    std::span<const char> received) override;
  const char* err() const override { return error.c_str(); }

  virtual ~Verifier() = default;

  std::string user_name;
  std::vector<std::string> roles;

 private:
  enum
  {
    INITIAL = 0,
    CHECKED_TOKEN,
    ERROR,
  } current_state{INITIAL};

  std::string error;

  config cfg;
};
};  // namespace auth::Oidc


#endif  // BAREOS_LIB_BAUTH_OIDC_VERIFIER_H_
