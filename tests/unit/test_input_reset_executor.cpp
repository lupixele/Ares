/**
 * @file tests/unit/test_input_reset_executor.cpp
 * @brief Tests for bounded input reset execution, worker reentrancy, and timer cancellation.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"
#include "src/thread_pool.h"
#include "src/utility.h"

using namespace std::literals;

namespace {
  /**
   * @brief Fixture verifying thread pool worker reentrancy, bounded input reset execution, and timer safety.
   */
  class InputResetExecutorTest: public ::testing::Test {
  protected:
    config::input_t original_input_;  ///< Saved input configuration restored in tear down.
    platf::virtualhid::input_context_t *context_ {nullptr};  ///< Active fake virtualhid context.
    lvh::Runtime *runtime_ {nullptr};  ///< Fake virtualhid runtime backing the platform context.

    /**
     * @brief Configure fake platform input devices and start the worker thread pool.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.keyboard = true;
      config::input.mouse = true;
      config::input.gamepad = "xseries";

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));

      if (!task_pool.running()) {
        task_pool.start(1);
      }
    }

    /**
     * @brief Quiesce the worker pool, restore configuration, and destroy retained session state.
     */
    void TearDown() override {
      if (task_pool.running()) {
        task_pool.stop();
        task_pool.join();
      }

      input::testing::set_keyboard_sink({});
      input::testing::set_input_packet_hook({});
      input::testing::set_input_task_sink({});
      input::testing::reset_keyboard_state();
      input::terminate_gamepads();
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }
  };
}  // namespace

/**
 * @brief Verify that ThreadPool accurately detects whether the current calling thread is a pool worker.
 */
TEST_F(InputResetExecutorTest, WorkerThreadIdentityDetection) {
  EXPECT_FALSE(task_pool.is_worker_thread());

  std::promise<bool> is_worker_promise;
  auto is_worker_future = is_worker_promise.get_future();

  task_pool.push([&is_worker_promise]() {
    is_worker_promise.set_value(task_pool.is_worker_thread());
  });

  ASSERT_EQ(is_worker_future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(is_worker_future.get());

  EXPECT_FALSE(task_pool.is_worker_thread());
}

/**
 * @brief Repeated start must not join a worker whose loop is still running.
 */
TEST_F(InputResetExecutorTest, StartingAnAlreadyRunningPoolIsIdempotent) {
  auto finished = std::make_shared<std::atomic<bool>>(false);
  std::jthread watchdog([finished] {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!finished->load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(10ms);
    }
    if (!finished->load()) {
      std::_Exit(1);
    }
  });
  task_pool.start(1);
  EXPECT_TRUE(task_pool.running());
  EXPECT_TRUE(task_pool.push([] { return task_pool.is_worker_thread(); }).get());
  finished->store(true);
}

/**
 * @brief Shutdown drains accepted reset work and rejects later handoffs.
 */
TEST_F(InputResetExecutorTest, AcceptedHandoffDrainsBeforeQuiescence) {
  auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  task_pool.push([entered, released] {
    entered->set_value();
    if (released.wait_for(2s) != std::future_status::ready) {
      std::_Exit(1);
    }
  });
  if (entered_future.wait_for(2s) != std::future_status::ready) {
    std::_Exit(1);
  }
  auto completed = std::make_shared<std::atomic<bool>>(false);
  auto accepted = task_pool.push_if_running([completed] { completed->store(true); });
  ASSERT_TRUE(accepted);
  task_pool.stop();
  EXPECT_FALSE(task_pool.push_if_running([] {}).has_value());
  EXPECT_FALSE(completed->load());
  release->set_value();
  if (accepted->wait_for(2s) != std::future_status::ready) {
    std::_Exit(1);
  }
  accepted->get();
  task_pool.wait_for_quiescence();
  EXPECT_TRUE(completed->load());
  task_pool.join();
}

/**
 * @brief Verify that resetting input from within a worker thread executes inline without self-deadlock.
 */
TEST_F(InputResetExecutorTest, ResetFromWorkerThreadInline) {
  std::atomic<bool> test_finished {false};
  std::thread watchdog([&test_finished]() {
    for (int i = 0; i < 20; ++i) {
      if (test_finished.load()) {
        return;
      }
      std::this_thread::sleep_for(100ms);
    }
    if (!test_finished.load()) {
      // Fail fast to prevent fixture teardown from hanging on deadlocked worker join
      std::_Exit(1);
    }
  });

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "worker-inline-reset");
  ASSERT_NE(stream_input, nullptr);

  auto completed_promise = std::make_shared<std::promise<bool>>();
  auto completed_future = completed_promise->get_future();

  task_pool.push([stream_input, completed_promise]() {
    auto mut_input = stream_input;
    input::reset(mut_input);
    completed_promise->set_value(input::testing::is_input_stopped(stream_input));
  });

  ASSERT_EQ(completed_future.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(completed_future.get());
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));

  test_finished.store(true);
  watchdog.join();
}

