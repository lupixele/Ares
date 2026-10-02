/**
 * @file src/platform/windows/session_driver_boundary.cpp
 * @brief Implementation of narrow C++ session and virtual display driver boundary.
 */
#include "src/platform/windows/session_driver_boundary.h"

namespace platf::windows {

  session_driver_boundary_t::session_driver_boundary_t(
    std::shared_ptr<isession_authorizer_t> authorizer,
    std::shared_ptr<idriver_backend_t> backend,
    uint32_t max_consecutive_heartbeat_fails
  ) :
      _authorizer(std::move(authorizer)),
      _backend(std::move(backend)),
      _max_consecutive_heartbeat_fails(max_consecutive_heartbeat_fails) {
  }

  session_driver_boundary_t::~session_driver_boundary_t() {
    rollback_all();
  }

  std::pair<driver_boundary_status_e, std::optional<session_display_allocation_t>>
  session_driver_boundary_t::acquire_session_display(
    const std::string &session_id,
    const session_display_spec_t &spec
  ) {
    if (session_id.empty() || spec.client_uid.empty()) {
      return {driver_boundary_status_e::unauthorized, std::nullopt};
    }

    if (spec.width == 0 || spec.height == 0 || spec.fps == 0) {
      return {driver_boundary_status_e::invalid_dimensions, std::nullopt};
    }

    std::lock_guard<std::mutex> lock(_mutex);

    // If session already holds an active display, rollback the prior display first
    auto it = _active_sessions.find(session_id);
    if (it != _active_sessions.end()) {
      if (_backend) {
        _backend->release_display(it->second.display_name);
      }
      _active_sessions.erase(it);
    }

    // Step 1: Pre-create authorization check
    if (!_authorizer || !_authorizer->authorize_session(session_id, spec)) {
      return {driver_boundary_status_e::unauthorized, std::nullopt};
    }

    // Step 2: Driver readiness check
    if (!_backend || !_backend->is_driver_ready()) {
      return {driver_boundary_status_e::driver_not_ready, std::nullopt};
    }

    // Step 3: Low-level driver allocation
    auto display_name = _backend->allocate_display(spec);
    if (!display_name || display_name->empty()) {
      return {driver_boundary_status_e::allocation_failed, std::nullopt};
    }

    // Step 4: Register session ownership
    session_display_allocation_t allocation {
      session_id,
      *display_name,
      spec,
      std::chrono::steady_clock::now()
    };

    _active_sessions.emplace(session_id, allocation);
    return {driver_boundary_status_e::ok, allocation};
  }

  bool session_driver_boundary_t::release_session_display(const std::string &session_id) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _active_sessions.find(session_id);
    if (it == _active_sessions.end()) {
      return false;
    }

    bool released = true;
    if (_backend) {
      released = _backend->release_display(it->second.display_name);
    }
    _active_sessions.erase(it);
    return released;
  }

  bool session_driver_boundary_t::tick_heartbeat() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_backend) {
      _consecutive_heartbeat_fails++;
      return false;
    }

    bool ping_ok = _backend->ping_watchdog();
    if (ping_ok) {
      // Immediate reset of consecutive failure counter
      _consecutive_heartbeat_fails = 0;
      return true;
    }

    _consecutive_heartbeat_fails++;
    return _consecutive_heartbeat_fails <= _max_consecutive_heartbeat_fails;
  }

  uint32_t session_driver_boundary_t::get_consecutive_heartbeat_failures() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _consecutive_heartbeat_fails;
  }

  size_t session_driver_boundary_t::get_active_session_count() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _active_sessions.size();
  }

  bool session_driver_boundary_t::has_session(const std::string &session_id) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _active_sessions.find(session_id) != _active_sessions.end();
  }

  void session_driver_boundary_t::rollback_all() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_backend) {
      for (const auto &pair : _active_sessions) {
        _backend->release_display(pair.second.display_name);
      }
    }
    _active_sessions.clear();
  }

}  // namespace platf::windows
