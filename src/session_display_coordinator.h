/** @file
 * @brief Experimental internal policy/lifecycle boundary, not a wire protocol.
 */
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ares::session {
  /** @brief Observable lifecycle state. */
  enum class session_display_state_e { prepared, pending, streaming, app_retained, restoring, released, unresolved };
  /** @brief Operation outcome. */
  enum class coordinator_status_e { ok, unauthorized, invalid_resolution, invalid_state, allocation_failed, timeout, probe_failed, app_start_failed, unresolved_resource, session_not_found };
  /** @brief Identity supplied by a future authenticated registry adapter. */
  struct paired_principal_t { std::string client_id, friendly_name, public_key_id; };
  /** @brief Parsed signed integers; validate before conversion to driver types. */
  struct display_mode_spec_t { int64_t width{1920}, height{1080}, refresh_rate{60}; bool hdr_capable{false}; };
  /** @brief Internal action, deliberately not an Apollo bit mask. */
  enum class action_e { launch, view };
  /** @brief Internal policy decision; no cryptographic verification claim. */
  struct policy_decision_t { bool action_allowed{false}, display_allowed{false}, mode_allowed{false}, hdr_allowed{false}; };
  /** @brief Explicit internal policy adapter. Production mapping remains unwired. */
  struct itrusted_registry_t {
    virtual ~itrusted_registry_t() = default;
    virtual policy_decision_t authorize(const paired_principal_t &, action_e) = 0;
  };
  /** @brief Real asynchronous publication acknowledgement. */
  enum class publication_e { pending, ready, failed };
  /** @brief Provider owns any allocation that throws before returning its handle. */
  struct idisplay_provider_t {
    virtual ~idisplay_provider_t() = default;
    virtual std::optional<std::wstring> allocate_display(const display_mode_spec_t &) = 0;
    virtual publication_e publication(const std::wstring &) = 0;
    virtual bool probe_display(const std::wstring &) = 0;
    virtual bool restore_display(const std::wstring &) = 0;
    virtual bool release_display(const std::wstring &) = 0;
    virtual void on_unresolved_resource(const std::wstring &) = 0;
  };
  /** @brief Stop/join must quiesce even a partially failed/throwing start. */
  struct iapp_lifecycle_t {
    virtual ~iapp_lifecycle_t() = default;
    virtual bool start_app(const std::string &, const std::wstring &) = 0;
    virtual bool stop_and_join_app(const std::string &) = 0;
  };
  /** @brief Monotonic clock. */
  struct iclock_t {
    virtual ~iclock_t() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
  };
  /** @brief Copy-only query snapshot, never cleanup ownership. */
  struct session_display_record_t {
    std::string session_id;
    display_mode_spec_t mode;
    session_display_state_e state{session_display_state_e::prepared};
    std::wstring allocated_resource;
    uint64_t generation{0};
  };
  /** @brief Durable cleanup owner. Caller retains this beyond coordinator lifetime.
   * Entries are registered BEFORE external work. Retry exclusively claims cleanup;
   * app quiescence precedes topology ack, which precedes release. Callbacks may query
   * coordinator snapshots, but MUST NOT reenter lifecycle mutations or ledger retry.
   */
  class recovery_ledger_t {
  public:
    /** @brief Retry retained obligations; return remaining count. */
    size_t retry();
    /** @brief Return number of retained obligations. */
    size_t pending() const;
  private:
    friend class session_display_coordinator_t;
    struct obligation_t {
      std::string session_id;
      std::wstring resource;
      std::shared_ptr<idisplay_provider_t> display;
      std::shared_ptr<iapp_lifecycle_t> app;
      bool app_live{false}, topology{false}, allocation{false}, active{true};
    };
    static bool cleanup(obligation_t &) noexcept;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<obligation_t>> entries_;
  };
  /** @brief Serialized synchronous lifecycle with separately locked snapshots.
   * All dependencies must be bounded synchronous calls. Mutating callback reentry
   * is forbidden; queries are safe. Owner must quiesce callers before destruction.
   */
  class session_display_coordinator_t {
  public:
    session_display_coordinator_t(std::shared_ptr<itrusted_registry_t>, std::shared_ptr<idisplay_provider_t>, std::shared_ptr<iapp_lifecycle_t>, std::shared_ptr<iclock_t>, std::shared_ptr<recovery_ledger_t>, std::chrono::milliseconds = std::chrono::seconds(10));
    ~session_display_coordinator_t() noexcept;
    coordinator_status_e prepare_session(const std::string &, const paired_principal_t &, const display_mode_spec_t &, action_e = action_e::launch);
    coordinator_status_e start_session(const std::string &, uint64_t);
    coordinator_status_e poll_session(const std::string &, uint64_t);
    coordinator_status_e disconnect_session(const std::string &, uint64_t, bool);
    coordinator_status_e app_exit(const std::string &, uint64_t);
    coordinator_status_e stop_session(const std::string &, uint64_t);
    coordinator_status_e close();
    std::optional<session_display_record_t> get_session(const std::string &) const;
  private:
    struct entry_t {
      session_display_record_t record;
      std::shared_ptr<recovery_ledger_t::obligation_t> obligation;
      std::chrono::steady_clock::time_point started;
      action_e action;
    };
    coordinator_status_e finish(entry_t &);
    coordinator_status_e cleanup(entry_t &);
    void publish(const entry_t &);
    entry_t *find(const std::string &, uint64_t);
    std::shared_ptr<itrusted_registry_t> registry_;
    std::shared_ptr<idisplay_provider_t> display_;
    std::shared_ptr<iapp_lifecycle_t> app_;
    std::shared_ptr<iclock_t> clock_;
    std::shared_ptr<recovery_ledger_t> ledger_;
    std::chrono::milliseconds timeout_;
    std::mutex operations_;
    mutable std::mutex snapshots_mutex_;
    std::unordered_map<std::string, entry_t> entries_;
    std::unordered_map<std::string, session_display_record_t> snapshots_;
  };
}
