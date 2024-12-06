#include <memory>
#include <cassert>
#include <vector>
#include <string>
#include <algorithm>
#include <span>
#include <memory_resource>

template <typename T>
struct allocator {
  std::vector<std::unique_ptr<T[]>> chunks;

  size_t size{};
  size_t cap{};
  size_t total_size{};
  size_t total_cap{};

  allocator() = default;
  allocator(size_t initial_cap) {
    chunks.emplace_back( std::make_unique<T[]>(initial_cap) );
    cap = initial_cap;
    total_cap = initial_cap;
  }

  T* alloc() {
    if (size == cap) {
      if (cap < 1024) { cap = 1024; }
      else if (cap < (256ULL * 1024ULL * 1024ULL) / sizeof(T)) { cap = cap * 2; }
      total_cap += cap;
      chunks.emplace_back( std::make_unique<T[]>(cap) );
      size = 0;
    }

    auto& ptr = chunks.back();

    T* res = &ptr[size];
    size += 1;
    total_size += 1;
    return res;
  }
};

#include <span>

template <typename UserT>
struct node {
private:
  node* parent{};
  std::vector<const char*> names_{};
  std::vector<node*> children_{};

public:
  UserT* value{nullptr};

  node() = default;
  node(const node&) = delete;
  node& operator=(const node&) = delete;
  node(node&&) = delete;
  node& operator=(node&&) = delete;

  std::span<const char * const> names() const noexcept {
    return { names_.begin(), names_.size() };
  }
  std::span<node const * const> children() const noexcept {
    return { children_.begin(), children_.size() };
  }

  node* find_or_emplace(const char* name,
                        allocator<node>& alloc)
  {
    for (size_t i = 0; i < names_.size(); ++i) {
      if (names_[i] == name) {
        return children_[i];
      }
    }

    auto* child = alloc.alloc();
    child->parent = this;

    names_.push_back(name);
    children_.push_back(child);

    return child;
  }

  std::string full_path() const {
    if (!parent) {
      return "";
    }

    std::string path = parent->full_path();
    path += "/";
    for (size_t i = 0; i < parent->children_.size(); ++i) {
      if (parent->children_[i] == this) {
        path += parent->names_[i];
        return path;
      }
    }

    ASSERT(0);
  }
};

#include <algorithm>
#include <cstring>

#include <unordered_set>

bool is_prefix(std::string_view prefix,
               std::string_view view)
{
  if (prefix.size() > view.size()) { return false; }
  if (prefix == view.substr(0, prefix.size())) { return true; }
  return false;
}

#include <chrono>

struct string_hash {
  using is_transparent = void;

  [[nodiscard]] size_t operator()(std::string_view txt) const {
    return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(std::string txt) const {
    return std::hash<std::string_view>{}(std::string_view{txt});
  }
};

struct StringCache {
  std::pmr::monotonic_buffer_resource buffer;
  std::pmr::unordered_set<std::pmr::string,
                     string_hash, std::equal_to<>> name_list[256];

  StringCache()
  {
    for (size_t i = 0; i < 256; ++i) {
      name_list[i] =
        std::pmr::unordered_set<std::pmr::string,
                                string_hash, std::equal_to<>>(&buffer);
    }
  }

  const char* intern(std::string_view str)
  {
    unsigned char first = str.size() ? str[0] : 0;

    if (auto found = name_list[first].find(str);
        found != name_list[first].end()) {
      return found->c_str();
    }

    auto [iter, _] = name_list[first].emplace(str);

    return iter->c_str();
  }

  std::pair<size_t, size_t> size() const noexcept {
    size_t count = 0;
    size_t size = 0;
    for (auto& l : name_list) {
      for (auto& s : l) {
        count += 1;
        size += s.size();
      }
    }

    return { count, size };
  }
};

#include <iostream>

std::size_t first_mismatch(std::string_view l,
                             std::string_view r)
{
  auto li = std::min(l.size(), r.size());
  for (size_t i = 0; i < li; ++i) {
    if (l[i] != r[i]) { return i; }
  }
  return li;
}

std::size_t my_binary_search(std::span<uint32_t> arr,
                             uint32_t val)
{
  if (arr[0] > val) { return arr.size(); }
  for (size_t i = 1; i < arr.size(); ++i) {
    if (arr[i] > val) { return i - 1; }
  }

  return arr.size() - 1;
}

// returns the number of chars in the common path
static std::uint32_t common_path(std::string_view l, std::string_view r)
{
  std::uint32_t common = 0;

  for (std::size_t i = 0; i < std::min(l.size(), r.size()); ++i) {
    if (l[i] != r[i]) {
      return common;
    }
    if (l[i] == '/') { common = i; }
  }

  if (l.size() == r.size()) { return l.size(); }
  return common;
}

template <typename T>
struct PathCache {
  PathCache(T* def)
  {
    (void) def;
  }

  std::string current_path{};
  size_t cache_hits{};
  using Entry = std::pair<std::uint32_t, T*>;
  std::vector<Entry> entries;

