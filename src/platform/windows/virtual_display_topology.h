/**
 * @file src/platform/windows/virtual_display_topology.h
 * @brief The display-configuration data the capture-targeting seam reads and builds.
 *
 * @details These helpers are deliberately separate from the seam: they are the parts whose
 * behaviour can be reasoned about without rearranging the desktop of the machine running the
 * tests, so they are unit tested directly. The seam in `virtual_display_target.h` owns the
 * ordering, the rollback and the logging; this file owns the transformations.
 *
 * The operating system's own vocabulary is used rather than a parallel model, because the only
 * question that matters about a helper here is whether `SetDisplayConfig` accepts its output.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// platform includes
#include <Windows.h>

// local includes
#include "src/platform/windows/virtual_display_geometry.h"
#include "src/platform/windows/virtual_display_target.h"

namespace platf::windows::detail {
  /**
   * @brief Path array as reported by the display configuration database.
   */
  using path_list_t = std::vector<DISPLAYCONFIG_PATH_INFO>;

  /**
   * @brief Mode array that path mode indexes refer to.
   */
  using mode_list_t = std::vector<DISPLAYCONFIG_MODE_INFO>;

  /**
   * @brief Paths together with the modes their indexes refer to.
   */
  struct display_topology_t {
    path_list_t paths;  ///< Paths of the layout.
    mode_list_t modes;  ///< Modes those paths refer to.
  };

  /**
   * @brief Render a display configuration error code for logging.
   *
   * @param code Code returned by a display configuration call.
   * @return Human readable description.
   */
  [[nodiscard]] std::string describe_error(LONG code);

  /**
   * @brief Read the display configuration database.
   *
   * @param only_active True to read only the layout currently driving the desktop.
   * @param paths Receives the path array; replaced.
   * @param modes Receives the mode array the path indexes refer to; replaced.
   * @return True when the database was read completely.
   *
   * @note Queried with `QDC_VIRTUAL_MODE_AWARE`, because the split source and target mode indexes
   *       are only reliable in that mode, and every `SetDisplayConfig` call made by this feature
   *       states the matching `SDC_VIRTUAL_MODE_AWARE` flag.
   */
  [[nodiscard]] bool query_topology(bool only_active, path_list_t &paths, mode_list_t &modes);

  /**
   * @brief Read the desktop name of the source that a path drives.
   *
   * @param path Path to inspect.
   * @return The name, for example `L"\\\\.\\DISPLAY2"`, or nullopt when it cannot be read.
   */
  [[nodiscard]] std::optional<std::wstring> path_source_name(const DISPLAYCONFIG_PATH_INFO &path);

  /**
   * @brief A display's identity, which survives a change to the layout.
   *
   * @details The adapter and target identifier identify a display in the display configuration
   *          database. They stay the same when paths are reordered or when the source is given a
   *          different desktop name, which is exactly what happens when a layout is applied.
   *
   *          This matters because Windows can accept a topology that names one display and then
   *          activate a different, near-identical one. Comparing desktop names cannot detect that;
   *          comparing identity can.
   */
  struct display_identity_t {
    LUID adapter;      ///< Adapter that owns the display.
    UINT32 target_id;  ///< Target identifier on that adapter.

    /**
     * @brief Compare two identities.
     *
     * @param other Identity to compare with.
     * @return True when both refer to the same display.
     */
    [[nodiscard]] bool operator==(const display_identity_t &other) const {
      return adapter.HighPart == other.adapter.HighPart &&
             adapter.LowPart == other.adapter.LowPart &&
             target_id == other.target_id;
    }
  };

  /**
   * @brief Read a path's identity.
   *
   * @param path Path to inspect.
   * @return The display the path addresses.
   */
  [[nodiscard]] display_identity_t identity_of(const DISPLAYCONFIG_PATH_INFO &path);

  /**
   * @brief Find the path that addresses a display.
   *
   * @param paths Paths from the display configuration database.
   * @param identity Identity to look for.
   * @return Index into @p paths, or nullopt when the display is not among them.
   */
  [[nodiscard]] std::optional<std::size_t> find_path_by_identity(const path_list_t &paths,
                                                                 const display_identity_t &identity);

  /**
   * @brief Check that a display is active and no other display is.
   *
   * @param paths Paths from the display configuration database; may include inactive paths.
   * @param identity Display that should be the only one in use.
   * @return True when the display is the sole active path.
   *
   * @note Used to verify a switch instead of trusting the status code, because Windows is known to
   *       report success while activating a different display that has a near-identical
   *       description.
   */
  [[nodiscard]] bool is_sole_active_path(const path_list_t &paths, const display_identity_t &identity);

  /**
   * @brief Project paths onto the small view used to choose a display.
   *
   * @param paths Paths from the display configuration database.
   * @return One entry per path, in the same order, so a selected index stays valid.
   */
  [[nodiscard]] std::vector<display_path_t> project_paths(const path_list_t &paths);

  /**
   * @brief Strip the mode references from a path while marking it as part of the layout.
   *
   * @param path Path from the display configuration database.
   * @return The same path with every mode index invalidated and the active flag set.
   *
   * @note This is the shape Windows expects when it is asked to build a layout: the caller states
   *       which displays belong to it, not which mode each one should carry.
   */
  [[nodiscard]] DISPLAYCONFIG_PATH_INFO path_without_modes(const DISPLAYCONFIG_PATH_INFO &path);

  /**
   * @brief Build the single path that proposes a display as the whole layout.
   *
   * @param path Path of the display to activate.
   * @return The path from @ref path_without_modes, in clone group zero.
   *
   * @note The clone group is stated rather than inherited because a display that has never been
   *       attached reports no group, and a group identifier is part of a valid proposal.
   */
  [[nodiscard]] DISPLAYCONFIG_PATH_INFO proposed_topology_path(const DISPLAYCONFIG_PATH_INFO &path);

  /**
   * @brief State the geometry a session wants on a display that is already active.
   *
   * @param path Active path whose target mode should be chosen by the operating system.
   * @param source_mode Source mode of @p path, from the same query.
   * @param geometry Geometry to present.
   * @return True when the display did not already have the requested mode.
   *
   * @note The target and desktop indexes are invalidated so Windows selects a target mode that
   *       matches the new source resolution instead of keeping a stale one. The refresh rate is
   *       stated on the path, which is where Windows reads it from.
   */
  [[nodiscard]] bool apply_geometry(DISPLAYCONFIG_PATH_INFO &path,
                                    DISPLAYCONFIG_MODE_INFO &source_mode,
                                    const virtual_display_geometry_t &geometry);

  /**
   * @brief Copy paths while folding their modes into a compact array.
   *
   * @param chosen Paths to keep.
   * @param all_modes Mode array the kept paths refer to.
   * @return The kept paths with mode indexes rewritten for the compact array.
   *
   * @note The display configuration API expects every mode index to address the supplied array,
   *       so dropping unused modes without rewriting the indexes would select the wrong mode.
   */
  [[nodiscard]] display_topology_t compact_topology(path_list_t chosen, const mode_list_t &all_modes);

  /**
   * @brief Collect the paths that are currently driving the desktop, with their modes.
   *
   * @param paths Every path from the display configuration database.
   * @param modes Mode array that @p paths refers to.
   * @return The active layout with a compact mode array, possibly empty.
   */
  [[nodiscard]] display_topology_t active_topology(const path_list_t &paths, const mode_list_t &modes);
}  // namespace platf::windows::detail
