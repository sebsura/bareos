/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2013 Free Software Foundation Europe e.V.
   Copyright (C) 2013-2025 Bareos GmbH & Co. KG

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
/*
 * Pulled out of dev.h
 *
 * Kern Sibbald, MMXIII
 *
 */
/**
 * @file
 * volume management definitions
 *
 */

/*
 * Some details of how volume reservations work
 *
 * class VolumeReservationItem:
 *   SetInUse()     volume being used on current drive
 *   ClearInUse()   no longer being used.  Can be re-used or moved.
 *   SetSwapping()   set volume being moved to another drive
 *   IsSwapping()    volume is being moved to another drive
 *   ClearSwapping() volume normal
 *
 */

#ifndef BAREOS_STORED_VOL_MGR_H_
#define BAREOS_STORED_VOL_MGR_H_

#include <atomic>

template <typename T> class dlist;

namespace storagedaemon {

class VolumeReservationItem;
VolumeReservationItem* vol_walk_start();
VolumeReservationItem* VolWalkNext(VolumeReservationItem* prev_vol);
void VolWalkEnd(VolumeReservationItem* vol);
VolumeReservationItem* read_vol_walk_start();
VolumeReservationItem* ReadVolWalkNext(VolumeReservationItem* prev_vol);
void ReadVolWalkEnd(VolumeReservationItem* vol);

// Volume reservation class -- see vol_mgr.c and reserve.c
class VolumeReservationItem {
  bool swapping_{false};              /**< set when swapping to another drive */
  bool in_use_{false};                /**< set when volume reserved or in use */
  bool reading_{false};               /**< set when reading */
  slot_number_t slot_{0};             /**< slot of swapping volume */
  uint32_t JobId_{0};                 /**< JobId for read volumes */
  std::atomic<int32_t> use_count_{0}; /**< Use count */
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER; /**< Vol muntex */
 public:
  dlink<VolumeReservationItem> link;
  char* vol_name{nullptr}; /**< Volume name */
  Device* dev{nullptr};    /**< Pointer to device to which we are attached */

