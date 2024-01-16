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

#include "config.h"
#include "util.h"

#include <stdexcept>

namespace dedup {
namespace {
template <typename T> using net = network_order::network<T>;

std::uint32_t SafeCast(std::size_t size)
{
  constexpr std::size_t max = std::numeric_limits<std::uint32_t>::max();
  if (size > max) {
    throw std::invalid_argument(std::to_string(size)
                                + " is bigger than allowed ("
                                + std::to_string(max) + ").");
  }

  return size;
}

struct net_string {
  net_u32 Start;
  net_u32 Size;
  net_string() = default;
  net_string(std::vector<char>& string_area,
             const char* data,
             std::uint32_t size)
      : Start{SafeCast(string_area.size())}, Size{size}
  {
    string_area.insert(string_area.end(), data, data + size);
  }
  std::string unserialize(std::string_view string_area)
  {
    if (string_area.size() < std::size_t{Start} + Size) {
      throw std::runtime_error("string area too small (size="
                               + std::to_string(string_area.size())
                               + ", want= [" + std::to_string(Start) + ", "
                               + std::to_string(Start + Size) + "])");
    }
    return std::string{string_area.substr(Start, Size)};
  }
};

struct serializable_block_file {
  net_string RelPath;
  net<decltype(config::block_file::Start)> Start;
  net<decltype(config::block_file::End)> End;
  net<decltype(config::block_file::Idx)> Idx;

  serializable_block_file() = default;
  serializable_block_file(const config::block_file& bf,
                          std::vector<char>& string_area)
      : RelPath(string_area, bf.relpath.data(), bf.relpath.size())
      , Start{bf.Start}
      , End{bf.End}
      , Idx{bf.Idx}
  {
  }

  config::block_file unserialize(std::string_view string_area)
  {
    return {RelPath.unserialize(string_area), Start, End, Idx};
  }
};
struct serializable_part_file {
  net_string RelPath;
  net<decltype(config::part_file::Start)> Start;
  net<decltype(config::part_file::End)> End;
  net<decltype(config::part_file::Idx)> Idx;

  serializable_part_file() = default;
  serializable_part_file(const config::part_file& rf,
                         std::vector<char>& string_area)
      : RelPath(string_area, rf.relpath.data(), rf.relpath.size())
      , Start{rf.Start}
      , End{rf.End}
      , Idx{rf.Idx}
  {
  }

  config::part_file unserialize(std::string_view string_area)
  {
    return {RelPath.unserialize(string_area), Start, End, Idx};
  }
};
struct serializable_data_file {
  net_string RelPath;
  net<decltype(config::data_file::Size)> Size;
  net<decltype(config::data_file::BlockSize)> BlockSize;
  net<decltype(config::data_file::Idx)> Idx;
  bool ReadOnly;

