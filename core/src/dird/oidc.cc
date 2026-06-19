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
#include "CLI/App.hpp"
#include "CLI/Config.hpp"
#include "CLI/Formatter.hpp"
#include "CLI/ExtraValidators.hpp"

#include <curl/curl.h>
#include <curl/easy.h>
#include <dirent.h>
#include <fmt/format.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <openssl/bn.h>
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
#include <stdexcept>
#include <thread>
#include <variant>

#if 0

struct string_writer {
  std::string s;

  size_t operator()(const char* data, size_t size)
  {
    s.insert(s.end(), data, data + size);
    return size;
  }
};

std::string_view json_string_view(json_t* json)
{
  assert(json_is_string(json));

  return std::string_view{json_string_value(json), json_string_length(json)};
}

template <typename... Args>
json_t* json_string_format(fmt::format_string<Args...> fmt, Args... args)
{
  auto formatted = fmt::format(fmt, std::forward<Args>(args)...);

  return json_stringn(formatted.c_str(), formatted.size());
}

struct resource_deleter {
  void operator()(json_t* json) const { json_decref(json); }

  void operator()(CURL* curl) const { curl_easy_cleanup(curl); }

  void operator()(char* str) const { free(str); }
};

template <typename T> struct ptr : std::unique_ptr<T, resource_deleter> {
  using base = std::unique_ptr<T, resource_deleter>;
  template <typename... Args>
  ptr(Args&&... args) : base(std::forward<Args>(args)...)
  {
  }

  operator T*() { return base::get(); }
};

template <typename T> ptr(T*) -> ptr<T>;

bool read_file(std::vector<char>& s, const char* path)
{
  std::size_t total_bytes_read = 0;
  FILE* f = fopen(path, "r");
  if (!f) {
    std::cerr << "error opening file " << path << ": " << strerror(errno)
              << std::endl;
    return false;
  }

  while (total_bytes_read < s.size()) {
    auto bytes_read
        = fread(s.data() + total_bytes_read, 1, s.size() - total_bytes_read, f);

    if (bytes_read == 0) {
      if (ferror(f)) {
        std::cerr << "error reading file " << path << ": " << strerror(errno)
                  << std::endl;
        return false;
      }
      assert(feof(f));
      break;
    }

    total_bytes_read += bytes_read;
  }

  fclose(f);

  return total_bytes_read == s.size();
}


struct fetch_token_data {
  bool execute()
  {
    std::optional token = request_token();

    if (!token) {
      std::cerr << "no token could be requested" << std::endl;
      return false;
    }

    std::cout << token->value << std::endl;

    return true;
  }
};

bool parse_id(json_t* json, json_int_t* id)
{
  // we give the graph api numbers as ids
  // graph api decides to return them as strings instead,
  // so we need to reparse them here

  if (json_is_integer(json)) {
    *id = json_integer_value(json);
    return true;
  }

  if (!json_is_string(json)) { return false; }

  const char* str = json_string_value(json);

  auto result = std::from_chars(str, str + json_string_length(json), *id);

  if (result.ec != std::errc{}) { return false; }
  if (result.ptr != str + json_string_length(json)) { return false; }

  return true;
}


auto continue_with(const auto& cmd, std::string link)
{
  auto cpy{cmd};

  cpy.continue_from = std::move(link);

  return cpy;
}

struct fetch_sites {
  std::string continue_from{};
};
struct fetch_drives_from_site {
  std::string name;
  std::string id;
  std::string continue_from{};
};

struct fetch_items_from_drive {
  std::string drive_name;
  std::string drive_id;
  std::string path;
  std::string destination;
  std::string continue_from{};
};

struct download_item {
  std::string download_url;
  std::string renew_url;    // to request a new download url
  std::string system_path;  // where to download the file to
  // std::string id;
  std::size_t size;

  std::size_t current_offset{};
};

struct renew_download_url {
  download_item item;
};

struct setup_download_item {
  std::string item_url;
  std::string system_path;
  bool folder{false};
  size_t size;
};

struct get_list_item {
  std::string system_path;
  std::string url;
};


using command = std::variant<fetch_sites,
                             fetch_drives_from_site,
                             fetch_items_from_drive,
                             renew_download_url,
                             setup_download_item,
                             get_list_item>;

json_t* GET = json_string("GET");
json_t* PUT = json_string("PUT");

struct downloader {
  bearer_token tk = {};
  std::vector<command> to_execute;
  std::vector<command> next_commands;

  using Clock = std::chrono::steady_clock;

  std::optional<Clock::time_point> wait_until;

  std::size_t expiry_offset{5};
  std::size_t max_batch_size{20};
  std::size_t worker_count{8};
  std::size_t maximum_chunk_size{size_t{10} << 20};


  std::string directory{"/tmp/m365"};

  bool no_download{false};

  void register_options(CLI::App* app)
  {
    app->add_option("--directory", directory,
                    "Defines the name of the output directory");
    app->add_option("--max-batch-size", max_batch_size,
                    "Defines the maximum number of requests in a single batch");
    app->add_option("--expiry-offset", expiry_offset,
                    "Defines how many seconds before the token expiry we "
                    "request a new one");
    app->add_option("--file-downloaders", worker_count,
                    "Defines how many download workers to create")
        ->check(CLI::Range(size_t{1}, std::numeric_limits<size_t>::max()));

    app->add_option("--chunk-size", maximum_chunk_size,
                    "Defines how many bytes per chunk to fetch")
        ->check(CLI::Range(size_t{1}, std::numeric_limits<size_t>::max()));

    app->add_flag("--dry", no_download, "Do not download files");
  }

  void wait_backoff()
  {
    if (!wait_until) { return; }

    std::cerr << "waiting for backoff" << std::endl;

    std::this_thread::sleep_until(*wait_until);

    wait_until.reset();
  }

