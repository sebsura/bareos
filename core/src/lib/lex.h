/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2016-2024 Bareos GmbH & Co. KG

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
// Kern Sibbald, MM
/**
 * @file
 * Lexical scanning of configuration files, used by parsers.
 */

#ifndef BAREOS_LIB_LEX_H_
#define BAREOS_LIB_LEX_H_

#include <memory>
#include "include/bareos.h"

/* Lex get_char() return values */
#define L_EOF (-1)
#define L_EOL (-2)

/* Internal tokens */
#define BCT_NONE 100

/* Tokens returned by get_token() */
#define BCT_EOF 101
#define BCT_NUMBER 102
#define BCT_IPADDR 103
#define BCT_IDENTIFIER 104
#define BCT_UNQUOTED_STRING 105
#define BCT_QUOTED_STRING 106
#define BCT_BOB 108 /* begin block */
#define BCT_EOB 109 /* end of block */
#define BCT_EQUALS 110
#define BCT_COMMA 111
#define BCT_EOL 112
#define BCT_ERROR 200
#define BCT_UTF8_BOM 201  /* File starts with a UTF-8 BOM*/
#define BCT_UTF16_BOM 202 /* File starts with a UTF-16LE BOM*/

/**
 * The following will be returned only if
 * the appropriate expect flag has been set
 */
#define BCT_SKIP_EOL 113     /* scan through EOLs */
#define BCT_PINT16 114       /* 16 bit positive integer */
#define BCT_PINT32 115       /* 32 bit positive integer */
#define BCT_PINT32_RANGE 116 /* 32 bit positive integer range */
#define BCT_INT16 117        /* 16 bit integer */
#define BCT_INT32 118        /* 32 bit integer */
#define BCT_INT64 119        /* 64 bit integer */
#define BCT_NAME 120         /* name max 128 chars */
#define BCT_STRING 121       /* string */
#define BCT_PINT64_RANGE 122 /* positive integer range */
#define BCT_PINT64 123       /* positive integer range */

#define BCT_ALL 0 /* no expectations */

/* Lexical state */
enum lex_state
{
  lex_none,
  lex_comment,
  lex_number,
  lex_ip_addr,
  lex_identifier,
  lex_string,
  lex_quoted_string,
  lex_include_quoted_string,
  lex_include,
  lex_utf8_bom,    /* we are parsing out a utf8 byte order mark */
  lex_utf16_le_bom /* we are parsing out a utf-16 (little endian) byte order
                      mark */
};

/* Lex scan options */
#define LOPT_NO_IDENT 0x1  /* No Identifiers -- use string */
#define LOPT_STRING 0x2    /* Force scan for string */
#define LOPT_NO_EXTERN 0x4 /* Don't follow @ command */

class Bpipe; /* forward reference */

/* Lexical context */
typedef struct s_lex_context {
  struct s_lex_context* next; /* pointer to next lexical context */
  int options;                /* scan options */
  char* fname;                /* filename */
  FILE* fd;                   /* file descriptor */
  POOLMEM* line;              /* input line */
  POOLMEM* str;               /* string being scanned */
  int str_len;                /* length of string */
  int str_max_len;            /* maximum length of string */
  int line_no;                /* file line number */
  int col_no;                 /* char position on line */
  int begin_line_no;          /* line no of beginning of string */
  enum lex_state state;       /* lex_state variable */
  int ch;                     /* last char/L_VAL returned by get_char */
  int token;
  union {
    uint16_t pint16_val;
    uint32_t pint32_val;
    uint64_t pint64_val;
    int16_t int16_val;
    int32_t int32_val;
    int64_t int64_val;
  } u;
  union {
    uint16_t pint16_val;
    uint32_t pint32_val;
    uint64_t pint64_val;
  } u2;
  void (*ScanError)(const char* file,
                    int line,
                    struct s_lex_context* lc,
                    const char* msg,
                    ...);
  void (*scan_warning)(const char* file,
                       int line,
                       struct s_lex_context* lc,
                       const char* msg,
                       ...);
  int err_type; /* message level for ScanError (M_..) */
  int error_counter;
  void* caller_ctx; /* caller private data */
  Bpipe* bpipe;     /* set if we are piping */
} LEX;

typedef void(LEX_ERROR_HANDLER)(const char* file,
                                int line,
                                LEX* lc,
                                const char* msg,
                                ...);
typedef void(LEX_WARNING_HANDLER)(const char* file,
                                  int line,
                                  LEX* lc,
                                  const char* msg,
                                  ...);

// Lexical scanning errors in parsing conf files
#define scan_err0(lc, msg) lc->ScanError(__FILE__, __LINE__, lc, msg)
#define scan_err1(lc, msg, a1) lc->ScanError(__FILE__, __LINE__, lc, msg, a1)
#define scan_err2(lc, msg, a1, a2) \
  lc->ScanError(__FILE__, __LINE__, lc, msg, a1, a2)
