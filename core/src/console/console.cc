/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
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
// Kern Sibbald, September MM
/**
 * @file
 * Bareos Console interface to the Director
 */

#include <grpcpp/grpcpp.h>
#include <config.grpc.pb.h>
#include <charconv>
#include <gsl/span>

#pragma GCC diagnostic ignored "-Wcpp"
#include <strstream>
#include <string_view>

#include "include/bareos.h"
#include "include/exit_codes.h"
#include "console/console_conf.h"
#include "console/console_globals.h"
#include "console/auth_pam.h"
#include "console/console_output.h"
#include "console/connect_to_director.h"
#include "include/jcr.h"
#include "lib/berrno.h"
#include "lib/bnet.h"
#include "lib/bnet_network_dump.h"
#include "lib/bsock.h"
#include "lib/bsock_tcp.h"
#include "lib/bstringlist.h"
#include "lib/cli.h"
#include "lib/qualified_resource_name_type_converter.h"
#include "lib/version.h"
#include "lib/watchdog.h"
#include "lib/bpipe.h"
#include <jansson.h>
#include <stdio.h>
#include <fstream>
#include <string>
#include <array>

#define ConInit(x)
#define ConTerm()
#define ConSetZedKeys()
#define trapctlc()
#define clrbrk()
#define usrbrk() 0

#if defined(HAVE_WIN32) && !defined(HAVE_MSVC)
// windows has its own isatty implemented, so
// if we are compiling with msvc we can just use that
#  define isatty(fd) ((fd) == 0)
#endif

using namespace console;

static void TerminateConsole(int sig);
static int CheckResources();
int GetCmd(FILE* input, const char* prompt, std::string& out);
static int DoOutputcmd(FILE* input, BareosSocket* UA_sock);

extern "C" void GotSigstop(int sig);
extern "C" void GotSigcontinue(int sig);
extern "C" void GotSigtout(int sig);
extern "C" void GotSigtin(int sig);

static char* configfile = NULL;
static BareosSocket* g_UA_sock = NULL;
static bool stop = false;
static int timeout = 0;
static int g_argc;
static int g_numdir;
static POOLMEM* g_args;
static char* g_argk[MAX_CMD_ARGS];
static char* g_argv[MAX_CMD_ARGS];

enum
{
  MAIN_PROMPT,
  FILE_SELECTION,
  CONFIG_EDIT,
} console_state
    = MAIN_PROMPT;

#if defined(HAVE_PAM)
static bool force_send_pam_credentials_unencrypted = false;
static bool use_pam_credentials_file = false;
static std::string pam_credentials_filename;
static const std::string program_arguments{"D:lc:d:np:ostu:x:z:?"};
#else
static const std::string program_arguments{"D:lc:d:nstu:x:z:?"};
#endif

/* Command prototypes */
static int Versioncmd(FILE* input, BareosSocket* UA_sock);
static int InputCmd(FILE* input, BareosSocket* UA_sock);
static int OutputCmd(FILE* input, BareosSocket* UA_sock);
static int TeeCmd(FILE* input, BareosSocket* UA_sock);
static int QuitCmd(FILE* input, BareosSocket* UA_sock);
static int HelpCmd(FILE* input, BareosSocket* UA_sock);
static int EchoCmd(FILE* input, BareosSocket* UA_sock);
static int TimeCmd(FILE* input, BareosSocket* UA_sock);
static int SleepCmd(FILE* input, BareosSocket* UA_sock);
static int ExecCmd(FILE* input, BareosSocket* UA_sock);
static int EolCmd(FILE* input, BareosSocket* UA_sock);
static int GrpcCmd(FILE* input, BareosSocket* UA_sock);
static int ConfigCmd(FILE* input, BareosSocket* UA_sock);

#ifndef HAVE_REGEX_H
#  include "lib/bregex.h"
#else
#  include <regex.h>
#endif

extern "C" void GotSigstop(int) { stop = true; }
extern "C" void GotSigcontinue(int) { stop = false; }
extern "C" void GotSigtout(int) {}
extern "C" void GotSigtin(int) {}

static int ZedKeyscmd(FILE*, BareosSocket*)
{
  ConSetZedKeys();
  return 1;
}

// These are the @commands that run only in bconsole
struct cmdstruct {
  const char* key;
  int (*func)(FILE* input, BareosSocket* UA_sock);
  const char* help;
};

static const auto commands = std::array{
    cmdstruct{NT_("input"), InputCmd, T_("input from file")},
    cmdstruct{NT_("output"), OutputCmd, T_("output to file")},
    cmdstruct{NT_("quit"), QuitCmd, T_("quit")},
    cmdstruct{NT_("tee"), TeeCmd, T_("output to file and terminal")},
    cmdstruct{NT_("sleep"), SleepCmd, T_("sleep specified time")},
    cmdstruct{NT_("time"), TimeCmd, T_("print current time")},
    cmdstruct{NT_("version"), Versioncmd, T_("print Console's version")},
    cmdstruct{NT_("echo"), EchoCmd, T_("echo command string")},
    cmdstruct{NT_("exec"), ExecCmd, T_("execute an external command")},
    cmdstruct{NT_("exit"), QuitCmd, T_("exit = quit")},
    cmdstruct{NT_("zed_keys"), ZedKeyscmd,
              T_("zed_keys = use zed keys instead of bash keys")},
    cmdstruct{NT_("help"), HelpCmd, T_("help listing")},
    cmdstruct{NT_("separator"), EolCmd, T_("set command separator")},
    cmdstruct{NT_("config"), ConfigCmd, T_("change the director config")},
    cmdstruct{NT_("grpc"), GrpcCmd, "set grpc address"},
};

static int Do_a_command(FILE* input, BareosSocket* UA_sock)
{
  int status;
  int found;
  int len;
  char* cmd;

  found = 0;
  status = 1;

  Dmsg1(120, "Command: %s\n", UA_sock->msg);
  if (g_argc == 0) { return 1; }

  cmd = g_argk[0] + 1;
  if (*cmd == '#') { /* comment */
    return 1;
  }
  len = strlen(cmd);
  for (auto& command : commands) { /* search for command */
    if (bstrncasecmp(cmd, T_(command.key), len)) {
      status = (*command.func)(input, UA_sock); /* go execute command */
      found = 1;
      break;
    }
  }
  if (!found) {
    PmStrcat(UA_sock->msg, T_(": is an invalid command\n"));
    UA_sock->message_length = strlen(UA_sock->msg);
    ConsoleOutput(UA_sock->msg);
  }
  return status;
}


static std::optional<std::string> GetInputLine(FILE* input, const char* prompt)
{
  bool tty_input = isatty(fileno(input));

  if (tty_input) {
    std::string line;
    if (GetCmd(input, prompt, line) < 0) { return std::nullopt; }
    return line;
  }

  std::size_t block_size = 1024;
  std::string line;

  for (;;) {
    auto start = line.size();
    line.resize(start + block_size);

    if (!fgets(line.data() + start, block_size, input)) {
      return std::nullopt;  // error occured
    }

    auto read_block = strlen(line.data() + start);
    line.resize(start + read_block);

    if (read_block == block_size - 1) {
      if (line[start + block_size - 2] == '\n') {
        // we read a line
        break;
      } else {
        continue;  // we need more data
      }
    }

    if (line[start + read_block - 1] != '\n') {
      printf("line: '%s'\n", line.c_str());
      printf("read_block: '%zu'\n", read_block);


      line.push_back('\n');
    }
    break;
  }

  // we only come here if we just read a line from file.
  // We now just need to print it.

  ConsoleOutput(line.c_str());

  return line;
}


