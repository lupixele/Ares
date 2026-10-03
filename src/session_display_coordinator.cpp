/**
 * @file src/session_display_coordinator.cpp
 * @brief Implementation of session display coordinator.
 */
#include "src/session_display_coordinator.h"

namespace ares::session {

  session_display_coordinator_t::session_display_coordinator_t(
    std::shared_ptr<itrusted_registry_t> registry,
    std::shared_ptr<idisplay_provider_t> display_provider,
    std::shared_ptr<iapp_lifecycle_t> app_lifecycle,
    std::shared_ptr<iclock_t> clock,
    std::chrono::milliseconds pending_timeout
  ) :
      registry_(std::move(registry)),
      display_provider_(std::move(display_provider)),
      app_lifecycle_(std::move(app_lifecycle)),
      clock_(std::move(clock)),
      pending_timeout_(pending_timeout) {}

  session_display_coordinator_t::~session_display_coordinator_t() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pair : sessions_) {
      auto &rec = pair.second.record;
      if (!rec.allocated_resource.empty()) {
        bool restored = false;
        try {
          if (display_provider_) {
            restored = display_provider_->restore_display(rec.allocated_resource);
          }
        } catch (...) {
          restored = false;
        }

        bool released = false;
        try {
          if (display_provider_) {
            released = display_provider_->release_display(rec.allocated_resource);
          }
        } catch (...) {
          released = false;
        }

        if (!restored || !released) {
          unresolved_resources_.push_back(rec.allocated_resource);
          if (display_provider_) {
            display_provider_->on_unresolved_resource(rec.allocated_resource);
          }
        }
        rec.allocated_resource.clear();
      }
    }
    sessions_.clear();
  }

  coordinator_status_e session_display_coordinator_t::prepare_session(
    const std::string &session_id,
    const paired_principal_t &principal,
    const std::string &server_session_token,
    const signed_resolution_request_t &request
  ) {
    if (session_id.empty() || principal.client_id.empty() || server_session_token.empty()) {
      return coordinator_status_e::unauthorized;
    }

    if (!registry_) {
      return coordinator_status_e::unauthorized;
    }

    // Pre-create authorization check: explicit Apollo permissions from trusted registry
    auto permissions_opt = registry_->get_principal_permissions(principal);
    if (!permissions_opt.has_value()) {
      // Explicitly deny if unset/missing
      return coordinator_status_e::unauthorized;
    }

    auto perms = permissions_opt.value();
    if ((perms & apollo_permission_flags_e::virtual_display_allowed) == apollo_permission_flags_e::none) {
      return coordinator_status_e::unauthorized;
    }

    // Verify token matches request
    if (request.server_session_token != server_session_token) {
      return coordinator_status_e::unauthorized;
    }

    // Verify cryptographically signed resolution
    if (!registry_->verify_resolution_signature(principal, request)) {
      return coordinator_status_e::invalid_resolution;
    }

    // Validate resolution bounds
    if (request.mode.width < 640 || request.mode.width > 7680 ||
        request.mode.height < 480 || request.mode.height > 4320 ||
        request.mode.refresh_rate < 30 || request.mode.refresh_rate > 360) {
      return coordinator_status_e::invalid_resolution;
    }

    if (request.mode.hdr_capable && ((perms & apollo_permission_flags_e::hdr_allowed) == apollo_permission_flags_e::none)) {
      return coordinator_status_e::unauthorized;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      // If an existing session is already in active or prepared state, reject duplicate
      if (it->second.record.state != session_display_state_e::released) {
        return coordinator_status_e::invalid_state;
      }
    }

    session_entry_t entry;
    entry.record.session_id = session_id;
    entry.record.principal = principal;
    entry.record.server_session_token = server_session_token;
    entry.record.mode = request.mode;
    entry.record.state = session_display_state_e::prepared;
    entry.record.generation = 1;
    entry.record.app_retained = false;

    sessions_[session_id] = std::move(entry);
    return coordinator_status_e::ok;
  }

  coordinator_status_e session_display_coordinator_t::start_streaming_display(const std::string &session_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return coordinator_status_e::session_not_found;
    }

    auto &entry = it->second;
    if (entry.record.state != session_display_state_e::prepared) {
      return coordinator_status_e::invalid_state;
    }

    entry.record.state = session_display_state_e::pending;
    entry.pending_start = clock_ ? clock_->now() : std::chrono::steady_clock::now();
    uint64_t current_gen = ++entry.record.generation;
    auto mode = entry.record.mode;

    // Unlock during provider IO
    lock.unlock();

    if (!display_provider_) {
      lock.lock();
      entry.record.state = session_display_state_e::prepared;
      return coordinator_status_e::allocation_failed;
    }

    auto allocated = display_provider_->allocate_display(mode);
    if (!allocated.has_value()) {
      lock.lock();
      if (entry.record.generation == current_gen) {
        entry.record.state = session_display_state_e::prepared;
      }
      return coordinator_status_e::allocation_failed;
    }

    std::wstring resource_name = allocated.value();

    // Probe display
    bool probe_ok = display_provider_->probe_display(resource_name);
    if (!probe_ok) {
      // Probe failed: rollback allocation immediately
      bool released = display_provider_->release_display(resource_name);
      lock.lock();
      if (entry.record.generation == current_gen) {
        if (!released) {
          entry.record.state = session_display_state_e::unresolved;
          entry.record.allocated_resource = resource_name;
          unresolved_resources_.push_back(resource_name);
          display_provider_->on_unresolved_resource(resource_name);
        } else {
          entry.record.state = session_display_state_e::prepared;
        }
      }
      return coordinator_status_e::probe_failed;
    }

    // App start
    bool app_ok = true;
    if (app_lifecycle_) {
      app_ok = app_lifecycle_->start_app(session_id, resource_name);
    }

    if (!app_ok) {
      // App start failed: rollback display
      display_provider_->restore_display(resource_name);
      bool released = display_provider_->release_display(resource_name);
      lock.lock();
      if (entry.record.generation == current_gen) {
        if (!released) {
          entry.record.state = session_display_state_e::unresolved;
          entry.record.allocated_resource = resource_name;
          unresolved_resources_.push_back(resource_name);
          display_provider_->on_unresolved_resource(resource_name);
        } else {
          entry.record.state = session_display_state_e::prepared;
        }
      }
      return coordinator_status_e::app_start_failed;
    }

    lock.lock();
    if (entry.record.generation != current_gen) {
      // Cancelled during startup! Rollback
      lock.unlock();
      display_provider_->restore_display(resource_name);
      display_provider_->release_display(resource_name);
      return coordinator_status_e::timeout;
    }

    entry.record.allocated_resource = resource_name;
    entry.record.state = session_display_state_e::streaming;
    return coordinator_status_e::ok;
  }

  size_t session_display_coordinator_t::check_pending_timeouts() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto now = clock_ ? clock_->now() : std::chrono::steady_clock::now();
    size_t timed_out_count = 0;

    for (auto &pair : sessions_) {
      auto &entry = pair.second;
      if (entry.record.state == session_display_state_e::pending) {
        if (now - entry.pending_start >= pending_timeout_) {
          timed_out_count++;
          entry.record.generation++; // Invalidate pending callbacks
          auto res = entry.record.allocated_resource;
          entry.record.allocated_resource.clear();
          entry.record.state = session_display_state_e::prepared;

          if (!res.empty() && display_provider_) {
            lock.unlock();
            display_provider_->restore_display(res);
            bool rel = display_provider_->release_display(res);
            lock.lock();
            if (!rel) {
              entry.record.state = session_display_state_e::unresolved;
              entry.record.allocated_resource = res;
              unresolved_resources_.push_back(res);
              display_provider_->on_unresolved_resource(res);
            }
          }
        }
      }
    }
    return timed_out_count;
  }

  coordinator_status_e session_display_coordinator_t::on_stream_disconnected(const std::string &session_id, bool app_retained) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return coordinator_status_e::session_not_found;
    }

    auto &entry = it->second;
    if (entry.record.state != session_display_state_e::streaming) {
      return coordinator_status_e::invalid_state;
    }

    if (app_retained) {
      entry.record.state = session_display_state_e::app_retained;
      entry.record.app_retained = true;
      // Do not release or restore: keep display allocated
      return coordinator_status_e::ok;
    } else {
      // Normal stop
      mutex_.unlock();
      auto res = stop_session(session_id);
      mutex_.lock();
      return res;
    }
  }

  coordinator_status_e session_display_coordinator_t::resume_streaming(
    const std::string &session_id,
    const std::string &server_session_token
  ) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return coordinator_status_e::session_not_found;
    }

    auto &entry = it->second;
    if (entry.record.server_session_token != server_session_token) {
      return coordinator_status_e::unauthorized;
    }

    if (entry.record.state != session_display_state_e::app_retained) {
      return coordinator_status_e::invalid_state;
    }

    // Reuse existing allocation
    entry.record.state = session_display_state_e::streaming;
    entry.record.app_retained = false;
    return coordinator_status_e::ok;
  }

  coordinator_status_e session_display_coordinator_t::stop_session(const std::string &session_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return coordinator_status_e::session_not_found;
    }

    auto &entry = it->second;
    if (entry.record.state != session_display_state_e::streaming &&
        entry.record.state != session_display_state_e::app_retained &&
        entry.record.state != session_display_state_e::pending) {
      return coordinator_status_e::invalid_state;
    }

    entry.record.state = session_display_state_e::restoring;
    entry.record.generation++;
    std::wstring resource = entry.record.allocated_resource;

    lock.unlock();

    // 1. Stop and join application
    if (app_lifecycle_) {
      app_lifecycle_->stop_and_join_app(session_id);
    }

    bool restore_ok = true;
    bool release_ok = true;

    if (!resource.empty() && display_provider_) {
      // 2. Restore display topology
      restore_ok = display_provider_->restore_display(resource);
      // 3. Release display
      release_ok = display_provider_->release_display(resource);
    }

    lock.lock();
    if (!restore_ok || !release_ok) {
      entry.record.state = session_display_state_e::unresolved;
      unresolved_resources_.push_back(resource);
      if (display_provider_) {
        display_provider_->on_unresolved_resource(resource);
      }
      return coordinator_status_e::unresolved_resource;
    }

    entry.record.allocated_resource.clear();
    entry.record.state = session_display_state_e::released;
    return coordinator_status_e::ok;
  }

  bool session_display_coordinator_t::handle_late_callback(
    const std::string &session_id,
    uint64_t generation,
    std::function<void()> cb
  ) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return false;
    }

    if (it->second.record.generation != generation) {
      // Generation mismatch or cancelled
      return false;
    }

    if (cb) {
      cb();
    }
    return true;
  }

  void session_display_coordinator_t::cancel_session_operations(const std::string &session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
      it->second.record.generation++;
    }
  }

  coordinator_status_e session_display_coordinator_t::retry_unresolved_restore(const std::string &session_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return coordinator_status_e::session_not_found;
    }

    auto &entry = it->second;
    if (entry.record.state != session_display_state_e::unresolved) {
      return coordinator_status_e::invalid_state;
    }

    std::wstring resource = entry.record.allocated_resource;
    lock.unlock();

    bool restore_ok = display_provider_ ? display_provider_->restore_display(resource) : false;
    bool release_ok = display_provider_ ? display_provider_->release_display(resource) : false;

    lock.lock();
    if (restore_ok && release_ok) {
      entry.record.allocated_resource.clear();
      entry.record.state = session_display_state_e::released;
      // Remove from unresolved list
      for (auto iter = unresolved_resources_.begin(); iter != unresolved_resources_.end(); ++iter) {
        if (*iter == resource) {
          unresolved_resources_.erase(iter);
          break;
        }
      }
      return coordinator_status_e::ok;
    }

    return coordinator_status_e::unresolved_resource;
  }

  std::optional<session_display_record_t> session_display_coordinator_t::get_session(const std::string &session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return std::nullopt;
    }
    return it->second.record;
  }

  std::vector<std::wstring> session_display_coordinator_t::get_unresolved_resources() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return unresolved_resources_;
  }

  size_t session_display_coordinator_t::get_session_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
  }

}  // namespace ares::session
