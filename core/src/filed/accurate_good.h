/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2024-2024 Bareos GmbH & Co. KG

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

#ifndef BAREOS_FILED_ACCURATE_GOOD_H_
#define BAREOS_FILED_ACCURATE_GOOD_H_

#include <cstring>
#include <memory>
#include <unordered_map>
#include <string>

#include "filed/accurate.h"


namespace filedaemon {

class BareosFastFilelist : public BareosAccurateFilelist {
  struct string {
    static string copy(const char* str)
    {
      char* memory = strdup(str);
      return string(memory, memory + strlen(memory), memory);
    }

    static string copy(const char* str, std::size_t size)
    {
      char* memory = strdup(str);
      return string(memory, memory + size, memory);
    }

    static string non_copy(char* str, size_t size)
    {
      return string(str, str + size, nullptr);
    }

    static string non_copy(char* str) { return non_copy(str, strlen(str)); }

    string copy() { return copy(begin, end - begin); }

    const char *begin, *end;
    char* memory;

    string(string&& s) : begin{s.begin}, end{s.end}, memory{s.memory}
    {
      s.begin = s.end = s.memory = nullptr;
    }

    string(const string&) = delete;

   private:
    string(const char* begin_, const char* end_, char* memory_)
        : begin{begin_}, end{end_}, memory{memory_}
    {
    }

   public:
    struct hash {
      size_t operator()(const string& s) const
      {
        return std::hash<std::string_view>{}(
            std::string_view{s.begin, (size_t)(s.end - s.begin)});
      }
    };

    friend bool operator==(const string& lhs, const string& rhs)
    {
      return std::string_view{lhs.begin, (size_t)(lhs.end - lhs.begin)}
             == std::string_view{rhs.begin, (size_t)(rhs.end - rhs.begin)};
    }

    ~string()
    {
      if (memory) { ::free(memory); }
    }
  };

 public:
  ~BareosFastFilelist() = default;
  bool init() override { return true; }
  bool AddFile(char* fname,
               int fname_length,
               char* lstat,
               int lstat_length,
               char* chksum,
               int chksum_length,
               int32_t delta_seq) override
  {
    if (fname_length <= 0) { return false; }

    auto as_str = string::non_copy(fname, fname_length);
    if (auto iter = files.find(as_str); iter != files.end()) { return false; }

    payload pl;
    pl.filenr = filenr_++;
    pl.delta_seq = delta_seq;
    if (lstat_length > 0) {
      pl.lstat = std::string_view{lstat, (size_t)lstat_length};
    }
    if (chksum_length > 0) {
      pl.chksum = std::string_view{chksum, (size_t)lstat_length};
    }
    files.try_emplace(files.end(), as_str.copy(), std::move(pl));
    return true;
  }

  bool EndLoad() override
  {
    // nothing to do
    return true;
  }

  accurate_payload* lookup_payload(char* fname) override
  {
    if (auto found = files.find(string::non_copy(fname));
        found != files.end()) {
      auto accurate = std::make_unique<accurate_payload>();

      auto& pl = found->second;

      accurate->filenr = pl.filenr;
      accurate->delta_seq = pl.delta_seq;
      accurate->lstat = (char*)pl.lstat.c_str();
      accurate->chksum = (char*)pl.chksum.c_str();

      // oh-oh! leaky time!
      return accurate.release();
    }

    return nullptr;
  }

  bool Iterate(filelist_callback* cb) override
  {
    accurate_payload accurate;

    for (auto& [name, pl] : files) {
      accurate.filenr = pl.filenr;
      accurate.delta_seq = pl.delta_seq;
      accurate.lstat = pl.lstat.data();
      accurate.chksum = pl.chksum.data();

      if (!(*cb)(name.begin, BitIsSet(pl.filenr, seen_bitmap_), &accurate)) {
        return false;
      }
    }
    return true;
  }

 private:
  struct payload {
    int64_t filenr;
    int32_t delta_seq;
    std::string lstat;
    std::string chksum;
  };

  std::unordered_map<string, payload, string::hash> files;
};

}  // namespace filedaemon

#endif  // BAREOS_FILED_ACCURATE_GOOD_H_