#if GTEST_HAS_DEATH_TEST
/**
 * @brief Subprocess death-test verifying worker context reset exits cleanly with fail-fast bounding.
 */
TEST_F(InputResetExecutorTest, WorkerContextResetSubprocessBounded) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
    {
      std::thread watchdog([]() {
        std::this_thread::sleep_for(3s);
        std::_Exit(1);
      });
      watchdog.detach();

      if (!task_pool.running()) {
        task_pool.start(1);
      }

      auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "child-worker-reset");
      if (!stream_input) {
        std::_Exit(2);
      }

      auto done = std::make_shared<std::promise<bool>>();
      auto fut = done->get_future();

      task_pool.push([stream_input, done]() {
        auto mut_input = stream_input;
        input::reset(mut_input);
        done->set_value(input::testing::is_input_stopped(stream_input));
      });

      if (fut.wait_for(2s) == std::future_status::ready && fut.get()) {
        std::_Exit(0);
      } else {
        std::_Exit(3);
      }
    },
    ::testing::ExitedWithCode(0),
    ""
  );
}
#endif

/**
 * @brief Verify that external reset drains queued unhandled input packets and marks context stopped.
 */
TEST_F(InputResetExecutorTest, ExternalResetDrainsQueueAndSetsStopped) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "drain-and-stop");
  ASSERT_NE(stream_input, nullptr);

  NV_KEYBOARD_PACKET key_packet {};
  key_packet.header.size = util::endian::big<std::uint32_t>(sizeof(key_packet) - sizeof(key_packet.header.size));
  key_packet.header.magic = util::endian::little(KEY_DOWN_EVENT_MAGIC);
  key_packet.keyCode = 0x41;

  std::vector<std::uint8_t> key_bytes(sizeof(key_packet));
  std::memcpy(key_bytes.data(), &key_packet, sizeof(key_packet));

  input::testing::set_input_task_sink([](auto) {});
  input::passthrough(stream_input, std::move(key_bytes));

  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 1u);
  EXPECT_FALSE(input::testing::is_input_stopped(stream_input));

  input::reset(stream_input);

  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0u);
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));

  std::vector<std::uint8_t> key_bytes2(sizeof(key_packet));
  std::memcpy(key_bytes2.data(), &key_packet, sizeof(key_packet));
  input::passthrough(stream_input, std::move(key_bytes2));
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0u);
}

/**
 * @brief Verify external reset blocks on active worker task without inline fallback before barrier release.
 */
TEST_F(InputResetExecutorTest, ExternalResetBlocksOnWorkerTaskWithoutFallback) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "external-pause-test");
  ASSERT_NE(stream_input, nullptr);

  auto worker_paused = std::make_shared<std::promise<void>>();
  auto release_worker = std::make_shared<std::promise<void>>();
  auto paused = worker_paused->get_future();
  auto release = release_worker->get_future().share();

  task_pool.push([worker_paused, release]() {
    worker_paused->set_value();
    release.wait();
  });

  auto release_on_exit = util::fail_guard([release_worker]() {
    try {
      release_worker->set_value();
    } catch (const std::future_error &) {
    }
  });
  ASSERT_EQ(paused.wait_for(2s), std::future_status::ready);

  auto reset_future = std::async(std::launch::async, [stream_input]() mutable {
    input::reset(stream_input);
  });

  EXPECT_EQ(reset_future.wait_for(50ms), std::future_status::timeout);

  release_worker->set_value();

  if (reset_future.wait_for(2s) != std::future_status::ready) {
    // An async future destructor would otherwise join the same hung reset.
    std::_Exit(1);
  }
  reset_future.get();
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));
}

/**
 * @brief Verify reset called after pool stop waits for in-flight worker task quiescence before inline execution.
 */
TEST_F(InputResetExecutorTest, ResetAfterPoolStopWaitsForInFlightWorkerQuiescence) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "stop-drain-test");
  ASSERT_NE(stream_input, nullptr);

  auto in_flight_started = std::make_shared<std::promise<void>>();
  auto release_in_flight = std::make_shared<std::promise<void>>();
  auto in_flight_finished = std::make_shared<std::atomic<bool>>(false);
  auto started = in_flight_started->get_future();
  auto release = release_in_flight->get_future().share();

  task_pool.push([in_flight_started, release, in_flight_finished]() {
    in_flight_started->set_value();
    release.wait();
    in_flight_finished->store(true);
  });

  auto release_on_exit = util::fail_guard([release_in_flight]() {
    try {
      release_in_flight->set_value();
    } catch (const std::future_error &) {
    }
  });
  ASSERT_EQ(started.wait_for(2s), std::future_status::ready);

  task_pool.stop();
  EXPECT_FALSE(task_pool.running());
  EXPECT_GT(task_pool.active_workers(), 0u);

  auto reset_future = std::async(std::launch::async, [stream_input]() mutable {
    input::reset(stream_input);
  });

  EXPECT_EQ(reset_future.wait_for(50ms), std::future_status::timeout);
  EXPECT_FALSE(in_flight_finished->load());

  release_in_flight->set_value();

  if (reset_future.wait_for(2s) != std::future_status::ready) {
    std::_Exit(1);
  }
  reset_future.get();
  EXPECT_TRUE(in_flight_finished->load());
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));

  task_pool.join();
  task_pool.start(1);
}

