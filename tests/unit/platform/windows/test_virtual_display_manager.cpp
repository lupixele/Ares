/**
 * @file tests/unit/platform/windows/test_virtual_display_manager.cpp
 * @brief Unit tests for the session-scoped virtual display manager.
 *
 * @details The manager is exercised against a recording boundary, so these tests describe the
 * lifecycle contract without allocating or destroying a real display.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/session_driver_boundary.h"
#include "src/platform/windows/sudovda_driver.h"
#include "src/platform/windows/virtual_display_manager.h"

#include <atomic>
#include <string>
#include <vector>

namespace {
  using platf::windows::driver_boundary_status_e;
  using platf::windows::session_display_spec_t;
  using platf::windows::session_display_allocation_t;
  using platf::windows::session_driver_boundary_t;
  using platf::windows::virtual_display_manager_t;
  using platf::windows::virtual_display_result_e;

  /**
   * @brief Recording authorizer that can be switched off.
   */
  class recording_authorizer_t : public platf::windows::isession_authorizer_t {
  public:
    std::atomic<bool> permit {true};
    std::vector<std::string> seen;

    bool authorize_session(const std::string &, const session_display_spec_t &) override {
      return permit.load();
    }
  };

  /**
   * @brief Recording backend that simulates driver outcomes without any real device.
   */
  class recording_backend_t : public platf::windows::idriver_backend_t {
  public:
    std::vector<session_display_spec_t> allocated;
    std::vector<std::wstring> released;
    std::vector<std::wstring> unresolved;
    std::atomic<bool> ready {true};
    std::atomic<bool> accept_allocations {true};

    bool is_driver_ready() override {
      return ready.load();
    }

    std::optional<std::wstring> allocate_display(const session_display_spec_t &spec) override {
      allocated.push_back(spec);
      if (!accept_allocations.load()) {
        return std::nullopt;
      }
      return std::optional<std::wstring> {L"\\\\.\\DISPLAY1"};
    }

    bool release_display(const std::wstring &display_name) override {
      released.push_back(display_name);
      return true;
    }

    bool ping_watchdog() override {
      return true;
    }

    void unresolved_cleanup(const std::wstring &display_name) noexcept override {
      unresolved.push_back(display_name);
    }
  };

  class VirtualDisplayManagerTest : public ::testing::Test {
  protected:
    std::shared_ptr<recording_backend_t> backend = std::make_shared<recording_backend_t>();
    std::shared_ptr<recording_authorizer_t> authorizer = std::make_shared<recording_authorizer_t>();
  };

  TEST_F(VirtualDisplayManagerTest, AllocatesOnAcquireAndPassesResolvedGeometry) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    const auto result = manager.acquire("session-a", 1280, 720, 60, 0);
    EXPECT_EQ(result, virtual_display_result_e::created);
    EXPECT_TRUE(manager.has_display("session-a"));

    ASSERT_EQ(backend->allocated.size(), 1u);
    EXPECT_EQ(backend->allocated[0].width, 1280u);
    EXPECT_EQ(backend->allocated[0].height, 720u);
    EXPECT_EQ(backend->allocated[0].fps, 60u);
    EXPECT_EQ(backend->allocated[0].client_uid, "session-a");

    const auto active = manager.active_sessions();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].display_name, L"\\\\.\\DISPLAY1");
  }

  TEST_F(VirtualDisplayManagerTest, ReacquireKeepsExistingDisplayInsteadOfLeakingIt) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);
    EXPECT_EQ(manager.acquire("session-a", 2560, 1440, 120, 0), virtual_display_result_e::already_present);

    // A second allocation would strand the first display on the desktop.
    ASSERT_EQ(backend->allocated.size(), 1u);
    EXPECT_EQ(backend->allocated[0].width, 1280u);
    EXPECT_EQ(backend->allocated[0].height, 720u);
  }

  TEST_F(VirtualDisplayManagerTest, ReleaseRemovesTheDisplayAndForgetsSession) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);
    const auto report = manager.release("session-a");

    EXPECT_TRUE(report.released);
    EXPECT_FALSE(report.retained.has_value());
    EXPECT_FALSE(manager.has_display("session-a"));
    ASSERT_EQ(backend->released.size(), 1u);
    EXPECT_EQ(backend->released[0], L"\\\\.\\DISPLAY1");
  }

  TEST_F(VirtualDisplayManagerTest, ReleaseForUnknownSessionIsQuietlySafe) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    // Sessions end without ever acquiring, so this must not be an error or a crash.
    const auto report = manager.release("never-started");
    EXPECT_FALSE(report.released);
    EXPECT_FALSE(report.retained.has_value());
    EXPECT_TRUE(backend->released.empty());
  }

  TEST_F(VirtualDisplayManagerTest, DoubleReleaseIsIdempotent) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);
    EXPECT_TRUE(manager.release("session-a").released);
    const auto second = manager.release("session-a");
    EXPECT_FALSE(second.released);
    EXPECT_EQ(backend->released.size(), 1u);
  }

  TEST_F(VirtualDisplayManagerTest, FailedCreateDoesNotBreakSessionAndReleasesNothing) {
    backend->accept_allocations.store(false);
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    const auto result = manager.acquire("session-a", 1280, 720, 60, 0);
    EXPECT_EQ(result, virtual_display_result_e::unavailable);
    EXPECT_FALSE(manager.has_display("session-a"));
    EXPECT_TRUE(backend->released.empty());

    // Ending such a session must still be safe.
    const auto report = manager.release("session-a");
    EXPECT_FALSE(report.released);
  }

  TEST_F(VirtualDisplayManagerTest, MissingDriverAlsoKeepsSessionAlive) {
    backend->ready.store(false);
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::unavailable);
    EXPECT_FALSE(manager.has_display("session-a"));
  }

  TEST_F(VirtualDisplayManagerTest, NoBoundaryConfigurationIsNotAFailure) {
    virtual_display_manager_t manager(nullptr);
    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::unavailable);
    EXPECT_FALSE(manager.has_display("session-a"));
  }

  TEST_F(VirtualDisplayManagerTest, UnauthoredSessionGetsNoDisplay) {
    // Policy says no, so the manager must not allocate. The underlying boundary reports the
    // refusal as "unavailable" rather than "rejected", because the policy gate sits above it.
    authorizer->permit.store(false);
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::unavailable);
    EXPECT_TRUE(backend->allocated.empty());
  }

  TEST_F(VirtualDisplayManagerTest, RefusingPolicyStopsAllocation) {
    class refusing_policy_t final : public virtual_display_manager_t::isession_policy_t {
    public:
      [[nodiscard]] bool allows_virtual_display() const override {
        return false;
      }
    };

    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary, std::make_shared<refusing_policy_t>());

    // When the manager's own policy refuses, nothing reaches the boundary at all.
    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::rejected);
    EXPECT_TRUE(backend->allocated.empty());
  }

  TEST_F(VirtualDisplayManagerTest, EmptySessionIdIsRefused) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("", 1280, 720, 60, 0), virtual_display_result_e::rejected);
    EXPECT_TRUE(backend->allocated.empty());
  }

  TEST_F(VirtualDisplayManagerTest, UnconfirmedReleaseIsRetainedNotForgotten) {
    // A boundary whose release cannot be confirmed must leave the obligation owned.
    class refusing_release_backend_t : public recording_backend_t {
    public:
      bool release_display(const std::wstring &) override {
        return false;
      }
    };

    auto refusing = std::make_shared<refusing_release_backend_t>();
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, refusing, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);

    const auto report = manager.release("session-a");
    EXPECT_FALSE(report.released);
    ASSERT_TRUE(report.retained.has_value());
    EXPECT_EQ(*report.retained, L"\\\\.\\DISPLAY1");

    // The obligation must survive: the session is still tracked, not silently dropped.
    EXPECT_TRUE(manager.has_display("session-a"));
    EXPECT_EQ(manager.active_sessions().size(), 1u);
  }

  TEST_F(VirtualDisplayManagerTest, ReleaseAllClearsEverySession) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);
    EXPECT_EQ(manager.acquire("session-b", 2560, 1440, 120, 0), virtual_display_result_e::created);
    EXPECT_EQ(manager.active_sessions().size(), 2u);

    auto reports = manager.release_all();
    EXPECT_EQ(reports.size(), 2u);
    for (const auto &report : reports) {
      EXPECT_TRUE(report.released);
    }
    EXPECT_TRUE(manager.active_sessions().empty());
  }

  TEST_F(VirtualDisplayManagerTest, SessionsAreIsolatedFromEachOther) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    EXPECT_EQ(manager.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);
    EXPECT_EQ(manager.acquire("session-b", 2560, 1440, 120, 0), virtual_display_result_e::created);

    EXPECT_EQ(backend->allocated.size(), 2u);
    EXPECT_EQ(manager.release("session-a").released, true);
    EXPECT_TRUE(manager.has_display("session-b"));
    EXPECT_FALSE(manager.has_display("session-a"));
    EXPECT_EQ(manager.active_sessions().size(), 1u);
  }

  TEST_F(VirtualDisplayManagerTest, GeometryPolicyAppliesToDriverRequest) {
    auto boundary = std::make_shared<session_driver_boundary_t>(authorizer, backend, 3);
    virtual_display_manager_t manager(boundary);

    // An oversized high-DPI client must still be handed a normal-sized panel.
    EXPECT_EQ(manager.acquire("session-a", 12000, 5000, 90, 150), virtual_display_result_e::created);

    ASSERT_EQ(backend->allocated.size(), 1u);
    EXPECT_LE(backend->allocated[0].width, 7680u);
    EXPECT_LE(backend->allocated[0].height, 4320u);
    EXPECT_EQ(backend->allocated[0].width % 2u, 0u);
    EXPECT_EQ(backend->allocated[0].height % 2u, 0u);
  }
}  // namespace
#endif  // _WIN32
