/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "lib/cli.h"
#include "lib/version.h"
#include "stored/backends/dedup/config.h"

#include <jansson.h>
#include <iostream>
#include <streambuf>
#include <string>
#include <fstream>
#include <vector>

namespace {

constexpr const char* bstring = "block_files";
constexpr const char* pstring = "part_files";
constexpr const char* dstring = "data_files";

constexpr const char* spath = "path";
constexpr const char* ssize = "size";
constexpr const char* sbsize = "block_size";
constexpr const char* sidx = "index";
constexpr const char* srdonly = "read_only";
constexpr const char* sstart = "start";
constexpr const char* send = "end";

constexpr auto debug_options = JSON_INDENT(2);

std::vector<char> ReadInput()
{
  return std::vector<char>{std::istreambuf_iterator<char>(std::cin),
                           std::istreambuf_iterator<char>()};
}

// WRITING

json_t* ParseJson(const char* str)
{
  json_error_t error;
  auto* json = json_loads(str, 0, &error);

  if (!json) {
    throw std::runtime_error("Could not parse string to json: "
                             + std::string{error.text} + " (line "
                             + std::to_string(error.line) + ").");
  }

  return json;
}

template <typename T> T LoadJson(json_t* val);

template <>
dedup::config::block_file LoadJson<dedup::config::block_file>(json_t* obj)
{
  if (!json_is_object(obj)) {
    throw std::runtime_error("Expected json object, got "
                             + std::string{json_dumps(obj, debug_options)});
  }

  auto* path = json_object_get(obj, spath);
  auto* start = json_object_get(obj, sstart);
  auto* end = json_object_get(obj, send);
  auto* idx = json_object_get(obj, sidx);

  if (!path || !json_is_string(path) || !start || !json_is_integer(start)
      || !end || !json_is_integer(end) || !idx || !json_is_integer(idx)) {
    throw std::runtime_error("Could not parse block_file from "
                             + std::string{json_dumps(obj, debug_options)});
  }

  if (json_object_size(obj) > 4) {
    throw std::runtime_error("Too many keys for block_file: "
                             + std::string{json_dumps(obj, debug_options)});
  }


  dedup::config::block_file bf{
      .relpath = json_string_value(path),
      .Start = static_cast<std::uint64_t>(json_integer_value(start)),
      .End = static_cast<std::uint64_t>(json_integer_value(end)),
      .Idx = static_cast<std::uint32_t>(json_integer_value(idx)),
  };

  return bf;
}

template <>
dedup::config::part_file LoadJson<dedup::config::part_file>(json_t* obj)
{
  if (!json_is_object(obj)) {
    throw std::runtime_error("Expected json object, got "
                             + std::string{json_dumps(obj, debug_options)});
  }

  auto* path = json_object_get(obj, spath);
  auto* start = json_object_get(obj, sstart);
  auto* end = json_object_get(obj, send);
  auto* idx = json_object_get(obj, sidx);

  if (!path || !json_is_string(path) || !start || !json_is_integer(start)
      || !end || !json_is_integer(end) || !idx || !json_is_integer(idx)) {
    throw std::runtime_error("Could not parse record_file from "
                             + std::string{json_dumps(obj, debug_options)});
  }

  if (json_object_size(obj) > 4) {
    throw std::runtime_error("Too many keys for record_file: "
                             + std::string{json_dumps(obj, debug_options)});
  }

  dedup::config::part_file pf{
      .relpath = json_string_value(path),
      .Start = static_cast<std::uint64_t>(json_integer_value(start)),
      .End = static_cast<std::uint64_t>(json_integer_value(end)),
      .Idx = static_cast<std::uint32_t>(json_integer_value(idx)),
  };

  return pf;
}

template <>
dedup::config::data_file LoadJson<dedup::config::data_file>(json_t* obj)
{
  if (!json_is_object(obj)) {
    throw std::runtime_error("Expected json object, got "
                             + std::string{json_dumps(obj, debug_options)});
  }

  auto* path = json_object_get(obj, spath);
  auto* size = json_object_get(obj, ssize);
  auto* block_size = json_object_get(obj, sbsize);
  auto* idx = json_object_get(obj, sidx);
  auto* read_only = json_object_get(obj, srdonly);

  if (!path || !json_is_string(path) || !size || !json_is_integer(size)
      || !block_size || !json_is_integer(block_size) || !idx
      || !json_is_integer(idx) || !read_only || !json_is_boolean(read_only)) {
    throw std::runtime_error("Could not parse data_file from "
                             + std::string{json_dumps(obj, debug_options)});
  }

  if (json_object_size(obj) > 5) {
    throw std::runtime_error("Too many keys for data_file: "
                             + std::string{json_dumps(obj, debug_options)});
  }

  dedup::config::data_file df{
      .relpath = json_string_value(path),
      .Size = static_cast<std::uint64_t>(json_integer_value(size)),
      .BlockSize = static_cast<std::uint64_t>(json_integer_value(block_size)),
      .Idx = static_cast<std::uint32_t>(json_integer_value(idx)),
      .ReadOnly = json_boolean_value(read_only),
  };

  return df;
}

template <typename T> std::vector<T> LoadJsonArray(json_t* arr)
{
  std::vector<T> vals;
  size_t idx;
  json_t* val;
  json_array_foreach(arr, idx, val) { vals.push_back(LoadJson<T>(val)); }

  if (json_array_size(arr) != vals.size()) {
    throw std::runtime_error("Could not convert every json value from: "
                             + std::string{json_dumps(arr, debug_options)});
  }

  return vals;
}

dedup::config json_to_conf(json_t* json)
{
  dedup::config conf;

  if (!json_is_object(json)) {
    throw std::runtime_error("Expected json object, got "
                             + std::string{json_dumps(json, debug_options)});
  }

  json_t* bfiles = json_object_get(json, bstring);
  json_t* pfiles = json_object_get(json, pstring);
  json_t* dfiles = json_object_get(json, dstring);

  if (!bfiles || !json_is_array(bfiles) || !pfiles || !json_is_array(pfiles)
      || !dfiles || !json_is_array(dfiles)) {
    throw std::runtime_error("Could not parse config from "
                             + std::string{json_dumps(json, debug_options)});
  }

  if (json_object_size(json) > 3) {
    throw std::runtime_error("Too many keys for config: "
                             + std::string{json_dumps(json, debug_options)});
  }

  conf.bfiles = LoadJsonArray<dedup::config::block_file>(bfiles);
  conf.pfiles = LoadJsonArray<dedup::config::part_file>(pfiles);
  conf.dfiles = LoadJsonArray<dedup::config::data_file>(dfiles);

  return conf;
}

void WriteOutput(const std::vector<char>& data)
{
  fwrite(data.data(), 1, data.size(), stdout);
}

/// READING

template <typename T> json_t* dump_json(const T&);

template <>
json_t* dump_json<dedup::config::block_file>(
    const dedup::config::block_file& bf)
{
  json_t* obj = json_object();

  if (!obj) { throw std::runtime_error("Could not allocate json object."); }

  if (json_object_set_new(obj, spath, json_string(bf.relpath.c_str()))
      || json_object_set_new(obj, sstart, json_integer(bf.Start))
      || json_object_set_new(obj, send, json_integer(bf.End))
      || json_object_set_new(obj, sidx, json_integer(bf.Idx))) {
    throw std::runtime_error("Could not create block file json object.\n");
  }

  return obj;
}
template <>
json_t* dump_json<dedup::config::part_file>(const dedup::config::part_file& pf)
{
  json_t* obj = json_object();

  if (!obj) { throw std::runtime_error("Could not allocate json object."); }

  if (json_object_set_new(obj, spath, json_string(pf.relpath.c_str()))
      || json_object_set_new(obj, sstart, json_integer(pf.Start))
      || json_object_set_new(obj, send, json_integer(pf.End))
      || json_object_set_new(obj, sidx, json_integer(pf.Idx))) {
    throw std::runtime_error("Could not create record file json object.\n");
  }

  return obj;
}
template <>
json_t* dump_json<dedup::config::data_file>(const dedup::config::data_file& df)
{
  json_t* obj = json_object();

  if (!obj) { throw std::runtime_error("Could not allocate json object."); }

  if (json_object_set_new(obj, spath, json_string(df.relpath.c_str()))
      || json_object_set_new(obj, ssize, json_integer(df.Size))
      || json_object_set_new(obj, sbsize, json_integer(df.BlockSize))
      || json_object_set_new(obj, sidx, json_integer(df.Idx))
      || json_object_set_new(obj, srdonly, json_boolean(df.ReadOnly))) {
    throw std::runtime_error("Could not create data file json object.\n");
  }

  return obj;
}

template <typename T> json_t* DumpJsonArray(const std::vector<T>& v)
{
  json_t* arr = json_array();

  if (!arr) { throw std::runtime_error("Could not allocate json array."); }

  for (auto& val : v) {
    if (json_array_append_new(arr, dump_json<T>(val))) {
      throw std::runtime_error("Could not append value to json array.");
    }
  }

  if (json_array_size(arr) != v.size()) {
    throw std::runtime_error("Could not append all values to json array (in = "
                             + std::to_string(v.size()) + ", out = "
                             + std::to_string(json_array_size(arr)) + ").");
  }

  return arr;
}

json_t* conf_to_json(const dedup::config& conf)
{
  json_t* json = json_object();

  if (!json) { throw std::runtime_error("Could not allocate json object."); }

  json_t* bfiles = DumpJsonArray(conf.bfiles);
  json_t* pfiles = DumpJsonArray(conf.pfiles);
  json_t* dfiles = DumpJsonArray(conf.dfiles);

  if (json_object_set_new(json, bstring, bfiles)
      || json_object_set_new(json, pstring, pfiles)
      || json_object_set_new(json, dstring, dfiles)) {
    throw std::runtime_error("Could not create config json object.");
  }

  return json;
}
};  // namespace

int main(int argc, const char* argv[])
{
  CLI::App app;
  std::string desc(1024, '\0');
  kBareosVersionStrings.FormatCopyright(desc.data(), desc.size(), 2023);
  desc += "The Bareos Dedup Config Viewer";
  InitCLIApp(app, desc, 0);

  auto* write = app.add_subcommand("write", "write config");
  auto* read = app.add_subcommand("read", "read config");
  app.require_subcommand(1);

  CLI11_PARSE(app, argc, argv);

  try {
    std::vector in = ReadInput();

    if (write->parsed()) {
      std::string s{in.begin(), in.end()};
      json_t* root = ParseJson(s.c_str());
      auto conf = json_to_conf(root);
      std::vector data = dedup::config::serialize(conf);
      WriteOutput(data);
    } else if (read->parsed()) {
      auto conf = dedup::config::deserialize(in.data(), in.size());
      auto* json = conf_to_json(conf);
      std::cout << json_dumps(json, JSON_COMPACT) << std::endl;
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Caught unexpected error: " << ex.what() << std::endl;
    return 1;
  }
}