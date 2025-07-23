/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2003-2011 Free Software Foundation Europe e.V.
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
/*
 * Dan Langille, December 2003
 * based upon work done by Kern Sibbald, March 2000
 * Major rewrite by Marco van Wieringen, January 2010 for catalog refactoring.
 */
/**
 * @file
 * BAREOS Catalog Database routines specific to PostgreSQL
 * These are PostgreSQL specific routines
 */

#include "include/bareos.h"
#include "lib/mem_pool.h"

#ifdef HAVE_POSTGRESQL

#  include "cats.h"
#  include "db_conn.h"
#  include "libpq-fe.h"
#  include "postgres_ext.h"     /* needed for NAMEDATALEN */
#  include "pg_config_manual.h" /* get NAMEDATALEN on version 8.3 or later */
#  include "postgresql.h"
#  include "lib/edit.h"
#  include "lib/berrno.h"
#  include "lib/dlist.h"

#  include <fmt/format.h>

/* -----------------------------------------------------------------------
 *
 *   PostgreSQL dependent defines and subroutines
 *
 * -----------------------------------------------------------------------
 */

namespace postgres {

struct result_deleter {
  void operator()(PGresult* result) const { PQclear(result); }
};

using result = std::unique_ptr<PGresult, result_deleter>;

struct connection_deleter {
  void operator()(PGconn* conn) const
  {
    // its unclear to me if we can call PQfinish on a nullptr
    if (conn) { PQfinish(conn); }
  }
};

using connection = std::unique_ptr<PGconn, connection_deleter>;

struct retries {
  int amount;
};

result do_query(PGconn* db_handle, const char* query, retries r = {10})
{
  for (int i = 0; i < r.amount; i++) {
    if (i > 1) { Bmicrosleep(5, 0); }
    result res{PQexec(db_handle, query)};
    if (res) {
      auto status = PQresultStatus(res.get());
      if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) { res = {}; }
      return res;
    }
  }
  return {};
}

struct query {
  query(PGresult* res)
      : result_{res}
      , num_fields_{static_cast<size_t>(PQnfields(res))}
      , num_rows_{static_cast<size_t>(PQntuples(res))}
  {
  }

  const char* fetch_value(int row, int field)
  {
    return PQgetvalue(result_, row, field);
  }

  std::size_t field_count() const { return num_fields_; }
  std::size_t row_count() const { return num_rows_; }

 private:
  PGresult* result_;
  std::size_t num_fields_{};
  std::size_t num_rows_{};
};

const char* strerror(PGconn* db_handle) { return PQerrorMessage(db_handle); }

result try_query(PGconn* db_handle, bool try_reconnection, const char* query)
{
  Dmsg1(500, "try_query starts with '%s'\n", query);

  auto res = do_query(db_handle, query);
  if (!res && try_reconnection) {
    PQreset(db_handle);
    if (PQstatus(db_handle) == CONNECTION_OK) {
      if (do_query(db_handle,
                   "SET datestyle TO 'ISO, YMD';"
                   "SET cursor_tuple_fraction=1;"
                   "SET standard_conforming_strings=on;"
                   "SET client_min_messages TO WARNING;",
                   retries{1})) {
        res = do_query(db_handle, query);
      }
    }
  }
  if (res) {
    Dmsg1(500, "try_query suceeded with query %s", query);
    Dmsg0(500, "We have a result\n");
  } else {
    Dmsg1(500, "try_query failed with query %s", query);
    Dmsg1(50, "Result status fatal: %s, %s\n", query, strerror(db_handle));
  }
  return res;
}
};  // namespace postgres

class BareosDbPostgresql : public db_conn {
 public:
  dlink<BareosDbPostgresql> link; /**< Queue control */
  db_command_result connect(JobControlRecord* jcr, connection_parameter params);

  virtual ~BareosDbPostgresql() = default;

 private:
  void SqlFieldSeek(int field) override { field_number_ = field; }
  int SqlNumFields(void) override { return num_fields_; }
  void CloseDatabase(JobControlRecord* jcr) override;
  void EscapeString(JobControlRecord* jcr,
                    std::string& buffer,
                    std::string_view input) override;
  void EscapeObject(JobControlRecord* jcr,
                    std::string& buffer,
                    gsl::span<char> input) override;
  void UnescapeObject(JobControlRecord* jcr,
                      const char* object,
                      std::size_t object_length,
                      POOLMEM*& out,
                      int32_t* new_length) override;
  void StartTransaction(JobControlRecord* jcr) override;
  void EndTransaction(JobControlRecord* jcr) override;
  db_command_result BigSqlQuery(const char* query,
                                DB_RESULT_HANDLER* ResultHandler,
                                void* ctx) override;

