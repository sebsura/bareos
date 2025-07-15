/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2018-2025 Bareos GmbH & Co. KG

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
#ifndef BAREOS_CATS_SQL_POOLING_H_
#define BAREOS_CATS_SQL_POOLING_H_

class BareosDb;

BareosDb* DbSqlGetNonPooledConnection(JobControlRecord* jcr,
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
                                      bool need_private = false);

#endif  // BAREOS_CATS_SQL_POOLING_H_
