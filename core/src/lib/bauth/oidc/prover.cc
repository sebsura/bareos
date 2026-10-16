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

#include "prover.h"
#include "common.h"

#include <sstream>
#include <memory>
#include <thread>

#include <curl/curl.h>
#include <curl/easy.h>
#include <jansson.h>

namespace auth::Oidc {
namespace {
struct DeviceAuthRequest {
  std::string client_id;

  std::string user_code;
  std::string device_code;
  std::string verification_uri;

  // this is an optional part of RFC 8628.  Its a verification uri that
  // has the device code encoded inside of it.
  std::optional<std::string> verification_uri_complete;
  std::chrono::seconds expires_in;
  std::chrono::seconds poll_interval;
  std::string message;
};

// See RFC 6749 Section 5.2 & RFC 8628 Section 3.5
enum class FetchResult
{
  Ok,
  Pending,
  SlowDown,
  AccessDenied,
  Expired,
  InvalidRequest,
  InvalidClient,
  InvalidGrant,
  UnauthorizedClient,
  UnsupportedGrantType,
  InvalidScope,
  Error,
};

static constexpr std::pair<FetchResult, std::string_view> error_table[] = {
    // RFC 8628 §3.5 — device-flow-specific errors
    {FetchResult::Pending, "authorization_pending"},
    {FetchResult::SlowDown, "slow_down"},
    {FetchResult::AccessDenied, "access_denied"},
    {FetchResult::Expired, "expired_token"},

    // RFC 6749 §5.2 — standard token-endpoint errors
    {FetchResult::InvalidRequest, "invalid_request"},
    {FetchResult::InvalidClient, "invalid_client"},
    {FetchResult::InvalidGrant, "invalid_grant"},
    {FetchResult::UnauthorizedClient, "unauthorized_client"},
    {FetchResult::UnsupportedGrantType, "unsupported_grant_type"},
    {FetchResult::InvalidScope, "invalid_scope"},

    // It seems some providers deviate from the standard.
    // We can add them below once we know of them
};

FetchResult handle_error_response(std::string_view buffer,
                                  std::string* err_desc)
{
  json_error_t err = {};
  json_ptr buffer_json{json_loadb(buffer.data(), buffer.size(), 0, &err)};

  if (!buffer_json) {
    *err_desc = "Error response was not json: ";
    *err_desc += err.text;
    return FetchResult::Error;
  }

  // These are required
  const char* error;

  // These are optional
  const char* error_desc{nullptr};
  const char* error_uri{nullptr};

  if (json_unpack_ex(buffer_json.get(), &err, 0, "{s:s, s?s, s?s}", "error",
                     &error, "error_description", &error_desc, "error_uri",
                     &error_uri)
      < 0) {
    *err_desc = "Error response could not be unpacked: ";
    *err_desc += err.text;
    return FetchResult::Error;
  }

  auto error_code = FetchResult::Error;

  Dmsg0(100, "Received error %s from token provider\n", error);

  for (auto [code, name] : error_table) {
    if (name == error) {
      error_code = code;
      break;
    }
  }

  if (error_desc) {
    err_desc->assign(error_desc);
  } else {
    err_desc->clear();
  }

  return error_code;
}

FetchResult handle_token_response(std::string_view buffer, std::string* token)
{
  json_error_t err = {};
  json_ptr buffer_json{json_loadb(buffer.data(), buffer.size(), 0, &err)};

  if (!buffer_json) {
    *token = "Token response was not json: ";
    *token += err.text;
    return FetchResult::Error;
  }

  // These are required
  const char* access_token;
  const char* token_type;

  // These are optional
  json_int_t expires_in{0};
  const char* refresh_token{nullptr};
  const char* scope{nullptr};

  if (json_unpack_ex(buffer_json.get(), &err, 0, "{s:s, s:s, s?i, s?s, s?s}",
                     "access_token", &access_token, "token_type", &token_type,
                     "expires_in", &expires_in, "refresh_token", &refresh_token,
                     "scope", &scope)
      < 0) {
    *token = "Error response could not be unpacked: ";
    *token += err.text;
    return FetchResult::Error;
  }

  if (!Bstrcasecmp(token_type, "Bearer")) {
    *token = "Bad token type: ";
    *token += token_type;
    return FetchResult::Error;
  }

  *token = access_token;
  return FetchResult::Ok;
}

FetchResult fetch_token(CURL* curl,
                        const site_metadata& meta,
                        DeviceAuthRequest& device_authentication,
                        std::string* response)
{
  curl_easy_setopt(curl, CURLOPT_URL, meta.token_endpoint.c_str());

  url_encoder encoder{};
  encoder.add_kv("client_id", device_authentication.client_id);
  encoder.add_kv("device_code", device_authentication.device_code);
  encoder.add_kv("grant_type", "urn:ietf:params:oauth:grant-type:device_code");

  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);

  std::string buffer;
  auto res = curl_easy_write_into_str(curl, &buffer);

  if (res != CURLE_OK) {
    Dmsg0(100, "Could not fetch from %s: %s (%d)\n",
          meta.token_endpoint.c_str(), curl_easy_strerror(res), res);
    curl_easy_reset(curl);
    return FetchResult::Error;
  }

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_easy_reset(curl);

