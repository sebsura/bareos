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
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
#include "gtest/gtest.h"

#include <volume.h>
#include <string_view>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <fstream>
#include <filesystem>

inline constexpr std::size_t dedup_block_size = 16 * 1024;

using namespace dedup;

std::vector<char> LoadFile(const char* name)
{
  std::ifstream f{name};

  return std::vector<char>(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
};

struct raii_volume {
  raii_volume(std::string_view name) : name{name} {}

  raii_volume(const raii_volume&) = default;
  raii_volume& operator=(const raii_volume&) = default;
  raii_volume(raii_volume&&) = default;
  raii_volume& operator=(raii_volume&&) = default;

  ~raii_volume()
  {
    if (name.size()) { std::filesystem::remove_all(name); }
  }

  const char* path() { return name.c_str(); }

  std::string name;
};

config LoadConfig(std::string_view volname)
{
  std::string conf_path{volname};
  conf_path += "/config";
  std::vector<char> data = LoadFile(conf_path.c_str());

  return config::deserialize(data.data(), data.size());
}

raii_volume CreateVolume(std::string_view name,
                         std::size_t block_size = dedup_block_size)
{
  std::string dir{"dedup-vols/"};
  std::string vol = dir.append(name.begin(), name.end());
  using namespace dedup;

  volume::create_new(0700, vol.c_str(), block_size);

  auto conf = LoadConfig(vol);

  if (conf.dfiles.size() != 2) {
    throw std::runtime_error("CreateVolume: bad config (num datafiles != 2)");
  }

  std::vector<std::size_t> sizes;
  for (auto df : conf.dfiles) { sizes.push_back(df.BlockSize); }

  std::sort(sizes.begin(), sizes.end());

  if (sizes[0] != 1 || sizes[1] != block_size) {
    throw std::runtime_error("CreateVolume: bad config (wrong block sizes)");
  }

  return raii_volume{vol};
}

TEST(dedup_vol, abort)
{
  try {
    auto name = CreateVolume("abort-vol");

    {
      volume vol{volume::open_type::ReadWrite, name.path()};

      block_header hdr;

      auto save = vol.BeginBlock(hdr);

      record_header rec;
      vol.PushRecord(rec, nullptr, 0);
      vol.PushRecord(rec, nullptr, 0);
      vol.PushRecord(rec, nullptr, 0);
      vol.PushRecord(rec, nullptr, 0);
      vol.PushRecord(rec, nullptr, 0);
      vol.PushRecord(rec, nullptr, 0);

      vol.AbortBlock(std::move(save));
    }


    auto conf = LoadConfig(name.path());

    for (auto& bf : conf.bfiles) { EXPECT_EQ(bf.Start, bf.End); }
    for (auto& rf : conf.rfiles) { EXPECT_EQ(rf.Start, rf.End); }
  } catch (const std::exception& ex) {
    FAIL() << "Caught exception: " << ex.what() << std::endl;
  }
}

TEST(dedup_vol, commit)
{
  constexpr std::size_t num_records = 10;
  try {
    auto name = CreateVolume("abort-vol");

    {
      volume vol{volume::open_type::ReadWrite, name.path()};

      block_header hdr;

      auto save = vol.BeginBlock(hdr);

      record_header rec;

      for (std::size_t i = 0; i < num_records; ++i) {
        vol.PushRecord(rec, nullptr, 0);
      }

      vol.CommitBlock(std::move(save));
    }


    auto conf = LoadConfig(name.path());

    std::size_t actual_num_blocks = 0;
    for (auto& bf : conf.bfiles) { actual_num_blocks += bf.End - bf.Start; }

    std::size_t actual_num_records = 0;
    for (auto& rf : conf.rfiles) { actual_num_records += rf.End - rf.Start; }

    EXPECT_EQ(actual_num_blocks, 1);
    EXPECT_EQ(actual_num_records, num_records);

  } catch (const std::exception& ex) {
    FAIL() << "Caught exception: " << ex.what() << std::endl;
  }
}

TEST(dedup_vol, read_only_open)
{
  auto name = CreateVolume("abort-vol");

  try {
    volume vol{volume::open_type::ReadOnly, name.path()};

    block_header hdr;

    auto save = vol.BeginBlock(hdr);

    vol.CommitBlock(std::move(save));

    FAIL() << "written to read only volume" << std::endl;
  } catch (const std::exception& ex) {
    auto conf = LoadConfig(name.path());

    for (auto& bf : conf.bfiles) { ASSERT_EQ(bf.Start, bf.End); }
    for (auto& rf : conf.rfiles) { ASSERT_EQ(rf.Start, rf.End); }

    SUCCEED() << "Caught exception: " << ex.what() << std::endl;
  }
}
