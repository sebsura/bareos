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

#ifndef BAREOS_DIRD_DATE_TIME_H_
#define BAREOS_DIRD_DATE_TIME_H_

#include "include/baconfig.h"

#include <array>
#include <string>
#include <ctime>
#include <stdexcept>
#include <chrono>

namespace directordaemon {

constexpr auto kSecondsPerMinute
    = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::minutes(1))
          .count();
constexpr auto kSecondsPerHour
    = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(1))
          .count();
constexpr auto kSecondsPerDay
    = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::hours(24))
          .count();

struct DaysInMonth {
  std::size_t normal_year;
  std::size_t leap_year;

  explicit constexpr DaysInMonth(std::size_t length)
      : normal_year{length}, leap_year{length}
  {
  }

  constexpr DaysInMonth(std::size_t nlength, std::size_t llength)
      : normal_year{nlength}, leap_year{llength}
  {
  }
};

struct MonthData {
  std::string_view name;
  DaysInMonth length;

  constexpr MonthData(std::string_view name_, DaysInMonth length_)
      : name{name_}, length{length_}
  {
  }
};

template <size_t N>
static constexpr auto ComputeFirstDays(const MonthData (&month_data)[N],
                                       bool leap)
{
  std::array<int, N> first_days = {};

  auto current_day = 0;
  for (std::size_t i = 0; i < N; ++i) {
    first_days[i] = current_day;
    if (leap) {
      current_day += month_data[i].length.leap_year;
    } else {
      current_day += month_data[i].length.normal_year;
    }
  }

  return first_days;
}

template <size_t N>
static constexpr auto ComputeLastDays(const MonthData (&month_data)[N],
                                      bool leap)
{
  std::array<int, N> last_days = {};

  auto current_day = 0;
  for (std::size_t i = 0; i < N; ++i) {
    if (leap) {
      current_day += month_data[i].length.leap_year;
    } else {
      current_day += month_data[i].length.normal_year;
    }
    last_days[i] = current_day;
  }

  return last_days;
}

class MonthOfYear {
 public:
  static std::optional<MonthOfYear> FromIndex(int index)
  {
    if (0 <= index && static_cast<size_t>(index) < std::size(month_data)) {
      return MonthOfYear{static_cast<std::size_t>(index)};
    }

    return std::nullopt;
  }
  static std::optional<MonthOfYear> FromName(std::string_view name)
  {
    for (std::size_t i = 0; i < std::size(month_data); ++i) {
      // you can either specify the full name, or just the first three letters
      if (name.length() == month_data[i].name.length() || name.length() == 3) {
        if (bstrncasecmp(name.data(), month_data[i].name.data(),
                         name.length())) {
          return MonthOfYear{i};
        }
      }
    }
    return std::nullopt;
  }

  size_t Index() const { return index; }
  operator int() const { return Index(); }

  std::string_view name() const { return month_data[index].name; }

  auto first_day(bool leap) const { return first_days[leap][index]; }

  auto last_day(bool leap) const { return last_days[leap][index]; }

  MonthOfYear() = default;

 private:
  static constexpr MonthData month_data[] = {
      {"January", DaysInMonth(30)},   {"February", DaysInMonth(28, 29)},
      {"March", DaysInMonth(31)},     {"April", DaysInMonth(30)},
      {"Mai", DaysInMonth(31)},       {"June", DaysInMonth(30)},
      {"Juli", DaysInMonth(31)},      {"August", DaysInMonth(31)},
      {"September", DaysInMonth(30)}, {"October", DaysInMonth(31)},
      {"November", DaysInMonth(30)},  {"December", DaysInMonth(31)},
  };

  static constexpr auto first_days = std::array{
      ComputeFirstDays(month_data, false), ComputeFirstDays(month_data, true)};
  static constexpr auto last_days = std::array{
      ComputeLastDays(month_data, false), ComputeLastDays(month_data, true)};

  std::uint32_t index{};
  MonthOfYear(std::size_t index_) : index(index_) {}
};

