/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
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
/*
 * Lexical scanner for BAREOS configuration file
 *
 * Kern Sibbald, 2000
 */

#include "include/bareos.h"
#include "lex.h"
#include "lib/edit.h"
#include "lib/parse_conf.h"
#include "lib/berrno.h"
#include "lib/bpipe.h"
#include <glob.h>
#include "lib/parse_err.h"

#include <charconv>
#include <sstream>

extern int debug_level;

/* Debug level for this source file */
static const int debuglevel = 5000;

/*
 * Scan to "logical" end of line. I.e. end of line,
 *   or semicolon, but stop on BCT_EOB (same as end of
 *   line except it is not eaten).
 */
void ScanToEol(LEX* lc)
{
  int token;

  Dmsg0(debuglevel, "start scan to eof\n");
  while ((token = LexGetToken(lc, BCT_ALL)) != BCT_EOL) {
    if (token == BCT_EOB) {
      LexUngetChar(lc);
      return;
    }
  }
}

// Get next token, but skip EOL
int ScanToNextNotEol(LEX* lc)
{
  int token;

  do {
    token = LexGetToken(lc, BCT_ALL);
  } while (token == BCT_EOL);

  return token;
}

// Format a scanner error message
static void s_err(const char* file, int line, LEX* lc, const char* msg, ...)
{
  va_list ap;
  int len, maxlen;
  PoolMem buf(PM_NAME), more(PM_NAME);

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

  if (lc->err_type == 0) { /* M_ERROR_TERM by default */
    lc->err_type = M_ERROR_TERM;
  }

  if (lc->line_no > lc->begin_line_no) {
    Mmsg(more, T_("Problem probably begins at line %d.\n"), lc->begin_line_no);
  } else {
    PmStrcpy(more, "");
  }

  if (lc->line_no > 0) {
    e_msg(file, line, lc->err_type, 0,
          T_("Config error: %s\n"
             "            : line %d, col %d of file %s\n%s\n%s"),
          buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line,
          more.c_str());
  } else {
    e_msg(file, line, lc->err_type, 0, T_("Config error: %s\n"), buf.c_str());
  }

  lc->error_counter++;
}

// Format a scanner warning message
static void s_warn(const char* file, int line, LEX* lc, const char* msg, ...)
{
  va_list ap;
  int len, maxlen;
  PoolMem buf(PM_NAME), more(PM_NAME);

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

  if (lc->line_no > lc->begin_line_no) {
    Mmsg(more, T_("Problem probably begins at line %d.\n"), lc->begin_line_no);
  } else {
    PmStrcpy(more, "");
  }

  if (lc->line_no > 0) {
    p_msg(file, line, 0,
          T_("Config warning: %s\n"
             "            : line %d, col %d of file %s\n%s\n%s"),
          buf.c_str(), lc->line_no, lc->col_no, lc->fname, lc->line,
          more.c_str());
  } else {
    p_msg(file, line, 0, T_("Config warning: %s\n"), buf.c_str());
  }
}

void LexSetDefaultErrorHandler(LEX* lf) { lf->ScanError = s_err; }

void LexSetDefaultWarningHandler(LEX* lf) { lf->scan_warning = s_warn; }

// Set err_type used in error_handler
void LexSetErrorHandlerErrorType(LEX* lf, int err_type)
{
  LEX* lex = lf;
  while (lex) {
    lex->err_type = err_type;
    lex = lex->next;
  }
}

/*
 * Free the current file, and retrieve the contents of the previous packet if
 * any.
 */
LEX* LexCloseFile(LEX* lf)
{
  LEX* of;

  if (lf == NULL) { Emsg0(M_ABORT, 0, T_("Close of NULL file\n")); }
  Dmsg1(debuglevel, "Close lex file: %s\n", lf->fname);

  of = lf->next;
  if (lf->bpipe) {
    CloseBpipe(lf->bpipe);
    lf->bpipe = NULL;
  } else {
    fclose(lf->fd);
  }
  Dmsg1(debuglevel, "Close cfg file %s\n", lf->fname);
  free(lf->fname);
  FreeMemory(lf->line);
  FreeMemory(lf->str);
  lf->line = NULL;
  if (of) {
    of->options = lf->options;              /* preserve options */
    of->error_counter += lf->error_counter; /* summarize the errors */
    memcpy(lf, of, sizeof(LEX));
    Dmsg1(debuglevel, "Restart scan of cfg file %s\n", of->fname);
  } else {
    of = lf;
    lf = NULL;
  }
  free(of);
  return lf;
}

// Add lex structure for an included config file.
LEX* lex_add(LEX* lf,
             const char* filename,
             FILE* fd,
             Bpipe* bpipe,
             LEX_ERROR_HANDLER* ScanError,
             LEX_WARNING_HANDLER* scan_warning)
{
  LEX* nf;

  Dmsg1(100, "open config file: %s\n", filename);
  nf = (LEX*)malloc(sizeof(LEX));
  if (lf) {
    *nf = *lf;
    *lf = {};
    lf->next = nf;             /* if have lf, push it behind new one */
    lf->options = nf->options; /* preserve user options */
    /* preserve err_type to prevent bareos exiting on 'reload'
     * if config is invalid. */
    lf->err_type = nf->err_type;
  } else {
    lf = nf; /* start new packet */
    memset(lf, 0, sizeof(LEX));
    LexSetErrorHandlerErrorType(lf, M_ERROR_TERM);
  }

  if (ScanError) {
    lf->ScanError = ScanError;
  } else {
    LexSetDefaultErrorHandler(lf);
  }

  if (scan_warning) {
    lf->scan_warning = scan_warning;
  } else {
    LexSetDefaultWarningHandler(lf);
  }

  lf->fd = fd;
  lf->bpipe = bpipe;
  lf->fname = strdup(filename ? filename : "");
  lf->line = GetMemory(1024);
  lf->str = GetMemory(256);
  lf->str_max_len = SizeofPoolMemory(lf->str);
  lf->state = lex_none;
  lf->ch = L_EOL;

  return lf;
}

#ifdef HAVE_GLOB
static inline bool IsWildcardString(const char* string)
{
  return (strchr(string, '*') || strchr(string, '?'));
}
#endif

/*
 * Open a new configuration file. We push the
 * state of the current file (lf) so that we
 * can do includes.  This is a bit of a hammer.
 * Instead of passing back the pointer to the
 * new packet, I simply replace the contents
 * of the caller's packet with the new packet,
 * and link the contents of the old packet into
 * the next field.
 */
