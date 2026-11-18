/**
 * @file tests/unit/platform/windows/test_virtual_display_topology.cpp
 * @brief Unit tests for the display-configuration data the capture-targeting seam builds.
 *
 * @details Everything here is a transformation of data structures: no display configuration is
 *          read, written, or even reachable from these tests, so they cannot disturb the layout of
 *          the machine running them.
 */
#ifdef _WIN32
#include "tests/tests_common.h"
#include "src/platform/windows/virtual_display_topology.h"

#include <vector>

namespace {
  using platf::windows::detail::active_topology;
  using platf::windows::detail::apply_geometry;
  using platf::windows::detail::compact_topology;
  using platf::windows::detail::display_identity_t;
  using platf::windows::detail::display_topology_t;
  using platf::windows::detail::find_path_by_identity;
  using platf::windows::detail::is_sole_active_path;
  using platf::windows::detail::mode_list_t;
  using platf::windows::detail::path_list_t;
  using platf::windows::detail::path_without_modes;
  using platf::windows::detail::proposed_topology_path;
  using platf::windows::virtual_display_geometry_t;

  /**
   * @brief Build a path that references two modes.
   * @param active Whether the path is driving a display.
   * @param source_index Source mode index the path references.
   * @param target_index Target mode index the path references.
   * @return The path.
   */
  DISPLAYCONFIG_PATH_INFO make_path(bool active, UINT32 source_index, UINT32 target_index) {
    DISPLAYCONFIG_PATH_INFO path {};
    path.flags = active ? DISPLAYCONFIG_PATH_ACTIVE : 0;
    path.sourceInfo.id = 1;
    path.sourceInfo.cloneGroupId = 7;
    path.sourceInfo.sourceModeInfoIdx = source_index;
    path.targetInfo.adapterId.LowPart = 1000;
    path.targetInfo.id = 2;
    path.targetInfo.targetAvailable = TRUE;
    path.targetInfo.targetModeInfoIdx = target_index;
    path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    return path;
  }

  /**
   * @brief Give a path a distinct display identity.
   * @param path Path to modify.
   * @param adapter_low Low part of the adapter LUID.
   * @param target_id Target identifier on that adapter.
   */
  void set_identity(DISPLAYCONFIG_PATH_INFO &path, UINT32 adapter_low, UINT32 target_id) {
    path.targetInfo.adapterId.LowPart = adapter_low;
    path.targetInfo.id = target_id;
  }

  /**
   * @brief Build a source mode entry.
   * @param width Mode width in pixels.
   * @param height Mode height in pixels.
   * @param x Desktop origin of the mode.
   * @param y Desktop origin of the mode.
   * @return The mode entry.
   */
  DISPLAYCONFIG_MODE_INFO make_source_mode(UINT32 width, UINT32 height, LONG x, LONG y) {
    DISPLAYCONFIG_MODE_INFO mode {};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.sourceMode.width = width;
    mode.sourceMode.height = height;
    mode.sourceMode.position = {x, y};
    mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    return mode;
  }

  /**
   * @brief Build a target mode entry.
   * @param width Signal width in pixels.
   * @param height Signal height in pixels.
   * @return The mode entry.
   */
  DISPLAYCONFIG_MODE_INFO make_target_mode(UINT32 width, UINT32 height) {
    DISPLAYCONFIG_MODE_INFO mode {};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    mode.targetMode.targetVideoSignalInfo.activeSize = {width, height};
    return mode;
  }

  /**
   * @brief Build a desktop image mode entry.
   * @return The mode entry.
   *
   * @note The image itself is left zeroed: these tests only care that the entry exists and that
   *       the path index addresses it after compaction. The ucrt64 headers predate the
   *       `desktopImageInfo` union member, so its fields are not reachable through them.
   */
  DISPLAYCONFIG_MODE_INFO make_desktop_mode() {
    DISPLAYCONFIG_MODE_INFO mode {};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE;
    return mode;
  }

  /**
   * @brief Geometry used by most cases.
   * @return A 1920x1080 panel at 60 Hz.
   */
  virtual_display_geometry_t full_hd() {
    return {1920, 1080, 60};
  }
}  // namespace

