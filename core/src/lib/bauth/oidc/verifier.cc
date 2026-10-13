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

#include "verifier.h"

#include <curl/curl.h>
#include <curl/easy.h>
#include <jansson.h>
#include "lib/base64.h"
#include <iostream>

#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>

#include <sstream>
#include <string_view>

#include "common.h"

#define UNIQUE_PTR(Type)                           \
  std::unique_ptr<Type, decltype([](Type* val) {   \
                    if (val) { Type##_free(val); } \
                  })>

using evp_pkey = UNIQUE_PTR(EVP_PKEY);
using evp_pkey_ctx = UNIQUE_PTR(EVP_PKEY_CTX);
using evp_md_ctx = UNIQUE_PTR(EVP_MD_CTX);

#undef UNIQUE_PTR

namespace auth::Oidc {
namespace {
// see rfc 7519
struct jwt_header {
  std::string algorithm;
  std::string key_id;
};

struct jwt_payload {
  std::string issuer;
  std::string subject;
  std::string audience;
  std::string scope;
  std::int64_t expiry;
  std::int64_t not_before;
  std::int64_t issued_at;
  std::string id;

  std::string user_id;
  std::string user_name;

  std::vector<std::string> roles;
};

struct jwt {
  jwt_header header;
  jwt_payload payload;
};

std::pair<std::string, std::vector<char>> ExtractSignature(
    std::string_view to_parse)
{
  auto end = to_parse.find_last_of('.');
  if (end == to_parse.npos || end == to_parse.size() - 1) {
    throw std::runtime_error{"no dots or bad dot at end"};
  }

  auto signature64 = to_parse.substr(end + 1);
  std::vector<char> signature;
  signature.resize(signature64.size() + 1);
  auto actual_size = Base64ToBin(signature.data(), signature.size(),
                                 signature64.data(), signature64.size());

  if (actual_size <= 0) {
    throw std::runtime_error{"bad base64 encoded signature"};
  }

  signature.resize(actual_size);

  return {std::string{to_parse.substr(0, end)}, std::move(signature)};
}

// std does not include the signature
jwt ExtractJwt(std::string_view token,
               const char* roles_name,
               const char* scopes_name)
{
  auto to_parse = token;
  auto end = to_parse.find('.');
  if (end == to_parse.npos) { throw std::runtime_error{"no dots"}; }

  auto header64 = to_parse.substr(0, end);
  to_parse.remove_prefix(end);

  std::cout << "HEADER64: " << header64 << std::endl;
  std::cout << "REST: " << to_parse << std::endl;

  std::string header;
  header.resize(header64.size());
  int actual_size = Base64ToBin(header.data(), header.size(), header64.data(),
                                header64.size());
  header.resize(actual_size);

  // check if "alg" is "RS256"; we do not accept anything else
  std::cout << "HEADER: " << header << std::endl;

  json_error_t err = {};
  json_t* jheader = json_loadb(header.data(), header.size(), 0, &err);
  if (!jheader || !json_is_object(jheader)) {
    throw std::runtime_error{"bad jwt header"};
  }

  const char* header_type{};
  const char* header_algo{};
  const char* header_key_id{};
  if (json_unpack_ex(jheader, &err, 0, "{s:s, s:s, s:s}", "typ", &header_type,
                     "alg", &header_algo, "kid", &header_key_id)
      < 0) {
    throw std::runtime_error{"missing values in jwt header"};
  }

  // todo: check the header for any other key and reject it if its unknown

  if (std::string_view{"JWT"} != header_type) {
    throw std::runtime_error{"bad jwt header (bad type)"};
  }
  if (std::string_view{"RS256"} != header_algo) {
    throw std::runtime_error{"bad jwt header (bad algo)"};
  }

  std::cerr << "Want key with id " << header_key_id << std::endl;

  auto payload64 = to_parse.substr(1);

  std::cout << "PAYLOAD64: " << payload64 << std::endl;

  std::string payload;
  payload.resize(payload64.size());
  actual_size = Base64ToBin(payload.data(), payload.size(), payload64.data(),
                            payload64.size());
  payload.resize(actual_size);

  std::cout << "PAYLOAD: " << payload << std::endl;

  json_t* jpayload = json_loadb(payload.data(), payload.size(), 0, &err);
  if (!jpayload || !json_is_object(jpayload)) {
    throw std::runtime_error{"bad jwt payload"};
  }


  json_t* roles{};
  const char* issuer{};
  const char* subject{};
  const char* audience{};
  json_int_t expiry{};
  json_int_t not_before{};
  json_int_t issued_at{};
  const char* id{"unknown"};
  const char* scope{};
  const char* user_id{};
  const char* user_name{};
  // first parse required fields
  if (json_unpack_ex(jpayload, &err, 0,
                     "{s:s, s:s, s:s, s:s, s:s,s:i, s:i, s:i, s:s, s:o}", "oid",
                     &user_id, "name", &user_name, "iss", &issuer, "sub",
                     &subject, "aud", &audience, "exp", &expiry, "nbf",
                     &not_before, "iat", &issued_at, scopes_name, &scope,
                     // "jti", &id,
                     roles_name, &roles)
      < 0) {
    throw std::runtime_error{std::string{"missing values in jwt payload: "}
                             + err.text};
  }

  if (!json_is_array(roles)) {
    throw std::runtime_error{"bad roles value in jwt payload"};
  }

  jwt_payload payload_jwt{issuer,  subject,    audience,  scope,
                          expiry,  not_before, issued_at, id,
                          user_id, user_name,  {}};

  json_t* role{};
  json_int_t idx{};
  json_array_foreach(roles, idx, role)
  {
    if (!json_is_string(role)) {
      throw std::runtime_error{"bad role value in jwt payload"};
    }

    payload_jwt.roles.push_back(json_string_value(role));
  }

  return {{header_algo, header_key_id}, payload_jwt};
}

struct rsa_key {
  std::vector<char> exponent;
  std::vector<char> modulus;
};

using keyset = std::unordered_map<std::string, rsa_key>;

keyset get_keys(CURL* curl, const std::string& key_uri, std::string_view issuer)
{
  curl_easy_setopt(curl, CURLOPT_URL, key_uri.c_str());

  std::string response;
  auto res = curl_easy_write_into_str(curl, &response);

  if (res != CURLE_OK) { throw std::runtime_error{"could not fetch keys"}; }

  json_error_t err = {};
  json_t* keys_json = json_loadb(response.data(), response.size(), 0, &err);

  if (!keys_json || !json_is_object(keys_json)) {
    throw std::runtime_error{"bad json keys"};
  }

  json_t* key_array = json_object_get(keys_json, "keys");
  if (!key_array || !json_is_array(key_array)) {
    throw std::runtime_error{"bad json key array"};
  }

  keyset keys;

  json_int_t index{};
  json_t* key{};
  json_array_foreach(key_array, index, key)
  {
    if (!key || !json_is_object(key)) {
      throw std::runtime_error{"bad json key"};
    }

    const char* key_type{};
    const char* key_usage{};
    const char* key_id{};
    const char* key_issuer{};
    const char* key_modulus64{};
    const char* key_exponent64{};
    if (json_unpack_ex(key, &err, 0, "{s:s, s:s, s:s, s:s, s:s, s:s}", "kty",
                       &key_type, "use", &key_usage, "kid", &key_id, "issuer",
                       &key_issuer, "n", &key_modulus64, "e", &key_exponent64)
        < 0) {
      continue;
    }

    // we only care about signing keys
    if (std::string_view{"sig"} != key_usage) {
      std::cerr << "Skipping key " << key_id << " (bad usage)" << std::endl;
      continue;
    }

    // we only care about keys from the issuer
    if (std::string_view{key_issuer} != issuer) {
      std::cerr << "Skipping key " << key_id << " (bad issuer)" << std::endl;
      continue;
    }

    if (std::string_view{"RSA"} != key_type) {
      std::cerr << "Skipping key " << key_id << " (bad key type)" << std::endl;
      continue;
    }

    rsa_key rsa;
    std::string_view m64{key_modulus64};
    rsa.modulus.resize(m64.size() + 1);

    auto actual_size = Base64ToBin(rsa.modulus.data(), rsa.modulus.size(),
                                   m64.data(), m64.size());

    if (actual_size <= 0) {
      std::cerr << "Skipping key " << key_id << " (bad key)" << std::endl;
      continue;
    }

    rsa.modulus.resize(actual_size);

    std::string_view e64{key_exponent64};
    rsa.exponent.resize(e64.size() + 1);

    actual_size = Base64ToBin(rsa.exponent.data(), rsa.exponent.size(),
                              e64.data(), e64.size());

    if (actual_size <= 0) {
      std::cerr << "Skipping key " << key_id << " (bad key)" << std::endl;
      continue;
    }

    rsa.exponent.resize(actual_size);

    assert(keys.find(key_id) == keys.end());
    keys.emplace(key_id, std::move(rsa));
  }

  return keys;
}

evp_pkey create_public_key(std::span<const char> modulus,
                           std::span<const char> exponent)
{
  // modulus is big endian encoded, but openssl expects the bytes in
  // native format
  std::vector<unsigned char> native(modulus.size());
  for (size_t i = 0; i < modulus.size(); ++i) {
    native[i] = modulus[modulus.size() - i - 1];
  }

  evp_pkey_ctx ctx{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
  if (!ctx) { return {}; }


  if (EVP_PKEY_fromdata_init(ctx.get()) != 1) { return {}; }

  OSSL_PARAM params[3]{};
  params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_N, native.data(),
                                      static_cast<int>(modulus.size()));

  // todo: this also needs to get converted to native
  params[1] = OSSL_PARAM_construct_BN(
      OSSL_PKEY_PARAM_RSA_E,
      const_cast<unsigned char*>(
          reinterpret_cast<const unsigned char*>(exponent.data())),
      static_cast<int>(exponent.size()));
  params[2] = OSSL_PARAM_construct_end();

  EVP_PKEY* result{};

  if (EVP_PKEY_fromdata(ctx.get(), &result, EVP_PKEY_PUBLIC_KEY, params) != 1) {
    return {};
  }

  return evp_pkey{result};
}

bool verify_signature(std::string_view token,
                      std::span<const char> signature,
                      const rsa_key& key)
{
  evp_pkey public_key = create_public_key(key.modulus, key.exponent);
  if (!public_key) { return false; }

  evp_md_ctx ctx{EVP_MD_CTX_new()};
  if (!ctx) { return false; }

  EVP_PKEY_CTX* kctx{};  // this is owned by ctx, so we use a non-owning ptr
  if (EVP_DigestVerifyInit(ctx.get(), &kctx, EVP_sha256(), nullptr,
                           public_key.get())
      != 1) {
    Dmsg0(100, "Could not init verify digest\n");
    return false;
  }

  if (!kctx) {
    Dmsg0(100, "No kctx was provided\n");
    return false;
  }

  if (EVP_PKEY_CTX_set_rsa_padding(kctx, RSA_PKCS1_PADDING) <= 0) {
    Dmsg0(100, "Could not set rsa padding\n");
    return false;
  }

  if (EVP_PKEY_CTX_set_signature_md(kctx, EVP_sha256()) <= 0) {
    Dmsg0(100, "Could not set signature md\n");
    return false;
  }

  if (EVP_DigestVerifyUpdate(ctx.get(), token.data(), token.size()) != 1) {
    Dmsg0(100, "Could not compute digest\n");
    return false;
  }

  if (EVP_DigestVerifyFinal(
          ctx.get(), reinterpret_cast<const unsigned char*>(signature.data()),
          signature.size())
      != 1) {
    Dmsg0(100, "Could not get digest\n");
    return false;
  }

  return true;
}

jwt_payload VerifyToken(const auth::Oidc::config& cfg,
                        const keyset& keys,
                        std::string_view token)
{
  auto [value, signature] = ExtractSignature(token);
  auto [header, payload]
      = ExtractJwt(value, cfg.roles_name.c_str(), cfg.scopes_name.c_str());

  if (payload.issuer != cfg.oidc_url) {
    throw std::runtime_error{
        "received token with bad issuer: expected:'" + cfg.oidc_url + "' got:'"
            + payload.issuer + "'",
    };
  }

  std::stringstream msg;
  msg << "Checking token for " << payload.user_name << " (" << payload.user_id
      << ")\n"
      << "with roles:\n";
  for (auto& role : payload.roles) { msg << " - " << role << "\n"; }

  Dmsg0(100, "%s\n", msg.str().c_str());

  if (auto iter = keys.find(header.key_id); iter != keys.end()) {
    const auto& used_key = iter->second;
    // we need to check that value matches signature via used_key

    if (!verify_signature(value, signature, used_key)) {
      throw std::runtime_error{"bad signature! rejecting jwt"};
    }

  } else {
    throw std::runtime_error{"unknown key id! rejecting jwt"};
  }

  static constexpr auto allowed_clock_skew = std::chrono::seconds{30};

  auto now = std::chrono::system_clock::now();


  if ((now - allowed_clock_skew).time_since_epoch().count()
      < payload.issued_at) {
    throw std::runtime_error{"token does not exist yet"};
  }

  if ((now + allowed_clock_skew).time_since_epoch().count()
      < payload.not_before) {
    throw std::runtime_error{"token not ready yet"};
  }

  if (payload.audience != cfg.audience) {
    throw std::runtime_error{"this token is not for me"};
  }

  if (payload.scope != cfg.token_scope) {
    throw std::runtime_error{"this token is for a different use"};
  }

  return std::move(payload);
}
};  // namespace

std::vector<char> Verifier::generate_challenge()
{
  Challenge data{
      cfg.oidc_url,
      cfg.audience,
      cfg.api_scope,
  };

  auto to_send = data.format();

  std::vector<char> result;

  result.insert(result.end(), to_send.begin(), to_send.end());

  return result;
}

Status Verifier::step(std::vector<char>&, std::span<const char> received)
{
  if (current_state != INITIAL) {
    if (error.empty()) error = "entered with bad state";
    current_state = ERROR;
    return Status::Error;
  }

  ASSERT(error.empty());

  if (cfg.oidc_url.empty()) {
    error = "empty oidc url";
    current_state = ERROR;
    return Status::Error;
  }

  auto response = KeyResponse::parse(received);
  if (!response) {
    error = "could not parse keyresponse input";
    current_state = ERROR;
    return Status::Error;
  }

  // TODO: this should be done periodically, not during verification
  CURL* curl = curl_easy_init();
  if (!curl) {
    error = "could not create curl connection";
    current_state = ERROR;
    return Status::Error;
  }

  std::string config_url = cfg.oidc_url;
  if (config_url.back() != '/') { config_url.push_back('/'); }

  config_url += ".well-known/openid-configuration";

  try {
    auto meta = fetch_metadata(curl, config_url);
    auto keys = get_keys(curl, meta.jwks_uri, meta.issuer);

    auto payload = VerifyToken(cfg, keys, response->key);

    roles = std::move(payload.roles);
    user_name = std::move(payload.user_name);
  } catch (const std::exception& ex) {
    error = ex.what();
    current_state = ERROR;
    return Status::Error;
  }

  current_state = CHECKED_TOKEN;
  return Status::Done;
}
};  // namespace auth::Oidc
