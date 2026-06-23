/* BAREOS® - Backup Archiving REcovery Open Sourced
 *
 * Copyright (C) 2026-2026 Bareos GmbH & Co. KG
 *
 * This program is Free Software; you can redistribute it and/or
 * modify it under the terms of version three of the GNU Affero General Public
 * License as published by the Free Software Foundation and included
 * in the file LICENSE.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. */
#include "dird/oidc.h"

#include <curl/curl.h>
#include <curl/easy.h>
#include <dirent.h>
#include <fmt/format.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/core_names.h>
#include <sys/stat.h>

#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <variant>
#include "dird/dird_conf.h"
#include "stored/backends/dedupable/device_options.h"

#include <lib/base64.h>

struct bearer_token {
  std::chrono::steady_clock::time_point expiry_point;
  std::string scope;
  std::string value;
};

struct string_writer {
  std::string s;

  size_t operator()(const char* data, size_t size)
  {
    s.insert(s.end(), data, data + size);
    return size;
  }

  void reset() { s = {}; }
};

bool ParseMessage(std::string_view to_parse, bearer_token* token)
{
  json_error_t err = {};
  json_t* json = json_loadb(to_parse.data(), to_parse.size(), 0, &err);

  if (!json || !json_is_object(json)) { return false; }


  const char* access_token{};
  const char* scope{};
  json_int_t expires_in{};

  bool success = false;

  if (json_unpack_ex(json, &err, 0, "{s:s, s:s, s:I}", "access_token",
                     &access_token, "scope", &scope, "expires_in", &expires_in)
      == 0) {
    token->scope = scope;
    token->value = access_token;
    token->expiry_point
        = std::chrono::steady_clock::now() + std::chrono::seconds{expires_in};
    success = true;
  }

  json_decref(json);
  return success;
}

template <typename F> auto curl_easy_write_cb(CURL* curl, F&& fun)
{
  curl_easy_setopt(
      curl, CURLOPT_WRITEFUNCTION,
      +[](char* curldata, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* cb = static_cast<std::remove_reference_t<F>*>(userdata);

        size_t bytes = size * nmemb;
        return (*cb)(curldata, bytes);
      });
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &fun);

  return curl_easy_perform(curl);
}

template <typename F> void curl_easy_set_read_cb(CURL* curl, F&& fun)
{
  curl_easy_setopt(
      curl, CURLOPT_READFUNCTION,
      +[](char* curldata, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* cb = static_cast<std::remove_reference_t<F>*>(userdata);

        size_t bytes = size * nmemb;
        return (*cb)(curldata, bytes);
      });
  curl_easy_setopt(curl, CURLOPT_READDATA, &fun);
}

struct url_encoder {
  void add_kv(std::string_view key, std::string_view value)
  {
    if (!encoded.empty()) { encoded += '&'; }

    char* escaped = curl_easy_escape(nullptr, value.data(), value.size());

    encoded += key;
    encoded += '=';
    encoded += escaped;

    curl_free(escaped);
  }


  const char* c_str() const { return encoded.c_str(); }
  const std::string& str() const& { return encoded; }
  std::string_view view() const { return encoded; }

  std::string&& str() && { return std::move(encoded); }

 private:
  std::string encoded{};
};

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
jwt ExtractJwt(std::string_view token)
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
                     &not_before, "iat", &issued_at, "scp", &scope,
                     // "jti", &id,
                     "roles", &roles)
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

struct site_metadata {
  std::string token_endpoint{};
  std::string jwks_uri{};
  std::string issuer{};
  std::string device_authorization_endpoint{};
};

struct rsa_key {
  std::vector<char> exponent;
  std::vector<char> modulus;
};

std::unordered_map<std::string, rsa_key> get_keys(CURL* curl,
                                                  const std::string& key_uri,
                                                  std::string_view issuer)
{
  curl_easy_setopt(curl, CURLOPT_URL, key_uri.c_str());

  string_writer sw;
  auto res = curl_easy_write_cb(curl, sw);

  if (res != CURLE_OK) { throw std::runtime_error{"could not fetch keys"}; }

  json_error_t err = {};
  json_t* keys = json_loadb(sw.s.data(), sw.s.size(), 0, &err);

  if (!keys || !json_is_object(keys)) {
    throw std::runtime_error{"bad json keys"};
  }

  json_t* key_array = json_object_get(keys, "keys");
  if (!key_array || !json_is_array(key_array)) {
    throw std::runtime_error{"bad json key array"};
  }

  std::unordered_map<std::string, rsa_key> keyset;

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

    assert(keyset.find(key_id) == keyset.end());
    keyset.emplace(key_id, std::move(rsa));
  }

  return keyset;
}