TEST(VirtualDisplayTopologyTest, ProposalActivatesThePathAndClearsEveryModeReference) {
  const auto original {make_path(false, 4, 5)};

  const auto proposed {proposed_topology_path(original)};

  EXPECT_NE(proposed.flags & DISPLAYCONFIG_PATH_ACTIVE, 0u);
  EXPECT_EQ(proposed.sourceInfo.sourceModeInfoIdx, DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID);
  EXPECT_EQ(proposed.targetInfo.targetModeInfoIdx, DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID);
  EXPECT_EQ(proposed.targetInfo.desktopModeInfoIdx, DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID);
  EXPECT_EQ(proposed.sourceInfo.cloneGroupId, 0u);
}

TEST(VirtualDisplayTopologyTest, ProposalLeavesThePathItWasGivenAlone) {
  const auto original {make_path(false, 4, 5)};

  static_cast<void>(proposed_topology_path(original));

  EXPECT_EQ(original.flags, 0u);
  EXPECT_EQ(original.sourceInfo.sourceModeInfoIdx, 4u);
  EXPECT_EQ(original.targetInfo.targetModeInfoIdx, 5u);
}

TEST(VirtualDisplayTopologyTest, RestorablePathKeepsItsCloneGroup) {
  const auto original {make_path(true, 4, 5)};

  const auto stripped {path_without_modes(original)};

  EXPECT_EQ(stripped.sourceInfo.cloneGroupId, 7u);
  EXPECT_NE(stripped.flags & DISPLAYCONFIG_PATH_ACTIVE, 0u);
  EXPECT_EQ(stripped.sourceInfo.sourceModeInfoIdx, DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID);
  EXPECT_EQ(stripped.targetInfo.targetModeInfoIdx, DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID);
}

TEST(VirtualDisplayTopologyTest, GeometryIsStatedOnTheSourceModeAndTheTargetIsLeftToWindows) {
  const auto geometry {full_hd()};
  auto source_mode {make_source_mode(800, 600, 100, 200)};
  auto path {make_path(true, 0, 1)};

  const auto changed {apply_geometry(path, source_mode, geometry)};

  EXPECT_TRUE(changed);
  EXPECT_EQ(source_mode.sourceMode.width, 1920u);
  EXPECT_EQ(source_mode.sourceMode.height, 1080u);
  // The display is placed by Windows once it is the only one active; the mode keeps its origin.
  EXPECT_EQ(source_mode.sourceMode.position.x, 100);
  EXPECT_EQ(source_mode.sourceMode.position.y, 200);
  EXPECT_EQ(path.targetInfo.refreshRate.Numerator, 60u);
  EXPECT_EQ(path.targetInfo.refreshRate.Denominator, 1u);
  EXPECT_EQ(path.targetInfo.targetModeInfoIdx, DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID);
  EXPECT_EQ(path.targetInfo.desktopModeInfoIdx, DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID);
}

TEST(VirtualDisplayTopologyTest, MatchingGeometryIsReportedAsUnchanged) {
  const auto geometry {full_hd()};
  auto source_mode {make_source_mode(1920, 1080, 0, 0)};
  auto path {make_path(true, 0, 1)};
  path.targetInfo.refreshRate = {60, 1};

  EXPECT_FALSE(apply_geometry(path, source_mode, geometry));
}

TEST(VirtualDisplayTopologyTest, AnEquivalentRefreshRateIsNotTreatedAsADifference) {
  const auto geometry {full_hd()};
  auto source_mode {make_source_mode(1920, 1080, 0, 0)};
  auto path {make_path(true, 0, 1)};
  // Windows reports the same rate with whatever numerator and denominator the driver uses.
  path.targetInfo.refreshRate = {120, 2};

  EXPECT_FALSE(apply_geometry(path, source_mode, geometry));
}

