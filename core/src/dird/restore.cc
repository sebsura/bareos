/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2024 Bareos GmbH & Co. KG

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
// Kern Sibbald, November MM
/**
 * @file
 * responsible for restoring files
 *
 * This routine is run as a separate thread.
 *
 * Current implementation is Catalog verification only (i.e. no verification
 * versus tape).
 *
 * Basic tasks done here:
 *    Open DB
 *    Open Message Channel with Storage daemon to tell him a job will be
 * starting. Open connection with File daemon and pass him commands to do the
 * restore.
 */


#include "include/bareos.h"
#include "dird.h"
#include "dird/dird_globals.h"
#include "dird/backup.h"
#include "dird/fd_cmds.h"
#include "dird/getmsg.h"
#include "dird/director_jcr_impl.h"
#include "dird/job.h"
#include "dird/msgchan.h"
#include "dird/restore.h"
#include "dird/sd_cmds.h"
#include "dird/storage.h"
#include "include/protocol_types.h"
#include "lib/edit.h"
#include "lib/util.h"
#include "lib/version.h"
#include "lib/tree.h"
#include "lib/attribs.h"

namespace directordaemon {

/* Commands sent to File daemon */
static char restorecmd[] = "restore replace=%c prelinks=%d where=%s\n";
static char restorecmdR[] = "restore replace=%c prelinks=%d regexwhere=%s\n";
static char storaddrcmd[]
    = "storage address=%s port=%d ssl=%d Authorization=%s\n";
static char setauthorizationcmd[] = "setauthorization Authorization=%s\n";
static char passiveclientcmd[] = "passive client address=%s port=%d ssl=%d\n";

/* Responses received from File daemon */
static char OKrestore[] = "2000 OK restore\n";
static char OKstore[] = "2000 OK storage\n";
static char OKstoreend[] = "2000 OK storage end\n";
static char OKAuthorization[] = "2000 OK Authorization\n";
static char OKpassiveclient[] = "2000 OK passive client\n";

/* Responses received from the Storage daemon */
static char OKbootstrap[] = "3000 OK bootstrap\n";

static void BuildRestoreCommand(JobControlRecord* jcr, PoolMem& ret)
{
  char replace, *where, *cmd;
  char empty = '\0';

  // Build the restore command
  if (jcr->dir_impl->replace != 0) {
    replace = jcr->dir_impl->replace;
  } else if (jcr->dir_impl->res.job->replace != 0) {
    replace = jcr->dir_impl->res.job->replace;
  } else {
    replace = REPLACE_ALWAYS; /* always replace */
  }

  if (jcr->RegexWhere) {
    where = jcr->RegexWhere; /* override */
    cmd = restorecmdR;
  } else if (jcr->dir_impl->res.job->RegexWhere) {
    where = jcr->dir_impl->res.job->RegexWhere; /* no override take from job */
    cmd = restorecmdR;
  } else if (jcr->where) {
    where = jcr->where; /* override */
    cmd = restorecmd;
  } else if (jcr->dir_impl->res.job->RestoreWhere) {
    where
        = jcr->dir_impl->res.job->RestoreWhere; /* no override take from job */
    cmd = restorecmd;
  } else {          /* nothing was specified */
    where = &empty; /* use default */
    cmd = restorecmd;
  }

  jcr->prefix_links = jcr->dir_impl->res.job->PrefixLinks;

  BashSpaces(where);
  Mmsg(ret, cmd, replace, jcr->prefix_links, where);
  UnbashSpaces(where);
}

/**
 * The bootstrap is stored in a file, so open the file, and loop
 *   through it processing each storage device in turn. If the
 *   storage is different from the prior one, we open a new connection
 *   to the new storage and do a restore for that part.
 *
 * This permits handling multiple storage daemons for a single
 *   restore.  E.g. your Full is stored on tape, and Incrementals
 *   on disk.
 */
static inline bool DoNativeRestoreBootstrap(JobControlRecord* jcr)
{
  StorageResource* store;
  ClientResource* client;
  bootstrap_info info;
  BareosSocket* fd = NULL;
  BareosSocket* sd = NULL;
  bool first_time = true;
  PoolMem RestoreCmd(PM_MESSAGE);
  char* connection_target_address;

  client = jcr->dir_impl->res.client;
  // This command is used for each part
  BuildRestoreCommand(jcr, RestoreCmd);

  // Open the bootstrap file
  if (!OpenBootstrapFile(jcr, info)) { goto bail_out; }

  // Read the bootstrap file
  jcr->passive_client = client->passive;
  while (!feof(info.bs)) {
    if (!SelectNextRstore(jcr, info)) { goto bail_out; }
    store = jcr->dir_impl->res.read_storage;

    /* Open a message channel connection with the Storage
     * daemon. This is to let him know that our client
     * will be contacting him for a backup session.
     * */
    Dmsg0(10, "Open connection with storage daemon\n");
    jcr->setJobStatusWithPriorityCheck(JS_WaitSD);

    // Start conversation with Storage daemon
    if (!ConnectToStorageDaemon(jcr, 10, me->SDConnectTimeout, true)) {
      goto bail_out;
    }
    sd = jcr->store_bsock;

    // Now start a job with the Storage daemon
    if (!StartStorageDaemonJob(jcr)) { goto bail_out; }
    if (!ReserveReadDevice(jcr, jcr->dir_impl->res.read_storage_list)) {
      goto bail_out;
    }

    if (first_time) {
      // Start conversation with File daemon
      jcr->setJobStatusWithPriorityCheck(JS_WaitFD);
      jcr->dir_impl->keep_sd_auth_key
          = true; /* don't clear the sd_auth_key now */

      if (!ConnectToFileDaemon(jcr, 10, me->FDConnectTimeout, true)) {
        goto bail_out;
      }
      SendJobInfoToFileDaemon(jcr);
      fd = jcr->file_bsock;

      if (!SendSecureEraseReqToFd(jcr)) {
        Dmsg1(500, "Unexpected %s secure erase\n", "client");
      }

      // Check if the file daemon supports passive client mode.
      if (jcr->passive_client && jcr->dir_impl->FDVersion < FD_VERSION_51) {
        Jmsg(jcr, M_FATAL, 0,
             T_("Client \"%s\" doesn't support passive client mode. "
                "Please upgrade your client or disable compat mode.\n"),
             jcr->dir_impl->res.client->resource_name_);
        goto bail_out;
      }
    }

    jcr->setJobStatusWithPriorityCheck(JS_Running);

    // Send the bootstrap file -- what Volumes/files to restore
    bool success = false;
    if (SendBootstrapFile(jcr, sd, info)) {
      Bmicrosleep(2, 0);
      if (response(jcr, sd, OKbootstrap, "Bootstrap", DISPLAY_ERROR)) {
        success = true;
      }
    }
    if (!success) { goto bail_out; }

    if (!jcr->passive_client) {
      /* When the client is not in passive mode we can put the SD in
       * listen mode for the FD connection. And ask the FD to connect
       * to the SD. */
      if (!sd->fsend("run")) { goto bail_out; }

      // Now start a Storage daemon message thread
      if (!StartStorageDaemonMessageThread(jcr)) { goto bail_out; }
      Dmsg0(50, "Storage daemon connection OK\n");

      /* Send Storage daemon address to the File daemon,
       * then wait for File daemon to make connection
       * with Storage daemon. */

      // TLS Requirement

      TlsPolicy tls_policy;
      if (jcr->dir_impl->res.client->connection_successful_handshake_
          != ClientConnectionHandshakeMode::kTlsFirst) {
        tls_policy = store->GetPolicy();
      } else {
        tls_policy = store->IsTlsConfigured() ? TlsPolicy::kBnetTlsAuto
                                              : TlsPolicy::kBnetTlsNone;
      }

      Dmsg1(200, "Tls Policy for active client is: %d\n", tls_policy);

      connection_target_address = StorageAddressToContact(client, store);

      fd->fsend(storaddrcmd, connection_target_address, store->SDport,
                tls_policy, jcr->sd_auth_key);
      memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));

      Dmsg1(6, "dird>filed: %s", fd->msg);
      if (!response(jcr, fd, OKstore, "Storage", DISPLAY_ERROR)) {
        goto bail_out;
      }
    } else {
      /* In passive mode we tell the FD what authorization key to use
       * and the ask the SD to initiate the connection. */
      fd->fsend(setauthorizationcmd, jcr->sd_auth_key);
      memset(jcr->sd_auth_key, 0, strlen(jcr->sd_auth_key));

      Dmsg1(6, "dird>filed: %s", fd->msg);
      if (!response(jcr, fd, OKAuthorization, "Setauthorization",
                    DISPLAY_ERROR)) {
        goto bail_out;
      }

      TlsPolicy tls_policy;

      if (jcr->dir_impl->res.client->connection_successful_handshake_
          != ClientConnectionHandshakeMode::kTlsFirst) {
        tls_policy = client->GetPolicy();
      } else {
        tls_policy = client->IsTlsConfigured() ? TlsPolicy::kBnetTlsAuto
                                               : TlsPolicy::kBnetTlsNone;
      }

      Dmsg1(200, "Tls Policy for passive client is: %d\n", tls_policy);

      connection_target_address = ClientAddressToContact(client, store);
      // Tell the SD to connect to the FD.
      sd->fsend(passiveclientcmd, connection_target_address, client->FDport,
                tls_policy);
      Bmicrosleep(2, 0);
      if (!response(jcr, sd, OKpassiveclient, "Passive client",
                    DISPLAY_ERROR)) {
        goto bail_out;
      }

      // Start the Job in the SD.
      if (!sd->fsend("run")) { goto bail_out; }

      // Now start a Storage daemon message thread
      if (!StartStorageDaemonMessageThread(jcr)) { goto bail_out; }
      Dmsg0(50, "Storage daemon connection OK\n");
    }