  void setup()
  {
    states = std::make_unique<worker_state[]>(worker_count);
    file_downloaders.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
      file_downloaders.emplace_back(file_download_thread, this, i);
    }
  }

  ~downloader()
  {
    done = true;
    for (auto& t : file_downloaders) { t.join(); }
  }

  bool execute()
  {
    for (;;) {
      for (size_t i = 0; i < worker_count; ++i) {
        auto& worker = states[i];

        std::unique_lock l{worker.renew_mut};

        for (auto& item : worker.to_renew) {
          assert(!item.renew_url.empty());
          to_execute.emplace_back(renew_download_url{std::move(item)});
        }

        worker.to_renew.clear();
      }

      // check if we are done
      bool queue_empty = to_execute.empty();
      bool tasks_done = ongoing_task_count.load() == 0;


      if (queue_empty && tasks_done) { break; }

      if (queue_empty) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      auto batch = next_batch();

      wait_backoff();
      if (!execute_batch(batch)) { return false; }

      // remove executed commands
      to_execute.resize(to_execute.size() - batch.size());

      // add new commands
      to_execute.insert(to_execute.end(),
                        std::make_move_iterator(next_commands.begin()),
                        std::make_move_iterator(next_commands.end()));


      next_commands.clear();
    }

    return true;
  }

 private:
  std::atomic<size_t> ongoing_task_count;

  /* NOTES:
   * - we can limit the amount of information requested by using ?select=a,b,c
   *   so if we just need id & name, we can say ?select=id,name
   */
  void setup_request(json_t* req, const fetch_sites& cmd) const
  {
    if (cmd.continue_from.empty()) {
      std::cerr << "- fetch all sites" << std::endl;
      json_object_set_new(req, "url", json_string("/sites"));
    } else {
      std::cerr << "- continuing fetching all sites (" << cmd.continue_from
                << ")" << std::endl;
      json_object_set_new(req, "url", json_string(cmd.continue_from.c_str()));
    }
    json_object_set(req, "method", GET);
  }

  void setup_request(json_t* req, const fetch_drives_from_site& cmd) const
  {
    if (cmd.continue_from.empty()) {
      std::cerr << "- fetch drives from site \"" << cmd.name << "\" (" << cmd.id
                << ")" << std::endl;
      json_object_set_new(
          req, "url",
          json_string(fmt::format("/sites/{}/drives", cmd.id).c_str()));
    } else {
      std::cerr << "- continuing fetching drives from site \"" << cmd.name
                << "\" (" << cmd.continue_from << ")" << std::endl;
      json_object_set_new(req, "url", json_string(cmd.continue_from.c_str()));
    }
    json_object_set(req, "method", GET);
  }

  void setup_request(json_t* req, const fetch_items_from_drive& cmd) const
  {
    assert(cmd.destination.starts_with(directory));

    if (cmd.continue_from.empty()) {
      std::cerr << "- fetch items from drive \"" << cmd.drive_name << "\""
                << std::endl;

      url_encoder enc{};
      enc.add_kv("$select", "id,size,name,file,folder,parentReference");

      json_object_set_new(req, "url",
                          json_string(fmt::format("/drives/{}/root/delta?{}",
                                                  cmd.drive_id, enc.view())
                                          .c_str()));
    } else {
      std::cerr << "- continuing fetching items from drive \"" << cmd.drive_name
                << "\" (" << cmd.continue_from << ")" << std::endl;
      json_object_set_new(req, "url", json_string(cmd.continue_from.c_str()));
    }
    json_object_set(req, "method", GET);
  }

  void setup_request(json_t* req, const renew_download_url& cmd) const
  {
    std::cerr << "- renewing expired download url for \""
              << cmd.item.system_path << "\" (" << cmd.item.renew_url << ")"
              << std::endl;
    json_object_set_new(req, "url", json_string(cmd.item.renew_url.c_str()));
    json_object_set(req, "method", GET);
  }

  void setup_request(json_t* req, const setup_download_item& cmd) const
  {
    std::cerr << "- fetch data for file \"" << cmd.system_path << "\""
              << std::endl;
    url_encoder enc{};


    // we should also try expand=listItem again,
    // maybe it actually contained the 'fields' field
    enc.add_kv("$expand", "permissions");
    enc.add_kv("$select", "permissions,sharepointIds");
    if (0 && cmd.size > 0) {
      enc.add_kv("select", "@microsoft.graph.downloadUrl");
    }
    json_object_set_new(req, "url",
                        json_string_format("{}?{}", cmd.item_url, enc.view()));
    json_object_set(req, "method", GET);
  }

  void setup_request(json_t* req, const get_list_item& cmd) const
  {
    json_object_set_new(req, "url",
                        json_stringn(cmd.url.c_str(), cmd.url.size()));
    json_object_set(req, "method", GET);
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const get_list_item& cmd) const
  {
    (void)cmd;
    (void)body;
    (void)headers;
    (void)to_download;
    (void)follow_up_commands;

    std::cerr << "'" << cmd.system_path
              << "': " << json_dumps(body, JSON_COMPACT) << std::endl;

    exit(1);

    return true;
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const fetch_sites& cmd) const
  {
    (void)to_download;
    (void)headers;
    json_t* values = json_object_get(body, "value");

    if (!values || !json_is_array(values)) { return false; }

    size_t index;
    json_t* value;
    json_array_foreach(values, index, value)
    {
      if (!value || !json_is_object(value)) {
        std::cerr << "bad value '"
                  << (value ? json_dumps(value, JSON_COMPACT) : "<null>") << "'"
                  << std::endl;
        return false;
      }
      auto* id = json_object_get(value, "id");
      auto* name = json_object_get(value, "name");

      if (!id || !json_is_string(id)) {
        std::cerr << "bad id '"
                  << (value ? json_dumps(value, JSON_COMPACT) : "<null>") << "'"
                  << std::endl;
        return false;
      }

      const char* name_val = "";
      // name is optional, but if it exists it should be a string
      if (name) {
        if (!json_is_string(name)) {
          std::cerr << "bad name '"
                    << (value ? json_dumps(value, JSON_COMPACT) : "<null>")
                    << "'" << std::endl;
          return false;
        }

        name_val = json_string_value(name);
      }

      follow_up_commands.push_back(
          fetch_drives_from_site{name_val, json_string_value(id)});
    }

    if (json_t* json_next_link = json_object_get(body, "@odata.nextLink")) {
      std::string_view url{json_string_value(json_next_link),
                           json_string_length(json_next_link)};
      if (!url.starts_with(api_root)) {
        std::cerr << "bad nextLink url: does not start with \"" << api_root
                  << "\"" << std::endl;
      } else {
        url.remove_prefix(api_root.size());
        std::cerr << "handling next link \"" << url << "\"" << std::endl;
        follow_up_commands.emplace_back(continue_with(cmd, std::string{url}));
      }
    }

    return true;
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const fetch_drives_from_site& cmd) const
  {
    (void)to_download;
    (void)headers;
    json_t* values = json_object_get(body, "value");

    if (!values || !json_is_array(values)) { return false; }

    size_t index;
    json_t* value;
    json_array_foreach(values, index, value)
    {
      if (!value || !json_is_object(value)) {
        std::cerr << "bad value '"
                  << (value ? json_dumps(value, JSON_COMPACT) : "<null>") << "'"
                  << std::endl;
        return false;
      }
      auto* id = json_object_get(value, "id");
      auto* name = json_object_get(value, "name");

      if (!id || !json_is_string(id)) {
        std::cerr << "bad id '"
                  << (value ? json_dumps(value, JSON_COMPACT) : "<null>") << "'"
                  << std::endl;
        return false;
      }

      const char* name_val = "";
      // name is optional, but if it exists it should be a string
      if (name) {
        if (!json_is_string(name)) {
          std::cerr << "bad name '"
                    << (value ? json_dumps(value, JSON_COMPACT) : "<null>")
                    << "'" << std::endl;
          return false;
        }

        name_val = json_string_value(name);
      }

      follow_up_commands.emplace_back(fetch_items_from_drive{
          name_val, json_string_value(id), "root",
          fmt::format("{}/{}/{}", this->directory, cmd.name, name_val)});
    }

    if (json_t* json_next_link = json_object_get(body, "@odata.nextLink")) {
      std::string_view url{json_string_value(json_next_link),
                           json_string_length(json_next_link)};
      if (!url.starts_with(api_root)) {
        std::cerr << "bad nextLink url: does not start with \"" << api_root
                  << "\"" << std::endl;
      } else {
        url.remove_prefix(api_root.size());
        std::cerr << "handling next link \"" << url << "\"" << std::endl;
        follow_up_commands.emplace_back(continue_with(cmd, std::string{url}));
      }
    }
    return true;
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const fetch_items_from_drive& cmd) const
  {
    assert(cmd.destination.starts_with(directory));

    (void)to_download;
    (void)headers;
    json_t* values = json_object_get(body, "value");

    if (!values || !json_is_array(values)) { return false; }

    size_t index;
    json_t* value;
    json_array_foreach(values, index, value)
    {
      json_t* folder = json_object_get(value, "folder");
      json_t* file = json_object_get(value, "file");

      if (!folder && !file) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains item that is neither a folder nor a file ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;

        return false;
      }

      if (folder && file) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains item that is both a folder and a file ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;

        return false;
      } else if (!folder && !file) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains item that is neither a folder nor a file ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
      }

      // nothing to do ?
      // we should somehow tell the system that we need to create a directory
      // here

      json_t* json_id = json_object_get(value, "id");
      if (!json_id || !json_is_string(json_id)) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains folder with bad id ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return false;
      }

      std::string_view id{json_string_value(json_id),
                          json_string_length(json_id)};

      json_t* json_name = json_object_get(value, "name");
      if (!json_name || !json_is_string(json_name)) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains folder with bad name ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return false;
      }
      std::string_view name{json_string_value(json_name),
                            json_string_length(json_name)};

      json_t* json_parent_ref = json_object_get(value, "parentReference");
      if (!json_parent_ref || !json_is_object(json_parent_ref)) {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains folder with bad parentReference ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return false;
      }


      std::string_view parent_path;
      if (json_t* json_parent_path = json_object_get(json_parent_ref, "path");
          json_parent_path && json_is_string(json_parent_path)) {
        parent_path = std::string_view{json_string_value(json_parent_path),
                                       json_string_length(json_parent_path)};

        auto pos = parent_path.rfind(':');
        if (pos == parent_path.npos) {
          std::cerr << "\"" << cmd.drive_name
                    << "\" contains folder with bad parentReference->Path ("
                    << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
          return false;
        }
        parent_path.remove_prefix(pos + 1);

      } else if (name == "root") {
        // the root folder has the drive as its parent, and no parent path
        parent_path = {};
      } else {
        std::cerr << "\"" << cmd.drive_name
                  << "\" contains file with bad parentReference->Path ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return false;
      }

      size_t size = 0;

      auto full_name
          = fmt::format("{}{}/{}", cmd.destination, parent_path, name);

      if (file) {
        json_t* json_size = json_object_get(value, "size");
        if (!json_size || !json_is_integer(json_size)) {
          std::cerr << full_name << ": response does not contain 'size' ("
                    << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
          return false;
        }

        size = json_integer_value(json_size);
      }


      follow_up_commands.emplace_back(setup_download_item{
          fmt::format("/drives/{}/items/{}", cmd.drive_id, id), full_name,
          folder ? true : false, size});
    }

    if (json_t* json_next_link = json_object_get(body, "@odata.nextLink")) {
      std::string_view url{json_string_value(json_next_link),
                           json_string_length(json_next_link)};
      if (!url.starts_with(api_root)) {
        std::cerr << "bad nextLink url: does not start with \"" << api_root
                  << "\"" << std::endl;
      } else {
        url.remove_prefix(api_root.size());
        std::cerr << "handling next link \"" << url << "\"" << std::endl;
        follow_up_commands.emplace_back(continue_with(cmd, std::string{url}));
      }
    } else if (json_t* json_delta_link
               = json_object_get(body, "@odata.deltaLink")) {
      std::string_view url{json_string_value(json_delta_link),
                           json_string_length(json_delta_link)};
      std::cerr << "DRIVE \"" << cmd.drive_name << "\": delta link \"" << url
                << "\"" << std::endl;
    } else {
      std::cerr << "response contains neither delta nor next link" << std::endl;
    }

    return true;
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const renew_download_url& cmd) const
  {
    (void)headers;
    (void)follow_up_commands;

    std::cerr << json_dumps(body, JSON_COMPACT) << std::endl;
    exit(1);

    // json_t* json_id = json_object_get(body, "id");
    // if (!json_id || !json_is_string(json_id)) {
    //   std::cerr << "\"" << cmd.item.system_path
    //             << "\" has bad id during renew ("
    //             << json_dumps(body, JSON_COMPACT) << ")" << std::endl;
    //   return false;
    // }

    // std::string_view id{json_string_value(json_id),
    //                     json_string_length(json_id)};

    json_t* json_download
        = json_object_get(body, "@microsoft.graph.downloadUrl");
    if (!json_download || !json_is_string(json_download)) {
      std::cerr << "\"" << cmd.item.system_path
                << "\" has bad download link during renew ("
                << json_dumps(body, JSON_COMPACT) << ")" << std::endl;
      return false;
    }

    std::string_view download{json_string_value(json_download),
                              json_string_length(json_download)};


    // if (cmd.item.id != id) {
    //   std::cerr << "id of \"" << cmd.item.system_path << "\" changed ('"
    //             << cmd.item.id << "' => '" << id << "')" << std::endl;
    //   return false;
    // }

    download_item cpy{cmd.item};
    cpy.download_url.assign(download);

    to_download.emplace_back(std::move(cpy));

    return true;
  }

  bool handle_response(std::vector<command>& follow_up_commands,
                       std::vector<download_item>& to_download,
                       json_t* headers,
                       json_t* body,
                       const setup_download_item& cmd) const
  {
    (void)headers;
    (void)follow_up_commands;

    // json_t* permissions = json_object_get(body, "permissions");
    // if (!permissions || !json_is_array(permissions)) {
    //   std::cerr << cmd.system_path << ": response for setup-download-item
    //   does not contain 'permissions' (" << json_dumps(body, JSON_COMPACT) <<
    //   ")"
    //             << std::endl;
    //   //return false;
    // }

    json_t* sharepoint_ids = json_object_get(body, "sharepointIds");
    if (!sharepoint_ids || !json_is_object(sharepoint_ids)) {
      std::cerr
          << cmd.system_path
          << ": response for setup-download-item does not contain 'listItem' ("
          << json_dumps(body, JSON_COMPACT) << ")" << std::endl;
      // some items (like root) do not contain this, so ignore
    } else {
      json_t* list_id = json_object_get(sharepoint_ids, "listId");
      json_t* list_item_id
          = json_object_get(sharepoint_ids, "listItemUniqueId");
      json_t* site_id = json_object_get(sharepoint_ids, "siteId");
      // (void)index;
      // json_t* parent_reference = json_object_get(sharepoint_ids,
      // "parentReference"); json_t* site_id = json_object_get(parent_reference,
      // "siteId"); json_t* list_id = json_object_get(parent_reference, "id");

      if (!cmd.system_path.ends_with("/root")) {
        auto sid = json_string_view(site_id);
        auto lid = json_string_view(list_id);
        auto iid = json_string_view(list_item_id);

        auto url = fmt::format(
            "/sites/{}/lists/{}/items/{}?$expand=fields&$select=fields", sid,
            lid, iid);

        follow_up_commands.emplace_back(get_list_item{cmd.system_path, url});
      }
    }

    if (!cmd.folder && cmd.size > 0 && cmd.size == 0) {
      json_t* url = json_object_get(body, "@microsoft.graph.downloadUrl");
      if (!url || !json_is_string(url)) {
        std::cerr << cmd.system_path
                  << ": response for setup-download-item does not contain "
                     "'@microsoft.graph.downloadUrl' ("
                  << json_dumps(body, JSON_COMPACT) << ")" << std::endl;
        return false;
      }

      download_item item{};
      // item.id = ;
      item.renew_url
          = fmt::format("{}?select=@microsoft.graph.downloadUrl", cmd.item_url);
      item.size = cmd.size;
      item.system_path = cmd.system_path;
      item.download_url.assign(json_string_value(url), json_string_length(url));

      to_download.emplace_back(std::move(item));
    }

    return true;

    // size_t actual_permissions = 0;

    // size_t index;
    // json_t* value;
    // json_array_foreach(permissions, index, value)
    // {
    //   if (!json_is_object(value)) {
    //     std::cerr << "Bad return value at index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (not an object)"
    //               << std::endl;
    //     continue;
    //   }

    //   if (auto* inherited = json_object_get(value, "inheritedFrom");
    //       inherited && !json_is_null(inherited)) {
    //     // value is inherited, no need to save it
    //     continue;
    //   }


    //   json_t* roles{};

    //   // this is for "user-type" permissions
    //   // "link-type" permissions instead use "grantedToIdentitiesV2"
    //   json_t* grantedTo{};

    //   json_error_t err = {};

    //   if (json_unpack_ex(value, &err, 0, "{s:o,s:o}", "roles", &roles,
    //                      "grantedToV2", &grantedTo)
    //       < 0) {
    //     std::cerr << "failed to unpack json value at index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (" << err.text <<
    //               ")"
    //               << std::endl;
    //     continue;
    //   }

    //   std::string_view role;
    //   std::string_view id;

    //   if (!json_is_array(roles)) {
    //     std::cerr << "bad roles for index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (not an array)"
    //               << std::endl;
    //     continue;
    //   }

    //   if (json_array_size(roles) != 1) {
    //     std::cerr << "bad roles for index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (size > 1)"
    //               << std::endl;
    //     continue;
    //   }

    //   json_t* json_role = json_array_get(roles, 0);
    //   assert(json_role);
    //   if (!json_is_string(json_role)) {
    //     std::cerr << "bad roles for index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (type != string)"
    //               << std::endl;
    //     continue;
    //   }

    //   role = json_string_view(json_role);

    //   if (role == "owner") {
    //     // we cannot set this anyways, so there is probobly no need to record
    //     it
    //   }

    //   if (!json_is_object(grantedTo)) {
    //     std::cerr << "bad object for index " << index << ": "
    //               << json_dumps(value, JSON_COMPACT) << " (not an object)"
    //               << std::endl;
    //     continue;
    //   }

    //   {
    //     json_t* user = json_object_get(grantedTo, "user");
    //     // json_t* site_user = json_object_get(grantedTo, "siteUser");

    //     json_t* group = json_object_get(grantedTo, "group");
    //     // json_t* site_group = json_object_get(grantedTo, "siteGroup");

    //     json_t* device = json_object_get(grantedTo, "device");
    //     json_t* application = json_object_get(grantedTo, "application");

    //     if (device) {
    //       std::cerr << "Cannot handle device permission for path "
    //                 << cmd.system_path << ": "
    //                 << json_dumps(value, JSON_COMPACT) << std::endl;
    //       continue;
    //     }
    //     if (application) {
    //       std::cerr << "Cannot handle application permission for path "
    //                 << cmd.system_path << ": "
    //                 << json_dumps(value, JSON_COMPACT) << std::endl;
    //       continue;
    //     }

    //     if (user && group) {
    //       std::cerr << "Cannot handle user + group permission for path "
    //                 << cmd.system_path << ": "
    //                 << json_dumps(value, JSON_COMPACT) << std::endl;
    //       continue;
    //     } else if (user) {
    //       if (!json_is_object(user)) {
    //         std::cerr << "Bad permission for path " << cmd.system_path << ":
    //         "
    //                   << json_dumps(value, JSON_COMPACT)
    //                   << " (user is not an object)" << std::endl;
    //         continue;
    //       }

    //       json_t* user_id = json_object_get(user, "id");

    //       if (!user_id || !json_is_string(user_id)) {
    //         std::cerr << "Bad permission for path " << cmd.system_path << ":
    //         "
    //                   << json_dumps(value, JSON_COMPACT)
    //                   << " (user-id is not a string)" << std::endl;
    //         continue;
    //       }

    //       id = json_string_view(user_id);
    //     } else if (group) {
    //       if (!json_is_object(group)) {
    //         std::cerr << "Bad permission for path " << cmd.system_path << ":
    //         "
    //                   << json_dumps(value, JSON_COMPACT)
    //                   << " (group is not an object)" << std::endl;
    //         continue;
    //       }

    //       json_t* group_id = json_object_get(group, "id");

    //       if (!group_id || !json_is_string(group_id)) {
    //         std::cerr << "Bad permission for path " << cmd.system_path << ":
    //         "
    //                   << json_dumps(value, JSON_COMPACT)
    //                   << " (group-id is not a string)" << std::endl;
    //         continue;
    //       }

    //       id = json_string_view(group_id);
    //     } else {
    //       std::cerr << "Bad permission for path " << cmd.system_path << ": "
    //                 << json_dumps(value, JSON_COMPACT)
    //                 << " (only site permissions)" << std::endl;
    //       continue;
    //     }

    //     std::cerr << "File: " << cmd.system_path << "; Id: " << id
    //               << "; Role: " << role << std::endl;
    //     actual_permissions += 1;
    //   }
    // }

    // if (actual_permissions == 0) {
    //   std::cerr << "no backed up permissions for " << cmd.system_path
    //             << std::endl;
    //   return true;
    // }

    // if (json_t* json_next_link = json_object_get(permissions,
    // "@odata.nextLink")) {
    //   std::string_view url{json_string_value(json_next_link),
    //                        json_string_length(json_next_link)};
    //   if (!url.starts_with(api_root)) {
    //     std::cerr << "bad nextLink url: does not start with \"" << api_root
    //               << "\"" << std::endl;
    //   } else {
    //     url.remove_prefix(api_root.size());
    //     std::cerr << "handling next link \"" << url << "\"" << std::endl;
    //     auto cpy{cmd};
    //     cpy.item_url.assign(url);

    //     get_permissions get_perms;
    //     get_perms.url = url;
    //     get_perms.path = cmd.system_path;

    //     follow_up_commands.emplace_back(get_perms);
    //   }
    // }

    return true;
  }

  struct worker_state {
    std::mutex download_mut;
    std::mutex renew_mut;
    std::condition_variable new_item;
    std::vector<download_item> to_download;  // input
    std::vector<download_item> to_renew;     // output
  };
  std::unique_ptr<worker_state[]> states;
  std::vector<std::thread> file_downloaders;
  std::atomic<bool> done{false};

  static void make_path(std::string& path)
  {
    auto current = 1;
    for (;;) {
      auto pos = path.find('/', current);
      if (pos == path.npos) { break; }

      path[pos] = '\0';
      mkdir(path.c_str(), 0777);
      path[pos] = '/';

      current = pos + 1;
    }
  }

  static void file_download_thread(downloader* d, size_t index)
  {
    std::span<worker_state> states{d->states.get(), d->worker_count};
    worker_state& me = states[index];

    CURL* curl = curl_easy_init();
    if (!curl) { return; }

    auto error_buffer = std::make_unique<char[]>(CURL_ERROR_SIZE);


    std::size_t current_backoff = 0;
    std::size_t wait_for = 0;

    for (;;) {
      if (d->done.load()) { break; }

      if (wait_for == 0 && current_backoff > 0) {
        if (current_backoff > 50) {
          // this should _never_ happen.
          // otherwise have fun waiting ...
          current_backoff = 50;
        }

        std::cerr << "WORKER[" << index << "]: reached backoff level "
                  << current_backoff << std::endl;

        wait_for = 1L << current_backoff;
      }

      if (wait_for > 0) {
        std::cerr << "WORKER[" << index << "]: waiting " << wait_for
                  << " seconds for backoff" << std::endl;

        auto time_to_wait = std::chrono::seconds(wait_for);
        std::this_thread::sleep_for(time_to_wait);

        wait_for = 0;
      }

      download_item item = {};
      bool found_item = false;
      {
        std::unique_lock l{me.download_mut};

        if (me.to_download.empty()) {
          me.new_item.wait_for(l, std::chrono::milliseconds(1));

          l.unlock();

          for (auto& state : states) {
            if (&state == &me) { continue; }

            std::unique_lock other_lock{state.download_mut, std::try_to_lock};

            if (other_lock.owns_lock() && !state.to_download.empty()) {
              item = std::move(state.to_download.back());
              state.to_download.pop_back();
              assert(!item.renew_url.empty());
              found_item = true;
              break;
            }
          }
        } else {
          item = std::move(me.to_download.back());
          me.to_download.pop_back();
          assert(!item.renew_url.empty());
          found_item = true;
        }

        if (!found_item) { continue; }
      }

      assert(found_item);

      make_path(item.system_path);

      std::ios_base::openmode open_mode = std::ios::binary;

      if (item.current_offset > 0) {
        open_mode |= std::ios::app;
        std::cerr << "WORKER[" << index << "] Continuing with file \""
                  << item.system_path << "\"" << std::endl;
      } else {
        std::cerr << "WORKER[" << index << "]: Downloading file \""
                  << item.system_path << "\"" << std::endl;
      }

      std::ofstream of{item.system_path, open_mode};

      curl_easy_setopt(curl, CURLOPT_URL, item.download_url.c_str());
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.get());
      static_assert(CURL_MAX_READ_SIZE == (10L << 20));
      curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, CURL_MAX_READ_SIZE);

      std::string header_string{};
      std::size_t retry_after = 0;
      auto header_cb = [&header_string, &retry_after](char* buffer,
                                                      size_t size) -> size_t {
        if (!header_string.empty()) { header_string += "\n"; }
        auto header = std::string_view{buffer, size};
        header_string.append(header);

        static constexpr std::string_view retry_after_start = "retry-after: ";

        if (header.size() > retry_after_start.size()
            && strncasecmp(header.data(), retry_after_start.data(),
                           retry_after_start.size())
                   == 0) {
          auto value = header.substr(retry_after_start.size());

          std::from_chars(value.data(), value.data() + value.size(),
                          retry_after);
        }

        return size;
      };

      std::vector<char> data_buffer = {};
      data_buffer.reserve(d->maximum_chunk_size);

      curl_easy_setopt(
          curl, CURLOPT_HEADERFUNCTION,
          +[](char* buffer, size_t size, size_t nitems,
              void* userdata) -> size_t {
            auto* fun = static_cast<decltype(header_cb)*>(userdata);
            return (*fun)(buffer, size * nitems);
          });

      curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_cb);

      while (item.current_offset < item.size) {
        header_string.clear();
        data_buffer.clear();

        size_t begin = item.current_offset;
        size_t end = item.size;

        if (end - begin > d->maximum_chunk_size) {
          end = begin + d->maximum_chunk_size;
        }

        std::cerr << "WORKER[" << index << "]: Grabbing bytes " << begin << "-"
                  << end << std::endl;

        struct curl_slist* headers = nullptr;

        headers = curl_slist_append(
            headers, fmt::format("Range: bytes={}-{}", begin, end - 1).c_str());

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        auto res = curl_easy_write_cb(
            curl,
            [&data_buffer](const char* data, std::size_t size) -> std::size_t {
              auto current_size = data_buffer.size();

              if (current_size + size > data_buffer.capacity()) {
                return CURLE_WRITE_ERROR;
              }


              data_buffer.resize(current_size + size);

              std::memcpy(data_buffer.data() + current_size, data, size);

              return size;
            });

        if (res != CURLE_OK) {
          std::cerr << "WORKER[" << index << "]: "
                    << "Download failed with " << curl_easy_strerror(res)
                    << ": " << error_buffer.get() << " (" << res << ")"
                    << std::endl;
          break;
        }

        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        // success or partial content
        if (http_code != 200 && http_code != 206) {
          if (http_code == 429) {
            // we issued too many requests
            current_backoff += 1;

            wait_for = retry_after;
          }

          std::cerr << "WORKER[" << index << "]: "
                    << "Download return status " << http_code << "\n-----\n"
                    << header_string << "\n-----\n"
                    << std::string_view{data_buffer.data(), data_buffer.size()}
                    << "\n-----" << std::endl;
          break;
        }

        current_backoff = 0;
        std::cerr << "WORKER[" << index << "]: Grabbing bytes " << begin << "-"
                  << end << std::endl;

        if (data_buffer.size() != end - begin) {
          std::cerr << "WORKER[" << index
                    << "]: short read: " << data_buffer.size() << " vs "
                    << end - begin << std::endl;
        }

        of.write(data_buffer.data(), data_buffer.size());

        item.current_offset += data_buffer.size();

        curl_slist_free_all(headers);
      }

      assert(found_item);

      if (item.current_offset < item.size) {
        std::unique_lock l{me.renew_mut};
        assert(!item.renew_url.empty());
        me.to_renew.emplace_back(std::move(item));
      }

      auto old_val = d->ongoing_task_count.fetch_sub(1);
      assert(old_val != 0);
      curl_easy_reset(curl);

      current_backoff = 0;
    }

    curl_easy_cleanup(curl);
  }

  void download_file(download_item item)
  {
    if (!no_download) {
      auto& worker = states[0];

      ongoing_task_count += 1;

      std::unique_lock l{worker.download_mut};
      worker.to_download.emplace_back(std::move(item));
      worker.new_item.notify_all();
    }
  }

  enum class handled_http_status : json_int_t
  {
    Unhandled,
    OK,
    TooManyRequests,
  };

  handled_http_status as_handled_status(json_int_t status)
  {
    using enum handled_http_status;
    switch (status) {
      case 200:
        return OK;
      case 429:
        return TooManyRequests;
      default:
        return Unhandled;
    }
  }


  bool renew_token_if_necessary()
  {
    auto now = std::chrono::steady_clock::now();

    if (now + std::chrono::seconds(expiry_offset) <= tk.expiry_point) {
      // nothing to do
      return true;
    }

    std::optional token = request_token();
    if (!token) { return false; }
    tk = std::move(*token);

    // either the token expired, or its about to expire, so request a new return
    return true;
  }

  void update_wait(Clock::time_point until)
  {
    if (!wait_until || *wait_until < until) { wait_until = until; }
  }

  bool execute_batch(std::span<command> batch)
  {
    if (!renew_token_if_necessary()) { return false; }

    CURL* curl = curl_easy_init();

    if (!curl) {
      std::cerr << "Could not initialize curl" << std::endl;
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://graph.microsoft.com/v1.0/$batch");

    json_t* requests = json_array();

    std::cerr << "begin batch\n---------" << std::endl;
    for (size_t i = 0; i < batch.size(); ++i) {
      auto* request = json_object();
      json_object_set_new(request, "id", json_integer(i));

      std::visit([&](auto& val) { setup_request(request, val); }, batch[i]);

      json_array_append_new(requests, request);
    }
    std::cerr << "---------" << std::endl;

    json_t* body = json_object();
    json_object_set_new(body, "requests", requests);

    char* body_as_str = json_dumps(body, JSON_COMPACT);


    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_as_str);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    string_writer sw;
    auto res = curl_easy_write_cb(curl, sw);
    curl_easy_cleanup(curl);
    free(body_as_str);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) { return false; }

    json_error_t err = {};
    json_t* response_body = json_loadb(sw.s.data(), sw.s.size(), 0, &err);
    if (!response_body || !json_is_object(response_body)) { return false; }

    json_t* responses = json_object_get(response_body, "responses");
    if (!responses || !json_is_array(responses)) { return false; }

    auto now = Clock::now();

    std::vector<download_item> to_download;

    size_t index;
    json_t* resp = nullptr;
    json_array_foreach(responses, index, resp)
    {
      if (!json_is_object(resp)) { return false; }

      json_t* json_return_code = json_object_get(resp, "status");

      if (!json_return_code || !json_is_integer(json_return_code)) {
        std::cerr << "response [" << index << "] has no return code"
                  << std::endl;
        return false;
      }

      json_t* resp_headers = json_object_get(resp, "headers");
      if (!resp_headers || !json_is_object(resp_headers)) {
        std::cerr << "response [" << index
                  << "] has bad headers: " << json_dumps(resp, JSON_COMPACT)
                  << std::endl;
        return false;
      }

      json_t* resp_body = json_object_get(resp, "body");
      if (!resp_body || !json_is_object(resp_body)) {
        std::cerr << "response [" << index
                  << "] has bad body: " << json_dumps(resp, JSON_COMPACT)
                  << std::endl;
        return false;
      }

      json_t* json_idx = json_object_get(resp, "id");

      json_int_t idx = {};
      if (!json_idx || !parse_id(json_idx, &idx)) { return false; }

      auto return_code = json_integer_value(json_return_code);

      switch (as_handled_status(return_code)) {
        using enum handled_http_status;
        case OK: {
          // parse result
          if (!std::visit(
                  [&](auto& val) {
                    return handle_response(next_commands, to_download,
                                           resp_headers, resp_body, val);
                  },
                  batch[idx])) {
            return false;
          }
        } break;
        case TooManyRequests: {
          std::cerr << "response {" << idx << "} ([" << index
                    << "]) got throttled" << std::endl;

          json_t* json_retry = json_object_get(resp_headers, "Retry-After");
          if (json_retry && json_is_integer(json_retry)) {
            auto retry_after_seconds = json_integer_value(json_retry);
            std::cerr << " -> retry possible in " << retry_after_seconds
                      << " seconds" << std::endl;
            update_wait(now + std::chrono::seconds(retry_after_seconds));
            next_commands.push_back(batch[idx]);
          } else if (json_retry && json_is_string(json_retry)) {
            std::string_view retry_string = {json_string_value(json_retry),
                                             json_string_length(json_retry)};

            std::size_t retry_after_seconds;
            auto conv_err = std::from_chars(
                retry_string.data(), retry_string.data() + retry_string.size(),
                retry_after_seconds);

            if (conv_err.ec != std::errc{}
                || conv_err.ptr != retry_string.data() + retry_string.size()) {
              std::cerr << "could not convert Retry-After ("
                        << json_dumps(json_retry, 0) << ") to integer"
                        << std::endl;
              return false;
            }

            std::cerr << " -> retry possible in " << retry_after_seconds
                      << " seconds" << std::endl;
            update_wait(now + std::chrono::seconds(retry_after_seconds));
            next_commands.push_back(batch[idx]);

          } else {
            std::cerr << " -> retry not possible ( "
                      << json_dumps(resp, JSON_COMPACT) << " )" << std::endl;
            return false;
          }
        } break;
        case Unhandled: {
          json_t* failed = json_array_get(requests, idx);
          std::cerr << "response {" << idx << "} ([" << index
                    << "]) failed with code " << return_code << "("
                    << json_dumps(failed, JSON_COMPACT) << ")" << std::endl;

          json_t* json_retry = json_object_get(resp_headers, "Retry-After");
          if (json_retry && json_is_integer(json_retry)) {
            auto retry_after_seconds = json_integer_value(json_retry);
            std::cerr << " -> retry possible in " << retry_after_seconds
                      << " seconds" << std::endl;
            update_wait(now + std::chrono::seconds(retry_after_seconds));
            next_commands.push_back(batch[idx]);
          } else {
            std::cerr << " -> retry not possible ( "
                      << json_dumps(resp, JSON_COMPACT) << " )" << std::endl;
            return false;
          }
        }
      }
    }

    for (auto& d : to_download) { download_file(d); }

    return true;
  }

  std::span<command> next_batch()
  {
    std::size_t batch_start = to_execute.size();

    if (batch_start > max_batch_size) {
      batch_start -= max_batch_size;
    } else {
      batch_start = 0;
    }

    // while (batch_start > 0) {
    //   std::size_t batch_size = to_execute.size() - batch_start;
    //   if (batch_size == max_batch_size) {
    //     break;
    //   }

    //   auto& next = to_execute[batch_start - 1];
    //   if (std::get_if<renew_token>)
    // }

    return std::span{to_execute}.subspan(batch_start);
  }
};

