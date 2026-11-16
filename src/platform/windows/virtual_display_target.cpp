/**
 * @file src/platform/windows/virtual_display_target.cpp
 * @brief Implementation of the virtual display capture-targeting seam.
 */

// located in src/platform/windows

// standard includes
#include <cstdint>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

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

namespace platf::windows {
  using namespace std::literals;

  namespace {
    /**
     * @brief Path array as reported by the display configuration database.
     */
    using path_list_t = std::vector<DISPLAYCONFIG_PATH_INFO>;

    /**
     * @brief Mode array that path mode indexes refer to.
     */
    using mode_list_t = std::vector<DISPLAYCONFIG_MODE_INFO>;

    /**
     * @brief Paths together with the modes they refer to.
     */
    struct topology_t {
      path_list_t paths;  ///< Paths of the topology.
      mode_list_t modes;  ///< Modes those paths refer to.
    };

    /**
     * @brief Render a Win32 error code for logging.
     * @param code Code returned by a display configuration call.
     * @return Human readable description.
     */
    std::string error_text(LONG code) {
      return std::system_category().message(static_cast<int>(code));
    }

    /**
     * @brief Read every path and mode the display configuration database knows about.
     *
     * @param paths Receives the path array.
     * @param modes Receives the mode array that the path indexes refer to.
     * @return True when the database was read completely.
     *
     * @note Two queries are needed by design: the size query and the data query can disagree when
     *       a display is added or removed in between, which is exactly what happens during session
     *       start. The buffer is therefore re-queried once instead of trusting the first size.
     */
    bool query_all_paths(path_list_t &paths, mode_list_t &modes) {
      for (int attempt = 0; attempt < 2; ++attempt) {
        UINT32 path_count {0};
        UINT32 mode_count {0};
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) != ERROR_SUCCESS ||
            path_count == 0) {
          return false;
        }

        paths.resize(path_count);
        modes.resize(mode_count);
        const auto status {QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr)};
        if (status == ERROR_INSUFFICIENT_BUFFER) {
          continue;
        }
        if (status != ERROR_SUCCESS) {
          BOOST_LOG(warning) << "Could not read the display configuration: "sv << error_text(status);
          return false;
        }

