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
#define private public
#include "stored/backends/dedup/volume.h"

#include <jansson.h>
#include <iostream>
#include <streambuf>
#include <string>
#include <fstream>
#include <vector>
#include <filesystem>

namespace {
json_t* part_to_json(dedup::part& part)
{
  auto* json = json_object();

  json_object_set_new(json, "idx", json_integer(part.FileIdx));
  json_object_set_new(json, "begin", json_integer(part.Begin));
  json_object_set_new(json, "size", json_integer(part.Size));

  return json;
}
};  // namespace

int main(int argc, const char* argv[])
{
  namespace stdfs = std::filesystem;

  CLI::App app;
  std::string desc(1024, '\0');
  kBareosVersionStrings.FormatCopyright(desc.data(), desc.size(), 2023);
  desc += "The Bareos Dedup Config Viewer";
  InitCLIApp(app, desc, 0);

  std::string vol;
  app.add_option("volume", vol, "the volume")->required();

  CLI11_PARSE(app, argc, argv);

  try {
    dedup::volume volume{dedup::volume::open_type::ReadOnly, vol.c_str()};

    auto& parts = volume.backing->parts;
    std::cout << "[" << std::endl;
    bool first = true;
    for (auto& part : parts) {
      auto json = part_to_json(part);
      auto txt = json_dumps(json, 0);

      if (!first) {
        std::cout << ",";
      } else {
        first = false;
      }

      std::cout << txt;
      free(txt);
      json_decref(json);
    }
    std::cout << "]" << std::endl;
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "Caught unexpected error: " << ex.what() << std::endl;
    return 1;
  }
}
