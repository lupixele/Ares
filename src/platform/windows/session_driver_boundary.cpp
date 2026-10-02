/** @file session_driver_boundary.cpp
 * @brief Portable lifecycle implementation; no native driver or nvhttp wiring.
 */
#include "src/platform/windows/session_driver_boundary.h"
#include <limits>
#include <type_traits>

namespace platf::windows {
  namespace {
    static_assert(std::is_nothrow_move_assignable_v<std::wstring>);
    static_assert(std::is_nothrow_move_constructible_v<std::pair<driver_boundary_status_e, std::optional<session_display_allocation_t>>>);
    /** @brief Contain backend exceptions, retaining ownership on ambiguous failure. */
    bool release_safely(idriver_backend_t &backend, const std::wstring &name) noexcept {
      try { return backend.release_display(name); }
      catch (...) { return false; }
    }

    /** @brief Immediate guard backed by ownership storage reserved before external I/O. */
    struct allocation_guard_t {
      idriver_backend_t &backend; ///< Backend outlives this guard.
      std::wstring &name; ///< No-throw moved identifier in preallocated map record.
      bool committed {false}; ///< Public snapshot successfully constructed.
      ~allocation_guard_t() noexcept {
        if (!committed && release_safely(backend, name)) { name.clear(); }
      }
    };
  }

  session_driver_boundary_t::session_driver_boundary_t(
    std::shared_ptr<isession_authorizer_t> authorizer,
    std::shared_ptr<idriver_backend_t> backend,
    uint32_t max_consecutive_heartbeat_fails
  ) : _authorizer(std::move(authorizer)), _backend(std::move(backend)),
      _max_consecutive_heartbeat_fails(max_consecutive_heartbeat_fails) {}

  session_driver_boundary_t::~session_driver_boundary_t() noexcept {
    close();
    for (const auto &entry : _active_sessions) {
      _backend->unresolved_cleanup(entry.second.display_name);
    }
  }

  std::pair<driver_boundary_status_e, std::optional<session_display_allocation_t>>
  session_driver_boundary_t::acquire_session_display(const std::string &session_id, const session_display_spec_t &spec) {
    if (session_id.empty() || spec.client_uid.empty()) { return {driver_boundary_status_e::unauthorized, {}}; }
    if (!spec.width || !spec.height || !spec.fps) { return {driver_boundary_status_e::invalid_dimensions, {}}; }
    std::lock_guard<std::mutex> operation(_operations);
    // Authorization precedes duplicate lookup; denied requests never mutate ownership.
    if (!_authorizer || !_authorizer->authorize_session(session_id, spec)) {
      return {driver_boundary_status_e::unauthorized, {}};
    }
    if (_closed) { return {driver_boundary_status_e::closed, {}}; }
    if (_active_sessions.count(session_id)) { return {driver_boundary_status_e::duplicate_session, {}}; }
    if (!_backend || !_backend->is_driver_ready()) { return {driver_boundary_status_e::driver_not_ready, {}}; }
    // Map/key/spec allocations precede external resource acquisition.
    auto it = _active_sessions.emplace(session_id, session_display_allocation_t {session_id, {}, spec, {}}).first;
    try {
      auto name = _backend->allocate_display(spec);
      if (!name || name->empty()) {
        _active_sessions.erase(it);
        return {driver_boundary_status_e::allocation_failed, {}};
      }
      it->second.display_name = std::move(*name); // Standard allocator string move is noexcept.
      allocation_guard_t guard {*_backend, it->second.display_name};
      it->second.created_at = std::chrono::steady_clock::now();
      std::pair<driver_boundary_status_e, std::optional<session_display_allocation_t>> result {
        driver_boundary_status_e::ok, it->second
      };
      guard.committed = true;
      return result;
    }
    catch (...) {
      if (it->second.display_name.empty()) { _active_sessions.erase(it); }
      throw;
    }
  }

  bool session_driver_boundary_t::release_session_display(const std::string &session_id) {
    std::lock_guard<std::mutex> operation(_operations);
    auto it = _active_sessions.find(session_id);
    if (it == _active_sessions.end()) { return false; }
    if (!release_safely(*_backend, it->second.display_name)) { return false; }
    _active_sessions.erase(it);
    return true;
  }

  session_driver_boundary_t::close_report_t session_driver_boundary_t::close() noexcept {
    std::lock_guard<std::mutex> operation(_operations);
    _closed = true;
    close_report_t report;
    for (auto it = _active_sessions.begin(); it != _active_sessions.end();) {
      ++report.attempted;
      if (release_safely(*_backend, it->second.display_name)) { it = _active_sessions.erase(it); }
      else { ++report.unresolved; ++it; }
    }
    return report;
  }

  void session_driver_boundary_t::rollback_all() noexcept { close(); }

  bool session_driver_boundary_t::tick_heartbeat() {
    // Separate lane: slow lifecycle I/O cannot block heartbeat scheduling.
    std::lock_guard<std::mutex> heartbeat(_mutex);
    bool ok = false;
    try { ok = _backend && _backend->is_driver_ready() && _backend->ping_watchdog(); }
    catch (...) {}
    if (ok) { _consecutive_heartbeat_fails = 0; }
    else if (_consecutive_heartbeat_fails != std::numeric_limits<uint32_t>::max()) { ++_consecutive_heartbeat_fails; }
    return ok || (_backend && _consecutive_heartbeat_fails <= _max_consecutive_heartbeat_fails);
  }

  uint32_t session_driver_boundary_t::get_consecutive_heartbeat_failures() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _consecutive_heartbeat_fails;
  }

  size_t session_driver_boundary_t::get_active_session_count() const {
    std::lock_guard<std::mutex> operation(_operations);
    return _active_sessions.size();
  }

  bool session_driver_boundary_t::has_session(const std::string &session_id) const {
    std::lock_guard<std::mutex> operation(_operations);
    return _active_sessions.count(session_id) != 0;
  }
}
