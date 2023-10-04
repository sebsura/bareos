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
/**
 * @file
 * simple thread pool
 */

#ifndef BAREOS_LIB_THREAD_POOL_H_
#define BAREOS_LIB_THREAD_POOL_H_

#include <vector>
#include <thread>
#include <optional>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <future>
#include <type_traits>
#include <exception>

#include "lib/thread_util.h"
#include "lib/channel.h"
#include "include/baconfig.h"

struct worker_pool {
  std::vector<std::thread> threads{};
  std::size_t num_borrowed{0};
  std::size_t min_workers{0};
  std::size_t dead_workers{0};

  std::size_t num_actual_workers() const
  {
    return threads.size() - num_borrowed - dead_workers;
  }
};

class thread_pool {
 public:
  // fixme(ssura): change this to std::move_only_function (c++23)
  using task = std::function<void()>;

  thread_pool(std::size_t num_threads = 0);

  void ensure_num_workers(std::size_t num_threads);
  void finish();   // wait until all submitted tasks are accomplished
  ~thread_pool();  // stops as fast as possible; dropping outstanding tasks

  void enqueue(task&& t);  // queue task to be worked on by worker threads;
                           // should not block
  void borrow_thread(task&& t);  // steal thread to work on task t; can block

 private:
  std::condition_variable worker_death;
  synchronized<worker_pool> workers{};

  std::condition_variable queue_or_death;
  synchronized<std::optional<std::deque<task>>> queue{std::in_place};

  std::size_t tasks_submitted{0};
  std::condition_variable on_task_completion;
  synchronized<std::size_t> tasks_completed{0u};

  static void pool_work(std::size_t id, thread_pool* pool);
  static void borrow_then_pool_work(task&& t,
                                    std::size_t id,
                                    thread_pool* pool);

  std::optional<task> dequeue();
  std::optional<task> finish_and_dequeue();
};

template <typename F>
auto enqueue(thread_pool& pool, F&& f) -> std::future<std::invoke_result_t<F>>
{
  using result_type = std::invoke_result_t<F>;
  // todo(ssura): in c++23 we can use std::move_only_function
  //              and pass the promise directly into the function.
  //              currently this approach does not work because
  //              std::function requires the function to be copyable,
  //              but std::promise obviously is not.
  std::shared_ptr p = std::make_shared<std::promise<result_type>>();
  std::future fut = p->get_future();
  pool.enqueue([f = std::move(f), mp = std::move(p)]() mutable {
    try {
      if constexpr (std::is_same_v<result_type, void>) {
        f();
        mp->set_value();
      } else {
        mp->set_value(f());
      }
    } catch (...) {
      mp->set_exception(std::current_exception());
    }
  });
  return fut;
}

template <typename F>
auto borrow_thread(thread_pool& pool, F&& f)
    -> std::future<std::invoke_result_t<F>>
{
  using result_type = std::invoke_result_t<F>;
  // todo(ssura): in c++23 we can use std::move_only_function
  //              and pass the promise directly into the function.
  //              currently this approach does not work because
  //              std::function requires the function to be copyable,
  //              but std::promise obviously is not.
  std::shared_ptr p = std::make_shared<std::promise<result_type>>();
  std::future fut = p->get_future();
  pool.borrow_thread([f = std::move(f), mp = std::move(p)]() mutable {
    try {
      if constexpr (std::is_same_v<result_type, void>) {
        f();
        mp->set_value();
      } else {
        mp->set_value(f());
      }
    } catch (...) {
      mp->set_exception(std::current_exception());
    }
  });
  return fut;
}

#include <memory>

class task {
  struct impl_base {
    virtual void operator()() = 0;
    virtual ~impl_base() = default;
  };

  template <typename F>
  struct impl : impl_base, F {
    impl(F&& f) : F(std::move(f))
    {}

    void operator()() override {
        F::operator()();
    }
  };

public:
  template <typename F,
  std::enable_if_t<!std::is_reference_v<F>, bool> = true,
  std::enable_if_t<!std::is_same_v<F, task>, bool> = true>
  task(F&& f) : ptr{new impl<F>(std::move(f))}
  {
  }

  task(task&&) = default;
  task& operator=(task&&) = default;