    // Declare the job started to start the MaxRunTime check
    jcr->setJobStarted();

    // Only pass "global" commands to the FD once
    if (first_time) {
      first_time = false;
      if (!SendRunscriptsCommands(jcr)) { goto bail_out; }

      // Only FD version 52 and later understand the sending of plugin options.
      if (jcr->dir_impl->FDVersion >= FD_VERSION_52) {
        if (!SendPluginOptions(jcr)) {
          Dmsg0(000, "FAIL: Send plugin options\n");
          goto bail_out;
        }
      } else {
        /* Plugin options specified and not a FD that understands the new
         * protocol keyword. */
        if (jcr->dir_impl->plugin_options) {
          Jmsg(jcr, M_FATAL, 0,
               T_("Client \"%s\" doesn't support plugin option passing. "
                  "Please upgrade your client or disable compat mode.\n"),
               jcr->dir_impl->res.client->resource_name_);
          goto bail_out;
        }
      }

      if (!SendRestoreObjects(jcr, 0, true)) {
        Dmsg0(000, "FAIL: Send restore objects\n");
        goto bail_out;
      }
    }

    fd->fsend("%s", RestoreCmd.c_str());

    if (!response(jcr, fd, OKrestore, "Restore", DISPLAY_ERROR)) {
      goto bail_out;
    }

