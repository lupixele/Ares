/**
 * @file src/platform/windows/virtual_display_target.cpp
 * @brief Implementation of the virtual display capture-targeting seam.
 */

// located in src/platform/windows

// standard includes
#include <cstdint>
#include <optional>
#include <string>

// local includes required before platform includes
#include "src/platform/windows/utf_utils.h"

// platform includes
// prevent clang format from "optimizing" the header include order
// clang-format off
#include <Windows.h>
// clang-format on

// local includes
#include "src/logging.h"
#include "src/platform/windows/virtual_display_target.h"
#include "src/platform/windows/virtual_display_topology.h"

namespace platf::windows {
  using namespace std::literals;

  namespace {
    using detail::display_topology_t;

    /**
     * @brief Hand a layout to the operating system.
     *
     * @param topology Paths and modes to submit; taken by value so the array pointers are writable,
     *                 which is the only shape the Win32 declaration accepts.
     * @param flags Flags that describe what the arrays mean.
     * @return The raw Win32 status, so the caller can decide whether to retry.
     *
     * @note `std::vector::data()` may return a null pointer for an empty vector, so empty arrays
     *       are passed as an explicit `nullptr`.
     */
    LONG submit_layout(display_topology_t topology, UINT32 flags) {
      return SetDisplayConfig(static_cast<UINT32>(topology.paths.size()),
                              topology.paths.empty() ? nullptr : topology.paths.data(),
                              static_cast<UINT32>(topology.modes.size()),
                              topology.modes.empty() ? nullptr : topology.modes.data(),
                              flags);
    }

    /**
     * @brief Ask Windows to make the proposed display the whole layout.
     *
     * @param topology A single path with no modes.
     * @return True when the layout is now active.
     *
     * @note Modes are deliberately absent: a display that has never been attached has no source
     *       or target mode to name, so Windows is asked to create the layout and assign them.
     *       The session geometry is stated in a second step, once the display is active.
     */
    bool propose_layout(const display_topology_t &topology) {
      auto status {submit_layout(topology, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE)};
      if (status == ERROR_GEN_FAILURE) {
        // Windows knows the path but will not take it as-is; asking it to build the layout is the
        // documented fallback.
        BOOST_LOG(warning) << detail::describe_error(status) << "; asking Windows to create the layout instead"sv;
        status = submit_layout(topology, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE);
      }

      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "The display layout could not be applied: "sv << detail::describe_error(status);
        return false;
      }
      return true;
    }

