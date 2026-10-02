/**
 * @file tests/unit/platform/windows/test_session_driver_boundary.cpp
 * @brief Unit tests for the C++ session and virtual display driver boundary.
 */
#include "tests/tests_common.h"
#include "src/platform/windows/session_driver_boundary.h"

#include <vector>

namespace {

  class mock_driver_backend_t : public platf::windows::idriver_backend_t {
  public:
    bool driver_ready {true};
    bool ping_response {true};
    bool allocation_success {true};
    std::wstring allocated_name {L"\\\\.\\DISPLAY3"};
    std::vector<std::wstring> released_displays;
    int ping_count {0};

    bool is_driver_ready() override {
      return driver_ready;
    }

    std::optional<std::wstring> allocate_display(const platf::windows::session_display_spec_t &spec) override {
      if (!allocation_success) {
        return std::nullopt;
      }
      return allocated_name;
    }

    bool release_display(const std::wstring &display_name) override {
      released_displays.push_back(display_name);
      return true;
    }

    bool ping_watchdog() override {
      ping_count++;
      return ping_response;
    }
  };

  class mock_session_authorizer_t : public platf::windows::isession_authorizer_t {
  public:
    bool allow_all {true};
    std::vector<std::string> rejected_uids;

    bool authorize_session(const std::string &session_id, const platf::windows::session_display_spec_t &spec) override {
      if (!allow_all) {
        return false;
      }
      for (const auto &rejected : rejected_uids) {
        if (spec.client_uid == rejected) {
          return false;
        }
      }
      return true;
    }
  };

  class SessionDriverBoundaryTest : public ::testing::Test {
  protected:
    std::shared_ptr<mock_session_authorizer_t> authorizer = std::make_shared<mock_session_authorizer_t>();
    std::shared_ptr<mock_driver_backend_t> backend = std::make_shared<mock_driver_backend_t>();
  };

  TEST_F(SessionDriverBoundaryTest, SuccessfulAcquireAndReleaseLifecycle) {
    platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

    platf::windows::session_display_spec_t spec;
    spec.client_uid = "client-uuid-001";
    spec.client_name = "Moonlight iOS";
    spec.width = 2560;
    spec.height = 1440;
    spec.fps = 120;
    spec.isolated_display = true;

    auto [status, alloc] = boundary.acquire_session_display("session-123", spec);
    EXPECT_EQ(status, platf::windows::driver_boundary_status_e::ok);
    ASSERT_TRUE(alloc.has_value());
    EXPECT_EQ(alloc->session_id, "session-123");
    EXPECT_EQ(alloc->display_name, L"\\\\.\\DISPLAY3");
    EXPECT_EQ(boundary.get_active_session_count(), 1u);
    EXPECT_TRUE(boundary.has_session("session-123"));

    bool released = boundary.release_session_display("session-123");
    EXPECT_TRUE(released);
    EXPECT_EQ(boundary.get_active_session_count(), 0u);
    EXPECT_FALSE(boundary.has_session("session-123"));
    ASSERT_EQ(backend->released_displays.size(), 1u);
    EXPECT_EQ(backend->released_displays[0], L"\\\\.\\DISPLAY3");
  }

  TEST_F(SessionDriverBoundaryTest, RejectsUnauthorizedClient) {
    authorizer->allow_all = false;
    platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

    platf::windows::session_display_spec_t spec;
    spec.client_uid = "unauthorized-uid";
    spec.client_name = "Intruder";
    spec.width = 1920;
    spec.height = 1080;
    spec.fps = 60;

    auto [status, alloc] = boundary.acquire_session_display("session-bad", spec);
    EXPECT_EQ(status, platf::windows::driver_boundary_status_e::unauthorized);
    EXPECT_FALSE(alloc.has_value());
    EXPECT_EQ(boundary.get_active_session_count(), 0u);
    EXPECT_EQ(backend->released_displays.size(), 0u);
  }