  db_command_result SqlQueryWithHandler(const char* query,
                                        DB_RESULT_HANDLER* ResultHandler,
                                        void* ctx) override;
  db_command_result SqlQueryWithoutHandler(const char* query,
                                           query_flags flags = {}) override;
  void SqlFreeResult(void) override;
  SQL_ROW SqlFetchRow(void) override;
  const char* sql_strerror(void) override;
  void SqlDataSeek(int row) override;
  int SqlAffectedRows(void) override;
  SQL_FIELD* SqlFetchField(void) override;
  bool SqlFieldIsNotNull(int field_type) override;
  bool SqlFieldIsNumeric(int field_type) override;
  db_command_result SqlBatchStartFileTable(JobControlRecord* jcr) override;
  db_command_result SqlBatchEndFileTable(JobControlRecord* jcr,
                                         const char* error) override;
  db_command_result SqlBatchInsertFileTable(JobControlRecord* jcr,
                                            AttributesDbRecord* ar) override;

  db_command_result CheckDatabaseEncoding(JobControlRecord* jcr);

  const char* GetType() const override { return "PostgreSQL"; }

  bool fields_fetched_
      = false;         /**< Marker, if field descriptions are already fetched */
  int num_fields_ = 0; /**< Number of fields returned by last query */
  int num_rows_ = 0;
  int rows_size_ = 0;               /**< Size of malloced rows */
  int fields_size_ = 0;             /**< Size of malloced fields */
  int row_number_ = 0;              /**< Row number from xx_data_seek */
  int field_number_ = 0;            /**< Field number from SqlFieldSeek */
  SQL_ROW rows_ = nullptr;          /**< Defined rows */
  SQL_FIELD* fields_ = nullptr;     /**< Defined fields */
  bool allow_transactions_ = false; /**< Transactions allowed ? */
  bool try_reconnect_ = false;
  bool transaction_ = false; /**< Transaction started ? */

  postgres::connection db_handle_{};
  // maybe this should be a postgres::query
  postgres::result result_{};

  PoolMem buf_{PM_FNAME}; /**< Buffer to manipulate queries */
  static const char*
      query_definitions[]; /**< table of predefined sql queries */
};

/* pull in the generated queries definitions */
#  include "postgresql_queries.inc"

#  if 0
BareosDbPostgresql::BareosDbPostgresql()
{
  /*** FIXUP ***/

  //   if (disable_batch_insert) {
  //     disabled_batch_insert_ = true;
  //     have_batch_insert_ = false;
  //   } else {
  //     disabled_batch_insert_ = false;
  // #  if defined(USE_BATCH_FILE_INSERT)
  //     have_batch_insert_ = PQisthreadsafe();
  // #  else
  //     have_batch_insert_ = false;
  // #  endif /* USE_BATCH_FILE_INSERT */
  //   }

  //   // Put the db in the list.
  //   if (db_list == NULL) { db_list = new dlist<BareosDbPostgresql>(); }
  //   db_list->append(this);
}
#  endif

// Check that the database correspond to the encoding we want
db_command_result BareosDbPostgresql::CheckDatabaseEncoding(
    JobControlRecord* jcr)
{
  auto db_encoding = postgres::try_query(db_handle_.get(), true,
                                         "SELECT getdatabaseencoding()");

  if (!db_encoding) {
    return db_command_result::Error(
        fmt::format("could not determine database encoding: Err={}",
                    postgres::strerror(db_handle_.get())));
  }

  postgres::query q{db_encoding.get()};

  if (q.row_count() != 1 || q.field_count() != 1) {
    return db_command_result::Error(fmt::format(
        "database encoding returned unexpected value: rows={} fields={}",
        q.row_count(), q.field_count()));
  }
  auto* encoding = q.fetch_value(0, 0);
  if (bstrcmp(encoding, "SQL_ASCII") != 0) {
    // Something is wrong with database encoding

    PoolMem warning;
    /*** FIXUP ***/
    warning.bsprintf(
        "Encoding error for database \"%s\". Wanted SQL_ASCII, got %s\n",
        "get_db_name()", encoding);
    Jmsg(jcr, M_WARNING, 0, "%s", warning.c_str());
    Dmsg1(50, "%s", warning.c_str());
  }

  /* If we are in SQL_ASCII, we can force the client_encoding to
  SQL_ASCII
   * too */
  auto client_encoding = postgres::try_query(
      db_handle_.get(), true, "SET client_encoding TO 'SQL_ASCII'");

  if (!client_encoding) {
    return db_command_result::Error(
        fmt::format("could not determine database encoding: Err={}",
                    postgres::strerror(db_handle_.get())));
  }

  return db_command_result::Ok();
}