struct uploader {
  void setup()
  {
    // the microsoft documentation tells us to always use chunk sizes
    // that are multiples of 320 KiB
    // https://learn.microsoft.com/en-us/graph/api/driveitem-createuploadsession
    auto chunk_size_multiple = 320 << 10;
    auto too_much = chunk_size % chunk_size_multiple;

    if (too_much != 0) {
      // we round up, so we do not have to deal with the
      // chunk_size == 0 case
      chunk_size += chunk_size_multiple - too_much;

      std::cerr << "chunk_size was not a multiple of 320KiB; enlarging it"
                << std::endl;
    }
  }

  void register_options(CLI::App* app)
  {
    app->add_option("source", source, "Which directory to upload")
        ->check(CLI::ExistingPath)
        ->required();
    app->add_option("target", target, "Where to upload stuff to")->required();
    app->add_option("--chunk-size", chunk_size,
                    "How many bytes the chunks for upload contain")
        ->transform(CLI::AsSizeValue{false})
        ->check(CLI::Range(size_t{1}, size_t{128} << 20));
    app->add_option("--worker-count", worker_count,
                    "How many bytes the chunks for upload contain")
        ->check(CLI::Range(size_t{1}, size_t{128}));
  }

  std::optional<std::string> fetch_site_by_name(CURL* curl,
                                                std::string_view name)
  {
    // there is sadly no easy wait to "search" for a site by name
    // neither `filter=` nor `search=` seem to work reliably for all sites.
    url_encoder encoder{};
    encoder.add_kv("select", "id,name");
    curl_easy_setopt(
        curl, CURLOPT_URL,
        fmt::format("https://graph.microsoft.com/v1.0/sites?{}", encoder.view())
            .c_str());

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    string_writer response_text;
    auto res = curl_easy_write_cb(curl, response_text);
    if (res != CURLE_OK) {
      std::cerr << "fetching all sites failed" << std::endl;
      curl_easy_reset(curl);
      return std::nullopt;
    }
    curl_easy_reset(curl);

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      return std::nullopt;
    }

