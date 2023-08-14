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
#include "lib/thread_pool.h"

thread_pool::thread_pool(std::size_t num_threads)
{
  std::unique_lock stop_threads(m);
  for (std::size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back(&pool_work, i, this);
  }
}

void thread_pool::enqueue(task&& t)
{
  std::unique_lock l{queue_mut};
  queue.push_back(std::move(t));
  tasks_submitted += 1;
  queue_or_death.notify_one();
}

void thread_pool::finish()
{
  std::unique_lock l{task_mut};

  task_cond.wait(l, [this]() { return tasks_submitted == tasks_completed; });
}

thread_pool::~thread_pool()
{
  should_stop = true;
  queue_or_death.notify_all();

  // wait until all threads are dead (i.e. they released their shared mtx)
  std::unique_lock l{m};

  for (auto& thread : threads) { thread.join(); }
}

void thread_pool::pool_work(std::size_t id, thread_pool* pool)
{
  auto lock = pool->wait_until_init_complete();

  thread_id my_id{id};

  for (std::optional my_task = pool->dequeue(); !!my_task;
       my_task = pool->finish_and_dequeue()) {
    (*my_task)(my_id);
  }
}

auto thread_pool::dequeue() -> std::optional<task>
{
  std::unique_lock l(queue_mut);
  queue_or_death.wait(
      l, [this]() { return queue.size() > 0 || should_stop.load(); });
  if (should_stop) { return std::nullopt; }
  task t = std::move(queue.front());
  queue.pop_front();
  return t;
}

auto thread_pool::finish_and_dequeue() -> std::optional<task>
{
  {
    std::unique_lock l{task_mut};
    tasks_completed += 1;
  }
  task_cond.notify_all();

  return dequeue();
}

bool thread_pool::stop_requested()
{
  return should_stop.load(std::memory_order_relaxed);
}

std::shared_lock<std::shared_mutex> thread_pool::wait_until_init_complete()
{
  return std::shared_lock{m};
}
