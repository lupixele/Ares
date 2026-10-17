/**
 * @file src/platform/windows/virtual_display_geometry.cpp
 * @brief Resolution policy implementation used before hand-off to the unmodified SudoVDA driver.
 */

// located in src/platform/windows

// standard includes
#include <algorithm>

// local includes
#include "src/platform/windows/virtual_display_geometry.h"

namespace platf::windows {
  namespace {
    /** @brief Horizontal resolution used when the client does not specify one. */
    constexpr uint32_t DEFAULT_WIDTH = 1920;
    /** @brief Vertical resolution used when the client does not specify one. */
    constexpr uint32_t DEFAULT_HEIGHT = 1080;
    /** @brief Refresh rate used when the client does not specify one. */
    constexpr uint32_t DEFAULT_FPS = 60;
    /** @brief Scale factor percentage treated as "no scaling". */
    constexpr int DEFAULT_SCALE_PERCENT = 100;

    /** @brief Largest virtual display area that still behaves like a normal panel. */
    constexpr uint32_t MAX_WIDTH = 7680;
    /** @brief Largest virtual display height that still behaves like a normal panel. */
    constexpr uint32_t MAX_HEIGHT = 4320;

    /** @brief Lowest useful display refresh rate. */
    constexpr uint32_t MIN_FPS = 30;
    /** @brief Highest refresh rate a display mode can report. */
    constexpr uint32_t MAX_FPS = 240;

    /** @brief Smallest dimension that still produces a usable panel. */
    constexpr uint32_t MIN_DIMENSION = 320;

    /**
     * @brief Round down to an even number.
     * @param value Value to round.
     * @return The value rounded down to the next even integer.
     *
     * @note Windows display modes and most encoders require even dimensions. Odd values are
     *       silently adjusted rather than rejected, because a 1-pixel round-off is harmless.
     */
    uint32_t make_even(uint32_t value) {
      return value & ~static_cast<uint32_t>(1);
    }
  }  // namespace

  uint32_t clamp_refresh_rate(uint32_t requested_fps) {
    if (requested_fps == 0) {
      return DEFAULT_FPS;
    }
    return std::clamp(requested_fps, MIN_FPS, MAX_FPS);
  }

  std::optional<virtual_display_geometry_t> clamp_oversized_resolution(uint32_t requested_width, uint32_t requested_height) {
    if (requested_width <= MAX_WIDTH && requested_height <= MAX_HEIGHT) {
      return std::nullopt;
    }

    // Preserve the aspect ratio while fitting inside the supported panel size.
    const auto width_ratio = static_cast<double>(MAX_WIDTH) / static_cast<double>(requested_width);
    const auto height_ratio = static_cast<double>(MAX_HEIGHT) / static_cast<double>(requested_height);
    const auto ratio = std::min(width_ratio, height_ratio);

    auto clamped_width = static_cast<uint32_t>(static_cast<double>(requested_width) * ratio);
    auto clamped_height = static_cast<uint32_t>(static_cast<double>(requested_height) * ratio);

    // Never fall below a usable panel, even if the clamp math would have.
    if (clamped_width < MIN_DIMENSION) {
      clamped_width = MIN_DIMENSION;
    }
    if (clamped_height < MIN_DIMENSION) {
      clamped_height = MIN_DIMENSION;
    }

    return virtual_display_geometry_t {make_even(clamped_width), make_even(clamped_height)};
  }

  virtual_display_geometry_t resolve_virtual_display_geometry(int client_width,
                                                             int client_height,
                                                             int client_fps,
                                                             int scale_factor) {
    auto width = static_cast<uint32_t>(client_width > 0 ? client_width : DEFAULT_WIDTH);
    auto height = static_cast<uint32_t>(client_height > 0 ? client_height : DEFAULT_HEIGHT);
    auto fps = static_cast<uint32_t>(client_fps > 0 ? client_fps : DEFAULT_FPS);

    if (scale_factor > 0 && scale_factor != DEFAULT_SCALE_PERCENT) {
      // Scaling is a percentage, so 125 means 1.25x. Round to nearest and then force even.
      const double scale = static_cast<double>(scale_factor) / static_cast<double>(DEFAULT_SCALE_PERCENT);
      width = make_even(static_cast<uint32_t>(static_cast<double>(width) * scale + 0.5));
      height = make_even(static_cast<uint32_t>(static_cast<double>(height) * scale + 0.5));
    }

    // Guarantee a floor before scaling can shrink a display into nothing.
    if (width < MIN_DIMENSION) {
      width = MIN_DIMENSION;
    }
    if (height < MIN_DIMENSION) {
      height = MIN_DIMENSION;
    }

    width = make_even(width);
    height = make_even(height);

    if (const auto clamped = clamp_oversized_resolution(width, height)) {
      width = clamped->width;
      height = clamped->height;
    }

    return {width, height, clamp_refresh_rate(fps)};
  }
}  // namespace platf::windows