static void ReadAndProcessInput(FILE* input, BareosSocket* UA_sock)
{
  const char* prompt = "*";
  bool at_prompt = false;
  int status;
  btimer_t* tid = NULL;

  while (1) {
    if (at_prompt) { /* don't prompt multiple times */
      prompt = "";
    } else {
      prompt = "*";
      at_prompt = true;
    }

    auto line = GetInputLine(input, prompt);

    if (!line) {
      status = -1;
    } else {
      UA_sock->message_length = PmStrcpy(UA_sock->msg, line->c_str());
      status = 1;
    }

    if (status < 0) {
      break;                  /* error or interrupt */
    } else if (status == 0) { /* timeout */
      if (bstrcmp(prompt, "*")) {
        tid = StartBsockTimer(UA_sock, timeout);
        UA_sock->fsend(".messages");
        StopBsockTimer(tid);
      } else {
        continue;
      }
    } else {
      at_prompt = false;
      // @ => internal command for us
      if (UA_sock->msg[0] == '@') {
        ParseArgs(UA_sock->msg, g_args, &g_argc, g_argk, g_argv, MAX_CMD_ARGS);
        if (!Do_a_command(input, UA_sock)) { break; }
        continue;
      }
      tid = StartBsockTimer(UA_sock, timeout);
      if (!UA_sock->send()) { /* send command */
        StopBsockTimer(tid);
        break; /* error */
      }
      StopBsockTimer(tid);
    }

    if (bstrcmp(UA_sock->msg, ".quit") || bstrcmp(UA_sock->msg, ".exit")) {
      break;
    }

    tid = StartBsockTimer(UA_sock, timeout);
    while ((status = UA_sock->recv()) >= 0
           || ((status == BNET_SIGNAL)
               && ((UA_sock->message_length != BNET_EOD)
                   && (UA_sock->message_length != BNET_MAIN_PROMPT)
                   && (UA_sock->message_length != BNET_SUB_PROMPT)))) {
      if (status == BNET_SIGNAL) {
        if (UA_sock->message_length == BNET_START_RTREE) {
          console_state = FILE_SELECTION;
        } else if (UA_sock->message_length == BNET_END_RTREE) {
          console_state = MAIN_PROMPT;
        }
        continue;
      }

      if (at_prompt) {
        if (!stop) { ConsoleOutput("\n"); }
        at_prompt = false;
      }

      /* Suppress output if running
       * in background or user hit ctl-c */
      if (!stop && !usrbrk()) {
        if (UA_sock->msg) { ConsoleOutput(UA_sock->msg); }
      }
    }
    StopBsockTimer(tid);

    if (usrbrk() > 1) {
      break;
    } else {
      clrbrk();
    }
    if (!stop) { fflush(stdout); }

    if (IsBnetStop(UA_sock)) {
      break; /* error or term */
    } else if (status == BNET_SIGNAL) {
      if (UA_sock->message_length == BNET_SUB_PROMPT) { at_prompt = true; }
      Dmsg1(100, "Got poll %s\n", BnetSignalToString(UA_sock).c_str());
    }
  }
}


#include <readline/readline.h>
#include <readline/history.h>
#include "lib/edit.h"
#include "lib/tls_openssl.h"
#include "lib/bsignal.h"

static char* get_first_keyword()
{
  char* ret = NULL;
  int len;
  char* first_space = strchr(rl_line_buffer, ' ');
  if (first_space) {
    len = first_space - rl_line_buffer;
    ret = (char*)malloc((len + 1) * sizeof(char));
    memcpy(ret, rl_line_buffer, len);
    ret[len] = 0;
  }
  return ret;
}

/**
 * Return the command before the current point.
 * Set nb to the number of command to skip
 */
static char* get_previous_keyword(int current_point, int nb)
{
  int i, end = -1, start = 0, inquotes = 0;
  char* s = NULL;

  while (nb-- >= 0) {
    // First we look for a space before the current word
    for (i = current_point; i >= 0; i--) {
      if (rl_line_buffer[i] == ' ' || rl_line_buffer[i] == '=') { break; }
    }

    for (; i >= 0; i--) {
      if (rl_line_buffer[i] != ' ') {
        end = i; /* end of command */
        break;
      }
    }

    if (end == -1) { return NULL; /* no end found */ }

    for (start = end; start > 0; start--) {
      if (rl_line_buffer[start] == '"') { inquotes = !inquotes; }
      if ((rl_line_buffer[start - 1] == ' ') && inquotes == 0) { break; }
      current_point = start; /* start of command */
    }
  }

  s = (char*)malloc(end - start + 2);
  memcpy(s, rl_line_buffer + start, end - start + 1);
  s[end - start + 1] = 0;

  //  printf("=======> %i:%i <%s>\n", start, end, s);

  return s;
}

struct ItemList {
  alist<const char*> list; /* holds the completion list */
};

static ItemList* items = NULL;
void init_items()
{
  if (!items) {
    items = (ItemList*)malloc(sizeof(ItemList));
    items = new (items) ItemList(); /* placement new instead of memset */
  } else {
    items->list.destroy();
    items->list.init();
  }
}

/**
 * Match a regexp and add the result to the items list
 * This function is recursive
 */
static void match_kw(regex_t* preg, const char* what, int len, POOLMEM*& buf)
{
  int rc, size;
  int nmatch = 20;
  regmatch_t pmatch[20]{};

  if (len <= 0) { return; }
  rc = regexec(preg, what, nmatch, pmatch, 0);
  if (rc == 0) {
    size = pmatch[1].rm_eo - pmatch[1].rm_so;
    buf = CheckPoolMemorySize(buf, size + 1);
    memcpy(buf, what + pmatch[1].rm_so, size);
    buf[size] = '\0';

    items->list.append(strdup(buf));

    /* search for next keyword */
    match_kw(preg, what + pmatch[1].rm_eo, len - pmatch[1].rm_eo, buf);
  }
}

/* fill the items list with the output of the help command */
void GetArguments(const char* what)
{
  regex_t preg{};
  POOLMEM* buf;
  int rc;
  init_items();

  rc = regcomp(&preg, "(([a-z_]+=)|([a-z]+)( |$))", REG_EXTENDED);
  if (rc != 0) { return; }

  buf = GetPoolMemory(PM_MESSAGE);
  g_UA_sock->fsend(".help item=%s", what);
  while (g_UA_sock->recv() > 0) {
    StripTrailingJunk(g_UA_sock->msg);
    match_kw(&preg, g_UA_sock->msg, g_UA_sock->message_length, buf);
  }
  FreePoolMemory(buf);
  regfree(&preg);
}

/* retreive a simple list (.pool, .client) and store it into items */
static void GetItems(const char* what)
{
  init_items();

  g_UA_sock->fsend("%s", what);
  while (g_UA_sock->recv() > 0) {
    StripTrailingJunk(g_UA_sock->msg);
    items->list.append(strdup(g_UA_sock->msg));
  }
}

typedef enum
{
  ITEM_ARG, /* item with simple list like .jobs */
  ITEM_HELP /* use help item=xxx and detect all arguments */
} cpl_item_t;

static char* item_generator(const char* text,
                            int state,
                            const char* item,
                            cpl_item_t type)
{
  static std::size_t list_index, len;
  const char* name;


  if (!state) {
    list_index = 0;
    len = strlen(text);
    switch (type) {
      case ITEM_ARG:
        GetItems(item);
        break;
      case ITEM_HELP:
        GetArguments(item);
        break;
    }
  }

  while (items && list_index < static_cast<size_t>(items->list.size())) {
    name = (char*)items->list[list_index];
    list_index++;
    if (bstrncmp(name, text, len)) { return strdup(name); }
  }

  /* no match */
  return ((char*)NULL);
}