LEX* lex_open_file(LEX* lf,
                   const char* filename,
                   LEX_ERROR_HANDLER* ScanError,
                   LEX_WARNING_HANDLER* scan_warning)
{
  FILE* fd;
  Bpipe* bpipe = NULL;
  char* bpipe_filename = NULL;

  if (filename[0] == '|') {
    bpipe_filename = strdup(filename);
    if ((bpipe = OpenBpipe(bpipe_filename + 1, 0, "rb")) == NULL) {
      free(bpipe_filename);
      return NULL;
    }
    free(bpipe_filename);
    fd = bpipe->rfd;
    return lex_add(lf, filename, fd, bpipe, ScanError, scan_warning);
  } else {
#ifdef HAVE_GLOB
    int globrc;
    glob_t fileglob;
    char* filename_expanded = NULL;

    /* Flag GLOB_NOMAGIC is a GNU extension, therefore manually check if string
     * is a wildcard string. */

    // Clear fileglob at least required for mingw version of glob()
    memset(&fileglob, 0, sizeof(fileglob));
    globrc = glob(filename, 0, NULL, &fileglob);

    if ((globrc == GLOB_NOMATCH) && (IsWildcardString(filename))) {
      /* fname is a wildcard string, but no matching files have been found.
       * Ignore this include statement and continue. */
      return lf;
    } else if (globrc != 0) {
      // glob() error has occurred. Giving up.
      return NULL;
    }

    Dmsg2(100, "glob %s: %i files\n", filename, fileglob.gl_pathc);
    for (size_t i = 0; i < fileglob.gl_pathc; i++) {
      filename_expanded = fileglob.gl_pathv[i];
      if ((fd = fopen(filename_expanded, "rb")) == NULL) {
        globfree(&fileglob);
        return NULL;
      }
      lf = lex_add(lf, filename_expanded, fd, bpipe, ScanError, scan_warning);
    }
    globfree(&fileglob);
#else
    if ((fd = fopen(filename, "rb")) == NULL) { return NULL; }
    lf = lex_add(lf, filename, fd, bpipe, ScanError, scan_warning);
#endif
    return lf;
  }
}

/*
 * Get the next character from the input.
 *  Returns the character or
 *    L_EOF if end of file
 *    L_EOL if end of line
 */
int LexGetChar(LEX* lf)
{
  if (lf->ch == L_EOF) {
    Emsg0(M_CONFIG_ERROR, 0,
          T_("get_char: called after EOF."
             " You may have a open double quote without the closing double "
             "quote.\n"));
  }

  if (lf->ch == L_EOL) {
    // See if we are really reading a file otherwise we have reached EndOfFile.
    if (!lf->fd || bfgets(lf->line, lf->fd) == NULL) {
      lf->ch = L_EOF;
      if (lf->next) {
        if (lf->fd) { LexCloseFile(lf); }
      }
      return lf->ch;
    }
    lf->line_no++;
    lf->col_no = 0;
    Dmsg2(1000, "fget line=%d %s", lf->line_no, lf->line);
  }

  lf->ch = (uint8_t)lf->line[lf->col_no];
  if (lf->ch == 0) {
    lf->ch = L_EOL;
  } else if (lf->ch == '\n') {
    lf->ch = L_EOL;
    lf->col_no++;
  } else {
    lf->col_no++;
  }
  Dmsg2(debuglevel, "LexGetChar: %c %d\n", lf->ch, lf->ch);

  return lf->ch;
}

void LexUngetChar(LEX* lf)
{
  if (lf->ch == L_EOL) {
    lf->ch = 0; /* End of line, force read of next one */
  } else {
    lf->col_no--; /* Backup to re-read char */
  }
}

// Add a character to the current string
static void add_str(LEX* lf, int ch)
{
  /* The default config string is sized to 256 bytes.
   * If we need longer config strings its increased with 256 bytes each time. */
  if ((lf->str_len + 3) >= lf->str_max_len) {
    lf->str = CheckPoolMemorySize(lf->str, lf->str_max_len + 256);
    lf->str_max_len = SizeofPoolMemory(lf->str);
  }

  lf->str[lf->str_len++] = ch;
  lf->str[lf->str_len] = 0;
}

// Begin the string
static void BeginStr(LEX* lf, int ch)
{
  lf->str_len = 0;
  lf->str[0] = 0;
  if (ch != 0) { add_str(lf, ch); }
  lf->begin_line_no = lf->line_no; /* save start string line no */
}

static const char* lex_state_to_str(int state)
{
  switch (state) {
    case lex_none:
      return T_("none");
    case lex_comment:
      return T_("comment");
    case lex_number:
      return T_("number");
    case lex_ip_addr:
      return T_("ip_addr");
    case lex_identifier:
      return T_("identifier");
    case lex_string:
      return T_("string");
    case lex_quoted_string:
      return T_("quoted_string");
    case lex_include:
      return T_("include");
    case lex_include_quoted_string:
      return T_("include_quoted_string");
    case lex_utf8_bom:
      return T_("UTF-8 Byte Order Mark");
    case lex_utf16_le_bom:
      return T_("UTF-16le Byte Order Mark");
    default:
      return "??????";
  }
}

/*
 * Convert a lex token to a string
 * used for debug/error printing.
 */
const char* lex_tok_to_str(int token)
{
  switch (token) {
    case L_EOF:
      return "L_EOF";
    case L_EOL:
      return "L_EOL";
    case BCT_NONE:
      return "BCT_NONE";
    case BCT_NUMBER:
      return "BCT_NUMBER";
    case BCT_IPADDR:
      return "BCT_IPADDR";
    case BCT_IDENTIFIER:
      return "BCT_IDENTIFIER";
    case BCT_UNQUOTED_STRING:
      return "BCT_UNQUOTED_STRING";
    case BCT_QUOTED_STRING:
      return "BCT_QUOTED_STRING";
    case BCT_BOB:
      return "BCT_BOB";
    case BCT_EOB:
      return "BCT_EOB";
    case BCT_EQUALS:
      return "BCT_EQUALS";
    case BCT_ERROR:
      return "BCT_ERROR";
    case BCT_EOF:
      return "BCT_EOF";
    case BCT_COMMA:
      return "BCT_COMMA";
    case BCT_EOL:
      return "BCT_EOL";
    case BCT_UTF8_BOM:
      return "BCT_UTF8_BOM";
    case BCT_UTF16_BOM:
      return "BCT_UTF16_BOM";
    default:
      return "??????";
  }
}

static uint32_t scan_pint(LEX* lf, char* str)
{
  int64_t val = 0;

  if (!Is_a_number(str)) {
    scan_err1(lf, T_("expected a positive integer number, got: %s"), str);
  } else {
    errno = 0;
    val = str_to_int64(str);
    if (errno != 0 || val < 0) {
      scan_err1(lf, T_("expected a positive integer number, got: %s"), str);
    }
  }

  return (uint32_t)(val & 0xffffffff);
}

