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

#include <jansson.h>

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

  struct record_set {
    std::vector<std::byte> datarecord;
    std::vector<record_id> recids{};

    record_set(const std::vector<std::byte>& data) : datarecord{data} {}

    std::vector<record_id>& ids() { return recids; }

    const std::vector<record_id>& ids() const { return recids; }

    const std::vector<std::byte>& data() const { return datarecord; }
  };

  template <typename Val>
  using sha_map = std::unordered_map<sha, Val, sha::hash>;

  void add_record(record_id id, const std::vector<std::byte>& datarecord)
  {
    auto size = datarecord.size();

    auto val = sha(datarecord);

    auto& map = records_by_size[size];

    if (auto found = map.find(val); found != map.end()) {
      found->second.ids().push_back(id);
    } else {
      auto [iter, inserted] = map.emplace(val, datarecord);
      ASSERT(inserted);
      iter->second.ids().push_back(id);
    }
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

json_t* vec_as_json(const std::vector<std::size_t> vec)
{
  json_t* array = json_array();
  for (auto num : vec) {
    json_t* val = json_integer(num);
    json_array_append_new(array, val);
  }
  return array;
}

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

    if (!vol.is_ok()) { continue; }
    for_each_record(vol, [volidx, &agg](auto recidx, auto& data) {
      agg.add_record(record_id{volidx, recidx}, data);
    });
  }

  auto data = agg.data();

  std::vector<std::byte> output;
  struct value {
    std::size_t start{}, size{};
    std::vector<std::size_t> records{};
    value() = default;
    value(std::size_t start, std::size_t size) : start{start}, size{size} {}
  };

  std::vector<std::vector<value>> vols;
  vols.resize(volumes.size());
  std::vector<value*> current;
  current.resize(volumes.size());
  for (auto& set : data) {
    auto& datarecord = set.data();
    auto start = output.size();
    auto size = datarecord.size();

    output.insert(output.end(), datarecord.begin(), datarecord.end());

    for (auto id : set.ids()) {
      if (current[id.volidx] == nullptr) {
        current[id.volidx] = &vols[id.volidx].emplace_back(start, size);
      }
      current[id.volidx]->records.push_back(id.recidx);
    }

    current.resize(0);
    current.resize(volumes.size());
  }

  json_error_t ec;
  json_t* json_volumes = json_object();
  for (std::size_t i = 0; i < vols.size(); ++i) {
    auto& vol = vols[i];
    json_t* array = json_array();
    for (auto& val : vol) {
      auto* ids = vec_as_json(val.records);
      auto* json = json_pack_ex(&ec, 0, "{s:i,s:i,s:o*}", "start", val.start,
                                "size", val.size, "records", ids);
      if (!json) {
        fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line,
                ec.column, ec.text);
        return 1;
      }

      json_array_append_new(array, json);
    }
    json_object_set_new(json_volumes, volumes[i].c_str(), array);
  }

  json_t* root = json_pack_ex(&ec, 0, "{s:s,s:o}", "output", outfile.c_str(),
                              "volumes", json_volumes);
  if (!root) {
    fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line, ec.column,
            ec.text);
    return 1;
  }


  int outfd = open(outfile.c_str(), O_CREAT | O_APPEND | O_WRONLY);

  if (outfd < 0) {
    perror(nullptr);
    return 1;
  }

  if (write(outfd, output.data(), output.size())
      != static_cast<ssize_t>(output.size())) {
    perror("bad write");
    return 1;
  }
  close(outfd);

  if (json_dump_file(root, replacement.c_str(), JSON_COMPACT) < 0) {
    fprintf(stderr, "could not dump json into %s\n", replacement.c_str());
  }
}