EVP_PKEY* create_public_key(std::span<const char> modulus,
                            std::span<const char> exponent)
{
  EVP_PKEY* result{};
  EVP_PKEY_CTX* ctx{};
  OSSL_PARAM params[3]{};

  // modulus is big endian encoded, but openssl expects the bytes in
  // native format
  std::vector<unsigned char> native(modulus.size());
  for (size_t i = 0; i < modulus.size(); ++i) {
    native[i] = modulus[modulus.size() - i - 1];
  }

  ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
  if (!ctx) { goto cleanup; }


  if (EVP_PKEY_fromdata_init(ctx) != 1) { goto cleanup; }
  params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_N, native.data(),
                                      static_cast<int>(modulus.size()));


  // todo: this also needs to get converted to native
  params[1] = OSSL_PARAM_construct_BN(
      OSSL_PKEY_PARAM_RSA_E,
      const_cast<unsigned char*>(
          reinterpret_cast<const unsigned char*>(exponent.data())),
      static_cast<int>(exponent.size()));
  params[2] = OSSL_PARAM_construct_end();

  if (EVP_PKEY_fromdata(ctx, &result, EVP_PKEY_PUBLIC_KEY, params) != 1) {
    goto cleanup;
  }

cleanup:
  if (ctx) { EVP_PKEY_CTX_free(ctx); }

  return result;
}

bool verify_signature(std::string_view token,
                      std::span<const char> signature,
                      const rsa_key& key)
{
  EVP_PKEY_CTX* kctx{};
  bool success = false;
  EVP_PKEY* public_key = create_public_key(key.modulus, key.exponent);
  if (!public_key) { return false; }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) { goto cleanup; }

  if (EVP_DigestVerifyInit(ctx, &kctx, EVP_sha256(), nullptr, public_key)
      != 1) {
    goto cleanup;
  }

  if (EVP_PKEY_CTX_set_rsa_padding(kctx, RSA_PKCS1_PADDING) <= 0) {
    goto cleanup;
  }

  if (EVP_PKEY_CTX_set_signature_md(kctx, EVP_sha256()) <= 0) { goto cleanup; }

  if (EVP_DigestVerifyUpdate(ctx, token.data(), token.size()) != 1) {
    goto cleanup;
  }

  {
    auto result = EVP_DigestVerifyFinal(
        ctx, reinterpret_cast<const unsigned char*>(signature.data()),
        signature.size());
    success = result == 1;
  }

cleanup:
  if (ctx) { EVP_MD_CTX_free(ctx); }
  EVP_PKEY_free(public_key);

  return success;
}

struct DeviceAuthRequest {
  std::string user_code;
  std::string device_code;
  std::string verification_uri;
  std::chrono::seconds expires_in;
  std::chrono::seconds poll_interval;
  std::string message;
};


namespace oidc {
struct config {
  std::string tenant_id{""};
  std::string client_id{""};
  std::string client_secret{""};
  std::string app_uri{""};
  std::string app_scope{""};

  std::string scoped_uri{app_uri + "/" + app_scope};


  bool fetch_token(CURL* curl,
                   const site_metadata& meta,
                   DeviceAuthRequest& device_authentication,
                   bearer_token* token)
  {
    curl_easy_setopt(curl, CURLOPT_URL, meta.token_endpoint.c_str());

    url_encoder encoder{};
    encoder.add_kv("client_id", client_id);
    encoder.add_kv("device_code", device_authentication.device_code);
    encoder.add_kv("grant_type",
                   "urn:ietf:params:oauth:grant-type:device_code");

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    string_writer writer;
    auto res = curl_easy_write_cb(curl, writer);

    curl_easy_reset(curl);

    if (res != CURLE_OK) {
      Dmsg0(100, "Could not fetch from %s: %s (%d)\n",
            meta.token_endpoint.c_str(), curl_easy_strerror(res), res);

      return false;
    }

    Dmsg0(0, "Result: %s\n", writer.s.c_str());

    // todo: we need to handle "slow down" messages, by adjusting
    //    the poll interval in device_authentication
    return ParseMessage(writer.s, token);
  }

