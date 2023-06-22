/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

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

#include "dedup_volume.h"
#include <algorithm>

namespace dedup {
constexpr config::loaded_general_info my_general_info = {
    .block_header_size = sizeof(bareos_block_header),
    .record_header_size = sizeof(bareos_record_header),
    .dedup_block_header_size = sizeof(block_header),
    .dedup_record_header_size = sizeof(record_header),
};

void volume::write_current_config()
{
  std::vector<config::loaded_block_section> blocksections;
  std::vector<config::loaded_record_section> recordsections;
  std::vector<config::loaded_data_section> datasections;


  for (auto&& blockfile : config.blockfiles) {
    blocksections.emplace_back(blockfile.start_block, blockfile.num_blocks,
                               blockfile.path);
  }
  for (auto&& recordfile : config.recordfiles) {
    recordsections.emplace_back(recordfile.start_record, recordfile.num_records,
                                recordfile.path);
  }
  for (auto&& datafile : config.datafiles) {
    datasections.emplace_back(datafile.file_index, datafile.block_size,
                              datafile.path);
  }

  std::vector<std::byte> bytes = config::to_bytes(
      my_general_info, datasections, recordsections, blocksections);
  if (ftruncate(configfile.fd.get(), 0) != 0) {
    error = true;
  } else if (::lseek(configfile.fd.get(), 0, SEEK_SET) != 0) {
    error = true;
  } else if (write(configfile.fd.get(), &bytes.front(), bytes.size())
             != static_cast<ssize_t>(bytes.size())) {
    error = true;
  }
}

bool volume::load_config()
{
  auto config_end = lseek(configfile.fd.get(), 0, SEEK_END);
  auto config_start = lseek(configfile.fd.get(), 0, SEEK_SET);

  if (config_start != 0 || config_start > config_end) {
    // error: cannot determine config file size
    return false;
  }

  std::vector<std::byte> bytes(config_end - config_start);

  if (read(configfile.fd.get(), &bytes.front(), bytes.size())
      != static_cast<ssize_t>(bytes.size())) {
    // error: cannot read config file
    return false;
  }

  std::optional loaded_config = config::from_bytes(bytes);
  if (!loaded_config) { return false; }

  // at the moment we only support configurations that have
  // exactly one block and one record file.
  // This might change in the future
  if (loaded_config->blockfiles.size() != 1) {
    // error: to many/few block files
    return false;
  }

  if (loaded_config->recordfiles.size() != 1) {
    // error: to many/few record files
    return false;
  }

  if (loaded_config->info.block_header_size != sizeof(bareos_block_header)
      || loaded_config->info.dedup_block_header_size != sizeof(block_header)) {
    // error: bad block header size
    return false;
  }

  if (loaded_config->info.record_header_size != sizeof(bareos_record_header)
      || loaded_config->info.dedup_record_header_size
             != sizeof(record_header)) {
    // error: bad record header size
    return false;
  }

  config = volume_config(std::move(loaded_config.value()));
  return true;
}

bool block_file::write(const bareos_block_header& header,
                       std::uint64_t start_record,
                       std::uint32_t num_records)
{
  block_header dedup{header, start_record, num_records};

  if (!volume_file::write(&dedup, sizeof(dedup))) { return false; }

  current_block += 1;
  num_blocks = current_block;
  return true;
}

bool record_file::write(const bareos_record_header& header,
                        std::uint64_t payload_start,
                        std::uint64_t payload_end,
                        std::uint32_t file_index)
{
  record_header dedup{header, payload_start, payload_end, file_index};

  if (!volume_file::write(&dedup, sizeof(dedup))) { return false; }

  current_record += 1;
  num_records = current_record;
  return true;
}


volume::volume(const char* path, DeviceMode dev_mode, int mode)
    : path(path), configfile{"config"}
{
  // to create files inside dir, we need executive permissions
  int dir_mode = mode | 0100;
  if (struct stat st; (dev_mode == DeviceMode::CREATE_READ_WRITE)
                      && (::stat(path, &st) == -1)) {
    if (mkdir(path, mode | 0100) < 0) {
      error = true;
      return;
    }

  } else {
    dev_mode = DeviceMode::OPEN_READ_WRITE;
  }

  dir = raii_fd(path, O_RDONLY | O_DIRECTORY, dir_mode);


  if (!dir.is_ok()) {
    error = true;
    return;
  }

  if (dev_mode == DeviceMode::OPEN_WRITE_ONLY) {
    // we always need to read the config file
    configfile.open_inside(dir, mode, DeviceMode::OPEN_READ_WRITE);
  } else {
    configfile.open_inside(dir, mode, dev_mode);
  }

  if (!configfile.is_ok()) {
    error = true;
    return;
  }

  if (dev_mode == DeviceMode::CREATE_READ_WRITE) {
    volume_changed = true;
  } else {
    if (!load_config()) {
      error = true;
      return;
    }
  }

  for (auto& blockfile : config.blockfiles) {
    if (!blockfile.open_inside(dir, mode, dev_mode)) {
      error = true;
      return;
    }
  }

  for (auto& recordfile : config.recordfiles) {
    if (!recordfile.open_inside(dir, mode, dev_mode)) {
      error = true;
      return;
    }
  }

  for (auto& datafile : config.datafiles) {
    if (!datafile.open_inside(dir, mode, dev_mode)) {
      error = true;
      return;
    }
  }
}

std::uint64_t volume::next_record_idx() { return 0; }

data_file& volume::get_data_file_by_size(std::uint32_t record_size)
{
  // we have to do this smarter
  // if datafile::any_size is first, we should ignore it until the end!
  // maybe split into _one_ any_size + map size -> file
  // + vector of read_only ?
  data_file* best = nullptr;
  for (auto& datafile : config.datafiles) {
    if (datafile.accepts_records_of_size(record_size)) {
      if (!best || best->block_size < datafile.block_size) { best = &datafile; }
    }
  }

  // best is never null here because we always have at least one
  // datafile with data_file::any_size
  return *best;
}

bool volume::reset()
{
  // TODO: look at unix_file_device for "secure_erase_cmdline"
  for (auto& blockfile : config.blockfiles) {
    if (!blockfile.truncate()) { return false; }
  }

  for (auto& recordfile : config.recordfiles) {
    if (!recordfile.truncate()) { return false; }
  }

  for (auto& datafile : config.datafiles) {
    if (!datafile.truncate()) { return false; }
  }
  return true;
}

bool volume::goto_begin()
{
  for (auto& blockfile : config.blockfiles) {
    if (!blockfile.goto_begin()) { return false; }
  }

  for (auto& recordfile : config.recordfiles) {
    if (!recordfile.goto_begin()) { return false; }
  }

  for (auto& datafile : config.datafiles) {
    if (!datafile.goto_begin()) { return false; }
  }
  return true;
}

bool volume::goto_block(std::uint64_t block_num)
{
  return config.goto_block(block_num);
}

bool volume::goto_end()
{
  for (auto& blockfile : config.blockfiles) {
    if (!blockfile.goto_end()) { return false; }
  }
  for (auto& recordfile : config.recordfiles) {
    if (!recordfile.goto_end()) { return false; }
  }
  for (auto& datafile : config.datafiles) {
    if (!datafile.goto_end()) { return false; }
  }
  return true;
}
std::optional<block_header> volume::read_block()
{
  // auto& blockfile = get_active_block_file();

  // return blockfile.read_block();
  return std::nullopt;
}
void volume::revert_to_record(std::uint64_t) {}
std::optional<std::uint64_t> volume::write_record(...) { return std::nullopt; }
std::optional<std::uint64_t> volume::write_block(...) { return std::nullopt; }
std::optional<record_header> volume::read_record(std::uint64_t record_index)
{
  // müssen wir hier überhaupt das record bewegen ?
  // wenn eod, bod & reposition das richtige machen, sollte man
  // immer bei der richtigen position sein

  auto lower = std::lower_bound(
      config.recordfiles.rbegin(), config.recordfiles.rend(), record_index,
      [](const auto& lhs, const auto& rhs) { return lhs.start_record > rhs; });
  // lower "points" to the last block that has start_record <= record_index
  // one invariant of our class is that there is always a record file
  // that starts at 0, so we know that lower was always found
  // to make doubly sure we still check
  if (lower == config.recordfiles.rend()) {
    // can never be true
    return std::nullopt;
  }

  return lower->read_record(record_index);
}
bool volume::read_data(std::uint32_t file_index,
                       std::uint64_t start,
                       std::uint64_t end,
                       write_buffer& buf)
{
  if (file_index > config.datafiles.size()) { return false; }

  auto& data_file = config.datafiles[file_index];

  // todo: check we are in the right position
  char* data = buf.reserve(end - start);
  if (!data) { return false; }
  if (!data_file.read_data(data, start, end)) { return false; }
  return true;
}

bool volume_config::goto_record(std::uint64_t record_idx)
{
  std::uint64_t max_record = recordfiles.back().last_record();
  if (record_idx >= max_record) { return false; }

  auto lower = std::lower_bound(
      recordfiles.rbegin(), recordfiles.rend(), record_idx,
      [](const auto& lhs, const auto& rhs) { return lhs.start_record > rhs; });
  // lower "points" to the last record that has start_record <= record_index
  // one invariant of our class is that there is always a record file
  // that starts at 0, so we know that lower was always found
  // to make doubly sure we still check
  if (lower == recordfiles.rend()) {
    // can never happen (unless the object is in a bad state)
    return false;
  }

  return lower->goto_record(record_idx);
}
bool volume_config::goto_block(std::uint64_t block_idx)
{
  // todo: if we are not at the end of the device
  //       we should read the block header and position
  //       the record and data files as well
  //       otherwise set the record and data files to their respective end

  std::uint64_t max_block = blockfiles.back().last_block();
  if (block_idx == max_block) { return false; }

  auto lower = std::lower_bound(
      blockfiles.rbegin(), blockfiles.rend(), block_idx,
      [](const auto& lhs, const auto& rhs) { return lhs.start_block > rhs; });
  // lower "points" to the last block that has start_block <= block_index
  // one invariant of our class is that there is always a block file
  // that starts at 0, so we know that lower was always found
  // to make doubly sure we still check
  if (lower == blockfiles.rend()) {
    // can never happen (unless the object is in a bad state)
    return false;
  }

  return lower->goto_block(block_idx);
}
} /* namespace dedup */
