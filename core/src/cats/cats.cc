/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2011-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
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
// Written by Marco van Wieringen, January 2011
/**
 * @file
 * Generic catalog class methods.
 */

#include "include/bareos.h"

#if HAVE_POSTGRESQL

#  include "cats.h"
#  include "db_conn.h"

#  include "bdb_query_names.inc"
#  include "lib/berrno.h"

#  include "postgresql.h"

bool BareosDb::MatchDatabase(const char* db_name,
                             const char* db_address,
                             std::uint32_t db_port)
{
  bool match = bstrcmp(params_.db_name.c_str(), db_name)
               && bstrcmp(params_.db_address.c_str(), db_address)
               && params_.db_port == db_port;
  return match;
}

/**
 * Clone a BareosDb class by either increasing the reference count
 * (when mult_db_connection == false) or by getting a new
 * connection otherwise. We use a method so we can reference
 * the protected members of the class.
 */
std::unique_ptr<BareosDb> BareosDb::CloneDatabaseConnection(
    JobControlRecord* jcr,
    bool mult_db_connections,
    bool need_private)
{
  auto copy = params_;
  copy.mult_db_connections = mult_db_connections;
  copy.need_private = need_private;
  return DbCreateConnection(jcr, std::move(copy));
}

const char* BareosDb::GetType(void) { return BackendCon->GetType(); }

/**
 * Lock database, this can be called multiple times by the same
 * thread without blocking, but must be unlocked the number of
 * times it was locked using DbUnlock().
 */
void BareosDb::LockDb(const char* file, int line)
{
  int errstat;

  if ((errstat = RwlWritelock(&lock_)) != 0) {
    BErrNo be;
    e_msg(file, line, M_FATAL, 0, "RwlWritelock failure. stat=%d: ERR=%s\n",
          errstat, be.bstrerror(errstat));
  }
}

/**
 * Unlock the database. This can be called multiple times by the
 * same thread up to the number of times that thread called
 * DbLock()/
 */
void BareosDb::UnlockDb(const char* file, int line)
{
  int errstat;

  if ((errstat = RwlWriteunlock(&lock_)) != 0) {
    BErrNo be;
    e_msg(file, line, M_FATAL, 0, "RwlWriteunlock failure. stat=%d: ERR=%s\n",
          errstat, be.bstrerror(errstat));
  }
}

void BareosDb::PrintLockInfo(FILE* fp)
{
  if (lock_.valid == RWLOCK_VALID) {
    fprintf(fp, "\tRWLOCK=%p w_active=%i w_wait=%i\n", &lock_, lock_.w_active,
            lock_.w_wait);
  }
}

std::unique_ptr<BareosDb> DbCreateConnection(JobControlRecord* jcr,
                                             connection_parameter params)
{
  auto* backend_con = postgresql::connect(jcr, params);
  if (!backend_con) {
    Jmsg(jcr, M_FATAL, 0, "%s", "could not establish postgresql connection");
    return nullptr;
  }

  auto ptr = std::make_unique<BareosDb>(std::move(params), backend_con);

  if (!ptr->CheckTablesVersion(jcr)) {
    /*** FIXME ***/
    // add cleanup for backend_con
    backend_con->CloseDatabase(jcr);
    return nullptr;
  }

  return ptr;
}

#endif /* HAVE_POSTGRESQL */
