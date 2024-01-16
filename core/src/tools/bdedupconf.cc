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

#include <jansson.h>

// #include "stored/backends/dedup/volume.h"

// void print_config(dedup::config& cfg)
// {

// }

#include <iostream>
#include <streambuf>
#include <string>
#include <filesystem>

std::string ReadInput()
{
  return std::string{std::istreambuf_iterator<char>(std::cin),
                     std::istreambuf_iterator<char>()};
}

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

void WriteConfig(const char* file, json_t* root)
{
  auto* str = json_dumps(root, 0);
  std::cout << "Dumping into " << file << ":\n" << str << std::endl;
}

int main(int argc, const char* argv[])
{
  namespace stdfs = std::filesystem;

  CLI::App app;
  std::string desc(1024, '\0');
  kBareosVersionStrings.FormatCopyright(desc.data(), desc.size(), 2023);
  desc += "The Bareos Dedup Config Viewer";
  InitCLIApp(app, desc, 0);

  std::string volume;
  auto* write = app.add_subcommand("write", "write config");
  write->add_option("volume", volume)
      ->check(CLI::ExistingDirectory)
      ->required();
  auto* read = app.add_subcommand("read", "read config");
  read->add_option("volume", volume)->check(CLI::ExistingDirectory)->required();
  app.require_subcommand(1);

  CLI11_PARSE(app, argc, argv);

  auto conf = stdfs::path{volume} / "config";


  if (write->parsed()) {
    std::cout << "writing to " << volume << std::endl;
    std::string s = ReadInput();
    json_t* root = LoadJson(s.c_str());
    if (root) { WriteConfig(conf.c_str(), root); }
  } else if (read->parsed()) {
    std::cout << "reading from " << volume << std::endl;
  }

  return 0;
}