TEST(VirtualDisplayTopologyTest, ARefreshRateMismatchIsReportedAsChanged) {
  const auto geometry {full_hd()};
  auto source_mode {make_source_mode(1920, 1080, 0, 0)};
  auto path {make_path(true, 0, 1)};
  path.targetInfo.refreshRate = {30, 1};

  EXPECT_TRUE(apply_geometry(path, source_mode, geometry));
  EXPECT_EQ(path.targetInfo.refreshRate.Numerator, 60u);
  EXPECT_EQ(path.targetInfo.refreshRate.Denominator, 1u);
}

TEST(VirtualDisplayTopologyTest, AnUnknownRefreshRateCountsAsAChange) {
  const auto geometry {full_hd()};
  auto source_mode {make_source_mode(1920, 1080, 0, 0)};
  auto path {make_path(true, 0, 1)};
  // A display that has never presented has no refresh rate, so there is nothing to compare.
  path.targetInfo.refreshRate = {0, 0};

  EXPECT_TRUE(apply_geometry(path, source_mode, geometry));
}

TEST(VirtualDisplayTopologyTest, CompactTopologyDropsUnusedModesAndRewritesEveryIndex) {
  const mode_list_t all_modes {
    make_target_mode(640, 480),
    make_source_mode(1920, 1080, 0, 0),
    make_target_mode(1920, 1080),
    make_source_mode(1280, 720, 1920, 0),
    make_target_mode(1280, 720),
  };

  const auto compacted {compact_topology({make_path(true, 1, 2), make_path(true, 3, 4)}, all_modes)};

  ASSERT_EQ(compacted.modes.size(), 4u);
  ASSERT_EQ(compacted.paths.size(), 2u);
  EXPECT_EQ(compacted.paths[0].sourceInfo.sourceModeInfoIdx, 0u);
  EXPECT_EQ(compacted.paths[0].targetInfo.targetModeInfoIdx, 1u);
  EXPECT_EQ(compacted.paths[1].sourceInfo.sourceModeInfoIdx, 2u);
  EXPECT_EQ(compacted.paths[1].targetInfo.targetModeInfoIdx, 3u);
  EXPECT_EQ(compacted.modes[0].sourceMode.width, 1920u);
  EXPECT_EQ(compacted.modes[1].infoType, DISPLAYCONFIG_MODE_INFO_TYPE_TARGET);
  EXPECT_EQ(compacted.modes[2].sourceMode.width, 1280u);
}

TEST(VirtualDisplayTopologyTest, CompactTopologySharesAModeThatTwoPathsReference) {
  const mode_list_t all_modes {
    make_source_mode(1920, 1080, 0, 0),
    make_target_mode(1920, 1080),
  };

  // Two paths driven by the same source, which is what a cloned display looks like.
  const auto compacted {compact_topology({make_path(true, 0, 1), make_path(true, 0, 1)}, all_modes)};

  ASSERT_EQ(compacted.modes.size(), 2u);
  EXPECT_EQ(compacted.paths[0].sourceInfo.sourceModeInfoIdx, 0u);
  EXPECT_EQ(compacted.paths[1].sourceInfo.sourceModeInfoIdx, 0u);
  EXPECT_EQ(compacted.paths[1].targetInfo.targetModeInfoIdx, 1u);
}

TEST(VirtualDisplayTopologyTest, CompactTopologyInvalidatesIndexesItCannotResolve) {
  const mode_list_t all_modes {make_source_mode(1920, 1080, 0, 0), make_target_mode(1920, 1080)};

  const auto compacted {compact_topology(
    {make_path(true, DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID, 99)},
    all_modes
  )};

  ASSERT_EQ(compacted.paths.size(), 1u);
  EXPECT_TRUE(compacted.modes.empty());
  EXPECT_EQ(compacted.paths[0].sourceInfo.sourceModeInfoIdx, DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID);
  EXPECT_EQ(compacted.paths[0].targetInfo.targetModeInfoIdx, DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID);
}