  TEST_F(SessionDriverBoundaryTest, EnforcesValidResolutionDimensions) {
    platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

    platf::windows::session_display_spec_t zero_dim_spec;
    zero_dim_spec.client_uid = "client-uuid-002";
    zero_dim_spec.width = 0;
    zero_dim_spec.height = 1080;

    auto [status, alloc] = boundary.acquire_session_display("session-zero", zero_dim_spec);
    EXPECT_EQ(status, platf::windows::driver_boundary_status_e::invalid_dimensions);
    EXPECT_FALSE(alloc.has_value());
    EXPECT_EQ(boundary.get_active_session_count(), 0u);
  }

  TEST_F(SessionDriverBoundaryTest, RollbackAllocationOnDuplicateSessionAcquire) {
    platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

    platf::windows::session_display_spec_t spec1;
    spec1.client_uid = "client-uuid-003";
    spec1.width = 1920;
    spec1.height = 1080;
    spec1.fps = 60;

    auto [status1, alloc1] = boundary.acquire_session_display("session-dup", spec1);
    EXPECT_EQ(status1, platf::windows::driver_boundary_status_e::ok);
    EXPECT_EQ(boundary.get_active_session_count(), 1u);

    // Re-acquire with new parameters - prior allocation must be rolled back
    backend->allocated_name = L"\\\\.\\DISPLAY4";
    platf::windows::session_display_spec_t spec2 = spec1;
    spec2.width = 3840;
    spec2.height = 2160;

    auto [status2, alloc2] = boundary.acquire_session_display("session-dup", spec2);
    EXPECT_EQ(status2, platf::windows::driver_boundary_status_e::ok);
    EXPECT_EQ(boundary.get_active_session_count(), 1u);
    ASSERT_EQ(backend->released_displays.size(), 1u);
    EXPECT_EQ(backend->released_displays[0], L"\\\\.\\DISPLAY3");
    EXPECT_EQ(alloc2->display_name, L"\\\\.\\DISPLAY4");
  }

  TEST_F(SessionDriverBoundaryTest, DestructorPerformsCleanRollbackOfAllSessions) {
    backend->allocated_name = L"\\\\.\\DISPLAY10";
    {
      platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

      platf::windows::session_display_spec_t spec;
      spec.client_uid = "uid-1";
      spec.width = 1920;
      spec.height = 1080;
      boundary.acquire_session_display("sess-1", spec);

      backend->allocated_name = L"\\\\.\\DISPLAY11";
      spec.client_uid = "uid-2";
      boundary.acquire_session_display("sess-2", spec);
      EXPECT_EQ(boundary.get_active_session_count(), 2u);
    } // boundary goes out of scope and destroys

    EXPECT_EQ(backend->released_displays.size(), 2u);
  }

  TEST_F(SessionDriverBoundaryTest, ConsecutiveHeartbeatFailureAndResetGuarantees) {
    platf::windows::session_driver_boundary_t boundary(authorizer, backend, 3);

    // Initial state
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 0u);

    // 1st tick ok
    EXPECT_TRUE(boundary.tick_heartbeat());
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 0u);

    // Driver ping fails 2 consecutive times
    backend->ping_response = false;
    EXPECT_TRUE(boundary.tick_heartbeat());
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 1u);

    EXPECT_TRUE(boundary.tick_heartbeat());
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 2u);

    // Driver ping recovers -> consecutive counter MUST reset to 0 immediately
    backend->ping_response = true;
    EXPECT_TRUE(boundary.tick_heartbeat());
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 0u);

    // Ping fails up to threshold (3)
    backend->ping_response = false;
    EXPECT_TRUE(boundary.tick_heartbeat()); // 1
    EXPECT_TRUE(boundary.tick_heartbeat()); // 2
    EXPECT_TRUE(boundary.tick_heartbeat()); // 3 (still within max_fails threshold)
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 3u);

    // 4th consecutive failure exceeds threshold -> heartbeat unhealthy
    EXPECT_FALSE(boundary.tick_heartbeat()); // 4 > 3
    EXPECT_EQ(boundary.get_consecutive_heartbeat_failures(), 4u);
  }

}  // namespace
