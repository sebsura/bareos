/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2021-2023 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
#if defined(HAVE_MINGW)
#  include "include/bareos.h"
#  include "benchmark/benchmark.h"
#else
#  include "include/bareos.h"
#  include "benchmark/benchmark.h"
#endif
#include <iostream>
#define private public
#include "dird/ua_tree.cc"
#include "dird/ua_output.h"
#include "dird/ua.h"
#include "dird/ua_restore.cc"
#include <cstring>
#include <memory_resource>

#include <utility>
#include <numeric>

#include "lib/tree_save.h"

using namespace directordaemon;

#include <fstream>

struct file_list {
  std::vector<std::pair<std::string, std::string>> paths;
  file_list(std::string path)
  {
    std::ifstream f{path.c_str()};
    std::string line;
    while (std::getline(f, line, '\0')) {
      auto pos = line.rfind('/');
      if (pos < line.size()) {
        paths.emplace_back(std::string(line.data(), pos + 1),
                           std::string(line.begin() + pos + 1, line.end()));
      } else {
        paths.emplace_back(std::string(line.data(), line.size()),
                           std::string());
      }
    }
  }
};

file_list list("/home/ssura/filelist");

template <typename T> struct array_list {
  using storage_type = std::aligned_storage_t<sizeof(T), alignof(T)>;

  std::unique_ptr<array_list> next;
  std::size_t capacity{0};
  std::size_t size{0};
  std::unique_ptr<storage_type[]> data;

  array_list(std::size_t capacity = 100) : capacity{capacity}
  {
    data = std::make_unique<storage_type[]>(capacity);
  }

  array_list(array_list&& l)
      : next{std::move(l.next)}
      , capacity{std::move(l.capacity)}
      , size{std::move(l.size)}
      , data{std::move(l.data)}
  {
  }

  template <typename... Args> T& emplace_back(Args... args)
  {
    if (size >= capacity) {
      std::unique_ptr<array_list> copy
          = std::make_unique<array_list<T>>(std::move(*this));

      next = std::move(copy);
      size = 0;
      capacity = capacity + (capacity >> 1);
      data = std::make_unique<storage_type[]>(capacity);
    }

    T* ptr = std::launder(new (&data[size++]) T(std::forward<Args>(args)...));
    return *ptr;
  }
};

struct tree3 {
  struct node {
    std::string_view name;
    size_t subtree_size;
  };

  std::vector<node> nodes;

  enum class visitor_result
  {
    SKIP,  // skip subtree, and continue with next sibling
    NEXT,  // goto next node; possibly entering a subtree
    END,   // finish up
  };

  template <typename F, typename R = std::invoke_result_t<F, node>>
  void visit(F f)
  {
    static_assert(std::is_same_v<R, visitor_result>);
    for (size_t i = 0; i < nodes.size(); ++i) {
      visitor_result r = f(nodes[i]);
      switch (r) {
        case NEXT: {
          // nothing to do
        } break;
        case SKIP: {
          i += nodes[i].subtree_size;
        } break;
        case END: {
          return;
        } break;
      }
    }
  }
};

UaContext ua;
TreeContext tree;
std::vector<char> bytes;
std::vector<char> loaded;
std::string filename = "files.out";

template <typename T> struct span {
  const T* mem{nullptr};
  std::size_t count{0};

  span() {}
  span(const T* mem, std::size_t count) : mem{mem}, count{count} {}

  const T* begin() const { return mem; }
  const T* end() const { return mem + count; }

  const T* data() const { return mem; }
  std::size_t size() const { return count; }

  const T& operator[](std::size_t i) { return mem[i]; }
};

const int max_depth = 30;

enum HIGH_FILE_NUMBERS
{
  hundred_thousand = 100'000,
  million = 1'000'000,
  ten_million = 10'000'000,
  hundred_million = 100'000'000,
  billion = 1'000'000'000
};

void InitContexts(UaContext* ua, TreeContext* tree, int count = 1)
{
  ua->cmd = GetPoolMemory(PM_FNAME);
  ua->args = GetPoolMemory(PM_FNAME);
  ua->verbose = true;
  ua->automount = true;
  ua->send = new OutputFormatter(sprintit, ua, filterit, ua);

  tree->root = new_tree(count);
  tree->ua = ua;
  tree->all = false;
  tree->FileEstimate = 100;
  tree->DeltaCount = 1;
  tree->node = (TREE_NODE*)tree->root;
}

int FakeCdCmd(UaContext* ua, TreeContext* tree, std::string path)
{
  std::string command = "cd " + path;
  PmStrcpy(ua->cmd, command.c_str());
  ParseArgsOnly(ua->cmd, ua->args, &ua->argc, ua->argk, ua->argv, MAX_CMD_ARGS);
  return cdcmd(ua, tree);
}