  std::pair<std::string_view, T*> find(std::string_view p, T* t)
  {
    auto i = common_path(p, current_path);

    auto j = search(i);

    if (j == 0) {
      // we dont know anything
      return { p, t };
    }

    auto found_size = entries[j-1].first;
    auto found_elem = entries[j-1].second;

    // we found it exactly
    if (found_size == p.size()) { return { "", found_elem }; }

    // +1 = remove /
    return { p.substr(found_size + 1), found_elem };
  }

  std::size_t search(std::uint32_t i)
  {
    // we know that entries is sorted
    // we want to find the index of the largest element <= i

    if (entries.size() == 0) { return 0; }

    if (entries[0].first > i) { return 0; }

    for (size_t j = 1; j < entries.size(); ++j) {
      if (entries[j].first > i) { return j; }
    }

    return entries.size();
  }

  void enter(std::string_view p, T* t)
  {
    if (p.back() == '/') {
      p.remove_suffix(1);
    }

    auto i = common_path(p, current_path);

    if (i == p.size()) {
      current_path.resize(i);
    } else {
      current_path.assign(p);
    }

    auto j = search(i);

    entries.resize(j);

    if (j == 0 || entries.back().first != i) {
      entries.emplace_back(p.size(), t);
    }
  }
};

template <typename UserT>
struct tree {
  using Node = node<UserT>;

  tree() = default;
  tree(size_t initial_cap)
    : nodes{initial_cap}
  {
  }

  size_t cache_hits() const noexcept { return cache.cache_hits; }
  size_t count() const noexcept { return nodes.total_size; }
  size_t cap() const noexcept { return nodes.total_cap; }

  Node* get_path(std::string_view p) noexcept
  {
    return get_path(p, root());
  }

  Node* get_path(std::string_view p, Node* current) noexcept
  {
    std::string_view orig_p = p;

    std::tie(p, current) = cache.find(p, current);

    while (p.size() != 0) {
      auto pos = p.find_first_of("/");
      auto comp = p.substr(0, pos);

      const char* name = intern_name(comp);
      current = current->find_or_emplace(name, nodes);

      if (pos == p.npos) {
        break;
      }

      p.remove_prefix(pos + 1);
    }

    cache.enter( orig_p, current );

    return current;
  }

  Node* root() noexcept { return &root_; }

  std::pair<size_t, size_t> name_size() {
    return name_list.size();
  }

  auto begin() { return nodes.begin(); }
  auto end() { return nodes.end(); }
private:
  Node root_{};
  allocator<Node> nodes;
  StringCache name_list;
  PathCache<Node> cache{&root_};

  const char* intern_name(std::string_view name)
  {
    return name_list.intern(name);
  }
};

#include <iostream>

#include <cinttypes>

using DeltaSeq = std::uint32_t;
using FileIndex = std::uint64_t;
using JobId = std::uint32_t;

struct ndmp_info {
  uint64_t fh_info;
  uint64_t fh_node;
};

struct entry_base {
  entry_base* left{};
  entry_base* right{};
  FileIndex file_index{};
  DeltaSeq delta_seq{};
  JobId job_id{};
};

struct default_entry : public entry_base {
};
struct ndmp_entry : public entry_base {
  ndmp_info info;
};

struct versioned_entry {
  using Version = JobId;

  entry_base sentinel{};

  versioned_entry() {
    sentinel.left = &sentinel;
    sentinel.right = &sentinel;
  }

  entry_base* get_version(Version version)
  {
    for (auto* current = sentinel.right;
         current != &sentinel; current = current->right) {
      if (current->job_id == version) { return current; }
    }
    return nullptr;
  }

  void chain_left(entry_base* base, entry_base *left)
  {
    left->right = base;
    left->left = base->left;
    left->left->right = left;
    base->left = left;
  }

  void chain_right(entry_base* base, entry_base *right)
  {
    right->left = base;
    right->right = base->right;
    right->right->left = right;
    base->right = right;
  }

  void unchain(entry_base* base)
  {
    base->left->right = base->right;
    base->right->left = base->left;
    base->right = nullptr;
    base->left = nullptr;
  }

  // returns the old version (or nullptr if there was none)
  entry_base* swap_version(entry_base* ent) {
    Version version = ent->job_id;

    auto* found = get_version(version);

    if (!found) {
      chain_left(&sentinel, ent);
      return nullptr;
    }

    chain_right(found, ent);
    unchain(found);
    return found;
  }
};

#include <optional>

struct file_tree {
  allocator<versioned_entry> entries;
  allocator<ndmp_entry> ndmp;
  allocator<default_entry> def;
  tree<versioned_entry> structure;

  ndmp_entry* alloc_ndmp() noexcept
  {
    return ndmp.alloc();
  }

  default_entry* alloc_default() noexcept
  {
    return def.alloc();
  }

  node<versioned_entry>& find(std::string_view path) noexcept
  {
    auto* found = structure.get_path(path);
    if (!found->value) {
      found->value = entries.alloc();
    }
    return *found;
  }

  node<versioned_entry>& find_from(node<versioned_entry>& ent,
                                   std::string_view path) noexcept
  {
    auto* found = structure.get_path(path, &ent);
    if (!found->value) {
      found->value = entries.alloc();
    }
    return *found;
  }
};

