#include <memory>
#include <cassert>
#include <vector>
#include <string>
#include <algorithm>
#include <span>
#include <memory_resource>

#include <unordered_map>

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

template <typename T>
struct intrusive_list {
  intrusive_list<T>* next;
};

template <typename UserT>
struct node : intrusive_list<node<UserT>> {
private:
  node* parent{};
  char const* name_{};
  intrusive_list<node> children_{};

public:
  UserT value{};

  node() {
    children_.next = &children_;
  }

  struct iter {
    intrusive_list<node>* current;

    node* operator*() noexcept {
      return static_cast<node*>(current);
    }
    iter& operator++() noexcept {
      current = current->next;
      return *this;
    }
    bool operator==(const iter& other) const noexcept {
      return current == other.current;
    }
  };

  const char* name() const noexcept { return name_; }

  iter begin() noexcept { return iter{ children_.next }; }
  iter end() noexcept { return iter{ &children_ }; }

  node(const node&) = delete;
  node& operator=(const node&) = delete;
  node(node&&) = delete;
  node& operator=(node&&) = delete;


  node* find_or_emplace(const char* name,
                        allocator<node>& alloc)
  {
    auto ptr = intptr_t(name);
    auto* prev = &children_;
    auto* current = children_.next;
    while (current != &children_) {
      auto elem = static_cast<node*>(current);
      auto child_ptr = intptr_t(elem->name_);

      if (child_ptr == ptr) { return elem; }
      if (child_ptr > ptr) { break; }

      prev = current;
      current = current->next;
    }

    auto* child = alloc.alloc();
    child->parent = this;
    child->name_ = name;

    child->next = current;
    prev->next = child;

    return child;
  }

