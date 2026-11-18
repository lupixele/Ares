/**
 * @file src/platform/windows/virtual_display_topology.cpp
 * @brief Implementation of the display-configuration data helpers.
 */

// located in src/platform/windows

// standard includes
#include <cstdint>
#include <system_error>
#include <unordered_map>
#include <utility>

// platform includes
#include <Windows.h>

// local includes
#include "src/logging.h"
#include "src/platform/windows/virtual_display_topology.h"

namespace platf::windows::detail {
  using namespace std::literals;

  std::string describe_error(LONG code) {
    return std::system_category().message(static_cast<int>(code));
  }

  bool query_topology(bool only_active, path_list_t &paths, mode_list_t &modes) {
    const UINT32 flags {static_cast<UINT32>((only_active ? QDC_ONLY_ACTIVE_PATHS : QDC_ALL_PATHS) | QDC_VIRTUAL_MODE_AWARE)};

    // Two queries are needed by design: the size query and the data query can disagree when a
    // display is added or removed in between, which is exactly what happens during session start.
    // The buffer is therefore re-queried once instead of trusting the first size.
    for (int attempt = 0; attempt < 2; ++attempt) {
      UINT32 path_count {0};
      UINT32 mode_count {0};
      if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Could not size the display configuration buffers"sv;
        return false;
      }

      paths.resize(path_count);
      modes.resize(mode_count);
      const auto status {QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr)};
      if (status == ERROR_INSUFFICIENT_BUFFER) {
        continue;
      }
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "Could not read the display configuration: "sv << describe_error(status);
        return false;
      }

      paths.resize(path_count);
      modes.resize(mode_count);
      return true;
    }

    BOOST_LOG(warning) << "Display configuration kept changing while it was being read"sv;
    return false;
  }

  std::optional<std::wstring> path_source_name(const DISPLAYCONFIG_PATH_INFO &path) {
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

  display_identity_t identity_of(const DISPLAYCONFIG_PATH_INFO &path) {
    return {path.targetInfo.adapterId, path.targetInfo.id};
  }

  std::optional<std::size_t> find_path_by_identity(const path_list_t &paths, const display_identity_t &identity) {
    for (std::size_t index = 0; index < paths.size(); ++index) {
      if (identity_of(paths[index]) == identity) {
        return index;
      }
    }
    return std::nullopt;
  }

  bool is_sole_active_path(const path_list_t &paths, const display_identity_t &identity) {
    bool ours_is_active {false};
    for (const auto &path : paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }

      if (identity_of(path) == identity) {
        ours_is_active = true;
        continue;
      }

      // Some other display is still in use, so the layout is not isolated.
      return false;
    }
    return ours_is_active;
  }

  std::vector<display_path_t> project_paths(const path_list_t &paths) {
    std::vector<display_path_t> result;
    result.reserve(paths.size());
    for (const auto &path : paths) {
      display_path_t projected;
      projected.gdi_name = path_source_name(path).value_or(L"");
      projected.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      projected.target_available = path.targetInfo.targetAvailable == TRUE;
      result.push_back(std::move(projected));
    }
    return result;
  }

  DISPLAYCONFIG_PATH_INFO path_without_modes(const DISPLAYCONFIG_PATH_INFO &path) {
    DISPLAYCONFIG_PATH_INFO result {path};
    result.sourceInfo.sourceModeInfoIdx = DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
    result.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    result.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
    result.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    return result;
  }

  DISPLAYCONFIG_PATH_INFO proposed_topology_path(const DISPLAYCONFIG_PATH_INFO &path) {
    auto result {path_without_modes(path)};
    result.sourceInfo.cloneGroupId = 0;
    return result;
  }

  bool apply_geometry(DISPLAYCONFIG_PATH_INFO &path,
                      DISPLAYCONFIG_MODE_INFO &source_mode,
                      const virtual_display_geometry_t &geometry) {
    const auto size_matches {source_mode.sourceMode.width == geometry.width &&
                             source_mode.sourceMode.height == geometry.height};

    // The refresh rate is a rational, so compare its value rather than its two fields: Windows
    // reports the same rate as 60/1, 60000/1000 or 120/2 depending on the driver.
    const auto &refresh {path.targetInfo.refreshRate};
    const auto refresh_matches {refresh.Denominator != 0 &&
                                static_cast<std::uint64_t>(refresh.Numerator) ==
                                  static_cast<std::uint64_t>(geometry.fps) * refresh.Denominator};

    if (size_matches && refresh_matches) {
      return false;
    }

    source_mode.sourceMode.width = geometry.width;
    source_mode.sourceMode.height = geometry.height;

    path.targetInfo.refreshRate = {geometry.fps, 1};

    // Windows chooses the target mode: stating one would either be accepted verbatim, which can
    // contradict the source resolution, or silently ignored. Its own choice always matches.
    path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    path.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;

    return true;
  }

  display_topology_t compact_topology(path_list_t chosen, const mode_list_t &all_modes) {
    display_topology_t result;
    std::unordered_map<UINT32, UINT32> remapped;

    // Every mode index is a 16-bit field, so an index that was never set reads back as 0xffff
    // rather than as the 32-bit invalid value.
    constexpr UINT32 unset_index {0xffff};

    const auto intern = [&](UINT32 mode_index) -> UINT32 {
      if (mode_index == unset_index || mode_index >= all_modes.size()) {
        return DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
      }

      if (const auto it = remapped.find(mode_index); it != remapped.end()) {
        return it->second;
      }

      const auto next = static_cast<UINT32>(result.modes.size());
      result.modes.push_back(all_modes[mode_index]);
      remapped.emplace(mode_index, next);
      return next;
    };

    for (auto &path : chosen) {
      path.sourceInfo.sourceModeInfoIdx = intern(path.sourceInfo.sourceModeInfoIdx);
      path.targetInfo.targetModeInfoIdx = intern(path.targetInfo.targetModeInfoIdx);
      // The desktop image index addresses the same mode array, so it has to be rewritten too or
      // the restored layout would point at whatever entry happens to occupy that slot.
      path.targetInfo.desktopModeInfoIdx = intern(path.targetInfo.desktopModeInfoIdx);
      result.paths.push_back(std::move(path));
    }

    return result;
  }

  display_topology_t active_topology(const path_list_t &paths, const mode_list_t &modes) {
    path_list_t active;
    for (const auto &path : paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0) {
        active.push_back(path);
      }
    }
    return compact_topology(std::move(active), modes);
  }
}  // namespace platf::windows::detail