static const char* cpl_item;
static cpl_item_t cpl_type;

static char* cpl_generator(const char* text, int state)
{
  return item_generator(text, state, cpl_item, cpl_type);
}

/* do not use the default filename completion */
static char* dummy_completion_function(const char*, int) { return NULL; }

struct cpl_keywords_t {
  const char* key;
  const char* cmd;
};

static const auto cpl_prompt_keywords = std::array{
    cpl_keywords_t{"pool=", ".pool"},
    cpl_keywords_t{"nextpool=", ".pool"},
    cpl_keywords_t{"fileset=", ".fileset"},
    cpl_keywords_t{"client=", ".client"},
    cpl_keywords_t{"jobdefs=", ".jobdefs"},
    cpl_keywords_t{"job=", ".jobs"},
    cpl_keywords_t{"restore_job=", ".jobs type=R"},
    cpl_keywords_t{"level=", ".level"},
    cpl_keywords_t{"storage=", ".storage"},
    cpl_keywords_t{"schedule=", ".schedule"},
    cpl_keywords_t{"volume=", ".media"},
    cpl_keywords_t{"oldvolume=", ".media"},
    cpl_keywords_t{"volstatus=", ".volstatus"},
    cpl_keywords_t{"catalog=", ".catalogs"},
    cpl_keywords_t{"message=", ".msgs"},
    cpl_keywords_t{"profile=", ".profiles"},
    cpl_keywords_t{"actiononpurge=", ".actiononpurge"},
};
static const auto cpl_file_selection_keywords = std::array{
    cpl_keywords_t{"ls", ".ls"},        cpl_keywords_t{"cd", ".lsdir"},
    cpl_keywords_t{"add", ".ls"},       cpl_keywords_t{"mark", ".ls"},
    cpl_keywords_t{"m", ".ls"},         cpl_keywords_t{"delete", ".lsmark"},
    cpl_keywords_t{"unmark", ".lsmark"}};

static char** select_from_keywords(gsl::span<const cpl_keywords_t> cpl_keywords,
                                   const char* text,
                                   int start)
{
  bool found = false;
  char** matches = nullptr;
  /* If this word is at the start of the line, then it is a command
   * to complete. Otherwise it is the name of a file in the current
   * directory.
   */
  char* s = get_previous_keyword(start, 0);
  char* cmd = get_first_keyword();
  if (s) {
    for (auto& keyword : cpl_keywords) {
      if (Bstrcasecmp(s, keyword.key)) {
        cpl_item = keyword.cmd;
        cpl_type = ITEM_ARG;
        matches = rl_completion_matches(text, cpl_generator);
        found = true;
        break;
      }
    }

    if (!found) { /* try to get help with the first command */
      cpl_item = cmd;
      cpl_type = ITEM_HELP;
      /* do not append space at the end */
      rl_completion_suppress_append = true;
      matches = rl_completion_matches(text, cpl_generator);
    }
    free(s);
  } else { /* nothing on the line, display all commands */
    cpl_item = ".help all";
    cpl_type = ITEM_ARG;
    matches = rl_completion_matches(text, cpl_generator);
  }
  if (cmd) { free(cmd); }

  return matches;
}

bareos::config::SchemaValue* current_value = nullptr;

static char* schema_value_generator(const char* text, int state)
{
  static int index = 0;
  static std::string_view start;

  if (!state) {
    // first call
    index = 0;
    start = text;
  }
  while (index < current_value->values_size()) {
    // we do not use a for loop here since we also need to increment index
    // in the case where we found a match (so we do a return from here)
    auto& val = current_value->values().Get(index++);
    if (start.size() <= val.size()
        && memcmp(start.data(), val.c_str(), start.size()) == 0) {
      return strdup(val.c_str());
    }
  }

  return nullptr;
}

/* Attempt to complete on the contents of TEXT.  START and END bound the
 * region of rl_line_buffer that contains the word to complete.  TEXT is
 * the word to complete.  We can use the entire contents of rl_line_buffer
 * in case we want to do some simple parsing.  Return the array of matches,
 * or NULL if there aren't any.
 */
static char** readline_completion(const char* text, int start, int)
{
  rl_attempted_completion_over = 1;

  switch (console_state) {
    case MAIN_PROMPT:
      return select_from_keywords(cpl_prompt_keywords, text, start);
    case FILE_SELECTION:
      return select_from_keywords(cpl_file_selection_keywords, text, start);
    case CONFIG_EDIT: {
      if (current_value && current_value->values_size() > 0) {
        return rl_completion_matches(text, schema_value_generator);
      }
      return nullptr;
    } break;
  }

  return nullptr;
}

static char eol = '\0';
static int EolCmd(FILE*, BareosSocket*)
{
  if ((g_argc > 1)
      && (strchr("!$%&'()*+,-/:;<>?[]^`{|}~", g_argk[1][0]) != NULL)) {
    eol = g_argk[1][0];
  } else if (g_argc == 1) {
    eol = '\0';
  } else {
    ConsoleOutput(T_("Illegal separator character.\n"));
  }
  return 1;
}

/**
 * Return 1 if OK
 *        0 if no input
 *       -1 error (must stop)
 */
int GetCmd(FILE* input, const char* prompt, std::string& out)
{
  static char* line = NULL;
  static char* next = NULL;
  static int do_history = 0;
  char* command;

  do_history = 0;
  rl_catch_signals = 0; /* do it ourselves */

  line = readline((char*)prompt); /* cast needed for old readlines */
  if (!line) { return -1; }
  StripTrailingJunk(line);
  command = line;

  /* Split "line" into multiple commands separated by the eol character.
   *   Each part is pointed to by "next" until finally it becomes null. */
  if (eol == '\0') {
    next = NULL;
  } else {
    next = strchr(command, eol);
    if (next) { *next = '\0'; }
  }
  if (command != line && isatty(fileno(input))) {
    ConsoleOutputFormat("%s%s\n", prompt, command);
  }

  out = command;
  if (out.size()) { do_history++; }

  if (!next) {
    if (do_history) {
      auto last_history_item = history_get(history_length);
      if (!last_history_item || strcmp(last_history_item->line, line) != 0) {
        add_history(line);
      }
    }
    free(line); /* allocated by readline() malloc */
    line = NULL;
  }
  return 1; /* OK */
}

static int ConsoleUpdateHistory(const char* histfile)
{
  int ret = 0;

  int max_history_length, truncate_entries;

  max_history_length
      = (console_resource) ? console_resource->history_length : 100;
  truncate_entries = max_history_length - history_length;
  if (truncate_entries < 0) { truncate_entries = 0; }

  if (history_truncate_file(histfile, truncate_entries) == 0) {
    ret = append_history(history_length, histfile);
  } else {
    ret = write_history(histfile);
  }

  return ret;
}

static int ConsoleInitHistory(const char* histfile)
{
  int ret = 0;

  int max_history_length;

  using_history();

  max_history_length
      = (console_resource) ? console_resource->history_length : 100;
  history_truncate_file(histfile, max_history_length);

  ret = read_history(histfile);

  rl_completion_entry_function = dummy_completion_function;
  rl_attempted_completion_function = readline_completion;
  rl_filename_completion_desired = 0;
  stifle_history(max_history_length);

  return ret;
}

