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

#include "include/bareos.h"
#include "stored/stored.h"
#include "stored/stored_globals.h"
#include "stored/sd_backends.h"
#include "stored/device_control_record.h"
#include "dedup_file_device.h"
#include "lib/berrno.h"
#include "lib/util.h"

#include "dedup/dedup_config.h"

#include <unistd.h>
#include <utility>
#include <optional>
#include <cstring>

namespace storagedaemon {

/**
 * Mount the device.
 *
 * If timeout, wait until the mount command returns 0.
 * If !timeout, try to mount the device only once.
 */
bool dedup_file_device::MountBackend(DeviceControlRecord*, int)
{
  bool was_mounted = std::exchange(mounted, true);
  return !was_mounted;
}

/**
 * Unmount the device
 *
 * If timeout, wait until the unmount command returns 0.
 * If !timeout, try to unmount the device only once.
 */
bool dedup_file_device::UnmountBackend(DeviceControlRecord*, int)
{
  bool was_mounted = std::exchange(mounted, false);
  return was_mounted;
}

bool dedup_file_device::ScanForVolumeImpl(DeviceControlRecord* dcr)
{
  return ScanDirectoryForVolume(dcr);
}

int dedup_file_device::d_open(const char* path, int, int mode)
{
  // todo parse mode
  // see Device::set_mode

  // create/open the folder structure
  // path
  // +- block
  // +- record
  // +- data

  switch (open_mode) {
    case DeviceMode::CREATE_READ_WRITE:
      break;
    case DeviceMode::OPEN_READ_WRITE:
      break;
    case DeviceMode::OPEN_READ_ONLY:
      break;
    case DeviceMode::OPEN_WRITE_ONLY:
      break;
    default: {
      Emsg0(M_ABORT, 0, _("Illegal mode given to open dev.\n"));
      return -1;
    }
  }

  dedup::volume vol{path, open_mode, mode};

  if (vol.is_ok()) {
    int new_fd = fd_ctr;
    auto [iter, inserted] = open_volumes.emplace(new_fd, std::move(vol));

    if (!inserted) {
      // volume was already open; that should not be possible
      open_volumes.erase(iter);
      return -1;
    }

    fd_ctr += 1;
    return new_fd;
  } else {
    return -1;
  }
}

ssize_t scatter(dedup::volume& vol, const void* data, size_t size)
{
  auto* block = static_cast<const dedup::bareos_block_header*>(data);
  uint32_t bsize = block->BlockSize;

  if (bsize < sizeof(*block)) {
    // the data size has to at least include the block header!
    // otherwise this will not make any sense
    Emsg0(M_ABORT, 0, _("Trying to write bad block!\n"));
    return -1;
  }

  if (size < bsize) {
    // cannot write an uncomplete block
    return -1;
  }

  if (bsize != size) {
    // todo: emit dmsg warning
  }

  auto* begin = static_cast<const char*>(data);
  auto* current = begin + sizeof(*block);
  auto* end = begin + bsize;

  auto& blockfile = vol.get_active_block_file();
  auto& recordfile = vol.get_active_record_file();

  uint32_t RecStart = recordfile.current();
  uint32_t RecEnd = RecStart;

  while (current != end) {
    dedup::bareos_record_header* record = (dedup::bareos_record_header*)current;
    if (current + sizeof(*record) > end) {
      Emsg0(M_ABORT, 0, _("Trying to write bad record!\n"));
      return -1;
    }

    RecEnd += 1;
    auto* payload_start = reinterpret_cast<const char*>(record + 1);
    auto* payload_end = payload_start + record->DataSize;

    if (payload_end > end) {
      // payload is split in multiple blocks
      payload_end = end;
    }

    std::optional written_loc
        = vol.write_data(*block, *record, payload_start, payload_end);
    if (!written_loc) { return -1; }

    if (!recordfile.write(*record, written_loc->begin, written_loc->end,
                          written_loc->file_index)
        || RecEnd != recordfile.current()) {
      // something went wrong
      return -1;
    }
    current = payload_end;
  }

  ASSERT(RecEnd == recordfile.current());
  blockfile.write(*block, RecStart, RecEnd, recordfile.file_index);

  return current - begin;
}

ssize_t dedup_file_device::d_write(int fd, const void* data, size_t size)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    vol.changed_volume();
    SetEot();
    return scatter(vol, data, size);
  } else {
    return -1;
  }
}

