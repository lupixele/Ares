/**
 * @file tests/unit/platform/windows/test_virtual_display_target.cpp
 * @brief Unit tests for pointing a streaming session at its virtual display.
 *
 * @details Path selection is tested as a pure function. The lifecycle tests drive the real
 *          display manager against a recording boundary and a recording target, so they describe
 *          the activation and restore contract without touching the display layout of the host
 *          running them.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/session_driver_boundary.h"
#include "src/platform/windows/sudovda_driver.h"
#include "src/platform/windows/virtual_display_manager.h"
#include "src/platform/windows/virtual_display_target.h"

#include <atomic>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  using platf::windows::display_path_t;
  using platf::windows::display_target_result_e;
  using platf::windows::find_display_path;
  using platf::windows::session_display_spec_t;
  using platf::windows::session_driver_boundary_t;
  using platf::windows::virtual_display_geometry_t;
  using platf::windows::virtual_display_manager_t;
  using platf::windows::virtual_display_result_e;

  /**
   * @brief Path list with one attached physical display and one detached virtual display.
   * @return Paths in the order a database query tends to return them.
   */
  std::vector<display_path_t> realistic_paths() {
    return {
      {L"\\\\.\\DISPLAY1", true, true},
      {L"\\\\.\\DISPLAY1", false, true},
      {L"\\\\.\\DISPLAY2", false, true},
      {L"\\\\.\\DISPLAY2", false, false},
    };
  }

  /**
   * @brief Recorder for the activation seam.
   */
  class recording_target_t: public platf::windows::idisplay_target_t {
  public:
    std::vector<std::wstring> applied;  ///< Display names that were activated.
    std::vector<virtual_display_geometry_t> geometries;  ///< Geometry supplied per activation.
    std::vector<std::string> *events {nullptr};  ///< Shared event order, when a test cares about it.
    int restores {0};  ///< Number of restore attempts.
    std::atomic<display_target_result_e> result {display_target_result_e::requested};  ///< Activation outcome.
    std::atomic<bool> restore_result {true};  ///< Restore outcome.
    std::atomic<bool> throw_on_apply {false};  ///< Simulates a seam that throws anyway.

    display_target_result_e apply(const std::wstring &display_name, const virtual_display_geometry_t &geometry) override {
      if (throw_on_apply.load()) {
        throw std::runtime_error {"activation failed"};
      }
      applied.push_back(display_name);
      geometries.push_back(geometry);
      if (events) {
        events->push_back("apply");
      }
      return result.load();
    }

    bool restore() override {
      ++restores;
      if (events) {
        events->push_back("restore");
      }
      return restore_result.load();
    }
  };

  /**
   * @brief Recording backend that simulates driver outcomes without any real device.
   */
  class recording_boundary_backend_t: public platf::windows::idriver_backend_t {
  public:
    std::vector<std::string> *events {nullptr};  ///< Shared event order, when a test cares about it.
    std::atomic<bool> accept_allocations {true};  ///< Whether a create request succeeds.

    bool is_driver_ready() override {
      return true;
    }

    std::optional<std::wstring> allocate_display(const session_display_spec_t &) override {
      if (!accept_allocations.load()) {
        return std::nullopt;
      }
      return std::optional<std::wstring> {L"\\\\.\\DISPLAY2"};
    }

    bool release_display(const std::wstring &) override {
      if (events) {
        events->push_back("release");
      }
      return true;
    }

    bool ping_watchdog() override {
      return true;
    }

    void unresolved_cleanup(const std::wstring &) noexcept override {}
  };

  /**
   * @brief Authorizer that permits every session.
   */
  class permitting_authorizer_t: public platf::windows::isession_authorizer_t {
  public:
    bool authorize_session(const std::string &, const session_display_spec_t &) override {
      return true;
    }
  };

  /**
   * @brief Fixture wiring a manager to recorders.
   */
  class VirtualDisplayTargetTest: public ::testing::Test {
  protected:
    std::vector<std::string> events;
    std::shared_ptr<recording_boundary_backend_t> backend = std::make_shared<recording_boundary_backend_t>();
    std::shared_ptr<recording_target_t> target = std::make_shared<recording_target_t>();
    std::shared_ptr<session_driver_boundary_t> boundary;

    void SetUp() override {
      backend->events = &events;
      target->events = &events;
      boundary = std::make_shared<session_driver_boundary_t>(std::make_shared<permitting_authorizer_t>(), backend, 3);
    }

    /**
     * @brief Manager under test.
     * @return A manager using the recording boundary and recording target.
     */
    virtual_display_manager_t manager() {
      return virtual_display_manager_t {boundary, nullptr, nullptr, target};
    }
  };

  TEST_F(VirtualDisplayTargetTest, EmptyNameNeverSelectsADisplay) {
    EXPECT_EQ(find_display_path(realistic_paths(), L""), std::nullopt);
  }

  TEST_F(VirtualDisplayTargetTest, AttachedDisplayIsPreferredForTheSameName) {
    const auto chosen = find_display_path(realistic_paths(), L"\\\\.\\DISPLAY1");
    ASSERT_TRUE(chosen.has_value());
    // Index 0 is the attached path; index 1 has the same name but is not presenting.
    EXPECT_EQ(*chosen, 0u);
  }

  TEST_F(VirtualDisplayTargetTest, NewlyCreatedDisplayIsSelectedWhenItIsNotAttachedYet) {
    const auto chosen = find_display_path(realistic_paths(), L"\\\\.\\DISPLAY2");
    ASSERT_TRUE(chosen.has_value());
    // The first DISPLAY2 entry is available but not attached; the second reports no usable target.
    EXPECT_EQ(*chosen, 2u);
  }

  TEST_F(VirtualDisplayTargetTest, UnknownDisplayIsReportedAsMissing) {
    EXPECT_EQ(find_display_path(realistic_paths(), L"\\\\.\\DISPLAY9"), std::nullopt);
  }

  TEST_F(VirtualDisplayTargetTest, PathsWithoutAUsableTargetAreIgnored) {
    const std::vector<display_path_t> paths {{L"\\\\.\\DISPLAY3", true, false}};
    EXPECT_EQ(find_display_path(paths, L"\\\\.\\DISPLAY3"), std::nullopt);
  }

  TEST_F(VirtualDisplayTargetTest, AcquireActivatesTheAllocatedDisplayWithResolvedGeometry) {
    auto subject = manager();

    EXPECT_EQ(subject.acquire("session-a", 1280, 720, 60, 0), virtual_display_result_e::created);

    ASSERT_EQ(target->applied.size(), 1u);
    EXPECT_EQ(target->applied[0], L"\\\\.\\DISPLAY2");
    ASSERT_EQ(target->geometries.size(), 1u);
    EXPECT_EQ(target->geometries[0].width, 1280u);
    EXPECT_EQ(target->geometries[0].height, 720u);
    EXPECT_EQ(target->geometries[0].fps, 60u);
  }

  TEST_F(VirtualDisplayTargetTest, ActivationHappensBeforeTheSessionIsReportedReady) {
    auto subject = manager();

    EXPECT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);

    // Activation is recorded by the target, so its presence alone proves it ran during acquire.
    EXPECT_EQ(events, std::vector<std::string> {"apply"});
  }

  TEST_F(VirtualDisplayTargetTest, FailedAllocationDoesNotActivateAnything) {
    backend->accept_allocations = false;
    auto subject = manager();

    EXPECT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::unavailable);
    EXPECT_TRUE(target->applied.empty());
  }

  TEST_F(VirtualDisplayTargetTest, RefusedActivationStillLeavesAWorkingSession) {
    target->result = display_target_result_e::device_missing;
    auto subject = manager();

    // A display that cannot be switched to is a degraded session, never a failed one.
    EXPECT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);
    EXPECT_TRUE(subject.has_display("session-a"));
  }

  TEST_F(VirtualDisplayTargetTest, ThrowingActivationStillLeavesAWorkingSession) {
    target->throw_on_apply = true;
    auto subject = manager();

    EXPECT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);
    EXPECT_TRUE(subject.has_display("session-a"));
  }

  TEST_F(VirtualDisplayTargetTest, RestoreHappensBeforeTheDisplayIsRemoved) {
    auto subject = manager();
    ASSERT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);

    ASSERT_TRUE(subject.release("session-a").released);

    // The layout must be put back while the virtual display still exists, so the operating
    // system moves from one valid layout to another.
    EXPECT_EQ(events, (std::vector<std::string> {"apply", "restore", "release"}));
  }

  TEST_F(VirtualDisplayTargetTest, OnlyTheLastSessionRestoresTheLayout) {
    auto subject = manager();
    ASSERT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);
    ASSERT_EQ(subject.acquire("session-b", 1280, 720, 60, 0), virtual_display_result_e::created);

    ASSERT_TRUE(subject.release("session-a").released);
    EXPECT_EQ(target->restores, 0);

    ASSERT_TRUE(subject.release("session-b").released);
    EXPECT_EQ(target->restores, 1);
  }

  TEST_F(VirtualDisplayTargetTest, ReleasingAnUnknownSessionDoesNotRestore) {
    auto subject = manager();
    ASSERT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);

    EXPECT_FALSE(subject.release("session-other").released);
    EXPECT_EQ(target->restores, 0);
    EXPECT_TRUE(subject.has_display("session-a"));
  }

  TEST_F(VirtualDisplayTargetTest, FailedRestoreStillReleasesTheDisplay) {
    target->restore_result = false;
    auto subject = manager();
    ASSERT_EQ(subject.acquire("session-a", 1920, 1080, 60, 0), virtual_display_result_e::created);

    // A layout that cannot be put back is worth a warning, not a leaked display.
    EXPECT_TRUE(subject.release("session-a").released);
    EXPECT_FALSE(subject.has_display("session-a"));
  }
}  // namespace
#endif
