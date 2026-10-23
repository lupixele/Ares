/**
 * @file src/platform/windows/virtual_display_manager.h
 * @brief Session-scoped virtual display lifecycle manager.
 *
 * @details This is the piece that connects a streaming session to a virtual display: when a
 * session starts, one display is created and its size is chosen by
 * platf::windows::resolve_virtual_display_geometry; when the session ends, the display is removed.
 * It uses the SudoVDA driver exactly as installed and modifies nothing about the driver itself.
 *
 * Two properties make it safe to sit in the streaming path:
 *
 * - **Failure never blocks streaming.** If the driver is absent, or a create request is rejected,
 *   the manager reports that no display was allocated and the session continues on whatever
 *   display it would have used anyway. Streaming must not die because virtual display failed.
 * - **Teardown is idempotent and never throws.** A session that ends is released exactly once,
 *   whatever else happened, and a release that cannot be confirmed is retained rather than
 *   forgotten, so a later run can retry it.
 *
 * The manager deliberately owns no process-global state and takes its dependencies through its
 * constructor, so unit tests can substitute a recording backend without touching real displays.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// local includes
#include "src/platform/windows/session_driver_boundary.h"
#include "src/platform/windows/session_heartbeat.h"
#include "src/platform/windows/sudovda_driver.h"
#include "src/platform/windows/virtual_display_geometry.h"

namespace platf::windows {
  /**
   * @brief Outcome of asking for a virtual display for a session.
   */
  enum class virtual_display_result_e {
    created,          ///< A display was allocated for this session.
    already_present,  ///< This session already holds a display; the existing one was kept.
    unavailable,      ///< No driver, or the request was refused. Streaming may continue.
    rejected,         ///< The session was not permitted a virtual display by policy.
  };

  /**
   * @brief Per-session bookkeeping, exposed for tests and diagnostics.
   */
  struct virtual_display_session_t {
    std::string session_id;              ///< Session identity the display is bound to.
    std::wstring display_name;           ///< Device name of the allocated display.
    virtual_display_geometry_t geometry; ///< Resolution actually handed to the driver.
  };

  /**
   * @brief Session-scoped virtual display manager.
   *
   * @note The manager owns one watchdog keep-alive for the process. It runs while any session holds
   *       a display and stops when the last one is released, so a long stream cannot lose its
   *       display to an un-pinged driver watchdog.
   */
  class virtual_display_manager_t {
  public:
    /**
     * @brief Policy deciding whether a session may hold a virtual display.
     *
     * @details Defaults to "yes" because new pairings already default to full permissions, and
     *          because refusing here would silently remove a working feature. It exists as a seam
     *          so a future permission policy can be plugged in without touching the lifecycle.
     */
    class isession_policy_t {
    public:
      virtual ~isession_policy_t() = default;
      /**
       * @brief Whether the session may request a virtual display.
       * @return True when permitted.
       */
      [[nodiscard]] virtual bool allows_virtual_display() const = 0;
    };

    /**
     * @brief Default policy that permits every session.
     */
    class allow_all_policy_t final : public isession_policy_t {
    public:
      [[nodiscard]] bool allows_virtual_display() const override {
        return true;
      }
    };

    /**
     * @brief Report used when a session's display could not be fully released.
     */
    struct teardown_report_t {
      std::string session_id;                 ///< Session the report concerns.
      bool released {false};                  ///< Whether the driver confirmed removal.
      std::optional<std::wstring> retained;   ///< Display left owned by the manager, if any.
    };

    virtual_display_manager_t(std::shared_ptr<session_driver_boundary_t> boundary,
                              std::shared_ptr<isession_policy_t> policy = nullptr);

    /**
     * @brief Construct a manager with an explicit heartbeat, for tests.
     *
     * @param boundary Driver boundary used to allocate and release displays.
     * @param policy Session policy deciding which sessions may hold a display.
     * @param heartbeat Keep-alive loop; may be substituted to observe ping scheduling.
     */
    virtual_display_manager_t(std::shared_ptr<session_driver_boundary_t> boundary,
                              std::shared_ptr<isession_policy_t> policy,
                              std::shared_ptr<session_heartbeat_t> heartbeat);

    /**
     * @brief Request a virtual display for a session.
     *
     * @param session_id Identity of the starting session.
     * @param client_width Width requested by the client, 0 meaning unset.
     * @param client_height Height requested by the client, 0 meaning unset.
     * @param client_fps Refresh rate requested by the client, 0 meaning unset.
     * @param scale_factor Client scale factor percentage, <= 0 treated as 100.
     * @return What happened, so the caller knows whether streaming is affected.
     *
     * @note Never throws. A failure here must not stop the session from starting.
     */
    [[nodiscard]] virtual_display_result_e acquire(const std::string &session_id,
                                                  int client_width,
                                                  int client_height,
                                                  int client_fps,
                                                  int scale_factor);

    /**
     * @brief Release the display held by a session.
     *
     * @param session_id Identity of the ending session.
     * @return A report describing whether the driver confirmed removal.
     *
     * @note Never throws, and is safe to call for a session that never acquired a display.
     */
    [[nodiscard]] teardown_report_t release(const std::string &session_id);

    /**
     * @brief Release every session display still held.
     * @return Reports for each session that still held a display.
     *
     * @note Used on shutdown. Never throws.
     */
    [[nodiscard]] std::vector<teardown_report_t> release_all();

    /**
     * @brief Whether a session currently holds a display.
     * @param session_id Identity to check.
     * @return True when a display is bound to the session.
     */
    [[nodiscard]] bool has_display(const std::string &session_id) const;

    /**
     * @brief Whether the watchdog keep-alive is currently running.
     * @return True while at least one session holds a display and the loop is pinging.
     */
    [[nodiscard]] bool heartbeat_running() const;

    /**
     * @brief Snapshot of every active session display.
     * @return Copy of the current bookkeeping.
     */
    [[nodiscard]] std::vector<virtual_display_session_t> active_sessions() const;

  private:
    /**
     * @brief Underlying driver boundary; may be null in tests.
     */
    std::shared_ptr<session_driver_boundary_t> _boundary;
    /**
     * @brief Session policy; never null after construction.
     */
    std::shared_ptr<isession_policy_t> _policy;

    /**
     * @brief Watchdog keep-alive; never null after construction.
     */
    std::shared_ptr<session_heartbeat_t> _heartbeat;

    mutable std::mutex _mutex;                ///< Guards _sessions.
    std::map<std::string, virtual_display_session_t> _sessions;  ///< Active session displays.
  };
}  // namespace platf::windows