static bool SelectDirector(const char* director,
                           DirectorResource** ret_dir,
                           ConsoleResource** ret_cons)
{
  int numcon = 0, numdir = 0;
  int i = 0, item = 0;
  BareosSocket* UA_sock;
  DirectorResource* director_resource_tmp = NULL;
  ConsoleResource* console_resource_tmp = NULL;

  *ret_cons = NULL;
  *ret_dir = NULL;

  numdir = 0;
  foreach_res (director_resource_tmp, R_DIRECTOR) { numdir++; }
  numcon = 0;
  foreach_res (console_resource_tmp, R_CONSOLE) { numcon++; }


  if (numdir == 1) { /* No choose */
    director_resource_tmp
        = (DirectorResource*)my_config->GetNextRes(R_DIRECTOR, NULL);
  }

  if (director) { /* Command line choice overwrite the no choose option */

    foreach_res (director_resource_tmp, R_DIRECTOR) {
      if (bstrcmp(director_resource_tmp->resource_name_, director)) { break; }
    }

    if (!director_resource_tmp) { /* Can't find Director used as argument */
      ConsoleOutputFormat(T_("Can't find %s in Director list\n"), director);
      return 0;
    }
  }

  if (!director_resource_tmp) { /* prompt for director */
    UA_sock = new BareosSocketTCP;
  try_again:
    ConsoleOutput(T_("Available Directors:\n"));

    numdir = 0;
    foreach_res (director_resource_tmp, R_DIRECTOR) {
      ConsoleOutputFormat(T_("%2d:  %s at %s:%d\n"), 1 + numdir++,
                          director_resource_tmp->resource_name_,
                          director_resource_tmp->address,
                          director_resource_tmp->DIRport);
    }

    std::string line{};
    if (GetCmd(stdin, T_("Select Director by entering a number: "), line) < 0) {
      WSACleanup(); /* Cleanup Windows sockets */
      return 0;
    }

    UA_sock->message_length = PmStrcpy(UA_sock->msg, line.c_str());

    if (!Is_a_number(UA_sock->msg)) {
      ConsoleOutputFormat(
          T_("%s is not a number. You must enter a number between "
             "1 and %d\n"),
          UA_sock->msg, numdir);
      goto try_again;
    }
    item = atoi(UA_sock->msg);
    if (item < 0 || item > numdir) {
      ConsoleOutputFormat(T_("You must enter a number between 1 and %d\n"),
                          numdir);
      goto try_again;
    }
    delete UA_sock;
    {
      ResLocker _{my_config};
      for (i = 0; i < item; i++) {
        director_resource_tmp = (DirectorResource*)my_config->GetNextRes(
            R_DIRECTOR, (BareosResource*)director_resource_tmp);
      }
    }
  }

  // Look for a console linked to this director
  ResLocker _{my_config};
  for (i = 0; i < numcon; i++) {
    console_resource_tmp = (ConsoleResource*)my_config->GetNextRes(
        R_CONSOLE, (BareosResource*)console_resource_tmp);
    if (console_resource_tmp->director
        && bstrcmp(console_resource_tmp->director,
                   director_resource_tmp->resource_name_)) {
      break;
    }
    console_resource_tmp = NULL;
  }

  // Look for the first non-linked console
  if (console_resource_tmp == NULL) {
    for (i = 0; i < numcon; i++) {
      console_resource_tmp = (ConsoleResource*)my_config->GetNextRes(
          R_CONSOLE, (BareosResource*)console_resource_tmp);
      if (console_resource_tmp->director == NULL) break;
      console_resource_tmp = NULL;
    }
  }

  // If no console, take first one
  if (!console_resource_tmp) {
    console_resource_tmp = (ConsoleResource*)my_config->GetNextRes(
        R_CONSOLE, (BareosResource*)NULL);
  }

  *ret_dir = director_resource_tmp;
  *ret_cons = console_resource_tmp;

  return 1;
}

#if defined(HAVE_PAM)
static BStringList ReadPamCredentialsFile(
    const std::string& t_pam_credentials_filename)
{
  std::ifstream s(t_pam_credentials_filename);
  std::string user, pw;
  if (!s.is_open()) {
    Emsg0(M_ERROR_TERM, 0, T_("Could not open PAM credentials file.\n"));
    return BStringList();
  } else {
    std::getline(s, user);
    std::getline(s, pw);
    if (user.empty() || pw.empty()) {
      Emsg0(M_ERROR_TERM, 0, T_("Could not read user or password.\n"));
      return BStringList();
    }
  }
  BStringList args;
  args << user << pw;
  return args;
}

static bool ExaminePamAuthentication(
    bool t_use_pam_credentials_file,
    const std::string& t_pam_credentials_filename)
{
  if (!g_UA_sock->tls_conn && !force_send_pam_credentials_unencrypted) {
    ConsoleOutput("Canceled because password would be sent unencrypted!\n");
    return false;
  }
  if (t_use_pam_credentials_file) {
    BStringList data(ReadPamCredentialsFile(t_pam_credentials_filename));
    if (data.empty()) { return false; }
    g_UA_sock->FormatAndSendResponseMessage(kMessageIdPamUserCredentials, data);
  } else {
    g_UA_sock->FormatAndSendResponseMessage(kMessageIdPamInteractive,
                                            std::string());
    if (!ConsolePamAuthenticate(stdin, g_UA_sock)) {
      TerminateConsole(0);
      return false;
    }
  }
  return true;
}
#endif /* HAVE_PAM */

int main(int argc, char* argv[])
{
  setlocale(LC_ALL, "");
  tzset();
  bindtextdomain("bareos", LOCALEDIR);
  textdomain("bareos");

  InitStackDump();
  MyNameIs(argc, argv, "bconsole");
  InitMsg(NULL, NULL);
  working_directory = "/tmp";
  g_args = GetPoolMemory(PM_FNAME);

  CLI::App console_app;
  InitCLIApp(console_app, "The Bareos Console.", 2000);

  console_app
      .add_option(
          "-c,--config",
          [](std::vector<std::string> val) {
            if (configfile != nullptr) { free(configfile); }
            configfile = strdup(val.front().c_str());
            return true;
          },
          "Use <path> as configuration file or directory")
      ->check(CLI::ExistingPath)
      ->type_name("<path>");

  char* director = nullptr;
  console_app
      .add_option(
          "-D,--director",
          [&director](std::vector<std::string> val) {
            if (director != nullptr) { free(director); }
            director = strdup(val.front().c_str());
            return true;
          },
          "Specify director.")
      ->type_name("<director>");

  AddDebugOptions(console_app);

  bool list_directors = false;
  bool test_config = false;
  console_app.add_flag(
      "-l,--list-directors",
      [&list_directors, &test_config](bool) {
        list_directors = true;
        test_config = true;
      },
      "List defined Directors.");

#if defined(HAVE_PAM)
  console_app
      .add_option(
          "-p,--pam-credentials-filename",
          [](std::vector<std::string> val) {
            pam_credentials_filename = val.front();
            if (FILE* f = fopen(pam_credentials_filename.c_str(), "r+")) {
              use_pam_credentials_file = true;
              fclose(f);
            } else { /* file cannot be opened, i.e. does not exist */
              Emsg0(M_ERROR_TERM, 0, T_("Could not open file for -p.\n"));
            }

            return true;
          },
          "PAM Credentials file.")
      ->check(CLI::ExistingFile)
      ->type_name("<path>");

  console_app.add_flag("-o", force_send_pam_credentials_unencrypted,
                       "Force sending pam credentials unencrypted.");

#endif /* HAVE_PAM */

  bool no_signals = false;
  console_app.add_flag("-s,--no-signals", no_signals,
                       "No signals (for debugging)");

  console_app.add_flag("-t,--test-config", test_config,
                       "Test - read configuration and exit");

  console_app
      .add_option("-u,--timeout", timeout,
                  "Set command execution timeout to <seconds>.")
      ->type_name("<seconds>")
      ->check(CLI::PositiveNumber);

  bool export_config = false;
  CLI::Option* xc
      = console_app.add_flag("--xc,--export-config", export_config,
                             "Print configuration resources and exit");

  bool export_config_schema = false;
  console_app
      .add_flag("--xs,--export-schema", export_config_schema,
                "Print configuration schema in JSON format and exit")
      ->excludes(xc);

  AddDeprecatedExportOptionsHelp(console_app);

  ParseBareosApp(console_app, argc, argv);

  if (!no_signals) { InitSignals(TerminateConsole); }

#if !defined(HAVE_WIN32)
  /* Override Bareos default signals */
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, GotSigstop);
  signal(SIGCONT, GotSigcontinue);
  signal(SIGTTIN, GotSigtin);
  signal(SIGTTOU, GotSigtout);
  trapctlc();