    /**
     * @brief State the modes of a layout that is already active.
     *
     * @param topology Paths and modes from one query, already adjusted.
     * @return True when Windows accepted them.
     *
     * @note The exact mode is requested first, because allowing Windows to adjust the request
     *       turns the requested resolution into a suggestion. Adjusting is only allowed after a
     *       rejection, since a working display at an adjusted size beats no display.
     */
    bool apply_layout_modes(display_topology_t topology) {
      auto status {submit_layout(topology, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE)};
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "Display mode was rejected as requested: "sv << detail::describe_error(status)
                         << "; retrying with adjustment allowed"sv;
        status = submit_layout(topology, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE);
      }

      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "The display mode could not be applied: "sv << detail::describe_error(status);
        return false;
      }
      return true;
    }

    /**
     * @brief Put a saved layout back.
     *
     * @param saved Paths and modes captured before the session changed anything.
     * @return True when the previous layout is in place.
     *
     * @note Four attempts, from most to least faithful: the exact layout, the same layout with
     *       stale modes corrected, its topology alone, and finally the same topology with Windows
     *       allowed to build it. Leaving the desktop isolated is worse than leaving it at a
     *       default resolution, so each failure earns a less precise attempt rather than giving up.
     */
    bool restore_layout(const display_topology_t &saved) {
      // The call the display-device library itself uses to put a saved configuration back: the
      // exact paths and modes, and persisted so the database agrees with what is on screen.
      const UINT32 exact_flags {SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE};
      auto status {submit_layout(saved, exact_flags)};
      if (status == ERROR_SUCCESS) {
        return true;
      }

      status = submit_layout(saved, exact_flags | SDC_ALLOW_CHANGES);
      if (status == ERROR_SUCCESS) {
        return true;
      }

      BOOST_LOG(warning) << "The previous display layout was rejected ("sv << detail::describe_error(status)
                         << "); restoring its topology without its modes"sv;

      display_topology_t topology_only;
      for (const auto &path : saved.paths) {
        topology_only.paths.push_back(detail::path_without_modes(path));
      }

      auto topology_status {submit_layout(topology_only, SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE)};
      if (topology_status == ERROR_SUCCESS) {
        return true;
      }

      if (topology_status == ERROR_GEN_FAILURE) {
        // Windows knows the topology but will not apply it from its own database; asking it to
        // create the layout is the fallback the display-device library uses for the same case.
        BOOST_LOG(warning) << detail::describe_error(topology_status) << "; asking Windows to create the layout instead"sv;
        topology_status = submit_layout(topology_only, SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE | SDC_VIRTUAL_MODE_AWARE);
        if (topology_status == ERROR_SUCCESS) {
          return true;
        }
      }

      BOOST_LOG(warning) << "The previous display topology could not be restored either: "sv
                         << detail::describe_error(topology_status);
      return false;
    }
  }  // namespace

  std::optional<std::size_t> find_display_path(const std::vector<display_path_t> &paths,
                                               std::wstring_view display_name) {
    if (display_name.empty()) {
      return std::nullopt;
    }

    std::optional<std::size_t> attached;
    std::optional<std::size_t> detached;
    for (std::size_t index = 0; index < paths.size(); ++index) {
      const auto &path = paths[index];
      if (path.gdi_name != display_name || !path.target_available) {
        continue;
      }

      if (path.active) {
        // A display that is already presenting is the one Windows is actually driving.
        if (!attached) {
          attached = index;
        }
      } else if (!detached) {
        detached = index;
      }
    }

    return attached ? attached : detached;
  }

  /**
   * @brief Layout that was active before this session changed it.
   */
  struct display_topology_target_t::saved_topology_t {
    detail::display_topology_t topology;  ///< Paths and modes to put back.
  };

  display_topology_target_t::display_topology_target_t() = default;
  display_topology_target_t::~display_topology_target_t() = default;

  display_target_result_e display_topology_target_t::apply(const std::wstring &display_name,
                                                           const virtual_display_geometry_t &geometry) {
    std::lock_guard lock(_mutex);

    detail::path_list_t paths;
    detail::mode_list_t modes;
    if (!detail::query_topology(false, paths, modes)) {
      return display_target_result_e::activation_failed;
    }

    const auto chosen {find_display_path(detail::project_paths(paths), display_name)};
    if (!chosen) {
      BOOST_LOG(warning) << "Virtual display ["sv << utf_utils::to_utf8(display_name)
                         << "] is not in the display configuration database yet"sv;
      return display_target_result_e::device_missing;
    }

    // Save the pre-session layout before changing anything: a failed switch must still leave a
    // restore record, otherwise a partially applied change would never be undone. The oldest
    // record wins, because that is the layout the operator actually started from.
    const bool captured_here {!_saved};
    if (captured_here) {
      auto saved {std::make_unique<saved_topology_t>()};
      saved->topology = detail::active_topology(paths, modes);
      if (saved->topology.paths.empty()) {
        BOOST_LOG(warning) << "No active display layout was found to return to; not switching displays"sv;
        return display_target_result_e::activation_failed;
      }
      _saved = std::move(saved);
    }

    // The display's identity is recorded now, while its path is known, because the desktop name
    // is not proof that the switch took: Windows can activate a different, near-identical display
    // and still report success.
    const auto identity {detail::identity_of(paths[*chosen])};

    display_topology_t proposal;
    proposal.paths.push_back(detail::proposed_topology_path(paths[*chosen]));
    if (!propose_layout(proposal)) {
      if (captured_here) {
        // Nothing was applied, so nothing is owed.
        _saved.reset();
      }
      return display_target_result_e::activation_failed;
    }

    // Put the saved layout back after a switch that cannot be proven correct. Keeping the
    // original record matters when the layout was captured by an earlier call: session end still
    // has something to restore.
    const auto abandon_switch {[&]() {
      if (_saved) {
        restore_layout(_saved->topology);
      }
      if (captured_here) {
        _saved.reset();
      }
    }};

    // The display is part of the desktop now, which is the only state in which it has a source
    // mode. Read the layout back and verify it before stating the geometry.
    detail::path_list_t active_paths;
    detail::mode_list_t active_modes;
    if (!detail::query_topology(true, active_paths, active_modes)) {
      BOOST_LOG(warning) << "The display layout was applied but could not be read back; restoring the previous layout"sv;
      abandon_switch();
      return display_target_result_e::activation_failed;
    }

    if (!detail::is_sole_active_path(active_paths, identity)) {
      BOOST_LOG(warning) << "Virtual display ["sv << utf_utils::to_utf8(display_name)
                         << "] was not activated as the only display; restoring the previous layout"sv;
      abandon_switch();
      return display_target_result_e::activation_failed;
    }

    bool geometry_stated {false};
    if (const auto active_chosen {detail::find_path_by_identity(active_paths, identity)}) {
      auto &path {active_paths[*active_chosen]};
      const auto source_index {path.sourceInfo.sourceModeInfoIdx};
      if (source_index == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID ||
          source_index >= active_modes.size() ||
          active_modes[source_index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        BOOST_LOG(warning) << "The session display has no source mode to resize"sv;
      } else if (detail::apply_geometry(path, active_modes[source_index], geometry)) {
        BOOST_LOG(debug) << "Stating "sv << geometry.width << 'x' << geometry.height << '@' << geometry.fps
                         << " for the session display"sv;
        geometry_stated = apply_layout_modes({std::move(active_paths), std::move(active_modes)});
      } else {
        // Windows chose exactly the requested mode when it built the layout, so there is nothing
        // to apply and no reason to disturb the display again.
        geometry_stated = true;
      }
    } else {
      // The sole-active check above already proved the display is present, so this cannot
      // normally happen.
      BOOST_LOG(warning) << "The session display could not be found after the switch"sv;
    }

    if (!geometry_stated) {
      // Keep the switch: the client is watching the right display, at a mode Windows chose. A
      // wrong size is a degraded session; losing the display would be a broken one.
      BOOST_LOG(warning) << "The session display could not be given "sv << geometry.width << 'x'
                         << geometry.height << '@' << geometry.fps
                         << "; the session continues at the mode Windows selected"sv;
    }

    _geometry = geometry;
    BOOST_LOG(info) << "Session display switched to virtual display ["sv << utf_utils::to_utf8(display_name)
                    << "] at "sv << geometry.width << 'x' << geometry.height << '@' << geometry.fps;
    return display_target_result_e::requested;
  }

  bool display_topology_target_t::restore() {
    std::lock_guard lock(_mutex);
    if (!_saved) {
      // Nothing was applied, or the layout was already restored.
      return true;
    }

    auto saved {std::move(_saved)};

    if (!restore_layout(saved->topology)) {
      BOOST_LOG(warning) << "The display layout from before the session could not be restored"sv;
      return false;
    }

    BOOST_LOG(info) << "Display layout restored after a virtual display session that requested "sv
                    << _geometry.width << 'x' << _geometry.height << '@' << _geometry.fps;
    return true;
  }
}  // namespace platf::windows