void BareosDbPostgresql::CloseDatabase(JobControlRecord* jcr)
{
  (void)jcr;
  /*** FIXUP ***/
  delete this;
  // if (connected_) { EndTransaction(jcr); }
  // lock_mutex(db_list_mutex);
  // ref_count_--;
  // if (ref_count_ == 0) {
  //   if (connected_) { SqlFreeResult(); }
  //   db_list->remove(this);
  //   if (db_handle_) { PQfinish(db_handle_); }
  //   if (RwlIsInit(&lock_)) { RwlDestroy(&lock_); }
  //   FreePoolMemory(errmsg);
  //   FreePoolMemory(cmd);
  //   FreePoolMemory(cached_path);
  //   FreePoolMemory(fname);
  //   FreePoolMemory(path);
  //   FreePoolMemory(esc_name);
  //   FreePoolMemory(esc_path);
  //   FreePoolMemory(esc_obj);
  //   FreePoolMemory(buf_);
  //   if (db_driver_) { free(db_driver_); }
  //   if (db_name_) { free(db_name_); }
  //   if (db_user_) { free(db_user_); }
  //   if (db_password_) { free(db_password_); }
  //   if (db_address_) { free(db_address_); }
  //   if (db_socket_) { free(db_socket_); }
  //   delete this;
  //   if (db_list->size() == 0) {
  //     delete db_list;
  //     db_list = NULL;
  //   }
  // }
  // unlock_mutex(db_list_mutex);
}

/**
 * Escape strings so that PostgreSQL is happy
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
void BareosDbPostgresql::EscapeString(JobControlRecord* jcr,
                                      std::string& buffer,
                                      std::string_view input)
{
  int error = 0;
  buffer.resize(2 * input.size() + 1);

  auto bytes_written = PQescapeStringConn(db_handle_.get(), buffer.data(),
                                          input.data(), input.size(), &error);
  if (error) {
    Jmsg(jcr, M_FATAL, 0, T_("PQescapeStringConn returned non-zero.\n"));
    /* error on encoding, probably invalid multibyte encoding in the source
      string see PQescapeStringConn documentation for details. */
    Dmsg0(500, "PQescapeStringConn failed\n");
    buffer.clear();
  } else {
    buffer.resize(bytes_written);
  }
}

/**
 * Escape binary so that PostgreSQL is happy
 *
 */
void BareosDbPostgresql::EscapeObject(JobControlRecord* jcr,
                                      std::string& buffer,
                                      gsl::span<char> input)
{
  size_t new_len;
  unsigned char* obj = PQescapeByteaConn(
      db_handle_.get(), reinterpret_cast<const unsigned char*>(input.data()),
      input.size(), &new_len);
  if (!obj) {
    Jmsg(jcr, M_FATAL, 0, T_("PQescapeByteaConn returned NULL.\n"));
    buffer.clear();
    return;
  }

  buffer.assign(reinterpret_cast<char*>(obj));

  PQfreemem(obj);
}

/**
 * Unescape binary object so that PostgreSQL is happy
 *
 */
void BareosDbPostgresql::UnescapeObject(JobControlRecord* jcr,
                                        const char* from,
                                        std::size_t,
                                        POOLMEM*& dest,
                                        int32_t* dest_len)
{
  size_t new_len;
  unsigned char* obj;

  if (!dest || !dest_len) { return; }

  if (!from) {
    dest[0] = '\0';
    *dest_len = 0;
    return;
  }

  obj = PQunescapeBytea((unsigned const char*)from, &new_len);

  if (!obj) {
    Jmsg(jcr, M_FATAL, 0, T_("PQunescapeByteaConn returned NULL.\n"));
    return;
  }

  *dest_len = new_len;
  dest = CheckPoolMemorySize(dest, new_len + 1);
  if (dest) {
    memcpy(dest, obj, new_len);
    dest[new_len] = '\0';
  }

  PQfreemem(obj);

  Dmsg1(010, "obj size: %d\n", *dest_len);
}

