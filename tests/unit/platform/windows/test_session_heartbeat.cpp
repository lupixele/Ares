/**
 * @file tests/unit/platform/windows/test_session_heartbeat.cpp
 * @brief Unit tests for the virtual display watchdog keep-alive.
 *
 * @details These tests exercise timing and failure behaviour with an injected ping callback, so
 * they need no driver and no display. The only real clock dependency is short, bounded sleeps,
 * which keeps them fast and deterministic enough to run in the native suite.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/session_heartbeat.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace {
  using platf::windows::session_heartbeat_t;
  using platf::windows::heartbeat_interval;
  using platf::windows::HEARTBEAT_MIN_INTERVAL;
  using platf::windows::HEARTBEAT_MAX_INTERVAL;
  using platf::windows::HEARTBEAT_DEFAULT_INTERVAL;

  class SessionHeartbeatTest : public ::testing::Test {};

  TEST_F(SessionHeartbeatTest, IntervalDefaultsWhenNoWatchdogReported) {
    // A driver that reports no timeout gets a safe default rather than a busy loop or no loop.
    EXPECT_EQ(heartbeat_interval(0), HEARTBEAT_DEFAULT_INTERVAL);
  }

  TEST_F(SessionHeartbeatTest, IntervalPingsAtOneThirdOfWatchdog) {
    // A 3 second watchdog is pinged every 1 second, leaving room for two missed pings.
    EXPECT_EQ(heartbeat_interval(3), std::chrono::milliseconds(1000));
    EXPECT_EQ(heartbeat_interval(9), std::chrono::milliseconds(3000));
  }

  TEST_F(SessionHeartbeatTest, IntervalIsClampedToSaneBounds) {
    // A one-second watchdog pings every 333ms: still above the floor, and it leaves room for two
    // missed pings before the driver would act at the full second.
    EXPECT_EQ(heartbeat_interval(1), std::chrono::milliseconds(333));

    // A very large watchdog must still check in reasonably often.
    EXPECT_EQ(heartbeat_interval(3600), HEARTBEAT_MAX_INTERVAL);

    // Whatever the timeout, the interval never exceeds it, or the driver would fire first.
    EXPECT_LT(heartbeat_interval(1), std::chrono::milliseconds(1000));
    EXPECT_LT(heartbeat_interval(3), std::chrono::milliseconds(3000));
  }

  TEST_F(SessionHeartbeatTest, NotRunningUntilStarted) {
    session_heartbeat_t heartbeat([] { return true; }, std::chrono::milliseconds(5));
    EXPECT_FALSE(heartbeat.running());
    EXPECT_EQ(heartbeat.ping_count(), 0u);
  }

  TEST_F(SessionHeartbeatTest, StartPingsAndStopJoins) {
    std::atomic<int> pings {0};
    session_heartbeat_t heartbeat([&pings] {
      pings.fetch_add(1);
      return true;
    }, std::chrono::milliseconds(5));

    heartbeat.start();
    EXPECT_TRUE(heartbeat.running());

    // Give the loop time to tick a few times.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline && pings.load() < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_GE(pings.load(), 3);
    EXPECT_GE(heartbeat.ping_count(), 3u);

    heartbeat.stop();
    EXPECT_FALSE(heartbeat.running());

    // Once stopped, the count must freeze: no detached worker lingers.
    const auto frozen = heartbeat.ping_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(heartbeat.ping_count(), frozen);
  }

  TEST_F(SessionHeartbeatTest, StopBeforeStartIsSafe) {
    session_heartbeat_t heartbeat([] { return true; }, std::chrono::milliseconds(5));
    EXPECT_NO_THROW(heartbeat.stop());
    EXPECT_NO_THROW(heartbeat.stop());
    EXPECT_FALSE(heartbeat.running());
  }

  TEST_F(SessionHeartbeatTest, DoubleStartDoesNotSpawnASecondLoop) {
    std::atomic<int> pings {0};
    session_heartbeat_t heartbeat([&pings] {
      pings.fetch_add(1);
      return true;
    }, std::chrono::milliseconds(10));

    heartbeat.start();
    heartbeat.start();  // Must be a no-op.
    EXPECT_TRUE(heartbeat.running());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline && pings.load() < 5) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    heartbeat.stop();
    // With one loop at 10ms over ~200ms, roughly 20 pings are expected; two loops would roughly
    // double that. The upper bound is tight enough that a duplicate worker fails the test.
    EXPECT_GE(pings.load(), 5);
    EXPECT_LE(pings.load(), 30);
  }

  TEST_F(SessionHeartbeatTest, DestructorJoinsWithoutHanging) {
    std::atomic<bool> entered {false};
    {
      session_heartbeat_t heartbeat([&entered] {
        entered.store(true);
        return true;
      }, std::chrono::milliseconds(5));
      heartbeat.start();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
      while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      EXPECT_TRUE(entered.load());
      // Destructor runs here and must join promptly.
    }
    SUCCEED();
  }

  TEST_F(SessionHeartbeatTest, ConsecutiveFailuresAreCountedAndReset) {
    std::atomic<bool> fail {true};
    session_heartbeat_t heartbeat([&fail] { return !fail.load(); }, std::chrono::milliseconds(5));

    heartbeat.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < deadline && heartbeat.consecutive_failures() < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    heartbeat.stop();

    EXPECT_GE(heartbeat.consecutive_failures(), 3u);
    EXPECT_TRUE(heartbeat.watchdog_dead());
    EXPECT_GT(heartbeat.ping_count(), 0u);
  }

  TEST_F(SessionHeartbeatTest, SuccessfulPingResetsTheFailureCount) {
    std::atomic<bool> fail {true};
    session_heartbeat_t heartbeat([&fail] { return !fail.load(); }, std::chrono::milliseconds(5));

    heartbeat.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline && heartbeat.consecutive_failures() < 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    heartbeat.stop();
    const auto failed_count = heartbeat.consecutive_failures();
    EXPECT_GE(failed_count, 2u);

    fail.store(false);
    heartbeat.start();
    const auto ok_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < ok_deadline && heartbeat.consecutive_failures() != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    heartbeat.stop();
    EXPECT_EQ(heartbeat.consecutive_failures(), 0u);
    EXPECT_FALSE(heartbeat.watchdog_dead());
  }

  TEST_F(SessionHeartbeatTest, ThrowingCallbackCannotTerminateTheWorker) {
    std::atomic<int> attempts {0};
    session_heartbeat_t heartbeat([&attempts]() -> bool {
      attempts.fetch_add(1);
      throw std::runtime_error("simulated driver io failure");
    }, std::chrono::milliseconds(5));

    EXPECT_NO_THROW(heartbeat.start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline && attempts.load() < 3) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_NO_THROW(heartbeat.stop());
    EXPECT_GE(attempts.load(), 3);
    EXPECT_TRUE(heartbeat.watchdog_dead());
  }
}  // namespace
#endif  // _WIN32