    if (jcr->dir_impl->FDVersion < FD_VERSION_2) { /* Old FD */
      break;                                       /* we do only one loop */
    } else {
      if (!response(jcr, fd, OKstoreend, "Store end", DISPLAY_ERROR)) {
        goto bail_out;
      }
      WaitForStorageDaemonTermination(jcr);
    }
  } /* the whole boostrap has been send */

  if (fd && jcr->dir_impl->FDVersion >= FD_VERSION_2) {
    fd->fsend("endrestore");
  }

  CloseBootstrapFile(info);
  return true;

bail_out:
  if (jcr->file_bsock) {
    jcr->file_bsock->signal(BNET_TERMINATE);
    jcr->file_bsock->close();
    delete jcr->file_bsock;
    jcr->file_bsock = NULL;
  }

  CloseBootstrapFile(info);
  return false;
}

/**
 * Do a restore initialization.
 *
 *  Returns:  false on failure
 *            true on success
 */
bool DoNativeRestoreInit(JobControlRecord* jcr)
{
  FreeWstorage(jcr); /* we don't write */

  return true;
}

/**
 * Do a restore of the specified files
 *
 *  Returns:  false on failure
 *            true on success
 */
bool DoNativeRestore(JobControlRecord* jcr)
{
  int status;

  jcr->dir_impl->jr.JobLevel = L_FULL; /* Full restore */
  if (!jcr->db->UpdateJobStartRecord(jcr, &jcr->dir_impl->jr)) {
    Jmsg(jcr, M_FATAL, 0, "%s", jcr->db->strerror());
    goto bail_out;
  }
  Dmsg0(20, "Updated job start record\n");

  Dmsg1(20, "RestoreJobId=%d\n", jcr->dir_impl->res.job->RestoreJobId);

  if (!jcr->RestoreBootstrap) {
    Jmsg(jcr, M_FATAL, 0,
         T_("Cannot restore without a bootstrap file.\n"
            "You probably ran a restore job directly. All restore jobs must\n"
            "be run using the restore command.\n"));
    goto bail_out;
  }

  // Print Job Start message
  Jmsg(jcr, M_INFO, 0, T_("Start Restore Job %s\n"), jcr->Job);

  // Read the bootstrap file and do the restore
  if (!DoNativeRestoreBootstrap(jcr)) { goto bail_out; }

  // Wait for Job Termination
  status = WaitForJobTermination(jcr);
  NativeRestoreCleanup(jcr, status);
  return true;

bail_out:
  NativeRestoreCleanup(jcr, JS_ErrorTerminated);
  return false;
}