    json_t* values = json_object_get(response_json, "value");
    if (!values || !json_is_array(values)) {
      std::cerr << "bad value in response" << std::endl;
      return std::nullopt;
    }

    size_t index{};
    json_t* value{};

    // TODO: handle nextLink
    json_array_foreach(values, index, value)
    {
      if (!json_is_object(value)) {
        auto str = json_dumps(value, JSON_COMPACT);
        std::cerr << "bad value in response: " << str << std::endl;
        free(str);
        json_decref(response_json);
        return std::nullopt;
      }

      json_t* json_id = json_object_get(value, "id");
      if (!json_id || !json_is_string(json_id)) {
        std::cerr << "fetched site has bad id ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }

      std::string_view id{json_string_value(json_id),
                          json_string_length(json_id)};

      json_t* json_name = json_object_get(value, "name");
      if (!json_name || !json_is_string(json_name)) {
        std::cerr << "fetched site has bad name ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }
      std::string_view site_name{json_string_value(json_name),
                                 json_string_length(json_name)};

      if (site_name == name) {
        std::string result;
        result.assign(id);
        json_decref(response_json);
        return result;
      }
    }

    json_decref(response_json);
    return std::nullopt;
  }

  std::optional<std::string> fetch_drive_by_name(CURL* curl,
                                                 std::string_view site_id,
                                                 std::string_view name)
  {
    // there is sadly no easy wait to "search" for a site by name
    // neither `filter=` nor `search=` seem to work reliably for all drives.
    url_encoder encoder{};
    encoder.add_kv("$select", "id,name");
    curl_easy_setopt(
        curl, CURLOPT_URL,
        fmt::format("https://graph.microsoft.com/v1.0/sites/{}/drives?{}",
                    site_id, encoder.view())
            .c_str());

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    string_writer response_text;
    auto res = curl_easy_write_cb(curl, response_text);
    if (res != CURLE_OK) {
      std::cerr << "fetching all drives failed" << std::endl;
      curl_easy_reset(curl);
      return std::nullopt;
    }
    curl_easy_reset(curl);

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      return std::nullopt;
    }

    json_t* values = json_object_get(response_json, "value");
    if (!values || !json_is_array(values)) {
      std::cerr << "bad value in response" << std::endl;
      return std::nullopt;
    }

    size_t index{};
    json_t* value{};

    // TODO: handle nextLink
    json_array_foreach(values, index, value)
    {
      if (!json_is_object(value)) {
        auto str = json_dumps(value, JSON_COMPACT);
        std::cerr << "bad value in response: " << str << std::endl;
        free(str);
        json_decref(response_json);
        return std::nullopt;
      }

      json_t* json_id = json_object_get(value, "id");
      if (!json_id || !json_is_string(json_id)) {
        std::cerr << "fetched drive has bad id ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }

      std::string_view id{json_string_value(json_id),
                          json_string_length(json_id)};

      json_t* json_name = json_object_get(value, "name");
      if (!json_name || !json_is_string(json_name)) {
        std::cerr << "fetched drive has bad name ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }
      std::string_view site_name{json_string_value(json_name),
                                 json_string_length(json_name)};

      if (site_name == name) {
        std::string result;
        result.assign(id);
        json_decref(response_json);
        return result;
      }
    }

    json_decref(response_json);
    return std::nullopt;
  }

  std::optional<std::string> fetch_item_by_name(CURL* curl,
                                                std::string_view drive_id,
                                                std::string_view path,
                                                std::string_view name)
  {
    // there is sadly no easy wait to "search" for a site by name
    // neither `filter=` nor `search=` seem to work reliably for all drives.
    url_encoder encoder{};
    encoder.add_kv("$select", "id,name");
    encoder.add_kv("$filter", fmt::format("name eq '{}'", name));
    curl_easy_setopt(
        curl, CURLOPT_URL,
        fmt::format("https://graph.microsoft.com/v1.0/drives/{}/{}/children?{}",
                    drive_id, path, encoder.view())
            .c_str());

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    string_writer response_text;
    auto res = curl_easy_write_cb(curl, response_text);
    if (res != CURLE_OK) {
      std::cerr << "fetching all drives failed" << std::endl;
      curl_easy_reset(curl);
      return std::nullopt;
    }
    curl_easy_reset(curl);

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      return std::nullopt;
    }

    json_t* values = json_object_get(response_json, "value");
    if (!values || !json_is_array(values)) {
      std::cerr << "bad value in response" << std::endl;
      return std::nullopt;
    }

    size_t index{};
    json_t* value{};

    // TODO: handle nextLink
    json_array_foreach(values, index, value)
    {
      if (!json_is_object(value)) {
        auto str = json_dumps(value, JSON_COMPACT);
        std::cerr << "bad value in response: " << str << std::endl;
        free(str);
        json_decref(response_json);
        return std::nullopt;
      }

      json_t* json_id = json_object_get(value, "id");
      if (!json_id || !json_is_string(json_id)) {
        std::cerr << "fetched drive has bad id ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }

      std::string_view id{json_string_value(json_id),
                          json_string_length(json_id)};

      json_t* json_name = json_object_get(value, "name");
      if (!json_name || !json_is_string(json_name)) {
        std::cerr << "fetched drive has bad name ("
                  << json_dumps(value, JSON_COMPACT) << ")" << std::endl;
        return std::nullopt;
      }
      std::string_view site_name{json_string_value(json_name),
                                 json_string_length(json_name)};

      if (site_name == name) {
        std::string result;
        result.assign(id);
        json_decref(response_json);
        return result;
      }
    }

    std::cerr << "\"" << name << "\" was not found" << std::endl;
    json_decref(response_json);
    return std::nullopt;
  }

  // returns the url to the last folder
  std::optional<std::pair<std::string, std::string>> make_remote_path(
      std::string_view path)
  {
    CURL* curl = curl_easy_init();

    std::size_t current = 0;

    auto site_slash = path.find('/', current);

    if (site_slash == path.npos) { site_slash = path.size(); }

    std::string_view site_name = path.substr(current, site_slash - current);

    std::cerr << "Looking up site \"" << site_name << "\"" << std::endl;

    auto site = fetch_site_by_name(curl, site_name);
    if (!site) { return std::nullopt; }

    std::cout << "found site: " << *site << std::endl;

    if (site_slash == path.size()) {
      std::cerr << "a drive is required" << std::endl;
      return std::nullopt;
    }

    current = site_slash + 1;

    auto drive_slash = path.find('/', current);

    if (drive_slash == path.npos) { drive_slash = path.size(); }

    std::string_view drive_name = path.substr(current, drive_slash - current);

    std::cerr << "Looking up drive \"" << drive_name << "\"" << std::endl;

    auto drive = fetch_drive_by_name(curl, *site, drive_name);
    if (!drive) { return std::nullopt; }

    std::cout << "found drive: " << *drive << std::endl;

    if (drive_slash == path.size()) {
      return std::pair{std::move(*drive), "root"};
    }

    current = drive_slash + 1;

    std::string current_path = "root";

    while (current < path.size()) {
      auto next_slash = path.find('/', current);
      if (next_slash == path.npos) { next_slash = path.size(); }

      std::string_view folder = path.substr(current, next_slash - current);

      std::cerr << "Looking up folder \"" << folder << "\"" << std::endl;

      auto item = fetch_item_by_name(curl, *drive, current_path, folder);

      if (!item) { return std::nullopt; }

      std::cerr << "Found item \"" << *item << "\"" << std::endl;

      current_path = fmt::format("items/{}", *item);

      current = next_slash + 1;
    }

    curl_easy_cleanup(curl);

    return std::pair{std::move(*drive), std::move(current_path)};
  }

  std::optional<std::string> create_directory_in(CURL* curl,
                                                 std::string_view path,
                                                 std::string_view name)
  {
    string_writer response_text;
    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(

        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    url_encoder encoder{};
    encoder.add_kv("@microsoft.graph.conflictBehavior", "replace");

    auto url = fmt::format("https://graph.microsoft.com/v1.0{}/children?{}",
                           path, encoder.view());

    std::cerr << "using url: " << url << std::endl;
    // we need to begin the session
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    auto* json = json_object();
    json_object_set_new(json, "name", json_stringn(name.data(), name.size()));
    json_object_set_new(json, "folder", json_object());

    std::string encoded = json_dumps(json, JSON_COMPACT);
    json_decref(json);
    std::cerr << "using request: " << encoded << std::endl;

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoded.c_str());
    curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1L << 10);

    auto res = curl_easy_write_cb(curl, response_text);
    if (res != CURLE_OK) {
      std::cerr << "create child request failed" << std::endl;
      curl_easy_reset(curl);
      return std::nullopt;
    }

    long http_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (200 < http_code && http_code >= 300) {
      std::cerr << "creation failed" << std::endl;

      curl_easy_reset(curl);
      curl_slist_free_all(headers);
      return std::nullopt;
    }

    curl_easy_reset(curl);
    curl_slist_free_all(headers);

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      curl_easy_reset(curl);
      curl_slist_free_all(headers);
      return std::nullopt;
    }

    std::string_view id;
    {
      size_t len;
      const char* id_c;
      if (json_unpack_ex(response_json, &err, 0, "{s:s%}", "id", &id_c, &len)
          < 0) {
        std::cerr << "failed to unpack json value: " << err.text << std::endl;
        json_decref(response_json);
        return std::nullopt;
      }

      id = std::string_view{id_c, len};
    }
    auto relative_path = fmt::format("items/{}", id);
    json_decref(response_json);
    return relative_path;
  }