  site_metadata fetch_metadata(CURL* curl)
  {
    curl_easy_setopt(curl, CURLOPT_URL,
                     fmt::format("https://login.microsoftonline.com/{}/v2.0/"
                                 ".well-known/openid-configuration",
                                 tenant_id)
                         .c_str());

    string_writer sw;
    auto res = curl_easy_write_cb(curl, sw);

    if (res != CURLE_OK) {
      throw std::runtime_error{"could not fetch metadata"};
    }

    curl_easy_reset(curl);

    json_error_t err = {};
    json_t* json = json_loadb(sw.s.data(), sw.s.size(), 0, &err);
    if (!json || !json_is_object(json)) {
      throw std::runtime_error{"bad json"};
    }

    const char* token_endpoint{};
    const char* jwks_uri{};
    const char* issuer{};
    const char* device_authorization_endpoint{};

    if (json_unpack_ex(json, &err, 0, "{s:s, s:s, s:s, s:s}", "token_endpoint",
                       &token_endpoint, "jwks_uri", &jwks_uri, "issuer",
                       &issuer, "device_authorization_endpoint",
                       &device_authorization_endpoint)
        < 0) {
      throw std::runtime_error{std::string{"missing values in json object: "}
                               + sw.s};
    }

    return {token_endpoint, jwks_uri, issuer, device_authorization_endpoint};
  }

  DeviceAuthRequest start_device_auth(CURL* curl, const site_metadata& meta)
  {
    curl_easy_setopt(curl, CURLOPT_URL,
                     meta.device_authorization_endpoint.c_str());
    string_writer sw;
    auto res = [&] {
      url_encoder encoder{};

      encoder.add_kv("client_id", client_id.c_str());
      encoder.add_kv("scope", (scoped_uri + " openid offline_access").c_str());

      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
      curl_easy_setopt(curl, CURLOPT_POST, 1L);

      return curl_easy_write_cb(curl, sw);
    }();

    curl_easy_reset(curl);

    if (res != CURLE_OK) {
      throw std::runtime_error{
          (std::stringstream{} << "could not start device auth: "
                               << curl_easy_strerror(res) << " (" << res << ")")
              .str()};
    }

    Dmsg0(0, "Result: %s\n", sw.s.c_str());

    json_error_t err = {};
    json_t* json = json_loadb(sw.s.data(), sw.s.size(), 0, &err);
    if (!json) {
      throw std::runtime_error{std::string{"Could not parse response as json: "}
                               + err.text};
    }


    const char* user_code;
    const char* device_code;
    const char* verification_uri;
    json_int_t expires_in;
    json_int_t poll_interval;
    const char* message;

    if (json_unpack_ex(json, &err, 0, "{s:s, s:s, s:s, s:i, s:i, s:s}",
                       "user_code", &user_code, "device_code", &device_code,
                       "verification_uri", &verification_uri, "expires_in",
                       &expires_in, "interval", &poll_interval, "message",
                       &message)
        < 0) {
      throw std::runtime_error{
          std::string{"missing values in device auth response: "} + err.text};
    }

    return DeviceAuthRequest{user_code,
                             device_code,
                             verification_uri,
                             std::chrono::seconds{expires_in},
                             std::chrono::seconds{poll_interval},
                             message};
  }
};

directordaemon::ConsoleResource* AuthenticateConnection(
    BareosSocket* conn,
    ConfigurationParser* parser)
{
  if (!conn) { return nullptr; }
  CURL* curl = curl_easy_init();
  if (!curl) { return nullptr; }

  directordaemon::ConsoleResource* result = nullptr;
  try {
    // auto env = [](const char* name){
    //   auto* value = getenv(name);
    //   if (!value) { throw std::runtime_error{std::string{"not set: "} +
    //   name}; } return value;
    // };
    config cfg = {};

    auto meta = cfg.fetch_metadata(curl);
    auto keys = get_keys(curl, meta.jwks_uri, meta.issuer);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    bearer_token token;

    auto start = std::chrono::steady_clock::now();
    auto device_authentication = cfg.start_device_auth(curl, meta);

    if (!conn->fsend("oidc auth %s %s\n",
                     device_authentication.verification_uri.c_str(),
                     device_authentication.user_code.c_str())) {
      throw std::runtime_error{"Could not send code to client"};
    }

    auto auth_timeout = start + device_authentication.expires_in;
    for (;;) {
      if (std::chrono::steady_clock::now() >= auth_timeout) {
        throw std::runtime_error{"auth request timed out"};
      }

      if (cfg.fetch_token(curl, meta, device_authentication, &token)) { break; }

      Dmsg0(100, "Fetching token failed, retrying in %ld seconds\n",
            device_authentication.poll_interval.count());
      std::this_thread::sleep_for(device_authentication.poll_interval);
    }

    // we should not log the token itself!
    Dmsg0(200, "Got token with scope %s\n", token.scope.c_str());

    std::string_view scope = token.scope;

    if (scope != cfg.scoped_uri) {
      throw std::runtime_error{
          "received token with bad scope: expected:'" + cfg.scoped_uri
              + "' got:'" + std::string{scope} + "'",
      };
    }

    // an jwt token conists of three parts:
    // header.payload.signature

    auto [value, signature] = ExtractSignature(token.value);
    auto [header, payload] = ExtractJwt(value);

    if (auto iter = keys.find(header.key_id); iter != keys.end()) {
      const auto& used_key = iter->second;
      // we need to check that value matches signature via used_key

      if (!verify_signature(value, signature, used_key)) {
        throw std::runtime_error{"bad signature! rejecting jwt"};
      }

    } else {
      throw std::runtime_error{"unknown key id! rejecting jwt"};
    }

    // TODO: allow skew of ~30 sec
    if (std::chrono::system_clock::now().time_since_epoch().count()
        < payload.issued_at) {
      throw std::runtime_error{"token does not exist yet"};
    }

    if (std::chrono::system_clock::now().time_since_epoch().count()
        < payload.not_before) {
      throw std::runtime_error{"token not ready yet"};
    }

    if (payload.expiry
        >= std::chrono::system_clock::now().time_since_epoch().count()) {
      throw std::runtime_error{"token expired"};
    }

    if (payload.audience != cfg.app_uri) {
      throw std::runtime_error{"this token is not for me"};
    }

    if (payload.scope != cfg.app_scope) {
      throw std::runtime_error{"this token is for a different use"};
    }

    std::stringstream msg;
    msg << "Authenticating " << payload.user_name << " (" << payload.user_id
        << ")\n"
        << "with roles:\n";

    for (auto& role : payload.roles) { msg << " - " << role << "\n"; }

    Dmsg0(100, "%s\n", msg.str().c_str());


    result = new directordaemon::ConsoleResource{};
    result->resource_name_ = strdup(payload.user_name.data());
    result->use_pam_authentication_ = false;
    result->description_ = strdup("Authenticated via oidc");

    auto find_profile = [&](const char* name) {
      return dynamic_cast<directordaemon::ProfileResource*>(
          parser->GetResWithName(directordaemon::R_PROFILE, name, 0));
    };

    result->user_acl.corresponding_resource = result;
    result->user_acl.profiles
        = new alist<directordaemon::ProfileResource*>(10, not_owned_by_alist);
    for (auto& role : payload.roles) {
      auto* profile = find_profile(role.c_str());
      if (profile) { result->user_acl.profiles->push(profile); }
    }

    return result;
  } catch (const std::exception& ex) {
    Dmsg0(100, "Caught exception: %s\n", ex.what());
  } catch (...) {
  }
  curl_easy_cleanup(curl);
  return result;
}
};  // namespace oidc

