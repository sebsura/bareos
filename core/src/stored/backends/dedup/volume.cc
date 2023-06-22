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

namespace dedup {

////////////////////////////////////////////////////////////////////////////////
// data_file_manager

data_file_manager::data_file_manager(config::loaded_config) { error = true; }

std::optional<location> data_file_manager::write_data(std::byte* data,
                                                      std::uint32_t count)
{
  auto* file = data_file_for_size(count);
  if (!file) { return std::nullopt; }
  auto loc = file->write(data, count);
  if (!loc) { return std::nullopt; }
  return {file.index, loc};
}

bool data_file_manager::read_data(write_buffer& buffer,
                                  location loc,
                                  std::size_t count)
{
  auto* file = get_data_file(location.index);

  if (!file) { return false; }

  char* reserved = buffer.reserve(count);

  if (!reserved) { return false; }

  return file->read(buffer, loc.start, count);
}

bool data_file_manager::goto_end()
{
  for (auto& file : files) { files.goto_end(); }
}

bool data_file_manager::goto_begin()
{
  for (auto& file : files) { files.goto_begin(); }
}

bool data_file_manager::truncate()
{
  for (auto& file : files) { files.truncate(); }
}

bool data_file_manager::is_ok() { return !error; }

bool data_file_manager::flush()
{
  for (auto& file : files) {
    if (!file.flush()) { return false; }
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
// block_file_manager

block_file_manager::block_file_manager(config::loaded_config) { error = true; }

std::optional<std::uint64_t> block_file_manager::write_block_headers(
    const std::vector<block_header>& headers)
{
  return std::nullopt;
}

std::vector<block_header> block_file_manager::read_block_headers(
    std::uint64_t start,
    std::uint32_t count)
{
  return {};
}

void block_file_manager::delete_blocks(std::uint64_t start, std::uint32_t count)
{
  return;
}

bool block_file_manager::goto_begin() { return false; }

bool block_file_manager::goto_end() { return false; }

bool block_file_manager::truncate() { return false; }

bool block_file_manager::is_ok() { return !error; }

bool block_file_manager::flush() { return false; }

////////////////////////////////////////////////////////////////////////////////
// record_file_manager

record_file_manager::record_file_manager(config::loaded_config)
{
  error = true;
}

std::optional<std::uint64_t> record_file_manager::write_record_headers(
    const std::vector<record_header>& headers)
{
  return std::nullopt;
}

std::vector<record_header> record_file_manager::read_record_headers(
    std::uint64_t start,
    std::uint32_t count)
{
  return {};
}

void record_file_manager::delete_records(std::uint64_t start,
                                         std::uint32_t count)
{
  return;
}

bool record_file_manager::goto_begin() { return false; }

bool record_file_manager::goto_end() { return false; }

bool record_file_manager::truncate() { return false; }

bool record_file_manager::is_ok() { return !error; }

bool record_file_manager::flush() { return false; }

////////////////////////////////////////////////////////////////////////////////
// volume

ssize_t volume::write(read_buffer buffer)
{
  auto* block_header = buffer.read<block_header>();
  std::vector<records> recs;
  while (auto* record = buffer.read<block_record>(buffer)) {
    auto real_size = std::min(record.size(), buffer.bytes_to_go());
    auto* data = buffer.read_array(real_size, buffer);
    auto loc = data.write(real_size, data);
    if (!loc) { return -1; }
    recs.push_back(record, *loc);
  }

  auto start_idx = record.write_records(recs);

  if (!start_idx) { return -1; }

  if (!block.write_block(block, *start_idx, recs.size())) {
    record.delete_records(*start_idx, recs.size());
    return -1;
  }

  return buffer.bytes_read();
}

ssize_t volume::read(write_buffer& buffer)
{
  auto block_header = block.read_block_header();
  if (!block_header) { return -1; }
  if (!buffer.write(block_header.bareos())) { return -1; }

  auto records = record.read_records(block_header.start(), block_header.num());

  if (records.size() != block_header.num()) { return -1; }

  for (std::uint32_t i = 0; i < block_header.num(); ++i) {
    if (!buffer.write(records[i].bareos())) { return -1; }

    if (!data.read(buffer, record.start(), record.size())) { return -1; }
  }

  return buffer.size();
}

bool volume::goto_begin()
{
  return block.goto_begin() && record.goto_begin() && data.goto_begin();
}

bool volume::goto_end()
{
  return block.goto_end() && record.goto_end() && data.goto_end();
}

bool volume::truncate()
{
  return block.truncate() && record.truncate() && data.truncate();
}

bool volume::flush()
{
  return block.flush() && record.flush() && data.flush() && write_config();
}

bool volume::is_ok()
{
  return !error && block.is_ok() && record.is_ok() && data.is_ok();
}

} /* namespace dedup */