  bool create_file_in(CURL* curl,
                      std::string_view path,
                      const std::string& file_path,
                      std::size_t file_size)
  {
    wait_for_backoff();

    if (file_size > 10 << 20) {
      std::cerr << "using large file upload for \"" << file_path << "\""
                << std::endl;
      return create_big_file_in(curl, path, file_path, file_size);
    }

    std::string_view file_name = basename(file_path.c_str());

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    url_encoder encoder{};
    encoder.add_kv("@microsoft.graph.conflictBehavior", "replace");
    encoder.add_kv("$select", "id");

    auto url = fmt::format("https://graph.microsoft.com/v1.0{}:/{}:/content?{}",
                           path, file_name, encoder.view());

    std::cerr << "using url: " << url << std::endl;
    // we need to begin the session
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

    std::ifstream f{file_path, std::ios::binary};
    auto lambda = [&](char* buffer, size_t size) -> size_t {
      f.read(buffer, size);

      auto written = f.gcount();

      return written;
    };
    curl_easy_setopt(
        curl, CURLOPT_READFUNCTION,
        +[](char* curldata, size_t size, size_t nmemb,
            void* userdata) -> size_t {
          auto* cb = static_cast<decltype(lambda)*>(userdata);

          size_t bytes = size * nmemb;
          return (*cb)(curldata, bytes);
        });
    curl_easy_setopt(curl, CURLOPT_READDATA, &lambda);

    string_writer response_text;
    auto res = curl_easy_write_cb(curl, response_text);
    if (res != CURLE_OK) {
      std::cerr << "upload request failed" << std::endl;
      curl_easy_reset(curl);
      return false;
    }
    long http_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 201 && http_code != 200) {
      std::cerr << "small file upload failed: " << http_code << std::endl;
      curl_easy_reset(curl);
      curl_slist_free_all(headers);
      return false;
    }

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      return false;
    }

    bool success = true;

    size_t len;
    const char* id_c;
    if (json_unpack_ex(response_json, &err, 0, "{s:s%}", "id", &id_c, &len)
        < 0) {
      std::cerr << "failed to unpack json value: " << err.text << std::endl;
      json_decref(response_json);
    }

    curl_easy_reset(curl);
    curl_slist_free_all(headers);
    return success;
  }

  bool create_big_file_in(CURL* curl,
                          std::string_view path,
                          const std::string& file_path,
                          std::size_t file_size)
  {
    wait_for_backoff();

    std::string_view file_name = basename(file_path.c_str());


    string_writer response_text;
    {
      struct curl_slist* headers = nullptr;

      headers = curl_slist_append(

          headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");

      url_encoder encoder{};
      encoder.add_kv("@microsoft.graph.conflictBehavior", "replace");

      auto url = fmt::format(
          "https://graph.microsoft.com/v1.0{}:/{}:/createUploadSession?{}",
          path, file_name, encoder.view());

      std::cerr << "using url: " << url << std::endl;
      // we need to begin the session
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

      auto* json = json_object();
      auto* item = json_object();
      json_object_set_new(item, "name",
                          json_stringn(file_name.data(), file_name.size()));
      // json_object_set_new(item, "fileSize", json_integer(file_size));
      // json_object_set_new(item, "@microsoft.graph.conflictBehavior",
      // json_string("fail"));
      json_object_set_new(json, "item", item);

      std::string encoded = json_dumps(json, JSON_COMPACT);
      json_decref(json);
      std::cerr << "using request: " << encoded << std::endl;

      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoded.c_str());

      auto res = curl_easy_write_cb(curl, response_text);
      if (res != CURLE_OK) {
        std::cerr << "createUploadSession request failed" << std::endl;
        curl_easy_reset(curl);
        return false;
      }
      curl_easy_reset(curl);
      curl_slist_free_all(headers);
    }

    json_error_t err = {};
    auto* response_json
        = json_loadb(response_text.s.c_str(), response_text.s.size(), 0, &err);

    if (!response_json) {
      std::cerr << "response is not json: " << err.text << std::endl;
      json_decref(response_json);
      return false;
    }

    json_t* json_upload_url = json_object_get(response_json, "uploadUrl");
    if (!json_upload_url || !json_is_string(json_upload_url)) {
      std::cerr << "response contains bad upload_url ("
                << json_dumps(response_json, JSON_COMPACT) << ")" << std::endl;
      return false;
    }

    std::string upload_url{json_string_value(json_upload_url),
                           json_string_length(json_upload_url)};

    std::cerr << "received upload url " << upload_url << std::endl;


    std::ifstream f{file_path, std::ios::binary};

    size_t error_count = 0;

    std::size_t backoff = 0;

    for (size_t current_offset = 0; current_offset < file_size;) {
      auto diff = file_size - current_offset;
      if (diff > chunk_size) { diff = chunk_size; }
      curl_off_t bytes_to_write = diff;
      std::cerr << "uploading " << current_offset << "-"
                << current_offset + bytes_to_write << " bytes" << std::endl;

      curl_easy_setopt(curl, CURLOPT_URL, upload_url.c_str());
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, bytes_to_write);
      // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
      curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, 1L << 10);

      struct curl_slist* headers = nullptr;

      headers = curl_slist_append(
          headers, fmt::format("Content-Range: bytes {}-{}/{}", current_offset,
                               current_offset + bytes_to_write - 1, file_size)
                       .c_str());

      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      std::size_t bytes_written = 0;
      auto lambda = [&](char* buffer, size_t size) -> size_t {
        f.read(buffer, size);

        auto written = f.gcount();

        // std::cerr << "Filled buffer with " << written << "/" << size << "
        // bytes" << std::endl;

        // this can be called multiple times
        bytes_written += written;
        return written;
      };
      curl_easy_setopt(
          curl, CURLOPT_READFUNCTION,
          +[](char* curldata, size_t size, size_t nmemb,
              void* userdata) -> size_t {
            auto* cb = static_cast<decltype(lambda)*>(userdata);

            size_t bytes = size * nmemb;
            return (*cb)(curldata, bytes);
          });
      curl_easy_setopt(curl, CURLOPT_READDATA, &lambda);


      string_writer upload_response_text;

      auto backoff_duration = std::chrono::milliseconds(size_t{100} << backoff);

      std::this_thread::sleep_for(backoff_duration);

      auto upload_res = curl_easy_write_cb(curl, upload_response_text);

      // std::cerr << "Wrote " << bytes_written << " bytes" << std::endl;
      if (upload_res != CURLE_OK) {
        std::cerr << "Upload of bytes " << current_offset << "-"
                  << current_offset + bytes_to_write << " failed" << std::endl;

        error_count += 1;
        if (error_count > 10) {
          curl_easy_reset(curl);
          curl_slist_free_all(headers);
          return false;
        }

        f.seekg(current_offset);
      } else {
        long http_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 202 || http_code == 201) {
          std::cerr << "upload returned: " << upload_response_text.s
                    << std::endl;

          current_offset += bytes_written;

          // upload succeeded, so reset the backoff
          backoff = 0;
        } else if (500 <= http_code && http_code < 600) {
          // if the upload fails with a 5XX value, then we need to retry
          // with exponential backoff
          std::cerr << "upload failed with: " << http_code << std::endl;

          backoff += 1;
        } else {
          // other errors are fatal

          std::cerr << "upload returned " << http_code << "; considered failure"
                    << std::endl;
          curl_easy_reset(curl);
          curl_slist_free_all(headers);
          return false;
        }
      }

      curl_easy_reset(curl);
      curl_slist_free_all(headers);
    }

    curl_easy_reset(curl);
    return true;
  }

  struct small_file {
    std::string path;
    std::size_t size;
  };

  static constexpr std::size_t max_files_per_batch = 20;

  struct bunch_of_small_files {
    std::array<small_file, max_files_per_batch> files{};
    std::size_t total_size{};
    std::size_t file_count{};

    void push(std::string_view path, std::size_t size)
    {
      assert(file_count < std::size(files));
      files[file_count].path.assign(path);
      files[file_count].size = size;
      file_count += 1;
      total_size += size;
    }

    std::span<const small_file> get() const
    {
      return std::span{files}.subspan(0, file_count);
    }

    std::size_t size() const { return total_size; }

    bool full() const { return file_count == std::size(files); }

    void reset()
    {
      total_size = 0;
      file_count = 0;
    }
  };

  std::optional<std::chrono::steady_clock::time_point> backoff_time{};

  void wait_until(std::chrono::steady_clock::time_point point)
  {
    if (backoff_time && *backoff_time > point) { return; }
    backoff_time = point;
  }

  void wait_for_backoff()
  {
    if (backoff_time) {
      std::this_thread::sleep_until(*backoff_time);
      backoff_time.reset();
    }
  }

  bool create_bunch_of_small_files(CURL* curl,
                                   std::string_view root_path,
                                   std::string_view base_dir,
                                   std::span<const small_file> files)
  {
    wait_for_backoff();

    json_t* requests = json_array();

    std::vector<char> file_data_buffer;
    std::vector<char> base64_buffer;

    url_encoder encoder{};
    encoder.add_kv("@microsoft.graph.conflictBehavior", "replace");
    encoder.add_kv("$select", "id");

    auto pos = base_dir.rfind('/');

    std::size_t prefix_size = 0;
    if (pos != base_dir.npos) { prefix_size = pos + 1; }

    std::cerr << "begin batch\n---------" << std::endl;
    for (size_t i = 0; i < files.size(); ++i) {
      auto& file = files[i];
      json_t* request = json_object();

      assert(file.path.starts_with(base_dir));

      auto remote_path = std::string_view{file.path}.substr(prefix_size);

      std::string file_url = fmt::format("{}:/{}:/content?{}", root_path,
                                         remote_path, encoder.view());

      std::cerr << " [" << i << "] creating \"" << remote_path << "\" ("
                << file_url << "); size = " << file.size << " bytes"
                << std::endl;

      json_object_set_new(request, "id", json_integer(i));
      json_object_set_new(request, "url",
                          json_stringn(file_url.c_str(), file_url.size()));
      json_object_set_new(request, "method", PUT);

      auto* file_headers = json_object();
      json_object_set_new(file_headers, "content-type",
                          json_string("text/plain"));
      // json_object_set_new(file_headers, "content-length",
      // json_integer(file.size));

      // file_data_buffer.resize(file.size);
      // if (!read_file(file_data_buffer, file.path.c_str())) {
      //   json_decref(requests);
      //   return false;
      // }

      {
        std::string_view x = "Hello World!";
        file_data_buffer.assign(std::begin(x), std::end(x));
      }

      base64_buffer.resize((file_data_buffer.size() * 4) / 3 + 3 + 1);

      {
        auto written = BinToBase64(base64_buffer.data(), base64_buffer.size(),
                                   file_data_buffer.data(),
                                   file_data_buffer.size(), true);
        assert(written >= 0);
        // we always over estimate
        assert(static_cast<std::size_t>(written) < base64_buffer.size());

        base64_buffer.resize(written);
      }

      auto* file_body
          = json_stringn(base64_buffer.data(), base64_buffer.size());

      // auto* file_body = json_string("SGVsbG8gV29ybGQh");
      json_object_set_new(request, "headers", file_headers);
      json_object_set_new(request, "body", file_body);

      json_array_append_new(requests, request);
    }

    std::cerr << "---------" << std::endl;

    struct curl_slist* headers = nullptr;

    headers = curl_slist_append(
        headers, fmt::format("Authorization: Bearer {}", tk.value).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://graph.microsoft.com/v1.0/$batch");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);

    json_t* body = json_object();
    json_object_set_new(body, "requests", requests);

    char* body_as_str = json_dumps(body, JSON_COMPACT);

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_as_str);

    string_writer sw;
    auto res = curl_easy_write_cb(curl, sw);

    if (res != CURLE_OK) {
      std::cerr << "Batch upload failed" << sw.s << std::endl;
    }

    long http_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_reset(curl);
    free(body_as_str);

    if (http_code != 200) {
      std::cerr << "bad batch upload: " << http_code << std::endl;
      return false;
    }
    json_error_t err = {};
    auto* response = json_loadb(sw.s.c_str(), sw.s.size(), 0, &err);
    if (!response) {
      std::cerr << "Batch upload returned non-json: " << sw.s << std::endl;
      json_decref(response);
      return false;
    }

    json_t* responses = json_object_get(response, "responses");
    if (!responses || !json_is_array(responses)) { return false; }

    size_t index;
    json_t* resp = nullptr;
    json_array_foreach(responses, index, resp)
    {
      if (!json_is_object(resp)) { return false; }

      json_t* json_return_code = json_object_get(resp, "status");

      if (!json_return_code || !json_is_integer(json_return_code)) {
        std::cerr << "response [" << index << "] has no return code"
                  << std::endl;
        return false;
      }

      json_t* resp_headers = json_object_get(resp, "headers");
      if (!resp_headers || !json_is_object(resp_headers)) {
        std::cerr << "response [" << index
                  << "] has bad headers: " << json_dumps(resp, JSON_COMPACT)
                  << std::endl;
        return false;
      }

      json_t* resp_body = json_object_get(resp, "body");
      if (!resp_body || !json_is_string(resp_body)) {
        std::cerr << "response [" << index
                  << "] has bad body: " << json_dumps(resp, JSON_COMPACT)
                  << std::endl;
        return false;
      }

      json_t* json_idx = json_object_get(resp, "id");

      json_int_t idx = {};
      if (!json_idx || !parse_id(json_idx, &idx)) { return false; }

      auto return_code = json_integer_value(json_return_code);

      switch (return_code) {
        case 201:
          [[fallthrough]];
        case 200: {
          // parse result
          std::cerr << "response {" << idx << "} ([" << index
                    << "]) succeeded with " << json_string_value(resp_body)
                    << std::endl;
        } break;
        case 429: {
          std::cerr << "response {" << idx << "} ([" << index
                    << "]) got throttled" << std::endl;

          json_t* json_retry = json_object_get(resp_headers, "Retry-After");
          if (json_retry) {
            if (json_is_integer(json_retry)) {
              auto retry_after_seconds = json_integer_value(json_retry);
              std::cerr << " -> retry possible in " << retry_after_seconds
                        << " seconds" << std::endl;
              wait_until(std::chrono::steady_clock::now()
                         + std::chrono::seconds(retry_after_seconds));
            } else if (json_is_string(json_retry)) {
              std::string_view retry_string = {json_string_value(json_retry),
                                               json_string_length(json_retry)};

              std::size_t retry_after_seconds;
              auto conv_err
                  = std::from_chars(retry_string.data(),
                                    retry_string.data() + retry_string.size(),
                                    retry_after_seconds);

              if (conv_err.ec != std::errc{}
                  || conv_err.ptr
                         != retry_string.data() + retry_string.size()) {
                std::cerr << "could not convert Retry-After ("
                          << json_dumps(json_retry, 0) << ") to integer"
                          << std::endl;
                return false;
              }

              std::cerr << " -> retry possible in " << retry_after_seconds
                        << " seconds" << std::endl;
              wait_until(std::chrono::steady_clock::now()
                         + std::chrono::seconds(retry_after_seconds));
            }
          }
        } break;
        default: {
          std::cerr << "response {" << idx << "} ([" << index
                    << "]) failed with code " << return_code << std::endl;

          return false;
        }
      }
    }

    return true;
  }

  struct upload_file {
    std::string source;
    std::string target;
    std::size_t file_size;
  };

  struct create_directory {
    std::string target;
  };

  using ucommand = std::variant<upload_file, create_directory>;

  std::mutex writer_mutex;
  moodycamel::BlockingReaderWriterCircularBuffer<ucommand> queue{20};

  friend void upload_thread(uploader* up)
  {
    ptr curl{curl_easy_init()};
    if (!curl) { return; }

    auto error_buffer = std::make_unique<char[]>(CURL_ERROR_SIZE);

    auto& bearer_token = up->tk;

    auto& mutex = up->writer_mutex;
    auto& queue = up->queue;

    for (;;) {
      {
        // only one writer at a time can read from the queue
        std::unique_lock wlock{mutex};

        queue.peek();
      }

      struct curl_slist* headers = nullptr;

      headers = curl_slist_append(
          headers,
          fmt::format("Authorization: Bearer {}", bearer_token.value).c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");

      curl_easy_setopt(curl, CURLOPT_URL,
                       "https://graph.microsoft.com/v1.0/$batch");
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.get());

      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);

      json_t* body = json_object();
      // json_object_set_new(body, "requests", requests);

      ptr body_as_str{json_dumps(body, JSON_COMPACT)};

      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_as_str.get());

      string_writer sw;
      auto res = curl_easy_write_cb(curl, sw);

      if (res != CURLE_OK) {
        std::cerr << "Batch upload failed" << sw.s << std::endl;
        curl_easy_reset(curl);
        free(body_as_str);
        continue;
      }

      long http_code;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

      curl_easy_reset(curl);

      if (http_code != 200) {
        std::cerr << "bad batch upload: " << http_code << std::endl;
        continue;
      }

      json_error_t err = {};
      ptr response{json_loadb(sw.s.c_str(), sw.s.size(), 0, &err)};
      if (!response) {
        std::cerr << "Batch upload returned non-json: " << sw.s << std::endl;
        continue;
      }

      auto* responses = json_object_get(response, "responses");
      if (!responses || !json_is_array(responses)) { continue; }

      size_t idx;
      json_t* resp = nullptr;
      json_array_foreach(responses, idx, resp) {}
    }

    curl_easy_cleanup(curl);
  }

  bool execute()
  {
    auto last_uploaded = std::chrono::steady_clock::now();
    auto last_sent = std::chrono::steady_clock::now();


    std::optional token = request_token();
    if (!token) { return false; }
    tk = std::move(*token);

    // first we need to find our destination

    auto folder_path = make_remote_path(target);
    if (!folder_path) { return false; }

    auto drive = std::move(folder_path->first);

    struct path {
      std::string path;
      std::string parent_id;
    };

    auto root_path = fmt::format("/drives/{}/{}", drive, folder_path->second);

    std::vector<path> stack;
    stack.push_back({source, std::move(folder_path->second)});

    std::size_t total_items_uploaded = 0;
    std::size_t items_uploaded = 0;
    size_t data_sent = 0;
    std::size_t last_data_sent = 0;

    size_t data_chkpnt = 1;
    bunch_of_small_files small_files;

    CURL* curl = curl_easy_init();
    while (!stack.empty()) {
      auto current = std::move(stack.back());
      stack.pop_back();

      struct stat s;
      if (stat(current.path.c_str(), &s) < 0) {
        std::cerr << "could not stat \"" << source << "\": " << strerror(errno)
                  << std::endl;
        return false;
      }

      auto parent_url = fmt::format("/drives/{}/{}", drive, current.parent_id);

      if (S_ISDIR(s.st_mode)) {
        std::string_view name = basename(current.path.c_str());
        std::optional directory_id
            = create_directory_in(curl, parent_url, name);
        if (!directory_id) {
          std::cerr << "creating of " << current.path << " failed" << std::endl;
          continue;
        }

        auto* dir = opendir(current.path.c_str());
        if (dir) {
          for (;;) {
            auto* dirent = readdir(dir);
            if (!dirent) { break; }

            if (strcmp(dirent->d_name, ".") == 0) { continue; }
            if (strcmp(dirent->d_name, "..") == 0) { continue; }

            std::string child_path = current.path + "/";
            child_path += dirent->d_name;
            stack.push_back({child_path, *directory_id});
          }
          closedir(dir);
        } else {
          std::cerr << "could not open dir " << current.path << std::endl;
        }

      } else if (S_ISREG(s.st_mode)) {
        static constexpr std::size_t SMALL_FILE_THRESHOLD = 4 << 20;
        // todo: this needs to take into account base64
        static constexpr std::size_t BATCH_SIZE_THRESHOLD = 4 << 20;

        if (s.st_size >= 0
            && static_cast<std::size_t>(s.st_size) <= SMALL_FILE_THRESHOLD) {
          if (small_files.full()
              || small_files.size() + s.st_size > BATCH_SIZE_THRESHOLD) {
            auto files_to_send = small_files.get();
            if (!create_bunch_of_small_files(curl, root_path, source,
                                             files_to_send)) {
              std::cerr << "creating of some batch failed" << std::endl;
            } else {
              items_uploaded += files_to_send.size();
              data_sent += small_files.size();
            }

            small_files.reset();
          }

          small_files.push(current.path, s.st_size);
        } else if (!create_file_in(curl, parent_url, current.path, s.st_size)) {
          std::cerr << "creating of " << current.path << " failed" << std::endl;
        } else {
          data_sent += s.st_size;
          items_uploaded += 1;
        }
      } else {
        std::cerr << "cannot handle file \"" << source << "\"";
        curl_easy_cleanup(curl);
        return false;
      }

      if (items_uploaded > 100) {
        auto current_uploaded = std::chrono::steady_clock::now();

        auto diff_in_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            current_uploaded - last_uploaded);
        if (diff_in_seconds.count() == 0) {
          diff_in_seconds = std::chrono::seconds(1);
        }

        total_items_uploaded += items_uploaded;

        std::cerr << " --- UPLOADED: " << total_items_uploaded
                  << " items; RATE: "
                  << (items_uploaded / diff_in_seconds.count())
                  << " items/s --- " << std::endl;

        last_uploaded = current_uploaded;
        items_uploaded = 0;
      }

      if (data_sent > data_chkpnt << 20) {
        auto current_sent = std::chrono::steady_clock::now();
        auto diff_in_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            current_sent - last_sent);
        if (diff_in_seconds.count() == 0) {
          diff_in_seconds = std::chrono::seconds(1);
        }
        std::cerr << " --- SENT: " << data_sent << " bytes; RATE: "
                  << ((double)data_sent - last_data_sent)
                         / diff_in_seconds.count()
                  << " bytes/s --- " << std::endl;
        data_chkpnt += 1;
        last_sent = current_sent;
      }
    }
    {
      auto files_to_send = small_files.get();
      if (files_to_send.size() > 0) {
        if (!create_bunch_of_small_files(curl, root_path, source,
                                         files_to_send)) {
          std::cerr << "creating of some batch failed" << std::endl;
        } else {
          items_uploaded += files_to_send.size();
        }
      }
    }

    curl_easy_cleanup(curl);

    std::cerr << " --- TOTAL UPLOADED: " << items_uploaded << " --- "
              << std::endl;
    return true;
  }

 private:
  std::string source;
  std::string target;
  std::size_t chunk_size{10 << 20};
  std::size_t worker_count{2};
  bearer_token tk;
};