/**
 * Start a transaction. This groups inserts and makes things
 * much more efficient. Usually started when inserting
 * file attributes.
 */
void BareosDbPostgresql::StartTransaction(JobControlRecord* jcr)
{
  (void)jcr;
  /*** FIXUP ***/
  // if (!jcr->attr) { jcr->attr = GetPoolMemory(PM_FNAME); }
  // if (!jcr->ar) {
  //   jcr->ar = (AttributesDbRecord*)malloc(sizeof(AttributesDbRecord));
  // }

  // /* This is turned off because transactions break
  //  * if multiple simultaneous jobs are run. */
  // if (!allow_transactions_) { return; }

  // // Allow only 25,000 changes per transaction
  // if (transaction_ && changes > 25000) { EndTransaction(jcr); }
  // if (!transaction_) {
  //   SqlQueryWithoutHandler("BEGIN"); /* begin transaction */
  //   Dmsg0(400, "Start PosgreSQL transaction\n");
  //   transaction_ = true;
  // }
}

void BareosDbPostgresql::EndTransaction(JobControlRecord* jcr)
{
  /*** FIXUP ***/
  (void)jcr;

  // if (jcr && jcr->cached_attribute) {
  //   Dmsg0(400, "Flush last cached attribute.\n");
  //   if (!CreateAttributesRecord(jcr, jcr->ar)) {
  //     Jmsg1(jcr, M_FATAL, 0, T_("Attribute create error. %s"), strerror());
  //   }
  //   jcr->cached_attribute = false;
  // }

  // if (!allow_transactions_) { return; }


  // if (transaction_) {
  //   SqlQueryWithoutHandler("COMMIT"); /* end transaction */
  //   transaction_ = false;
  //   Dmsg1(400, "End PostgreSQL transaction changes=%d\n", changes);
  // }
  // changes = 0;
}

/**
 * Submit a general SQL command (cmd), and for each row returned,
 * the ResultHandler is called with the ctx.
 */
db_command_result BareosDbPostgresql::BigSqlQuery(
    const char* query,
    DB_RESULT_HANDLER* ResultHandler,
    void* ctx)
{
  SQL_ROW row;
  Dmsg1(500, "BigSqlQuery starts with '%s'\n", query);

  /* This code handles only SELECT queries */
  if (!bstrncasecmp(query, "SELECT", 6)) {
    return SqlQueryWithHandler(query, ResultHandler, ctx);
  }

  if (!ResultHandler) { /* no need of big_query without handler */
    return db_command_result::Error("no result handler specified");
  }


  bool in_transaction = transaction_;
  if (!in_transaction) { /* CURSOR needs transaction */
    SqlQueryWithoutHandler("BEGIN");
  }

  std::string errmsg;
  PoolMem buf;
  Mmsg(buf, "DECLARE _bar_cursor CURSOR FOR %s", query);

  if (auto result = SqlQueryWithoutHandler(buf.c_str()); result.error()) {
    // Mmsg(errmsg, T_("Query failed: %s: ERR=%s\n"), buf, sql_strerror());
    errmsg = fmt::format("Query failed: {}: ERR={}\n", buf.c_str(),
                         result.error());
    Dmsg0(50, "SqlQueryWithoutHandler(%s) failed: %s\n", buf.c_str(),
          result.error());

    goto bail_out;
  }

  do {
    if (auto result = SqlQueryWithoutHandler("FETCH 100 FROM _bar_cursor");
        result.error()) {
      errmsg = fmt::format("Fetch failed: ERR={}\n", result.error());
      Dmsg0(50, "SqlQueryWithoutHandler(Fetch) failed: %s\n", result.error());
      goto bail_out;
    }
    while ((row = SqlFetchRow()) != NULL) {
      Dmsg1(500, "Fetching %d rows\n", num_rows_);
      if (ResultHandler(ctx, num_fields_, row)) break;
    }
    result_.reset();
  } while (num_rows_ > 0);

  SqlQueryWithoutHandler("CLOSE _bar_cursor");

  Dmsg0(500, "BigSqlQuery finished\n");
  SqlFreeResult();

bail_out:
  if (!in_transaction) {
    SqlQueryWithoutHandler("COMMIT"); /* end transaction */
  }

  if (!errmsg.empty()) { return db_command_result::Error(std::move(errmsg)); }
  return db_command_result::Ok();
}