static uint64_t scan_pint64(LEX* lf, char* str)
{
  uint64_t val = 0;

  if (!Is_a_number(str)) {
    scan_err1(lf, T_("expected a positive integer number, got: %s"), str);
  } else {
    errno = 0;
    val = str_to_uint64(str);
    if (errno != 0) {
      scan_err1(lf, T_("expected a positive integer number, got: %s"), str);
    }
  }

  return val;
}

class TemporaryBuffer {
 public:
  TemporaryBuffer(FILE* fd) : buf(GetPoolMemory(PM_NAME)), fd_(fd)
  {
    pos_ = ftell(fd_);
  }
  ~TemporaryBuffer()
  {
    FreePoolMemory(buf);
    fseek(fd_, pos_, SEEK_SET);
  }
  POOLMEM* buf;

 private:
  FILE* fd_;
  long pos_;
};

static bool NextLineContinuesWithQuotes(LEX* lf)
{
  TemporaryBuffer t(lf->fd);

  if (bfgets(t.buf, lf->fd) != NULL) {
    int i = 0;
    while (t.buf[i] != '\0') {
      if (t.buf[i] == '"') { return true; }
      if (t.buf[i] != ' ' && t.buf[i] != '\t') { return false; }
      ++i;
    };
  }
  return false;
}

static bool CurrentLineContinuesWithQuotes(LEX* lf)
{
  int i = lf->col_no;
  while (lf->line[i] != '\0') {
    if (lf->line[i] == '"') { return true; }
    if (lf->line[i] != ' ' && lf->line[i] != '\t') { return false; }
    ++i;
  };
  return false;
}

/*
 *
 * Get the next token from the input
 *
 */