ssize_t gather(dedup::volume& vol, char* data, std::size_t size)
{
  std::optional block = vol.read_block();
  dedup::write_buffer buf{data, size};

  if (!block) { return -1; }

  if (block->BareosHeader.BlockSize > size) { return -1; }

  if (!buf.write(block->BareosHeader)) { return -1; }

  for (std::size_t record_idx = block->RecStart; record_idx < block->RecEnd;
       ++record_idx) {
    std::optional record = vol.read_record(block->file_index, record_idx);

    if (!record) { return -1; }

    if (!buf.write(record->BareosHeader)) { return -1; }

    if (!vol.read_data(record->file_index, record->DataStart, record->DataEnd,
                       buf)) {
      return -1;
    }
  }

  return buf.current - data;
}

ssize_t dedup_file_device::d_read(int fd, void* data, size_t size)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    ssize_t bytes_written = gather(vol, static_cast<char*>(data), size);
    if (vol.is_at_end()) {
      SetEot();
    } else {
      ClearEot();
    }
    return bytes_written;
  } else {
    return -1;
  }
}

int dedup_file_device::d_close(int fd)
{
  size_t num_erased = open_volumes.erase(fd);
  if (num_erased == 1) {
    return 0;
  } else {
    return -1;
  }
}

int dedup_file_device::d_ioctl(int, ioctl_req_t, char*) { return -1; }

boffset_t dedup_file_device::d_lseek(DeviceControlRecord*, boffset_t, int)
{
  return -1;
}

bool dedup_file_device::d_truncate(DeviceControlRecord*)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    vol.changed_volume();
    return vol.reset();
  } else {
    return false;
  }
}

bool dedup_file_device::rewind(DeviceControlRecord* dcr)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    if (!vol.goto_begin()) { return false; }
    block_num = 0;
    file = 0;
    file_addr = 0;
    if (vol.is_at_end()) {
      SetEot();
    } else {
      ClearEot();
    }
    return UpdatePos(dcr);
  } else {
    return false;
  }
}

bool dedup_file_device::UpdatePos(DeviceControlRecord*)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    auto pos = vol.get_active_block_file().current_pos();
    if (!pos) return false;

    file_addr = *pos;
    block_num = *pos / sizeof(dedup::block_header);

    ASSERT(block_num * sizeof(dedup::block_header) == file_addr);

    file = 0;

    return true;
  } else {
    return false;
  }
}

bool dedup_file_device::Reposition(DeviceControlRecord* dcr,
                                   uint32_t rfile,
                                   uint32_t rblock)
{
  Dmsg2(10, "file: %u -> %u; block: %u -> %u\n", file, rfile, block_num,
        rblock);
  ASSERT(file == 0);

  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());

    if (!vol.goto_block(rblock)) { return false; }

    if (vol.is_at_end()) {
      SetEot();
    } else {
      ClearEot();
    }
    return UpdatePos(dcr);
  } else {
    return false;
  }
}

bool dedup_file_device::eod(DeviceControlRecord* dcr)
{
  if (auto found = open_volumes.find(fd); found != open_volumes.end()) {
    dedup::volume& vol = found->second;
    ASSERT(vol.is_ok());
    if (!vol.goto_end()) { return false; }
    SetEot();
    return UpdatePos(dcr);
  } else {
    return false;
  }
}

REGISTER_SD_BACKEND(dedup, dedup_file_device);

} /* namespace storagedaemon  */