int main(int argc, const char* argv[])
{
  CLI::App app;

  app.add_option("--tenant-id", tenant_id)
      ->required()
      ->envname("MGRAPH_TENANT_ID");
  app.add_option("--client-id", client_id)
      ->required()
      ->envname("MGRAPH_CLIENT_ID");
  app.add_option("--client-secret", client_secret)
      ->required()
      ->envname("MGRAPH_CLIENT_SECRET");

  auto* fetch_token
      = app.add_subcommand("fetch-token", "fetch a new token to use")
            ->fallthrough();

  downloader dl{};

  auto* download = app.add_subcommand("download", "download data");
  dl.register_options(download);

  auto* upload = app.add_subcommand("upload", "upload data");
  uploader up{};
  up.register_options(upload);

  app.require_subcommand(1);
  CLI11_PARSE(app, argc, argv);

  curl_global_init(CURL_GLOBAL_ALL);
  if (*fetch_token) {
    fetch_token_data d{};
    if (!d.execute()) { return 1; }
  }

  if (*download) {
    dl.setup();
    dl.to_execute.push_back(fetch_sites{});
    if (!dl.execute()) { return 1; }
  }

  if (*upload) {
    up.setup();
    if (!up.execute()) { return 1; }
  }

  return 0;
}

#endif