// Release resources allocated during restore.
void NativeRestoreCleanup(JobControlRecord* jcr, int TermCode)
{
  char term_code[100];
  const char* TermMsg;
  int msg_type = M_INFO;

  Dmsg0(20, "In NativeRestoreCleanup\n");

  if (jcr->dir_impl->unlink_bsr && jcr->RestoreBootstrap) {
    SecureErase(jcr, jcr->RestoreBootstrap);
    jcr->dir_impl->unlink_bsr = false;
  }

  if (jcr->IsJobCanceled()) { CancelStorageDaemonJob(jcr); }

  if (jcr->dir_impl->ExpectedFiles != jcr->JobFiles) {
    Jmsg(jcr, M_WARNING, 0,
         T_("File count mismatch: expected=%lu , restored=%lu\n"),
         jcr->dir_impl->ExpectedFiles, jcr->JobFiles);
    if (TermCode == JS_Terminated) { TermCode = JS_Warnings; }
  }

  switch (TermCode) {
    case JS_Terminated:
      TermMsg = T_("Restore OK");
      break;
    case JS_Warnings:
      TermMsg = T_("Restore OK -- with warnings");
      break;
    case JS_FatalError:
    case JS_ErrorTerminated:
      TermMsg = T_("*** Restore Error ***");
      msg_type = M_ERROR;  // Generate error message
      if (jcr->store_bsock) {
        jcr->store_bsock->signal(BNET_TERMINATE);
        if (jcr->dir_impl->SD_msg_chan_started) {
          pthread_cancel(jcr->dir_impl->SD_msg_chan);
        }
      }
      break;
    case JS_Canceled:
      TermMsg = T_("Restore Canceled");
      if (jcr->store_bsock) {
        jcr->store_bsock->signal(BNET_TERMINATE);
        if (jcr->dir_impl->SD_msg_chan_started) {
          pthread_cancel(jcr->dir_impl->SD_msg_chan);
        }
      }
      break;
    default:
      TermMsg = term_code;
      sprintf(term_code, T_("Inappropriate term code: %c\n"), TermCode);
      break;
  }

  UpdateJobEnd(jcr, TermCode);

  GenerateRestoreSummary(jcr, msg_type, TermMsg);

  Dmsg0(20, "Leaving NativeRestoreCleanup\n");
}