#endif

  OSDependentInit();

  if (export_config_schema) {
    PoolMem buffer;

    my_config = InitConsConfig(configfile, M_CONFIG_ERROR);
    PrintConfigSchemaJson(buffer);
    printf("%s\n", buffer.c_str());
    exit(BEXIT_SUCCESS);
  }

  my_config = InitConsConfig(configfile, M_CONFIG_ERROR);
  my_config->ParseConfigOrExit();

  if (export_config) {
    my_config->DumpResources(PrintMessage, NULL);
    TerminateConsole(BEXIT_SUCCESS);
    exit(BEXIT_SUCCESS);
  }

  if (InitCrypto() != 0) {
    Emsg0(M_ERROR_TERM, 0, T_("Cryptography library initialization failed.\n"));
  }

  if (!CheckResources()) {
    Emsg1(M_ERROR_TERM, 0, T_("Please correct configuration file: %s\n"),
          my_config->get_base_config_path().c_str());
  }

  ConInit(stdin);

  if (list_directors) {
    foreach_res (director_resource, R_DIRECTOR) {
      ConsoleOutputFormat("%s\n", director_resource->resource_name_);
    }
  }

  if (test_config) {
    TerminateConsole(BEXIT_SUCCESS);
    exit(BEXIT_SUCCESS);
  }

  (void)WSA_Init(); /* Initialize Windows sockets */

  StartWatchdog(); /* Start socket watchdog */

  if (!SelectDirector(director, &director_resource, &console_resource)) {
    return 1;
  }

  ConsoleOutputFormat(T_("Connecting to Director %s:%d\n"),
                      director_resource->address, director_resource->DIRport);

  utime_t heart_beat;
  if (director_resource->heartbeat_interval) {
    heart_beat = director_resource->heartbeat_interval;
  } else if (console_resource) {
    heart_beat = console_resource->heartbeat_interval;
  } else {
    heart_beat = 0;
  }

  uint32_t response_id;
  BStringList response_args;

  JobControlRecord jcr;
  g_UA_sock = ConnectToDirector(jcr, heart_beat, response_args, response_id);
  if (!g_UA_sock) {
    ConsoleOutput(T_("Failed to connect to Director. Giving up.\n"));
    TerminateConsole(0);
    return 1;
  }

  g_UA_sock->OutputCipherMessageString(ConsoleOutput);

  if (response_id == kMessageIdPamRequired) {
#if defined(HAVE_PAM)
    if (!ExaminePamAuthentication(use_pam_credentials_file,
                                  pam_credentials_filename)) {
      ConsoleOutput(T_("PAM authentication failed. Giving up.\n"));
      TerminateConsole(0);
      return 1;
    }
    response_args.clear();
    if (!g_UA_sock->ReceiveAndEvaluateResponseMessage(response_id,
                                                      response_args)) {
      ConsoleOutput(T_("PAM authentication failed. Giving up.\n"));
      TerminateConsole(0);
      return 1;
    }
#else
    ConsoleOutput(
        T_("PAM authentication requested by Director, however this console "
           "does not have this feature. Giving up.\n"));
    TerminateConsole(0);
    return 1;
#endif /* HAVE_PAM */
  }    /* kMessageIdPamRequired */

  if (response_id == kMessageIdOk) {
    ConsoleOutput(response_args.JoinReadable().c_str());
    ConsoleOutput("\n");
  }

  response_args.clear();
  if (!g_UA_sock->ReceiveAndEvaluateResponseMessage(response_id,
                                                    response_args)) {
    Dmsg0(200, "Could not receive the response message\n");
    TerminateConsole(0);
    return 1;
  }

  if (response_id != kMessageIdInfoMessage) {
    Dmsg0(200, "Could not receive the response message\n");
    TerminateConsole(0);
    return 1;
  }
  response_args.PopFront();
  ConsoleOutput(response_args.JoinReadable().c_str());
  ConsoleOutput("\n");

  Dmsg0(40, "Opened connection with Director daemon\n");

  ConsoleOutput(T_("\nEnter a period (.) to cancel a command.\n"));

#if defined(HAVE_WIN32)
  char* env = getenv("USERPROFILE");
#else
  char* env = getenv("HOME");
#endif

  // Run commands in ~/.bconsolerc if any
  if (env) {
    FILE* fp;

    PmStrcpy(g_UA_sock->msg, env);
    PmStrcat(g_UA_sock->msg, "/.bconsolerc");
    fp = fopen(g_UA_sock->msg, "rb");
    if (fp) {
      ReadAndProcessInput(fp, g_UA_sock);
      fclose(fp);
    }
  }

  PoolMem history_file;
  if (me && me->history_file) {
    PmStrcpy(history_file, me->history_file);
    ConsoleInitHistory(history_file.c_str());
  } else {
    if (env) {
      PmStrcpy(history_file, env);
      PmStrcat(history_file, "/.bconsole_history");
      ConsoleInitHistory(history_file.c_str());
    } else {
      PmStrcpy(history_file, "");
    }
  }

  ReadAndProcessInput(stdin, g_UA_sock);

  if (g_UA_sock) {
    g_UA_sock->signal(BNET_TERMINATE); /* send EOF */
    g_UA_sock->close();
  }

  if (history_file.size()) { ConsoleUpdateHistory(history_file.c_str()); }

  clear_history();

  TerminateConsole(BEXIT_SUCCESS);
  return BEXIT_SUCCESS;
}

static void TerminateConsole(int sig)
{
  static bool already_here = false;

  if (already_here) { /* avoid recursive temination problems */
    exit(BEXIT_FAILURE);
  }
  already_here = true;
  StopWatchdog();
  delete my_config;
  my_config = NULL;
  CleanupCrypto();
  FreePoolMemory(g_args);
  ConTerm();
  WSACleanup(); /* Cleanup Windows sockets */

  if (sig != 0) { exit(BEXIT_FAILURE); }
  return;
}

static int CheckResources()
{
  bool OK = true;
  DirectorResource* director;

  ResLocker _{my_config};

  g_numdir = 0;
  foreach_res (director, R_DIRECTOR) { g_numdir++; }

  if (g_numdir == 0) {
    const std::string& configfile_name = my_config->get_base_config_path();
    Emsg1(M_FATAL, 0,
          T_("No Director resource defined in %s\n"
             "Without that I don't how to speak to the Director :-(\n"),
          configfile_name.c_str());
    OK = false;
  }

  me = (ConsoleResource*)my_config->GetNextRes(R_CONSOLE, NULL);
  my_config->own_resource_ = me;

  return OK;
}

