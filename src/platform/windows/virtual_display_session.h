/**
 * @file src/platform/windows/virtual_display_session.h
 * @brief Seam between the streaming session path and session-scoped virtual displays.
 *
 * @details The streaming path lives in cross-platform files and must stay that way. This header is
 * the only place that knows a virtual display is a Windows feature backed by the installed
 * SudoVDA driver, and it is compiled only on Windows.
 *
 * Two free functions are exposed so callers need a single `#ifdef _WIN32` and no state threading:
 *
 * - @ref session_virtual_display_begin allocates a display for a starting session.
 * - @ref session_virtual_display_end releases whatever the most recent begin allocated.
 *
 * The crucial property is that a session is released exactly once, no matter which of the many
 * early-return and error paths in the streaming code is taken. That is what prevents a phantom
 * display being left on the operator's desktop, and it is why the release is driven by an owning
 * object rather than by remembering to call a function on every branch.
 */

// located in src/platform/windows

#pragma once

// local includes
#include "src/config.h"
#include "src/rtsp.h"
#include "src/platform/windows/virtual_display_manager.h"

namespace platf::windows {
  /**
   * @brief Manager used for session virtual displays in this process.
   *
   * @details Lazily constructed on first use so a host that never enables the feature never opens
   * the driver. Replaced by tests to observe wiring without touching a real display.
   *
   * @return Reference to the manager; never null.
   */
  [[nodiscard]] virtual_display_manager_t &session_virtual_display_manager();

  /**
   * @brief Allocate a virtual display for a starting session.
   *
   * @param video Video configuration, used to decide whether the feature is enabled.
   * @param launch_session Session that is starting.
   *
   * @note Safe to call when the feature is disabled: nothing is allocated and the session
   *       proceeds exactly as it would have. Never throws and never fails the session.
   */
  void session_virtual_display_begin(const config::video_t &video, const rtsp_stream::launch_session_t &launch_session);

  /**
   * @brief Release the display held by the most recent session, if any.
   *
   * @note Idempotent and never throws. Calling it when no display was allocated is a no-op, so
   *       it can be called unconditionally from every teardown path.
   */
  void session_virtual_display_end();

  /**
   * @brief Whether the most recent session currently holds a virtual display.
   * @return True when a display is held.
   */
  [[nodiscard]] bool session_virtual_display_active();
}  // namespace platf::windows