/*
 * Generic function which generates a restore summary message.
 * Used by:
 *    - NativeRestoreCleanup e.g. normal restores
 *    - NdmpRestoreCleanup e.g. NDMP restores
 */
void GenerateRestoreSummary(JobControlRecord* jcr,
                            int msg_type,
                            const char* TermMsg)
{
  char sdt[MAX_TIME_LENGTH], edt[MAX_TIME_LENGTH];
  char ec1[30], ec2[30], ec3[30], elapsed[50];
  utime_t RunTime;
  double kbps;
  PoolMem temp, secure_erase_status;

  bstrftimes(sdt, sizeof(sdt), jcr->dir_impl->jr.StartTime);
  bstrftimes(edt, sizeof(edt), jcr->dir_impl->jr.EndTime);
  RunTime = jcr->dir_impl->jr.EndTime - jcr->dir_impl->jr.StartTime;
  if (RunTime <= 0) {
    kbps = 0;
  } else {
    kbps = ((double)jcr->dir_impl->jr.JobBytes) / (1000.0 * (double)RunTime);
  }
  if (kbps < 0.05) { kbps = 0; }

  std::string fd_term_msg = JobstatusToAscii(jcr->dir_impl->FDJobStatus);
  std::string sd_term_msg = JobstatusToAscii(jcr->dir_impl->SDJobStatus);

  ClientDbRecord cr;
  bstrncpy(cr.Name, jcr->dir_impl->res.client->resource_name_, sizeof(cr.Name));
  if (!jcr->db->GetClientRecord(jcr, &cr)) {
    Jmsg(jcr, M_WARNING, 0,
         T_("Error getting Client record for Job report: ERR=%s\n"),
         jcr->db->strerror());
    // if we could not look up the client record we print nothing
    cr.Uname[0] = '\0';
  }

  switch (jcr->getJobProtocol()) {
    case PT_NDMP_BAREOS:
    case PT_NDMP_NATIVE:
      Jmsg(jcr, msg_type, 0,
           T_("%s %s %s (%s):\n"
              "  Build OS:               %s\n"
              "  JobId:                  %d\n"
              "  Job:                    %s\n"
              "  Restore Client:         \"%s\" %s\n"
              "  Start time:             %s\n"
              "  End time:               %s\n"
              "  Elapsed time:           %s\n"
              "  Files Expected:         %s\n"
              "  Files Restored:         %s\n"
              "  Bytes Restored:         %s\n"
              "  Rate:                   %.1f KB/s\n"
              "  SD termination status:  %s\n"
              "  Bareos binary info:     %s\n"
              "  Job triggered by:       %s\n"
              "  Termination:            %s\n\n"),
           BAREOS, my_name, kBareosVersionStrings.Full,
           kBareosVersionStrings.ShortDate, kBareosVersionStrings.GetOsInfo(),
           jcr->dir_impl->jr.JobId, jcr->dir_impl->jr.Job,
           jcr->dir_impl->res.client->resource_name_, cr.Uname, sdt, edt,
           edit_utime(RunTime, elapsed, sizeof(elapsed)),
           edit_uint64_with_commas((uint64_t)jcr->dir_impl->ExpectedFiles, ec1),
           edit_uint64_with_commas((uint64_t)jcr->dir_impl->jr.JobFiles, ec2),
           edit_uint64_with_commas(jcr->dir_impl->jr.JobBytes, ec3),
           (float)kbps, sd_term_msg.c_str(),
           kBareosVersionStrings.JoblogMessage,
           JobTriggerToString(jcr->dir_impl->job_trigger).c_str(), TermMsg);
      break;
    default:
      if (me->secure_erase_cmdline) {
        Mmsg(temp, "  Dir Secure Erase Cmd:   %s\n", me->secure_erase_cmdline);
        PmStrcat(secure_erase_status, temp.c_str());
      }
      if (!bstrcmp(jcr->dir_impl->FDSecureEraseCmd, "*None*")) {
        Mmsg(temp, "  FD  Secure Erase Cmd:   %s\n",
             jcr->dir_impl->FDSecureEraseCmd);
        PmStrcat(secure_erase_status, temp.c_str());
      }
      if (!bstrcmp(jcr->dir_impl->SDSecureEraseCmd, "*None*")) {
        Mmsg(temp, "  SD  Secure Erase Cmd:   %s\n",
             jcr->dir_impl->SDSecureEraseCmd);
        PmStrcat(secure_erase_status, temp.c_str());
      }

      Jmsg(jcr, msg_type, 0,
           T_("%s %s %s (%s):\n"
              "  Build OS:               %s\n"
              "  JobId:                  %d\n"
              "  Job:                    %s\n"
              "  Restore Client:         \"%s\" %s\n"
              "  Start time:             %s\n"
              "  End time:               %s\n"
              "  Elapsed time:           %s\n"
              "  Files Expected:         %s\n"
              "  Files Restored:         %s\n"
              "  Bytes Restored:         %s\n"
              "  Rate:                   %.1f KB/s\n"
              "  FD Errors:              %d\n"
              "  FD termination status:  %s\n"
              "  SD termination status:  %s\n"
              "%s"
              "  Bareos binary info:     %s\n"
              "  Job triggered by:       %s\n"
              "  Termination:            %s\n\n"),
           BAREOS, my_name, kBareosVersionStrings.Full,
           kBareosVersionStrings.ShortDate, kBareosVersionStrings.GetOsInfo(),
           jcr->dir_impl->jr.JobId, jcr->dir_impl->jr.Job,
           jcr->dir_impl->res.client->resource_name_, cr.Uname, sdt, edt,
           edit_utime(RunTime, elapsed, sizeof(elapsed)),
           edit_uint64_with_commas((uint64_t)jcr->dir_impl->ExpectedFiles, ec1),
           edit_uint64_with_commas((uint64_t)jcr->dir_impl->jr.JobFiles, ec2),
           edit_uint64_with_commas(jcr->dir_impl->jr.JobBytes, ec3),
           (float)kbps, jcr->JobErrors, fd_term_msg.c_str(),
           sd_term_msg.c_str(), secure_erase_status.c_str(),
           kBareosVersionStrings.JoblogMessage,
           JobTriggerToString(jcr->dir_impl->job_trigger).c_str(), TermMsg);
      break;
  }
}

