/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#include "create.h"
#include <utility>

#include "job.h"
#include "jcr_util.h"
#include "include/protocol_types.h"


namespace directordaemon {
namespace {
bool SetDataFromJob(JobControlRecord* jcr, JobResource* job)
{
  ASSERT(job);

  if (job->JobType != JT_RESTORE) { return false; }

  // Copy storage definitions -- deleted in dir_free_jcr above
  // if (job->storage) {
  //   CopyRwstorage(jcr, job->storage, T_("Job resource"));
  // } else if (job->pool) {
  //   CopyRwstorage(jcr, job->pool->storage, T_("Pool resource"));
  // }

  jcr->setJobType(JT_RESTORE);
  jcr->setJobStatus(JS_Created);
  jcr->setJobProtocol(job->Protocol);
  jcr->setJobLevel(L_FULL);  // restores always have Level = FULL

  jcr->JobPriority = job->Priority;
  jcr->allow_mixed_priority = job->allow_mixed_priority;

  jcr->dir_impl->max_concurrent_jobs = job->MaxConcurrentJobs;
  // jcr->dir_impl->res.fileset = job->fileset;
  // jcr->accurate = job->accurate;
  // jcr->dir_impl->res.messages = job->messages;
  // jcr->dir_impl->spool_data = job->spool_data;
  // jcr->dir_impl->spool_size = job->spool_size;
  // jcr->dir_impl->IgnoreDuplicateJobChecking =
  // job->IgnoreDuplicateJobChecking; jcr->dir_impl->MaxRunSchedTime =
  // job->MaxRunSchedTime;

  return true;
}
};  // namespace

JobControlRecord* CreateJob(RestoreOptions&& opts)
{
  std::unique_ptr<JobControlRecord> jcr{NewDirectorJcr(DirdFreeJcr)};

  if (!SetDataFromJob(jcr.get(), opts.job)) {
    // error: bad job
    return nullptr;
  }


  // TODO: add compile time safety
  if (auto* ndmp = std::get_if<RestoreOptions::ndmp_data>(&opts.data)) {
    if (opts.job->Protocol != PT_NDMP_BAREOS
        && opts.job->Protocol != PT_NDMP_NATIVE) {
      // error: bad protocol
      return nullptr;
    }

    jcr->JobIds = strdup(ndmp->JobIds.c_str());
    jcr->dir_impl->restore_tree_root
        = std::exchange(ndmp->restore_tree, nullptr);
  } else if (auto* native
             = std::get_if<RestoreOptions::native_data>(&opts.data)) {
    if (opts.job->Protocol != PT_NATIVE) {
      // error: bad protocol
      return nullptr;
    }

    jcr->RestoreBootstrap = strdup(native->BootStrapPath.c_str());
    jcr->dir_impl->ExpectedFiles = native->expected_file_count;
    jcr->dir_impl->unlink_bsr = native->unlink_bsr;
  } else {
    ASSERT(0);
  }

  // set resources
  jcr->dir_impl->res.job = opts.job;
  jcr->dir_impl->res.rjs = opts.job->rjs;
  jcr->dir_impl->res.client = opts.restore_client;
  jcr->dir_impl->res.catalog = opts.catalog;
  jcr->dir_impl->res.catalog_source = nullptr;  // TODO + allocate
  jcr->dir_impl->res.messages = opts.job->messages;
  jcr->dir_impl->res.pool = opts.job->pool;

  if (opts.backup_format) {
    jcr->dir_impl->backup_format = strdup(opts.backup_format->c_str());
  }

  if (opts.comment) { PmStrcpy(jcr->comment, opts.comment->c_str()); }

  if (opts.location) {
    // TODO: make compile time safe
    if (auto* where
        = std::get_if<RestoreOptions::where>(&opts.location.value())) {
      jcr->where = strdup(where->c_str());
    } else if (auto* regex = std::get_if<RestoreOptions::regex_where>(
                   &opts.location.value())) {
      jcr->RegexWhere = strdup(regex->c_str());
    } else {
      ASSERT(0);
    }
  }

  if (opts.plugin_options) {
    // poolstr
    PmStrcpy(jcr->dir_impl->plugin_options, opts.plugin_options->c_str());
  }

  jcr->dir_impl->replace
      = (uint32_t)opts.replace.value_or(replace_option::Always);

  return jcr.release();
}

};  // namespace directordaemon
