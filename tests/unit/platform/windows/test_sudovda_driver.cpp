/**
 * @file tests/unit/platform/windows/test_sudovda_driver.cpp
 * @brief Unit tests for the SudoVDA virtual display driver client.
 *
 * @details These tests exercise the driver-independent contract only: protocol
 * compatibility decisions, rejections of unusable geometry, and the retention
 * rules that keep a failed release owned. They intentionally never issue an
 * IOCTL, so they are valid on hosts where no virtual display driver is installed.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/sudovda_driver.h"

#include <string>
#include <vector>

namespace {
  class SudovdaDriverTest : public ::testing::Test {
  protected:
    platf::windows::sudovda_driver_t driver;
  };

  TEST_F(SudovdaDriverTest, ReportsDriverAbsentWithoutThrowing) {
    // The driver may legitimately be absent on this host. The contract requires a
    // plain false, not an exception and not an abort.
    EXPECT_NO_THROW(driver.close());
    EXPECT_FALSE(driver.handle_open());

    if (driver.is_driver_ready()) {
      GTEST_SKIP() << "SudoVDA driver is installed; absence path not exercised";
    }

    platf::windows::session_display_spec_t spec;
    spec.client_uid = "client-uuid-001";
    spec.client_name = "Moonlight iOS";
    spec.width = 1920;
    spec.height = 1080;
    spec.fps = 60;

    EXPECT_FALSE(driver.ping_watchdog());
    EXPECT_FALSE(driver.release_display(L"\\\\.\\DISPLAY9"));
    EXPECT_TRUE(driver.retained_monitors().empty());
  }

  TEST_F(SudovdaDriverTest, RejectsUnusableDisplayGeometry) {
    // Zero and out-of-range geometry must be refused before any driver request.
    platf::windows::session_display_spec_t zero;
    zero.client_uid = "client-uuid-002";
    zero.width = 1920;
    zero.height = 0;
    zero.fps = 60;
    EXPECT_FALSE(driver.allocate_display(zero).has_value());

    platf::windows::session_display_spec_t zero_fps;
    zero_fps.client_uid = "client-uuid-003";
    zero_fps.width = 1920;
    zero_fps.height = 1080;
    zero_fps.fps = 0;
    EXPECT_FALSE(driver.allocate_display(zero_fps).has_value());

    platf::windows::session_display_spec_t too_wide;
    too_wide.client_uid = "client-uuid-004";
    too_wide.width = 10000;
    too_wide.height = 1080;
    too_wide.fps = 60;
    EXPECT_FALSE(driver.allocate_display(too_wide).has_value());

    platf::windows::session_display_spec_t too_tall;
    too_tall.client_uid = "client-uuid-005";
    too_tall.width = 1920;
    too_tall.height = 10000;
    too_tall.fps = 60;
    EXPECT_FALSE(driver.allocate_display(too_tall).has_value());

    platf::windows::session_display_spec_t too_fast;
    too_fast.client_uid = "client-uuid-006";
    too_fast.width = 1920;
    too_fast.height = 1080;
    too_fast.fps = 5000;
    EXPECT_FALSE(driver.allocate_display(too_fast).has_value());
  }

  TEST_F(SudovdaDriverTest, RetainsFailedReleasesForLaterRetry) {
    // A release the driver refuses must stay owned rather than being forgotten.
    const std::wstring name = L"\\\\.\\DISPLAY4";
    driver.unresolved_cleanup(name);

    const auto retained = driver.retained_monitors();
    ASSERT_EQ(retained.size(), 1u);
    EXPECT_EQ(retained[0], name);

    // Re-recording the same name must not duplicate the obligation.
    driver.unresolved_cleanup(name);
    EXPECT_EQ(driver.retained_monitors().size(), 1u);
  }

  TEST_F(SudovdaDriverTest, DistinctMonitorsRetainSeparateObligations) {
    driver.unresolved_cleanup(L"\\\\.\\DISPLAY5");
    driver.unresolved_cleanup(L"\\\\.\\DISPLAY6");

    EXPECT_EQ(driver.retained_monitors().size(), 2u);
  }

  TEST_F(SudovdaDriverTest, ProtocolCompatibilityFollowsMajorMinorRules) {
    SUDOVDA::SUVDA_PROTOCAL_VERSION client {0, 2, 1, true};

    SUDOVDA::SUVDA_PROTOCAL_VERSION same {0, 2, 1, true};
    EXPECT_TRUE(platf::windows::sudovda_protocol_compatible(client, same));

    // A newer driver may still implement the current IOCTL set.
    SUDOVDA::SUVDA_PROTOCAL_VERSION newer_minor {0, 3, 0, false};
    EXPECT_TRUE(platf::windows::sudovda_protocol_compatible(client, newer_minor));

    // An older driver may not implement IOCTLs this client sends.
    SUDOVDA::SUVDA_PROTOCAL_VERSION older_minor {0, 1, 9, true};
    EXPECT_FALSE(platf::windows::sudovda_protocol_compatible(client, older_minor));

    // Any major change is always incompatible in both directions.
    SUDOVDA::SUVDA_PROTOCAL_VERSION newer_major {1, 2, 1, false};
    EXPECT_FALSE(platf::windows::sudovda_protocol_compatible(client, newer_major));

    SUDOVDA::SUVDA_PROTOCAL_VERSION older_major {0, 0, 1, true};
    EXPECT_FALSE(platf::windows::sudovda_protocol_compatible(client, older_major));
  }

  TEST_F(SudovdaDriverTest, ReportsUnknownMonitorClientName) {
    // Names that were never allocated must not resolve to a stored identity.
    EXPECT_TRUE(driver.monitor_client_name(L"\\\\.\\DISPLAY7").empty());
  }

  TEST_F(SudovdaDriverTest, RepeatedCloseIsSafeAndIdempotent) {
    driver.close();
    EXPECT_NO_THROW(driver.close());
    EXPECT_FALSE(driver.handle_open());
  }
}  // namespace
#endif  // _WIN32