int FakeMarkCmd(UaContext* ua, TreeContext* tree, std::string path)
{
  std::string command = "mark " + path;
  PmStrcpy(ua->cmd, command.c_str());

  ParseArgsOnly(ua->cmd, ua->args, &ua->argc, ua->argk, ua->argv, MAX_CMD_ARGS);
  return MarkElements(ua, tree);
}

void PopulateTree(int quantity, TreeContext* tree)
{
  me = new DirectorResource;
  me->optimize_for_size = true;
  me->optimize_for_speed = false;
  InitContexts(&ua, tree, quantity);

#if !defined(FILE_LIST)
  char* filename = GetPoolMemory(PM_FNAME);
  char* path = GetPoolMemory(PM_FNAME);


  std::string file_path = "/";
  std::string file{};

  std::size_t nfile = 0;
  std::size_t ndir = 0;
  for (int i = 0; i < max_depth; ++i) {
    file_path.append("dir" + std::to_string(i) + "/");
    ndir += 1;
    for (int j = 0; j < (quantity / max_depth); ++j) {
      nfile += 1;
      file = "file" + std::to_string(j);

      strcpy(path, file_path.c_str());
      strcpy(filename, file.c_str());

      char* row0 = path;
      char* row1 = filename;
      char row2[] = "1";
      char row3[] = "2";
      char row4[]
          = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
      char row5[] = "0";
      char row6[] = "0";
      char row7[] = "0";
      char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

      InsertTreeHandler(tree, 0, row);
    }
  }
#else

  std::string path;
  std::string filename;
  for (std::size_t i = 0; i < list.paths.size(); ++i) {
    auto& path = list.paths[i].first;
    auto& filename = list.paths[i].second;
    char* row0 = path.data();
    char* row1 = filename.data();
    char row2[] = "1";
    char row3[] = "2";
    char row4[] = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
    char row5[] = "0";
    char row6[] = "0";
    char row7[] = "0";
    char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

    InsertTreeHandler(tree, 0, row);
  }
#endif
}

void PopulateTree2(int quantity)
{
  std::unique_ptr builder = MakeTreeBuilder(quantity);

#if !defined(FILE_LIST)
  char* filename = GetPoolMemory(PM_FNAME);
  char* path = GetPoolMemory(PM_FNAME);

  std::string file_path = "/";
  std::string file{};

  for (int i = 0; i < max_depth; ++i) {
    file_path.append("dir" + std::to_string(i) + "/");
    for (int j = 0; j < (quantity / max_depth); ++j) {
      file = "file" + std::to_string(j);

      strcpy(path, file_path.c_str());
      strcpy(filename, file.c_str());

      char* row0 = path;
      char* row1 = filename;
      char row2[] = "1";
      char row3[] = "2";
      char row4[]
          = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
      char row5[] = "0";
      char row6[] = "0";
      char row7[] = "0";
      char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

      job_tree_builder_cb((void*)builder.get(), 8, row);
    }
  }
#else
  std::string path;
  std::string filename;
  for (std::size_t i = 0; i < list.paths.size(); ++i) {
    auto& path = list.paths[i].first;
    auto& filename = list.paths[i].second;
    char* row0 = path.data();
    char* row1 = filename.data();
    char row2[] = "1";
    char row3[] = "2";
    char row4[] = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
    char row5[] = "0";
    char row6[] = "0";
    char row7[] = "0";
    char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

    job_tree_builder_cb((void*)builder.get(), 8, row);
  }

#endif

  benchmark::DoNotOptimize(builder);

  std::cout << num_nodes(builder.get()) << std::endl;
  builder.release();
}

void PopulateTree3(int quantity)
{
  (void)quantity;

  my_data data;

#if 1
  char* filename = GetPoolMemory(PM_FNAME);
  char* path = GetPoolMemory(PM_FNAME);

  std::string file_path = "/";
  std::string file{};

  for (int i = 0; i < max_depth; ++i) {
    file_path.append("dir" + std::to_string(i) + "/");
    for (int j = 0; j < (quantity / max_depth); ++j) {
      file = "file" + std::to_string(j);

      strcpy(path, file_path.c_str());
      strcpy(filename, file.c_str());

      char* row0 = path;
      char* row1 = filename;
      char row2[] = "1";
      char row3[] = "2";
      char row4[]
          = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
      char row5[] = "0";
      char row6[] = "0";
      char row7[] = "0";
      char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

      insert((void*)&data, 8, row);
    }
  }
#else
  std::string path;
  std::string filename;
  for (auto& fullpath : list.lines) {
    auto pos = fullpath.rfind('/');
    if (pos < fullpath.size()) {
      path.assign(fullpath.data(), pos + 1);
      filename.assign(fullpath.begin() + pos + 1, fullpath.end());
    } else {
      path.assign(fullpath.data(), fullpath.size());
      filename.clear();
    }
    char* row0 = path.data();
    char* row1 = filename.data();
    char row2[] = "1";
    char row3[] = "2";
    char row4[] = "P0C BHoVZ IGk B Po Po A Cr BAA I BlA+1A BF2dbV BlA+1A A A C";
    char row5[] = "0";
    char row6[] = "0";
    char row7[] = "0";
    char* row[] = {row0, row1, row2, row3, row4, row5, row6, row7};

    insert((void*)&data, 8, row);
  }

#endif

  finish(data);

  std::cout << data.names.size() << std::endl;

  benchmark::DoNotOptimize(data);
}

