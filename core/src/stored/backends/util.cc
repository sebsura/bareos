/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2024 Bareos GmbH & Co. KG

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

#include "stored/backends/util.h"
#include "include/baconfig.h"

namespace util::options {

int key_compare(std::string_view l, std::string_view r)
{
  // NOTE(ssura): this is not unicode compliant.
  constexpr auto npos = l.npos;
  for (;;) {
    auto lidx = l.find_first_not_of(" \n\t_");
    auto ridx = r.find_first_not_of(" \n\t_");

    if (lidx == npos && ridx == npos) {
      return 0;
    } else if (lidx == npos) {
      return -1;
    } else if (ridx == npos) {
      return 1;
    } else if (std::tolower(l[lidx]) != std::tolower(r[ridx])) {
      return std::tolower(l[lidx]) < std::tolower(r[ridx]) ? -1 : 1;
    } else {
      l = l.substr(lidx + 1);
      r = r.substr(ridx + 1);
    }
  }
}

namespace {
template <typename... View>
std::string highlight_str_parts(std::string_view str, View... subviews)
{
  // subviews need to be disjoint!
  std::vector<std::string_view> sorted{std::forward<View>(subviews)...};

  std::sort(sorted.begin(), sorted.end(),
            [](auto l, auto r) { return l.data() < r.data(); });

  auto iter = str;

  std::string result;
  for (auto part : sorted) {
    ASSERT(part.data() >= iter.data());
    if (part.data() > iter.data()) {
      result += iter.substr(0, part.data() - iter.data());
    }

    result += '[';
    result += part;
    result += ']';

    iter = iter.substr(part.end() - iter.begin());
  }

  result += iter;

  return result;
}

template <typename... View>
std::string format_parse_error_at(std::string_view error_msg,
                                  std::string_view str,
                                  View... errors)
{
  ASSERT((str.begin() <= errors.begin()) && ...);
  ASSERT((str.end() >= errors.end()) && ...);
  std::string error;

  error += "Encountered error while parsing the highlighted block: '";
  error += highlight_str_parts(str, std::forward<View>(errors)...);
  error += "'";

  if (error_msg.size()) {
    error += " (";
    error += error_msg;
    error += ")";
  }

  return error;
}
};  // namespace

std::variant<options, error> parse_options(std::string_view str)
{
  constexpr auto npos = str.npos;

  auto iter = str;

  options parsed;

  for (; iter.size();) {
    auto end = iter.find(',');

    if (end == 0) {
      return format_parse_error_at("found nothing expecting kv-pair", str,
                                   iter.substr(0, end + 1));
    }

    while (end != npos && iter[end - 1] == '\\') {
      auto last_non_backslash = iter.find_last_not_of('\\', end - 1);

      auto first_backslash
          = (last_non_backslash == npos) ? 0 : last_non_backslash + 1;

      auto num_backslash = end - first_backslash;

      if (num_backslash % 2 == 0) {
        // if all backslashes are escaped, then stop
        break;
      }

      end = iter.find(',', end + 1);
    }

    // pair should look like k=v
    std::string_view pair = iter.substr(0, end);

    auto eq_pos = pair.find('=');
    if (eq_pos == npos) {
      // no '=' found -> print error

      return format_parse_error_at("expected equals sign in kv pair.", str,
                                   pair);
    }
    std::string_view key = pair.substr(0, eq_pos);
    std::string_view val = pair.substr(eq_pos + 1);

    if (key.size() == 0) {
      return format_parse_error_at("key is empty", str, pair);
    }

    if (val.size() == 0) {
      return format_parse_error_at("val is empty", str, pair);
    }

    if (auto [miter, inserted] = parsed.emplace(key, val); !inserted) {
      return format_parse_error_at("duplicate key", str, miter->first, key);
    }

    if (end == npos) { break; }
    iter = iter.substr(end + 1);
  }

  return parsed;
}
};  // namespace util::options
