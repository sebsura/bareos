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
// Kern Sibbald, May MM
/**
 * @file
 * Configuration parser for Director Run Configuration
 * directives, which are part of the Schedule Resource
 */

#include "include/bareos.h"
#include "dird.h"
#include "dird/dird_globals.h"
#include "lib/edit.h"
#include "lib/keyword_table_s.h"
#include "lib/parse_conf.h"
#include "lib/util.h"

namespace directordaemon {

extern struct s_jl joblevels[];

// Forward referenced subroutines
enum e_state
{
  s_none = 0,
  s_range,
  s_mday,
  s_month,
  s_time,
  s_at,
  s_wday,
  s_daily,
  s_weekly,
  s_monthly,
  s_hourly,
  s_wom,   /* 1st, 2nd, ...*/
  s_woy,   /* week of year w00 - w53 */
  s_last,  /* last week of month */
  s_modulo /* every nth monthday/week */
};

struct s_keyw {
  const char* name;   /* keyword */
  enum e_state state; /* parser state */
  int code;           /* state value */
};

// Keywords understood by parser
static struct s_keyw keyw[] = {{NT_("on"), s_none, 0},
                               {NT_("at"), s_at, 0},
                               {NT_("last"), s_last, 0},
                               {NT_("sun"), s_wday, 0},
                               {NT_("mon"), s_wday, 1},
                               {NT_("tue"), s_wday, 2},
                               {NT_("wed"), s_wday, 3},
                               {NT_("thu"), s_wday, 4},
                               {NT_("fri"), s_wday, 5},
                               {NT_("sat"), s_wday, 6},
                               {NT_("jan"), s_month, 0},
                               {NT_("feb"), s_month, 1},
                               {NT_("mar"), s_month, 2},
                               {NT_("apr"), s_month, 3},
                               {NT_("may"), s_month, 4},
                               {NT_("jun"), s_month, 5},
                               {NT_("jul"), s_month, 6},
                               {NT_("aug"), s_month, 7},
                               {NT_("sep"), s_month, 8},
                               {NT_("oct"), s_month, 9},
                               {NT_("nov"), s_month, 10},
                               {NT_("dec"), s_month, 11},
                               {NT_("sunday"), s_wday, 0},
                               {NT_("monday"), s_wday, 1},
                               {NT_("tuesday"), s_wday, 2},
                               {NT_("wednesday"), s_wday, 3},
                               {NT_("thursday"), s_wday, 4},
                               {NT_("friday"), s_wday, 5},
                               {NT_("saturday"), s_wday, 6},
                               {NT_("january"), s_month, 0},
                               {NT_("february"), s_month, 1},
                               {NT_("march"), s_month, 2},
                               {NT_("april"), s_month, 3},
                               {NT_("june"), s_month, 5},
                               {NT_("july"), s_month, 6},
                               {NT_("august"), s_month, 7},
                               {NT_("september"), s_month, 8},
                               {NT_("october"), s_month, 9},
                               {NT_("november"), s_month, 10},
                               {NT_("december"), s_month, 11},
                               {NT_("daily"), s_daily, 0},
                               {NT_("weekly"), s_weekly, 0},
                               {NT_("monthly"), s_monthly, 0},
                               {NT_("hourly"), s_hourly, 0},
                               {NT_("1st"), s_wom, 0},
                               {NT_("2nd"), s_wom, 1},
                               {NT_("3rd"), s_wom, 2},
                               {NT_("4th"), s_wom, 3},
                               {NT_("5th"), s_wom, 4},
                               {NT_("first"), s_wom, 0},
                               {NT_("second"), s_wom, 1},
                               {NT_("third"), s_wom, 2},
                               {NT_("fourth"), s_wom, 3},
                               {NT_("fifth"), s_wom, 4},
                               {NULL, s_none, 0}};

// Keywords (RHS) permitted in Run records
struct s_kw RunFields[] = {{"pool", 'P'},
                           {"fullpool", 'f'},
                           {"incrementalpool", 'i'},
                           {"differentialpool", 'd'},
                           {"nextpool", 'n'},
                           {"level", 'L'},
                           {"storage", 'S'},
                           {"messages", 'M'},
                           {"priority", 'p'},
                           {"spooldata", 's'},
                           {"maxrunschedtime", 'm'},
                           {"accurate", 'a'},
                           {NULL, 0}};

struct ScheduleParserState {
  bool have_hour = false;
  bool have_mday = false;
  bool have_wday = false;
  bool have_month = false;
  bool have_wom = false;
  bool have_at = false;
  bool have_woy = false;
  uint32_t minute;

  DateTimeBitfield dt;

