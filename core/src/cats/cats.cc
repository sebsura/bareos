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

#  include "bdb_query_names.inc"
#  include "lib/berrno.h"

bool BareosDb::MatchDatabase(const char* db_driver,
                             const char* db_name,
                             const char* db_address,
                             int db_port)
{
  bool match;

  if (db_driver) {
    match = Bstrcasecmp(db_driver_, db_driver) && bstrcmp(db_name_, db_name)
            && bstrcmp(db_address_, db_address) && db_port_ == db_port;
  } else {
    match = bstrcmp(db_name_, db_name) && bstrcmp(db_address_, db_address)
            && db_port_ == db_port;
  }
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
  return DbCreateConnection(jcr, db_driver_, db_name_, db_user_, db_password_,
                            db_address_, db_port_, db_socket_,
                            mult_db_connections, disabled_batch_insert_,
                            try_reconnect_, exit_on_fatal_, need_private);
}

const char* BareosDb::GetType(void)
{
  switch (db_interface_type_) {
    case SQL_INTERFACE_TYPE_POSTGRESQL:
      return "PostgreSQL";
    default:
      return "Unknown";
  }
}

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
                                             const char* db_drivername,
                                             const char* db_name,
                                             const char* db_user,
                                             const char* db_password,
                                             const char* db_address,
                                             int db_port,
                                             const char* db_socket,
                                             bool mult_db_connections,
                                             bool disable_batch_insert,
                                             bool try_reconnect,
                                             bool exit_on_fatal,
                                             bool need_private)
{
  // BareosDb* mdb;
  // Dmsg1(100,
  //       "DbSqlGetNonPooledConnection allocating 1 new non pooled database "
  //       "connection to database %s\n",
  //       db_name);

  // mdb = db_init_database(jcr, db_drivername, db_name, db_user, db_password,
  //                        db_address, db_port, db_socket, mult_db_connections,
  //                        disable_batch_insert, try_reconnect, exit_on_fatal,
  //                        need_private);
  // if (mdb == NULL) { return NULL; }

  // if (auto err = mdb->BackendCon->OpenDatabase(jcr)) {
  //   Jmsg(jcr, M_FATAL, 0, "%s", err);
  //   mdb->BackendCon->CloseDatabase(jcr);
  //   return NULL;
  // }

  // return mdb;

  /*** FIXUP ***/
  (void)jcr;
  (void)db_drivername;
  (void)db_name;
  (void)db_user;
  (void)db_password;
  (void)db_address;
  (void)db_port;
  (void)db_socket;
  (void)mult_db_connections;
  (void)disable_batch_insert;
  (void)try_reconnect;
  (void)exit_on_fatal;
  (void)need_private;
  return {};
}

#endif /* HAVE_POSTGRESQL */
