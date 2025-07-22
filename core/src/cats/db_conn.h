/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
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

#ifndef BAREOS_CATS_DB_CONN_H_
#define BAREOS_CATS_DB_CONN_H_

#include "cats.h"
#include <gsl/span>
#include <string_view>

struct db_conn {
  /* Virtual low level methods */
  virtual const char* GetType() const = 0;
  virtual void EscapeString(JobControlRecord* jcr,
                            std::string& buffer,
                            std::string_view input)
      = 0;
  virtual void EscapeObject(JobControlRecord* jcr,
                            std::string& buffer,
                            gsl::span<char> input)
      = 0;
  virtual void UnescapeObject(JobControlRecord* jcr,
                              const char* object,
                              std::size_t object_length,
                              POOLMEM*& out,
                              int32_t* new_length)
      = 0;

  /* Pure virtual low level methods */
  // returns an error string on error
  virtual void CloseDatabase(JobControlRecord* jcr) = 0;
  virtual void StartTransaction(JobControlRecord* jcr) = 0;
  virtual void EndTransaction(JobControlRecord* jcr) = 0;

  virtual void SqlFieldSeek(int field) = 0;
  virtual int SqlNumFields(void) = 0;
  virtual void SqlFreeResult(void) = 0;
  virtual SQL_ROW SqlFetchRow(void) = 0;


  virtual bool SqlQueryWithoutHandler(const char* query, query_flags flags = {})
      = 0;
  virtual bool SqlQueryWithHandler(const char* query,
                                   DB_RESULT_HANDLER* ResultHandler,
                                   void* ctx)
      = 0;
  virtual bool BigSqlQuery(const char* query,
                           DB_RESULT_HANDLER* ResultHandler,
                           void* ctx)
      = 0;
  virtual const char* sql_strerror(void) = 0;
  virtual void SqlDataSeek(int row) = 0;
  virtual int SqlAffectedRows(void) = 0;
  virtual uint64_t SqlInsertAutokeyRecord(const char* query,
                                          const char* table_name)
      = 0;
  virtual SQL_FIELD* SqlFetchField(void) = 0;
  virtual bool SqlFieldIsNotNull(int field_type) = 0;
  virtual bool SqlFieldIsNumeric(int field_type) = 0;
  virtual bool SqlBatchStartFileTable(JobControlRecord* jcr) = 0;
  virtual bool SqlBatchEndFileTable(JobControlRecord* jcr, const char* error)
      = 0;
  virtual bool SqlBatchInsertFileTable(JobControlRecord* jcr,
                                       AttributesDbRecord* ar)
      = 0;
};

#endif  // BAREOS_CATS_DB_CONN_H_
