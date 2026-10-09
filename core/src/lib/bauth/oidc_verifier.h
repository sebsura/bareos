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

// this authentication scheme does nothing
// it is intended for situations where the authentication
// was already done (implicitly) by tls
namespace auth::Oidc {
struct Verifier : public auth::Verifier {
  using Base = auth::Verifier;

  Verifier() {}

  std::string_view name() const override { return "oidc"; };
  std::vector<char> generate_challenge() override;
  auth::Status step(std::vector<char>&,
                    std::span<const char> received) override;
  const char* err() const override { return error.c_str(); }

  virtual ~Verifier() = default;

 private:
  enum
  {
    INITIAL = 0,
    ACCEPTED_RESULT,
    ERROR,
  } current_state{INITIAL};

  std::string error;
};
};  // namespace auth::Oidc


#endif  // BAREOS_LIB_BAUTH_OIDC_VERIFIER_H_