/* @version */
static int Versioncmd(FILE*, BareosSocket*)
{
  ConsoleOutputFormat("Version: %s (%s) %s\n", kBareosVersionStrings.Full,
                      kBareosVersionStrings.Date,
                      kBareosVersionStrings.GetOsInfo());
  return 1;
}

/* @input <input-filename> */
static int InputCmd(FILE*, BareosSocket*)
{
  FILE* fd;

  if (g_argc > 2) {
    ConsoleOutput(T_("Too many arguments on input command.\n"));
    return 1;
  }
  if (g_argc == 1) {
    ConsoleOutput(T_("First argument to input command must be a filename.\n"));
    return 1;
  }
  fd = fopen(g_argk[1], "rb");
  if (!fd) {
    BErrNo be;
    ConsoleOutputFormat(T_("Cannot open file %s for input. ERR=%s\n"),
                        g_argk[1], be.bstrerror());
    return 1;
  }
  ReadAndProcessInput(fd, g_UA_sock);
  fclose(fd);
  return 1;
}

/* @tee <output-filename> */
/* Send output to both terminal and specified file */
static int TeeCmd(FILE* input, BareosSocket* UA_sock)
{
  EnableTeeOut();
  return DoOutputcmd(input, UA_sock);
}

/* @output <output-filename> */
/* Send output to specified "file" */
static int OutputCmd(FILE* input, BareosSocket* UA_sock)
{
  DisableTeeOut();
  return DoOutputcmd(input, UA_sock);
}

static int DoOutputcmd(FILE*, BareosSocket*)
{
  FILE* file;
  const char* mode = "a+b";

  if (g_argc > 3) {
    ConsoleOutput(T_("Too many arguments on output/tee command.\n"));
    return 1;
  }
  if (g_argc == 1) {
    CloseTeeFile();
    return 1;
  }
  if (g_argc == 3) { mode = g_argk[2]; }
  file = fopen(g_argk[1], mode);
  if (!file) {
    BErrNo be;
    ConsoleOutputFormat(T_("Cannot open file %s for output. ERR=%s\n"),
                        g_argk[1], be.bstrerror(errno));
    return 1;
  }
  SetTeeFile(file);
  return 1;
}

// @exec "some-command" [wait-seconds]
static int ExecCmd(FILE*, BareosSocket*)
{
  Bpipe* bpipe;
  char line[5000];
  int status;
  int wait = 0;

  if (g_argc > 3) {
    ConsoleOutput(
        T_("Too many arguments. Enclose command in double quotes.\n"));
    return 1;
  }
  if (g_argc == 3) { wait = atoi(g_argk[2]); }
  bpipe = OpenBpipe(g_argk[1], wait, "r");
  if (!bpipe) {
    BErrNo be;
    ConsoleOutputFormat(T_("Cannot popen(\"%s\", \"r\"): ERR=%s\n"), g_argk[1],
                        be.bstrerror(errno));
    return 1;
  }

  while (fgets(line, sizeof(line), bpipe->rfd)) {
    ConsoleOutputFormat("%s", line);
  }
  status = CloseBpipe(bpipe);
  if (status != 0) {
    BErrNo be;
    be.SetErrno(status);
    ConsoleOutputFormat(T_("Autochanger error: ERR=%s\n"), be.bstrerror());
  }
  return 1;
}

/* @echo xxx yyy */
static int EchoCmd(FILE*, BareosSocket*)
{
  for (int i = 1; i < g_argc; i++) { ConsoleOutputFormat("%s ", g_argk[i]); }
  ConsoleOutput("\n");
  return 1;
}

/* @quit */
static int QuitCmd(FILE*, BareosSocket*) { return 0; }

/* @help */
static int HelpCmd(FILE*, BareosSocket*)
{
  for (auto& command : commands) {
    ConsoleOutputFormat("  %-10s %s\n", command.key, command.help);
  }
  return 1;
}

/* @sleep secs */
static int SleepCmd(FILE*, BareosSocket*)
{
  if (g_argc > 1) { sleep(atoi(g_argk[1])); }
  return 1;
}

/* @time */
static int TimeCmd(FILE*, BareosSocket*)
{
  char sdt[50];

  bstrftimes(sdt, sizeof(sdt), time(NULL));
  ConsoleOutputFormat("%s\n", sdt);
  return 1;
}

std::optional<std::string> grpc_address;

static int GrpcCmd(FILE*, BareosSocket*)
{
  if (g_argc != 2) {
    ConsoleOutput("Usage: grpc <address>\n");
    return 1;
  }

  if (!g_argk[1]) {
    ConsoleOutput("Usage: grpc <address>\n");
    return 1;
  }

  grpc_address = g_argk[1];
  std::string buffer;
  buffer += "Setting grpc address to '";
  buffer += *grpc_address;
  buffer += "'\n";
  ConsoleOutput(buffer.c_str());

  return 1;
}

namespace {

template <size_t N> using char_arr = const char* [N];

template <size_t N> int find_string(const char* s, const char_arr<N>& arr)
{
  for (size_t i = 0; i < N; ++i) {
    if (strcasecmp(s, arr[i]) == 0) { return static_cast<int>(i); }
  }

  return -1;
}

std::optional<bareos::config::ResourceType> get_resource_type(const char* name)
{
  bareos::config::ResourceType types[] = {
      bareos::config::ResourceType::DIRECTOR,
      bareos::config::ResourceType::CLIENT,
      bareos::config::ResourceType::JOBDEFS,
      bareos::config::ResourceType::JOB,
      bareos::config::ResourceType::STORAGE,
      bareos::config::ResourceType::CATALOG,
      bareos::config::ResourceType::SCHEDULE,
      bareos::config::ResourceType::FILESET,
      bareos::config::ResourceType::POOL,
      bareos::config::ResourceType::MSGS,
      bareos::config::ResourceType::COUNTER,
      bareos::config::ResourceType::PROFILE,
      bareos::config::ResourceType::CONSOLE,
      bareos::config::ResourceType::USER,
      bareos::config::ResourceType::GRPC,
  };

  auto idx = find_string(name, {
                                   "DIRECTOR",
                                   "CLIENT",
                                   "JOBDEFS",
                                   "JOB",
                                   "STORAGE",
                                   "CATALOG",
                                   "SCHEDULE",
                                   "FILESET",
                                   "POOL",
                                   "MSGS",
                                   "COUNTER",
                                   "PROFILE",
                                   "CONSOLE",
                                   "USER",
                                   "GRPC",
                               });

  if (idx < 0) { return std::nullopt; }

  return types[idx];
}

struct Value {
  virtual bool set_from(std::string_view chars) = 0;
  virtual std::string_view printable() const = 0;
  virtual const char* type() const = 0;
  virtual bool is_set() const = 0;
  virtual void unset() = 0;
  virtual ~Value() = default;
};

struct StringValue : public Value {
  std::optional<std::string> content{};
  bool set_from(std::string_view chars) override
  {
    content = chars;
    return true;
  }

  const char* type() const override { return "string"; }

  std::string_view printable() const override
  {
    if (content) {
      return std::string_view{*content};
    } else {
      return {};
    }
  }

  bool is_set() const override { return content.has_value(); }

  void unset() override { content.reset(); }

  ~StringValue() override = default;
};

struct EnumValue : public Value {
  std::vector<std::string> possibilities;

