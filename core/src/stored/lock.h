/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2007 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2025 Bareos GmbH & Co. KG

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
// Kern Sibbald, pulled out of dev.h June 2007
/**
 * @file
 * Definitions for locking and blocking functions in the SD
 */

#ifndef BAREOS_STORED_LOCK_H_
#define BAREOS_STORED_LOCK_H_

#include "stored/dev.h"
#include "lib/source_location.h"

namespace storagedaemon {

// blocked_ states (mutually exclusive)
enum
{
  BST_NOT_BLOCKED = 0,             /**< Not blocked */
  BST_UNMOUNTED,                   /**< User unmounted device */
  BST_WAITING_FOR_SYSOP,           /**< Waiting for operator to mount tape */
  BST_DOING_ACQUIRE,               /**< Opening/validating/moving tape */
  BST_WRITING_LABEL,               /**< Labeling a tape */
  BST_UNMOUNTED_WAITING_FOR_SYSOP, /**< User unmounted during wait for op */
  BST_MOUNT,                       /**< Mount request */
  BST_DESPOOLING,                  /**< Despooling -- i.e. multiple writes */
  BST_RELEASING                    /**< Releasing the device */
};

typedef struct s_steal_lock {
  pthread_t no_wait_id; /**< id of no wait thread */
  int dev_blocked;      /**< state */
  int dev_prev_blocked; /**< previous blocked state */
} bsteal_lock_t;

// Used in unblock() call
enum
{
  DEV_LOCKED = true,
  DEV_UNLOCKED = false
};

class Device;

void LockDevice(Device* dev,
                libbareos::source_location
                = libbareos::source_location::current());
void UnlockDevice(Device* dev,
                  libbareos::source_location
                  = libbareos::source_location::current());
void BlockDevice(Device* dev,
                 int state,
                 libbareos::source_location
                 = libbareos::source_location::current());
void UnblockDevice(Device* dev,
                   libbareos::source_location
                   = libbareos::source_location::current());
void StealDeviceLock(Device* dev,
                     bsteal_lock_t* hold,
                     int state,
                     libbareos::source_location
                     = libbareos::source_location::current());
void GiveBackDeviceLock(Device* dev,
                        bsteal_lock_t* hold,
                        libbareos::source_location
                        = libbareos::source_location::current());

} /* namespace storagedaemon */

#endif  // BAREOS_STORED_LOCK_H_
