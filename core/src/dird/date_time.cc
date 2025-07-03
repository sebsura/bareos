/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2011 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
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

#include "include/bareos.h"
#include "dird/date_time.h"

#include <array>
#include <iostream>

namespace directordaemon {

static constexpr int kMonthsPerYear{12};
static constexpr int kDaysPerWeek{7};

static bool IsLeapYear(int year)
{
  if (year % 400 == 0) { return true; }
  if (year % 100 == 0) { return false; }
  if (year % 4 == 0) { return true; }
  return false;
}

DateTime::DateTime(time_t time)
{
  Blocaltime(&time, &original_time_);
  original_time_.tm_isdst = -1;
  year = 1900 + original_time_.tm_year;
  moy = MonthOfYear::FromIndex(original_time_.tm_mon).value();
  woy = WeekOfYear::FromIndex(TmWoy(time)).value();
  wom = WeekOfMonth::FromIndex((original_time_.tm_mday - 1) / 7).value();
  day_of_year = original_time_.tm_yday;
  day_of_month = original_time_.tm_mday - 1;
  day_of_week = original_time_.tm_wday;
  hour = original_time_.tm_hour;
  minute = original_time_.tm_min;
  second = original_time_.tm_sec;
}

bool DateTime::OnLast7DaysOfMonth() const
{
  auto last_day = moy.last_day(IsLeapYear(year));
  ASSERT(last_day >= day_of_year);
  return last_day - kDaysPerWeek < day_of_year;
}

time_t DateTime::GetTime() const
{
  struct tm tm = original_time_;
  tm.tm_year = year - 1900;
  tm.tm_mon = moy.Index();
  tm.tm_yday = day_of_year;
  tm.tm_mday = day_of_month + 1;
  tm.tm_wday = day_of_week;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  return mktime(&tm);
}

void DateTime::PrintDebugMessage(int debug_level) const
{
  Dmsg8(debug_level, "now = %lx: h=%d m=%zu md=%d wd=%d woy=%zu yday=%d\n ",
        GetTime(), hour, moy.Index(), day_of_month, day_of_week, woy.Index(),
        day_of_year);
}

std::ostream& operator<<(std::ostream& stream, const DateTime& date_time)
{
  stream << "DateTime{";
  stream << "yr=" << date_time.year << ", ";
  stream << "mon=" << date_time.moy.Index() << ", ";
  stream << "yweek=" << date_time.woy.Index() << ", ";
  stream << "mweek=" << date_time.wom.Index() << ", ";
  stream << "yday=" << date_time.day_of_year << ", ";
  stream << "mday=" << date_time.day_of_month << ", ";
  stream << "wday=" << date_time.day_of_week << ", ";
  stream << "hr=" << date_time.hour << ", ";
  stream << "min=" << date_time.minute << ", ";
  stream << "sec=" << date_time.second;
  stream << '}';
  return stream;
}


}  // namespace directordaemon