struct TreeArgs {
  enum class selection
  {
    None,
    All,
  };

  std::unordered_set<JobId_t> jobids;
  std::size_t estimated_size;
  selection initial_selection;
};

struct InsertTreeContext {
  std::size_t TotalCount;
  TREE_ROOT* root;

  std::optional<std::string> error;
  bool mark_on_create;
};

static inline bool ShouldOverwriteNode(TREE_NODE* node,
                                       JobId_t jobid,
                                       int32_t FileIndex,
                                       bool hard_link)
{
  // if the node is new, we "overwrite" it
  if (node->inserted) { return true; }
  // if the node is from a different job, we overwrite it
  if (node->JobId != jobid) { return true; }

  // normally the same path should not be included in the same job multiple
  // times, but they are technically possible so we still have to handle them!

  if (hard_link) {
    // for hardlink we use the first/oldest node
    // since the other copies should just be links to this one
    return FileIndex <= node->FileIndex;
  }

  // ... otherwise we use the last/newest node
  return FileIndex >= node->FileIndex;
}

static int InsertTreeHandler(void* arg, int num_rows, char** row)
{
  auto* ctx = static_cast<InsertTreeContext*>(arg);

  if (ctx->error) {
    ctx->error = std::string{"Handler called while in error with \""}
                 + ctx->error.value() + "\"";
  }

  if (num_rows != 8) {
    ctx->error = "Handler called with bad row (count = "
                 + std::to_string(num_rows) + ")";
    return 1;
  }

  const char* str_path = row[0];
  const char* str_file = row[1];
  const char* str_findex = row[2];
  const char* str_jobid = row[3];
  const char* str_lstat = row[4];
  const char* str_dseq = row[5];
  const char* str_fhinfo = row[6];
  const char* str_fhnode = row[7];

  int type;

  if (str_file[0] == 0) {                /* no filename => directory */
    if (!IsPathSeparator(str_path[0])) { /* Must be Win32 directory */
      type = TN_DIR_NLS;
    } else {
      type = TN_DIR;
    }
  } else {
    type = TN_FILE;
  }

  auto* node
      = insert_tree_node(const_cast<char*>(str_path),
                         const_cast<char*>(str_file), type, ctx->root, NULL);

  JobId_t JobId = str_to_int64(str_jobid);
  int32_t FileIndex = str_to_int64(str_findex);
  int32_t delta_seq = str_to_int64(str_dseq);

  int32_t LinkFI;
  struct stat statp;
  DecodeStat(const_cast<char*>(str_lstat), &statp, sizeof(statp), &LinkFI);

  bool hard_link = (LinkFI != 0);

  // TODO handle delta_seq

  if (ShouldOverwriteNode(node, JobId, FileIndex, hard_link)) {
    node->soft_link = S_ISLNK(statp.st_mode) != 0;
    node->hard_link = hard_link;

    node->FileIndex = FileIndex;
    node->type = type;
    node->delta_seq = delta_seq;
    node->fhinfo = str_to_uint64(str_fhinfo);
    node->fhnode = str_to_uint64(str_fhnode);
    node->JobId = JobId;

    if (ctx->mark_on_create) {
      node->extract = true;
      if (type == TN_DIR || type == TN_DIR_NLS) { node->extract_dir = true; }
    }

    if (statp.st_nlink > 1 && type != TN_DIR && type != TN_DIR_NLS) {
      if (!LinkFI) {
        // First occurence - file hardlinked to
        auto* entry
            = (HL_ENTRY*)ctx->root->hardlinks.hash_malloc(sizeof(HL_ENTRY));
        entry->key = (((uint64_t)JobId) << 32) + FileIndex;
        entry->node = node;
        ctx->root->hardlinks.insert(entry->key, entry);
      } else {
        // Hardlink to known file index: lookup original file
        uint64_t file_key = (((uint64_t)JobId) << 32) + LinkFI;
        HL_ENTRY* first_hl = (HL_ENTRY*)ctx->root->hardlinks.lookup(file_key);

        if (first_hl && first_hl->node) {
          // Then add hardlink entry to linked node.
          auto* entry
              = (HL_ENTRY*)ctx->root->hardlinks.hash_malloc(sizeof(HL_ENTRY));
          entry->key = (((uint64_t)JobId) << 32) + FileIndex;
          entry->node = first_hl->node;
          ctx->root->hardlinks.insert(entry->key, entry);
        }
      }
    }

    if (node->inserted) { ctx->TotalCount += 1; }
  }

  return 0;
}

InsertTreeContext BuildDirectoryTree(BareosDb* db, TreeArgs args)
{
  auto* root = new_tree(args.estimated_size);


  InsertTreeContext ctx;
  ctx.root = root;
  ctx.mark_on_create = args.initial_selection == TreeArgs::selection::All;
  ctx.TotalCount = 0;

  std::string jobids;
  for (auto& jobid : args.jobids) {
    if (jobids.size()) { jobids += ","; }
    jobids += jobid;
  }
  bool get_md5 = false;
  bool get_delta = true;

  if (!db->GetFileList(nullptr, jobids.c_str(), get_md5, get_delta,
                       InsertTreeHandler, &ctx)) {
    if (ctx.error) {
      ctx.error.value() += std::string{"\n"} + db->strerror();
    } else {
      ctx.error = db->strerror();
    }
    return ctx;
  }

  return ctx;
}


} /* namespace directordaemon */
