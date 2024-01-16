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

std::vector<char> ReadInput()
{
  return std::vector<char>{std::istreambuf_iterator<char>(std::cin),
                           std::istreambuf_iterator<char>()};
}

// WRITING

json_t* LoadJson(const char* str)
{
  json_error_t error;
  auto* json = json_loads(str, 0, &error);

  if (!json) {
    std::cout << "error on line " << error.line << ". ERR=" << error.text
              << std::endl;
  }

  return json;
}

template <typename T> T LoadJson(json_t* val);

template <>
dedup::config::block_file LoadJson<dedup::config::block_file>(json_t* obj)
{
  if (!json_is_object(obj)) { exit(1); }
  auto* path = json_object_get(obj, "path");
  auto* start = json_object_get(obj, "start");
  auto* end = json_object_get(obj, "end");
  auto* idx = json_object_get(obj, "index");

  if (!path || !json_is_string(path) || !start || !json_is_integer(start)
      || !end || !json_is_integer(end) || !idx || !json_is_integer(idx)) {
    exit(1);
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
  if (!json_is_object(obj)) { exit(1); }
  auto* path = json_object_get(obj, "path");
  auto* start = json_object_get(obj, "start");
  auto* end = json_object_get(obj, "end");
  auto* idx = json_object_get(obj, "index");

  if (!path || !json_is_string(path) || !start || !json_is_integer(start)
      || !end || !json_is_integer(end) || !idx || !json_is_integer(idx)) {
    exit(1);
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
  if (!json_is_object(obj)) { exit(1); }
  auto* path = json_object_get(obj, "path");
  auto* size = json_object_get(obj, "size");
  auto* block_size = json_object_get(obj, "block_size");
  auto* idx = json_object_get(obj, "index");
  auto* read_only = json_object_get(obj, "read_only");

  if (!path || !json_is_string(path) || !size || !json_is_integer(size)
      || !block_size || !json_is_integer(block_size) || !idx
      || !json_is_integer(idx) || !read_only || !json_is_boolean(read_only)) {
    exit(1);
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

  return vals;
}

dedup::config json_to_conf(json_t* json)
{
  dedup::config conf;

  if (!json_is_object(json)) { exit(1); }

  json_t* bfiles = json_object_get(json, bstring);
  json_t* pfiles = json_object_get(json, pstring);
  json_t* dfiles = json_object_get(json, dstring);

  if (!bfiles || !json_is_array(bfiles) || !pfiles || !json_is_array(pfiles)
      || !dfiles || !json_is_array(dfiles)) {
    exit(1);
  }

  conf.bfiles = LoadJsonArray<dedup::config::block_file>(bfiles);
  conf.pfiles = LoadJsonArray<dedup::config::part_file>(pfiles);
  conf.dfiles = LoadJsonArray<dedup::config::data_file>(dfiles);

  json_object_del(json, bstring);
  json_object_del(json, pstring);
  json_object_del(json, dstring);

  const char* key;
  json_t* val;
  json_object_foreach(json, key, val)
  {
    std::cerr << "Unknown key " << key << "(with val " << json_dumps(val, 0)
              << ")" << std::endl;
  }

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

  if (json_object_set_new(obj, "path", json_string(bf.relpath.c_str()))
      || json_object_set_new(obj, "start", json_integer(bf.Start))
      || json_object_set_new(obj, "end", json_integer(bf.End))
      || json_object_set_new(obj, "index", json_integer(bf.Idx))) {
    exit(1);
  }

  return obj;
}
template <>
json_t* dump_json<dedup::config::part_file>(const dedup::config::part_file& pf)
{
  json_t* obj = json_object();

  if (json_object_set_new(obj, "path", json_string(pf.relpath.c_str()))
      || json_object_set_new(obj, "start", json_integer(pf.Start))
      || json_object_set_new(obj, "end", json_integer(pf.End))
      || json_object_set_new(obj, "index", json_integer(pf.Idx))) {
    exit(1);
  }

  return obj;
}
template <>
json_t* dump_json<dedup::config::data_file>(const dedup::config::data_file& df)
{
  json_t* obj = json_object();

  if (json_object_set_new(obj, "path", json_string(df.relpath.c_str()))
      || json_object_set_new(obj, "size", json_integer(df.Size))
      || json_object_set_new(obj, "block_size", json_integer(df.BlockSize))
      || json_object_set_new(obj, "index", json_integer(df.Idx))
      || json_object_set_new(obj, "read_only", json_boolean(df.ReadOnly))) {
    exit(1);
  }

  return obj;
}

template <typename T> json_t* DumpJsonArray(const std::vector<T>& v)
{
  json_t* arr = json_array();
  for (auto& val : v) {
    if (json_array_append_new(arr, dump_json<T>(val))) { exit(1); }
  }
  return arr;
}

json_t* conf_to_json(const dedup::config& conf)
{
  json_t* json = json_object();
  json_t* bfiles = DumpJsonArray(conf.bfiles);
  json_t* pfiles = DumpJsonArray(conf.pfiles);
  json_t* dfiles = DumpJsonArray(conf.dfiles);

  if (json_object_set_new(json, "block_files", bfiles)
      || json_object_set_new(json, "part_files", pfiles)
      || json_object_set_new(json, "data_files", dfiles)) {
    exit(1);
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

  if (write->parsed()) {
    std::vector in = ReadInput();
    std::string s{in.begin(), in.end()};
    json_t* root = LoadJson(s.c_str());
    if (!root) { return 1; }
    auto conf = json_to_conf(root);
    std::vector data = dedup::config::serialize(conf);
    WriteOutput(data);
  } else if (read->parsed()) {
    std::vector in = ReadInput();
    auto conf = dedup::config::deserialize(in.data(), in.size());
    auto* json = conf_to_json(conf);
    if (!json) { return 1; }
    std::cout << json_dumps(json, 0) << std::endl;
  }

  return 0;
}
