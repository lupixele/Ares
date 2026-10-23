/**
 * @file src/platform/windows/session_heartbeat.h
 * @brief Periodic watchdog keep-alive for virtual displays held by streaming sessions.
 *
 * @details The SudoVDA driver can run a watchdog that removes a virtual display if nothing keeps
 * it alive. A long streaming session must therefore ping the driver, or the display it is streaming
 * to can be silently torn down mid-session.
 *
 * This class owns that ping loop. It is deliberately independent of the driver and of the session
 * manager: it takes a callback and an interval, so its timing and failure behaviour are testable
 * without any display existing.
 *
 * Guarantees:
 * - `stop()` always joins. There is no detached worker outliving the object.
 * - No operation throws, and the worker thread itself never throws.
 * - Ping failures are counted and surfaced; the loop never crashes or hangs on them.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace platf::windows {
  /**
   * @brief Maximum interval the watchdog loop will wait between pings.
   */
  constexpr std::chrono::milliseconds HEARTBEAT_MAX_INTERVAL(5000);

  /**
   * @brief Minimum interval the watchdog loop will wait between pings.
   */
  constexpr std::chrono::milliseconds HEARTBEAT_MIN_INTERVAL(200);

  /**
   * @brief Interval used when the driver does not report a watchdog timeout.
   */
  constexpr std::chrono::milliseconds HEARTBEAT_DEFAULT_INTERVAL(1000);

  /**
   * @brief Consecutive ping failures tolerated before the loop reports the watchdog as dead.
   */
  constexpr std::uint32_t HEARTBEAT_MAX_FAILURES = 3;

  /**
   * @brief Decide how often the watchdog should be pinged, given the driver's timeout.
   *
   * @param watchdog_timeout_seconds Driver-reported watchdog timeout in seconds, 0 when unknown.
   * @return Interval to wait between pings.
   *
   * @note When the driver reports a timeout, pinging at one third of it leaves room for two missed
   *       pings before the driver acts, which matches the tolerance built into HEARTBEAT_MAX_FAILURES.
   */
  [[nodiscard]] std::chrono::milliseconds heartbeat_interval(std::uint32_t watchdog_timeout_seconds);

  /**
   * @brief Periodic keep-alive loop for a driver watchdog.
   */
  class session_heartbeat_t {
  public:
    /**
     * @brief Callback invoked on each ping; must return true when the driver acknowledged.
     */
    using ping_fn = std::function<bool()>;

    /**
     * @brief Construct a heartbeat that does nothing until started.
     */
    session_heartbeat_t();

    /**
     * @brief Construct a heartbeat with a live ping callback.
     * @param ping Callback invoked on each tick; must not throw.
     * @param interval Time to wait between ticks, used as given.
     *
     * @note No clamping is applied here. This type is a low-level utility, and production callers
     *       should derive their interval from @ref heartbeat_interval, which enforces the sane
     *       bounds. Clamping here would make it impossible to exercise fast timing in tests.
     */
    session_heartbeat_t(ping_fn ping, std::chrono::milliseconds interval);

    ~session_heartbeat_t();

    session_heartbeat_t(const session_heartbeat_t &) = delete;
    session_heartbeat_t &operator=(const session_heartbeat_t &) = delete;
    session_heartbeat_t(session_heartbeat_t &&) = delete;
    session_heartbeat_t &operator=(session_heartbeat_t &&) = delete;

    /**
     * @brief Begin pinging on the configured interval.
     * @note Idempotent: starting an already-running heartbeat does nothing.
     */
    void start();

    /**
     * @brief Stop pinging and wait for the worker to finish.
     * @note Idempotent and safe to call from any thread, including a destructor path. Never blocks
     *       longer than one interval plus one ping.
     */
    void stop();

    /**
     * @brief Whether the loop is currently running.
     * @return True while pinging.
     */
    [[nodiscard]] bool running() const;

    /**
     * @brief Number of pings that have been sent since construction or the last reset.
     * @return Ping count.
     */
    [[nodiscard]] std::uint64_t ping_count() const;

    /**
     * @brief Current run of consecutive unacknowledged pings.
     * @return Failure count since the last success.
     */
    [[nodiscard]] std::uint32_t consecutive_failures() const;

    /**
     * @brief Whether the watchdog is considered dead after too many failures.
     * @return True when the failure tolerance has been exceeded.
     */
    [[nodiscard]] bool watchdog_dead() const;

  private:
    /**
     * @brief Worker loop: sleep, ping, record the result, until stopped.
     */
    void worker();

    ping_fn _ping;                                    ///< Callback invoked on each tick.
    std::chrono::milliseconds _interval;              ///< Time to wait between ticks.
    mutable std::mutex _mutex;                        ///< Guards the worker thread and counters.
    std::thread _worker;                              ///< Ping loop worker, empty when stopped.
    std::atomic<bool> _stop_requested {false};        ///< Signals the worker to exit.
    std::atomic<bool> _running {false};               ///< Whether the worker is pinging.
    std::atomic<std::uint64_t> _ping_count {0};       ///< Total pings sent.
    std::atomic<std::uint32_t> _consecutive_failures {0};  ///< Pings since the last success.
  };
}  // namespace platf::windows
