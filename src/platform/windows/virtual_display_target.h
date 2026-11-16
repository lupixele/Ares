/**
 * @file src/platform/windows/virtual_display_target.h
 * @brief Points the streaming session at the virtual display that was just created.
 *
 * @details Creating a display is not the same as using it. Until the operating system is told to
 * activate the new monitor, the application launches on the operator's monitor and the client
 * captures that same physical desktop - the virtual display stays dark. This module closes that
 * gap.
 *
 * The session is prepared as an isolated display: the virtual display becomes the only active
 * display for the duration of the session, which is what makes a launched application appear on
 * the display the client is actually watching, and what makes the usual capture enumeration
 * return exactly one candidate.
 *
 * Both transitions are performed here, synchronously, with the Win32 display configuration API,
 * and the pre-session layout is kept in memory for the duration of the session:
 *
 * - On activation the current layout is captured before the new one is applied.
 * - On restore the captured layout is applied again and then forgotten.
 *
 * Doing the restore here rather than through the project's display-device persistence layer is
 * deliberate. That layer defers its revert by a configurable delay (three seconds by default), so
 * a revert requested just before the virtual display is removed would run after the display no
 * longer exists and fail against a topology that references it. Restoring synchronously while the
 * display is still present keeps both transitions correct, and keeps the operator's persisted
 * display-device state untouched by this feature.
 *
 * Nothing here is allowed to break streaming. A missing device or a rejected configuration is
 * reported and the session continues on whatever display it would have used anyway.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/platform/windows/virtual_display_geometry.h"

namespace platf::windows {
  /**
   * @brief Outcome of asking the host to switch to a virtual display.
   */
  enum class display_target_result_e {
    requested,          ///< The new layout was applied; the session owns the display now.
    device_missing,     ///< The new display never appeared in the display configuration database.
    activation_failed,  ///< The display exists, but the operating system refused the new layout.
  };

  /**
   * @brief Minimal view of one display configuration path.
   *
   * @details Exists so path selection is a pure function that unit tests can exercise without a
   *          display configuration database, a driver, or real monitors.
   */
  struct display_path_t {
    std::wstring gdi_name;     ///< Desktop name of the path's source, for example `L"\\\\.\\DISPLAY2"`.
    bool active {false};       ///< Path is currently driving an attached display.
    bool target_available {false};  ///< Target reports that it can be used.
  };

  /**
   * @brief Choose the path that belongs to a named display.
   *
   * @param paths Paths currently known to the display configuration database.
   * @param display_name Desktop name reported by the driver, for example `L"\\\\.\\DISPLAY2"`.
   * @return Index into @p paths of the chosen path, or nullopt when no display matches.
   *
   * @note A display that is already attached is preferred over one that is not, because its
   *       source mode is the one Windows is actually presenting. An empty name never matches: it
   *       would otherwise silently select an unrelated display.
   */
  [[nodiscard]] std::optional<std::size_t> find_display_path(const std::vector<display_path_t> &paths,
                                                             std::wstring_view display_name);

  /**
   * @brief Narrow seam that activates a virtual display and later restores the desktop.
   *
   * @note Tests substitute a recorder so lifecycle behaviour can be asserted without changing
   *       the operator's display layout.
   */
  class idisplay_target_t {
  public:
    virtual ~idisplay_target_t() = default;

    /**
     * @brief Make the named display the only active display.
     *
     * @param display_name Desktop name reported by the driver.
     * @param geometry Geometry the display was created with.
     * @return What happened; never throws.
     */
    virtual display_target_result_e apply(const std::wstring &display_name, const virtual_display_geometry_t &geometry) = 0;

    /**
     * @brief Restore the layout that was active before @ref apply.
     *
     * @return True when the previous layout is in place (or when there was nothing to restore).
     *
     * @note Safe to call when nothing was applied; never throws.
     */
    virtual bool restore() = 0;
  };

  /**
   * @brief Target backed by the Win32 display configuration API.
   *
   * @details Reads the layout before switching, applies an isolated single-display layout for the
   *          virtual display, and re-applies the saved layout on restore. The whole object is
   *          guarded by a mutex because session start and session end run on different threads.
   */
  class display_topology_target_t final: public idisplay_target_t {
  public:
    display_topology_target_t();

    /**
     * @brief Release the saved layout without touching the display configuration.
     */
    ~display_topology_target_t() override;

    display_target_result_e apply(const std::wstring &display_name, const virtual_display_geometry_t &geometry) override;
    bool restore() override;

  private:
    /**
     * @brief Layout that was active before this session changed it.
     */
    struct saved_topology_t;

    std::mutex _mutex;                              ///< Serializes session start and session end.
    std::unique_ptr<saved_topology_t> _saved;       ///< Layout to restore, or null when none is owed.
    virtual_display_geometry_t _geometry;           ///< Geometry of the applied display, for restore diagnostics.
  };
}  // namespace platf::windows
