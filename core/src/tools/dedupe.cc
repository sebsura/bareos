/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2023-2023 Bareos GmbH & Co. KG

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

#include "lib/cli.h"
#include "lib/version.h"

#include "stored/backends/dedup/dedup_config.h"
#include "stored/backends/dedup/dedup_volume.h"

#include "lib/crypto.h"

#include <unordered_map>
#include <vector>

#include <cassert>

template <typename F> void for_each_record(dedup::volume& vol, F callback)
{
  std::vector<dedup::record_header> records;
  std::vector<std::byte> data;
  for (auto& rf : vol.recordfiles()) {
    records.clear();
    records.resize(rf.size());
    if (!rf.read_at(rf.begin(), records.data(), records.size())) {
      std::cout << "Error while reading the records from record file "
                << rf.path();
      continue;
    }

    // todo: ingest by file_index

    for (std::size_t i = 0; i < records.size(); ++i) {
      data.clear();
      data.resize(records[i].size);
      dedup::write_buffer buffer((char*)data.data(), data.size());
      if (!vol.read_data(records[i].file_index, records[i].start,
                         records[i].size, buffer)) {
        std::cout << "Could not read record " << i + rf.begin() << " from file "
                  << rf.path() << "\n";
        continue;
      }
      callback(i + rf.begin(), data);
    }
  }
}

struct record_id {
  std::size_t volidx, recidx;
  record_id(std::size_t volidx, std::size_t recidx)
      : volidx{volidx}, recidx{recidx}
  {
  }
};

struct sha_aggregator {
  struct sha {
    sha(const std::vector<std::byte>& record)
    {
      DIGEST* digester = crypto_digest_new(nullptr, CRYPTO_DIGEST_SHA256);

      uint32_t size = sizeof(data);
      assert(digester->Update((const uint8_t*)record.data(), record.size()));
      assert(digester->Finalize((uint8_t*)data.data(), &size));

      if (size != sizeof(data)) { throw "Bad size"; }

      CryptoDigestFree(digester);
    }
    std::array<uint64_t, 4> data;


    friend bool operator==(const sha& l, const sha& r)
    {
      return l.data == r.data;
    }

    struct hash {
      std::size_t operator()(const sha& val) const
      {
        return val.data[0] + val.data[1] + val.data[2] + val.data[3];
      }
    };
  };

  using record_set = std::vector<record_id>;
  template <typename Val>
  using sha_map = std::unordered_map<sha, Val, sha::hash>;

  void add_record(record_id idx, const std::vector<std::byte>& datarecord)
  {
    auto size = datarecord.size();

    auto val = sha(datarecord);

    records_by_size[size][val].push_back(idx);
  }

  std::unordered_map<std::size_t, sha_map<record_set>> records_by_size;

  std::vector<record_set> data()
  {
    std::vector<record_set> data;
    for (auto& [_, map] : records_by_size) {
      for (auto& [_, set] : map) { data.push_back(set); }
    }

    return data;
  }
};

int main(int argc, const char** argv)
{
  CLI::App app;
  InitCLIApp(app, "bareos dedupe records", 2023);

  std::vector<std::string> volumes;
  app.add_option("-v,--volumes,volumes", volumes)->required();
  std::string outfile{"dedup.out"};
  app.add_option("-o,--output", outfile)->check(CLI::NonexistentPath);
  std::string replacement{"dedup.repl"};
  app.add_option("-r,--replace", replacement)->check(CLI::NonexistentPath);

  CLI11_PARSE(app, argc, argv);
  sha_aggregator agg;

  for (std::size_t volidx = 0; volidx < volumes.size(); ++volidx) {
    auto& volume = volumes[volidx];
    dedup::volume vol{volume.c_str(), storagedaemon::DeviceMode::OPEN_READ_ONLY,
                      0, 0};

    if (!vol.is_ok()) {
      std::cout << volume << " NOT ok\n";
      continue;
    }
    std::cout << volume << " ok\n";

    for_each_record(vol, [volidx, &agg](auto recidx, auto& data) {
      agg.add_record(record_id{volidx, recidx}, data);
    });
  }
  auto data = agg.data();

  std::size_t singles{};
  std::size_t lovers{};

  for (auto& set : data) {
    std::cout << "[\n";
    if (set.size() == 1)
      singles += 1;
    else
      lovers += set.size();
    for (auto id : set) {
      std::cout << "  <" << id.volidx << ", " << id.recidx << ">,\n";
    }
    std::cout << "]\n";
  }

  std::cout << "singles: " << singles << ", not: " << lovers << "\n";
}