  serializable_data_file() = default;
  serializable_data_file(const config::data_file& df,
                         std::vector<char>& string_area)
      : RelPath(string_area, df.relpath.data(), df.relpath.size())
      , Size{df.Size}
      , BlockSize{df.BlockSize}
      , Idx{df.Idx}
      , ReadOnly{df.ReadOnly}
  {
  }
  config::data_file unserialize(std::string_view string_area)
  {
    return {
        RelPath.unserialize(string_area), Size, BlockSize, Idx, ReadOnly,
    };
  }
};

struct config_header {
  enum version : std::uint64_t
  {
    v0,  // for testing purposes if needed
    v1,
  };
  net_u64 version{version::v0};
  net<std::uint32_t> string_size{};
  net<std::uint32_t> num_blockfiles{};
  net<std::uint32_t> num_partfiles{};
  net<std::uint32_t> num_datafiles{};
};
};  // namespace


std::vector<char> config::serialize(const config& conf)
{
  std::vector<char> serial;

  config_header hdr;
  hdr.version = config_header::version::v1;

  std::vector<serializable_block_file> bfs;
  for (auto bfile : conf.bfiles) { bfs.emplace_back(bfile, serial); }
  std::vector<serializable_part_file> pfs;
  for (auto pfile : conf.pfiles) { pfs.emplace_back(pfile, serial); }
  std::vector<serializable_data_file> dfs;
  for (auto dfile : conf.dfiles) { dfs.emplace_back(dfile, serial); }

  hdr.string_size = serial.size();

  for (auto& bf : bfs) {
    auto* as_char = reinterpret_cast<const char*>(&bf);
    serial.insert(serial.end(), as_char, as_char + sizeof(bf));
    hdr.num_blockfiles = hdr.num_blockfiles + 1;
  }
  for (auto& pf : pfs) {
    auto* as_char = reinterpret_cast<const char*>(&pf);
    serial.insert(serial.end(), as_char, as_char + sizeof(pf));
    hdr.num_partfiles = hdr.num_partfiles + 1;
  }
  for (auto& df : dfs) {
    auto* as_char = reinterpret_cast<const char*>(&df);
    serial.insert(serial.end(), as_char, as_char + sizeof(df));
    hdr.num_datafiles = hdr.num_datafiles + 1;
  }

  {
    auto* as_char = reinterpret_cast<const char*>(&hdr);
    serial.insert(serial.begin(), as_char, as_char + sizeof(hdr));
  }

  return serial;
}

config config::make_default(std::uint64_t BlockSize)
{
  return config{
    .bfiles = {
      {"blocks", 0, 0, 0},
    },
    .pfiles = {
      {"parts", 0, 0, 0},
    },
    .dfiles = {
      {"aligned.data", 0, BlockSize, 0, false},
      {"unaligned.data", 0, 1, 1, false},
    }
  };
}
namespace {
config deserialize_config_v1(chunked_reader stream, config_header& hdr)
{
  config conf;

  if (hdr.version != config_header::version::v1) {
    throw std::runtime_error(
        "Internal error: trying to deserialize wrong config version.");
  }

  if (hdr.num_blockfiles != 1) {
    throw std::runtime_error("bad config file (num blockfiles != 1)");
  }
  if (hdr.num_partfiles != 1) {
    throw std::runtime_error("bad config file (num partfiles != 1)");
  }
  if (hdr.num_datafiles != 2) {
    throw std::runtime_error("bad config file (num datafiles != 2)");
  }

  const char* string_begin = stream.get(hdr.string_size);
  if (!string_begin) { throw std::runtime_error("config file to small."); }
  std::string_view string_area(string_begin, hdr.string_size);

  {
    serializable_block_file bf;
    if (!stream.read(&bf, sizeof(bf))) {
      throw std::runtime_error("config file to small.");
    }

    conf.bfiles.push_back(bf.unserialize(string_area));
  }
  {
    serializable_part_file pf;
    if (!stream.read(&pf, sizeof(pf))) {
      throw std::runtime_error("config file to small.");
    }

    conf.pfiles.push_back(pf.unserialize(string_area));
  }

  for (std::size_t i = 0; i < hdr.num_datafiles; ++i) {
    serializable_data_file df;
    if (!stream.read(&df, sizeof(df))) {
      throw std::runtime_error("config file to small.");
    }

    conf.dfiles.push_back(df.unserialize(string_area));
  }

  if (!stream.finished()) { throw std::runtime_error("config file to big."); }

  return conf;
}
};  // namespace

config config::deserialize(const char* data, std::size_t size)
{
  chunked_reader stream(data, size);

  config_header hdr;
  if (!stream.read(&hdr, sizeof(hdr))) {
    throw std::runtime_error("config file to small.");
  }

  switch (hdr.version.load()) {
    case config_header::version::v1: {
      return deserialize_config_v1(std::move(stream), hdr);
    } break;
    default: {
      throw std::runtime_error("bad config version (version = "
                               + std::to_string(hdr.version.load()) + ")");
    }
  }
}
};  // namespace dedup
