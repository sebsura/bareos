/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2024 Bareos GmbH & Co. KG

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

extern "C" {
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
}

#include <system_error>
#include <cerrno>

#include "volume.h"
#include "util.h"

namespace dedup {
namespace {
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

std::vector<char> LoadFile(int fd)
{
  std::vector<char> loaded;

  auto current_size = loaded.size();
  for (;;) {
    auto next_size = current_size + 1024 * 1024;

    loaded.resize(next_size);

    auto res = read(fd, loaded.data() + current_size, next_size - current_size);

    if (res < 0) {
      std::string errctx = "while reading";
      throw std::system_error(errno, std::generic_category(), errctx);
    } else if (res == 0) {
      break;
    } else {
      current_size += res;
    }
  }

  loaded.resize(current_size);
  return loaded;
};

void WriteFile(int fd, const std::vector<char>& written)
{
  std::size_t progress = 0;
  while (progress < written.size()) {
    auto res = write(fd, written.data() + progress, written.size() - progress);
    if (res < 0) {
      std::string errctx = "while writing";
      throw std::system_error(errno, std::generic_category(), errctx);
    } else if (res == 0) {
      break;
    } else {
      progress += res;
    }
  }
}

int OpenRelative(open_context ctx, const char* path)
{
  int fd = openat(ctx.dird, path, ctx.flags);

  if (fd < 0) {
    std::string errctx = "while opening '";
    errctx += path;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  return fd;
}

block to_dedup(block_header header, std::uint64_t Begin, std::uint32_t Count)
{
  return block{
      .CheckSum = header.CheckSum,
      .BlockSize = header.BlockSize,
      .BlockNumber = header.BlockNumber,
      .ID = {header.ID[0], header.ID[1], header.ID[2], header.ID[3]},
      .VolSessionId = header.VolSessionId,
      .VolSessionTime = header.VolSessionTime,
      .Count = Count,
      .Begin = Begin,
  };
}
record to_dedup(record_header header,
                std::uint32_t FileIdx,
                std::uint64_t Begin,
                std::uint32_t Size)
{
  return record{
      .FileIndex = header.FileIndex,
      .Stream = header.Stream,
      .DataSize = header.DataSize,
      .Padding = 0,
      .FileIdx = FileIdx,
      .Size = Size,
      .Begin = Begin,
  };
}
block_header from_dedup(block b)
{
  return block_header{
      .CheckSum = b.CheckSum,
      .BlockSize = b.BlockSize,
      .BlockNumber = b.BlockNumber,
      .ID = {b.ID[0], b.ID[1], b.ID[2], b.ID[3]},
      .VolSessionId = b.VolSessionId,
      .VolSessionTime = b.VolSessionTime,
  };
}
record_header from_dedup(record r)
{
  return record_header{
      .FileIndex = r.FileIndex,
      .Stream = r.Stream,
      .DataSize = r.DataSize,
  };
}

auto FindDataIdx(const data::bsize_map& map, std::uint64_t size)
{
  for (auto [bsize, idx] : map) {
    if (size % bsize == 0) { return idx; }
  }

  throw std::runtime_error(
      "Could not find an appropriate data file for "
      "record of size "
      + std::to_string(size));
}
};  // namespace

volume::volume(open_type type, const char* path) : sys_path{path}
{
  bool read_only = type == open_type::ReadOnly;
  int flags = (read_only) ? O_RDONLY : O_RDWR;
  int dir_flags = O_RDONLY | O_DIRECTORY;
  dird = open(path, dir_flags);

  if (dird < 0) {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  if (raii_fd conf_fd = openat(dird, "config", flags); conf_fd) {
    conf.emplace(LoadFile(conf_fd.fileno()));
  } else {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "/config'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  backing.emplace(
      open_context{
          .read_only = read_only,
          .flags = flags,
          .dird = dird,
      },
      conf.value());
}

data::data(open_context ctx, const config& conf)
{
  auto& bf = conf.blockfile();
  if (bf.Start != 0) { throw std::runtime_error("blockfile start != 0."); }

  auto& rf = conf.recordfile();
  if (rf.Start != 0) { throw std::runtime_error("recordfile start != 0."); }

  raii_fd bfd = OpenRelative(ctx, bf.relpath.c_str());
  raii_fd rfd = OpenRelative(ctx, rf.relpath.c_str());
  blocks = decltype(blocks){ctx.read_only, bfd.fileno(), bf.End};
  records = decltype(records){ctx.read_only, rfd.fileno(), rf.End};
  fds.emplace_back(std::move(bfd));
  fds.emplace_back(std::move(rfd));

  auto& dfs = conf.datafiles();

  for (auto& df : dfs) {
    raii_fd& fd = fds.emplace_back(OpenRelative(ctx, df.relpath.c_str()));
    if (!ctx.read_only && df.ReadOnly) {
      throw std::runtime_error("file '" + df.relpath + "' is readonly,"
			       " but write permissions requested.");
    }
    auto idx = datafiles.size();
    datafiles.emplace_back(ctx.read_only, fd.fileno(), df.Size);

    idx_to_dfile[df.Idx] = idx;
    bsize_to_idx[df.BlockSize] = df.Idx;
  }
}

void volume::update_config()
{
  conf->blockfile().End = backing->blocks.size();
  conf->recordfile().End = backing->records.size();
  for (auto& dfile : conf->datafiles()) {
    auto dfile_idx = backing->idx_to_dfile.at(dfile.Idx);
    dfile.Size = backing->datafiles[dfile_idx].size();
  }

  auto serialized = conf->serialize();

  raii_fd conf_fd = openat(dird, "config", O_WRONLY);

  if (!conf_fd) {
    std::string errctx = "Could not open dedup config file";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  WriteFile(conf_fd.fileno(), serialized);
}

save_state volume::BeginBlock()
{
  save_state s;

  s.block_size = backing->blocks.size();
  s.record_size = backing->records.size();

  for (auto& vec : backing->datafiles) { s.data_sizes.push_back(vec.size()); }

  return s;
}

void volume::CommitBlock(save_state s, block_header header)
{
  auto start = s.record_size;
  auto count = backing->records.size() - s.record_size;
  backing->blocks.push_back(to_dedup(header, start, count));

  update_config();
}

void volume::AbortBlock(save_state s)
{
  backing->blocks.resize_uninitialized(s.block_size);
  backing->records.resize_uninitialized(s.record_size);
  ASSERT(s.data_sizes.size() == backing->datafiles.size());
  for (std::size_t i = 0; i < s.data_sizes.size(); ++i) {
    backing->datafiles[i].resize_uninitialized(s.data_sizes[i]);
  }
}

void volume::PushRecord(record_header header,
                        const char* data,
                        std::size_t size)
{
  auto idx = FindDataIdx(backing->bsize_to_idx, header.DataSize);

  auto& vec = backing->datafiles[backing->idx_to_dfile[idx]];

  auto start = vec.size();
  vec.reserve_extra(header.DataSize);
  vec.append_range(data, size);
  backing->records.push_back(to_dedup(header, idx, start, size));
}

void volume::create_new(int creation_mode,
                        const char* path,
                        std::size_t blocksize)
{
  int dir_mode
      = creation_mode | S_IXUSR;  // directories need execute permissions

  if (mkdir(path, dir_mode) < 0) {
    std::string errctx = "Cannot create directory: '";
    errctx += path;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  int flags = O_RDWR | O_CREAT;
  int dir_flags = O_RDONLY | O_DIRECTORY;
  raii_fd dird = open(path, dir_flags);

  if (!dird) {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }

  config def{blocksize};

  auto data = def.serialize();

  raii_fd conf_fd = openat(dird.fileno(), "config", flags, creation_mode);
  if (!conf_fd) {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "/config'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }
  WriteFile(conf_fd.fileno(), data);

  if (raii_fd block_fd = openat(dird.fileno(), def.blockfile().relpath.c_str(),
                                flags, creation_mode);
      !block_fd) {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "/";
    errctx += def.blockfile().relpath;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }
  if (raii_fd record_fd
      = openat(dird.fileno(), def.recordfile().relpath.c_str(), flags,
               creation_mode);
      !record_fd) {
    std::string errctx = "Cannot open '";
    errctx += path;
    errctx += "/";
    errctx += def.recordfile().relpath;
    errctx += "'";
    throw std::system_error(errno, std::generic_category(), errctx);
  }
  for (auto& dfile : def.datafiles()) {
    if (raii_fd block_fd
        = openat(dird.fileno(), dfile.relpath.c_str(), flags, creation_mode);
        !block_fd) {
      std::string errctx = "Cannot open '";
      errctx += path;
      errctx += "/";
      errctx += dfile.relpath;
      errctx += "'";
      throw std::system_error(errno, std::generic_category(), errctx);
    }
  }
}

void volume::reset()
{
  backing->blocks.clear();
  backing->records.clear();
  for (auto& vec : backing->datafiles) { vec.clear(); }

  update_config();
}

void volume::flush()
{
  backing->blocks.flush();
  backing->records.flush();
  for (auto& vec : backing->datafiles) { vec.flush(); }
}

std::size_t volume::ReadBlock(std::size_t blocknum,
                              void* data,
                              std::size_t size)
{
  if (backing->blocks.size() == blocknum) {
    // trying to read one past the end is ok.  Just return 0 here to signal
    // that the volume has reached its end.
    return 0;
  } else if (backing->blocks.size() < blocknum) {
    throw std::invalid_argument("blocknum is out of bounds ("
                                + std::to_string(blocknum) + " > "
                                + std::to_string(backing->blocks.size()) + ")");
  }

  chunked_writer stream(data, size);

  auto block = backing->blocks[blocknum];
  auto begin = block.Begin.load();
  auto end = begin + block.Count;

  if (backing->records.size() < end) {
    throw std::runtime_error("Trying to read records [" + std::to_string(begin)
                             + ", " + std::to_string(end) + ") but only "
                             + std::to_string(backing->records.size())
                             + " records exist.");
  }

  block_header header = from_dedup(block);

  if (!stream.write(&header, sizeof(header))) { return 0; }

  for (auto cur = begin; cur != end; ++cur) {
    auto record = backing->records[cur];
    auto rheader = from_dedup(record);

    auto didx = record.FileIdx.load();

    auto dfile = backing->idx_to_dfile.find(didx);
    if (dfile == backing->idx_to_dfile.end()) {
      throw std::runtime_error("Trying to read from unknown file index "
                               + std::to_string(didx)
                               + "; known file indices are ...");
    }

    if (!stream.write(&rheader, sizeof(rheader))) { return 0; }

    auto dbegin = record.Begin.load();
    auto dsize = record.Size.load();

    auto& vec = backing->datafiles[dfile->second];

    if (vec.size() < dbegin + dsize) {
      throw std::runtime_error(
          "Trying to read region [" + std::to_string(dbegin) + ", "
          + std::to_string(dbegin + dsize) + ") from file __ but only"
          + std::to_string(vec.size()) + " bytes are used.");
    }

    if (!stream.write(vec.data() + dbegin, dsize)) { return 0; }
  }

  return size - stream.leftover();
}

namespace {
template <typename T> using net = network_order::network<T>;

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
struct serializable_record_file {
  net_string RelPath;
  net<decltype(config::record_file::Start)> Start;
  net<decltype(config::record_file::End)> End;
  net<decltype(config::record_file::Idx)> Idx;

  serializable_record_file() = default;
  serializable_record_file(const config::record_file& rf,
                           std::vector<char>& string_area)
      : RelPath(string_area, rf.relpath.data(), rf.relpath.size())
      , Start{rf.Start}
      , End{rf.End}
      , Idx{rf.Idx}
  {
  }

  config::record_file unserialize(std::string_view string_area)
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
  net<std::uint32_t> string_size{};
  net<std::uint32_t> num_blockfiles{};
  net<std::uint32_t> num_recordfiles{};
  net<std::uint32_t> num_datafiles{};
};
};  // namespace


std::vector<char> config::serialize()
{
  std::vector<char> serial;

  config_header hdr;

  serializable_block_file bf{bfile, serial};
  serializable_record_file rf{rfile, serial};
  std::vector<serializable_data_file> dfs;
  for (auto dfile : dfiles) { dfs.emplace_back(dfile, serial); }

  hdr.string_size = serial.size();

  {
    auto* as_char = reinterpret_cast<const char*>(&bf);
    serial.insert(serial.end(), as_char, as_char + sizeof(bf));
    hdr.num_blockfiles = hdr.num_blockfiles + 1;
  }
  {
    auto* as_char = reinterpret_cast<const char*>(&rf);
    serial.insert(serial.end(), as_char, as_char + sizeof(rf));
    hdr.num_recordfiles = hdr.num_recordfiles + 1;
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

config::config(const std::vector<char>& serial)
{
  chunked_reader stream(serial.data(), serial.size());

  config_header hdr;
  if (!stream.read(&hdr, sizeof(hdr))) {
    throw std::runtime_error("config file to small.");
  }

  if (hdr.num_blockfiles != 1) {
    throw std::runtime_error("bad config file (num blockfiles != 1)");
  }
  if (hdr.num_recordfiles != 1) {
    throw std::runtime_error("bad config file (num recordfiles != 1)");
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

    bfile = bf.unserialize(string_area);
  }
  {
    serializable_record_file rf;
    if (!stream.read(&rf, sizeof(rf))) {
      throw std::runtime_error("config file to small.");
    }

    rfile = rf.unserialize(string_area);
  }

  for (std::size_t i = 0; i < hdr.num_datafiles; ++i) {
    serializable_data_file df;
    if (!stream.read(&df, sizeof(df))) {
      throw std::runtime_error("config file to small.");
    }

    dfiles.push_back(df.unserialize(string_area));
  }

  if (!stream.finished()) { throw std::runtime_error("config file to big."); }
}
};  // namespace dedup
