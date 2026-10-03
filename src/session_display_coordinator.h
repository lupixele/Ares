/**
 * @file src/session_display_coordinator.h
 * @brief High-level session display coordinator managing paired streaming session
 * display lifecycle, resolution validation, capabilities, publication/probe/app states,
 * and display restoration.
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
#include <vector>

namespace ares::session {

  /**
   * @brief Lifecycle states of a session display.
   */
  enum class session_display_state_e {
    prepared,     ///< Initial state after creation, pre-create authorized.
    pending,      ///< Awaiting backend allocation / probe / acceptance.
    streaming,    ///< Display active and bound to streaming session.
    app_retained, ///< Stream disconnected but display retained by active application.
    restoring,    ///< Restoration in progress, awaiting display ack / release.
    released,     ///< Display allocation cleanly released.
    unresolved    ///< Display cleanup or restore failed; resource retained for reconciliation.
  };

  /**
   * @brief Status result of coordinator operations.
   */
  enum class coordinator_status_e {
    ok = 0,
    unauthorized = -1,
    invalid_resolution = -2,
    invalid_state = -3,
    allocation_failed = -4,
    timeout = -5,
    probe_failed = -6,
    app_start_failed = -7,
    unresolved_resource = -8,
    session_not_found = -9
  };

  /**
   * @brief Strongly-typed paired principal identity.
   */
  struct paired_principal_t {
    std::string client_id;      ///< Client unique identifier.
    std::string friendly_name;  ///< Friendly client name.
    std::string public_key_id;  ///< Public key ID or fingerprint.
  };

  /**
   * @brief Resolution and display mode specification.
   */
  struct display_mode_spec_t {
    uint32_t width {1920};
    uint32_t height {1080};
    uint32_t refresh_rate {60};
    bool hdr_capable {false};
  };

  /**
   * @brief Cryptographically signed resolution request.
   */
  struct signed_resolution_request_t {
    display_mode_spec_t mode;
    std::string signature;
    std::string server_session_token;
  };

  /**
   * @brief Apollo-compatible permission flags.
   */
  enum class apollo_permission_flags_e : uint32_t {
    none = 0,
    virtual_display_allowed = 1 << 0,
    resolution_change_allowed = 1 << 1,
    hdr_allowed = 1 << 2
  };

  inline apollo_permission_flags_e operator|(apollo_permission_flags_e a, apollo_permission_flags_e b) {
    return static_cast<apollo_permission_flags_e>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  inline apollo_permission_flags_e operator&(apollo_permission_flags_e a, apollo_permission_flags_e b) {
    return static_cast<apollo_permission_flags_e>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  /**
   * @brief Session display snapshot.
   */
  struct session_display_record_t {
    std::string session_id;
    paired_principal_t principal;
    std::string server_session_token;
    display_mode_spec_t mode;
    session_display_state_e state;
    std::wstring allocated_resource;
    uint64_t generation {0};
    bool app_retained {false};
  };

  /**
   * @brief Monotonic clock interface for deterministic testing.
   */
  class iclock_t {
  public:
    virtual ~iclock_t() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
  };

  /**
   * @brief Trusted registry for pre-create authorization and permissions.
   */
  class itrusted_registry_t {
  public:
    virtual ~itrusted_registry_t() = default;

    /**
     * @brief Check if principal is paired and get granted permissions.
     * Permissions must be explicitly granted; unset is denied.
     */
    virtual std::optional<apollo_permission_flags_e> get_principal_permissions(
      const paired_principal_t &principal
    ) = 0;

    /**
     * @brief Validate signed resolution request against principal.
     */
    virtual bool verify_resolution_signature(
      const paired_principal_t &principal,
      const signed_resolution_request_t &request
    ) = 0;
  };

  /**
   * @brief Display allocation and IO provider (e.g. wrapping Sunshine display manager / driver boundary).
   */
  class idisplay_provider_t {
  public:
    virtual ~idisplay_provider_t() = default;

    /**
     * @brief Allocate virtual display device.
     */
    virtual std::optional<std::wstring> allocate_display(const display_mode_spec_t &mode) = 0;

    /**
     * @brief Release virtual display device.
     */
    virtual bool release_display(const std::wstring &resource_name) = 0;

    /**
     * @brief Probe display readiness after allocation.
     */
    virtual bool probe_display(const std::wstring &resource_name) = 0;

    /**
     * @brief Request display restoration to previous topology/resolution.
     */
    virtual bool restore_display(const std::wstring &resource_name) = 0;

    /**
     * @brief Notify failure of release or restore for exact resource tracking.
     */
    virtual void on_unresolved_resource(const std::wstring &resource_name) noexcept = 0;
  };

  /**
   * @brief Application lifecycle notifier.
   */
  class iapp_lifecycle_t {
  public:
    virtual ~iapp_lifecycle_t() = default;

    /**
     * @brief Start app on display.
     */
    virtual bool start_app(const std::string &session_id, const std::wstring &display_resource) = 0;

    /**
     * @brief Acknowledge stop/join of application before restore/release.
     */
    virtual bool stop_and_join_app(const std::string &session_id) = 0;
  };

  /**
   * @brief Core session display coordinator.
   */
  class session_display_coordinator_t {
  public:
    /**
     * @brief Construct coordinator with injected dependencies.
     */
    explicit session_display_coordinator_t(
      std::shared_ptr<itrusted_registry_t> registry,
      std::shared_ptr<idisplay_provider_t> display_provider,
      std::shared_ptr<iapp_lifecycle_t> app_lifecycle,
      std::shared_ptr<iclock_t> clock,
      std::chrono::milliseconds pending_timeout = std::chrono::milliseconds(5000)
    );

    ~session_display_coordinator_t() noexcept;

    // Non-copyable
    session_display_coordinator_t(const session_display_coordinator_t &) = delete;
    session_display_coordinator_t &operator=(const session_display_coordinator_t &) = delete;

    /**
     * @brief Step 1: Prepare session with pre-create authorization from trusted registry.
     */
    coordinator_status_e prepare_session(
      const std::string &session_id,
      const paired_principal_t &principal,
      const std::string &server_session_token,
      const signed_resolution_request_t &request
    );

    /**
     * @brief Step 2: Begin display allocation and probe (transitions prepared -> pending -> streaming).
     */
    coordinator_status_e start_streaming_display(const std::string &session_id);

    /**
     * @brief Check pending timeouts and clean up expired pending sessions.
     */
    size_t check_pending_timeouts();

    /**
     * @brief Handle client stream disconnect. If app_retained is true, display stays active.
     */
    coordinator_status_e on_stream_disconnected(const std::string &session_id, bool app_retained);

    /**
     * @brief Resume a disconnected session (reuses existing allocation if app_retained).
     */
    coordinator_status_e resume_streaming(
      const std::string &session_id,
      const std::string &server_session_token
    );

    /**
     * @brief Stop session display: stop/join app, restore display, ack, then release.
     */
    coordinator_status_e stop_session(const std::string &session_id);

    /**
     * @brief Generation-safe late callback handler. Returns false if generation mismatch or cancelled.
     */
    bool handle_late_callback(const std::string &session_id, uint64_t generation, std::function<void()> cb);

    /**
     * @brief Cancel/invalidate active operation generation.
     */
    void cancel_session_operations(const std::string &session_id);

    /**
     * @brief Retry restoration of an unresolved resource.
     */
    coordinator_status_e retry_unresolved_restore(const std::string &session_id);

    /**
     * @brief Get snapshot record for a session.
     */
    std::optional<session_display_record_t> get_session(const std::string &session_id) const;

    /**
     * @brief Get list of currently unresolved display resources.
     */
    std::vector<std::wstring> get_unresolved_resources() const;

    /**
     * @brief Get total active session count.
     */
    size_t get_session_count() const;

  private:
    std::shared_ptr<itrusted_registry_t> registry_;
    std::shared_ptr<idisplay_provider_t> display_provider_;
    std::shared_ptr<iapp_lifecycle_t> app_lifecycle_;
    std::shared_ptr<iclock_t> clock_;
    std::chrono::milliseconds pending_timeout_;

    struct session_entry_t {
      session_display_record_t record;
      std::chrono::steady_clock::time_point pending_start {};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, session_entry_t> sessions_;
    std::vector<std::wstring> unresolved_resources_;
  };

}  // namespace ares::session
