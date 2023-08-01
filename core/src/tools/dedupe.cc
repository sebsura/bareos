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

#include <unistd.h>

struct dedup_opportunity {
  std::size_t start{}, size{};
  std::vector<std::size_t> records{};
  dedup_opportunity() = default;
  dedup_opportunity(std::size_t start, std::size_t size)
      : start{start}, size{size}
  {
  }
};

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
      callback(i + rf.begin(), records[i].BareosHeader, data);
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

  template <typename Val> using map = std::unordered_map<sha, Val, sha::hash>;
};

template <typename T> struct aggregator {
  // T needs to have a constructor that takes std::vector<std::byte>
  // as well as a type T::map<Val> that acts like a map between
  // T and Val.
  struct record_set {
    std::vector<std::byte> datarecord;
    std::vector<record_id> recids{};

    record_set(const std::vector<std::byte>& data) : datarecord{data} {}

    std::vector<record_id>& ids() { return recids; }

    const std::vector<record_id>& ids() const { return recids; }

    const std::vector<std::byte>& data() const { return datarecord; }
  };

  template <typename Val> using val_map = typename T::map<Val>;

  void add_record(record_id id, const std::vector<std::byte>& datarecord)
  {
    auto size = datarecord.size();

    auto val = T(datarecord);

    auto& map = records_by_size[size];

    if (auto found = map.find(val); found != map.end()) {
      found->second.ids().push_back(id);
    } else {
      auto [iter, inserted] = map.emplace(val, datarecord);
      ASSERT(inserted);
      iter->second.ids().push_back(id);
    }
  }

  std::unordered_map<std::size_t, val_map<record_set>> records_by_size;

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

template <typename Acceptor, typename Aggregator>
bool analyze_volumes(const std::vector<std::string>& volumes,
                     const std::string& bin_out,
                     const std::string& json_out,
                     Aggregator agg,
                     Acceptor accept)
{
  for (std::size_t volidx = 0; volidx < volumes.size(); ++volidx) {
    auto& volume = volumes[volidx];
    dedup::volume vol{volume.c_str(), storagedaemon::DeviceMode::OPEN_READ_ONLY,
                      0, 0};

    if (!vol.is_ok()) {
      fprintf(stderr, "could not open volume %s\n", volume.c_str());
      continue;
    }
    for_each_record(vol, [volidx, &agg](auto recidx, auto, auto& data) {
      agg.add_record(record_id{volidx, recidx}, data);
    });
  }

  auto data = agg.data();

  std::vector<std::byte> output;
  std::vector<std::vector<dedup_opportunity>> vols;
  vols.resize(volumes.size());
  std::vector<dedup_opportunity*> current;
  current.resize(volumes.size());
  for (auto& set : data) {
    if (!accept(set)) { continue; }
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
      auto* json = json_pack_ex(&ec, 0, "{s:I,s:I,s:o*}", "start", val.start,
                                "size", val.size, "records", ids);
      if (!json) {
        fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line,
                ec.column, ec.text);
        return false;
      }

      json_array_append_new(array, json);
    }
    json_object_set_new(json_volumes, volumes[i].c_str(), array);
  }

  std::string absolute_bin_out;
  if (bin_out.front() == '/') {
    absolute_bin_out = bin_out;
  } else {
    const char* cwd = get_current_dir_name();
    absolute_bin_out += cwd;
    absolute_bin_out += "/";
    absolute_bin_out += bin_out;
    free((void*)cwd);
  }

  json_t* root
      = json_pack_ex(&ec, 0, "{s:s,s:o}", "output", absolute_bin_out.c_str(),
                     "volumes", json_volumes);
  if (!root) {
    fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line, ec.column,
            ec.text);
    return false;
  }

  int outfd = open(bin_out.c_str(), O_CREAT | O_WRONLY | O_TRUNC);

  if (outfd < 0) {
    perror("bin file");
    return false;
  }

  if (write(outfd, output.data(), output.size())
      != static_cast<ssize_t>(output.size())) {
    perror("bad write");
    return false;
  }
  close(outfd);

  int jsonfd = open(json_out.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
  if (jsonfd < 0) {
    perror("json file");
    return false;
  }
  if (json_dumpfd(root, jsonfd, JSON_COMPACT) < 0) {
    fprintf(stderr, "could not dump json into %s\n", json_out.c_str());
    return false;
  }
  close(jsonfd);

  return true;
}

[[maybe_unused]] static std::vector<dedup::block_header> get_blocks(
    const dedup::volume& vol)
{
  const auto& files = vol.blockfiles();

  std::size_t total_size = 0;
  for (auto& file : files) { total_size += file.size(); }

  std::vector<dedup::block_header> header;
  header.resize(total_size);
  std::size_t current = 0;
  for (auto& file : files) {
    if (!file.read_at(current, &header[current], file.size())) { abort(); }
    current += file.size();
  }

  return header;
}

static int analyze(CLI::App& app, int argc, const char** argv)
{
  std::vector<std::string> volumes;
  app.add_option("-v,--volumes,volumes", volumes)->required();
  std::string outfile{"dedup.out"};
  app.add_option("-o,--output", outfile)->check(CLI::NonexistentPath);
  std::string json{"dedup.json"};
  app.add_option("-j,--json", json)->check(CLI::NonexistentPath);
  std::size_t min_save{0};
  app.add_option("-s,--min-size", min_save)->transform(CLI::AsSizeValue{false});

  CLI11_PARSE(app, argc, argv);

  aggregator<sha> agg;

  return analyze_volumes(volumes, outfile, json, agg,
                         [min_save](const auto& set) {
                           return set.data().size() * (set.ids().size() - 1)
                                  > min_save;
                         })
             ? 0
             : 1;
}

