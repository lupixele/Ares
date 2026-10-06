/**
 * @file src/thread_pool.h
 * @brief Declarations for the thread pool system.
 */
#pragma once

// standard includes
#include <atomic>
#include <cassert>
#include <chrono>
#include <optional>
#include <thread>

// local includes
#include "platform/common.h"
#include "task_pool.h"

namespace thread_pool_util {
  /**
   * Allow threads to execute unhindered while keeping full control over the threads.
   */
  class ThreadPool: public task_pool_util::TaskPool {
  public:
    /**
     * @brief Callable unit executed by a worker thread.
     */
    typedef TaskPool::__task __task;

  private:
    static inline thread_local const ThreadPool *_current_worker_pool {nullptr};  ///< Active thread pool instance executing on the calling thread.

    std::mutex _lifecycle_lock;  ///< Synchronizes worker thread startup and thread list lifecycle operations.
    std::vector<std::jthread> _thread;  ///< Worker threads owned by the pool.

    std::condition_variable _cv;  ///< Coordinates work availability and quiescence.
    std::mutex _lock;  ///< Synchronizes task queue operations and lifecycle state.

    std::atomic<bool> _continue {false};  ///< Flag indicating whether worker threads should continue running.
    std::size_t _active_workers {0};  ///< Number of currently active worker threads. Guarded by _lock.

  public:
    ThreadPool():
        _continue {false},
        _active_workers {0} {
    }

    /**
     * @brief Start a pool with the requested number of worker threads.
     *
     * @param threads Number of worker threads to start.
     */
    explicit ThreadPool(int threads):
        _continue {false},
        _active_workers {0} {
      start(threads);
    }

    ~ThreadPool() noexcept {
      stop();
      join();
    }

    /**
     * @brief Queue work for asynchronous execution.
     *
     * @param newTask New task.
     * @param args Arguments forwarded to the callable or parser.
     * @return Identifier assigned to the queued task.
     */
    template<class Function, class... Args>
    auto push(Function &&newTask, Args &&...args) {
      std::lock_guard lg(_lock);
      auto future = TaskPool::push(std::forward<Function>(newTask), std::forward<Args>(args)...);

      _cv.notify_one();
      return future;
    }

    /**
     * @brief Queue work for asynchronous execution only if the thread pool is currently running.
     *
     * Checks _continue under the same lock as stop before enqueuing.
     *
     * @param newTask New task.
     * @param args Arguments forwarded to the callable or parser.
     * @return Optional future of the queued task, or std::nullopt if the pool is stopping or stopped.
     */
    template<class Function, class... Args>
    auto push_if_running(Function &&newTask, Args &&...args)
      -> std::optional<decltype(TaskPool::push(std::forward<Function>(newTask), std::forward<Args>(args)...))> {
      std::lock_guard lg(_lock);
      if (!_continue.load(std::memory_order_relaxed)) {
        return std::nullopt;
      }
      auto future = TaskPool::push(std::forward<Function>(newTask), std::forward<Args>(args)...);

      _cv.notify_one();
      return future;
    }

    /**
     * @brief Queue a task that becomes runnable after a delay.
     *
     * @param task Task object to enqueue or execute.
     */
    void pushDelayed(std::pair<__time_point, __task> &&task) {
      std::lock_guard lg(_lock);

      TaskPool::pushDelayed(std::move(task));
    }

    /**
     * @brief Queue a task that becomes runnable after a delay.
     *
     * @param newTask New task.
     * @param duration Delay before the timed task should run.
     * @param args Arguments forwarded to the callable or parser.
     * @return Future that becomes ready after the delayed task completes.
     */
    template<class Function, class X, class Y, class... Args>
    auto pushDelayed(Function &&newTask, std::chrono::duration<X, Y> duration, Args &&...args) {
      std::lock_guard lg(_lock);
      auto future = TaskPool::pushDelayed(std::forward<Function>(newTask), duration, std::forward<Args>(args)...);

      // Update all timers for wait_until
      _cv.notify_all();
      return future;
    }