        paths.resize(path_count);
        modes.resize(mode_count);
        return true;
      }

      BOOST_LOG(warning) << "Display configuration kept changing while it was being read"sv;
      return false;
    }

    /**
     * @brief Read the desktop name of the source that a path drives.
     * @param path Path to inspect.
     * @return The name, for example `L"\\\\.\\DISPLAY2"`, or nullopt when it cannot be read.
     */
    std::optional<std::wstring> source_name(const DISPLAYCONFIG_PATH_INFO &path) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME info {};
      info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      info.header.size = sizeof(info);
      info.header.adapterId = path.sourceInfo.adapterId;
      info.header.id = path.sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return std::wstring {info.viewGdiDeviceName};
    }

    /**
     * @brief Project paths onto the testable view.
     *
     * @param paths Paths from the display configuration database.
     * @return One entry per path, in the same order, so a selected index stays valid.
     */
    std::vector<display_path_t> project_paths(const path_list_t &paths) {
      std::vector<display_path_t> result;
      result.reserve(paths.size());
      for (const auto &path : paths) {
        display_path_t projected;
        projected.gdi_name = source_name(path).value_or(L"");
        projected.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        projected.target_available = path.targetInfo.targetAvailable == TRUE;
        result.push_back(std::move(projected));
      }
      return result;
    }

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
    topology_t compact_topology(path_list_t chosen, const mode_list_t &all_modes) {
      topology_t result;
      std::unordered_map<UINT32, UINT32> remapped;

      const auto intern = [&](UINT32 &mode_index) {
        if (mode_index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || mode_index >= all_modes.size()) {
          mode_index = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
          return;
        }

        if (const auto it = remapped.find(mode_index); it != remapped.end()) {
          mode_index = it->second;
          return;
        }

        const auto next = static_cast<UINT32>(result.modes.size());
        result.modes.push_back(all_modes[mode_index]);
        remapped.emplace(mode_index, next);
        mode_index = next;
      };

      for (auto &path : chosen) {
        intern(path.sourceInfo.modeInfoIdx);
        intern(path.targetInfo.modeInfoIdx);
        result.paths.push_back(std::move(path));
      }

      return result;
    }

    /**
     * @brief Hand a topology to the operating system.
     *
     * @param topology Layout to apply.
     * @return True when the layout was applied.
     *
     * @note The exact mode is requested first, because allowing Windows to adjust the request
     *       turns the requested resolution into a suggestion, which would make the captured frame
     *       differ from what the client asked for. Only a rejected strict request is retried with
     *       adjustment allowed, since a working display at an adjusted size beats no display.
     */
    bool apply_topology(topology_t topology) {
      const auto submit = [&topology](UINT32 flags) {
        return SetDisplayConfig(static_cast<UINT32>(topology.paths.size()), topology.paths.data(),
                                static_cast<UINT32>(topology.modes.size()), topology.modes.data(), flags);
      };

      auto status {submit(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE)};
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(debug) << "Display configuration was rejected as requested: "sv << error_text(status)
                         << "; retrying with adjustment allowed"sv;
        status = submit(SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE | SDC_ALLOW_CHANGES);
      }

      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Display configuration could not be applied: "sv << error_text(status);
        return false;
      }

      return true;
    }

    /**
     * @brief Build a single-display layout for one path.
     *
     * @param path Path of the display to isolate.
     * @param all_modes Mode array that @p path refers to.
     * @param geometry Geometry to present on that display.
     * @return The isolated layout, or nullopt when the path has no usable mode.
     */
    std::optional<topology_t> isolated_topology(const DISPLAYCONFIG_PATH_INFO &path,
                                                const mode_list_t &all_modes,
                                                const virtual_display_geometry_t &geometry) {
      if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
          path.sourceInfo.modeInfoIdx >= all_modes.size() ||
          path.targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID ||
          path.targetInfo.modeInfoIdx >= all_modes.size()) {
        return std::nullopt;
      }

      auto source_mode {all_modes[path.sourceInfo.modeInfoIdx]};
      if (source_mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        return std::nullopt;
      }

      auto target_mode {all_modes[path.targetInfo.modeInfoIdx]};
      if (target_mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
        return std::nullopt;
      }

      // The monitor was created at the requested geometry, so the mode in the database is the one
      // the driver reports and the one the operating system will accept. Only the desktop origin
      // is stated; rewriting the mode here would make a rejected layout more likely without
      // giving the client anything it did not already ask the driver for.
      if (source_mode.sourceMode.width != geometry.width || source_mode.sourceMode.height != geometry.height) {
        BOOST_LOG(warning) << "The virtual display reports "sv << source_mode.sourceMode.width << 'x'
                           << source_mode.sourceMode.height << " while "sv << geometry.width << 'x'
                           << geometry.height << " was requested; capture will use the reported mode"sv;
      }
      source_mode.sourceMode.position.x = 0;
      source_mode.sourceMode.position.y = 0;

      DISPLAYCONFIG_PATH_INFO isolated {path};
      isolated.flags |= DISPLAYCONFIG_PATH_ACTIVE;
      isolated.sourceInfo.modeInfoIdx = 0;
      isolated.targetInfo.modeInfoIdx = 1;

      topology_t result;
      result.paths.push_back(isolated);
      result.modes.push_back(source_mode);
      result.modes.push_back(target_mode);
      return result;
    }

    /**
     * @brief Collect the paths that are currently driving the desktop.
     * @param paths Every path from the display configuration database.
     * @param modes Mode array that @p paths refers to.
     * @return The active layout with a compact mode array, possibly empty.
     */
    topology_t active_topology(const path_list_t &paths, const mode_list_t &modes) {
      path_list_t active;
      for (const auto &path : paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
          active.push_back(path);
        }
      }
      return compact_topology(std::move(active), modes);
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
        // A display that is already presenting has a source mode Windows trusts; prefer it.
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
    topology_t topology;  ///< Paths and modes to put back.
  };

  display_topology_target_t::display_topology_target_t() = default;
  display_topology_target_t::~display_topology_target_t() = default;

  display_target_result_e display_topology_target_t::apply(const std::wstring &display_name,
                                                           const virtual_display_geometry_t &geometry) {
    std::lock_guard lock(_mutex);

    path_list_t paths;
    mode_list_t modes;
    if (!query_all_paths(paths, modes)) {
      return display_target_result_e::activation_failed;
    }

    const auto projected {project_paths(paths)};
    const auto chosen {find_display_path(projected, display_name)};
    if (!chosen) {
      BOOST_LOG(warning) << "Virtual display ["sv << utf_utils::to_utf8(display_name)
                         << "] is not in the display configuration database yet"sv;
      return display_target_result_e::device_missing;
    }

    const auto isolated {isolated_topology(paths[*chosen], modes, geometry)};
    if (!isolated) {
      BOOST_LOG(warning) << "Virtual display ["sv << utf_utils::to_utf8(display_name)
                         << "] has no usable display mode; capture will use the existing display"sv;
      return display_target_result_e::activation_failed;
    }

    // Save the pre-session layout before changing anything: a failed switch must still leave a
    // restore record, otherwise a partially applied change would never be undone. The oldest
    // record wins, because that is the layout the operator actually started from.
    if (!_saved) {
      auto saved {std::make_unique<saved_topology_t>()};
      saved->topology = active_topology(paths, modes);
      if (saved->topology.paths.empty()) {
        BOOST_LOG(warning) << "No active display layout was found to return to; not switching displays"sv;
        return display_target_result_e::activation_failed;
      }
      _saved = std::move(saved);
    }

    if (!apply_topology(*isolated)) {
      return display_target_result_e::activation_failed;
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
    _saved.reset();

    if (!apply_topology(std::move(saved->topology))) {
      BOOST_LOG(warning) << "The display layout from before the session could not be restored"sv;
      return false;
    }

    BOOST_LOG(info) << "Display layout restored after a "sv << _geometry.width << 'x' << _geometry.height << '@'
                    << _geometry.fps << " virtual display session"sv;
    return true;
  }
}  // namespace platf::windows