/**
 * Submit a general SQL command (cmd), and for each row returned,
 * the ResultHandler is called with the ctx.
 */
db_command_result BareosDbPostgresql::SqlQueryWithHandler(
    const char* query,
    DB_RESULT_HANDLER* ResultHandler,
    void* ctx)
{
  SQL_ROW row;

  Dmsg1(500, "SqlQueryWithHandler starts with '%s'\n", query);

  if (auto result = SqlQueryWithoutHandler(query); result.error()) {
    Dmsg0(500, "SqlQueryWithHandler failed: %s\n", result.error());
    return result;
  }

  Dmsg0(500, "SqlQueryWithHandler succeeded. checking handler\n");

  if (ResultHandler != NULL) {
    Dmsg0(500, "SqlQueryWithHandler invoking handler\n");
    while ((row = SqlFetchRow()) != NULL) {
      Dmsg0(500, "SqlQueryWithHandler SqlFetchRow worked\n");
      if (ResultHandler(ctx, num_fields_, row)) break;
    }
    SqlFreeResult();
  }

  Dmsg0(500, "SqlQueryWithHandler finished\n");

  return db_command_result::Ok();
}

/**
 * Note, if this routine returns false (failure), BAREOS expects
 * that no result has been stored.
 * This is where QueryDb comes with Postgresql.
 *
 * Returns:  true  on success
 *           false on failure
 */
db_command_result BareosDbPostgresql::SqlQueryWithoutHandler(const char* query,
                                                             query_flags flags)
{
  auto result = postgres::try_query(db_handle_.get(),
                                    try_reconnect_ && !transaction_, query);

  if (result) {
    if (!flags.test(query_flag::DiscardResult)) {
      result_ = std::move(result);
      postgres::query q{result_.get()};
      field_number_ = -1;
      fields_fetched_ = false;
      num_fields_ = q.field_count();
      Dmsg1(500, "We have %d fields\n", num_fields_);
      num_rows_ = q.row_count();
      Dmsg1(500, "We have %d rows\n", num_rows_);
      row_number_ = 0; /* we can start to fetch something */
    }
    return db_command_result::Ok();
  } else {
    return db_command_result::Error(sql_strerror());
  }
}

void BareosDbPostgresql::SqlFreeResult(void)
{
  if (result_) { result_.reset(); }
  if (rows_) {
    free(rows_);
    rows_ = NULL;
  }
  if (fields_) {
    free(fields_);
    fields_ = NULL;
  }
  fields_fetched_ = false;
  num_rows_ = num_fields_ = 0;
}

SQL_ROW BareosDbPostgresql::SqlFetchRow(void)
{
  int j;
  SQL_ROW row = NULL; /* by default, return NULL */

  Dmsg0(500, "SqlFetchRow start\n");

  if (num_fields_ == 0) { /* No field, no row */
    Dmsg0(500, "SqlFetchRow finishes returning NULL, no fields\n");
    return NULL;
  }

  if (!rows_ || rows_size_ < num_fields_) {
    if (rows_) {
      Dmsg0(500, "SqlFetchRow freeing space\n");
      free(rows_);
    }
    Dmsg1(500, "we need space for %" PRIuz " bytes\n",
          sizeof(char*) * num_fields_);
    rows_ = (SQL_ROW)malloc(sizeof(char*) * num_fields_);
    rows_size_ = num_fields_;

    // Now reset the row_number now that we have the space allocated
    row_number_ = 0;
  }

  // If still within the result set
  if (row_number_ >= 0 && row_number_ < num_rows_) {
    Dmsg2(500, "SqlFetchRow row number '%d' is acceptable (0..%d)\n",
          row_number_, num_rows_);
    for (j = 0; j < num_fields_; j++) {
      rows_[j] = PQgetvalue(result_.get(), row_number_, j);
      Dmsg2(500, "SqlFetchRow field '%d' has value '%s'\n", j, rows_[j]);
    }
    // Increment the row number for the next call
    row_number_++;
    row = rows_;
  } else {
    Dmsg2(500, "SqlFetchRow row number '%d' is NOT acceptable (0..%d)\n",
          row_number_, num_rows_);
  }

  Dmsg1(500, "SqlFetchRow finishes returning %p\n", row);

  return row;
}

const char* BareosDbPostgresql::sql_strerror(void)
{
  return PQerrorMessage(db_handle_.get());
}

void BareosDbPostgresql::SqlDataSeek(int row)
{
  // Set the row number to be returned on the next call to sql_fetch_row
  row_number_ = row;
}