[[maybe_unused]] static void BM_populatetree(benchmark::State& state)
{
  for (auto _ : state) { PopulateTree(state.range(0), &tree); }
}

[[maybe_unused]] static void BM_populatetree2(benchmark::State& state)
{
  for (auto _ : state) { PopulateTree2(state.range(0)); }
}

[[maybe_unused]] static void BM_populatetree3(benchmark::State& state)
{
  for (auto _ : state) { PopulateTree3(state.range(0)); }
}


[[maybe_unused]] static void BM_markallfiles(benchmark::State& state)
{
  FakeCdCmd(&ua, &tree, "/");
  [[maybe_unused]] int count = 0;
  for (auto _ : state) { count = FakeMarkCmd(&ua, &tree, "*"); }

  std::cout << "Marked: " << count << " files." << std::endl;
}

[[maybe_unused]] static void BM_buildtree(benchmark::State& state)
{
  // for (auto _ : state) {
  //   tree_builder builder(tree.root);
  //   bytes = builder.to_bytes();
  // }

  // std::cout << bytes.size() << std::endl;
  (void)state;
}

[[maybe_unused]] static void BM_savetree(benchmark::State& state)
{
  for (auto _ : state) {
    unlink(filename.c_str());
    SaveTree(filename.c_str(), tree.root);
    // int fd = open(filename.c_str(), O_WRONLY | O_CREAT, S_IRWXU);
    // ASSERT(fd >= 0);
    // std::size_t written = 0;
    // while (written < bytes.size()) {
    //   auto bytes_written = write(fd,
    // 				 (bytes.data() + written),
    // 				 bytes.size() - written);
    //   ASSERT(bytes_written > 0);

    //   written += bytes_written;
    // }

    // ASSERT(fsync(fd) == 0);
    // ASSERT(close(fd) == 0);
  }
}

[[maybe_unused]] static void BM_loadtree(benchmark::State& state)
{
  for (auto _ : state) {
    std::size_t size{0};
    TREE_ROOT* root = LoadTree(filename.c_str(), &size, false);
    ASSERT(root);
    benchmark::DoNotOptimize(root);
    state.PauseTiming();
    FreeTree(root);
    state.ResumeTiming();
  }
}

[[maybe_unused]] static void BM_markallfiles2(benchmark::State& state)
{
  // [[maybe_unused]] std::size_t count = 0;
  // for (auto _ : state) { count = tree2.mark_glob("*"); }

  // std::cout << "Marked: " << count << " files." << std::endl;
  (void)state;
}


// BENCHMARK(BM_populatetree)
//     ->Arg(HIGH_FILE_NUMBERS::hundred_thousand)
//     ->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles)->Unit(benchmark::kSecond);
// BENCHMARK(BM_buildtree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles2)->Unit(benchmark::kSecond);


// BENCHMARK(BM_populatetree)
//     ->Arg(HIGH_FILE_NUMBERS::million)
//     ->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles)->Unit(benchmark::kSecond);
// BENCHMARK(BM_buildtree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles2)->Unit(benchmark::kSecond);


// BENCHMARK(BM_populatetree)
//     ->Arg(HIGH_FILE_NUMBERS::ten_million)
//     ->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles)->Unit(benchmark::kSecond);
// BENCHMARK(BM_buildtree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles2)->Unit(benchmark::kSecond);

// BENCHMARK(BM_populatetree)->Arg(500'000)->Unit(benchmark::kSecond);
BENCHMARK(BM_populatetree2)->Arg(50'000'000)->Unit(benchmark::kSecond);
// BENCHMARK(BM_populatetree3)->Arg(50'000'000)->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles)->Unit(benchmark::kSecond);

// BENCHMARK(BM_buildtree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_savetree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_loadtree)->Unit(benchmark::kSecond);
// BENCHMARK(BM_markallfiles2)->Unit(benchmark::kSecond);


/*
 * Over ten million files requires quiet a bit a ram, so if you are going to
 * use the higher numbers, make sure you have enough ressources, otherwise the
 * benchmark will crash
 */

BENCHMARK_MAIN();
