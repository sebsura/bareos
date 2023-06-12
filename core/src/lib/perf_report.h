/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2022-2023 Bareos GmbH & Co. KG

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
#ifndef BAREOS_LIB_PERF_REPORT_H_
#define BAREOS_LIB_PERF_REPORT_H_

#include <unordered_map>
#include <string>
#include <optional>
#include <cstdint>
#include <shared_mutex>

#include "lib/event.h"
#include "lib/thread_util.h"
#include "include/messages.h"

extern int debug_level;

class ReportGenerator {
 public:
  virtual void add_events(const EventBuffer& buf [[maybe_unused]]) {}
  virtual ~ReportGenerator() = default;
};

class ThreadPerformanceReport {
 public:
  class Node {
   public:
    using childmap
        = std::unordered_map<BlockIdentity const*, std::unique_ptr<Node>>;
    Node* parent() const { return parent_; }
    Node* child(BlockIdentity const* source)
    {
      std::unique_ptr child = std::make_unique<Node>(this, source);
      auto [iter, _] = children.try_emplace(source, std::move(child));

      return iter->second.get();
    }
    bool is_open() const { return since.has_value(); }
    void open(event::time_point at)
    {
      ASSERT(!is_open());
      since = at;
    }
    void close(event::time_point at)
    {
      using namespace std::chrono;

      ASSERT(is_open());
      ns += duration_cast<nanoseconds>(at - since.value());
      since.reset();
    }
    // creates a deep copy of this node, except every open node
    // gets replaced by a closed node in the copy with the endtime being at
    std::unique_ptr<Node> closed_deep_copy_at(event::time_point at) const
    {
      using namespace std::chrono;
      std::unique_ptr copy = std::make_unique<Node>();
      copy->ns = ns;
      copy->depth_ = depth_;
      if (is_open() && at > since.value()) {
        copy->ns += duration_cast<nanoseconds>(at - since.value());
      }

      for (auto& [source, child] : children) {
        auto [child_copy, inserted]
            = copy->children.emplace(source, child->closed_deep_copy_at(at));
        ASSERT(inserted);
        child_copy->second->parent_ = copy.get();
      }
      return copy;
    }

    std::chrono::nanoseconds time_spent() const
    {
      // this should only be called on closed nodes!
      return ns;
    }

    std::size_t depth() const { return depth_; }
    BlockIdentity const* source() const { return source_; }
    Node() = default;
    Node(Node* parent, BlockIdentity const* source) : parent_{parent}
						    , depth_{parent_->depth_ + 1}
						    , source_{source}
    {}
    Node(const Node&) = default;
    Node(Node&&) = default;
    Node& operator=(Node&&) = default;
    Node& operator=(const Node&) = default;
    const childmap& children_view() const { return children; }

   private:
    std::optional<event::time_point> since{std::nullopt};
    Node* parent_{nullptr};
    std::size_t depth_{0};
    BlockIdentity const* source_{nullptr};
    std::chrono::nanoseconds ns{0};
    childmap children{};
  };

  void begin_report(event::StartRecording start) {
    if (error_str.has_value()) {
      return;
    } else if (top.is_open()) {
      set_error("Tried starting recording when it was already started.");
    } else {
      top.open(start.when);
    }
  }

  void end_report(event::StopRecording stop) {
    if (error_str.has_value()) {
      return;
    } else if (!top.is_open()) {
      set_error("Tried stopping recording when it was not started.");
    } else {
      top.close(stop.when);
    }
  }

  void begin_event(event::OpenEvent e)
  {
    if (error_str.has_value()) {
      return;
    }
    if (current == nullptr) {
      set_error("Internal error while processing performance counters (enter).");
    } else if (!current->is_open()) {
      set_error("Trying to enter block when recording has not started yet!");
    } else {
      current = current->child(e.source);
      current->open(e.start);
    }
  }

  void end_event(event::CloseEvent e)
  {
    if (error_str.has_value()) {
      return;
    }
    if (current == nullptr) {
      set_error("Internal error while processing performance counters (exit).");
    } else if (current == &top) {
      std::string error;
      error += "Trying to leave block '";
      error += e.source->c_str();
      error += "' while no block is active.";
      set_error(std::move(error));
    } else if (current->source() != e.source) {
      std::string error;
      error += "Trying to leave block '";
      error += e.source->c_str();
      error += "' while block '";
      error += current->source()->c_str();
      error += "' is active.";
      set_error(error);
    } else if (!current->is_open()) {
      set_error("Trying to enter block when recording has not started yet!!");
    } else {
      current->close(e.end);
      current = current->parent();
    }
  }

  ThreadPerformanceReport() : current{&top}
  {
  }

  std::variant<std::unique_ptr<Node>, std::string> snapshot() const {
    event::time_point now = event::clock::now();
    if (error_str.has_value()) {
      return error_str.value();
    } else {
      return top.closed_deep_copy_at(now);
    }
  }

 private:
  Node top{};
  Node* current{nullptr};
  std::optional<std::string> error_str{std::nullopt};

  void set_error(std::string error)
  {
    Dmsg1(50, "%s", error.c_str());
    if (!error_str.has_value()) {
      error_str.emplace(std::move(error));
    }
  }
};


class PerformanceReport : public ReportGenerator {
 public:
  ~PerformanceReport() override;
  static constexpr std::size_t ShowAll = std::numeric_limits<std::size_t>::max();

  void add_events(const EventBuffer& buf) override
  {
    synchronized<ThreadPerformanceReport>* thread = nullptr;
    auto thread_id = buf.threadid();
    {
      auto locked = threads.rlock();
      auto iter = locked->find(thread_id);
      if (iter != locked->end()) { thread = const_cast<synchronized<ThreadPerformanceReport>*>(&iter->second); }
    }
    if (thread == nullptr) {
      auto locked = threads.wlock();
      auto [iter, did_insert] = locked->try_emplace(thread_id);
      ASSERT(did_insert);
      ASSERT(iter != locked->end());
      thread = const_cast<synchronized<ThreadPerformanceReport>*>(&iter->second);
    }
    // pointers to elements of unordered_map are stable
    // (as long as the element does not get removed)
    // so there is no need to keep the map locked here

    auto locked = thread->lock();

    for (auto event : buf) {
      if (auto* open = std::get_if<event::OpenEvent>(&event)) {
        locked->begin_event(*open);
      } else if (auto* close = std::get_if<event::CloseEvent>(&event)) {
        locked->end_event(*close);
      } else if (auto* start = std::get_if<event::StartRecording>(&event)) {
	locked->begin_report(*start);
      } else if (auto* stop = std::get_if<event::StopRecording>(&event)) {
	locked->end_report(*stop);
      }
    }
  }

  std::string callstack_str(std::size_t max_depth = PerformanceReport::ShowAll, bool relative = true) const;
  std::string overview_str(std::size_t num_to_show = PerformanceReport::ShowAll, bool relative = false) const;
  std::string collapsed_str(std::size_t max_depth = PerformanceReport::ShowAll) const;

 private:
  rw_synchronized<std::unordered_map<EventBuffer::thread_id, synchronized<ThreadPerformanceReport>>> threads{};
};

#endif  // BAREOS_LIB_PERF_REPORT_H_