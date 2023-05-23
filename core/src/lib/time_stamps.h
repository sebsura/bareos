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
#ifndef BAREOS_LIB_TIME_STAMPS_H_
#define BAREOS_LIB_TIME_STAMPS_H_

#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <variant>
#include <memory>  // shared_ptr
#include <deque>
#include <optional>

#include "event.h"
#include "perf_report.h"
#include "lib/thread_util.h"
#include "lib/channel.h"

static constexpr std::size_t buffer_full = 2000;

class TimeKeeper;

class ThreadTimeKeeper {
  friend class TimeKeeper;

 public:
  ThreadTimeKeeper(TimeKeeper& keeper)
      : this_id{std::this_thread::get_id()}, keeper{keeper}
  {
  }
  ~ThreadTimeKeeper();
  void enter(const BlockIdentity& block);
  void switch_to(const BlockIdentity& block);
  void exit();

 protected:
  EventBuffer flush()
  {
    EventBuffer new_buffer(this_id, buffer_full, stack);
    std::swap(new_buffer, *buffer.lock());
    return new_buffer;
  }

 private:
  std::thread::id this_id;
  TimeKeeper& keeper;
  std::vector<event::OpenEvent> stack{};
  synchronized<EventBuffer> buffer{this_id, buffer_full, stack};
};

class TimeKeeper {
 public:
  TimeKeeper(std::pair<channel::in<EventBuffer>, channel::out<EventBuffer>> p
             = channel::CreateBufferedChannel<EventBuffer>(1000));
  ~TimeKeeper()
  {
    auto now = event::clock::now();
    flush();
    {
      std::unique_lock lock{gen_mut};
      if (overview.has_value()) overview->end_report(now);
      if (callstack.has_value()) callstack->end_report(now);
    }
    queue.lock()->close();
    report_writer.join();
  }

  ThreadTimeKeeper& get_thread_local();

  void handle_event_buffer(EventBuffer buf)
  {
    queue.lock()->put(std::move(buf));
  }
  bool try_handle_event_buffer(EventBuffer& buf)
  {
    if (std::optional locked = queue.try_lock();
	locked.has_value()) {
      return (*locked)->try_put(buf);
    } else {
      return false;
    }
  }
  void flush()
  {
    {
      auto locked = keeper.wlock();
      for (auto& [_, thread] : *locked) {
        auto buf = thread.flush();
        handle_event_buffer(std::move(buf));
      }
    }
    // TODO: we should somehow only wait until the above buffers were handled
    queue.lock()->wait_till_empty();
  }
  std::string str()
  {
    flush();
    std::unique_lock lock(gen_mut);
    std::string result{};
    if (overview.has_value()) { result += overview.value().str(); }
    if (callstack.has_value()) { result += callstack.value().str(); }
    return result;
  }

 private:
  mutable std::mutex gen_mut{};
  std::condition_variable buf_empty{};
  std::condition_variable buf_not_empty{};
  synchronized<channel::in<EventBuffer>> queue;
  rw_synchronized<std::unordered_map<std::thread::id, ThreadTimeKeeper>>
      keeper{};
  std::optional<OverviewReport> overview{std::nullopt};
  std::optional<CallstackReport> callstack{std::nullopt};
  std::thread report_writer;
};

class TimedBlock {
 public:
  TimedBlock(ThreadTimeKeeper& timer, const BlockIdentity& block) : timer{timer}
  {
    timer.enter(block);
  }
  ~TimedBlock() { timer.exit(); }
  void switch_to(const BlockIdentity& block) { timer.switch_to(block); }

 private:
  ThreadTimeKeeper& timer;
};

#endif  // BAREOS_LIB_TIME_STAMPS_H_