static int dedupe(CLI::App& app, int argc, const char** argv)
{
  std::string in_vol{};
  app.add_option("-i,--input", in_vol)->required();
  std::string out_vol{};
  app.add_option("-o,--output", out_vol)
      ->check(CLI::NonexistentPath)
      ->required();
  std::string json_file{"dedup.json"};
  app.add_option("-j,--json", json_file)->check(CLI::ExistingFile);

  CLI11_PARSE(app, argc, argv);

  json_error_t ec;
  json_t* root = json_load_file(json_file.c_str(), 0, &ec);
  if (!root) {
    fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line, ec.column,
            ec.text);
    return 1;
  }

  const char* bin_file;
  json_t* json_volumes;
  if (json_unpack_ex(root, &ec, 0, "{s:s,s:o}", "output", &bin_file, "volumes",
                     &json_volumes)
      < 0) {
    fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line, ec.column,
            ec.text);
    return 1;
  }

  const char* name;
  json_t* records;

  bool found = false;
  std::vector<dedup_opportunity> dedup_data;
  json_object_foreach(json_volumes, name, records)
  {
    if (in_vol == name) {
      std::size_t index;
      json_t* record;

      json_array_foreach(records, index, record)
      {
        json_int_t jstart, jsize;
        json_t* ids;
        if (json_unpack_ex(record, &ec, 0, "{s:I,s:I,s:o}", "start", &jstart,
                           "size", &jsize, "records", &ids)
            < 0) {
          fprintf(stderr, "json error %s:%d,%d: %s\n", ec.source, ec.line,
                  ec.column, ec.text);
          return 1;
        }

        std::size_t start, size;
        start = jstart;
        size = jsize;

        auto& val = dedup_data.emplace_back(start, size);

        std::size_t index2;
        json_t* json_int;
        json_array_foreach(ids, index2, json_int)
        {
          val.records.push_back(json_integer_value(json_int));
        }
      }
      found = true;
      break;
    }
  }

  if (!found) {
    fprintf(stderr, "Volume %s not found inside json. Nothing to do.\n",
            in_vol.c_str());
    return 1;
  }

  if (dedup_data.size() == 0) {
    fprintf(stderr, "Volume %s has no dedupable records. Nothing to do.\n",
            in_vol.c_str());
    return 1;
  }

  std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>>
      rec_to_loc;

  for (auto opp : dedup_data) {
    for (auto rec : opp.records) {
      rec_to_loc.try_emplace(rec, opp.start, opp.size);
    }
  }

  dedup::volume old_vol{in_vol.c_str(),
                        storagedaemon::DeviceMode::OPEN_READ_ONLY, 0, 0};

  if (!old_vol.is_ok()) {
    fprintf(stderr, "could not open volume %s; skipping\n", old_vol.name());
    return 1;
  }
  std::vector<dedup::block_header> blocks = get_blocks(old_vol);

  if (blocks.size() == 0) {
    fprintf(stderr, "no blocks in volume %s; skipping\n", old_vol.name());
    return 1;
  }

  dedup::volume new_vol{out_vol.c_str(),
                        storagedaemon::DeviceMode::CREATE_READ_WRITE, 0777, 0};

  if (!new_vol.is_ok()) {
    fprintf(stderr, "could not open new volume %s; skipping\n", new_vol.name());
    return 1;
  }
  new_vol.reset();

  std::size_t file_index = new_vol.add_read_only(bin_file);

  auto current_block = blocks.begin();


  for_each_record(old_vol, [&current_block, &blocks, &new_vol, &old_vol,
                            &rec_to_loc, file_index](
                               auto recidx, auto record_header, auto& data) {
    auto copy = current_block;
    while ((recidx >= current_block->start + current_block->count)
           && current_block != blocks.end()) {
      ++current_block;
    }

    if ((recidx >= current_block->start + current_block->count)
        || recidx < current_block->start) {
      fprintf(stderr, "no block found for record %zu in volume %s; skipping\n",
              recidx, old_vol.name());
      current_block = copy;
      return;
    }
    dedup::record_header header;
    header.BareosHeader = record_header;

    if (auto rec_found = rec_to_loc.find(recidx);
        rec_found != rec_to_loc.end()) {
      header.start = rec_found->second.first;
      assert(data.size() == rec_found->second.second);
      header.size = rec_found->second.second;
      header.file_index = file_index;
    } else {
      std::optional loc = new_vol.append_data(
          current_block->BareosHeader, record_header,
          reinterpret_cast<const char*>(data.data()), data.size());
      header.start = loc->begin;
      header.size = data.size();
      header.file_index = loc->file_index;
    }
    new_vol.append_records(&header, 1);
  });

  for (auto& block : blocks) { new_vol.append_block(block); }

  return 0;
}

int main(int argc, const char** argv)
{
  CLI::App app;
  std::string desc(1024, '\0');
  kBareosVersionStrings.FormatCopyright(desc.data(), desc.size(), 2023);
  desc += "The Bareos Record Deduplication Tool";
  InitCLIApp(app, desc, 0);
  AddDebugOptions(app);


  if (argc >= 2) {
    if (Bstrcasecmp(argv[1], "analyze")) {
      std::exchange(argv[0], argv[1]);
      argc -= 1;
      return analyze(app, argc, argv + 1);
    } else if (Bstrcasecmp(argv[1], "dedupe")) {
      std::exchange(argv[0], argv[1]);
      argc -= 1;
      return dedupe(app, argc, argv + 1);
    }
  }

  return analyze(app, argc, argv);
}