  void InitMutex() { pthread_mutex_init(&mutex_, NULL); }
  void DestroyMutex() { pthread_mutex_destroy(&mutex_); }
  void Lock() { lock_mutex(mutex_); }
  void Unlock() { unlock_mutex(mutex_); }
  void IncUseCount(void) { ++use_count_; }
  void DecUseCount(void) { --use_count_; }
  int32_t UseCount() const { return use_count_; }
  bool IsSwapping() const { return swapping_; }
  bool is_reading() const { return reading_; }
  bool IsWriting() const { return !reading_; }
  void SetReading() { reading_ = true; }
  void clear_reading() { reading_ = false; }
  void SetSwapping() { swapping_ = true; }
  void ClearSwapping() { swapping_ = false; }
  bool IsInUse() const { return in_use_; }
  void SetInUse() { in_use_ = true; }
  void ClearInUse() { in_use_ = false; }
  void SetSlotNumber(slot_number_t slot) { slot_ = slot; }
  void InvalidateSlotNumber() { slot_ = kInvalidSlotNumber; }
  slot_number_t GetSlot() const { return slot_; }
  uint32_t GetJobid() const { return JobId_; }
  void SetJobid(uint32_t JobId) { JobId_ = JobId; }
};

#define foreach_vol(vol) \
  for (vol = vol_walk_start(); vol; (vol = VolWalkNext(vol)))

#define endeach_vol(vol) VolWalkEnd(vol)

#define foreach_read_vol(vol) \
  for (vol = read_vol_walk_start(); vol; (vol = ReadVolWalkNext(vol)))

#define endeach_read_vol(vol) ReadVolWalkEnd(vol)

void InitVolListLock();
void TermVolListLock();
VolumeReservationItem* reserve_volume(DeviceControlRecord* dcr,
                                      const char* VolumeName);
bool FreeVolume(Device* dev);
bool IsVolListEmpty();
dlist<VolumeReservationItem>* dup_vol_list(JobControlRecord* jcr);
void FreeTempVolList(dlist<VolumeReservationItem>* temp_vol_list);
bool VolumeUnused(DeviceControlRecord* dcr);
void CreateVolumeLists();
void FreeVolumeLists();
bool IsVolumeInUse(DeviceControlRecord* dcr);
void AddReadVolume(JobControlRecord* jcr, const char* VolumeName);
void RemoveReadVolume(JobControlRecord* jcr, const char* VolumeName);

} /* namespace storagedaemon */

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace my_storagedaemon {

struct media_type {
  std::string name_;
};

struct media_pool {
  std::string name;
};

struct media_id {
  std::uint64_t value;
};

using mtype_ref = const media_type*;
using mpool_ref = const media_pool*;

struct device {
  mtype_ref type{nullptr};
};

struct volume {
 public:
  mtype_ref mtype() const { return type; }
  mpool_ref mpool() const { return pool; }
  media_id mid() const { return id; }

  device*& loaded_device() { return loaded_in; }

 private:
  mtype_ref type{nullptr};
  mpool_ref pool{nullptr};
  media_id id{};

  device* loaded_in{nullptr};
};

struct volume_descriptor {
  mtype_ref type;
  mpool_ref pool;
};


struct device_manager {
  // struct device_ptr {
  //   device* dev{nullptr};
  //   device_manager* manager{nullptr};

  //   device_ptr() = default;
  //   device_ptr(device_manager* manager_, device* dev_)
  //       : dev{dev_}, manager{manager_}
  //   {
  //   }
  //   device_ptr(const device_ptr&) = delete;
  //   device_ptr& operator=(const device_ptr&) = default;
  //   device_ptr(device_ptr&& other) { *this = std::move(other); }
  //   device_ptr& operator=(device_ptr&& other)
  //   {
  //     cleanup();
  //     dev = other.dev;
  //     manager = other.manager;

  //     other.dev = nullptr;
  //     other.manager = nullptr;
  //   }

  //   ~device_ptr() { cleanup(); }

  //   operator bool() const { return this->dev != nullptr; }

  //   device& operator*() { return *dev; }
  //   const device& operator*() const { return *dev; }

  //  private:
  //   void cleanup()
  //   {
  //     if (dev) {
  //       manager->unlock(dev);
  //       dev = nullptr;
  //     }
  //     manager = nullptr;
  //   }
  // };

  std::vector<std::unique_ptr<device>> devices;
};

struct volume_manager {
  // struct volume_ptr {
  //   volume* vol{nullptr};
  //   volume_manager* manager{nullptr};

  //   volume_ptr() = default;
  //   volume_ptr(volume_manager* manager_, volume* vol_)
  //       : vol{vol_}, manager{manager_}
  //   {
  //   }
  //   volume_ptr(const volume_ptr&) = delete;
  //   volume_ptr& operator=(const volume_ptr&) = default;
  //   volume_ptr(volume_ptr&& other) { *this = std::move(other); }
  //   volume_ptr& operator=(volume_ptr&& other)
  //   {
  //     cleanup();
  //     vol = other.vol;
  //     manager = other.manager;

  //     other.vol = nullptr;
  //     other.manager = nullptr;
  //   }

  //   ~volume_ptr() { cleanup(); }

  //   operator bool() const { return this->vol != nullptr; }

  //   volume& operator*() { return *vol; }
  //   const volume& operator*() const { return *vol; }
  //   volume* operator->() { return vol; }
  //   const volume* operator->() const { return vol; }

  //  private:
  //   void cleanup()
  //   {
  //     if (vol) {
  //       manager->release_volume(vol);
  //       vol = nullptr;
  //     }
  //     manager = nullptr;
  //   }
  // };

  std::vector<std::unique_ptr<volume>> volumes;
};

struct mounted_device {
  std::unique_ptr<device> dev;
  std::unique_ptr<volume> vol;
};

struct reservation_manager {
  device_manager devices;
  volume_manager volumes;

  std::vector<mounted_device> mounted_devices;

  std::optional<mounted_device> acquire_for_reading(
      const volume* vol,
      const std::unordered_set<const device*> device_candidates);
};

};  // namespace my_storagedaemon

#endif  // BAREOS_STORED_VOL_MGR_H_