#define scan_err3(lc, msg, a1, a2, a3) \
  lc->ScanError(__FILE__, __LINE__, lc, msg, a1, a2, a3)
#define scan_err4(lc, msg, a1, a2, a3, a4) \
  lc->ScanError(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4)
#define scan_err5(lc, msg, a1, a2, a3, a4, a5) \
  lc->ScanError(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4, a5)
#define scan_err6(lc, msg, a1, a2, a3, a4, a5, a6) \
  lc->ScanError(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4, a5, a6)

// Lexical scanning warnings in parsing conf files
#define scan_warn0(lc, msg) lc->scan_warning(__FILE__, __LINE__, lc, msg)
#define scan_warn1(lc, msg, a1) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1)
#define scan_warn2(lc, msg, a1, a2) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1, a2)
#define scan_warn3(lc, msg, a1, a2, a3) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1, a2, a3)
#define scan_warn4(lc, msg, a1, a2, a3, a4) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4)
#define scan_warn5(lc, msg, a1, a2, a3, a4, a5) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4, a5)
#define scan_warn6(lc, msg, a1, a2, a3, a4, a5, a6) \
  lc->scan_warning(__FILE__, __LINE__, lc, msg, a1, a2, a3, a4, a5, a6)

void ScanToEol(LEX* lc);
int ScanToNextNotEol(LEX* lc);

LEX* LexCloseFile(LEX* lf);
LEX* lex_open_file(LEX* lf,
                   const char* fname,
                   LEX_ERROR_HANDLER* ScanError,
                   LEX_WARNING_HANDLER* scan_warning);
int LexGetChar(LEX* lf);
void LexUngetChar(LEX* lf);
const char* lex_tok_to_str(int token);
int LexGetToken(LEX* lf, int expect);
void LexSetDefaultErrorHandler(LEX* lf);
void LexSetDefaultWarningHandler(LEX* lf);
void LexSetErrorHandlerErrorType(LEX* lf, int err_type);

struct lex_closer {
  void operator()(LEX* l) const
  {
    auto* ptr = l;

    while (ptr) { ptr = LexCloseFile(ptr); }
  }
};

using lex_ptr = std::unique_ptr<LEX, lex_closer>;

lex_ptr LexFile(const char* file,
                void* ctx,
                int err_type,
                LEX_ERROR_HANDLER* err,
                LEX_WARNING_HANDLER* warn);

namespace lex {
struct source {
  std::string path;
  std::string data;
};

struct source_point {
  size_t byte_offset;
  uint32_t line;
  uint32_t col;
};

struct source_state {
  source_point current_pos() const { return {byte_offset, line, col}; }

  // returns nullptr on EOF
  std::optional<char> read(const std::vector<source>& sources);
  void reset_to(size_t file_index, source_point p)
  {
    current_index = file_index;
    byte_offset = p.byte_offset;
    line = p.line;
    col = p.col;
    line_offset = 0;  // ??
  }
  size_t current_index{};
  size_t line_offset{};
  size_t byte_offset{};
  uint32_t line{}, col{};
};

struct options {
  bool no_identifiers{};
  bool force_string{};
  bool disable_includes{};
};

enum class lex_state
{
  None,
  Comment,
  Number,
  Ident,
  String,
  QuotedString,
  Include,
  IncludeQuoted,
  Utf8Bom,  /* we are parsing out a utf8 byte order mark */
  Utf16Bom, /* we are parsing out a utf-16 (little endian) byte order
               mark */
};


enum class token_type
{
  FileEnd,
  Number,
  IpAddr,
  Identifier,
  UnquotedString,
  QuotedString,
  OpenBlock,
  CloseBlock,
  Eq,
  Comma,
  LineEnd,
  Err,
  Utf8_BOM,
  Utf16_BOM,
};

enum class expected_type
{
  SkipEol,
  PosInt16,
  PosInt32,
  PosInt32Range,
  PosInt64,
  PosInt64Range,
  Int16,
  Int32,
  Int64,
  Name,
  String,
};

struct source_location {
  size_t source_index;
  source_point start, end;
};

struct token {
  token_type type;
  source_location loc;
};

struct lexer {
  token next_token();

  bool finished() const { return current_index == sources.size(); }

  std::vector<source> sources{};

  options opts;

  // index of current source we are working on
  size_t current_index{0};

  // state of the current source
  source_state source{};
  lex_state internal_state{};

  std::string buffer;
};

lexer open_files(const char* path);

std::string format_comment(LEX* lex,
                           source_location loc,
                           std::string_view comment);

};  // namespace lex


#endif  // BAREOS_LIB_LEX_H_