  std::string full_path() const {
    if (!parent) {
      return name_;
    }

    std::string path = parent->full_path();
    path += "/";
    path += name_;

    return path;
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

    cache_hits += 1;
    auto found_size = entries[j-1].first;
    auto found_elem = entries[j-1].second;

    // we found it exactly
    if (found_size == p.size()) {
      return { "", found_elem };
    }

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

  size_t count() const noexcept { return nodes.total_size; }
  size_t cap() const noexcept { return nodes.total_cap; }

  Node* get_path(std::string_view p) noexcept
  {
    return get_path(p, root());
  }

  Node* get_path(std::string_view p, Node* current) noexcept
  {
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

    return current;
  }

  // p _may not_ contain a /, this only gets the immediate child
  Node* get_child(std::string_view p, Node* current) noexcept
  {
    const char* name = intern_name(p);
    return current->find_or_emplace(name, nodes);
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

struct entry_base : intrusive_list<entry_base> {
  FileIndex file_index{};
  DeltaSeq delta_seq{};
  JobId job_id{};
};

struct default_entry : public entry_base {
};
struct ndmp_entry : public entry_base {
  ndmp_info info;
};

#include <optional>

struct file_tree {
  allocator<ndmp_entry> ndmp;
  allocator<default_entry> def;
  tree<intrusive_list<entry_base>> structure;

  using Node = decltype(structure)::Node;

  ndmp_entry* alloc_ndmp() noexcept
  {
    return ndmp.alloc();
  }

  default_entry* alloc_default() noexcept
  {
    return def.alloc();
  }

  Node& find(std::string_view path) noexcept
  {
    auto* found = structure.get_path(path);
    if (found->value.next == nullptr) {
      found->value.next = &found->value;
    }
    return *found;
  }

  Node& find_from(Node& ent, std::string_view path) noexcept
  {
    auto* found = structure.get_path(path, &ent);
    if (found->value.next == nullptr) {
      found->value.next = &found->value;
    }
    return *found;
  }
  Node& child_of(Node& ent, std::string_view comp) noexcept
  {
    auto* found = structure.get_child(comp, &ent);
    if (found->value.next == nullptr) {
      found->value.next = &found->value;
    }
    return *found;
  }
};

entry_base* swap_version(intrusive_list<entry_base>* sentinel,
                         entry_base* new_ent)
{
  auto* prev = sentinel;
  auto* current = sentinel->next;
  while (current != sentinel) {

    auto* val = static_cast<entry_base*>(current);
    if (val->job_id == new_ent->job_id) {
      // remove current/val & insert new_int in its place
      new_ent->next = val->next;
      prev->next = new_ent;
      val->next = nullptr;

      return val;
    }

    prev = current;
    current = current->next;
  }

  // insert new_int after the last entry (i.e. prev)
  new_ent->next = sentinel;
  prev->next = new_ent;

  // there is no old version
  return nullptr;
}

namespace idea2 {
  template <typename T>
  struct tree {
    struct node {
      const char* name;
      std::vector<node> children;
      T value;

      auto begin() noexcept {
        return children.begin();
      }
      auto end() noexcept {
        return children.end();
      }
    };

    node* find_or_create(std::string_view path, node* base)
    {
      tmp.clear();

      for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') { tmp.push_back(i); }
      }


      auto current_start = 0;
      node* current = base;

      for (std::size_t end : tmp) {
        std::string_view component = path.substr(current_start,
                                                 end - current_start);

        current = find_child_or_create(current, component);
        current_start = end + 1;
      }

      return find_child_or_create(current, path.substr(current_start));
    }

    node* find(std::string_view path, node* base)
    {
      tmp.clear();

      for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/') { tmp.push_back(i); }
      }


      auto current_start = 0;
      node* current = base;

      if (!current) { return nullptr; }

      for (std::size_t end : tmp) {
        std::string_view component = path.substr(current_start,
                                                 end - current_start);

        current = find_child(current, component);

        if (current == nullptr) { return nullptr; }

        current_start = end + 1;
      }

      return find_child(current, path.substr(current_start));
    }

    const char* intern(std::string_view v)
    {
      return interner->intern(v);
    }

    node* find_child_or_create(node* parent, std::string_view component)
    {
      if (component.size() == 0) { return parent; }
      const char* interned = intern(component);
      auto iter = std::lower_bound(parent->children.begin(),
                                   parent->children.end(),
                                   interned,
                                   [](const node& n, const char* str) -> bool {
                                     return (intptr_t)n.name < (intptr_t)str;
                                   });

      if (iter != parent->children.end() &&
         iter->name == interned) {
        return &*iter;
      }

      auto& child = *parent->children.emplace(iter);
      child.name = interned;
      return &child;
    }

    node* find_child(node* parent, std::string_view component)
    {
      if (component.size() == 0) { return parent; }
      const char* interned = intern(component);
      auto iter = std::lower_bound(parent->children.begin(),
                                   parent->children.end(),
                                   interned,
                                   [](const node& n, const char* str) -> bool {
                                     return (intptr_t)n.name < (intptr_t)str;
                                   });

      if (iter != parent->children.end() &&
         iter->name == interned) {
        return &*iter;
      }

      return nullptr;
    }

    tree(StringCache* c)
      : interner{c}
    {
      root.name = interner->intern("");
    }


    StringCache* interner{};
    std::vector<std::uint32_t> tmp;
    node root;
  };

  struct file_tree {
    struct file {
      const char* name{};
      FileIndex file_index{};
      time_t ctime{};
      // by transforming job_id into version_id (jobid[0] -> 0, jobid[1] -> 1, ...)
      // we can ensure that this version id is smaller than e.g. 255, that is,
      // it needs less than 8 bits for storage.  This means we can store it
      // in the upper bits of the 64bit FileIndex
      JobId job_id{};
      std::uint32_t extra_info{};
    };

    // some extra information that is not used most of the time
    struct extra_info {
      DeltaSeq delta_seq{};
      ndmp_info ndmp{};
    };
    struct directory {
      std::unordered_map<const char*, std::uint64_t> files;
    };

    StringCache cache;
    tree<directory> dir_tree{&cache};

    std::vector<file> file_data;
    std::vector<extra_info> extra_data;

    using Node = tree<directory>::node;

    Node& mkpath_from(Node& base, std::string_view path)
    {
      return *dir_tree.find_or_create(path, &base);
    }

    Node* find_from(Node& base, std::string_view path)
    {
      return dir_tree.find(path, &base);
    }

    std::pair<size_t, size_t> name_size() {
      return cache.size();
    }

    Node* root() { return  &dir_tree.root; }
  };
};
