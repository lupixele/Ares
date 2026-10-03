/**
 * @file tests/unit/test_session_display_coordinator.cpp
 * @brief Unit tests for the experimental session display coordinator lifecycle.
 */
#include "tests/tests_common.h"
#include "src/session_display_coordinator.h"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

using namespace ares::session;
using namespace std::chrono_literals;

namespace {

  /**
   * @brief Mock implementation of coordinator dependencies for lifecycle verification.
   */
  struct mock_coordinator_deps_t : itrusted_registry_t, idisplay_provider_t, iapp_lifecycle_t, iclock_t {
    policy_decision_t policy{true, true, true, true};
    publication_e published{publication_e::ready};
    bool allocate{true};
    bool probe{true};
    bool start{true};
    bool stop{true};
    bool restore{true};
    bool release{true};
    std::string throws;
    std::vector<std::string> events;
    std::function<void(const std::string &)> callback;
    std::chrono::steady_clock::time_point time{};

    void event(const std::string &s) {
      events.push_back(s);
      if (callback) {
        callback(s);
      }
      if (throws == s) {
        throw std::runtime_error(s);
      }
    }

    policy_decision_t authorize(const paired_principal_t &, action_e) override {
      event("authorize");
      return policy;
    }

    std::optional<std::wstring> allocate_display(const display_mode_spec_t &) override {
      event("allocate");
      return allocate ? std::optional<std::wstring>(L"display") : std::nullopt;
    }

    publication_e publication(const std::wstring &) override {
      event("publication");
      return published;
    }

    bool probe_display(const std::wstring &) override {
      event("probe");
      return probe;
    }

    bool restore_display(const std::wstring &) override {
      event("restore");
      return restore;
    }

    bool release_display(const std::wstring &) override {
      event("release");
      return release;
    }

    void on_unresolved_resource(const std::wstring &) override {
      event("notify");
    }

    bool start_app(const std::string &, const std::wstring &) override {
      event("start");
      return start;
    }

    bool stop_and_join_app(const std::string &) override {
      event("stop");
      return stop;
    }

    std::chrono::steady_clock::time_point now() override {
      event("clock");
      return time;
    }

    [[nodiscard]] size_t count(const std::string &s) const {
      return std::count(events.begin(), events.end(), s);
    }
  };

  /**
   * @brief Synchronization rendezvous primitive for concurrency testing.
   */
  struct test_gate_t {
    std::promise<void> arrived;
    std::promise<void> proceed;
    std::shared_future<void> go{proceed.get_future().share()};

    void wait() {
      arrived.set_value();
      EXPECT_EQ(go.wait_for(3s), std::future_status::ready);
    }

    void entered() {
      EXPECT_EQ(arrived.get_future().wait_for(3s), std::future_status::ready);
    }
  };

  /**
   * @brief Test fixture for coordinator tests.
   */
  class SessionDisplayCoordinatorTest : public ::testing::Test {
  protected:
    std::shared_ptr<mock_coordinator_deps_t> f{std::make_shared<mock_coordinator_deps_t>()};
    std::shared_ptr<recovery_ledger_t> ledger{std::make_shared<recovery_ledger_t>()};
    std::unique_ptr<session_display_coordinator_t> c{std::make_unique<session_display_coordinator_t>(f, f, f, f, ledger, 100ms)};

    uint64_t prepare(action_e a = action_e::launch) {
      EXPECT_EQ(c->prepare_session("s", {"client", "", ""}, {1920, 1080, 60, false}, a), coordinator_status_e::ok);
      auto rec = c->get_session("s");
      EXPECT_TRUE(rec.has_value());
      return rec ? rec->generation : 0;
    }

    uint64_t run() {
      auto g = prepare();
      EXPECT_EQ(c->start_session("s", g), coordinator_status_e::ok);
      return g;
    }

    void state(session_display_state_e s) {
      auto rec = c->get_session("s");
      ASSERT_TRUE(rec.has_value());
      EXPECT_EQ(rec->state, s);
    }
  };

  TEST_F(SessionDisplayCoordinatorTest, Denial) {
    f->policy = {};
    EXPECT_EQ(c->prepare_session("s", {"client", "", ""}, {}), coordinator_status_e::unauthorized);
    EXPECT_EQ(f->count("allocate"), 0u);

    for (const auto &decision : {
           policy_decision_t{false, true, true, true},
           policy_decision_t{true, false, true, true},
           policy_decision_t{true, true, false, true}}) {
      f->policy = decision;
      EXPECT_EQ(c->prepare_session("s", {"client", "", ""}, {}), coordinator_status_e::unauthorized);
    }

    f->policy = {true, true, true, false};
    display_mode_spec_t m;
    m.hdr_capable = true;
    EXPECT_EQ(c->prepare_session("s", {"client", "", ""}, m), coordinator_status_e::unauthorized);
  }

