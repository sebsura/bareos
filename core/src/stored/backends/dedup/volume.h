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

#ifndef BAREOS_STORED_BACKENDS_DEDUP_VOLUME_H_
#define BAREOS_STORED_BACKENDS_DEDUP_VOLUME_H_

namespace dedup {
class data_file_manager {
 public:
  data_file_manager(config::loaded_config);
  std::optional<location> write(char* data, std::uint32_t count);
  bool read_data(std::byte* data, std::size_t count);
  bool goto_end();
  bool goto_begin();
  bool truncate();
  bool is_ok();
  bool flush();

 private:
  bool error{false};
  std::vector<data_file> files;
  data_file* data_file_for_size(std::uint32_t size);
};

class block_file_manager {
 public:
  block_file_manager(config::loaded_config);
  std::optional<std::uint64_t> write_block_header(block_header);
  std::optional<block_header> read_block_header();
  bool goto_block(std::uint64_t block_idx);
  bool goto_end();
  bool goto_begin();
  bool truncate();
  bool is_ok();
  bool flush();

 private:
  bool error{false};
  std::vector<file_based_array<block_header>> blocks;
};

class record_file_manager {
 public:
  record_file_manager(config::loaded_config);
  std::optional<std::uint64_t> write_record_headers(std::vector record_headers);
  std::vector<record_header> read_record_headers(std::uint64_t start,
                                                 std::uint32_t count);
  void delete_records(std::uint64_t start, std::uint32_t num);
  bool goto_end();
  bool goto_begin();
  bool truncate();
  bool is_ok();
  bool flush();

 private:
  bool error{false};
  std::vector<file_based_array<record_header>> records;
};


class volume {
 public:
  volume(const char* path, DeviceMode dev_mode, int mode);
  ~volume();
  ssize_t read(read_buffer);
  ssize_t write(write_buffer);
  bool truncate();
  bool goto_begin();
  bool goto_block(std::uint64_t block);
  bool goto_end();
  bool flush();

  bool is_ok();

 private:
  data_file_manager data;
  record_file_manager record;
  block_file_manager block;

  bool error{false};
  bool write_config();
  bool load_config();
};
}  // namespace dedup

#endif  // BAREOS_STORED_BACKENDS_DEDUP_VOLUME_H_
