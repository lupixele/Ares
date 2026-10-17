/**
 * @file src/platform/windows/virtual_display_geometry.h
 * @brief Host-side resolution policy for virtual display creation.
 *
 * @details SudoVDA is used unmodified: the driver creates a monitor of whatever size the host asks
 * for. This module decides what size to ask for, so that the Windows touch keyboard keeps docking
 * predictably on the virtual display.
 *
 * Background, stated plainly: on a high-DPI virtual display, Windows can place the touch keyboard
 * poorly when the display is much larger than what the client actually rendered, and when the
 * display does not match a mode the OS reports as a physical panel. The fix that costs nothing and
 * breaks nothing is to keep the requested monitor close to a size Windows treats as a normal panel
 * and to keep the requested mode achievable, instead of forwarding an unbounded client dimension.
 *
 * This is deliberately a pure function of its inputs: no driver handle, no display state, no
 * singletons. That makes it testable on any host, with or without a driver installed.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <cstdint>
#include <optional>

namespace platf::windows {
  /**
   * @brief Resolution the host requests when creating a virtual display.
   */
  struct virtual_display_geometry_t {
    uint32_t width {1920};   ///< Requested horizontal resolution in pixels.
    uint32_t height {1080};  ///< Requested vertical resolution in pixels.
    uint32_t fps {60};       ///< Requested refresh rate in hertz.
  };

  /**
   * @brief Clamp a requested refresh rate to the range the driver and a normal panel accept.
   * @param requested_fps Refresh rate the client asked for.
   * @return A sane refresh rate in hertz; never zero.
   *
   * @note 240 fps is the practical upper bound for a display mode. A client asking for more is
   *       limited rather than passed on, since the driver would reject or mis-handle it.
   */
  [[nodiscard]] uint32_t clamp_refresh_rate(uint32_t requested_fps);

  /**
   * @brief Reduce an oversized request to the largest supported panel size that preserves aspect.
   * @param requested_width Requested horizontal resolution.
   * @param requested_height Requested vertical resolution.
   * @return The clamped resolution, or nullopt when the request is already within limits.
   *
   * @note The cap exists because extremely large virtual displays misplace the Windows touch
   *       keyboard and some DPIs, and because capture cost scales with area. Aspect ratio is kept
   *       so a 21:9 phone is not squashed into a 16:9 shape.
   */
  [[nodiscard]] std::optional<virtual_display_geometry_t>
  clamp_oversized_resolution(uint32_t requested_width, uint32_t requested_height);

  /**
   * @brief Derive the resolution Ares should request for a virtual display.
   *
   * @param client_width Width requested by the streaming client, 0 meaning unset.
   * @param client_height Height requested by the streaming client, 0 meaning unset.
   * @param client_fps Refresh rate requested by the streaming client, 0 meaning unset.
   * @param scale_factor Client scale factor percentage, values <= 0 treated as 100.
   * @return The geometry to hand to the driver.
   *
   * @details Applies, in order: defaulting of unset values, client scaling, even-dimension
   *          enforcement, refresh-rate clamping and oversized-resolution clamping. Every step
   *          never reduces below a usable panel, so a bad client cannot request a broken display.
   */
  [[nodiscard]] virtual_display_geometry_t resolve_virtual_display_geometry(int client_width,
                                                                            int client_height,
                                                                            int client_fps,
                                                                            int scale_factor);
}  // namespace platf::windows