#if 0
int main(int argc, const char* argv[])
{
  CLI::App app;

  std::string tok{};

  app.add_option("--tenant-id", tenant_id)->required()->envname("TENANT_ID");
  app.add_option("--client-id", client_id)->required()->envname("CLIENT_ID");
  app.add_option("--client-secret", client_secret)
      ->required()
      ->envname("CLIENT_SECRET");
  app.add_option("--app-uri", app_uri)->required()->envname("APP_URI");
  app.add_option("--app-scope", app_scope)->envname("APP_SCOPE");
  app.add_option("--token", tok);

  CLI11_PARSE(app, argc, argv);

  auto scoped_uri = app_uri + "/" + app_scope;

  CURL* curl = curl_easy_init();

  if (!curl) { std::cerr << "Could not initialize curl" << std::endl; }

  auto meta = fetch_metadata(curl);
  auto keys = get_keys(curl, meta.jwks_uri, meta.issuer);

  std::cerr << "KEYS:" << std::endl;
  for (const auto& [id, _] : keys) { std::cerr << " - " << id << std::endl; }

  curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  curl_easy_setopt(curl, CURLOPT_URL,
                   meta.device_authorization_endpoint.c_str());
  auto start = std::chrono::steady_clock::now();
  string_writer sw;
  auto res = [&] {
    url_encoder encoder{};

    encoder.add_kv("client_id", client_id);
    encoder.add_kv("scope", scoped_uri);
    // encoder.add_kv("client_secret", client_secret);
    // encoder.add_kv("grant_type", "client_credentials");

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    return curl_easy_write_cb(curl, sw);
  }();


  curl_easy_reset(curl);

  if (res != CURLE_OK) {
    std::cerr << "Could not fetch device" << std::endl;
    return 1;
  }

  json_error_t err = {};
  json_t* json = json_loadb(sw.s.data(), sw.s.size(), 0, &err);

  if (!json) {
    std::cerr << "Response is not json: '" << sw.s << "'" << std::endl;
    return 1;
  }

  // std::cout << json_dumps(json, 0) << std::endl;

  // TODO: do proper checking of these values
  std::string user_code = json_string_value(json_object_get(json, "user_code"));
  std::string device_code
      = json_string_value(json_object_get(json, "device_code"));
  std::string verification_uri
      = json_string_value(json_object_get(json, "verification_uri"));
  std::chrono::seconds expires_in{
      json_integer_value(json_object_get(json, "expires_in"))};
  std::chrono::seconds poll_interval{
      json_integer_value(json_object_get(json, "interval"))};
  std::string message = json_string_value(json_object_get(json, "message"));

  std::cout << "URL:  " << verification_uri << std::endl;
  std::cout << "CODE: " << user_code << std::endl;

  bearer_token token;

  if (tok.empty()) {
    for (;;) {
      if (std::chrono::steady_clock::now() - start >= expires_in) {
        std::cout << "Too slow ..." << std::endl;
        return 1;
      }

      curl_easy_setopt(curl, CURLOPT_URL, meta.token_endpoint.c_str());

      url_encoder encoder{};
      encoder.add_kv("client_id", client_id);
      encoder.add_kv("device_code", device_code);
      encoder.add_kv("grant_type",
                     "urn:ietf:params:oauth:grant-type:device_code");
      // encoder.add_kv("client_secret", client_secret);

      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
      curl_easy_setopt(curl, CURLOPT_POST, 1L);

      string_writer writer;
      auto res2 = curl_easy_write_cb(curl, writer);

      curl_easy_reset(curl);

      if (res2 == CURLE_OK) {
        if (ParseMessage(writer.s, &token)) { break; }
        std::cout << "Got: " << writer.s << std::endl;
      } else {
        std::cout << "Got err: " << writer.s << " (" << res2 << ")"
                  << std::endl;
      }


      std::cerr << "Retrying in " << poll_interval << " seconds" << std::endl;
      std::this_thread::sleep_for(poll_interval);
    }

    std::cout << token.value << " | " << token.scope << std::endl;

    std::string_view scope = token.scope;

    if (scope != scoped_uri) {
      std::cerr << "bad scope" << std::endl;
      return 1;
    }
  } else {
    token.value = tok;
  }

  // an jwt token conists of three parts:
  // header.payload.signature

  auto [value, signature] = ExtractSignature(token.value);
  auto [header, payload] = ExtractJwt(value);

  if (auto iter = keys.find(header.key_id); iter != keys.end()) {
    const auto& used_key = iter->second;
    // we need to check that value matches signature via used_key

    if (!verify_signature(value, signature, used_key)) {
      std::cerr << "bad signature! rejecting key" << std::endl;
    }

  } else {
    std::cerr << "key not available" << std::endl;
    return 1;
  }

  // TODO: allow skew of ~30 sec
  if (std::chrono::system_clock::now().time_since_epoch().count()
      < payload.issued_at) {
    std::cerr << "token does not exist yet" << std::endl;
  }

  if (std::chrono::system_clock::now().time_since_epoch().count()
      < payload.not_before) {
    std::cerr << "token not ready yet" << std::endl;
  }

  if (payload.expiry
      >= std::chrono::system_clock::now().time_since_epoch().count()) {
    std::cerr << "token expired" << std::endl;
  }

  if (payload.audience != app_uri) {
    std::cerr << "this token is not for me" << std::endl;
  }

  if (payload.scope != app_scope) {
    std::cerr << "this token is for a different use" << std::endl;
  }

  std::cout << "Hello " << payload.user_name << " (" << payload.user_id << ")"
            << std::endl;
  std::cout << "Your roles:" << std::endl;
  for (auto& role : payload.roles) { std::cout << " - " << role << std::endl; }

  curl_easy_cleanup(curl);
}
#endif
