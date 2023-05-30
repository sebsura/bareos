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

#include <lib/time_stamps.h>

#include <mutex>
#include <cassert>
#include <algorithm>

#include "lib/message.h"
#include "include/messages.h"

ThreadTimeKeeper::~ThreadTimeKeeper()
{
  queue.lock()->put(std::move(buffer));
}


static void FlushEventsIfNecessary(synchronized<channel::in<EventBuffer>>& queue,
                                   std::thread::id this_id,
                                   const std::vector<event::OpenEvent>& stack,
                                   EventBuffer& buffer)
{
  constexpr std::size_t buffer_filled_ok = 1000;
  if (buffer.size() >= buffer_filled_ok) {
    if (std::optional locked = queue.try_lock();
	locked.has_value()) {
      if ((*locked)->try_put(buffer)) {
	buffer = EventBuffer(this_id, ThreadTimeKeeper::event_buffer_init_capacity, stack);
      }
    }
  }
}

void ThreadTimeKeeper::enter(const BlockIdentity& block)
{
  FlushEventsIfNecessary(queue, this_id, stack, buffer);
  auto& event = stack.emplace_back(block);
  buffer.emplace_back(event);
}

void ThreadTimeKeeper::switch_to(const BlockIdentity& block)
{
  FlushEventsIfNecessary(queue, this_id, stack, buffer);
  ASSERT(stack.size() != 0);
  auto event = stack.back().close();
  buffer.emplace_back(event);
  stack.back() = event::OpenEvent(block);
  buffer.emplace_back(stack.back());
}

void ThreadTimeKeeper::exit(const BlockIdentity& block)
{
  FlushEventsIfNecessary(queue, this_id, stack, buffer);
  ASSERT(stack.size() != 0);
  auto event = stack.back().close();
  ASSERT(event.source == &block);
  buffer.emplace_back(event);
  stack.pop_back();
}

static void write_reports(ReportGenerator* gen,
                          channel::out<EventBuffer> queue)
{
  auto start = event::clock::now();
  gen->begin_report(start);
  for (;;) {
    if (std::optional opt = queue.get_all(); opt.has_value()) {
      for (EventBuffer& buf : opt.value()) {
	gen->add_events(buf);
      }
    } else {
      break;
    }
  }
  auto end = event::clock::now();
  gen->end_report(end);
}

TimeKeeper::TimeKeeper(bool enabled,
    std::pair<channel::in<EventBuffer>, channel::out<EventBuffer>> p)
    : enabled{enabled}
    , queue{std::move(p.first)}
    , report_writer{&write_reports, &callstack,
                    std::move(p.second)}
{
}

ThreadHandle TimeKeeper::get_thread_local()
{
  if (enabled) {
    // this is most likely just a read from a thread local variable
    // anyways, so we do not need to store this inside a threadlocal ourselves
    std::thread::id my_id = std::this_thread::get_id();
    {
      auto locked = keeper.rlock();
      if (auto found = locked->find(my_id); found != locked->end()) {
	return ThreadHandle{&const_cast<ThreadTimeKeeper&>(found->second)};
      }
    }
    {
      auto [iter, inserted] = keeper.wlock()->emplace(std::piecewise_construct,
						      std::forward_as_tuple(my_id),
						      std::forward_as_tuple(queue));
      ASSERT(inserted);
      return ThreadHandle{&iter->second};
    }
  } else {
    return ThreadHandle{};
  }
}