int BareosDbPostgresql::SqlAffectedRows(void)
{
  return (unsigned)str_to_int32(PQcmdTuples(result_.get()));
}

static void ComputeFields(int num_fields,
                          int num_rows,
                          SQL_FIELD fields[/* num_fields */],
                          PGresult* result)
{
  // For a given column, find the max length.
  for (int fidx = 0; fidx < num_fields; ++fidx) { fields[fidx].max_length = 0; }

  for (int ridx = 0; ridx < num_rows; ++ridx) {
    for (int fidx = 0; fidx < num_fields; ++fidx) {
      int length = PQgetisnull(result, ridx, fidx)
                       ? 4 /* "NULL" */
                       : cstrlen(PQgetvalue(result, ridx, fidx));

      if (fields[fidx].max_length < length) {
        fields[fidx].max_length = length;
      }
    }
  }

  for (int fidx = 0; fidx < num_fields; ++fidx) {
    Dmsg1(500, "filling field %d\n", fidx);
    fields[fidx].name = PQfname(result, fidx);
    fields[fidx].type = PQftype(result, fidx);
    fields[fidx].flags = 0;
    Dmsg4(500,
          "ComputeFields finds field '%s' has length='%d' type='%d' and "
          "IsNull=%d\n",
          fields[fidx].name, fields[fidx].max_length, fields[fidx].type,
          fields[fidx].flags);
  }
}

SQL_FIELD* BareosDbPostgresql::SqlFetchField(void)
{
  Dmsg0(500, "SqlFetchField starts\n");

  if (field_number_ >= num_fields_) {
    Dmsg1(100, "requesting field number %d, but only %d fields given\n",
          field_number_, num_fields_);
    return nullptr;
  }

  if (!fields_fetched_) {
    if (!fields_ || fields_size_ < num_fields_) {
      fields_fetched_ = false;
      if (fields_) {
        free(fields_);
        fields_ = NULL;
      }
      Dmsg1(500, "allocating space for %d fields\n", num_fields_);
      fields_ = (SQL_FIELD*)malloc(sizeof(SQL_FIELD) * num_fields_);
      fields_size_ = num_fields_;
    }

    ComputeFields(num_fields_, num_rows_, fields_, result_.get());

    fields_fetched_ = true;
  }

  // Increment field number for the next time around
  return &fields_[field_number_++];
}

bool BareosDbPostgresql::SqlFieldIsNotNull(int field_type)
{
  switch (field_type) {
    case 1:
      return true;
    default:
      return false;
  }
}

bool BareosDbPostgresql::SqlFieldIsNumeric(int field_type)
{
  // TEMP: the following is taken from select OID, typname from pg_type;
  switch (field_type) {
    case 20:   /* int8 (8-byte) */
    case 21:   /* int2 (2-byte) */
    case 23:   /* int4 (4-byte) */
    case 700:  /* float4 (single precision) */
    case 701:  /* float8 (double precision) */
    case 1700: /* numeric + decimal */
      return true;
    default:
      return false;
  }
}

db_command_result BareosDbPostgresql::SqlBatchStartFileTable(JobControlRecord*)
{
  /*** FIXUP ***/
  return db_command_result::Error("not implemented");
  //   AssertOwnership();
  //   const char* query = "COPY batch FROM STDIN";

  //   Dmsg0(500, "SqlBatchStartFileTable started\n");

  //   if (!SqlQueryWithoutHandler("CREATE TEMPORARY TABLE batch ("
  //                               "FileIndex int,"
  //                               "JobId int,"
  //                               "Path varchar,"
  //                               "Name varchar,"
  //                               "LStat varchar,"
  //                               "Md5 varchar,"
  //                               "DeltaSeq smallint,"
  //                               "Fhinfo NUMERIC(20),"
  //                               "Fhnode NUMERIC(20))")) {
  //     Dmsg0(500, "SqlBatchStartFileTable failed\n");
  //     return false;
  //   }

  //   // We are starting a new query.  reset everything.
  //   num_rows_ = -1;
  //   row_number_ = -1;
  //   field_number_ = -1;

  //   SqlFreeResult();

  //   postgres::result res;
  //   for (int i = 0; i < 10; i++) {
  //     res.reset(PQexec(db_handle_, query));
  //     if (res) { break; }
  //     Bmicrosleep(5, 0);
  //   }
  //   if (!res) {
  //     Dmsg1(50, "Query failed: %s\n", query);
  //     goto bail_out;
  //   }

  //   {
  //     auto status = PQresultStatus(res.get());
  //     if (status == PGRES_COPY_IN) {
  //       num_fields_ = (int)PQnfields(res.get());
  //       num_rows_ = 0;
  //     } else {
  //       Dmsg1(50, "Result status failed: %s\n", query);
  //       goto bail_out;
  //     }
  //   }

  //   Dmsg0(500, "SqlBatchStartFileTable finishing\n");

  //   result_ = res.release();

  //   return true;

  // bail_out:
  //   Mmsg1(errmsg, T_("error starting batch mode: %s"),
  //         PQerrorMessage(db_handle_));
  //   return false;
}