  void operator()() {
    ptr->operator()();
  }

private:
  std::unique_ptr<impl_base> ptr;
};

struct tpool {
  std::vector<std::size_t> find_free_threads(std::size_t size)
  {
    std::vector<size_t> free_threads;
    free_threads.reserve(threads.size());
    for (std::size_t i = 0; i < threads.size() && free_threads.size() < size;
         ++i) {
      if (auto locked = units[i]->try_lock();
	  locked && locked.value()->is_waiting()) { free_threads.push_back(i); }
    }

    if (free_threads.size() < size) {
      std::size_t num_new_hires = size - free_threads.size();

      for (std::size_t i = 0; i < num_new_hires; ++i) {
        free_threads.push_back(threads.size());
        add_thread();
      }
    }

    ASSERT(free_threads.size() == size);

    return free_threads;
  }

  template <typename F> void borrow_thread(F&& f)
  {
    std::vector worker = find_free_threads(1);
    units[worker[0]]->lock()->submit(std::move(f));
  }
  template <typename F> void borrow_threads(std::size_t size, F&& f)
  {
    std::vector free_threads = find_free_threads(size);
    for (auto index : free_threads) { units[index]->lock()->submit(f); }
  }

  ~tpool()
  {
    for (auto sync : units) { sync->lock()->close(); }

    for (auto& thread : threads) { thread.join(); }
  }

 private:
  class work_unit {
   public:
    template <typename F> void submit(F f)
    {
      ASSERT(state == work_state::WAITING);
      fun = std::move(f);
      state = work_state::WORKING;
      state_changed.notify_all();
    }

    void close()
    {
      state = work_state::CLOSED;
      fun.reset();
      state_changed.notify_all();
    }

    bool is_waiting() const { return state == work_state::WAITING; }

    bool is_closed() const { return state == work_state::CLOSED; }

    void do_work()
    {
      ASSERT(state == work_state::WORKING);
      ASSERT(fun.has_value());
      fun.value()();
      fun.reset();
      state = work_state::WAITING;
      state_changed.notify_all();
    }

    std::condition_variable state_changed;

   private:
    enum class work_state
    {
      WORKING,
      WAITING,
      CLOSED
    };
    work_state state{work_state::WAITING};
    std::optional<task> fun;
  };

  std::vector<synchronized<work_unit>*> units;
  std::vector<std::thread> threads{};

  void add_thread()
  {
    auto* sync = new synchronized<work_unit>();
    units.push_back(sync);
    threads.emplace_back(
        [](tpool* pool, synchronized<work_unit>* unit) {
          pool->pool_work(*unit);
        },
        this, sync);
  }

  void pool_work(synchronized<work_unit>& unit)
  {
    auto locked = unit.lock();
    for (;;) {
      locked.wait(locked->state_changed,
                  [](const work_unit& w) { return !w.is_waiting(); });

      if (locked->is_closed()) { return; }

      locked->do_work();
    }
  }
};

struct work_group {
  void work_until_completion()
  {
    std::optional<task> my_task;
    while (my_task = task_out.lock()->get(), my_task != std::nullopt) {
      my_task->operator()();
    }
  }

  template <typename F,
            typename T = std::invoke_result_t<F>>
  std::future<T> submit(F&& f)
  {
    std::promise<T> prom;
    std::future ret = prom.get_future();
    task t{[prom = std::move(prom), f = std::move(f)]() mutable {
      try {
	if constexpr (std::is_same_v<T, void>) {
	  f();
	  prom.set_value();
	} else {
	  prom.set_value(f());
	}
      } catch (...) {
	prom.set_exception(std::current_exception());
      }
    }};

    task_in.emplace(std::move(t));
    return ret;
  }

  void shutdown() {
    task_in.close();
  }

  channel::input<task> task_in;
  synchronized<channel::output<task>> task_out;

  work_group(channel::channel_pair<task> tasks)
      : task_in{std::move(tasks.first)}, task_out{std::move(tasks.second)}
  {
  }
  work_group(std::size_t cap)
      : work_group(channel::CreateBufferedChannel<task>(cap))
  {
  }

  ~work_group() {
    shutdown();
  }
};

#endif  // BAREOS_LIB_THREAD_POOL_H_
