/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2008 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2026 Bareos GmbH & Co. KG

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
// Kern Sibbald, June 2002
/**
 * @file
 * BootStrap record definition -- for restoring files.
 */
#ifndef BAREOS_STORED_BSR_H_
#define BAREOS_STORED_BSR_H_

#include "lib/bregex.h"
#include "lib/attr.h"

namespace storagedaemon {

/**
 * List of Volume names to be read by Storage daemon.
 *  Formed by Storage daemon from BootStrapRecord
 */
struct VolumeList {
  VolumeList* next;
  char VolumeName[MAX_NAME_LENGTH];
  char MediaType[MAX_NAME_LENGTH];
  char device[MAX_NAME_LENGTH];
  int Slot;
  uint32_t start_file;
};

struct BsrVolume {
  char VolumeName[MAX_NAME_LENGTH];
  char MediaType[MAX_NAME_LENGTH];
  char device[MAX_NAME_LENGTH];
  int32_t Slot; /* Slot */
};

struct BsrClient {
  char ClientName[MAX_NAME_LENGTH];
};

struct BsrSessionId {
  uint32_t sessid;
  uint32_t sessid2;
};

struct BsrSessionTime {
  uint32_t sesstime;
  bool done; /* local done */
};

struct BsrVolumeFile {
  uint32_t sfile; /* start file */
  uint32_t efile; /* end file */
  bool done;      /* local done */
};

struct BsrVolumeBlock {
  uint32_t sblock; /* start block */
  uint32_t eblock; /* end block */
  bool done;       /* local done */
};

struct BsrVolumeAddress {
  uint64_t saddr; /* start address */
  uint64_t eaddr; /* end address */
  bool done;      /* local done */
};

struct BsrFileIndex {
  int32_t findex;  /* start file index */
  int32_t findex2; /* end file index */
  bool done;       /* local done */
};

struct BsrJobid {
  uint32_t JobId;
  uint32_t JobId2;
};

struct BsrJob {
  char Job[MAX_NAME_LENGTH];
  bool done; /* local done */
};

struct BsrStream {
  int32_t stream; /* stream desired */
};

struct BootStrapRecord {
  /* NOTE!!! next must be the first item */
  BootStrapRecord* next;   /* pointer to next one */
  BootStrapRecord* prev;   /* pointer to previous one */
  BootStrapRecord* root;   /* root bsr */
  bool Reposition;         /* set when any bsr is marked done */
  bool mount_next_volume;  /* set when next volume should be mounted */
  bool done;               /* set when everything found for this bsr */
  bool use_fast_rejection; /* set if fast rejection can be used */
  bool use_positioning;    /* set if we can position the archive */
  bool skip_file;          /* skip all records for current file */
  uint32_t count;          /* count of files to restore this bsr */
  uint32_t found;          /* count of restored files this bsr */

  std::vector<BsrVolume> volume;
  std::vector<BsrVolumeFile> volfile;
  std::vector<BsrVolumeBlock> volblock;
  std::vector<BsrVolumeAddress> voladdr;
  std::vector<BsrSessionTime> sesstime;
  std::vector<BsrSessionId> sessid;
  std::vector<BsrJobid> JobId;
  std::vector<BsrJob> job;
  std::vector<BsrClient> client;
  std::vector<BsrFileIndex> FileIndex;
  std::vector<BsrStream> stream;
  std::string fileregex; /* set if restore is filtered on filename */
  std::optional<regex_t> fileregex_re;
  Attributes* attr; /* scratch space for unpacking */
};


void CreateRestoreVolumeList(JobControlRecord* jcr);
void FreeRestoreVolumeList(JobControlRecord* jcr);

} /* namespace storagedaemon */

#endif  // BAREOS_STORED_BSR_H_