int LexGetToken(LEX* lf, int expect)
{
  int ch;
  int token = BCT_NONE;
  bool continue_string = false;
  bool esc_next = false;
  /* Unicode files, especially on Win32, may begin with a "Byte Order Mark"
     to indicate which transmission format the file is in. The codepoint for
     this mark is U+FEFF and is represented as the octets EF-BB-BF in UTF-8
     and as FF-FE in UTF-16le(little endian) and  FE-FF in UTF-16(big endian).
     We use a distinct state for UTF-8 and UTF-16le, and use bom_bytes_seen
     to tell which byte we are expecting. */
  int bom_bytes_seen = 0;

  Dmsg0(debuglevel, "enter LexGetToken\n");
  while (token == BCT_NONE) {
    ch = LexGetChar(lf);
    switch (lf->state) {
      case lex_none:
        Dmsg2(debuglevel, "Lex state lex_none ch=%d,%x\n", ch, ch);
        if (B_ISSPACE(ch)) break;
        if (B_ISALPHA(ch)) {
          if (lf->options & LOPT_NO_IDENT || lf->options & LOPT_STRING) {
            lf->state = lex_string;
          } else {
            lf->state = lex_identifier;
          }
          BeginStr(lf, ch);
          break;
        }
        if (B_ISDIGIT(ch)) {
          if (lf->options & LOPT_STRING) {
            lf->state = lex_string;
          } else {
            lf->state = lex_number;
          }
          BeginStr(lf, ch);
          break;
        }
        Dmsg0(debuglevel, "Enter lex_none switch\n");
        switch (ch) {
          case L_EOF:
            token = BCT_EOF;
            Dmsg0(debuglevel, "got L_EOF set token=T_EOF\n");
            break;
          case '#':
            lf->state = lex_comment;
            break;
          case '{':
            token = BCT_BOB;
            BeginStr(lf, ch);
            break;
          case '}':
            token = BCT_EOB;
            BeginStr(lf, ch);
            break;
          case ' ':
            if (continue_string) { continue; }
            break;
          case '"':
            lf->state = lex_quoted_string;
            if (!continue_string) { BeginStr(lf, 0); }
            break;
          case '=':
            token = BCT_EQUALS;
            BeginStr(lf, ch);
            break;
          case ',':
            token = BCT_COMMA;
            BeginStr(lf, ch);
            break;
          case ';':
            if (expect != BCT_SKIP_EOL) {
              token = BCT_EOL; /* treat ; like EOL */
            }
            break;
          case L_EOL:
            if (continue_string) {
              continue;
            } else {
              Dmsg0(debuglevel, "got L_EOL set token=BCT_EOL\n");
              if (expect != BCT_SKIP_EOL) { token = BCT_EOL; }
            }
            break;
          case '@':
            /* In NO_EXTERN mode, @ is part of a string */
            if (lf->options & LOPT_NO_EXTERN) {
              lf->state = lex_string;
              BeginStr(lf, ch);
            } else {
              lf->state = lex_include;
              BeginStr(lf, 0);
            }
            break;
          case 0xEF: /* probably a UTF-8 BOM */
          case 0xFF: /* probably a UTF-16le BOM */
          case 0xFE: /* probably a UTF-16be BOM (error)*/
            if (lf->line_no != 1 || lf->col_no != 1) {
              lf->state = lex_string;
              BeginStr(lf, ch);
            } else {
              bom_bytes_seen = 1;
              if (ch == 0xEF) {
                lf->state = lex_utf8_bom;
              } else if (ch == 0xFF) {
                lf->state = lex_utf16_le_bom;
              } else {
                scan_err0(lf,
                          T_("This config file appears to be in an "
                             "unsupported Unicode format (UTF-16be). Please "
                             "resave as UTF-8\n"));
                return BCT_ERROR;
              }
            }
            break;
          default:
            lf->state = lex_string;
            BeginStr(lf, ch);
            break;
        }
        break;
      case lex_comment:
        Dmsg1(debuglevel, "Lex state lex_comment ch=%x\n", ch);
        if (ch == L_EOL) {
          lf->state = lex_none;
          if (expect != BCT_SKIP_EOL) { token = BCT_EOL; }
        } else if (ch == L_EOF) {
          token = BCT_ERROR;
        }
        break;
      case lex_number:
        Dmsg2(debuglevel, "Lex state lex_number ch=%x %c\n", ch, ch);
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        /* Might want to allow trailing specifications here */
        if (B_ISDIGIT(ch)) {
          add_str(lf, ch);
          break;
        }

        /* A valid number can be terminated by the following */
        if (B_ISSPACE(ch) || ch == L_EOL || ch == ',' || ch == ';') {
          token = BCT_NUMBER;
          lf->state = lex_none;
        } else {
          lf->state = lex_string;
        }
        LexUngetChar(lf);
        break;
      case lex_ip_addr:
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        Dmsg1(debuglevel, "Lex state lex_ip_addr ch=%x\n", ch);
        break;
      case lex_string:
        Dmsg1(debuglevel, "Lex state lex_string ch=%x\n", ch);
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        if (ch == '\n' || ch == L_EOL || ch == '=' || ch == '}' || ch == '{'
            || ch == '\r' || ch == ';' || ch == ',' || ch == '#'
            || (B_ISSPACE(ch)) || ch == '"') {
          LexUngetChar(lf);
          token = BCT_UNQUOTED_STRING;
          lf->state = lex_none;
          break;
        }
        add_str(lf, ch);
        break;
      case lex_identifier:
        Dmsg2(debuglevel, "Lex state lex_identifier ch=%x %c\n", ch, ch);
        if (B_ISALPHA(ch)) {
          add_str(lf, ch);
          break;
        } else if (B_ISSPACE(ch)) {
          break;
        } else if (ch == '\n' || ch == L_EOL || ch == '=' || ch == '}'
                   || ch == '{' || ch == '\r' || ch == ';' || ch == ','
                   || ch == '"' || ch == '#') {
          LexUngetChar(lf);
          token = BCT_IDENTIFIER;
          lf->state = lex_none;
          break;
        } else if (ch == L_EOF) {
          token = BCT_ERROR;
          lf->state = lex_none;
          BeginStr(lf, ch);
          break;
        }
        /* Some non-alpha character => string */
        lf->state = lex_string;
        add_str(lf, ch);
        break;
      case lex_quoted_string:
        Dmsg2(debuglevel, "Lex state lex_quoted_string ch=%x %c\n", ch, ch);
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        if (ch == L_EOL) {
          esc_next = false;
          break;
        }
        if (esc_next) {
          add_str(lf, ch);
          esc_next = false;
          break;
        }
        if (ch == '\\') {
          esc_next = true;
          break;
        }
        if (ch == '"') {
          if (NextLineContinuesWithQuotes(lf)
              || CurrentLineContinuesWithQuotes(lf)) {
            continue_string = true;
            lf->state = lex_none;
            continue;
          } else {
            token = BCT_QUOTED_STRING;
            /* Since we may be scanning a quoted list of names,
             *  we get the next character (a comma indicates another
             *  one), then we put it back for rescanning. */
            LexGetChar(lf);
            LexUngetChar(lf);
            lf->state = lex_none;
          }
          break;
        }
        continue_string = false;
        add_str(lf, ch);
        break;
      case lex_include_quoted_string:
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        if (esc_next) {
          add_str(lf, ch);
          esc_next = false;
          break;
        }
        if (ch == '\\') {
          esc_next = true;
          break;
        }
        if (ch == '"') {
          /* Keep the original LEX so we can print an error if the included file
           * can't be opened. */
          LEX* lfori = lf;
          /* Skip the double quote when restarting parsing */
          LexGetChar(lf);

          lf->state = lex_none;
          lf = lex_open_file(lf, lf->str, lf->ScanError, lf->scan_warning);
          if (lf == NULL) {
            BErrNo be;
            scan_err2(lfori, T_("Cannot open included config file %s: %s\n"),
                      lfori->str, be.bstrerror());
            return BCT_ERROR;
          }
          break;
        }
        add_str(lf, ch);
        break;
      case lex_include: /* scanning a filename */
        if (ch == L_EOF) {
          token = BCT_ERROR;
          break;
        }
        if (ch == '"') {
          lf->state = lex_include_quoted_string;
          break;
        }


        if (B_ISSPACE(ch) || ch == '\n' || ch == L_EOL || ch == '}' || ch == '{'
            || ch == ';' || ch == ',' || ch == '"' || ch == '#') {
          /* Keep the original LEX so we can print an error if the included file
           * can't be opened. */
          LEX* lfori = lf;

          lf->state = lex_none;
          lf = lex_open_file(lf, lf->str, lf->ScanError, lf->scan_warning);
          if (lf == NULL) {
            BErrNo be;
            scan_err2(lfori, T_("Cannot open included config file %s: %s\n"),
                      lfori->str, be.bstrerror());
            return BCT_ERROR;
          }
          break;
        }
        add_str(lf, ch);
        break;
      case lex_utf8_bom:
        /* we only end up in this state if we have read an 0xEF
           as the first byte of the file, indicating we are probably
           reading a UTF-8 file */
        if (ch == 0xBB && bom_bytes_seen == 1) {
          bom_bytes_seen++;
        } else if (ch == 0xBF && bom_bytes_seen == 2) {
          token = BCT_UTF8_BOM;
          lf->state = lex_none;
        } else {
          token = BCT_ERROR;
        }
        break;
      case lex_utf16_le_bom:
        /* we only end up in this state if we have read an 0xFF
           as the first byte of the file -- indicating that we are
           probably dealing with an Intel based (little endian) UTF-16 file*/
        if (ch == 0xFE) {
          token = BCT_UTF16_BOM;
          lf->state = lex_none;
        } else {
          token = BCT_ERROR;
        }
        break;
    }
    Dmsg4(debuglevel, "ch=%d state=%s token=%s %c\n", ch,
          lex_state_to_str(lf->state), lex_tok_to_str(token), ch);
  }
  Dmsg2(debuglevel, "lex returning: line %d token: %s\n", lf->line_no,
        lex_tok_to_str(token));
  lf->token = token;

  /* Here is where we check to see if the user has set certain
   *  expectations (e.g. 32 bit integer). If so, we do type checking
   *  and possible additional scanning (e.g. for range). */
  switch (expect) {
    case BCT_PINT16:
      lf->u.pint16_val = (scan_pint(lf, lf->str) & 0xffff);
      lf->u2.pint16_val = lf->u.pint16_val;
      token = BCT_PINT16;
      break;

    case BCT_PINT32:
      lf->u.pint32_val = scan_pint(lf, lf->str);
      lf->u2.pint32_val = lf->u.pint32_val;
      token = BCT_PINT32;
      break;

    case BCT_PINT32_RANGE:
      if (token == BCT_NUMBER) {
        lf->u.pint32_val = scan_pint(lf, lf->str);
        lf->u2.pint32_val = lf->u.pint32_val;
        token = BCT_PINT32;
      } else {
        char* p = strchr(lf->str, '-');
        if (!p) {
          scan_err2(lf, T_("expected an integer or a range, got %s: %s"),
                    lex_tok_to_str(token), lf->str);
          token = BCT_ERROR;
          break;
        }
        *p++ = 0; /* Terminate first half of range */
        lf->u.pint32_val = scan_pint(lf, lf->str);
        lf->u2.pint32_val = scan_pint(lf, p);
        token = BCT_PINT32_RANGE;
      }
      break;

    case BCT_INT16:
      if (token != BCT_NUMBER || !Is_a_number(lf->str)) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
        break;
      }
      errno = 0;
      lf->u.int16_val = (int16_t)str_to_int64(lf->str);
      if (errno != 0) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
      } else {
        token = BCT_INT16;
      }
      break;

    case BCT_INT32:
      if (token != BCT_NUMBER || !Is_a_number(lf->str)) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
        break;
      }
      errno = 0;
      lf->u.int32_val = (int32_t)str_to_int64(lf->str);
      if (errno != 0) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
      } else {
        token = BCT_INT32;
      }
      break;

    case BCT_INT64:
      Dmsg2(debuglevel, "int64=:%s: %f\n", lf->str, strtod(lf->str, NULL));
      if (token != BCT_NUMBER || !Is_a_number(lf->str)) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
        break;
      }
      errno = 0;
      lf->u.int64_val = str_to_int64(lf->str);
      if (errno != 0) {
        scan_err2(lf, T_("expected an integer number, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
      } else {
        token = BCT_INT64;
      }
      break;

    case BCT_PINT64_RANGE:
      if (token == BCT_NUMBER) {
        lf->u.pint64_val = scan_pint64(lf, lf->str);
        lf->u2.pint64_val = lf->u.pint64_val;
        token = BCT_PINT64;
      } else {
        char* p = strchr(lf->str, '-');
        if (!p) {
          scan_err2(lf, T_("expected an integer or a range, got %s: %s"),
                    lex_tok_to_str(token), lf->str);
          token = BCT_ERROR;
          break;
        }
        *p++ = 0; /* Terminate first half of range */
        lf->u.pint64_val = scan_pint64(lf, lf->str);
        lf->u2.pint64_val = scan_pint64(lf, p);
        token = BCT_PINT64_RANGE;
      }
      break;

    case BCT_NAME:
      if (token != BCT_IDENTIFIER && token != BCT_UNQUOTED_STRING
          && token != BCT_QUOTED_STRING) {
        scan_err2(lf, T_("expected a name, got %s: %s"), lex_tok_to_str(token),
                  lf->str);
        token = BCT_ERROR;
      } else if (lf->str_len > MAX_RES_NAME_LENGTH) {
        scan_err3(lf, T_("name %s length %d too long, max is %d\n"), lf->str,
                  lf->str_len, MAX_RES_NAME_LENGTH);
        token = BCT_ERROR;
      }
      break;

    case BCT_STRING:
      if (token != BCT_IDENTIFIER && token != BCT_UNQUOTED_STRING
          && token != BCT_QUOTED_STRING) {
        scan_err2(lf, T_("expected a string, got %s: %s"),
                  lex_tok_to_str(token), lf->str);
        token = BCT_ERROR;
      } else {
        token = BCT_STRING;
      }
      break;


    default:
      break; /* no expectation given */
  }
  lf->token = token; /* set possible new token */
  return token;
}

