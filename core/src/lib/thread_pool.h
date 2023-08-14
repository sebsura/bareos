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


class thread_pool {
 public:
  thread_pool(std::size_t num_threads);

  struct thread_id {
    std::size_t id;

    thread_id(std::size_t id) : id{id} {}

    explicit operator std::size_t() const { return id; }

    std::size_t get() const { return id; }
  };

  using task = std::function<void(thread_id)>;

  void enqueue(task&& t);
  void finish();   // wait until all submitted tasks are accomplished
  ~thread_pool();  // stops as fast as possible; ignoring outstanding tasks

 private:
  std::shared_mutex m;
  std::vector<std::thread> threads{};

  std::condition_variable queue_or_death;

  std::mutex queue_mut;
  std::deque<task> queue;

  std::atomic<bool> should_stop{false};

  std::size_t tasks_submitted{0};
  std::mutex task_mut;
  std::condition_variable task_cond;
  std::size_t tasks_completed{0};

  [[nodiscard]] std::shared_lock<std::shared_mutex> wait_until_init_complete();
  static void pool_work(std::size_t id, thread_pool* pool);

  std::optional<task> dequeue();

  std::optional<task> finish_and_dequeue();
  bool stop_requested();
};

template <typename F>
auto enqueue(thread_pool& pool, F&& f)
    -> std::future<std::invoke_result_t<F, thread_pool::thread_id>>
{
  using result_type = std::invoke_result_t<F, thread_pool::thread_id>;
  // todo(ssura): in c++23 we can use std::move_only_function
  //              and pass the promise directly into the function.
  //              currently this approach does not work because
  //              std::function requires the function to be copyable,
  //              but std::promise obviously is not.
  std::shared_ptr p = std::make_shared<std::promise<result_type>>();
  std::future fut = p->get_future();
  pool.enqueue(
      [f = std::move(f), mp = std::move(p)](thread_pool::thread_id id) mutable {
        try {
          result_type res = f(id);
          mp->set_value(res);
        } catch (...) {
          mp->set_exception(std::current_exception());
        }
      });
  return fut;
}

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
  pool.enqueue(
      [f = std::move(f), mp = std::move(p)](thread_pool::thread_id) mutable {
        try {
          if constexpr (std::is_same_v<result_type, void>) {
            f();
            mp->set_value();
          } else {
            result_type res = f();
            mp->set_value(res);
          }
        } catch (...) {
          mp->set_exception(std::current_exception());
        }
      });
  return fut;
}


#endif  // BAREOS_LIB_THREAD_POOL_H_