  if (http_code >= 400) {
    Dmsg0(500, "Got an error response\n");
    return handle_error_response(buffer, response);
  } else {
    Dmsg0(500, "Got a success response\n");
    return handle_token_response(buffer, response);
  }
}

DeviceAuthRequest start_device_auth(CURL* curl,
                                    const std::string& client_id,
                                    const std::string& scope,
                                    const site_metadata& meta)
{
  Dmsg0(200, "Starting device auth via %s\n",
        meta.device_authorization_endpoint.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,
                   meta.device_authorization_endpoint.c_str());
  std::string response;
  auto res = [&] {
    url_encoder encoder{};

    encoder.add_kv("client_id", client_id.c_str());

    std::string scopes = scope;
    // if (!refresh_scope.empty()) { scopes += " " + refresh_scope; }

    encoder.add_kv("scope", scopes.c_str());

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoder.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    return curl_easy_write_into_str(curl, &response);
  }();

  curl_easy_reset(curl);

  if (res != CURLE_OK) {
    throw std::runtime_error{(std::stringstream{}
                              << "could not start device auth: "
                              << curl_easy_strerror(res) << " (" << res << ")")
                                 .str()};
  }

  Dmsg0(200, "GET was successful\n");

  json_error_t err = {};
  json_ptr json{json_loadb(response.data(), response.size(), 0, &err)};
  if (!json) {
    throw std::runtime_error{std::string{"Could not parse response as json: "}
                             + err.text};
  }


  const char* user_code;
  const char* device_code;
  const char* verification_uri;
  json_int_t expires_in;
  const char* message{""};
  json_int_t poll_interval{5};  // optional - default MUST be 5 per RFC
  const char* verification_uri_complete_{nullptr};  // optional

  if (json_unpack_ex(json.get(), &err, 0, "{s:s, s:s, s:s, s:i, s?s, s?i, s?s}",
                     "user_code", &user_code, "device_code", &device_code,
                     "verification_uri", &verification_uri, "expires_in",
                     &expires_in, "message", &message, "interval",
                     &poll_interval, "verification_uri_complete",
                     &verification_uri_complete_)
      < 0) {
    throw std::runtime_error{
        std::string{"missing values in device auth response: "} + err.text};
  }

  Dmsg0(200, "Unpack of GET was successful\n");

  std::optional<std::string> verification_uri_complete;
  if (verification_uri_complete_) {
    verification_uri_complete.emplace(verification_uri_complete_);
  }

  return DeviceAuthRequest{client_id,
                           user_code,
                           device_code,
                           verification_uri,
                           std::move(verification_uri_complete),
                           std::chrono::seconds{expires_in},
                           std::chrono::seconds{poll_interval},
                           message};
}

void print_user_message(const DeviceAuthRequest& device_auth)
{
  if (device_auth.verification_uri_complete) {
    printf("Please visit %s to verify.\n",
           device_auth.verification_uri_complete->c_str());
  } else {
    printf("Please visit %s to verify.\nThe authorization code is %s\n",
           device_auth.verification_uri.c_str(), device_auth.user_code.c_str());
  }
}
};  // namespace

bool Prover::step(std::vector<char>& to_send, std::span<const char> received)
{
  switch (current_state) {
    case WAITING_FOR_CHALLENGE: {
      auto data = Challenge::parse(received);
      if (!data) {
        current_state = ERROR;
        error = "Could not parse challenge";
        return false;
      }

      std::string key_str;
      curl_ptr curl{curl_easy_init()};

      std::string metadata_url = data->url;
      if (!metadata_url.ends_with('/')) { metadata_url.push_back('/'); }
      metadata_url += ".well-known/openid-configuration";

      try {
        auto meta = fetch_metadata(curl.get(), metadata_url.c_str());

        auto now = std::chrono::steady_clock::now;
        auto start_time = now();
        auto device_auth
            = start_device_auth(curl.get(), data->audience, data->scope, meta);

        print_user_message(device_auth);

        auto poll_interval = device_auth.poll_interval;
        bool done = false;
        while (!done) {
          auto delta = now() - start_time;

          if (delta >= device_auth.expires_in) {
            current_state = ERROR;
            error = "Device auth expiered";
            Dmsg0(100, "request expired: %zu ns vs max %zu ns\n",
                  std::chrono::duration_cast<std::chrono::nanoseconds>(delta)
                      .count(),
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      device_auth.expires_in)
                      .count());
            return false;
          }

          std::this_thread::sleep_for(poll_interval);

          switch (fetch_token(curl.get(), meta, device_auth, &key_str)) {
            case FetchResult::Ok: {
              Dmsg0(200, "token was fetched sucessfully\n");
              done = true;
            } break;
            case FetchResult::Pending: {
              Dmsg0(300, "token is not ready yet\n");
            } break;
            case FetchResult::SlowDown: {
              Dmsg0(300, "token is not ready yet; we need to slow down\n");

              // we are required by the standard to (permanently) add 5 seconds
              poll_interval += std::chrono::seconds(5);
            } break;
            default: {
              if (key_str.empty()) { key_str = "unknown error"; }
              auto* err_desc = key_str.c_str();

              Dmsg0(100, "We received an error during token fetch: %s\n",
                    err_desc);

              error = std::move(key_str);
              current_state = ERROR;
              return false;
            } break;
          }
        }
      } catch (const std::exception& ex) {
        error = ex.what();
        current_state = ERROR;
        return false;
      }

      KeyResponse response{key_str};
      auto response_str = response.format();

      to_send.assign(key_str.begin(), key_str.end());
      current_state = RESPONSE_SENT;
      return true;
    } break;
    default: {
      current_state = ERROR;
      if (error.empty()) { error = "Entered step() on bad state"; };
      return false;
    } break;
  }
}
};  // namespace auth::Oidc