class WeekOfYear {
 public:
  WeekOfYear() = default;
  operator int() const { return index; }
  std::size_t Index() const { return index; }

  static constexpr std::optional<WeekOfYear> FromIndex(int value)
  {
    if (0 <= value && value <= 53) { return WeekOfYear{value}; }
    return std::nullopt;
  }

 private:
  WeekOfYear(int value) : index(value) {}
  std::uint32_t index;
};

struct WomData {
  std::string_view primary_name;
  std::string_view alternative_name;
};

class WeekOfMonth {
 public:
  static std::optional<WeekOfMonth> FromIndex(int index)
  {
    if (0 <= index && static_cast<size_t>(index) < std::size(data)) {
      return WeekOfMonth(index);
    }

    return std::nullopt;
  }
  static std::optional<WeekOfMonth> FromName(std::string_view name)
  {
    for (size_t i = 0; i < std::size(data); ++i) {
      auto [primary, alternative] = data[i];
      if (name.length() == primary.length()) {
        if (bstrncasecmp(name.data(), primary.data(), name.length())) {
          return WeekOfMonth{i};
        }
      }
      if (name.length() == alternative.length()) {
        if (bstrncasecmp(name.data(), alternative.data(), name.length())) {
          return WeekOfMonth{i};
        }
      }
    }
    return std::nullopt;
  }

  size_t Index() const { return index; }
  operator int() const { return Index(); }

  std::string_view name() const { return data[index].primary_name; }

  WeekOfMonth() = default;

 private:
  static constexpr WomData data[] = {
      {"first", "1st"},  {"second", "2nd"}, {"third", "3rd"},
      {"fourth", "4th"}, {"last", "last"},  {"fifth", "fifth"},
  };

  std::uint32_t index;
  WeekOfMonth(std::size_t index_) : index(index_) {}
};

class DayOfMonth {
 public:
  DayOfMonth() = default;
  DayOfMonth(int value) : _value(value) { ASSERT(0 <= value && value <= 30); }

  operator int() const { return _value; }

 private:
  int _value;
};
struct DayOfWeek {
  static constexpr std::array<std::string_view, 7> kNames = {
      "Sunday",   "Monday", "Tuesday",  "Wednesday",
      "Thursday", "Friday", "Saturday",
  };

  DayOfWeek(int index)
  {
    ASSERT(0 <= index && static_cast<size_t>(index) < kNames.size());
    name = kNames.at(index);
  }
  static std::optional<DayOfWeek> FromName(std::string_view name)
  {
    for (std::string_view other_name : kNames) {
      if (name.length() == other_name.length() || name.length() == 3) {
        if (bstrncasecmp(name.data(), other_name.data(), name.length())) {
          return DayOfWeek{other_name};
        }
      }
    }
    return std::nullopt;
  }

  size_t Index() const
  {
    for (size_t i = 0; i < kNames.size(); ++i) {
      if (kNames.at(i).data() == name.data()) { return i; }
    }
    throw std::logic_error{"Illegal DayOfWeek instance."};
  }
  operator int() const { return Index(); }

  std::string_view name;

 private:
  DayOfWeek(std::string_view _name) : name(_name) {}
};

struct TimeOfDay {
  TimeOfDay(int h, int min) : hour(h), minute(min) {}
  TimeOfDay(int h, int min, int sec) : hour(h), minute(min), second(sec) {}

  int hour, minute;
  int second = 0;
};

struct DateTime {
  DateTime(time_t time);

  bool OnLast7DaysOfMonth() const;
  void PrintDebugMessage(int debug_level) const;
  time_t GetTime() const;

  int year{0};
  MonthOfYear moy{};
  WeekOfYear woy{};
  WeekOfMonth wom{};
  int day_of_year{0};
  int day_of_month{0};
  int day_of_week{0};
  int hour{0};
  int minute{0};
  int second{0};

  friend std::ostream& operator<<(std::ostream& stream,
                                  const DateTime& date_time);

 private:
  tm original_time_;
};

}  // namespace directordaemon

#endif  // BAREOS_DIRD_DATE_TIME_H_