  TEST_F(SessionDisplayCoordinatorTest, Bounds) {
    for (const auto &m : {
           display_mode_spec_t{-1, 1080, 60, false},
           display_mode_spec_t{1920, -1, 60, false},
           display_mode_spec_t{1920, 1080, -1, false},
           display_mode_spec_t{7681, 1080, 60, false},
           display_mode_spec_t{1920, 4321, 60, false},
           display_mode_spec_t{1920, 1080, 241, false}}) {
      EXPECT_EQ(c->prepare_session("s", {"client", "", ""}, m), coordinator_status_e::invalid_resolution);
    }
    EXPECT_EQ(f->count("authorize"), 0u);
  }

  TEST_F(SessionDisplayCoordinatorTest, AllocationFailure) {
    auto g = prepare();
    f->allocate = false;
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::allocation_failed);
    EXPECT_EQ(f->count("release"), 0u);
    state(session_display_state_e::released);
  }

  TEST_F(SessionDisplayCoordinatorTest, PublicationFailure) {
    auto g = prepare();
    f->published = publication_e::failed;
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::probe_failed);
    EXPECT_EQ(f->count("release"), 1u);
    EXPECT_EQ(f->count("probe"), 0u);
  }

  TEST_F(SessionDisplayCoordinatorTest, ProbeFailure) {
    auto g = prepare();
    f->probe = false;
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::probe_failed);
    EXPECT_EQ(f->count("release"), 1u);
    EXPECT_EQ(f->count("start"), 0u);
  }

  TEST_F(SessionDisplayCoordinatorTest, AppFailure) {
    auto g = prepare();
    f->start = false;
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::app_start_failed);
    EXPECT_EQ(f->count("stop"), 1u);
    EXPECT_EQ(f->count("release"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, ActiveStop) {
    auto g = run();
    f->events.clear();
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    const std::vector<std::string> expected{"stop", "restore", "release"};
    EXPECT_EQ(f->events, expected);
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("release"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, PendingTimeout) {
    f->published = publication_e::pending;
    auto g = run();
    state(session_display_state_e::pending);
    EXPECT_EQ(f->count("allocate"), 1u);
    EXPECT_EQ(f->count("start"), 0u);
    f->time += 101ms;
    EXPECT_EQ(c->poll_session("s", g), coordinator_status_e::timeout);
    EXPECT_EQ(f->count("restore"), 1u);
    EXPECT_EQ(f->count("release"), 1u);
    state(session_display_state_e::released);
  }

  TEST_F(SessionDisplayCoordinatorTest, PendingReady) {
    f->published = publication_e::pending;
    auto g = run();
    f->published = publication_e::ready;
    EXPECT_EQ(c->poll_session("s", g), coordinator_status_e::ok);
    state(session_display_state_e::streaming);
    EXPECT_EQ(f->count("start"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, Retained) {
    auto g = run();
    EXPECT_EQ(c->disconnect_session("s", g, true), coordinator_status_e::ok);
    state(session_display_state_e::app_retained);
    EXPECT_EQ(f->count("release"), 0u);
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::invalid_state);
  }

  TEST_F(SessionDisplayCoordinatorTest, StaleGeneration) {
    auto g = run();
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    auto fresh = prepare();
    EXPECT_GT(fresh, g);
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::invalid_state);
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::invalid_state);
    EXPECT_EQ(c->start_session("s", fresh), coordinator_status_e::ok);
    state(session_display_state_e::streaming);
  }

  TEST_F(SessionDisplayCoordinatorTest, RestoreRetry) {
    auto g = run();
    f->restore = false;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::unresolved_resource);
    EXPECT_EQ(f->count("release"), 0u);
    EXPECT_FALSE(c->get_session("s")->allocated_resource.empty());
    f->restore = true;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("stop"), 1u);
    EXPECT_EQ(f->count("release"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, ReleaseRetry) {
    auto g = run();
    f->release = false;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::unresolved_resource);
    f->release = true;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("restore"), 1u);
    EXPECT_EQ(f->count("release"), 2u);
  }

  TEST_F(SessionDisplayCoordinatorTest, StopFailure) {
    auto g = run();
    f->stop = false;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::unresolved_resource);
    EXPECT_EQ(f->count("restore"), 0u);
    EXPECT_EQ(f->count("release"), 0u);
    f->stop = true;
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
  }

  TEST_F(SessionDisplayCoordinatorTest, Queries) {
    f->callback = [&](const auto &) {
      (void)c->get_session("s");
    };
    auto g = run();
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    f->callback = {};
  }

  TEST_F(SessionDisplayCoordinatorTest, Exceptions) {
    for (const std::string point : {"authorize", "clock", "allocate", "publication", "probe", "start", "stop", "restore", "release", "notify"}) {
      auto xf = std::make_shared<mock_coordinator_deps_t>();
      auto xl = std::make_shared<recovery_ledger_t>();
      session_display_coordinator_t xc(xf, xf, xf, xf, xl, 100ms);
      EXPECT_EQ(xc.prepare_session("s", {"client", "", ""}, {}), coordinator_status_e::ok);
      auto g = xc.get_session("s")->generation;
      xf->throws = point;
      if (point == "authorize") {
        EXPECT_EQ(xc.prepare_session("other", {"c", "", ""}, {}), coordinator_status_e::unauthorized);
      } else if (point == "stop" || point == "restore" || point == "release" || point == "notify") {
        EXPECT_EQ(xc.start_session("s", g), coordinator_status_e::ok);
        if (point == "notify") {
          xf->restore = false;
        }
        EXPECT_EQ(xc.stop_session("s", g), coordinator_status_e::unresolved_resource);
      } else {
        EXPECT_NE(xc.start_session("s", g), coordinator_status_e::ok);
      }
      xf->throws.clear();
      xf->restore = true;
      EXPECT_EQ(xc.close(), coordinator_status_e::ok);
      EXPECT_LE(xf->count("release"), 2u);
    }
  }

  TEST_F(SessionDisplayCoordinatorTest, DestructorHandoff) {
    run();
    f->restore = false;
    f->throws = "notify";
    c.reset();
    EXPECT_EQ(f->count("stop"), 1u);
    EXPECT_EQ(f->count("release"), 0u);
    EXPECT_EQ(ledger->pending(), 1u);
    f->throws.clear();
    f->restore = true;
    EXPECT_EQ(ledger->retry(), 0u);
    EXPECT_EQ(f->count("release"), 1u);
    EXPECT_EQ(ledger->retry(), 0u);
    EXPECT_EQ(f->count("release"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, ConcurrentRetry) {
    run();
    f->restore = false;
    c.reset();
    f->restore = true;
    test_gate_t gate;
    f->callback = [&](const auto &s) {
      if (s == "restore") {
        gate.wait();
      }
    };
    auto a = std::async(std::launch::async, [&] { return ledger->retry(); });
    gate.entered();
    // entered.set_value() ensures second thread launched before proceed is signaled; not a proof of OS-level lock suspension.
    std::promise<void> entered;
    auto b = std::async(std::launch::async, [&] {
      entered.set_value();
      return ledger->retry();
    });
    EXPECT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
    gate.proceed.set_value();
    EXPECT_EQ(a.get(), 0u);
    EXPECT_EQ(b.get(), 0u);
    EXPECT_EQ(f->count("release"), 1u);
    f->callback = {};
  }

  TEST_F(SessionDisplayCoordinatorTest, StopDuringStart) {
    auto g = prepare();
    test_gate_t gate;
    f->callback = [&](const auto &s) {
      if (s == "allocate") {
        gate.wait();
      }
    };
    auto a = std::async(std::launch::async, [&] { return c->start_session("s", g); });
    gate.entered();
    EXPECT_EQ(c->get_session("s")->generation, g);
    // entered.set_value() ensures caller thread started before gate release; not proving OS lock contention.
    std::promise<void> entered;
    auto b = std::async(std::launch::async, [&] {
      entered.set_value();
      return c->stop_session("s", g);
    });
    EXPECT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
    gate.proceed.set_value();
    EXPECT_EQ(a.get(), coordinator_status_e::ok);
    EXPECT_EQ(b.get(), coordinator_status_e::ok);
    state(session_display_state_e::released);
    EXPECT_EQ(f->count("release"), 1u);
    f->callback = {};
    auto fresh = prepare();
    EXPECT_GT(fresh, g);
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::invalid_state);
  }

  TEST_F(SessionDisplayCoordinatorTest, ViewPolicy) {
    auto g = prepare(action_e::view);
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("start"), 0u);
    EXPECT_EQ(c->stop_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("stop"), 0u);
  }

  TEST_F(SessionDisplayCoordinatorTest, ViewDisconnectRetain) {
    auto g = prepare(action_e::view);
    EXPECT_EQ(c->start_session("s", g), coordinator_status_e::ok);
    EXPECT_EQ(f->count("start"), 0u);
    EXPECT_EQ(c->disconnect_session("s", g, true), coordinator_status_e::ok);
    state(session_display_state_e::released);
    EXPECT_EQ(f->count("start"), 0u);
    EXPECT_EQ(f->count("stop"), 0u);
    EXPECT_EQ(f->count("restore"), 1u);
    EXPECT_EQ(f->count("release"), 1u);
  }

  TEST_F(SessionDisplayCoordinatorTest, DestructorAttemptAll) {
    run();
    EXPECT_EQ(c->prepare_session("second", {"client", "", ""}, {}), coordinator_status_e::ok);
    EXPECT_EQ(c->start_session("second", c->get_session("second")->generation), coordinator_status_e::ok);
    f->restore = false;
    f->throws = "notify";
    c.reset();
    EXPECT_EQ(f->count("stop"), 2u);
    EXPECT_EQ(f->count("restore"), 2u);
    EXPECT_EQ(f->count("release"), 0u);
    EXPECT_EQ(ledger->pending(), 2u);
    f->throws.clear();
    f->restore = true;
    EXPECT_EQ(ledger->retry(), 0u);
    EXPECT_EQ(f->count("release"), 2u);
  }

}  // namespace
