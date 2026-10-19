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

#include "qrcodegen.h"

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
                     "id_token", &access_token, "token_type", &token_type,
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
    case INITIAL: {
      json_error_t err{};
      json_ptr received_json{
          json_loadb(received.data(), received.size(), 0, &err)};
      if (!received_json) {
        return Error(std::string{"received bad json request: "} + err.text);
      }

      json_t* flows_json{};
      if (json_unpack_ex(received_json.get(), &err, 0, "{s:o}",
                         "supported_auth_flows", &flows_json)
          < 0) {
        return Error(std::string{"received bad json: "} + err.text);
      }
      if (!json_is_array(flows_json)) {
        return Error(std::string{
            "received bad json request: supported_auth_flows is not an array"});
      }

      bool device_flow_found = false;

      json_int_t index{};
      json_t* flow_json;
      json_array_foreach(flows_json, index, flow_json)
      {
        if (!json_is_string(flow_json)) {
          return Error(
              std::string{"received bad json request: supported_auth_flows is "
                          "not a string array"});
        }

        std::string flow{json_string_value(flow_json),
                         json_string_length(flow_json)};
        if (flow == "device") {
          device_flow_found = true;
          break;
        }
      }

      if (!device_flow_found) {
        return Error(
            std::string{"received bad json request: supported_auth_flows "
                        "contains no known flows"});
      }

      std::string_view selected_flow = "device";
      to_send.assign(selected_flow.begin(), selected_flow.end());

      Dmsg0(200, "Selected oidc auth flow: %.*s\n", (int)selected_flow.size(),
            selected_flow.data());

      current_state = WAITING_FOR_CHALLENGE;

      return true;

    } break;
    case WAITING_FOR_CHALLENGE: {
      json_error_t err{};
      json_ptr received_json{
          json_loadb(received.data(), received.size(), 0, &err)};
      if (!received_json) {
        return Error(std::string{"received bad json: "} + err.text);
      }

      const char* url;
      const char* code;
      const char* complete_url = nullptr;
      if (json_unpack_ex(received_json.get(), &err, 0, "{s:s, s:s, s?s}", "url",
                         &url, "code", &code, "complete_url", &complete_url)
          < 0) {
        return Error(std::string{"received bad json request: "} + err.text);
      }

      const char* url_to_use = complete_url;
      if (!url_to_use) { url_to_use = url; }

      ASSERT(url_to_use);

      auto url_qr = qrcodegen::QrCode::encodeText(
          url_to_use, qrcodegen::QrCode::Ecc::MEDIUM);

      // qr codes are always quadratic
      auto size = url_qr.getSize();

      std::string url_qr_printed;

      // each module is printed as a 1x2 block (so its more square in a
      // terminal) We also leave 4 modules space around the qr code, so that it
      // is easier to catch for qr readers.

      url_qr_printed.reserve(
          size * (size * 2) /* actual module matrix */
          + 8               /* 8 empty lines (4 + 4) */
          + size            /* new lines in matrix */
          + 2 * 4 * size    /* for empty modules at the start of each line */
      );


      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < size + 8; ++x) {
          url_qr_printed.push_back('#');
          url_qr_printed.push_back('#');
        }
        url_qr_printed.push_back('\n');
      }

      for (int y = 0; y < size; ++y) {
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');

        for (int x = 0; x < size; ++x) {
          if (url_qr.getModule(x, y)) {
            url_qr_printed.push_back(' ');
            url_qr_printed.push_back(' ');
          } else {
            url_qr_printed.push_back('#');
            url_qr_printed.push_back('#');
          }
        }

        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('#');
        url_qr_printed.push_back('\n');
      }

      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < size + 8; ++x) {
          url_qr_printed.push_back('#');
          url_qr_printed.push_back('#');
        }
        url_qr_printed.push_back('\n');
      }

      printf("%s\n", url_qr_printed.c_str());


      if (complete_url) {
        printf(
            "Please visit %s to verify.\nAlternatively head to %s and enter "
            "code %s\n",
            complete_url, url, code);
      } else {
        printf("Please visit %s to verify.\nThe authorization code is %s\n",
               url, code);
      }

      current_state = RESPONSE_SENT;
      return true;
    } break;
    default: {
      return Error("Entered step() on bad state");
    } break;
  }
}
};  // namespace auth::Oidc