#include <lib/base64.h>

static constexpr std::string_view api_root = "https://graph.microsoft.com/v1.0";
std::string tenant_id;
std::string client_id;
std::string client_secret;
std::string app_uri;

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

  bool success = true;

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
  std::string expiry;
  std::string not_before;
  std::string issued_at;
  std::string id;
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

  return {{header_algo, header_key_id}, {}};
}

struct site_metadata {
  std::string token_endpoint{};
  std::string jwks_uri{};
  std::string issuer{};
  std::string device_authorization_endpoint{};
};

site_metadata fetch_metadata(CURL* curl)
{
  curl_easy_setopt(curl, CURLOPT_URL,
                   fmt::format("https://login.microsoftonline.com/{}/v2.0/"
                               ".well-known/openid-configuration",
                               tenant_id)
                       .c_str());

  string_writer sw;
  auto res = curl_easy_write_cb(curl, sw);

  if (res != CURLE_OK) { throw std::runtime_error{"could not fetch metadata"}; }

  curl_easy_reset(curl);

  json_error_t err = {};
  json_t* json = json_loadb(sw.s.data(), sw.s.size(), 0, &err);
  if (!json || !json_is_object(json)) { throw std::runtime_error{"bad json"}; }

  const char* token_endpoint{};
  const char* jwks_uri{};
  const char* issuer{};
  const char* device_authorization_endpoint{};

  if (json_unpack_ex(json, &err, 0, "{s:s, s:s, s:s, s:s}", "token_endpoint",
                     &token_endpoint, "jwks_uri", &jwks_uri, "issuer", &issuer,
                     "device_authorization_endpoint",
                     &device_authorization_endpoint)
      < 0) {
    throw std::runtime_error{"missing values in json object"};
  }

  return {token_endpoint, jwks_uri, issuer, device_authorization_endpoint};
}

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

  ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
  if (!ctx) { goto cleanup; }

  if (EVP_PKEY_fromdata_init(ctx) != 1) { goto cleanup; }
  params[0] = OSSL_PARAM_construct_BN(
      OSSL_PKEY_PARAM_RSA_N,
      const_cast<unsigned char*>(
          reinterpret_cast<const unsigned char*>(modulus.data())),
      static_cast<int>(modulus.size()));
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
  bool success = false;
  (void)key;
  EVP_PKEY* public_key = create_public_key(key.modulus, key.exponent);
  if (!public_key) { return false; }

  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) { goto cleanup; }

  if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, public_key)
      != 1) {
    goto cleanup;
  }

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