  ScheduleParserState()
  {
    SetBitRange(0, 23, dt.hour);
    SetBitRange(0, 30, dt.mday);
    SetBitRange(0, 6, dt.wday);
    SetBitRange(0, 11, dt.month);
    SetBitRange(0, 4, dt.wom);
    SetBitRange(0, 53, dt.woy);
  }
};

#if 0

static ResourceItem run_items[] = {
  { "Pool", CFG_TYPE_RES, ITEM(RunResource, pool), R_POOL, 0, NULL, NULL, nullptr },
  { "FullPool", CFG_TYPE_RES, ITEM(RunResource, full_pool), R_POOL, 0, NULL, NULL, nullptr },
  { "IncrementalPool", CFG_TYPE_RES, ITEM(RunResource, inc_pool), R_POOL, 0, NULL, NULL, nullptr },
  { "DifferentialPool", CFG_TYPE_RES, ITEM(RunResource, diff_pool), R_POOL, 0, NULL, NULL, nullptr },
  { "NextPool", CFG_TYPE_RES, ITEM(RunResource, next_pool), R_POOL, 0, NULL, NULL, nullptr },
  { "VirtualFullPool", CFG_TYPE_RES, ITEM(RunResource, vfull_pool), R_POOL, 0, NULL, NULL, nullptr },
  { "Level", CFG_TYPE_LEVEL, ITEM(RunResource, level), 0, 0, NULL, NULL, nullptr },
  { "Storage", CFG_TYPE_RES, ITEM(RunResource, storage), R_STORAGE, 0, NULL, NULL, nullptr },
  { "Messages", CFG_TYPE_RES, ITEM(RunResource, msgs), R_MSGS, 0, NULL, NULL, nullptr },
  { "Priority", CFG_TYPE_PINT32, ITEM(RunResource, Priority), 0, 0, NULL, NULL, "priority of job" },
  { "SpoolData", CFG_TYPE_BOOL, ITEM(RunResource, spool_data), 0, 0, NULL, NULL, nullptr },
  { "MaxRunSchedTime", CFG_TYPE_TIME, ITEM(RunResource, MaxRunSchedTime), 0, 0, NULL, NULL, nullptr },
  { "Accurate", CFG_TYPE_BOOL, ITEM(RunResource, accurate), 0, 0, NULL, NULL, "run with accurate" },
  { "Schedule", CFG_TYPE_SCHEDULE, ITEM(RunResource, date_time_bitfield), 0, 0, NULL, NULL, "run with accurate" },
  {}
};

extern void DirdConfigCb(ConfigurationParser* p,
                         BareosResource* res,
                         LEX* lc,
                         ResourceItem* item,
                         int index);

static void StoreSchedule(ConfigurationParser*,
                          BareosResource* res,
                          LEX* lc,
                          ResourceItem* item,
                          int index)
{
  auto* dt = GetItemVariablePointer<DateTimeBitfield*>(res, *item);

  /* Scan schedule times.
   * Default is: daily at 0:0 */
  ScheduleParserState st{};

  e_state state = s_none;
  e_state state2 = s_none;
  int code{0}, code2{0};

  for (int token = LexGetToken(lc, BCT_ALL);
       token != BCT_EOL;
       token = LexGetToken(lc, BCT_ALL)) {
    int len;
    bool pm = false;
    bool am = false;
    switch (token) {
      case BCT_NUMBER:
        state = s_mday;
        code = atoi(lc->str) - 1;
        if (code < 0 || code > 30) {
          scan_err0(lc, T_("Day number out of range (1-31)"));
          return;
        }
        break;
      case BCT_NAME: /* This handles drop through from keyword */
    case BCT_UNQUOTED_STRING: {
        if (strchr(lc->str, (int)'-')) {
          state = s_range;
          break;
        }
        if (strchr(lc->str, (int)':')) {
          state = s_time;
          break;
        }
        if (strchr(lc->str, (int)'/')) {
          state = s_modulo;
          break;
        }
        if (lc->str_len == 3 && (lc->str[0] == 'w' || lc->str[0] == 'W')
            && IsAnInteger(lc->str + 1)) {
          code = atoi(lc->str + 1);
          if (code < 0 || code > 53) {
            scan_err0(lc, T_("Week number out of range (0-53)"));
            return;
          }
          state = s_woy; /* Week of year */
          break;
        }
        // Everything else must be a keyword
        bool found = false;
        for (int i = 0; keyw[i].name; i++) {
          if (Bstrcasecmp(lc->str, keyw[i].name)) {
            state = keyw[i].state;
            code = keyw[i].code;
            found = true;
            break;
          }
        }
        if (!found) {
          scan_err1(lc, T_("Job type field: %s in run record not found"),
                    lc->str);
          return;
        }
    } break;
      case BCT_COMMA:
        continue;
      default:
        scan_err2(lc, T_("Unexpected token: %d:%s"), token, lc->str);
        return;
        break;
    }
    switch (state) {
      case s_none:
        continue;
      case s_mday: /* Day of month */
        if (!st.have_mday) {
          ClearBitRange(0, 30, st.dt.mday);
          st.have_mday = true;
        }
        SetBit(code, st.dt.mday);
        break;
      case s_month: /* Month of year */
        if (!st.have_month) {
          ClearBitRange(0, 11, st.dt.month);
          st.have_month = true;
        }
        SetBit(code, st.dt.month);
        break;
      case s_wday: /* Week day */
        if (!st.have_wday) {
          ClearBitRange(0, 6, st.dt.wday);
          st.have_wday = true;
        }
        SetBit(code, st.dt.wday);
        break;
      case s_wom: /* Week of month 1st, ... */
        if (!st.have_wom) {
          ClearBitRange(0, 4, st.dt.wom);
          st.have_wom = true;
        }
        SetBit(code, st.dt.wom);
        break;
      case s_woy:
        if (!st.have_woy) {
          ClearBitRange(0, 53, st.dt.woy);
          st.have_woy = true;
        }
        SetBit(code, st.dt.woy);
        break;
    case s_time: /* Time */ {
        if (!st.have_at) {
          scan_err0(lc, T_("Time must be preceded by keyword AT."));
          return;
        }
        if (!st.have_hour) {
          ClearBitRange(0, 23, st.dt.hour);
        }
        //       Dmsg1(000, "s_time=%s\n", lc->str);
        char* p = strchr(lc->str, ':');
        if (!p) {
          scan_err0(lc, T_("Time logic error.\n"));
          return;
        }
        *p++ = 0;             /* Separate two halves */
        code = atoi(lc->str); /* Pick up hour */
        code2 = atoi(p);      /* Pick up minutes */
        len = strlen(p);
        if (len >= 2) { p += 2; }
        if (Bstrcasecmp(p, "pm")) {
          pm = true;
        } else if (Bstrcasecmp(p, "am")) {
          am = true;
        } else if (len != 2) {
          scan_err0(lc, T_("Bad time specification."));
          return;
        }
        /* Note, according to NIST, 12am and 12pm are ambiguous and
         *  can be defined to anything.  However, 12:01am is the same
         *  as 00:01 and 12:01pm is the same as 12:01, so we define
         *  12am as 00:00 and 12pm as 12:00. */
        if (pm) {
          // Convert to 24 hour time
          if (code != 12) { code += 12; }
        } else if (am && code == 12) {
          // AM
          code -= 12;
        }
        if (code < 0 || code > 23 || code2 < 0 || code2 > 59) {
          scan_err0(lc, T_("Bad time specification."));
          return;
        }
        SetBit(code, st.dt.hour);
        st.minute = code2;
        st.have_hour = true;
    } break;
      case s_at:
        st.have_at = true;
        break;
      case s_last:
        st.dt.last_week_of_month = true;
        if (!st.have_wom) {
          ClearBitRange(0, 4, st.dt.wom);
          st.have_wom = true;
        }
        break;
    case s_modulo: {
        char* p = strchr(lc->str, '/');
        if (!p) {
          scan_err0(lc, T_("Modulo logic error.\n"));
          return;
        }
        *p++ = 0; /* Separate two halves */

        if (IsAnInteger(lc->str) && IsAnInteger(p)) {
          // Check for day modulo specification.
          code = atoi(lc->str) - 1;
          code2 = atoi(p);
          if (code < 0 || code > 30 || code2 < 0 || code2 > 30) {
            scan_err0(lc, T_("Bad day specification in modulo."));
            return;
          }
          if (code > code2) {
            scan_err0(lc, T_("Bad day specification, offset must always be <= "
                             "than modulo."));
            return;
          }
          if (!st.have_mday) {
            ClearBitRange(0, 30, st.dt.mday);
            st.have_mday = true;
          }
          // Set the bits according to the modulo specification.
          for (int i = 0; i < 31; i++) {
            if (i % code2 == 0) {
              SetBit(i + code, st.dt.mday);
            }
          }
        } else if (strlen(lc->str) == 3 && strlen(p) == 3
                   && (lc->str[0] == 'w' || lc->str[0] == 'W')
                   && (p[0] == 'w' || p[0] == 'W') && IsAnInteger(lc->str + 1)
                   && IsAnInteger(p + 1)) {
          // Check for week modulo specification.
          code = atoi(lc->str + 1);
          code2 = atoi(p + 1);
          if (code < 0 || code > 53 || code2 < 0 || code2 > 53) {
            scan_err0(lc, T_("Week number out of range (0-53) in modulo"));
            return;
          }
          if (code > code2) {
            scan_err0(lc, T_("Bad week number specification in modulo, offset "
                             "must always be <= than modulo."));
            return;
          }
          if (!st.have_woy) {
            ClearBitRange(0, 53, st.dt.woy);
            st.have_woy = true;
          }
          // Set the bits according to the modulo specification.
          for (int i = 0; i < 53; i++) {
            if (i % code2 == 0) {
              SetBit(i + code, st.dt.woy);
            }
          }
        } else {
          scan_err0(lc, T_("Bad modulo time specification. Format for weekdays "
                           "is '01/02', for yearweeks is 'w01/w02'."));
          return;
        }
    } break;
    case s_range: {
        char* p = strchr(lc->str, '-');
        if (!p) {
          scan_err0(lc, T_("Range logic error.\n"));
          return;
        }
        *p++ = 0; /* Separate two halves */

        if (IsAnInteger(lc->str) && IsAnInteger(p)) {
          // Check for day range.
          code = atoi(lc->str) - 1;
          code2 = atoi(p) - 1;
          if (code < 0 || code > 30 || code2 < 0 || code2 > 30) {
            scan_err0(lc, T_("Bad day range specification."));
            return;
          }
          if (!st.have_mday) {
            ClearBitRange(0, 30, st.dt.mday);
            st.have_mday = true;
          }
          if (code < code2) {
            SetBitRange(code, code2, st.dt.mday);
          } else {
            SetBitRange(code, 30, st.dt.mday);
            SetBitRange(0, code2, st.dt.mday);
          }
        } else if (strlen(lc->str) == 3 && strlen(p) == 3
                   && (lc->str[0] == 'w' || lc->str[0] == 'W')
                   && (p[0] == 'w' || p[0] == 'W') && IsAnInteger(lc->str + 1)
                   && IsAnInteger(p + 1)) {
          // Check for week of year range.
          code = atoi(lc->str + 1);
          code2 = atoi(p + 1);
          if (code < 0 || code > 53 || code2 < 0 || code2 > 53) {
            scan_err0(lc, T_("Week number out of range (0-53)"));
            return;
          }
          if (!st.have_woy) {
            ClearBitRange(0, 53, st.dt.woy);
            st.have_woy = true;
          }
          if (code < code2) {
            SetBitRange(code, code2, st.dt.woy);
          } else {
            SetBitRange(code, 53, st.dt.woy);
            SetBitRange(0, code2, st.dt.woy);
          }
        } else {
          // lookup first half of keyword range (week days or months).
          lcase(lc->str);
          bool found = false;
          for (int i = 0; keyw[i].name; i++) {
            if (bstrcmp(lc->str, keyw[i].name)) {
              state = keyw[i].state;
              code = keyw[i].code;
              found = true;
              break;
            }
          }
          if (!found
              || (state != s_month && state != s_wday && state != s_wom)) {
            scan_err0(lc, T_("Invalid month, week or position day range"));
            return;
          }

          // Lookup end of range.
          lcase(p);
          found = false;
          for (int i = 0; keyw[i].name; i++) {
            if (bstrcmp(p, keyw[i].name)) {
              state2 = keyw[i].state;
              code2 = keyw[i].code;
              found = true;
              break;
            }
          }
          if (!found || state != state2 || code == code2) {
            scan_err0(lc, T_("Invalid month, weekday or position range"));
            return;
          }
          if (state == s_wday) {
            if (!st.have_wday) {
              ClearBitRange(0, 6, st.dt.wday);
              st.have_wday = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.wday);
            } else {
              SetBitRange(code, 6, st.dt.wday);
              SetBitRange(0, code2, st.dt.wday);
            }
          } else if (state == s_month) {
            if (!st.have_month) {
              ClearBitRange(0, 11, st.dt.month);
              st.have_month = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.month);
            } else {
              // This is a bit odd, but we accept it anyway
              SetBitRange(code, 11, st.dt.month);
              SetBitRange(0, code2, st.dt.month);
            }
          } else {
            // Must be position
            if (!st.have_wom) {
              ClearBitRange(0, 4, st.dt.wom);
              st.have_wom = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.wom);
            } else {
              SetBitRange(code, 4, st.dt.wom);
              SetBitRange(0, code2, st.dt.wom);
            }
          }
        }
    } break;
      case s_hourly:
        st.have_hour = true;
        SetBitRange(0, 23, st.dt.hour);
        break;
      case s_weekly:
        st.have_mday = st.have_wom = st.have_woy = true;
        SetBitRange(0, 30, st.dt.mday);
        SetBitRange(0, 4, st.dt.wom);
        SetBitRange(0, 53, st.dt.woy);
        break;
      case s_daily:
        st.have_mday = true;
        SetBitRange(0, 6, st.dt.wday);
        break;
      case s_monthly:
        st.have_month = true;
        SetBitRange(0, 11, st.dt.month);
        break;
      default:
        scan_err0(lc, T_("Unexpected run state\n"));
        return;
        break;
    }
  }


  *dt = st.dt;
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
}

