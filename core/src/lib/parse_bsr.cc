/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2002-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2025 Bareos GmbH & Co. KG

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

namespace libbareos {

typedef storagedaemon::BootStrapRecord*(
    ITEM_HANDLER)(LEX* lc, storagedaemon::BootStrapRecord* bsr);

static storagedaemon::BootStrapRecord* store_vol(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_mediatype(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* StoreDevice(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_client(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_job(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_jobid(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_count(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* StoreJobtype(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_joblevel(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_findex(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_sessid(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_volfile(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_volblock(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_voladdr(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_sesstime(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_include(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_exclude(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_stream(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_slot(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_fileregex(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr);
static storagedaemon::BootStrapRecord* store_nothing(
    LEX* lc,
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
  return new storagedaemon::BootStrapRecord{};
}

// Format a scanner error message
static void s_err(const char* file, int line, LEX* lc, const char* msg, ...)
{
  va_list ap;
  int len, maxlen;
  PoolMem buf(PM_NAME);
  JobControlRecord* jcr = (JobControlRecord*)(lc->caller_ctx);

  while (1) {
    maxlen = buf.size() - 1;
    va_start(ap, msg);
    len = Bvsnprintf(buf.c_str(), maxlen, msg, ap);
    va_end(ap);

    if (len < 0 || len >= (maxlen - 5)) {
      buf.ReallocPm(maxlen + maxlen / 2);
      continue;
    }

    break;
  }

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
static void s_warn(const char* file, int line, LEX* lc, const char* msg, ...)
{
  va_list ap;
  int len, maxlen;
  PoolMem buf(PM_NAME);
  JobControlRecord* jcr = (JobControlRecord*)(lc->caller_ctx);

  while (1) {
    maxlen = buf.size() - 1;
    va_start(ap, msg);
    len = Bvsnprintf(buf.c_str(), maxlen, msg, ap);
    va_end(ap);

    if (len < 0 || len >= (maxlen - 5)) {
      buf.ReallocPm(maxlen + maxlen / 2);
      continue;
    }

    break;
  }

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
  for (auto& entry : bsr->entries) {
    if (!entry.sesstime || !entry.sessid) { return false; }
  }
  return true;
}

static inline bool IsPositioningOk(storagedaemon::BootStrapRecord* bsr)
{
  /* Every bsr should have a volfile entry and a volblock entry
   * or a VolAddr
   *   if we are going to use positioning */
  for (auto& entry : bsr->entries) {
    if (!entry.volfile || !entry.volblock) { return false; }
  }
  return true;
}

// Parse Bootstrap file
storagedaemon::BootStrapRecord* parse_bsr(JobControlRecord* jcr, char* fname)
{
  LEX* lc = NULL;
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
  for (auto& entry : root_bsr->entries) { entry.root = root_bsr; }
  return root_bsr;
}

storagedaemon::BootStrapEntry& get_entry(storagedaemon::BootStrapRecord* bsr)
{
  if (bsr->entries.empty()) { return bsr->entries.emplace_back(); }

  auto& current_entry = bsr->entries.back();

  return current_entry;
}
template <typename T>
storagedaemon::BootStrapEntry& get_entry(
    storagedaemon::BootStrapRecord* bsr,
    T storagedaemon::BootStrapEntry::* member)
{
  if (bsr->entries.empty()) { return bsr->entries.emplace_back(); }

  auto& current_entry = bsr->entries.back();

  if (current_entry.*member) { return bsr->entries.emplace_back(); }

  return current_entry;
}

static storagedaemon::BootStrapRecord* store_vol(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolume* volume;
  char *p, *n;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }


  auto current_entry = get_entry(bsr, &storagedaemon::BootStrapEntry::volume);

  /* This may actually be more than one volume separated by a |
   * If so, separate them.
   */
  for (p = lc->str; p && *p;) {
    n = strchr(p, '|');
    if (n) { *n++ = 0; }
    volume
        = (storagedaemon::BsrVolume*)malloc(sizeof(storagedaemon::BsrVolume));
    memset(volume, 0, sizeof(storagedaemon::BsrVolume));
    bstrncpy(volume->VolumeName, p, sizeof(volume->VolumeName));

    // Add it to the end of the volume chain
    if (!current_entry.volume) {
      current_entry.volume = volume;
    } else {
      storagedaemon::BsrVolume* bc = current_entry.volume;
      for (; bc->next; bc = bc->next) {}
      bc->next = volume;
    }
    p = n;
  }
  return bsr;
}

// Shove the MediaType in each Volume in the current bsr
static storagedaemon::BootStrapRecord* store_mediatype(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }

  auto& entry = get_entry(bsr);

  if (!entry.volume) {
    Emsg1(M_ERROR, 0, T_("MediaType %s in bsr at inappropriate place.\n"),
          lc->str);
    return bsr;
  }
  for (auto* bv = entry.volume; bv; bv = bv->next) {
    bstrncpy(bv->MediaType, lc->str, sizeof(bv->MediaType));
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_nothing(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }
  return bsr;
}

// Shove the Device name in each Volume in the current bsr
static storagedaemon::BootStrapRecord* StoreDevice(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }

  auto& entry = get_entry(bsr);

  if (!entry.volume) {
    Emsg1(M_ERROR, 0, T_("Device \"%s\" in bsr at inappropriate place.\n"),
          lc->str);
    return bsr;
  }
  for (auto* bv = entry.volume; bv; bv = bv->next) {
    bstrncpy(bv->device, lc->str, sizeof(bv->device));
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_client(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrClient* client;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_NAME);
    if (token == BCT_ERROR) { return NULL; }
    client
        = (storagedaemon::BsrClient*)malloc(sizeof(storagedaemon::BsrClient));
    memset(client, 0, sizeof(storagedaemon::BsrClient));
    bstrncpy(client->ClientName, lc->str, sizeof(client->ClientName));

    // Add it to the end of the client chain
    if (!entry.client) {
      entry.client = client;
    } else {
      storagedaemon::BsrClient* bc = entry.client;
      for (; bc->next; bc = bc->next) {}
      bc->next = client;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_job(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrJob* job;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_NAME);
    if (token == BCT_ERROR) { return NULL; }
    job = (storagedaemon::BsrJob*)malloc(sizeof(storagedaemon::BsrJob));
    memset(job, 0, sizeof(storagedaemon::BsrJob));
    bstrncpy(job->Job, lc->str, sizeof(job->Job));

    // Add it to the end of the client chain
    if (!entry.job) {
      entry.job = job;
    } else {
      // Add to end of chain
      storagedaemon::BsrJob* bc = entry.job;
      for (; bc->next; bc = bc->next) {}
      bc->next = job;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_findex(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrFileIndex* findex;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    findex = (storagedaemon::BsrFileIndex*)malloc(
        sizeof(storagedaemon::BsrFileIndex));
    memset(findex, 0, sizeof(storagedaemon::BsrFileIndex));
    findex->findex = lc->u.pint32_val;
    findex->findex2 = lc->u2.pint32_val;

    // Add it to the end of the chain
    if (!entry.FileIndex) {
      entry.FileIndex = findex;
    } else {
      // Add to end of chain
      storagedaemon::BsrFileIndex* bs = entry.FileIndex;
      for (; bs->next; bs = bs->next) {}
      bs->next = findex;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_jobid(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrJobid* jobid;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    jobid = (storagedaemon::BsrJobid*)malloc(sizeof(storagedaemon::BsrJobid));
    memset(jobid, 0, sizeof(storagedaemon::BsrJobid));
    jobid->JobId = lc->u.pint32_val;
    jobid->JobId2 = lc->u2.pint32_val;

    // Add it to the end of the chain
    if (!entry.JobId) {
      entry.JobId = jobid;
    } else {
      // Add to end of chain
      storagedaemon::BsrJobid* bs = entry.JobId;
      for (; bs->next; bs = bs->next) {}
      bs->next = jobid;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_count(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  auto& entry = get_entry(bsr);

  token = LexGetToken(lc, BCT_PINT32);
  if (token == BCT_ERROR) { return NULL; }
  entry.count = lc->u.pint32_val;
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_fileregex(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  int rc;

  token = LexGetToken(lc, BCT_STRING);
  if (token == BCT_ERROR) { return NULL; }

  if (bsr->fileregex) { free(bsr->fileregex); }
  bsr->fileregex = strdup(lc->str);

  if (bsr->fileregex_re == NULL) {
    bsr->fileregex_re = (regex_t*)malloc(sizeof(regex_t));
  }

  rc = regcomp(bsr->fileregex_re, bsr->fileregex, REG_EXTENDED | REG_NOSUB);
  if (rc != 0) {
    char prbuf[500];
    regerror(rc, bsr->fileregex_re, prbuf, sizeof(prbuf));
    Emsg2(M_ERROR, 0, T_("REGEX '%s' compile error. ERR=%s\n"), bsr->fileregex,
          prbuf);
    return NULL;
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* StoreJobtype(
    LEX*,
    storagedaemon::BootStrapRecord* bsr)
{
  /* *****FIXME****** */
  Pmsg0(-1, T_("JobType not yet implemented\n"));
  return bsr;
}

static storagedaemon::BootStrapRecord* store_joblevel(
    LEX*,
    storagedaemon::BootStrapRecord* bsr)
{
  /* *****FIXME****** */
  Pmsg0(-1, T_("JobLevel not yet implemented\n"));
  return bsr;
}

// Routine to handle Volume start/end file
static storagedaemon::BootStrapRecord* store_volfile(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeFile* volfile;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    volfile = (storagedaemon::BsrVolumeFile*)malloc(
        sizeof(storagedaemon::BsrVolumeFile));
    memset(volfile, 0, sizeof(storagedaemon::BsrVolumeFile));
    volfile->sfile = lc->u.pint32_val;
    volfile->efile = lc->u2.pint32_val;

    // Add it to the end of the chain
    if (!entry.volfile) {
      entry.volfile = volfile;
    } else {
      // Add to end of chain
      storagedaemon::BsrVolumeFile* bs = entry.volfile;
      for (; bs->next; bs = bs->next) {}
      bs->next = volfile;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

// Routine to handle Volume start/end Block
static storagedaemon::BootStrapRecord* store_volblock(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeBlock* volblock;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    volblock = (storagedaemon::BsrVolumeBlock*)malloc(
        sizeof(storagedaemon::BsrVolumeBlock));
    memset(volblock, 0, sizeof(storagedaemon::BsrVolumeBlock));
    volblock->sblock = lc->u.pint32_val;
    volblock->eblock = lc->u2.pint32_val;

    // Add it to the end of the chain
    if (!entry.volblock) {
      entry.volblock = volblock;
    } else {
      // Add to end of chain
      storagedaemon::BsrVolumeBlock* bs = entry.volblock;
      for (; bs->next; bs = bs->next) {}
      bs->next = volblock;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

// Routine to handle Volume start/end address
static storagedaemon::BootStrapRecord* store_voladdr(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrVolumeAddress* voladdr;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT64_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    voladdr = (storagedaemon::BsrVolumeAddress*)malloc(
        sizeof(storagedaemon::BsrVolumeAddress));
    memset(voladdr, 0, sizeof(storagedaemon::BsrVolumeAddress));
    voladdr->saddr = lc->u.pint64_val;
    voladdr->eaddr = lc->u2.pint64_val;

    // Add it to the end of the chain
    if (!entry.voladdr) {
      entry.voladdr = voladdr;
    } else {
      // Add to end of chain
      storagedaemon::BsrVolumeAddress* bs = entry.voladdr;
      for (; bs->next; bs = bs->next) {}
      bs->next = voladdr;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_sessid(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrSessionId* sid;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32_RANGE);
    if (token == BCT_ERROR) { return NULL; }
    sid = (storagedaemon::BsrSessionId*)malloc(
        sizeof(storagedaemon::BsrSessionId));
    memset(sid, 0, sizeof(storagedaemon::BsrSessionId));
    sid->sessid = lc->u.pint32_val;
    sid->sessid2 = lc->u2.pint32_val;

    // Add it to the end of the chain
    if (!entry.sessid) {
      entry.sessid = sid;
    } else {
      // Add to end of chain
      storagedaemon::BsrSessionId* bs = entry.sessid;
      for (; bs->next; bs = bs->next) {}
      bs->next = sid;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_sesstime(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrSessionTime* stime;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_PINT32);
    if (token == BCT_ERROR) { return NULL; }
    stime = (storagedaemon::BsrSessionTime*)malloc(
        sizeof(storagedaemon::BsrSessionTime));
    memset(stime, 0, sizeof(storagedaemon::BsrSessionTime));
    stime->sesstime = lc->u.pint32_val;

    // Add it to the end of the chain
    if (!entry.sesstime) {
      entry.sesstime = stime;
    } else {
      // Add to end of chain
      storagedaemon::BsrSessionTime* bs = entry.sesstime;
      for (; bs->next; bs = bs->next) {}
      bs->next = stime;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_stream(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;
  storagedaemon::BsrStream* stream;

  auto& entry = get_entry(bsr);

  for (;;) {
    token = LexGetToken(lc, BCT_INT32);
    if (token == BCT_ERROR) { return NULL; }
    stream
        = (storagedaemon::BsrStream*)malloc(sizeof(storagedaemon::BsrStream));
    memset(stream, 0, sizeof(storagedaemon::BsrStream));
    stream->stream = lc->u.int32_val;

    // Add it to the end of the chain
    if (!entry.stream) {
      entry.stream = stream;
    } else {
      // Add to end of chain
      storagedaemon::BsrStream* bs = entry.stream;
      for (; bs->next; bs = bs->next) {}
      bs->next = stream;
    }
    token = LexGetToken(lc, BCT_ALL);
    if (token != BCT_COMMA) { break; }
  }
  return bsr;
}

static storagedaemon::BootStrapRecord* store_slot(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  int token;

  auto& entry = get_entry(bsr);

  token = LexGetToken(lc, BCT_PINT32);
  if (token == BCT_ERROR) { return NULL; }
  if (!entry.volume) {
    Emsg1(M_ERROR, 0, T_("Slot %d in bsr at inappropriate place.\n"),
          lc->u.pint32_val);
    return bsr;
  }
  entry.volume->Slot = lc->u.pint32_val;
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_include(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  ScanToEol(lc);
  return bsr;
}

static storagedaemon::BootStrapRecord* store_exclude(
    LEX* lc,
    storagedaemon::BootStrapRecord* bsr)
{
  ScanToEol(lc);
  return bsr;
}

static inline void DumpVolfile(storagedaemon::BsrVolumeFile* volfile)
{
  if (volfile) {
    Pmsg2(-1, T_("VolFile     : %u-%u\n"), volfile->sfile, volfile->efile);
    DumpVolfile(volfile->next);
  }
}

static inline void DumpVolblock(storagedaemon::BsrVolumeBlock* volblock)
{
  if (volblock) {
    Pmsg2(-1, T_("VolBlock    : %u-%u\n"), volblock->sblock, volblock->eblock);
    DumpVolblock(volblock->next);
  }
}

static inline void DumpVoladdr(storagedaemon::BsrVolumeAddress* voladdr)
{
  if (voladdr) {
    Pmsg2(-1, T_("VolAddr    : %llu-%llu\n"), voladdr->saddr, voladdr->eaddr);
    DumpVoladdr(voladdr->next);
  }
}

static inline void DumpFindex(storagedaemon::BsrFileIndex* FileIndex)
{
  if (FileIndex) {
    if (FileIndex->findex == FileIndex->findex2) {
      Pmsg1(-1, T_("FileIndex   : %u\n"), FileIndex->findex);
    } else {
      Pmsg2(-1, T_("FileIndex   : %u-%u\n"), FileIndex->findex,
            FileIndex->findex2);
    }
    DumpFindex(FileIndex->next);
  }
}

static inline void DumpJobid(storagedaemon::BsrJobid* jobid)
{
  if (jobid) {
    if (jobid->JobId == jobid->JobId2) {
      Pmsg1(-1, T_("JobId       : %u\n"), jobid->JobId);
    } else {
      Pmsg2(-1, T_("JobId       : %u-%u\n"), jobid->JobId, jobid->JobId2);
    }
    DumpJobid(jobid->next);
  }
}

static inline void DumpSessid(storagedaemon::BsrSessionId* sessid)
{
  if (sessid) {
    if (sessid->sessid == sessid->sessid2) {
      Pmsg1(-1, T_("SessId      : %u\n"), sessid->sessid);
    } else {
      Pmsg2(-1, T_("SessId      : %u-%u\n"), sessid->sessid, sessid->sessid2);
    }
    DumpSessid(sessid->next);
  }
}

static inline void DumpVolume(storagedaemon::BsrVolume* volume)
{
  if (volume) {
    Pmsg1(-1, T_("VolumeName  : %s\n"), volume->VolumeName);
    Pmsg1(-1, T_("  MediaType : %s\n"), volume->MediaType);
    Pmsg1(-1, T_("  Device    : %s\n"), volume->device);
    Pmsg1(-1, T_("  Slot      : %d\n"), volume->Slot);
    DumpVolume(volume->next);
  }
}

static inline void DumpClient(storagedaemon::BsrClient* client)
{
  if (client) {
    Pmsg1(-1, T_("Client      : %s\n"), client->ClientName);
    DumpClient(client->next);
  }
}

static inline void dump_job(storagedaemon::BsrJob* job)
{
  if (job) {
    Pmsg1(-1, T_("Job          : %s\n"), job->Job);
    dump_job(job->next);
  }
}

static inline void DumpSesstime(storagedaemon::BsrSessionTime* sesstime)
{
  if (sesstime) {
    Pmsg1(-1, T_("SessTime    : %u\n"), sesstime->sesstime);
    DumpSesstime(sesstime->next);
  }
}

void DumpBsr(storagedaemon::BootStrapRecord* bsr)
{
  int save_debug = debug_level;

  debug_level = 1;
  if (!bsr) {
    Pmsg0(-1, T_("storagedaemon::BootStrapRecord is NULL\n"));
    debug_level = save_debug;
    return;
  }
  Pmsg1(-1, T_("Root        : 0x%x\n"), bsr);
  Pmsg1(-1, T_("done        : %s\n"), bsr->done ? T_("yes") : T_("no"));
  Pmsg1(-1, T_("positioning : %d\n"), bsr->use_positioning);
  Pmsg1(-1, T_("fast_reject : %d\n"), bsr->use_fast_rejection);

  for (auto& entry : bsr->entries) {
    DumpVolume(entry.volume);
    DumpSessid(entry.sessid);
    DumpSesstime(entry.sesstime);
    DumpVolfile(entry.volfile);
    DumpVolblock(entry.volblock);
    DumpVoladdr(entry.voladdr);
    DumpClient(entry.client);
    DumpJobid(entry.JobId);
    dump_job(entry.job);
    DumpFindex(entry.FileIndex);
    if (entry.count) {
      Pmsg1(-1, T_("count       : %u\n"), entry.count);
      Pmsg1(-1, T_("found       : %u\n"), entry.found);
    }

    Pmsg0(-1, "\n");
  }

  debug_level = save_debug;
}

// Free all bsrs in chain
void FreeBsr(storagedaemon::BootStrapRecord* bsr)
{
  if (!bsr) { return; }

  delete bsr;
}

storagedaemon::BootStrapRecord* simple_bsr(JobControlRecord* jcr
                                           [[maybe_unused]],
                                           std::string_view VolumeNames)
{
  storagedaemon::BootStrapRecord* bsr = new_bsr();

  for (;;) {
    auto pos = VolumeNames.find_first_of('|');

    auto volname = VolumeNames.substr(0, pos);

    auto& volume_entry = get_entry(bsr, &storagedaemon::BootStrapEntry::volume);

    // bsr are still based on malloc/free

    void* memory = calloc(1, sizeof(*volume_entry.volume));
    volume_entry.volume = (storagedaemon::BsrVolume*)memory;

    auto str_len
        = std::max(sizeof(volume_entry.volume->VolumeName) - 1, volname.size());
    memcpy(volume_entry.volume->VolumeName, volname.data(), str_len);
    volume_entry.volume->VolumeName[str_len] = '\0';

    if (pos == VolumeNames.npos) {
      break;
    } else {
      VolumeNames.remove_prefix(pos + 1);
    }
  }

  bsr->use_fast_rejection = IsFastRejectionOk(bsr);
  bsr->use_positioning = IsPositioningOk(bsr);
  for (auto& entry : bsr->entries) { entry.root = bsr; }

  return bsr;
}

} /* namespace libbareos */