/**
 * @brief Verify that held keys are released and cancelled repeat timers emit no subsequent events after reset.
 */
TEST_F(InputResetExecutorTest, ActiveRepeatCancelledAndNoEventsAfterReset) {
  config::input.key_repeat_delay = 50ms;
  config::input.key_repeat_period = 50ms;

  std::mutex sink_mutex;
  std::vector<input::testing::keyboard_event_t> recorded_events;
  input::testing::set_keyboard_sink([&sink_mutex, &recorded_events](const input::testing::keyboard_event_t &event) {
    std::lock_guard lock(sink_mutex);
    recorded_events.push_back(event);
  });

  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "repeat-cancelled-on-reset");
  ASSERT_NE(stream_input, nullptr);

  // Submit keyboard packet through worker pool to avoid concurrent modification of global key_press
  task_pool.push([stream_input]() mutable {
    input::testing::send_keyboard_packet(stream_input, 0x41, 0, 0, false);
  }).get();

  {
    std::lock_guard lock(sink_mutex);
    ASSERT_EQ(recorded_events.size(), 1u);
    EXPECT_FALSE(recorded_events[0].release);
    EXPECT_EQ(recorded_events[0].key_code, 0x41);
  }

  input::reset(stream_input);

  {
    std::lock_guard lock(sink_mutex);
    ASSERT_EQ(recorded_events.size(), 2u);
    EXPECT_TRUE(recorded_events[1].release);
    EXPECT_EQ(recorded_events[1].key_code, 0x41);
  }

  std::this_thread::sleep_for(120ms);

  {
    std::lock_guard lock(sink_mutex);
    EXPECT_EQ(recorded_events.size(), 2u);
  }
  EXPECT_EQ(input::testing::queued_input_packet_count(stream_input), 0u);
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));
}

/**
 * @brief Verify that resetting an input session multiple times is safe and idempotent.
 */
TEST_F(InputResetExecutorTest, RepeatedResetIsIdempotent) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "idempotent-reset");
  ASSERT_NE(stream_input, nullptr);

  std::mutex sink_mutex;
  std::vector<input::testing::keyboard_event_t> recorded_events;
  input::testing::set_keyboard_sink([&sink_mutex, &recorded_events](const input::testing::keyboard_event_t &event) {
    std::lock_guard lock(sink_mutex);
    recorded_events.push_back(event);
  });

  task_pool.push([stream_input]() mutable {
    input::testing::send_keyboard_packet(stream_input, 0x42, 0, 0, false);
  }).get();

  {
    std::lock_guard lock(sink_mutex);
    ASSERT_EQ(recorded_events.size(), 1u);
  }

  input::reset(stream_input);

  {
    std::lock_guard lock(sink_mutex);
    EXPECT_EQ(recorded_events.size(), 2u);
    EXPECT_TRUE(recorded_events[1].release);
  }

  input::reset(stream_input);
  {
    std::lock_guard lock(sink_mutex);
    EXPECT_EQ(recorded_events.size(), 2u);
  }

  input::reset(stream_input);
  {
    std::lock_guard lock(sink_mutex);
    EXPECT_EQ(recorded_events.size(), 2u);
  }
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));
}

/**
 * @brief Concurrent stopped-pool resets serialize host-global held-key cleanup.
 */
