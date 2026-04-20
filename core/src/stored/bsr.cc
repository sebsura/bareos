/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2010 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2026 Bareos GmbH & Co. KG

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
// Kern Sibbald, June MMII

/**
 * @file
 * Match Bootstrap Records (used for restores) against
 * Volume Records
 */

/*
 * ***FIXME***
 * Also for efficiency, once a bsr is done, it really should be
 *   delinked from the bsr chain.  This will avoid the above
 *   problem and make traversal of the bsr chain more efficient.
 *
 *   To be done ...
 */

#include "include/bareos.h"
#include "include/streams.h"
#include "stored/bsr.h"
#include "stored/device_control_record.h"
#include "stored/stored_jcr_impl.h"
#include "stored/stored.h"
#include "include/jcr.h"

#include <span>

namespace storagedaemon {

const int dbglevel = 500;

/* Forward references */
static int MatchVolume(BootStrapRecord* bsr,
                       std::span<const BsrVolume> volume,
                       Volume_Label* volrec,
                       bool done);
static int MatchSesstime(BootStrapRecord* bsr,
                         std::span<BsrSessionTime> sesstime,
                         DeviceRecord* rec,
                         bool done);
static int MatchSessid(BootStrapRecord* bsr,
                       std::span<const BsrSessionId> sessid,
                       DeviceRecord* rec);
static int MatchClient(BootStrapRecord* bsr,
                       std::span<const BsrClient> client,
                       Session_Label* sessrec,
                       bool done);
static int MatchJob(BootStrapRecord* bsr,
                    std::span<const BsrJob> job,
                    Session_Label* sessrec,
                    bool done);
static int MatchJobid(BootStrapRecord* bsr,
                      std::span<const BsrJobid> jobid,
                      Session_Label* sessrec,
                      bool done);
static int MatchFindex(BootStrapRecord* bsr,
                       std::span<BsrFileIndex> findex,
                       DeviceRecord* rec,
                       bool done);
static int MatchVolfile(BootStrapRecord* bsr,
                        std::span<BsrVolumeFile> volfile,
                        DeviceRecord* rec,
                        bool done);
static int MatchVoladdr(BootStrapRecord* bsr,
                        std::span<BsrVolumeAddress> voladdr,
                        DeviceRecord* rec,
                        bool done);
static int MatchStream(BootStrapRecord* bsr,
                       std::span<const BsrStream> stream,
                       DeviceRecord* rec,
                       bool done);
static int MatchAll(BootStrapRecord* bsr,
                    DeviceRecord* rec,
                    Volume_Label* volrec,
                    Session_Label* sessrec,
                    bool done,
                    JobControlRecord* jcr);
static int MatchBlockSesstime(BootStrapRecord* bsr,
                              std::span<const BsrSessionTime> sesstime,
                              DeviceBlock* block);
static int MatchBlockSessid(BootStrapRecord* bsr,
                            std::span<const BsrSessionId> sessid,
                            DeviceBlock* block);
static BootStrapRecord* find_smallest_volfile(BootStrapRecord* fbsr,
                                              BootStrapRecord* bsr);

/**
 *
 *  Do fast block rejection based on bootstrap records.
 *    use_fast_rejection will be set if we have VolSessionId and VolSessTime
 *    in each record. When BlockVer is >= 2, we have those in the block header
 *    so can do fast rejection.
 *
 *   returns:  1 if block may contain valid records
 *             0 if block may be skipped (i.e. it contains no records of
 *                  that can match the bsr).
 *
 */
int MatchBsrBlock(BootStrapRecord* bsr, DeviceBlock* block)
{
  if (!bsr || !bsr->use_fast_rejection || (block->BlockVer < 2)) {
    return 1; /* cannot fast reject */
  }

  for (; bsr; bsr = bsr->next) {
    if (!MatchBlockSesstime(bsr, bsr->sesstime, block)) { continue; }
    if (!MatchBlockSessid(bsr, bsr->sessid, block)) { continue; }
    return 1;
  }
  return 0;
}

static int MatchBlockSesstime(BootStrapRecord*,
                              std::span<const BsrSessionTime> sesstimes,
                              DeviceBlock* block)
{
  if (sesstimes.empty()) { return 1; /* no specification matches all */ }

  for (auto& sesstime : sesstimes) {
    if (sesstime.sesstime == block->VolSessionTime) { return 1; }
  }

  return 0;
}

static int MatchBlockSessid(BootStrapRecord*,
                            std::span<const BsrSessionId> sessids,
                            DeviceBlock* block)
{
  if (sessids.empty()) { return 1; /* no specification matches all */ }
  for (auto& sessid : sessids) {
    if (sessid.sessid <= block->VolSessionId
        && sessid.sessid2 >= block->VolSessionId) {
      return 1;
    }
  }
  return 0;
}

static int MatchFileregex(BootStrapRecord* bsr,
                          DeviceRecord* rec,
                          JobControlRecord* jcr)
{
  if (!bsr->fileregex_re) return 1;

  if (bsr->attr == NULL) { bsr->attr = new_attr(jcr); }

  /* The code breaks if the first record associated with a file is
   * not of this type */
  if (rec->maskedStream == STREAM_UNIX_ATTRIBUTES
      || rec->maskedStream == STREAM_UNIX_ATTRIBUTES_EX) {
    bsr->skip_file = false;
    if (UnpackAttributesRecord(jcr, rec->Stream, rec->data, rec->data_len,
                               bsr->attr)) {
      if (regexec(&*bsr->fileregex_re, bsr->attr->fname, 0, NULL, 0) == 0) {
        Dmsg2(dbglevel, "Matched pattern, fname=%s FI=%d\n", bsr->attr->fname,
              rec->FileIndex);
      } else {
        Dmsg2(dbglevel, "Didn't match, skipping fname=%s FI=%d\n",
              bsr->attr->fname, rec->FileIndex);
        bsr->skip_file = true;
      }
    }
  }
  return 1;
}

/**
 *
 *      Match Bootstrap records
 *        returns  1 on match
 *        returns  0 no match and Reposition is set if we should
 *                      Reposition the tape
 *       returns -1 no additional matches possible
 */
int MatchBsr(BootStrapRecord* bsr,
             DeviceRecord* rec,
             Volume_Label* volrec,
             Session_Label* sessrec,
             JobControlRecord* jcr)
{
  int status;

  /* The bsr->Reposition flag is set any time a bsr is done.
   *   In this case, we can probably Reposition the
   *   tape to the next available bsr position. */
  if (bsr) {
    bsr->Reposition = false;
    status = MatchAll(bsr, rec, volrec, sessrec, true, jcr);
    /* Note, bsr->Reposition is set by MatchAll when
     *  a bsr is done. We turn it off if a match was
     *  found or if we cannot use positioning */
    if (status != 0 || !bsr->use_positioning) { bsr->Reposition = false; }
  } else {
    status = 1; /* no bsr => match all */
  }
  return status;
}

/**
 * Find the next bsr that applies to the current tape.
 *   It is the one with the smallest VolFile position.
 */
BootStrapRecord* find_next_bsr(BootStrapRecord* root_bsr, Device* dev)
{
  BootStrapRecord* bsr;
  BootStrapRecord* found_bsr = NULL;

  /* Do tape/disk seeking only if CAP_POSITIONBLOCKS is on */
  if (!root_bsr) {
    Dmsg0(dbglevel, "NULL root bsr pointer passed to find_next_bsr.\n");
    return NULL;
  }
  if (!root_bsr->use_positioning || !root_bsr->Reposition
      || !dev->HasCap(CAP_POSITIONBLOCKS)) {
    Dmsg2(dbglevel, "No nxt_bsr use_pos=%d repos=%d\n",
          root_bsr->use_positioning, root_bsr->Reposition);
    return NULL;
  }
  Dmsg2(dbglevel, "use_pos=%d repos=%d\n", root_bsr->use_positioning,
        root_bsr->Reposition);
  root_bsr->mount_next_volume = false;
  /* Walk through all bsrs to find the next one to use => smallest file,block */
  for (bsr = root_bsr; bsr; bsr = bsr->next) {
    if (bsr->done || !MatchVolume(bsr, bsr->volume, &dev->VolHdr, 1)) {
      continue;
    }
    if (found_bsr == NULL) {
      found_bsr = bsr;
    } else {
      found_bsr = find_smallest_volfile(found_bsr, bsr);
    }
  }
  /* If we get to this point and found no bsr, it means
   *  that any additional bsr's must apply to the next
   *  tape, so set a flag. */
  if (found_bsr == NULL) { root_bsr->mount_next_volume = true; }
  return found_bsr;
}

/**
 * Get the smallest address from this voladdr part
 * Don't use "done" elements
 */
static bool GetSmallestVoladdr(std::span<const BsrVolumeAddress> vas,
                               uint64_t* ret)
{
  bool ok = false;
  uint64_t min_val = 0;

  for (auto& va : vas) {
    if (!va.done) {
      if (ok) {
        min_val = MIN(min_val, va.saddr);
      } else {
        min_val = va.saddr;
        ok = true;
      }
    }
  }
  *ret = min_val;
  return ok;
}

/* FIXME
 * This routine needs to be fixed to only look at items that
 *   are not marked as done.  Otherwise, it can find a bsr
 *   that has already been consumed, and this will cause the
 *   bsr to be used, thus we may seek back and re-read the
 *   same records, causing an error.  This deficiency must
 *   be fixed.  For the moment, it has been kludged in
 *   read_record.c to avoid seeking back if find_next_bsr
 *   returns a bsr pointing to a smaller address (file/block).
 *
 */
static BootStrapRecord* find_smallest_volfile(BootStrapRecord* found_bsr,
                                              BootStrapRecord* bsr)
{
  BootStrapRecord* return_bsr = found_bsr;
  uint32_t found_bsr_sfile, bsr_sfile;
  uint32_t found_bsr_sblock, bsr_sblock;
  uint64_t found_bsr_saddr, bsr_saddr;

  /* if we have VolAddr, use it, else try with File and Block */
  if (GetSmallestVoladdr(found_bsr->voladdr, &found_bsr_saddr)) {
    if (GetSmallestVoladdr(bsr->voladdr, &bsr_saddr)) {
      if (found_bsr_saddr > bsr_saddr) {
        return bsr;
      } else {
        return found_bsr;
      }
    }
  }

  auto smallest_sfile = [](std::span<const BsrVolumeFile> files) {
    auto smallest = &files[0];

    for (auto& file : files.subspan(1)) {
      if (file.sfile < smallest->sfile) { smallest = &file; }
    }

    return smallest->sfile;
  };

  auto smallest_block = [](std::span<const BsrVolumeBlock> blocks) {
    auto smallest = &blocks[0];

    for (auto& block : blocks.subspan(1)) {
      if (block.sblock < smallest->sblock) { smallest = &block; }
    }

    return smallest->sblock;
  };

  /* Find the smallest file */
  found_bsr_sfile = smallest_sfile(found_bsr->volfile);
  bsr_sfile = smallest_sfile(bsr->volfile);

  /* if the bsr file is less than the found_bsr file, return bsr */
  if (found_bsr_sfile > bsr_sfile) {
    return_bsr = bsr;
  } else if (found_bsr_sfile == bsr_sfile) {
    /* Files are equal */
    /* find smallest block */
    found_bsr_sblock = smallest_block(found_bsr->volblock);
    bsr_sblock = smallest_block(bsr->volblock);
    /* Compare and return the smallest */
    if (found_bsr_sblock > bsr_sblock) { return_bsr = bsr; }
  }
  return return_bsr;
}

/**
 * Called after the signature record so that
 *   we can see if the current bsr has been
 *   fully processed (i.e. is done).
 *  The bsr argument is not used, but is included
 *    for consistency with the other match calls.
 *
 * Returns: true if we should reposition
 *        : false otherwise.
 */
bool IsThisBsrDone(BootStrapRecord*, DeviceRecord* rec)
{
  BootStrapRecord* rbsr = rec->bsr;
  Dmsg1(dbglevel, "match_set %d\n", rbsr != NULL);
  if (!rbsr) { return false; }
  rec->bsr = NULL;
  rbsr->found++;
  if (rbsr->count && rbsr->found >= rbsr->count) {
    rbsr->done = true;
    rbsr->root->Reposition = true;
    Dmsg2(dbglevel, "is_end_this_bsr set Reposition=1 count=%d found=%d\n",
          rbsr->count, rbsr->found);
    return true;
  }
  Dmsg2(dbglevel, "is_end_this_bsr not done count=%d found=%d\n", rbsr->count,
        rbsr->found);
  return false;
}

/**
 * Match all the components of current record
 *   returns  1 on match
 *   returns  0 no match
 *   returns -1 no additional matches possible
 */
static int MatchAll(BootStrapRecord* bsr,
                    DeviceRecord* rec,
                    Volume_Label* volrec,
                    Session_Label* sessrec,
                    bool done,
                    JobControlRecord* jcr)
{
  Dmsg0(dbglevel, "Enter MatchAll\n");
  if (bsr->done) {
    //    Dmsg0(dbglevel, "bsr->done set\n");
    goto no_match;
  }
  if (!MatchVolume(bsr, bsr->volume, volrec, 1)) {
    Dmsg2(dbglevel, "bsr fail bsr_vol=%s != rec read_vol=%s\n",
          bsr->volume[0].VolumeName, volrec->VolumeName);
    goto no_match;
  }
  Dmsg2(dbglevel, "OK bsr match bsr_vol=%s read_vol=%s\n",
        bsr->volume[0].VolumeName, volrec->VolumeName);

  if (!MatchVolfile(bsr, bsr->volfile, rec, 1)) {
    if (!bsr->volfile.empty()) {
      Dmsg3(dbglevel, "Fail on file=%u. bsr=%u,%u\n", rec->File,
            bsr->volfile[0].sfile, bsr->volfile[0].efile);
    }
    goto no_match;
  }

  if (!MatchVoladdr(bsr, bsr->voladdr, rec, 1)) {
    if (!bsr->voladdr.empty()) {
      Dmsg3(dbglevel, "Fail on Addr=%" PRIu64 ". bsr=%" PRIu64 ",%" PRIu64 "\n",
            GetRecordAddress(rec), bsr->voladdr[0].saddr,
            bsr->voladdr[0].eaddr);
    }
    goto no_match;
  }

  if (!MatchSesstime(bsr, bsr->sesstime, rec, 1)) {
    Dmsg2(dbglevel, "Fail on sesstime. bsr=%u rec=%u\n",
          bsr->sesstime[0].sesstime, rec->VolSessionTime);
    goto no_match;
  }

  /* NOTE!! This test MUST come after the sesstime test */
  if (!MatchSessid(bsr, bsr->sessid, rec)) {
    Dmsg2(dbglevel, "Fail on sessid. bsr=%u rec=%u\n", bsr->sessid[0].sessid,
          rec->VolSessionId);
    goto no_match;
  }

  /* NOTE!! This test MUST come after sesstime and sessid tests */
  if (!bsr->FileIndex.empty()) {
    if (!MatchFindex(bsr, bsr->FileIndex, rec, 1)) {
      Dmsg3(dbglevel, "Fail on findex=%d. bsr=%d,%d\n", rec->FileIndex,
            bsr->FileIndex[0].findex, bsr->FileIndex[0].findex2);
      goto no_match;
    } else {
      Dmsg3(dbglevel, "match on findex=%d. bsr=%d,%d\n", rec->FileIndex,
            bsr->FileIndex[0].findex, bsr->FileIndex[0].findex2);
    }
  } else {
    Dmsg0(dbglevel, "No bsr->FileIndex!\n");
  }

  if (!MatchFileregex(bsr, rec, jcr)) {
    Dmsg1(dbglevel, "Fail on fileregex='%s'\n", bsr->fileregex.c_str());
    goto no_match;
  }

  /* This flag is set by MatchFileregex (and perhaps other tests) */
  if (bsr->skip_file) {
    Dmsg1(dbglevel, "Skipping findex=%d\n", rec->FileIndex);
    goto no_match;
  }

  /* If a count was specified and we have a FileIndex, assume
   *   it is a Bareos created bsr (or the equivalent). We
   *   then save the bsr where the match occurred so that
   *   after processing the record or records, we can update
   *   the found count. I.e. rec->bsr points to the bsr that
   *   satisfied the match. */
  if (bsr->count && !bsr->FileIndex.empty()) {
    rec->bsr = bsr;
    Dmsg0(dbglevel, "Leave MatchAll 1\n");
    return 1; /* this is a complete match */
  }

  /* The selections below are not used by Bareos's
   *   restore command, and don't work because of
   *   the rec->bsr = bsr optimization above. */
  if (!MatchJobid(bsr, bsr->JobId, sessrec, 1)) {
    Dmsg0(dbglevel, "fail on JobId\n");
    goto no_match;
  }
  if (!MatchJob(bsr, bsr->job, sessrec, 1)) {
    Dmsg0(dbglevel, "fail on Job\n");
    goto no_match;
  }
  if (!MatchClient(bsr, bsr->client, sessrec, 1)) {
    Dmsg0(dbglevel, "fail on Client\n");
    goto no_match;
  }
  if (!MatchStream(bsr, bsr->stream, rec, 1)) {
    Dmsg0(dbglevel, "fail on stream\n");
    goto no_match;
  }
  return 1;

no_match:
  if (bsr->next) {
    return MatchAll(bsr->next, rec, volrec, sessrec, bsr->done && done, jcr);
  }
  if (bsr->done && done) {
    Dmsg0(dbglevel, "Leave match all -1\n");
    return -1;
  }
  Dmsg0(dbglevel, "Leave match all 0\n");
  return 0;
}

static int MatchVolume(BootStrapRecord*,
                       std::span<const BsrVolume> volumes,
                       Volume_Label* volrec,
                       bool)
{
  if (volumes.empty()) { return 0; /* Volume must match */ }
  for (auto& volume : volumes) {
    if (bstrcmp(volume.VolumeName, volrec->VolumeName)) {
      Dmsg1(dbglevel, "MatchVolume=%s\n", volrec->VolumeName);
      return 1;
    }
  }
  return 0;
}

static int MatchClient(BootStrapRecord*,
                       std::span<const BsrClient> clients,
                       Session_Label* sessrec,
                       bool)
{
  if (clients.empty()) { return 1; /* no specification matches all */ }
  for (auto& client : clients) {
    if (bstrcmp(client.ClientName, sessrec->ClientName)) { return 1; }
  }
  return 0;
}

static int MatchJob(BootStrapRecord*,
                    std::span<const BsrJob> jobs,
                    Session_Label* sessrec,
                    bool)
{
  if (jobs.empty()) { return 1; /* no specification matches all */ }
  for (auto& job : jobs) {
    if (bstrcmp(job.Job, sessrec->Job)) { return 1; }
  }
  return 0;
}

static int MatchJobid(BootStrapRecord*,
                      std::span<const BsrJobid> jobids,
                      Session_Label* sessrec,
                      bool)
{
  if (jobids.empty()) { return 1; /* no specification matches all */ }
  for (auto& jobid : jobids) {
    if (jobid.JobId <= sessrec->JobId && jobid.JobId2 >= sessrec->JobId) {
      return 1;
    }
  }
  return 0;
}

static int MatchVolfile(BootStrapRecord* bsr,
                        std::span<BsrVolumeFile> volfiles,
                        DeviceRecord* rec,
                        bool done)
{
  if (volfiles.empty()) { return 1; /* no specification matches all */ }
  /* The following code is turned off because this should now work
   *   with disk files too, though since a "volfile" is 4GB, it does
   *   not improve performance much. */

  for (auto& volfile : volfiles) {
    if (volfile.sfile <= rec->File && volfile.efile >= rec->File) { return 1; }
    /* Once we get past last efile, we are done */
    if (rec->File > volfile.efile) {
      volfile.done = true; /* set local done */
    }
    /* otherwise the bsr is not done */
    else if (!volfile.done) {
      done = false;
    }
  }

  /* If we are done and all prior matches are done, this bsr is finished */
  if (done) {
    bsr->done = true;
    bsr->root->Reposition = true;
    Dmsg2(dbglevel, "bsr done from volfile rec=%u volefile=%u\n", rec->File,
          volfiles.back().efile);
  }
  return 0;
}

static int MatchVoladdr(BootStrapRecord* bsr,
                        std::span<BsrVolumeAddress> voladdrs,
                        DeviceRecord* rec,
                        bool done)
{
  if (voladdrs.empty()) { return 1; /* no specification matches all */ }

  uint64_t addr = GetRecordAddress(rec);
  for (auto& voladdr : voladdrs) {
    Dmsg6(dbglevel,
          "MatchVoladdr: saddr=%" PRIu64 " eaddr=%" PRIu64 " recaddr=%" PRIu64
          " "
          "sfile=%" PRIu64 " efile=%" PRIu64 " recfile=%" PRIu64 "\n",
          voladdr.saddr, voladdr.eaddr, addr, voladdr.saddr >> 32,
          voladdr.eaddr >> 32, addr >> 32);

    if (voladdr.saddr <= addr && voladdr.eaddr >= addr) { return 1; }
    /* Once we get past last eblock, we are done */
    if (addr > voladdr.eaddr) {
      voladdr.done = true; /* set local done */
    } else if (!voladdr.done) {
      done = false;
    }
  }

  /* If we are done and all prior matches are done, this bsr is finished */
  if (done) {
    bsr->done = true;
    bsr->root->Reposition = true;
    Dmsg2(dbglevel,
          "bsr done from voladdr rec=%" PRIu64 " voleaddr=%" PRIu64 "\n", addr,
          voladdrs.back().eaddr);
  }
  return 0;
}


static int MatchStream(BootStrapRecord*,
                       std::span<const BsrStream> streams,
                       DeviceRecord* rec,
                       bool)
{
  if (streams.empty()) { return 1; /* no specification matches all */ }
  for (auto& stream : streams) {
    if (stream.stream == rec->Stream) { return 1; }
  }
  return 0;
}

static int MatchSesstime(BootStrapRecord* bsr,
                         std::span<BsrSessionTime> sesstimes,
                         DeviceRecord* rec,
                         bool done)
{
  if (sesstimes.empty()) { return 1; /* no specification matches all */ }

  for (auto& sesstime : sesstimes) {
    if (sesstime.sesstime == rec->VolSessionTime) { return 1; }
    if (rec->VolSessionTime > sesstime.sesstime) {
      sesstime.done = true;
    } else if (!sesstime.done) {
      done = false;
    }
  }
  if (done) {
    bsr->done = true;
    bsr->root->Reposition = true;
    Dmsg0(dbglevel, "bsr done from sesstime\n");
  }
  return 0;
}

/**
 * Note, we cannot mark bsr done based on session id because we may
 *  have interleaved records, and there may be more of what we want
 *  later.
 */
static int MatchSessid(BootStrapRecord*,
                       std::span<const BsrSessionId> sessids,
                       DeviceRecord* rec)
{
  if (sessids.empty()) { return 1; /* no specification matches all */ }

  for (auto& sessid : sessids) {
    if (sessid.sessid <= rec->VolSessionId
        && sessid.sessid2 >= rec->VolSessionId) {
      return 1;
    }
  }
  return 0;
}

/**
 * When reading the Volume, the Volume Findex (rec->FileIndex) always
 *   are found in sequential order. Thus we can make optimizations.
 *
 *  ***FIXME*** optimizations
 * We could optimize by removing the recursion.
 */
static int MatchFindex(BootStrapRecord* bsr,
                       std::span<BsrFileIndex> findices,
                       DeviceRecord* rec,
                       bool done)
{
  if (findices.empty()) { return 1; /* no specification matches all */ }
  for (auto& findex : findices) {
    if (!findex.done) {
      if (findex.findex <= rec->FileIndex && findex.findex2 >= rec->FileIndex) {
        Dmsg3(dbglevel, "Match on findex=%d. bsrFIs=%d,%d\n", rec->FileIndex,
              findex.findex, findex.findex2);
        return 1;
      }
      if (rec->FileIndex > findex.findex2) {
        findex.done = true;
      } else if (!findex.done) {
        done = false;
      }
    }
  }
  if (done) {
    bsr->done = true;
    bsr->root->Reposition = true;
    Dmsg1(dbglevel, "bsr done from findex %d\n", rec->FileIndex);
  }
  return 0;
}

uint64_t GetBsrStartAddr(BootStrapRecord* bsr, uint32_t* file, uint32_t* block)
{
  uint64_t bsr_addr = 0;
  uint32_t sfile = 0, sblock = 0;

  if (bsr) {
    if (!bsr->voladdr.empty()) {
      bsr_addr = bsr->voladdr[0].saddr;
      sfile = bsr_addr >> 32;
      sblock = (uint32_t)bsr_addr;

    } else if (!bsr->volfile.empty() && !bsr->volblock.empty()) {
      bsr_addr
          = (((uint64_t)bsr->volfile[0].sfile) << 32) | bsr->volblock[0].sblock;
      sfile = bsr->volfile[0].sfile;
      sblock = bsr->volblock[0].sblock;
    }
  }

  if (file && block) {
    *file = sfile;
    *block = sblock;
  }

  return bsr_addr;
}

/* ****************************************************************
 * Routines for handling volumes
 */
static VolumeList* new_restore_volume()
{
  VolumeList* vol;
  vol = (VolumeList*)malloc(sizeof(VolumeList));
  memset(vol, 0, sizeof(VolumeList));
  return vol;
}

/**
 * Add current volume to end of list, only if the Volume
 * is not already in the list.
 *
 *   returns: 1 if volume added
 *            0 if volume already in list
 */
static bool AddRestoreVolume(JobControlRecord* jcr, VolumeList* vol)
{
  VolumeList* next = jcr->sd_impl->VolList;

  /* Add volume to volume manager's read list */
  AddReadVolume(jcr, vol->VolumeName);

  if (!next) {                   /* list empty ? */
    jcr->sd_impl->VolList = vol; /* yes, add volume */
  } else {
    /* Loop through all but last */
    for (; next->next; next = next->next) {
      if (bstrcmp(vol->VolumeName, next->VolumeName)) {
        /* Save smallest start file */
        if (vol->start_file < next->start_file) {
          next->start_file = vol->start_file;
        }
        return false; /* already in list */
      }
    }
    /* Check last volume in list */
    if (bstrcmp(vol->VolumeName, next->VolumeName)) {
      if (vol->start_file < next->start_file) {
        next->start_file = vol->start_file;
      }
      return false; /* already in list */
    }
    next->next = vol; /* add volume */
  }
  return true;
}

/**
 * Create a list of Volumes (and Slots and Start positions) to be
 *  used in the current restore job.
 */
void CreateRestoreVolumeList(JobControlRecord* jcr)
{
  char *p, *n;
  VolumeList* vol;

  // Build a list of volumes to be processed
  jcr->sd_impl->NumReadVolumes = 0;
  jcr->sd_impl->CurReadVolume = 0;
  if (jcr->sd_impl->read_session.bsr) {
    BootStrapRecord* bsr = jcr->sd_impl->read_session.bsr;
    if (bsr->volume.empty() || !bsr->volume[0].VolumeName[0]) { return; }
    for (; bsr; bsr = bsr->next) {
      uint32_t sfile = UINT32_MAX;

      /* Find minimum start file so that we can forward space to it */
      for (auto& volfile : bsr->volfile) {
        if (volfile.sfile < sfile) { sfile = volfile.sfile; }
      }

      /* Now add volumes for this bsr */
      for (auto& bsrvol : bsr->volume) {
        vol = new_restore_volume();
        bstrncpy(vol->VolumeName, bsrvol.VolumeName, sizeof(vol->VolumeName));
        bstrncpy(vol->MediaType, bsrvol.MediaType, sizeof(vol->MediaType));
        bstrncpy(vol->device, bsrvol.device, sizeof(vol->device));
        vol->Slot = bsrvol.Slot;
        vol->start_file = sfile;
        if (AddRestoreVolume(jcr, vol)) {
          jcr->sd_impl->NumReadVolumes++;
          Dmsg2(400, "Added volume=%s mediatype=%s\n", vol->VolumeName,
                vol->MediaType);
        } else {
          Dmsg1(400, "Duplicate volume %s\n", vol->VolumeName);
          free((char*)vol);
        }
        sfile = 0; /* start at beginning of second volume */
      }
    }
  } else {
    /* This is the old way -- deprecated */
    for (p = jcr->sd_impl->dcr->VolumeName; p && *p;) {
      n = strchr(p, '|'); /* volume name separator */
      if (n) { *n++ = 0; /* Terminate name */ }
      vol = new_restore_volume();
      bstrncpy(vol->VolumeName, p, sizeof(vol->VolumeName));
      bstrncpy(vol->MediaType, jcr->sd_impl->dcr->media_type,
               sizeof(vol->MediaType));
      if (AddRestoreVolume(jcr, vol)) {
        jcr->sd_impl->NumReadVolumes++;
      } else {
        free((char*)vol);
      }
      p = n;
    }
  }
}

void FreeRestoreVolumeList(JobControlRecord* jcr)
{
  VolumeList* vol = jcr->sd_impl->VolList;
  VolumeList* tmp;

  for (; vol;) {
    tmp = vol->next;
    RemoveReadVolume(jcr, vol->VolumeName);
    free(vol);
    vol = tmp;
  }
  jcr->sd_impl->VolList = NULL;
}

} /* namespace storagedaemon */
