/**
 * @file src/platform/windows/session_driver_boundary.h
 * @brief Narrow C++ session and virtual display driver boundary with owned lifecycle,
 * pre-create authorization, exact per-session allocation rollback, and heartbeat
 * consecutive counter reset guarantees.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace platf::windows {

  /**
   * @brief Status of the underlying virtual display driver boundary.
   */
  enum class driver_boundary_status_e : int32_t {
    ok = 0,                    ///< Driver handle ready and responsive.
    unknown = 1,               ///< Initial or unprobed state.
    unauthorized = -1,         ///< Precreate authorization failed (permission/client check denied).
    invalid_dimensions = -2,   ///< Requested resolution or fps does not meet boundary constraints.
    allocation_failed = -3,    ///< Driver failed to allocate virtual display.
    heartbeat_stale = -4,      ///< Consecutive heartbeat failures exceeded threshold.
    driver_not_ready = -5      ///< Virtual display device IOCTL handle unavailable.
  };

  /**
   * @brief Specification for requesting a session-scoped virtual display.
   */
  struct session_display_spec_t {
    std::string client_uid;       ///< Unique identifier of the requesting Moonlight/Apollo client.
    std::string client_name;      ///< Friendly client name.
    uint32_t width {1920};        ///< Target display width in pixels.
    uint32_t height {1080};       ///< Target display height in pixels.
    uint32_t fps {60};            ///< Target refresh rate.
    bool isolated_display {false};///< Whether isolated single-display streaming mode is requested.
  };

  /**
   * @brief Snapshot of active session-bound virtual display allocation.
   */
  struct session_display_allocation_t {
    std::string session_id;       ///< Unique session token/UUID.
    std::wstring display_name;    ///< Target system display adapter/monitor path.
    session_display_spec_t spec;  ///< Initial configuration parameters.
    std::chrono::steady_clock::time_point created_at; ///< Timestamp when allocation completed.
  };

  /**
   * @brief Low-level driver communication interface stubbed for native testing and execution.
   */
  class idriver_backend_t {
  public:
    virtual ~idriver_backend_t() = default;

    /**
     * @brief Check whether driver IOCTL handle is available.
     * @return True if available, false otherwise.
     */
    virtual bool is_driver_ready() = 0;

    /**
     * @brief Allocate a virtual display device via IOCTL.
     * @param spec Display specifications.
     * @return Assigned display device name or empty on failure.
     */
    virtual std::optional<std::wstring> allocate_display(const session_display_spec_t &spec) = 0;

    /**
     * @brief Release an allocated virtual display device via IOCTL.
     * @param display_name System display name to remove.
     * @return True if teardown succeeded.
     */
    virtual bool release_display(const std::wstring &display_name) = 0;

    /**
     * @brief Ping the virtual display driver watchdog.
     * @return True on successful ping response.
     */
    virtual bool ping_watchdog() = 0;
  };

  /**
   * @brief Policy interface for validating pre-create authorization for a session.
   */
  class isession_authorizer_t {
  public:
    virtual ~isession_authorizer_t() = default;

    /**
     * @brief Validate whether client is authorized to request a virtual display.
     * @param session_id Session token.
     * @param spec Requested parameters.
     * @return True if authorized, false otherwise.
     */
    virtual bool authorize_session(const std::string &session_id, const session_display_spec_t &spec) = 0;
  };

  /**
   * @brief Session display boundary managing owned lifecycles and rollback.
   */
  class session_driver_boundary_t {
  public:
    /**
     * @brief Construct boundary manager with injected authorizer and backend.
     * @param authorizer Client authorization provider.
     * @param backend Low-level virtual display IOCTL backend.
     * @param max_consecutive_heartbeat_fails Max dropped pings before state flips to stale.
     */
    explicit session_driver_boundary_t(
      std::shared_ptr<isession_authorizer_t> authorizer,
      std::shared_ptr<idriver_backend_t> backend,
      uint32_t max_consecutive_heartbeat_fails = 3
    );

    ~session_driver_boundary_t();

    /**
     * @brief Request, authorize, and allocate a virtual display bound strictly to a session.
     *
     * If allocation or configuration fails midway, any partial allocation is immediately rolled back.
     *
     * @param session_id Unique session identifier.
     * @param spec Requested display properties.
     * @return Allocated session display record, or status code error.
     */
    std::pair<driver_boundary_status_e, std::optional<session_display_allocation_t>>
    acquire_session_display(const std::string &session_id, const session_display_spec_t &spec);

    /**
     * @brief Explicitly release and rollback a session's allocated virtual display.
     * @param session_id Session identifier.
     * @return True if found and cleanly destroyed, false if session had no allocation.
     */
    bool release_session_display(const std::string &session_id);

    /**
     * @brief Tick watchdog heartbeat and manage consecutive failure counters.
     *
     * Consecutive failure counter resets to 0 immediately on any successful ping.
     *
     * @return True if heartbeat is healthy, false if failure threshold exceeded.
     */
    bool tick_heartbeat();

    /**
     * @brief Get active consecutive heartbeat failure count.
     * @return Number of consecutive unacknowledged pings.
     */
    uint32_t get_consecutive_heartbeat_failures() const;

    /**
     * @brief Total count of actively managed sessions.
     * @return Number of active session allocations.
     */
    size_t get_active_session_count() const;

    /**
     * @brief Check whether a session currently holds an allocated virtual display.
     * @param session_id Session identifier.
     * @return True if session is registered.
     */
    bool has_session(const std::string &session_id) const;

    /**
     * @brief Rollback and destroy all active session allocations (used on daemon teardown/fail).
     */
    void rollback_all();

  private:
    std::shared_ptr<isession_authorizer_t> _authorizer;
    std::shared_ptr<idriver_backend_t> _backend;
    uint32_t _max_consecutive_heartbeat_fails;

    mutable std::mutex _mutex;
    uint32_t _consecutive_heartbeat_fails {0};
    std::unordered_map<std::string, session_display_allocation_t> _active_sessions;
  };

}  // namespace platf::windows
