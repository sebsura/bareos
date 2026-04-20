/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2012 Free Software Foundation Europe e.V.
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
/*
 * Parse Bootstrap Records (used for restores)
 *
 * Kern Sibbald, June MMII
 */

#include "include/bareos.h"
#include "jcr.h"
#include "stored/bsr.h"
#include "lib/berrno.h"
#include "lib/parse_bsr.h"
#include "lib/lex.h"

#include <span>

namespace libbareos {

typedef storagedaemon::BootStrapRecord*(
    ITEM_HANDLER)(lexer* lc, storagedaemon::BootStrapRecord* bsr);

static storagedaemon::BootStrapRecord* store_vol(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_mediatype(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* StoreDevice(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_client(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_job(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_jobid(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_count(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* StoreJobtype(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_joblevel(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_findex(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_sessid(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_volfile(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_volblock(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_voladdr(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_sesstime(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_include(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_exclude(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_stream(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_slot(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_fileregex(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_nothing(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr);

struct kw_items {
  const char* name;
  ITEM_HANDLER* handler;
};

// List of all keywords permitted in bsr files and their handlers
struct kw_items items[] = {{"volume", store_vol},
                           {"mediatype", store_mediatype},
                           {"client", store_client},
                           {"job", store_job},
                           {"jobid", store_jobid},
                           {"count", store_count},
                           {"fileindex", store_findex},
                           {"jobtype", StoreJobtype},
                           {"joblevel", store_joblevel},
                           {"volsessionid", store_sessid},
                           {"volsessiontime", store_sesstime},
                           {"include", store_include},
                           {"exclude", store_exclude},
                           {"volfile", store_volfile},
                           {"volblock", store_volblock},
                           {"voladdr", store_voladdr},
                           {"stream", store_stream},
                           {"slot", store_slot},
                           {"device", StoreDevice},
                           {"fileregex", store_fileregex},
                           {"storage", store_nothing},
                           {NULL, NULL}};

// Create a storagedaemon::BootStrapRecord record
static storagedaemon::BootStrapRecord* new_bsr()
{
  storagedaemon::BootStrapRecord* bsr = new storagedaemon::BootStrapRecord{};
  return bsr;
}

// Format a scanner error message
PRINTF_LIKE(4, 5)
static void s_err(const char* file, int line, lexer* lc, const char* msg, ...)
{
  va_list ap;
  PoolMem buf(PM_NAME);
  JobControlRecord* jcr = (JobControlRecord*)(lc->caller_ctx);

  va_start(ap, msg);
  buf.Bvsprintf(msg, ap);
  va_end(ap);

  if (jcr) {
    Jmsg(jcr, M_FATAL, 0,
         T_("Bootstrap file error: %s\n"
            "            : Line %d, col %d of file %s\n%s\n"),
         buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line);
  } else {
    e_msg(file, line, M_FATAL, 0,
          T_("Bootstrap file error: %s\n"
             "            : Line %d, col %d of file %s\n%s\n"),
          buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line);
  }
}

// Format a scanner warning message
PRINTF_LIKE(4, 5)
static void s_warn(const char* file, int line, lexer* lc, const char* msg, ...)
{
  va_list ap;
  PoolMem buf(PM_NAME);
  JobControlRecord* jcr = (JobControlRecord*)(lc->caller_ctx);

  va_start(ap, msg);
  buf.Bvsprintf(msg, ap);
  va_end(ap);

  if (jcr) {
    Jmsg(jcr, M_WARNING, 0,
         T_("Bootstrap file warning: %s\n"
            "            : Line %d, col %d of file %s\n%s\n"),
         buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line);
  } else {
    p_msg(file, line, 0,
          T_("Bootstrap file warning: %s\n"
             "            : Line %d, col %d of file %s\n%s\n"),
          buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line);
  }
}

static inline bool IsFastRejectionOk(storagedaemon::BootStrapRecord* bsr)
{
  /* Although, this can be optimized, for the moment, require
   *  all bsrs to have both sesstime and sessid set before
   *  we do fast rejection. */
  for (; bsr; bsr = bsr->next) {
    if (bsr->sesstime.empty() || bsr->sessid.empty()) { return false; }
  }
  return true;
}

static inline bool IsPositioningOk(storagedaemon::BootStrapRecord* bsr)
{
  /* Every bsr should have a volfile entry and a volblock entry
   * or a VolAddr
   *   if we are going to use positioning */
  for (; bsr; bsr = bsr->next) {
    bool no_addr = bsr->voladdr.empty();
    bool no_fileblock = bsr->volfile.empty() || bsr->volblock.empty();
    if (no_addr && no_fileblock) { return false; }
  }
  return true;
}

// Parse Bootstrap file
storagedaemon::BootStrapRecord* parse_bsr(JobControlRecord* jcr, char* fname)
{
  lexer* lc = NULL;
  int token, i;
  storagedaemon::BootStrapRecord* root_bsr = new_bsr();
  storagedaemon::BootStrapRecord* bsr = root_bsr;

  Dmsg1(300, "Enter parse_bsf %s\n", fname);
  if ((lc = lex_open_file(lc, fname, s_err, s_warn)) == NULL) {
    BErrNo be;
    Emsg2(M_ERROR_TERM, 0, T_("Cannot open bootstrap file %s: %s\n"), fname,
          be.bstrerror());
  }
  lc->caller_ctx = (void*)jcr;
  while ((token = LexGetToken(lc, BCT_ALL)) != BCT_EOF) {
    Dmsg1(300, "parse got token=%s\n", lex_tok_to_str(token));
    if (token == BCT_EOL) { continue; }
    for (i = 0; items[i].name; i++) {
      if (Bstrcasecmp(items[i].name, lc->str)) {
        token = LexGetToken(lc, BCT_ALL);
        Dmsg1(300, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
        if (token != BCT_EQUALS) {
          scan_err1(lc, "expected an equals, got: %s", lc->str);
          bsr = NULL;
          break;
        }
        Dmsg1(300, "calling handler for %s\n", items[i].name);
        // Call item handler
        bsr = items[i].handler(lc, bsr);
        i = -1;
        break;
      }
    }
    if (i >= 0) {
      Dmsg1(300, "Keyword = %s\n", lc->str);
      scan_err1(lc, "Keyword %s not found", lc->str);
      bsr = NULL;
      break;
    }
    if (!bsr) { break; }
  }
  lc = LexCloseFile(lc);
  Dmsg0(300, "Leave parse_bsf()\n");
  if (!bsr) {
    FreeBsr(root_bsr);
    root_bsr = NULL;
  }
  if (root_bsr) {
    root_bsr->use_fast_rejection = IsFastRejectionOk(root_bsr);
    root_bsr->use_positioning = IsPositioningOk(root_bsr);
  }
  for (bsr = root_bsr; bsr; bsr = bsr->next) { bsr->root = root_bsr; }
  return root_bsr;
}

static storagedaemon::BootStrapRecord* store_vol(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolume* volume;
  char *p, *n;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }
  if (!bsr->volume.empty()) {
    bsr->next = new_bsr();
    bsr->next->prev = bsr;
    bsr = bsr->next;
  }
  /* This may actually be more than one volume separated by a |
   * If so, separate them.
   */
  for (p = lc->str; p && *p;) {
    n = strchr(p, '|');
    if (n) { *n++ = 0; }

    // Add it to the end of the volume chain
    volume = &bsr->volume.emplace_back();
    bstrncpy(volume->VolumeName, p, sizeof(volume->VolumeName));

    p = n;
  }
  return bsr;
}

// Shove the MediaType in each Volume in the current bsr
static storagedaemon::BootStrapRecord* store_mediatype(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }
  if (bsr->volume.empty()) {
    Emsg1(M_ERROR, 0, T_("MediaType %s in bsr at inappropriate place.\n"),
          lc->str);
    return bsr;
  }
  for (auto& bv : bsr->volume) {
    bstrncpy(bv.MediaType, lc->str, sizeof(bv.MediaType));
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_nothing(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }
  return bsr;
}

// Shove the Device name in each Volume in the current bsr
static storagedaemon::BootStrapRecord* StoreDevice(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }
  if (bsr->volume.empty()) {
    Emsg1(M_ERROR, 0, T_("Device \"%s\" in bsr at inappropriate place.\n"),
          lc->str);
    return bsr;
  }
  for (auto& bv : bsr->volume) {
    bstrncpy(bv.device, lc->str, sizeof(bv.device));
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_client(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrClient* client;

  for (;;) {
    token = LexGetToken(lc, BCT_NAME);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the client chain
    client = &bsr->client.emplace_back();
    bstrncpy(client->ClientName, lc->str, sizeof(client->ClientName));

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_job(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrJob* job;

  for (;;) {
    token = LexGetToken(lc, BCT_NAME);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the job chain
    job = &bsr->job.emplace_back();
    bstrncpy(job->Job, lc->str, sizeof(job->Job));

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_findex(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrFileIndex* findex;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    findex = &bsr->FileIndex.emplace_back();
    findex->findex = lc->u.pint32_val;
    findex->findex2 = lc->u2.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_jobid(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrJobid* jobid;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    jobid = &bsr->JobId.emplace_back();
    jobid->JobId = lc->u.pint32_val;
    jobid->JobId2 = lc->u2.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_count(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_PINT32);
  if (token == BCT_ERROR) { return NULL; }
  bsr->count = lc->u.pint32_val;
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_fileregex(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  int rc;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }

  bsr->fileregex = lc->str;

  if (!bsr->fileregex_re) {
    bsr->fileregex_re.emplace();
  } else {
    regfree(&*bsr->fileregex_re);
  }

  rc = regcomp(&*bsr->fileregex_re, bsr->fileregex.c_str(),
               REG_EXTENDED | REG_NOSUB);
  if (rc != 0) {
    char prbuf[500];
    regerror(rc, &*bsr->fileregex_re, prbuf, sizeof(prbuf));
    Emsg2(M_ERROR, 0, T_("REGEX '%s' compile error. ERR=%s\n"),
          bsr->fileregex.c_str(), prbuf);
    return NULL;
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* StoreJobtype(
    lexer*,
    storagedaemon::BootStrapRecord* bsr)
{
  /* *****FIXME****** */
  Pmsg0(-1, T_("JobType not yet implemented\n"));
  return bsr;
}

static storagedaemon::BootStrapRecord* store_joblevel(
    lexer*,
    storagedaemon::BootStrapRecord* bsr)
{
  /* *****FIXME****** */
  Pmsg0(-1, T_("JobLevel not yet implemented\n"));
  return bsr;
}

// Routine to handle Volume start/end file
static storagedaemon::BootStrapRecord* store_volfile(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeFile* volfile;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    volfile = &bsr->volfile.emplace_back();
    volfile->sfile = lc->u.pint32_val;
    volfile->efile = lc->u2.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

// Routine to handle Volume start/end Block
static storagedaemon::BootStrapRecord* store_volblock(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeBlock* volblock;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    volblock = &bsr->volblock.emplace_back();
    volblock->sblock = lc->u.pint32_val;
    volblock->eblock = lc->u2.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

// Routine to handle Volume start/end address
static storagedaemon::BootStrapRecord* store_voladdr(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeAddress* voladdr;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT64_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    voladdr = &bsr->voladdr.emplace_back();
    voladdr->saddr = lc->u.pint64_val;
    voladdr->eaddr = lc->u2.pint64_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_sessid(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrSessionId* sid;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }

    // Add it to the end of the chain
    sid = &bsr->sessid.emplace_back();
    sid->sessid = lc->u.pint32_val;
    sid->sessid2 = lc->u2.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_sesstime(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrSessionTime* stime;

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32);
    if (token == BCT_ERROR) { return NULL; }
    // Add it to the end of the chain
    stime = &bsr->sesstime.emplace_back();
    stime->sesstime = lc->u.pint32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_stream(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrStream* stream;

  for (;;) {
    token = LexGetToken(lc, BCT_INT32);
    if (token == BCT_ERROR) { return NULL; }
    // Add it to the end of the chain
    stream = &bsr->stream.emplace_back();
    stream->stream = lc->u.int32_val;

    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_slot(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_PINT32);
  if (token == BCT_ERROR) { return NULL; }
  if (bsr->volume.empty()) {
    Emsg1(M_ERROR, 0, T_("Slot %d in bsr at inappropriate place.\n"),
          lc->u.pint32_val);
    return bsr;
  }
  bsr->volume[0].Slot = lc->u.pint32_val;
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_include(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_exclude(
    lexer* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  ScanToEol(lc);
  return bsr;
}

static inline void DumpVolfile(
    std::span<const storagedaemon::BsrVolumeFile> volfiles)
{
  for (auto& volfile : volfiles) {
    Pmsg2(-1, T_("VolFile     : %u-%u\n"), volfile.sfile, volfile.efile);
  }
}

static inline void DumpVolblock(
    std::span<const storagedaemon::BsrVolumeBlock> volblocks)
{
  for (auto& volblock : volblocks) {
    Pmsg2(-1, T_("VolBlock    : %u-%u\n"), volblock.sblock, volblock.eblock);
  }
}

static inline void DumpVoladdr(
    std::span<const storagedaemon::BsrVolumeAddress> voladdrs)
{
  for (auto& voladdr : voladdrs) {
    Pmsg2(-1, T_("VolAddr    : %" PRIu64 "-%" PRIu64 "\n"), voladdr.saddr,
          voladdr.eaddr);
  }
}

static inline void DumpFindex(
    std::span<const storagedaemon::BsrFileIndex> FileIndices)
{
  for (auto& FileIndex : FileIndices) {
    if (FileIndex.findex == FileIndex.findex2) {
      Pmsg1(-1, T_("FileIndex   : %u\n"), FileIndex.findex);
    } else {
      Pmsg2(-1, T_("FileIndex   : %u-%u\n"), FileIndex.findex,
            FileIndex.findex2);
    }
  }
}

static inline void DumpJobid(std::span<const storagedaemon::BsrJobid> jobids)
{
  for (auto& jobid : jobids) {
    if (jobid.JobId == jobid.JobId2) {
      Pmsg1(-1, T_("JobId       : %u\n"), jobid.JobId);
    } else {
      Pmsg2(-1, T_("JobId       : %u-%u\n"), jobid.JobId, jobid.JobId2);
    }
  }
}

static inline void DumpSessid(
    std::span<const storagedaemon::BsrSessionId> sessids)
{
  for (auto& sessid : sessids) {
    if (sessid.sessid == sessid.sessid2) {
      Pmsg1(-1, T_("SessId      : %u\n"), sessid.sessid);
    } else {
      Pmsg2(-1, T_("SessId      : %u-%u\n"), sessid.sessid, sessid.sessid2);
    }
  }
}

static inline void DumpVolume(std::span<const storagedaemon::BsrVolume> volume)
{
  for (auto& vol : volume) {
    Pmsg1(-1, T_("VolumeName  : %s\n"), vol.VolumeName);
    Pmsg1(-1, T_("  MediaType : %s\n"), vol.MediaType);
    Pmsg1(-1, T_("  Device    : %s\n"), vol.device);
    Pmsg1(-1, T_("  Slot      : %d\n"), vol.Slot);
  }
}

static inline void DumpClient(std::span<const storagedaemon::BsrClient> clients)
{
  for (auto& client : clients) {
    Pmsg1(-1, T_("Client      : %s\n"), client.ClientName);
  }
}

static inline void dump_job(std::span<const storagedaemon::BsrJob> jobs)
{
  for (auto& job : jobs) { Pmsg1(-1, T_("Job          : %s\n"), job.Job); }
}

static inline void DumpSesstime(
    std::span<const storagedaemon::BsrSessionTime> sesstimes)
{
  for (auto& sesstime : sesstimes) {
    Pmsg1(-1, T_("SessTime    : %u\n"), sesstime.sesstime);
  }
}

void DumpBsr(storagedaemon::BootStrapRecord* bsr, bool recurse)
{
  int save_debug = debug_level;
  debug_level = 1;
  if (!bsr) {
    Pmsg0(-1, T_("storagedaemon::BootStrapRecord is NULL\n"));
    debug_level = save_debug;
    return;
  }
  Pmsg1(-1, T_("Next        : %p\n"), bsr->next);
  Pmsg1(-1, T_("Root bsr    : %p\n"), bsr->root);
  DumpVolume(bsr->volume);
  DumpSessid(bsr->sessid);
  DumpSesstime(bsr->sesstime);
  DumpVolfile(bsr->volfile);
  DumpVolblock(bsr->volblock);
  DumpVoladdr(bsr->voladdr);
  DumpClient(bsr->client);
  DumpJobid(bsr->JobId);
  dump_job(bsr->job);
  DumpFindex(bsr->FileIndex);
  if (bsr->count) {
    Pmsg1(-1, T_("count       : %u\n"), bsr->count);
    Pmsg1(-1, T_("found       : %u\n"), bsr->found);
  }

  Pmsg1(-1, T_("done        : %s\n"), bsr->done ? T_("yes") : T_("no"));
  Pmsg1(-1, T_("positioning : %d\n"), bsr->use_positioning);
  Pmsg1(-1, T_("fast_reject : %d\n"), bsr->use_fast_rejection);
  if (recurse && bsr->next) {
    Pmsg0(-1, "\n");
    DumpBsr(bsr->next, true);
  }
  debug_level = save_debug;
}

// Remove a single item from the bsr tree
static inline void RemoveBsr(storagedaemon::BootStrapRecord* bsr)
{
  if (bsr->fileregex_re) { regfree(&*bsr->fileregex_re); }
  if (bsr->attr) { FreeAttr(bsr->attr); }
  if (bsr->next) { bsr->next->prev = bsr->prev; }
  if (bsr->prev) { bsr->prev->next = bsr->next; }
  delete bsr;
}

// Free all bsrs in chain
void FreeBsr(storagedaemon::BootStrapRecord* bsr)
{
  while (bsr) {
    auto* next_bsr = bsr->next;
    RemoveBsr(bsr);
    bsr = next_bsr;
  }
}

} /* namespace libbareos */