    /**
     * @brief Start worker threads for queued thread-pool tasks.
     *
     * @param threads Number of worker threads to start.
     */
    void start(int threads) {
      if (threads <= 0) {
        return;
      }

      std::lock_guard lifecycle_guard(_lifecycle_lock);

      if (_continue.load(std::memory_order_acquire)) {
        return;
      }

      for (auto &t : _thread) {
        if (t.joinable()) {
          t.join();
        }
      }
      _thread.clear();

      {
        std::lock_guard lg(_lock);
        _continue.store(true, std::memory_order_release);
        _active_workers += static_cast<std::size_t>(threads);
      }

      std::size_t spawned = 0;
      try {
        _thread.reserve(static_cast<std::size_t>(threads));
        for (int i = 0; i < threads; ++i) {
          _thread.emplace_back(&ThreadPool::_main, this);
          ++spawned;
        }
      } catch (...) {
        {
          std::lock_guard lg(_lock);
          _continue.store(false, std::memory_order_release);
          _active_workers -= (static_cast<std::size_t>(threads) - spawned);
        }
        _cv.notify_all();
        for (auto &t : _thread) {
          if (t.joinable()) {
            t.join();
          }
        }
        _thread.clear();
        throw;
      }
    }

    /**
     * @brief Stop worker threads and prevent additional task execution.
     */
    void stop() {
      {
        std::lock_guard lg(_lock);
        _continue.store(false, std::memory_order_release);
      }
      _cv.notify_all();
    }

    /**
     * @brief Wait for worker threads owned by the session to exit.
     */
    void join() {
      std::lock_guard lifecycle_guard(_lifecycle_lock);
      for (auto &t : _thread) {
        if (t.joinable()) {
          t.join();
        }
      }
      _thread.clear();
    }

    /**
     * @brief Wait until all worker threads have completed task execution and quiesced.
     *
     * Must only be called from an external thread (asserts calling thread is not a worker).
     * Does not join worker threads.
     */
    void wait_for_quiescence() {
      assert(!is_worker_thread());
      std::unique_lock lg(_lock);
      _cv.wait(lg, [this]() {
        return _active_workers == 0;
      });
    }

    /**
     * @brief Wait with a timeout until all worker threads have completed task execution and quiesced.
     *
     * @param timeout Maximum duration to wait for worker quiescence.
     * @return True if all workers quiesced within the timeout, false otherwise.
     */
    template<class Rep, class Period>
    bool wait_for_quiescence(const std::chrono::duration<Rep, Period> &timeout) {
      assert(!is_worker_thread());
      std::unique_lock lg(_lock);
      return _cv.wait_for(lg, timeout, [this]() {
        return _active_workers == 0;
      });
    }

    /**
     * @brief Return the number of currently active worker threads.
     *
     * @return Number of active worker threads.
     */
    [[nodiscard]] std::size_t active_workers() const noexcept {
      std::lock_guard lg(const_cast<std::mutex &>(_lock));
      return _active_workers;
    }

    /**
     * @brief Check whether worker threads are available to execute queued tasks.
     *
     * @return True while the thread pool is running.
     */
    [[nodiscard]] bool running() const noexcept {
      return _continue.load(std::memory_order_acquire);
    }

    /**
     * @brief Check whether the current calling thread is a worker thread of this pool.
     *
     * @return True if the calling thread is executing as a worker thread belonging to this pool.
     */
    [[nodiscard]] bool is_worker_thread() const noexcept {
      return _current_worker_pool == this;
    }

  public:
    /**
     * @brief Run the main application or worker loop.
     */
    void _main() {
      struct WorkerScope {
        const ThreadPool *prev;  ///< Previously active thread pool pointer.
        explicit WorkerScope(const ThreadPool *pool) noexcept:
            prev {_current_worker_pool} {
          _current_worker_pool = pool;
        }
        ~WorkerScope() noexcept {
          _current_worker_pool = prev;
        }
      } scope(this);

      struct WorkerExitGuard {
        ThreadPool *pool;  ///< Owning thread pool instance.
        ~WorkerExitGuard() noexcept {
          std::lock_guard lg(pool->_lock);
          if (pool->_active_workers > 0) {
            --pool->_active_workers;
          }
          if (pool->_active_workers == 0) {
            pool->_cv.notify_all();
          }
        }
      } exit_guard {this};

      platf::set_thread_name("TaskPool::worker");
      while (_continue.load(std::memory_order_relaxed)) {
        if (auto task = this->pop()) {
          (*task)->run();
        } else {
          std::unique_lock uniq_lock(_lock);

          if (ready()) {
            continue;
          }

          if (!_continue.load(std::memory_order_relaxed)) {
            break;
          }

          if (auto tp = next()) {
            _cv.wait_until(uniq_lock, *tp);
          } else {
            _cv.wait(uniq_lock);
          }
        }
      }

      // Execute remaining tasks
      while (auto task = this->pop()) {
        (*task)->run();
      }
    }
  };
}  // namespace thread_pool_util