int main(int argc, const char* argv[])
{
  CLI::App app;

  app.add_option("--tenant-id", tenant_id)->required()->envname("TENANT_ID");
  app.add_option("--client-id", client_id)->required()->envname("CLIENT_ID");
  app.add_option("--client-secret", client_secret)
      ->required()
      ->envname("CLIENT_SECRET");
  app.add_option("--app-uri", app_uri)->required()->envname("APP_URI");

  CLI11_PARSE(app, argc, argv);

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
    encoder.add_kv("scope", app_uri);
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

  auto str
      = R"END({"token_type":"Bearer","scope":"api://9c3e303f-c576-4e48-b916-c127cc32256a/console","expires_in":4623,"ext_expires_in":4623,"access_token":"eyJ0eXAiOiJKV1QiLCJhbGciOiJSUzI1NiIsIng1dCI6IndoMDZzRWt6TEhKNXNOTmFVeVJZMl82TzhLMCIsImtpZCI6IndoMDZzRWt6TEhKNXNOTmFVeVJZMl82TzhLMCJ9.eyJhdWQiOiJhcGk6Ly85YzNlMzAzZi1jNTc2LTRlNDgtYjkxNi1jMTI3Y2MzMjI1NmEiLCJpc3MiOiJodHRwczovL3N0cy53aW5kb3dzLm5ldC9iNTk2MmU2My05OGIyLTRkMDktYjcyYS0yNzc1MWFkYzQ1MjQvIiwiaWF0IjoxNzgxODQ2MTM4LCJuYmYiOjE3ODE4NDYxMzgsImV4cCI6MTc4MTg1MTA2MiwiYWNyIjoiMSIsImFpbyI6IkFWUUFxLzhjQUFBQVJhdWtKM2cxdWlaYzZMOXhab1hKOXBnRlFXaU5jVW5CaEdDVS9jWnVBVVFjRWw3VTBKYXA4TlozSzd5OENSTDh6NzkrNDRBQWlSeFZDNGovdWltNU5UWWlZRTUrZS9jUzY0VG5GVjMwY1pFPSIsImFtciI6WyJwd2QiXSwiYXBwaWQiOiI5YzNlMzAzZi1jNTc2LTRlNDgtYjkxNi1jMTI3Y2MzMjI1NmEiLCJhcHBpZGFjciI6IjAiLCJnaXZlbl9uYW1lIjoiU2ViYXN0aWFuIiwiaXBhZGRyIjoiMjAwMTo0ZGQ0OmE3NWU6MDpmMGM3OjQ4NTk6NGRkNTo5ZmY0IiwibmFtZSI6IlNlYmFzdGlhbiIsIm9pZCI6IjQ3YmJjNGFkLTBkNWMtNDQ0Ny04MTNiLTRiNTg4ZTUxNDI2NiIsInJoIjoiMS5BUk1CWXk2V3RiS1lDVTIzS2lkMUd0eEZKRDh3UHB4MnhVaE91UmJCSjh3eUpXb0FBSzRUQVEuIiwicm9sZXMiOlsiYWRtaW4iXSwic2NwIjoiY29uc29sZSIsInNpZCI6IjAwNWU1NDhhLThjZGUtOGI1Ny1kMTM4LTI4NmUzYTQ4YjAzZSIsInN1YiI6IklNMjJSZE1RbU1QX0U4UkdwbU12RThud3NwMjRKWDFab0JKUFV2SXg3OUUiLCJ0aWQiOiJiNTk2MmU2My05OGIyLTRkMDktYjcyYS0yNzc1MWFkYzQ1MjQiLCJ1bmlxdWVfbmFtZSI6InNlYmFzdGlhbkBxN3A0Lm9ubWljcm9zb2Z0LmNvbSIsInVwbiI6InNlYmFzdGlhbkBxN3A0Lm9ubWljcm9zb2Z0LmNvbSIsInV0aSI6IkFSaE0zQ3VXbTBTdmtuelF4MDFhQUEiLCJ2ZXIiOiIxLjAiLCJ4bXNfZnRkIjoiLTY0YTVEdl9LRUR5bmFGUWl6S0J1aklSNDlyM2U0WVJVeGVRTWdsQmFsOEJaWFZ5YjNCbGQyVnpkQzFrYzIxeiJ9.cU1kdeqqho-qAK7HcmRhaoZ66W-0WnRyirypFkbDXKy_rDDSxCArr5a7gcnl42ZDGbcQhli3RU5iYLZXCQ3s9P-mE0dePBHJITKX_zI1du--FdBUeT0zfcTaJDuHOVXjiqG0QhOjCpFpQoGaaQsBqb_41Z0rKOl-13Qg8QqSJlDOUbHyDOXGp2quU42rk8zMp_EUNKekSvxgn0yvfOkUCjfnfJ1-tM4OOYRGQkSlAvIqOv8ltWJgKWn5u8O6Dx1CK79hdE_YbWURJLTcwVZDjgNJea-tSruPoVTwYUFszzJSmjdhHE6NK9WyGNaQTRWBTDXlW5FT1fimhLN9IzIjrQ"})END";

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
      writer.s = str;
      if (ParseMessage(writer.s, &token)) { break; }
      std::cout << "Got: " << writer.s << std::endl;
    } else {
      std::cout << "Got err: " << writer.s << " (" << res2 << ")" << std::endl;
    }


    std::cerr << "Retrying in " << poll_interval << " seconds" << std::endl;
    std::this_thread::sleep_for(poll_interval);
  }


  std::cout << token.value << " | " << token.scope << std::endl;

  std::string_view scope = token.scope;

#if 0
  if (!scope.starts_with(app_uri)) {
    std::cerr << "bad scope" << std::endl;
    return 1;
  }

  scope.remove_prefix(app_uri.size());

  if (!scope.starts_with("/")) {
    std::cerr << "bad scope (2)" << std::endl;
    return 1;
  }

  scope.remove_prefix(1);

  // this is now the "actual" scope
  if (scope != "console") {
    std::cerr << "bad scope (3)" << std::endl;
    return 1;
  }
#else
  if (scope != app_uri) {
    std::cerr << "bad scope" << std::endl;
    return 1;
  }
#endif

  // an jwt token conists of three parts:
  // header.payload.signature

  auto [value, signature] = ExtractSignature(token.value);
  auto [header, paylod] = ExtractJwt(value);

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


  curl_easy_cleanup(curl);
}