  std::optional<size_t> index;

  std::vector<std::string>& options() { return possibilities; }

  bool set_from(std::string_view chars) override
  {
    for (size_t i = 0; i < possibilities.size(); ++i) {
      auto& opt = possibilities[i];
      if (opt.size() == chars.size()
          && strncasecmp(opt.c_str(), chars.data(), opt.size()) == 0) {
        index = i;
        return true;
      }
    }

    return false;
  }

  bool is_set() const override { return index.has_value(); }

  void unset() override { index.reset(); }

  const char* type() const override { return "enum"; }

  std::string_view printable() const override
  {
    if (index) {
      return possibilities[*index];
    } else {
      return {};
    }
  }

  ~EnumValue() override = default;
};

struct IntValue : public Value {
  std::optional<std::uint64_t> content{};
  std::string buffer;

  bool set_from(std::string_view chars) override
  {
    std::uint64_t val;
    auto result = std::from_chars(std::begin(chars), std::end(chars), val);

    if (result.ec != std::errc()) { return false; }

    content = val;
    buffer = std::to_string(*content);
    return true;
  }

  bool is_set() const override { return content.has_value(); }

  void unset() override
  {
    content.reset();
    buffer.clear();
  }


  const char* type() const override { return "int"; }

  std::string_view printable() const override
  {
    if (content) {
      return buffer;
    } else {
      return {};
    }
  }