TEST(VirtualDisplayTopologyTest, ActiveTopologyKeepsOnlyTheDisplaysInUse) {
  const mode_list_t all_modes {
    make_source_mode(1920, 1080, 0, 0),
    make_target_mode(1920, 1080),
    make_source_mode(1280, 720, 0, 0),
    make_target_mode(1280, 720),
  };
  const path_list_t all_paths {
    make_path(true, 0, 1),
    make_path(false, 2, 3),
  };

  const auto active {active_topology(all_paths, all_modes)};

  ASSERT_EQ(active.paths.size(), 1u);
  ASSERT_EQ(active.modes.size(), 2u);
  EXPECT_EQ(active.paths[0].sourceInfo.sourceModeInfoIdx, 0u);
  EXPECT_EQ(active.paths[0].targetInfo.targetModeInfoIdx, 1u);
  EXPECT_EQ(active.modes[0].sourceMode.width, 1920u);
}

TEST(VirtualDisplayTopologyTest, CompactTopologyRewritesTheDesktopImageIndexToo) {
  const mode_list_t all_modes {
    make_desktop_mode(),
    make_source_mode(1920, 1080, 0, 0),
    make_target_mode(1920, 1080),
  };

  auto path {make_path(true, 1, 2)};
  path.targetInfo.desktopModeInfoIdx = 0;

  const auto compacted {compact_topology({path}, all_modes)};

  ASSERT_EQ(compacted.modes.size(), 3u);
  // Source and target take the first two slots, so the desktop image mode has to move to the last.
  EXPECT_EQ(compacted.paths[0].targetInfo.desktopModeInfoIdx, 2u);
  EXPECT_EQ(compacted.modes[2].infoType, DISPLAYCONFIG_MODE_INFO_TYPE_DESKTOP_IMAGE);
}

TEST(VirtualDisplayTopologyTest, IdentityFindsADisplayWhateverItsPositionInTheList) {
  path_list_t paths {make_path(true, 0, 1), make_path(true, 2, 3)};
  set_identity(paths[0], 1000, 2);
  set_identity(paths[1], 2000, 9);

  const auto found {find_path_by_identity(paths, display_identity_t {paths[1].targetInfo.adapterId, 9})};

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 1u);
}

TEST(VirtualDisplayTopologyTest, IdentityIsNotFoundWhenThatDisplayIsAbsent) {
  path_list_t paths {make_path(true, 0, 1)};
  set_identity(paths[0], 1000, 2);

  EXPECT_FALSE(find_path_by_identity(paths, display_identity_t {paths[0].targetInfo.adapterId, 77}).has_value());
}

TEST(VirtualDisplayTopologyTest, SameTargetIdOnAnotherAdapterIsADifferentDisplay) {
  path_list_t paths {make_path(true, 0, 1)};
  set_identity(paths[0], 1000, 2);

  // Low part alone is not an identity; the adapter has to match as a whole.
  EXPECT_FALSE(find_path_by_identity(paths, display_identity_t {{2, 0}, 2}).has_value());
}

TEST(VirtualDisplayTopologyTest, SoleActivePathAcceptsAnIsolatedDisplay) {
  path_list_t paths {make_path(true, 0, 1), make_path(false, 2, 3)};
  set_identity(paths[0], 1000, 2);
  set_identity(paths[1], 2000, 9);

  EXPECT_TRUE(is_sole_active_path(paths, display_identity_t {paths[0].targetInfo.adapterId, 2}));
}

TEST(VirtualDisplayTopologyTest, SoleActivePathRejectsALayoutThatKeepsAnotherDisplay) {
  path_list_t paths {make_path(true, 0, 1), make_path(true, 2, 3)};
  set_identity(paths[0], 1000, 2);
  set_identity(paths[1], 2000, 9);

  EXPECT_FALSE(is_sole_active_path(paths, display_identity_t {paths[0].targetInfo.adapterId, 2}));
}

TEST(VirtualDisplayTopologyTest, SoleActivePathRejectsADisplayThatIsNotActive) {
  path_list_t paths {make_path(false, 0, 1)};
  set_identity(paths[0], 1000, 2);

  EXPECT_FALSE(is_sole_active_path(paths, display_identity_t {paths[0].targetInfo.adapterId, 2}));
}

TEST(VirtualDisplayTopologyTest, AnEmptyLayoutProducesNoModes) {
  const auto compacted {compact_topology({}, {})};

  EXPECT_TRUE(compacted.paths.empty());
  EXPECT_TRUE(compacted.modes.empty());
}
#endif
