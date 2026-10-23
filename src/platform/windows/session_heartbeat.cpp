/**
 * @file src/platform/windows/session_heartbeat.cpp
 * @brief Implementation of the virtual display watchdog keep-alive loop.
 */

// located in src/platform/windows

// standard includes
#include <algorithm>
#include <chrono>
#include <utility>

// local includes
#include "src/logging.h"
#include "src/platform/windows/session_heartbeat.h"

namespace platf::windows {
  using namespace std::literals;

  std::chrono::milliseconds heartbeat_interval(std::uint32_t watchdog_timeout_seconds) {
    if (watchdog_timeout_seconds == 0) {
      // Driver reports no timeout: it is disabled or unqueried, so use a safe default that is
      // cheap enough to be harmless and frequent enough to cover a driver that turns it on later.
      return HEARTBEAT_DEFAULT_INTERVAL;
    }

    const auto third = std::chrono::milliseconds {static_cast<long long>(watchdog_timeout_seconds) * 1000 / 3};
    return std::clamp(third, HEARTBEAT_MIN_INTERVAL, HEARTBEAT_MAX_INTERVAL);
  }

  session_heartbeat_t::session_heartbeat_t() :
      session_heartbeat_t([] { return true; }, HEARTBEAT_DEFAULT_INTERVAL) {}

  session_heartbeat_t::session_heartbeat_t(ping_fn ping, std::chrono::milliseconds interval) :
      _ping(std::move(ping)),
      _interval(interval) {
    if (_interval <= std::chrono::milliseconds::zero()) {
      // A non-positive interval would spin the CPU; that is the one value worth guarding.
      _interval = HEARTBEAT_DEFAULT_INTERVAL;
    }
  }

  session_heartbeat_t::~session_heartbeat_t() {
    stop();
  }

  void session_heartbeat_t::start() {
    std::lock_guard lock(_mutex);
    if (_worker.joinable()) {
      return;  // Already running; starting again must not spawn a second loop.
    }

    _stop_requested.store(false);
    _running.store(true);
    _worker = std::thread([this] { worker(); });
  }

  void session_heartbeat_t::stop() {
    // Join while holding the lock. The worker touches only atomics, so it never needs this mutex
    // and cannot deadlock; joining under the lock also prevents a concurrent start() from spawning
    // a second worker before this one has been reaped.
    std::lock_guard lock(_mutex);
    if (!_worker.joinable()) {
      return;  // Never started, or already stopped.
    }
    _stop_requested.store(true);
    _worker.join();
    _running.store(false);
  }

  bool session_heartbeat_t::running() const {
    return _running.load();
  }

  std::uint64_t session_heartbeat_t::ping_count() const {
    return _ping_count.load();
  }

  std::uint32_t session_heartbeat_t::consecutive_failures() const {
    return _consecutive_failures.load();
  }

  bool session_heartbeat_t::watchdog_dead() const {
    return _consecutive_failures.load() >= HEARTBEAT_MAX_FAILURES;
  }

  void session_heartbeat_t::worker() {
    for (;;) {
      // Interruptible sleep: exit promptly on stop rather than waiting out the full interval.
      const auto waited = _interval;
      auto remaining = waited;
      while (remaining > std::chrono::milliseconds::zero()) {
        if (_stop_requested.load()) {
          return;
        }
        const auto slice = std::min(remaining, std::chrono::milliseconds(20));
        std::this_thread::sleep_for(slice);
        remaining -= slice;
      }

      if (_stop_requested.load()) {
        return;
      }

      // The callback is operator-supplied and must not throw; guard anyway so a misbehaving
      // callback cannot terminate the process from inside a detached-in-spirit worker.
      bool ok = false;
      try {
        ok = _ping ? _ping() : true;
      } catch (...) {
        ok = false;
      }

      _ping_count.fetch_add(1);
      if (ok) {
        _consecutive_failures.store(0);
      } else {
        const auto failures = _consecutive_failures.fetch_add(1) + 1;
        if (failures >= HEARTBEAT_MAX_FAILURES) {
          BOOST_LOG(warning) << "Virtual display watchdog has not acknowledged "sv << failures
                             << " consecutive pings"sv;
        }
      }
    }
  }
}  // namespace platf::windows