TEST_F(InputResetExecutorTest, ConcurrentStoppedPoolResetsSerializeHeldKeyCleanup) {
  auto first = input::alloc(std::make_shared<safe::mail_raw_t>(), "stopped-reset-first");
  auto second = input::alloc(std::make_shared<safe::mail_raw_t>(), "stopped-reset-second");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  task_pool.push([first]() mutable { input::testing::send_keyboard_packet(first, 0x57, 0, 0, false); }).get();
  task_pool.stop();
  task_pool.join();

  auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  auto releases = std::make_shared<std::atomic<int>>(0);
  input::testing::set_keyboard_sink([entered, released, releases](const input::testing::keyboard_event_t &event) {
    if (event.key_code == 0x57 && event.release && releases->fetch_add(1) == 0) {
      entered->set_value();
      if (released.wait_for(2s) != std::future_status::ready) {
        std::_Exit(1);
      }
    }
  });
  auto first_reset = std::async(std::launch::async, [first]() mutable { input::reset(first); });
  if (entered_future.wait_for(2s) != std::future_status::ready) {
    std::_Exit(1);
  }
  auto second_reset = std::async(std::launch::async, [second]() mutable { input::reset(second); });
  EXPECT_EQ(second_reset.wait_for(50ms), std::future_status::timeout);
  release->set_value();
  if (first_reset.wait_for(2s) != std::future_status::ready || second_reset.wait_for(2s) != std::future_status::ready) {
    std::_Exit(1);
  }
  first_reset.get();
  second_reset.get();
  EXPECT_EQ(releases->load(), 1);
  input::testing::set_keyboard_sink({});
}

/**
 * @brief Verify that delayed mouse button release callbacks are cancelled and do not re-arm after reset.
 */
TEST_F(InputResetExecutorTest, MouseLeftDelayCancelledOnReset) {
  auto stream_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "mouse-delay-cancelled");
  ASSERT_NE(stream_input, nullptr);
  ASSERT_NE(context_->mouse, nullptr);

  // 1. Send absolute mouse move packet to enable BUTTON_LEFT release delay
  NV_ABS_MOUSE_MOVE_PACKET abs_pkt {};
  abs_pkt.header.size = util::endian::big<std::uint32_t>(sizeof(abs_pkt) - sizeof(abs_pkt.header.size));
  abs_pkt.header.magic = util::endian::little(MOUSE_MOVE_ABS_MAGIC);
  abs_pkt.width = util::endian::big<std::uint16_t>(1920);
  abs_pkt.height = util::endian::big<std::uint16_t>(1080);
  abs_pkt.x = util::endian::big<std::uint16_t>(960);
  abs_pkt.y = util::endian::big<std::uint16_t>(540);

  std::vector<std::uint8_t> abs_bytes(sizeof(abs_pkt));
  std::memcpy(abs_bytes.data(), &abs_pkt, sizeof(abs_pkt));

  // 2. Send mouse button press packet (BUTTON_LEFT down)
  NV_MOUSE_BUTTON_PACKET down_pkt {};
  down_pkt.header.size = util::endian::big<std::uint32_t>(sizeof(down_pkt) - sizeof(down_pkt.header.size));
  down_pkt.header.magic = util::endian::little(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5);
  down_pkt.button = BUTTON_LEFT;

  std::vector<std::uint8_t> down_bytes(sizeof(down_pkt));
  std::memcpy(down_bytes.data(), &down_pkt, sizeof(down_pkt));

  // 3. Send mouse button release packet (BUTTON_LEFT up)
  NV_MOUSE_BUTTON_PACKET up_pkt {};
  up_pkt.header.size = util::endian::big<std::uint32_t>(sizeof(up_pkt) - sizeof(up_pkt.header.size));
  up_pkt.header.magic = util::endian::little(MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5);
  up_pkt.button = BUTTON_LEFT;

  std::vector<std::uint8_t> up_bytes(sizeof(up_pkt));
  std::memcpy(up_bytes.data(), &up_pkt, sizeof(up_pkt));

  // Process and reset on one worker task. The delayed release cannot run between
  // those operations, so this exercises an actually pending timer without a race.
  auto *mouse = context_->mouse.get();
  const auto reset_counts = task_pool.push([stream_input, mouse,
                                           abs_bytes = std::move(abs_bytes),
                                           down_bytes = std::move(down_bytes),
                                           up_bytes = std::move(up_bytes)]() mutable {
    input::passthrough(stream_input, std::move(abs_bytes));
    input::passthrough(stream_input, std::move(down_bytes));
    input::passthrough(stream_input, std::move(up_bytes));
    input::testing::process_queued_messages(stream_input);
    EXPECT_TRUE(mouse->last_submitted_event().pressed);
    const auto before = mouse->submit_count();
    input::reset(stream_input);
    return std::pair {before, mouse->submit_count()};
  }).get();
  const auto submit_count_before_reset = reset_counts.first;

  // Held button neutralized to released
  EXPECT_FALSE(context_->mouse->last_submitted_event().pressed);
  const auto submit_count_after_reset = reset_counts.second;
  EXPECT_GT(submit_count_after_reset, submit_count_before_reset);

  // Sleep longer than the 10ms delay to verify cancelled callback never executes
  std::this_thread::sleep_for(150ms);

  EXPECT_EQ(context_->mouse->submit_count(), submit_count_after_reset);
  EXPECT_FALSE(context_->mouse->last_submitted_event().pressed);
  EXPECT_TRUE(input::testing::is_input_stopped(stream_input));
}