static void RunConfigCb(ConfigurationParser* p,
                         BareosResource* res,
                         LEX* lc,
                         ResourceItem* item,
                         int index)
{
  switch (item->type) {
  case CFG_TYPE_SCHEDULE: {
    StoreSchedule(p, res, lc, item, index);
  } break;
  default: {
    DirdConfigCb(p, res, lc, item, index);
  } break;
  }
}
#endif

static void AppendRunResource(RunResource** head_loc, RunResource* next)
{
  if (!*head_loc) {
    *head_loc = next;
  } else {
    auto* ptr = *head_loc;
    while (ptr->next) { ptr = ptr->next; }
    ptr->next = next;
  }
}

/**
 * Store Schedule Run information
 *
 * Parse Run statement:
 *
 *  Run <keyword=value ...> [on] 2 january at 23:45
 *
 *   Default Run time is daily at 0:0
 *
 *   There can be multiple run statements, they are simply chained
 *   together.
 *
 */
void StoreRun(ConfigurationParser* p,
              BareosResource* res,
              LEX* lc,
              ResourceItem* item,
              int index)
{
#if 0
    (void)index;

    RunResource** current = GetItemVariablePointer<RunResource**>(res, *item);

    RunResource* new_res = new RunResource();

    auto result = p->ParseResource(new_res, run_items, lc, RunConfigCb);

    if (!result) {
      scan_err2(lc, "Could not parse %s: %s\n",
                item->name, result.strerror());
      return;
    }

    AppendRunResource(current, new_res);
#else
  RunResource** current = GetItemVariablePointer<RunResource**>(res, *item);
  RunResource* new_res = new RunResource();

  /* MARKER */
  int options = lc->options;
  int token, state, state2 = 0, code = 0, code2 = 0;
  utime_t utime;

  lc->options |= LOPT_NO_IDENT; /* Want only "strings" */

  // Scan for Job level "full", "incremental", ...
  for (;;) {
    bool found = false;
    token = LexGetToken(lc, BCT_NAME);
    for (int i = 0; !found && RunFields[i].name; i++) {
      if (Bstrcasecmp(lc->str, RunFields[i].name)) {
        found = true;
        if (LexGetToken(lc, BCT_ALL) != BCT_EQUALS) {
          scan_err1(lc, T_("Expected an equals, got: %s"), lc->str);
          return;
        }
        switch (RunFields[i].token) {
          case 's': /* Data spooling */
            token = LexGetToken(lc, BCT_NAME);
            if (Bstrcasecmp(lc->str, "yes") || Bstrcasecmp(lc->str, "true")) {
              new_res->spool_data = true;
              new_res->spool_data_set = true;
            } else if (Bstrcasecmp(lc->str, "no")
                       || Bstrcasecmp(lc->str, "false")) {
              new_res->spool_data = false;
              new_res->spool_data_set = true;
            } else {
              scan_err1(lc, T_("Expect a YES or NO, got: %s"), lc->str);
              return;
            }
            break;
          case 'L': { /* Level */
            token = LexGetToken(lc, BCT_NAME);
            int j;
            for (j = 0; joblevels[j].level_name; j++) {
              if (Bstrcasecmp(lc->str, joblevels[j].level_name)) {
                new_res->level = joblevels[j].level;
                new_res->job_type = joblevels[j].job_type;
                j = -1;
                break;
              }
            }
            if (j >= 0) {
              scan_err1(lc, T_("Job level field: %s not found in run record"),
                        lc->str);
              return;
            }
          } break;
          case 'p': { /* Priority */
            token = LexGetToken(lc, BCT_PINT32);
            new_res->Priority = lc->u.pint32_val;
          } break;
          case 'P': {
            static ResourceItem pool_item{
                "Pool", CFG_TYPE_RES, ITEM(RunResource, pool), R_POOL, 0, NULL,
                NULL,   nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &pool_item,
                             lc->str);
          } break;
          case 'f': {
            static ResourceItem full_item{"FullPool",
                                          CFG_TYPE_RES,
                                          ITEM(RunResource, full_pool),
                                          R_POOL,
                                          0,
                                          NULL,
                                          NULL,
                                          nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &full_item,
                             lc->str);
          } break;
          case 'v': {
            static ResourceItem vpool_item{"VirtualFullPool",
                                           CFG_TYPE_RES,
                                           ITEM(RunResource, vfull_pool),
                                           R_POOL,
                                           0,
                                           NULL,
                                           NULL,
                                           nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res,
                             &vpool_item, lc->str);
          } break;
          case 'i': {
            static ResourceItem incr_item{"IncrementalPool",
                                          CFG_TYPE_RES,
                                          ITEM(RunResource, inc_pool),
                                          R_POOL,
                                          0,
                                          NULL,
                                          NULL,
                                          nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &incr_item,
                             lc->str);
          } break;
          case 'd': {
            static ResourceItem diff_item{"DifferentialPool",
                                          CFG_TYPE_RES,
                                          ITEM(RunResource, diff_pool),
                                          R_POOL,
                                          0,
                                          NULL,
                                          NULL,
                                          nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &diff_item,
                             lc->str);
          } break;
          case 'n': {
            static ResourceItem next_item{"NextPool",
                                          CFG_TYPE_RES,
                                          ITEM(RunResource, next_pool),
                                          R_POOL,
                                          0,
                                          NULL,
                                          NULL,
                                          nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &next_item,
                             lc->str);
          } break;
          case 'S': /* Storage */ {
            static ResourceItem storage_item{"Storage",
                                             CFG_TYPE_RES,
                                             ITEM(RunResource, storage),
                                             R_STORAGE,
                                             0,
                                             NULL,
                                             NULL,
                                             nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res,
                             &storage_item, lc->str);
          } break;
          case 'M': /* Messages */ {
            static ResourceItem msgs_item{"Messages",
                                          CFG_TYPE_RES,
                                          ITEM(RunResource, msgs),
                                          R_MSGS,
                                          0,
                                          NULL,
                                          NULL,
                                          nullptr};
            token = LexGetToken(lc, BCT_NAME);
            p->AddDependency(DependencyStorageType::SINGLE, new_res, &msgs_item,
                             lc->str);
          } break;
          case 'm': /* Max run sched time */
            token = LexGetToken(lc, BCT_QUOTED_STRING);
            if (!DurationToUtime(lc->str, &utime)) {
              scan_err1(lc, T_("expected a time period, got: %s"), lc->str);
              return;
            }
            new_res->MaxRunSchedTime = utime;
            new_res->MaxRunSchedTime_set = true;
            break;
          case 'a': /* Accurate */
            token = LexGetToken(lc, BCT_NAME);
            if (strcasecmp(lc->str, "yes") == 0
                || strcasecmp(lc->str, "true") == 0) {
              new_res->accurate = true;
              new_res->accurate_set = true;
            } else if (strcasecmp(lc->str, "no") == 0
                       || strcasecmp(lc->str, "false") == 0) {
              new_res->accurate = false;
              new_res->accurate_set = true;
            } else {
              scan_err1(lc, T_("Expect a YES or NO, got: %s"), lc->str);
            }
            break;
          default:
            scan_err1(lc, T_("Expected a keyword name, got: %s"), lc->str);
            return;
            break;
        } /* end switch */
      }   /* end if Bstrcasecmp */
    }     /* end for RunFields */

    /* At this point, it is not a keyword. Check for old syle
     * Job Levels without keyword. This form is depreciated!!! */
    if (!found) {
      for (int j = 0; joblevels[j].level_name; j++) {
        if (Bstrcasecmp(lc->str, joblevels[j].level_name)) {
          new_res->level = joblevels[j].level;
          new_res->job_type = joblevels[j].job_type;
          found = true;
          break;
        }
      }
    }

    if (!found) { break; }
  } /* end for found */

  /* Scan schedule times.
   * Default is: daily at 0:0 */
  state = s_none;
  ScheduleParserState st;

  for (; token != BCT_EOL; (token = LexGetToken(lc, BCT_ALL))) {
    int len;
    bool pm = false;
    bool am = false;
    switch (token) {
      case BCT_NUMBER:
        state = s_mday;
        code = atoi(lc->str) - 1;
        if (code < 0 || code > 30) {
          scan_err0(lc, T_("Day number out of range (1-31)"));
          return;
        }
        break;
      case BCT_NAME: /* This handles drop through from keyword */
      case BCT_UNQUOTED_STRING: {
        if (strchr(lc->str, (int)'-')) {
          state = s_range;
          break;
        }
        if (strchr(lc->str, (int)':')) {
          state = s_time;
          break;
        }
        if (strchr(lc->str, (int)'/')) {
          state = s_modulo;
          break;
        }
        if (lc->str_len == 3 && (lc->str[0] == 'w' || lc->str[0] == 'W')
            && IsAnInteger(lc->str + 1)) {
          code = atoi(lc->str + 1);
          if (code < 0 || code > 53) {
            scan_err0(lc, T_("Week number out of range (0-53)"));
            return;
          }
          state = s_woy; /* Week of year */
          break;
        }
        bool found = false;
        // Everything else must be a keyword
        for (int i = 0; keyw[i].name; i++) {
          if (Bstrcasecmp(lc->str, keyw[i].name)) {
            state = keyw[i].state;
            code = keyw[i].code;
            found = true;
            break;
          }
        }
        if (!found) {
          scan_err1(lc, T_("Job type field: %s in run record not found"),
                    lc->str);
          return;
        }
      } break;
      case BCT_COMMA:
        continue;
      default:
        scan_err2(lc, T_("Unexpected token: %d:%s"), token, lc->str);
        return;
        break;
    }
    switch (state) {
      case s_none:
        continue;
      case s_mday: /* Day of month */
        if (!st.have_mday) {
          ClearBitRange(0, 30, st.dt.mday);
          st.have_mday = true;
        }
        SetBit(code, st.dt.mday);
        break;
      case s_month: /* Month of year */
        if (!st.have_month) {
          ClearBitRange(0, 11, st.dt.month);
          st.have_month = true;
        }
        SetBit(code, st.dt.month);
        break;
      case s_wday: /* Week day */
        if (!st.have_wday) {
          ClearBitRange(0, 6, st.dt.wday);
          st.have_wday = true;
        }
        SetBit(code, st.dt.wday);
        break;
      case s_wom: /* Week of month 1st, ... */
        if (!st.have_wom) {
          ClearBitRange(0, 4, st.dt.wom);
          st.have_wom = true;
        }
        SetBit(code, st.dt.wom);
        break;
      case s_woy:
        if (!st.have_woy) {
          ClearBitRange(0, 53, st.dt.woy);
          st.have_woy = true;
        }
        SetBit(code, st.dt.woy);
        break;
      case s_time: { /* Time */
        if (!st.have_at) {
          scan_err0(lc, T_("Time must be preceded by keyword AT."));
          return;
        }
        if (!st.have_hour) { ClearBitRange(0, 23, st.dt.hour); }
        //       Dmsg1(000, "s_time=%s\n", lc->str);
        char* str = strchr(lc->str, ':');
        if (!str) {
          scan_err0(lc, T_("Time logic error.\n"));
          return;
        }
        *str++ = 0;           /* Separate two halves */
        code = atoi(lc->str); /* Pick up hour */
        code2 = atoi(str);    /* Pick up minutes */
        len = strlen(str);
        if (len >= 2) { str += 2; }
        if (Bstrcasecmp(str, "pm")) {
          pm = true;
        } else if (Bstrcasecmp(str, "am")) {
          am = true;
        } else if (len != 2) {
          scan_err0(lc, T_("Bad time specification."));
          return;
        }
        /* Note, according to NIST, 12am and 12pm are ambiguous and
         *  can be defined to anything.  However, 12:01am is the same
         *  as 00:01 and 12:01pm is the same as 12:01, so we define
         *  12am as 00:00 and 12pm as 12:00. */
        if (pm) {
          // Convert to 24 hour time
          if (code != 12) { code += 12; }
        } else if (am && code == 12) {
          // AM
          code -= 12;
        }
        if (code < 0 || code > 23 || code2 < 0 || code2 > 59) {
          scan_err0(lc, T_("Bad time specification."));
          return;
        }
        SetBit(code, st.dt.hour);
        st.minute = code2;
        st.have_hour = true;
      } break;
      case s_at:
        st.have_at = true;
        break;
      case s_last:
        st.dt.last_week_of_month = true;
        if (!st.have_wom) {
          ClearBitRange(0, 4, st.dt.wom);
          st.have_wom = true;
        }
        break;
      case s_modulo: {
        char* str = strchr(lc->str, '/');
        if (!str) {
          scan_err0(lc, T_("Modulo logic error.\n"));
          return;
        }
        *str++ = 0; /* Separate two halves */

        if (IsAnInteger(lc->str) && IsAnInteger(str)) {
          // Check for day modulo specification.
          code = atoi(lc->str) - 1;
          code2 = atoi(str);
          if (code < 0 || code > 30 || code2 < 0 || code2 > 30) {
            scan_err0(lc, T_("Bad day specification in modulo."));
            return;
          }
          if (code > code2) {
            scan_err0(lc, T_("Bad day specification, offset must always be <= "
                             "than modulo."));
            return;
          }
          if (!st.have_mday) {
            ClearBitRange(0, 30, st.dt.mday);
            st.have_mday = true;
          }
          // Set the bits according to the modulo specification.
          for (int i = 0; i < 31; i++) {
            if (i % code2 == 0) { SetBit(i + code, st.dt.mday); }
          }
        } else if (strlen(lc->str) == 3 && strlen(str) == 3
                   && (lc->str[0] == 'w' || lc->str[0] == 'W')
                   && (str[0] == 'w' || str[0] == 'W')
                   && IsAnInteger(lc->str + 1) && IsAnInteger(str + 1)) {
          // Check for week modulo specification.
          code = atoi(lc->str + 1);
          code2 = atoi(str + 1);
          if (code < 0 || code > 53 || code2 < 0 || code2 > 53) {
            scan_err0(lc, T_("Week number out of range (0-53) in modulo"));
            return;
          }
          if (code > code2) {
            scan_err0(lc, T_("Bad week number specification in modulo, offset "
                             "must always be <= than modulo."));
            return;
          }
          if (!st.have_woy) {
            ClearBitRange(0, 53, st.dt.woy);
            st.have_woy = true;
          }
          // Set the bits according to the modulo specification.
          for (int i = 0; i < 53; i++) {
            if (i % code2 == 0) { SetBit(i + code, st.dt.woy); }
          }
        } else {
          scan_err0(lc, T_("Bad modulo time specification. Format for weekdays "
                           "is '01/02', for yearweeks is 'w01/w02'."));
          return;
        }
      } break;
      case s_range: {
        char* str = strchr(lc->str, '-');
        if (!str) {
          scan_err0(lc, T_("Range logic error.\n"));
          return;
        }
        *str++ = 0; /* Separate two halves */

        if (IsAnInteger(lc->str) && IsAnInteger(str)) {
          // Check for day range.
          code = atoi(lc->str) - 1;
          code2 = atoi(str) - 1;
          if (code < 0 || code > 30 || code2 < 0 || code2 > 30) {
            scan_err0(lc, T_("Bad day range specification."));
            return;
          }
          if (!st.have_mday) {
            ClearBitRange(0, 30, st.dt.mday);
            st.have_mday = true;
          }
          if (code < code2) {
            SetBitRange(code, code2, st.dt.mday);
          } else {
            SetBitRange(code, 30, st.dt.mday);
            SetBitRange(0, code2, st.dt.mday);
          }
        } else if (strlen(lc->str) == 3 && strlen(str) == 3
                   && (lc->str[0] == 'w' || lc->str[0] == 'W')
                   && (str[0] == 'w' || str[0] == 'W')
                   && IsAnInteger(lc->str + 1) && IsAnInteger(str + 1)) {
          // Check for week of year range.
          code = atoi(lc->str + 1);
          code2 = atoi(str + 1);
          if (code < 0 || code > 53 || code2 < 0 || code2 > 53) {
            scan_err0(lc, T_("Week number out of range (0-53)"));
            return;
          }
          if (!st.have_woy) {
            ClearBitRange(0, 53, st.dt.woy);
            st.have_woy = true;
          }
          if (code < code2) {
            SetBitRange(code, code2, st.dt.woy);
          } else {
            SetBitRange(code, 53, st.dt.woy);
            SetBitRange(0, code2, st.dt.woy);
          }
        } else {
          // lookup first half of keyword range (week days or months).
          lcase(lc->str);
          bool found = false;
          for (int i = 0; keyw[i].name; i++) {
            if (bstrcmp(lc->str, keyw[i].name)) {
              state = keyw[i].state;
              code = keyw[i].code;
              found = true;
              break;
            }
          }
          if (!found
              || (state != s_month && state != s_wday && state != s_wom)) {
            scan_err0(lc, T_("Invalid month, week or position day range"));
            return;
          }

          // Lookup end of range.
          lcase(str);
          found = false;
          for (int i = 0; keyw[i].name; i++) {
            if (bstrcmp(str, keyw[i].name)) {
              state2 = keyw[i].state;
              code2 = keyw[i].code;
              found = true;
              break;
            }
          }
          if (!found || state != state2 || code == code2) {
            scan_err0(lc, T_("Invalid month, weekday or position range"));
            return;
          }
          if (state == s_wday) {
            if (!st.have_wday) {
              ClearBitRange(0, 6, st.dt.wday);
              st.have_wday = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.wday);
            } else {
              SetBitRange(code, 6, st.dt.wday);
              SetBitRange(0, code2, st.dt.wday);
            }
          } else if (state == s_month) {
            if (!st.have_month) {
              ClearBitRange(0, 11, st.dt.month);
              st.have_month = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.month);
            } else {
              // This is a bit odd, but we accept it anyway
              SetBitRange(code, 11, st.dt.month);
              SetBitRange(0, code2, st.dt.month);
            }
          } else {
            // Must be position
            if (!st.have_wom) {
              ClearBitRange(0, 4, st.dt.wom);
              st.have_wom = true;
            }
            if (code < code2) {
              SetBitRange(code, code2, st.dt.wom);
            } else {
              SetBitRange(code, 4, st.dt.wom);
              SetBitRange(0, code2, st.dt.wom);
            }
          }
        }
      } break;
      case s_hourly:
        st.have_hour = true;
        SetBitRange(0, 23, st.dt.hour);
        break;
      case s_weekly:
        st.have_mday = st.have_wom = st.have_woy = true;
        SetBitRange(0, 30, st.dt.mday);
        SetBitRange(0, 4, st.dt.wom);
        SetBitRange(0, 53, st.dt.woy);
        break;
      case s_daily:
        st.have_mday = true;
        SetBitRange(0, 6, st.dt.wday);
        break;
      case s_monthly:
        st.have_month = true;
        SetBitRange(0, 11, st.dt.month);
        break;
      default:
        scan_err0(lc, T_("Unexpected run state\n"));
        return;
        break;
    }
  }

  /* Allocate run record, copy new stuff into it,
   * and append it to the list of run records
   * in the schedule resource.
   */

  new_res->minute = st.minute;
  new_res->date_time_bitfield = st.dt;
  AppendRunResource(current, new_res);

  lc->options = options; /* Restore scanner options */
  item->SetPresent(res);
  ClearBit(index, res->inherit_content_);
#endif
}
} /* namespace directordaemon */