  ~IntValue() override = default;
};

std::unique_ptr<Value> ValueOf(const bareos::config::SchemaValue& val)
{
  switch (val.type()) {
    case bareos::config::POS_INT:
    case bareos::config::NAT_INT:
      return std::make_unique<IntValue>();
    case bareos::config::STRING:
      return std::make_unique<StringValue>();
    case bareos::config::ENUM: {
      auto result = std::make_unique<EnumValue>();

      for (auto& opt : val.values()) { result->options().emplace_back(opt); }

      return result;
    }
    default:
      return nullptr;
  }
}

std::vector<std::unique_ptr<Value>> MakeDefaultValues(
    gsl::span<bareos::config::SchemaValue> schema)
{
  std::vector<std::unique_ptr<Value>> values;
  values.reserve(schema.size());
  for (auto entry : schema) {
    auto value = ValueOf(entry);
    if (value && entry.has_default_value()) {
      value->set_from(entry.default_value());
    }
    values.emplace_back(std::move(value));
  }
  return values;
}

template <char sep = '|', size_t... Ns> struct row {
  static constexpr auto col_count = sizeof...(Ns);

  static constexpr auto max_size = (Ns + ...) + 4 + 3 * (col_count - 1);

  std::array<char, max_size> bytes;

  static constexpr auto sizes = std::array{Ns...};

  constexpr row()
  {
    size_t current = 0;
    bytes[current++] = sep;
    bytes[current++] = ' ';

    for (size_t i = 0; i < col_count; ++i) {
      if (i != 0) {
        bytes[current++] = ' ';
        bytes[current++] = sep;
        bytes[current++] = ' ';
      }

      memset(&bytes[current], ' ', sizes[i]);

      current += sizes[i];
    }

    bytes[current++] = ' ';
    bytes[current++] = sep;
  }

  constexpr gsl::span<char> column(size_t col)
  {
    size_t offset = 2 + 3 * col;
    for (size_t i = 0; i < col; ++i) { offset += sizes[i]; }

    return gsl::span{bytes.data() + offset, sizes[col]};
  }

  const std::string_view data() const
  {
    return std::string_view{bytes.data(), bytes.size()};
  }
};

template <size_t... Ns> using simple_row = row<'|', Ns...>;

template <size_t... Ns> struct value_table {
  static constexpr auto col_count = sizeof...(Ns);

  static_assert(((Ns >= 3) && ...),
                "Table needs at least enough space to print '...'");

  using internal_row = simple_row<Ns...>;

  std::vector<simple_row<Ns...>> rows;

  std::array<gsl::span<char>, col_count> next_row()
  {
    auto& next_row = rows.emplace_back();

    std::array<gsl::span<char>, col_count> result;

    for (size_t i = 0; i < col_count; ++i) { result[i] = next_row.column(i); }

    return result;
  }

  std::string to_string()
  {
    row<'+', Ns...> delim{};

    for (size_t i = 0; i < sizeof...(Ns); ++i) {
      auto col = delim.column(i);

      memset(col.data(), '-', col.size());
    }

    // constexpr auto max_size = simple_row<Ns...>::max_size;

    // s.resize((max_size + 1) * (rows.size() + 2) - 1);

    // static_assert(delim.max_size == max_size);

    std::stringstream ss;

    ss << delim.data() << "\n";

    for (size_t i = 0; i < rows.size(); ++i) { ss << rows[i].data() << "\n"; }

    ss << delim.data() << "\n";

    return ss.str();
  }
};


void PrintValueTable(gsl::span<bareos::config::SchemaValue> schema,
                     gsl::span<std::unique_ptr<Value>> values)
{
  value_table<3, 25, 40> table;

  static_assert(decltype(table)::internal_row::max_size <= 80);

  for (size_t i = 0; i < schema.size(); ++i) {
    auto [num, name, value] = table.next_row();

    {
      std::strstream s{num.data(), static_cast<int>(num.size())};
      s << i;
    }
    {
      std::strstream s{name.data(), static_cast<int>(name.size())};
      s << (schema[i].is_required() ? '*' : '-') << schema[i].name();
    }
    if (values[i]) {
      auto printable = values[i]->printable();
      memcpy(value.data(), printable.data(),
             std::min(value.size(), printable.size()));
    } else {
      std::strstream s{value.data(), static_cast<int>(value.size())};
      s << "uhoh";
    }
  }

  auto s = table.to_string();

  ConsoleOutput(s.c_str());
  ConsoleOutput("\n");
  fflush(stdout);
}

bool CaseEq(std::string_view l, std::string_view r)
{
  if (l.size() != r.size()) { return false; }
  return strncasecmp(l.data(), r.data(), l.size()) == 0;
}
// returns if l is a prefix of r (modulo case)
bool CasePrefix(std::string_view l, std::string_view r)
{
  if (l.size() > r.size()) { return false; }
  return strncasecmp(l.data(), r.data(), l.size()) == 0;
}

enum class match_status
{
  NoMatch,
  Match,
  Error,
};

match_status GetIndexByName(
    std::string_view input,
    int* idx,
    const gsl::span<bareos::config::SchemaValue>& schema,
    std::string& errmsg)
{
  match_status status = match_status::NoMatch;

  int maybe_idx = -1;
  bool found_partial_match = false;


  for (size_t i = 0; i < schema.size(); ++i) {
    auto& entry = schema[i];
    if (CaseEq(input, entry.name())) {
      // we always allow exact matches
      *idx = i;
      return match_status::Match;
    } else if (CasePrefix(input, entry.name())) {
      if (found_partial_match) {
        maybe_idx = -1;  // do not accept a partial match if its not unique
        errmsg = "found no exact match for '";
        errmsg += input;
        errmsg += "' and partial matches are not unique";
        status = match_status::Error;
      } else {
        maybe_idx = i;
        found_partial_match = true;
        status = match_status::Match;
      }
    }
  }

  *idx = maybe_idx;
  return status;
}

match_status GetIndexByNumber(std::string_view input,
                              int* idx,
                              int max,
                              std::string& errmsg)
{
  int index;
  auto result
      = std::from_chars(input.data(), input.data() + input.size(), index);

  if (result.ec != std::errc() || result.ptr != input.data() + input.size()) {
    // if its not a number, we cannot match it
    return match_status::NoMatch;
  }

  if (index >= max || index < 0) {
    errmsg = "index '";
    errmsg += std::to_string(index);
    errmsg += "' is out of bounds";
    return match_status::Error;
  }

  *idx = index;
  return match_status::Match;
}

int GetIndex(std::string_view input,
             const gsl::span<bareos::config::SchemaValue>& schema,
             std::string& errmsg)
{
  int idx;

  switch (GetIndexByName(input, &idx, schema, errmsg)) {
    case match_status::Match:
      return idx;
    case match_status::Error:
      return -1;
    case match_status::NoMatch:
      // just continue
      break;
  }
  switch (GetIndexByNumber(input, &idx, schema.size(), errmsg)) {
    case match_status::Match:
      return idx;
    case match_status::Error:
      return -1;
    case match_status::NoMatch:
      // just continue
      break;
  }

  errmsg = "could not parse '";
  errmsg += input;
  errmsg += "'";

  return -1;
}

struct Cancel {};

struct Finish {};

struct Edit {
  int index;
};

struct Doc {
  int index;
};

struct Error {
  std::string message;
};

using Action = std::variant<Cancel, Finish, Edit, Doc, Error>;

std::string_view trim(std::string_view v)
{
  auto start = v.find_first_not_of(" \t\v\n");
  if (start == v.npos) { return {}; }
  auto end = v.find_last_not_of(" \t\v\n");

  return v.substr(start, end - start + 1);
}

Action GetNextAction(FILE* input, gsl::span<bareos::config::SchemaValue> schema)
{
  const char* prompt = "*";
start:
  auto line = GetInputLine(input, prompt);
  if (!line) { return Cancel{}; }

  auto view = std::string_view{*line};

  auto word_start = view.find_first_not_of(" \t\v\n");
  if (word_start == view.npos) {
    prompt = "";
    goto start;
  }

  auto word_end = view.find_first_of(" \t\v\n", word_start);
  if (word_end == view.npos) { word_end = view.size(); }

  auto command = view.substr(word_start, word_end - word_start);
  auto rest = view.substr(word_end);
  auto trimmed = trim(rest);

  if (CasePrefix(command, "cancel") || CasePrefix(command, ".")) {
    return Cancel{};
  } else if (CasePrefix(command, "finish") || CasePrefix(command, "done")) {
    return Finish{};
  } else if (CasePrefix(command, "edit")) {
    std::string errmsg;

    auto idx = GetIndex(trimmed, schema, errmsg);
    if (idx < 0) { return Error{std::move(errmsg)}; }

    return Edit{idx};
  } else if (CasePrefix(command, "doc") || CasePrefix(command, "help")) {
    std::string errmsg;
    auto idx = GetIndex(trimmed, schema, errmsg);
    if (idx < 0) { return Error{std::move(errmsg)}; }

    return Doc{idx};
  } else {
    std::string msg;
    msg += "Unknown command: '";
    msg += command;
    msg += "'";
    return Error{std::move(msg)};
  }
}

std::optional<std::vector<std::unique_ptr<Value>>> EditValues(
    FILE* input,
    gsl::span<bareos::config::SchemaValue> schema)
{
  auto values = MakeDefaultValues(schema);

  std::string notification{};

  auto old_console_state = console_state;

  for (;;) {
    PrintValueTable(schema, values);
    if (notification.size()) {
      ConsoleOutput(notification.c_str());
      ConsoleOutput("\n");
    }
    notification.clear();

    auto action = GetNextAction(input, schema);

    bool cancel = false;
    bool finish = false;
    std::visit(
        [&](auto&& a) {
          using T = std::decay_t<decltype(a)>;
          if constexpr (std::is_same_v<T, Cancel>) {
            cancel = true;
          } else if constexpr (std::is_same_v<T, Finish>) {
            finish = true;
          } else if constexpr (std::is_same_v<T, Doc>) {
            auto& schema_entry = schema[a.index];
            if (schema_entry.has_description()) {
              std::string out;
              out += schema_entry.name();
              out += ": ";
              out += schema_entry.description();
              out += "\n";
              notification = std::move(out);
            } else {
              std::string out;
              out += "Option '";
              out += schema_entry.name();
              out += "' has not been documented yet.\n";
              notification = std::move(out);
            }
          } else if constexpr (std::is_same_v<T, Edit>) {
            auto& schema_entry = schema[a.index];
            auto& value = values[a.index];

            current_value = &schema_entry;

            std::string prompt = schema_entry.name() + ": ";

            console_state = CONFIG_EDIT;
            auto line = GetInputLine(input, prompt.c_str());
            if (!line) { return; }

            // remove trailing '\n'
            if (line->back() == '\n') line->pop_back();

            if (!value->set_from(*line)) {
              notification = "Could not set ";
              notification += schema_entry.name();
              notification += " to ";
              notification += *line;
            }
            console_state = old_console_state;
          } else if constexpr (std::is_same_v<T, Error>) {
            notification = std::move(a.message);
          } else {
            static_assert("Type not handled\n");
          }
        },
        action);

    if (cancel) {
      return std::nullopt;
    } else if (finish) {
      return values;
    }
  }

  return std::nullopt;
}

bool ConfigAdd(FILE* input,
               int argc,
               const char* const*,
               const char* const argk[])
{
  if (argc != 1) {
    ConsoleOutput("missing argument\n");
    return false;
  }

  auto type = get_resource_type(argk[0]);

  if (!type) {
    ConsoleOutput("unknown resource type\n");
    return false;
  }

  if (!grpc_address) {
    ConsoleOutput("grpc address not yet set\n");
    return 1;
  }
  auto channel = grpc::CreateChannel(grpc_address->c_str(),
                                     grpc::InsecureChannelCredentials());

  using namespace bareos::config;

  auto stub = Config::NewStub(channel);


  SchemaRequest request;
  request.set_type(*type);
  SchemaResponse response;

  {
    // dont reuse client context!
    grpc::ClientContext ctx;
    if (auto status = stub->Schema(&ctx, request, &response); !status.ok()) {
      char buffer[100];
      snprintf(buffer, sizeof(buffer), "Got Error %d: %s\n",
               status.error_code(), status.error_message().c_str());
      ConsoleOutput(buffer);
      fflush(stdout);
      return 1;
    }
  }

  std::vector<SchemaValue> schema;
  for (auto value : response.schema()) { schema.push_back(value); }

  std::stable_sort(std::begin(schema), std::end(schema), [](auto& l, auto& r) {
    // required < not required

    bool lr = l.is_required();
    bool rr = r.is_required();

    if (lr == rr) { return false; }

    if (lr) {
      // so rr = false
      return true;
    }

    return false;
  });

  auto values = EditValues(input, schema);

  if (!values) { return false; }

  {
    // todo: send config to dir here
  }

  return true;
}
};  // namespace

static int ConfigCmd(FILE* input, BareosSocket*)
{
  if (g_argc < 2) {
    ConsoleOutput("Usage: config <cmd> <args>...\n");
    fflush(stdout);
    return 1;
  }

  if (g_argk[1] && bstrcmp(g_argk[1], "add")) {
    if (ConfigAdd(input, g_argc - 2, g_argv + 2, g_argk + 2)) {
      ConsoleOutput("Command succeded\n");
    } else {
      ConsoleOutput("Command failed\n");
    }
  } else {
    ConsoleOutput("unknown command\n");
  }

  return 1;
}
