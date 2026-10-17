/**
 * @file tests/unit/platform/windows/test_virtual_display_geometry.cpp
 * @brief Unit tests for the host-side virtual display resolution policy.
 *
 * @details No driver is involved: the policy is a pure function, so these tests are valid on any
 * Windows host and describe the size handed off to the unmodified SudoVDA driver.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/virtual_display_geometry.h"

namespace {
  using platf::windows::resolve_virtual_display_geometry;
  using platf::windows::clamp_refresh_rate;
  using platf::windows::clamp_oversized_resolution;

  class VirtualDisplayGeometryTest : public ::testing::Test {};

  TEST_F(VirtualDisplayGeometryTest, UsesClientRequestWhenSane) {
    const auto g = resolve_virtual_display_geometry(2560, 1440, 120, 0);
    EXPECT_EQ(g.width, 2560u);
    EXPECT_EQ(g.height, 1440u);
    EXPECT_EQ(g.fps, 120u);
  }

  TEST_F(VirtualDisplayGeometryTest, DefaultsUnsetClientValues) {
    const auto g = resolve_virtual_display_geometry(0, 0, 0, 0);
    EXPECT_EQ(g.width, 1920u);
    EXPECT_EQ(g.height, 1080u);
    EXPECT_EQ(g.fps, 60u);
  }

  TEST_F(VirtualDisplayGeometryTest, NegativeValuesAreTreatedAsUnset) {
    const auto g = resolve_virtual_display_geometry(-1, -1, -1, -50);
    EXPECT_EQ(g.width, 1920u);
    EXPECT_EQ(g.height, 1080u);
    EXPECT_EQ(g.fps, 60u);
  }

  TEST_F(VirtualDisplayGeometryTest, ForcesEvenDimensions) {
    // Display modes require even dimensions; a 1-pixel round-off is harmless.
    const auto g = resolve_virtual_display_geometry(1921, 1081, 60, 0);
    EXPECT_EQ(g.width, 1920u);
    EXPECT_EQ(g.height, 1080u);
  }

  TEST_F(VirtualDisplayGeometryTest, SmallButUsableDimensionsSurvive) {
    const auto g = resolve_virtual_display_geometry(200, 100, 60, 0);
    EXPECT_EQ(g.width, 320u);
    EXPECT_EQ(g.height, 320u);
  }

  TEST_F(VirtualDisplayGeometryTest, AppliesClientScaleFactor) {
    // 1440p at 125% is 1800p; a 21:9 phone is used so the aspect is obvious.
    const auto g = resolve_virtual_display_geometry(2000, 1000, 60, 125);
    EXPECT_EQ(g.width, 2500u);
    EXPECT_EQ(g.height, 1250u);

    const auto shrunk = resolve_virtual_display_geometry(1600, 900, 60, 50);
    EXPECT_EQ(shrunk.width, 800u);
    EXPECT_EQ(shrunk.height, 450u);
  }

  TEST_F(VirtualDisplayGeometryTest, ClampsOversizedRequestsWhilePreservingAspect) {
    const auto g = resolve_virtual_display_geometry(12000, 5000, 60, 0);
    // 12000x5000 is 12:5. Fitting 7680 wide gives 7680x3200, which is under 4320 tall.
    EXPECT_EQ(g.width, 7680u);
    EXPECT_EQ(g.height, 3200u);
  }

  TEST_F(VirtualDisplayGeometryTest, ClampsOversizedTallRequests) {
    // A portrait request taller than the cap must clamp on height, not width.
    const auto g = resolve_virtual_display_geometry(2000, 9000, 60, 0);
    EXPECT_LE(g.width, 7680u);
    EXPECT_LE(g.height, 4320u);
    // Aspect ratio must survive: width/height should stay within a hair of 2/9.
    const double ratio = static_cast<double>(g.width) / static_cast<double>(g.height);
    EXPECT_NEAR(ratio, 2.0 / 9.0, 0.05);
  }

  TEST_F(VirtualDisplayGeometryTest, ReportsNothingToClampWhenWithinLimits) {
    EXPECT_FALSE(clamp_oversized_resolution(1920, 1080).has_value());
    EXPECT_FALSE(clamp_oversized_resolution(7680, 4320).has_value());
  }

  TEST_F(VirtualDisplayGeometryTest, ClampsRefreshRateToPracticalModeRange) {
    EXPECT_EQ(clamp_refresh_rate(0), 60u);
    EXPECT_EQ(clamp_refresh_rate(1), 30u);
    EXPECT_EQ(clamp_refresh_rate(30), 30u);
    EXPECT_EQ(clamp_refresh_rate(144), 144u);
    EXPECT_EQ(clamp_refresh_rate(240), 240u);
    EXPECT_EQ(clamp_refresh_rate(1000), 240u);
  }

  TEST_F(VirtualDisplayGeometryTest, ScaleFactorCannotShrinkBelowUsablePanel) {
    const auto g = resolve_virtual_display_geometry(800, 600, 60, 1);
    EXPECT_GE(g.width, 320u);
    EXPECT_GE(g.height, 320u);
  }

  TEST_F(VirtualDisplayGeometryTest, HighDpiClientStaysWithinNormalPanelBounds) {
    // The scenario this policy exists for: a high-DPI client requests a large display, and the
    // Windows touch keyboard must still dock predictably, so the panel stays normal-sized.
    const auto g = resolve_virtual_display_geometry(3840, 2160, 90, 150);
    EXPECT_LE(g.width, 7680u);
    EXPECT_LE(g.height, 4320u);
    EXPECT_EQ(g.width % 2u, 0u);
    EXPECT_EQ(g.height % 2u, 0u);
  }
}  // namespace
#endif  // _WIN32