// Set error to something to abort operation
db_command_result BareosDbPostgresql::SqlBatchEndFileTable(JobControlRecord*,
                                                           const char* error)
{
  /*** FIXUP ***/
  (void)error;

  return db_command_result::Error("not implemented");
  // AssertOwnership();
  // int res;
  // int count = 30;
  // PGresult* pg_result;

  // Dmsg0(500, "SqlBatchEndFileTable started\n");

  // do {
  //   res = PQputCopyEnd(db_handle_, error);
  // } while (res == 0 && --count > 0);

  // if (res == 1) { Dmsg0(500, "ok\n"); }

  // if (res <= 0) {
  //   Dmsg0(500, "we failed\n");
  //   Mmsg1(errmsg, T_("error ending batch mode: %s"),
  //         PQerrorMessage(db_handle_));
  //   Dmsg1(500, "failure %s\n", errmsg);
  // }

  // pg_result = PQgetResult(db_handle_);
  // if (PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
  //   Mmsg1(errmsg, T_("error ending batch mode: %s"),
  //         PQerrorMessage(db_handle_));
  // }

  // PQclear(pg_result);

  // Dmsg0(500, "SqlBatchEndFileTable finishing\n");

  // return true;
}

/**
 * Escape strings so that PostgreSQL is happy on COPY
 *
 *   NOTE! len is the length of the old string. Your new
 *         string must be long enough (max 2*old+1) to hold
 *         the escaped output.
 */
// static char* pgsql_copy_escape(char* dest, const char* src, size_t len)
// {
//   char c = '\0';

//   while (len > 0 && *src) {
//     switch (*src) {
//       case '\b':
//         c = 'b';
//         break;
//       case '\f':
//         c = 'f';
//         break;
//       case '\n':
//         c = 'n';
//         break;
//       case '\\':
//         c = '\\';
//         break;
//       case '\t':
//         c = 't';
//         break;
//       case '\r':
//         c = 'r';
//         break;
//       case '\v':
//         c = 'v';
//         break;
//       case '\'':
//         c = '\'';
//         break;
//       default:
//         c = '\0';
//         break;
//     }

//     if (c) {
//       *dest = '\\';
//       dest++;
//       *dest = c;
//     } else {
//       *dest = *src;
//     }

//     len--;
//     src++;
//     dest++;
//   }

//   *dest = '\0';
//   return dest;
// }

db_command_result BareosDbPostgresql::SqlBatchInsertFileTable(
    JobControlRecord*,
    AttributesDbRecord* ar)
{
  /*** FIXUP ***/
  (void)ar;

  return db_command_result::Error("not implemented");

  // int res;
  // int count = 30;
  // size_t len;
  // const char* digest;
  // char ed1[50], ed2[50], ed3[50];

  // AssertOwnership();
  // esc_name = CheckPoolMemorySize(esc_name, fnl * 2 + 1);
  // pgsql_copy_escape(esc_name, fname, fnl);

  // esc_path = CheckPoolMemorySize(esc_path, pnl * 2 + 1);
  // pgsql_copy_escape(esc_path, path, pnl);

  // if (ar->Digest == NULL || ar->Digest[0] == 0) {
  //   digest = "0";
  // } else {
  //   digest = ar->Digest;
  // }

  // len = Mmsg(cmd, "%u\t%s\t%s\t%s\t%s\t%s\t%u\t%s\t%s\n", ar->FileIndex,
  //            edit_int64(ar->JobId, ed1), esc_path, esc_name, ar->attr,
  //            digest, ar->DeltaSeq, edit_uint64(ar->Fhinfo, ed2),
  //            edit_uint64(ar->Fhnode, ed3));

  // do {
  //   res = PQputCopyData(db_handle_, cmd, len);
  // } while (res == 0 && --count > 0);

  // if (res == 1) {
  //   Dmsg0(500, "ok\n");
  //   changes++;
  // }

  // if (res <= 0) {
  //   Dmsg0(500, "we failed\n");
  //   Mmsg1(errmsg, T_("error copying in batch mode: %s"),
  //         PQerrorMessage(db_handle_));
  //   Dmsg1(500, "failure %s\n", errmsg);
  // }

  // Dmsg0(500, "SqlBatchInsertFileTable finishing\n");

  // return true;
}