lex_ptr LexFile(const char* file,
                void* ctx,
                int err_type,
                LEX_ERROR_HANDLER* err,
                LEX_WARNING_HANDLER* warn)
{
  lex_ptr p{lex_open_file(nullptr, file, err, warn)};

  if (!p) { return p; }

  LexSetErrorHandlerErrorType(p.get(), err_type);
  p->error_counter = 0;
  p->caller_ctx = ctx;

  return p;
}

namespace lex {

namespace {

std::optional<std::string> read_fd(FILE* f)
{
  auto size = 64 * 1024;
  auto buffer = std::make_unique<char[]>(size);
  ASSERT(buffer);

  std::string res;
  for (;;) {
    auto bytes_read = fread(buffer.get(), 1, size, f);

    if (bytes_read == 0) {
      if (ferror(f)) { return std::nullopt; }

      return res;
    }

    res += std::string_view{buffer.get(), bytes_read};
  }
}

source read_pipe(const char* cmd)
{
  auto* pipe = OpenBpipe(cmd, 0, "rb");

  if (!pipe) { throw parse_error("Could not execute cmd '%s'", cmd); }


  auto content = read_fd(pipe->rfd);

  CloseBpipe(pipe);

  if (!content) {
    throw parse_error("An error occured while reading from command '%s'", cmd);
  }

  return {cmd, std::move(content).value()};
}

source read_file(const char* path)
{
  auto fd = fopen(path, "rb");
  if (!fd) { throw parse_error("Could not open file '%s'", path); }

  auto content = read_fd(fd);
  fclose(fd);

  if (!content) {
    throw parse_error("An error occured while reading from file '%s'", path);
  }

  return {path, std::move(content).value()};
}

static constexpr token Err = {.type = token_type::Err, .loc = {}};

token simple_token(token_type t, lex_point start, lex_point end)
{
  source_location loc{start, end};
  return token{t, loc};
}

const char* state_name(lex_state state)
{
  switch (state) {
    case lex_state::None:
      return "Nothing";
    case lex_state::Comment:
      return "Comment";
    case lex_state::Number:
      return "Number";
    case lex_state::Ident:
      return "Identifier";
    case lex_state::String:
      return "String";
    case lex_state::QuotedString:
      return "Quoted String";
    case lex_state::Include:
      return "Include Directive";
    case lex_state::IncludeQuoted:
      return "Quoted Include Directive";
    case lex_state::Utf8Bom:
      return "Byte-Order-Mark (Utf8)";
    case lex_state::Utf16Bom:
      return "Byte-Order-Mark (Utf16)";
  }

  return "Unknown";
}


}  // namespace

void read_files(lexer& lex, const char* path)
{
#ifdef HAVE_GLOB
  glob_t fileglob;

  /* Flag GLOB_NOMAGIC is a GNU extension, therefore manually check if string
   * is a wildcard string. */

  // Clear fileglob at least required for mingw version of glob()
  memset(&fileglob, 0, sizeof(fileglob));
  int globrc = glob(path, 0, NULL, &fileglob);

  if ((globrc == GLOB_NOMATCH) && (IsWildcardString(path))) {
    return;
  } else if (globrc != 0) {
    throw parse_error("Could not find file '%s'", path);
  }

  Dmsg2(100, "glob %s: %i files\n", path, fileglob.gl_pathc);
  for (size_t i = 0; i < fileglob.gl_pathc; i++) {
    const char* filename_expanded = fileglob.gl_pathv[i];
    lex.append_source(read_file(filename_expanded));
  }
  globfree(&fileglob);
#else
  sources.emplace_back(read_file(path));
#endif
}

lexer open_files(const char* path)
{
  lexer res;

  if (path[0] == '|') {
    // get contents from pipe
    source s = read_pipe(path + 1);

    res.append_source(std::move(s));
  } else {
    read_files(res, path);
  }
  return res;
}

std::optional<char> lexed_source::read()
{
  auto& data = content.data;
  ASSERT(byte_offset <= data.size());

  if (byte_offset > data.size()) {
    throw parse_error("Tried to read past eof");
  }

  if (byte_offset == data.size()) {
    byte_offset += 1;  // make sure next read is an error
    return std::nullopt;
  }

  char c = data[byte_offset++];

  if (c == '\n') {
    line += 1;
  } else {
    col += 1;
  }

  return c;
}

void lexer::reset_global_offset_to(lex_point p)
{
  for (size_t i = 0; i < translations.size(); ++i) {
    if (translations[i].start.offset > p.offset) {
      translations.resize(i);
      break;
    }
  }
  current_offset = p;
}

token lexer::next_token(bool skip_eol)
{
  // local parsing state
  lex_state internal_state{lex_state::None};
  bool continue_string = false;
  bool escape_next = false;
  size_t bom_bytes_seen = 0;
  auto start = current_offset;

  buffer.clear();

  if (source_queue.size() == 0) {
    return simple_token(token_type::FileEnd, current_offset, current_offset);
  }

  for (;;) {
    auto file_index = source_queue.front();

    ASSERT(file_index < sources.size());

    auto& current_source = sources[file_index];

    auto local_pos = current_source.current_pos();
    auto current = this->current_offset;
    auto c = current_source.read();

    if (!c) {
      Dmsg3(debuglevel, "state = %s, char = 'EOF' (-1)\n",
            state_name(internal_state));
      source_queue.pop_front();

      if (source_queue.size() > 0) {
        continue;
      } else if (internal_state == lex_state::None) {
        return simple_token(token_type::FileEnd, start, current);
      } else {
        throw parse_error("Hit unexpected end of file while reading %s\n",
                          state_name(internal_state));
      }
    }

    // if we actually read a character, we need to update the global_offset ...
    this->current_offset.offset += 1;

    // ... and update the translation map in case we changed files
    if (translations.size() == 0
        || translations.back().source_index != file_index) {
      // we cant have advanced more bytes locally than we did globally
      translations.push_back({
          .start = current,
          .source_index = file_index,
          .start_offset = local_pos.byte_offset,
          .start_line = local_pos.line,
          .start_col = local_pos.col,
      });
    }


    auto ch = *c;

    Dmsg3(debuglevel, "state = %s, char = '%c' (%d)\n",
          state_name(internal_state), ch, (int)ch);

    switch (internal_state) {
      case lex_state::None: {
        if (B_ISALPHA(ch)) {
          if (opts.no_identifiers || opts.force_string) {
            internal_state = lex_state::String;
          } else {
            internal_state = lex_state::Ident;
          }

          buffer.push_back(ch);
          break;
        }

        if (B_ISDIGIT(ch)) {
          if (opts.force_string) {
            internal_state = lex_state::String;
          } else {
            internal_state = lex_state::Number;
          }

          buffer.push_back(ch);

          break;
        }

        switch (ch) {
          case '#': {
            internal_state = lex_state::Comment;
          } break;
          case '{': {
            return simple_token(token_type::OpenBlock, start, current);
          } break;
          case '}': {
            return simple_token(token_type::CloseBlock, start, current);
          } break;

          case ' ': {
          } break;
          case '"': {
            internal_state = lex_state::QuotedString;
            // if (!continue_string) { } ??
          } break;
          case '=': {
            return simple_token(token_type::Eq, start, current);
          } break;
          case ',': {
            return simple_token(token_type::Comma, start, current);
          } break;
          case ';': {
            if (!skip_eol) {
              return simple_token(token_type::LineEnd, start, current);
            }
          } break;
          case '\n': {
            if (continue_string) {
              continue;
            } else {
              return simple_token(token_type::LineEnd, start, current);
            }
          } break;
          case '@': {
            if (opts.disable_includes) {
              internal_state = lex_state::String;
              buffer.push_back(ch);
            } else {
              internal_state = lex_state::Include;
            }
          } break;
          case (char)0xEF:
            [[fallthrough]];
          case (char)0xFF:
            [[fallthrough]];
          case (char)0xFE: {
            /* MARKER */
            return Err;
          } break;
          default: {
            if (B_ISSPACE(ch)) {
              // we just ignore space
            } else {
              internal_state = lex_state::String;
              buffer.push_back(ch);
            }
          } break;
        }

      } break;
      case lex_state::Comment: {
        if (ch == '\n') {
          internal_state = lex_state::None;

          reset_global_offset_to(current);
          current_source.reset_to(local_pos);

          if (!skip_eol) {
            return simple_token(token_type::LineEnd, start, current);
          }
        }
      } break;
      case lex_state::Number: {
        if (B_ISDIGIT(ch)) {
          buffer.push_back(ch);
        } else if (B_ISSPACE(ch) || ch == '\n' || ch == ',' || ch == ';') {
          /* A valid number can be terminated by the following */

          reset_global_offset_to(current);
          current_source.reset_to(local_pos);

          return simple_token(token_type::Number, start, current);
        } else {
          internal_state = lex_state::String;

          reset_global_offset_to(current);
          current_source.reset_to(local_pos);
        }
      } break;
      case lex_state::String: {
        if (ch == '\n' || ch == '=' || ch == '}' || ch == '{' || ch == '\r'
            || ch == ';' || ch == ',' || ch == '#' || (B_ISSPACE(ch))
            || ch == '"') {
          reset_global_offset_to(current);
          current_source.reset_to(local_pos);
          return simple_token(token_type::UnquotedString, start, current);
        }

        buffer.push_back(ch);
      } break;
      case lex_state::Ident: {
        if (B_ISALPHA(ch)) {
          buffer.push_back(ch);
        } else if (ch == '\n' || ch == '=' || ch == '}' || ch == '{'
                   || ch == '\r' || ch == ';' || ch == ',' || ch == '"'
                   || ch == '#') {
          reset_global_offset_to(current);
          current_source.reset_to(local_pos);
          return simple_token(token_type::Identifier, start, current);
        } else if (B_ISSPACE(ch)) {
          // ignore
        } else {
          /* Some non-alpha character => string */
          internal_state = lex_state::String;
          buffer.push_back(ch);
        }
      } break;
      case lex_state::QuotedString: {
        if (ch == '\n') {
          escape_next = false;
        } else if (escape_next) {
          buffer.push_back(ch);
          escape_next = false;
        } else if (ch == '\\') {
          escape_next = true;
        } else if (ch == '"') {
          auto now = current_source.current_pos();

          /* MARKER */
          // this is not completely correct.  We need to allow this
          // to span files
          // Maybe this should not be allowed and we should always
          // treat file change as a complete token divider ?

          // we want to continue this string if the next line
          // essentially starts with '"'
          size_t bytes_read = 0;
          bool cont = [&] {
            for (;;) {
              auto next = current_source.read();
              bytes_read += 1;
              if (next && *next == '"') {
                // we found '"', so we continue reading the string
                return true;
              } else if (next && B_ISSPACE(*next)) {
                // we ignore space
                continue;
              }

              // either we reached the eof or
              // we found something that is neither space nor '"', so we stop
              return false;
            }
          }();

          if (cont) {
            // we do not need to reset the position now because we already
            // are at the start of the next string, but we need to update
            // the global offset
            current_offset.offset += bytes_read;
            continue_string = true;
          } else {
            // as we read stuff that does not belong to us, we need to
            // reset the position
            current_source.reset_to(now);
            return simple_token(token_type::QuotedString, start, current);
          }
        } else {
          continue_string = false;
          buffer.push_back(ch);
        }
      } break;
      case lex_state::Include: {
        if (ch == '"') {
          /* MARKER */  // this does not make sense.
                        // this should only be possible as the first character

          // maybe this was done to support things like
          //    @/my/path/"with a space"
          // ? But also isnt great as this does not work as expected:
          //    @/my/path/"with a space"/and/some/subdirs

          internal_state = lex_state::IncludeQuoted;
        } else if (B_ISSPACE(ch) || ch == '\n' || ch == '}' || ch == '{'
                   || ch == ';' || ch == ',' || ch == '"' || ch == '#') {
          current_source.reset_to(local_pos);
          /* MARKER */
          // TODO: we need to update the global offset & translation map
          // as well!

          try {
            auto new_lex = open_files(buffer.c_str());
            auto offset = sources.size();

            // we need to insert them backwards to preserve the order
            // in the queue

            auto last = new_lex.sources.size() - 1;
            for (size_t i = 0; i < new_lex.sources.size(); ++i) {
              sources.push_back(std::move(new_lex.sources[last - i]));
              source_queue.push_front(offset + i);
            }
          } catch (parse_error& s) {
            source_location loc{start, current};
            s.add_context(
                format_comment(loc, "File included from here").c_str());
            throw s;
          }

          buffer.clear();
          internal_state = lex_state::None;
          continue;
        } else {
          buffer.push_back(ch);
        }
      } break;
      case lex_state::IncludeQuoted: {
        if (escape_next) {
          buffer.push_back(ch);
          escape_next = false;
        } else if (ch == '\\') {
          escape_next = true;
        } else if (ch == '"') {
          try {
            auto new_lex = open_files(buffer.c_str());
            auto offset = sources.size();

            // we need to insert them backwards to preserve the order
            // in the queue

            auto last = new_lex.sources.size() - 1;
            for (size_t i = 0; i < new_lex.sources.size(); ++i) {
              sources.push_back(std::move(new_lex.sources[last - i]));
              source_queue.push_front(offset + i);
            }
          } catch (parse_error& s) {
            source_location loc{start, current};
            s.add_context(
                format_comment(loc, "File included from here").c_str());
            throw s;
          }

          buffer.clear();
          internal_state = lex_state::None;
          continue;
        } else {
          buffer.push_back(ch);
        }
      } break;
      case lex_state::Utf8Bom: {
        if ((std::byte)ch == (std::byte)0xBB && bom_bytes_seen == 1) {
          bom_bytes_seen += 1;
        } else if ((std::byte)ch == (std::byte)0xBB && bom_bytes_seen == 2) {
          internal_state = lex_state::None;
          return simple_token(token_type::Utf8_BOM, start, current);
        } else {
          return Err;
        }

      } break;
      case lex_state::Utf16Bom: {
        if ((std::byte)ch == (std::byte)0xBB && bom_bytes_seen == 1) {
          bom_bytes_seen += 1;
        } else if ((std::byte)ch == (std::byte)0xBB && bom_bytes_seen == 2) {
          internal_state = lex_state::None;
          return simple_token(token_type::Utf16_BOM, start, current);
        } else {
          return Err;
        }
      } break;
    }
  }
}

using sm_iter = source_map::const_iterator;

static sm_iter translation_for_offset(const source_map& map, lex_point p)
{
  return std::lower_bound(map.begin(), map.end(), p.offset,
                          [](const source_translation& tl, size_t offset) {
                            return tl.start.offset < offset;
                          });
}

void lexer::reset_to(lex_point p)
{
  auto iter = translation_for_offset(translations, p);

  ASSERT(iter != translations.end());


  translations.erase(iter + 1, translations.end());

  auto& translation = *iter;
  (void) translation;

  // translation.global_start_offset;


}

static size_t inclusion_size(const source_map& map, sm_iter translation)
{
  if (translation + 1 == map.end()) {
    // no known end
    return std::numeric_limits<size_t>::max();
  }

  auto start = translation->start;
  auto end = (translation + 1)->start;

  // each inclusion is at least 1 char big
  ASSERT(start.offset < end.offset);

  return end.offset - start.offset;
}

static std::tuple<size_t, size_t, size_t> get_line_bounds(const lexed_source& s,
                                                          size_t byte_offset)
{
  // TODO: create lookup table

  const std::string& data = s.content.data;

  ASSERT(byte_offset < data.size());

  // we have to start looking from the one before byte_offset in case
  // byte_offset points to an '\n' as otherwise we just find byte_offset again
  auto prev_end
      = (byte_offset == 0) ? data.npos : data.rfind('\n', byte_offset - 1);
  auto end = data.find('\n', byte_offset);

  if (end == data.npos) {
    end = data.size();
  } else {
    // end should be one past the last character
    end += 1;
  }

  auto start = (prev_end == data.npos) ? 0 : (prev_end + 1);

  return {0, start, end};
}

std::string lexer::format_comment(source_location loc,
                                  std::string_view comment) const
{
  auto start = translation_for_offset(translations, loc.start);
  auto end = translation_for_offset(translations, loc.end);

  std::stringstream res;


  res << comment << '\n';

  auto current = start;

  struct printed_line {
    std::string_view source;
    size_t linum;

    std::string_view content;

    size_t highlight_start;
    size_t highlight_end;
  };

  std::vector<printed_line> lines;

  std::string highlight;

  for (size_t global_offset = loc.start.offset;
       global_offset < loc.end.offset;) {
    if (current == translations.end()) {
      // somebody gave us a source_location with an end that is too big
      // just ignore the rest
      res << "[missing " << std::to_string(loc.end.offset - global_offset)
          << " bytes]\n";
      break;
    }

    ASSERT(current != end);

    const lexed_source& current_source = sources[current->source_index];

    size_t max_size = loc.end.offset - global_offset;
    size_t total_local_size = inclusion_size(translations, current);
    size_t inclusion_offset = global_offset - current->start.offset;

    size_t local_start = current->start_offset + inclusion_offset;
    size_t local_size = std::min(max_size, total_local_size);
    size_t local_end = local_start + local_size;

    for (size_t local_current = local_start; local_current < local_end;) {
      auto [num, line_start, line_end]
          = get_line_bounds(current_source, local_current);
      ASSERT(line_start <= local_current);
      ASSERT(line_end > local_current);

      auto print_end = std::min(line_end, local_end);

      // we should probably only print to print_end and highlight to max_size
      // (and overshooting max size for 1 line)
      auto line = std::string_view{current_source.content.data}.substr(
          line_start, line_end - line_start);

      lines.push_back({
          .source = current_source.content.path,
          .linum = num,
          .content = line,
          .highlight_start = local_current - line_start,
          .highlight_end = print_end - line_start,
      });

      local_current = line_end;
    }

    global_offset += local_size;
    current += 1;
  }

  size_t max_num = 0;
  size_t max_source_len = 0;
  for (auto& line : lines) {
    max_source_len = std::max(max_source_len, line.source.size());
    max_num = std::max(max_num, line.linum);
  }

  // add 1 for ':'
  auto max_prefix_size = max_source_len + std::to_string(max_num).size() + 1;

  auto old_width = res.width();
  auto old_fill = res.fill();

  for (auto& line : lines) {
    res.fill(' ');
    res.width(0);

    size_t prefix_size = 0;
    if (line.source.size() > 0) {
      auto num_str = std::to_string(line.linum);
      prefix_size = line.source.size() + num_str.size() + 1;
      res << line.source << ":" << num_str;
    }


    res.width(max_prefix_size - prefix_size + 1);
    res << "";
    res.width(0);

    res << line.content;

    if (line.highlight_start != line.highlight_end) {
      res.width(max_prefix_size + 1);
      res << "";
      res.width(0);

      res.width(line.highlight_start);
      res << "";

      res.fill('~');
      res.width(line.highlight_end - line.highlight_start);
      res << "";
      res.width(0);

      res << "\n";
    }
  }

  res.width(old_width);
  res.fill(old_fill);

  return res.str();
}

const char* token_type_name(token_type t)
{
  switch (t) {
    case token_type::FileEnd:
      return "end of file";
    case token_type::Number:
      return "number";
    case token_type::Identifier:
      return "identifier";
    case token_type::UnquotedString:
      return "unquoted string";
    case token_type::QuotedString:
      return "quoted string";
    case token_type::OpenBlock:
      return "opening brace";
    case token_type::CloseBlock:
      return "closing brace";
    case token_type::Eq:
      return "equality sign";
    case token_type::Comma:
      return "comma";
    case token_type::LineEnd:
      return "end of line";
    case token_type::Err:
      return "error";
    case token_type::Utf8_BOM:
      return "byte order mark (utf8)";
    case token_type::Utf16_BOM:
      return "byte order mark (utf16)";
  }

  return "<unknown>";
}

template <> std::string GetValue(lexer* lex)
{
  auto token = lex->next_token();
  switch (token.type) {
    case token_type::Identifier:
    case token_type::QuotedString:
    case token_type::UnquotedString: {
      return lex->buffer;
    }
    default: {
      throw parse_error(
          "Expected a string, got %s:\n%s", token_type_name(token.type),
          lex->format_comment(token.loc, "Expected string").c_str());
    }
  }
}

template <> name_t GetValue(lexer* lex)
{
  auto token = lex->next_token();
  auto str = [&] {
    switch (token.type) {
      case token_type::Identifier:
      case token_type::QuotedString:
      case token_type::UnquotedString: {
        return lex->buffer;
      }
      default: {
        throw parse_error(
            "Expected a string, got %s:\n%s", token_type_name(token.type),
            lex->format_comment(token.loc, "Expected string").c_str());
      }
    }
  }();

  if (str.size() > MAX_NAME_LENGTH) {
    throw parse_error("Name is too long (%zu > %zu).\n%s", str.size(),
                      MAX_NAME_LENGTH,
                      lex->format_comment(token.loc, "defined here").c_str());
  }
  return {std::move(str)};
}

template <> quoted_string_t GetValue(lexer* lex)
{
  auto token = lex->next_token();
  if (token.type != token_type::QuotedString) {
    throw parse_error(
        "Expected a quoted string, got %s:\n%s", token_type_name(token.type),
        lex->format_comment(token.loc, "Expected quoted string here").c_str());
  }
  return {lex->buffer};
}

template <typename Int> Int GetIntValue(lexer* lex)
{
  Int value{};

  auto token = lex->next_token();

  const char* begin = lex->buffer.c_str();
  const char* end = lex->buffer.c_str() + lex->buffer.size();

  auto result = std::from_chars(begin, end, value);

  if (result.ec != std::errc()) {
    throw parse_error(
        "Expected a number here\n%s",
        lex->format_comment(token.loc,
                            make_error_condition(result.ec).message())
            .c_str());
  } else if (result.ptr != end) {
    throw parse_error(
        "Expected a number here\n%s",
        lex->format_comment(token.loc,
                            make_error_condition(result.ec).message())
            .c_str());
  }

  return value;
}

template <typename Int> std::pair<Int, Int> GetIntRangeValue(lexer* lex)
{
  std::pair<Int, Int> value{};

  auto token = lex->next_token();

  const char* begin = lex->buffer.c_str();
  const char* end = lex->buffer.c_str() + lex->buffer.size();

  auto result_left = std::from_chars(begin, end, value.first);

  if (result_left.ec != std::errc()) {
    throw parse_error(
        "Expected a number here\n%s",
        lex->format_comment(token.loc,
                            make_error_condition(result_left.ec).message())
            .c_str());
  }

  if (result_left.ptr == end) { return value; }

  const char* right = strchr(result_left.ptr, '-');

  if (!right) {
    throw parse_error("Expected range here\n%s",
                      lex->format_comment(token.loc, "").c_str());
  }

  auto result_right = std::from_chars(right + 1, end, value.second);

  if (result_right.ec != std::errc()) {
    throw parse_error(
        "Expected a number here\n%s",
        lex->format_comment(token.loc,
                            make_error_condition(result_right.ec).message())
            .c_str());
  }

  if (result_right.ptr != end) {
    throw parse_error("Expected range here\n%s",
                      lex->format_comment(token.loc, "").c_str());
  }

  return value;
}

template <> int16_t GetValue(lexer* lex) { return GetIntValue<int16_t>(lex); }
template <> int32_t GetValue(lexer* lex) { return GetIntValue<int32_t>(lex); }
template <> int64_t GetValue(lexer* lex) { return GetIntValue<int64_t>(lex); }
template <> uint16_t GetValue(lexer* lex) { return GetIntValue<uint16_t>(lex); }
template <> uint32_t GetValue(lexer* lex) { return GetIntValue<uint32_t>(lex); }
template <> uint64_t GetValue(lexer* lex) { return GetIntValue<uint64_t>(lex); }
template <> range32 GetValue(lexer* lex)
{
  return {GetIntRangeValue<uint32_t>(lex)};
}
template <> range64 GetValue(lexer* lex)
{
  return {GetIntRangeValue<uint64_t>(lex)};
}


}  // namespace lex