db_command_result BareosDbPostgresql::connect(JobControlRecord* jcr,
                                              connection_parameter params)
{
  char buffer[12];

  std::size_t option_count = 0;
  static constexpr std::size_t max_option_count = 7;
  const char* keys[max_option_count];
  const char* values[max_option_count];

  if (!params.db_address.empty()) {
    keys[option_count] = "host";
    values[option_count] = params.db_address.c_str();
    option_count += 1;
  }

  if (params.db_port) {
    snprintf(buffer, sizeof(buffer), "%u", params.db_port);
    keys[option_count] = "port";
    values[option_count] = buffer;
    option_count += 1;
  }

  if (!params.db_name.empty()) {
    keys[option_count] = "dbname";
    values[option_count] = params.db_name.c_str();
    option_count += 1;
  }

  if (!params.db_user.empty()) {
    keys[option_count] = "user";
    values[option_count] = params.db_user.c_str();
    option_count += 1;
  }

  if (!params.db_password.empty()) {
    keys[option_count] = "password";
    values[option_count] = params.db_password.c_str();
    option_count += 1;
  }

  keys[option_count] = "sslmode";
  values[option_count] = "disable";
  option_count += 1;

  keys[option_count] = nullptr;
  values[option_count] = nullptr;
  option_count += 1;

  ASSERT(option_count <= max_option_count);

  std::string err_msg;

  // If connection fails, try at 5 sec intervals for 30 seconds.
  for (int retry = 0; retry < 6; retry++) {
    db_handle_.reset(PQconnectdbParams(keys, values, true));

    // If connecting does not succeed, try again in case it was a timing
    // problem
    if (PQstatus(db_handle_.get()) == CONNECTION_OK) { break; }

    const char* err = PQerrorMessage(db_handle_.get());
    if (!err) { err = "unknown reason"; }
    Dmsg1(50, "Could not connect to db: Err=%s\n", err);

    err_msg.assign(err);

    // free memory if not successful
    db_handle_.reset();

    Bmicrosleep(5, 0);
  }

  Dmsg0(50, "pg_real_connect %s\n", db_handle_ ? "ok" : "failed");
  Dmsg3(50, "db_user=%s db_name=%s db_password=%s\n", params.db_user.c_str(),
        params.db_name.c_str(),
        (params.db_password.empty()) ? "(NULL)" : params.db_password.c_str());

  if (!db_handle_) {
    return db_command_result::Error(fmt::format(
        "Unable to connect to PostgreSQL server. Database={} User={}\n"
        "Possible causes: SQL server not running; password incorrect; "
        "server requires ssl; max_connections exceeded.\n({})\n",
        params.db_name.c_str(), params.db_user.c_str(), err_msg.c_str()));
  }

  SqlQueryWithoutHandler("SET datestyle TO 'ISO, YMD'");
  SqlQueryWithoutHandler("SET cursor_tuple_fraction=1");
  SqlQueryWithoutHandler("SET client_min_messages TO WARNING");

  /* Tell PostgreSQL we are using standard conforming strings
   * and avoid warnings such as:
   *  WARNING:  nonstandard use of \\ in a string literal */
  SqlQueryWithoutHandler("SET standard_conforming_strings=on");

  // Check that encoding is SQL_ASCII
  auto result = CheckDatabaseEncoding(jcr);
  if (result.error()) { return result; }

  allow_transactions_ = params.mult_db_connections;
  try_reconnect_ = params.try_reconnect;

  return db_command_result::Ok();
}

namespace postgresql {
db_conn* connect(JobControlRecord* jcr, const connection_parameter& params)
{
  auto connection = new BareosDbPostgresql{};

  if (auto result = connection->connect(jcr, params); result.error()) {
    delete connection;

    return nullptr;
  }

  return connection;
}
}  // namespace postgresql

#endif /* HAVE_POSTGRESQL */
