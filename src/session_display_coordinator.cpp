/** @file @brief Serialized experimental coordinator implementation. */
#include "src/session_display_coordinator.h"
#include <atomic>
#include <limits>
#include <stdexcept>
#include <algorithm>

namespace ares::session {
  namespace {
    // Process-global, never reset on session reuse or coordinator reconstruction.
    std::atomic<uint64_t> next_generation{0};
    uint64_t token() {
      auto n = next_generation.load();
      do {
        if (n == std::numeric_limits<uint64_t>::max()) {
          throw std::overflow_error("incarnation exhausted");
        }
      } while (!next_generation.compare_exchange_weak(n, n + 1));
      return n + 1;
    }
  }
  bool recovery_ledger_t::cleanup(obligation_t &o) noexcept {
    try {
      if (o.app_live) {
        if (!o.app->stop_and_join_app(o.session_id)) { return false; }
        o.app_live = false;
      }
      if (o.topology) {
        if (!o.display->restore_display(o.resource)) { return false; }
        o.topology = false;
      }
      if (o.allocation) {
        if (!o.display->release_display(o.resource)) { return false; }
        o.allocation = false;
        o.resource.clear();
      }
      return true;
    } catch (...) { return false; }
  }
  size_t recovery_ledger_t::retry() {
    std::lock_guard lock(mutex_);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](auto &o) {
      return !o->active && cleanup(*o);
    }), entries_.end());
    return entries_.size();
  }
  size_t recovery_ledger_t::pending() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }
  session_display_coordinator_t::session_display_coordinator_t(std::shared_ptr<itrusted_registry_t> r, std::shared_ptr<idisplay_provider_t> d, std::shared_ptr<iapp_lifecycle_t> a, std::shared_ptr<iclock_t> c, std::shared_ptr<recovery_ledger_t> l, std::chrono::milliseconds t) : registry_(std::move(r)), display_(std::move(d)), app_(std::move(a)), clock_(std::move(c)), ledger_(std::move(l)), timeout_(t) {
    if (!registry_ || !display_ || !app_ || !clock_ || !ledger_ || t.count() <= 0) { throw std::invalid_argument("coordinator dependencies/timeout"); }
  }
  session_display_coordinator_t::~session_display_coordinator_t() noexcept {
    try { close(); } catch (...) {}
    // No allocation in handoff: ledger already owns every obligation and dependency.
    std::lock_guard lock(ledger_->mutex_);
    for (auto &[id, e] : entries_) { (void)id; e.obligation->active = false; }
  }
  void session_display_coordinator_t::publish(const entry_t &e) {
    std::lock_guard lock(snapshots_mutex_);
    snapshots_.insert_or_assign(e.record.session_id, e.record);
  }
  std::optional<session_display_record_t> session_display_coordinator_t::get_session(const std::string &id) const {
    std::lock_guard lock(snapshots_mutex_);
    auto it = snapshots_.find(id);
    if (it == snapshots_.end()) { return std::nullopt; }
    return it->second;
  }
  session_display_coordinator_t::entry_t *session_display_coordinator_t::find(const std::string &id, uint64_t generation) {
    auto it = entries_.find(id);
    return it != entries_.end() && it->second.record.generation == generation ? &it->second : nullptr;
  }
  coordinator_status_e session_display_coordinator_t::prepare_session(const std::string &id, const paired_principal_t &p, const display_mode_spec_t &m, action_e action) {
    std::lock_guard lock(operations_);
    if (m.width < 640 || m.width > 7680 || m.height < 480 || m.height > 4320 || m.refresh_rate < 24 || m.refresh_rate > 240) { return coordinator_status_e::invalid_resolution; }
    if (id.empty() || p.client_id.empty()) { return coordinator_status_e::unauthorized; }
    try {
      auto policy = registry_->authorize(p, action);
      if (!policy.action_allowed || !policy.display_allowed || !policy.mode_allowed || (m.hdr_capable && !policy.hdr_allowed)) { return coordinator_status_e::unauthorized; }
    } catch (...) { return coordinator_status_e::unauthorized; }
    auto old = entries_.find(id);
    if (old != entries_.end() && old->second.record.state != session_display_state_e::released) { return coordinator_status_e::invalid_state; }
    auto o = std::make_shared<recovery_ledger_t::obligation_t>();
    o->session_id = id; o->display = display_; o->app = app_;
    entry_t e{{id, m, session_display_state_e::prepared, {}, token()}, o, {}, action};
    {
      std::lock_guard ledger_lock(ledger_->mutex_);
      ledger_->entries_.push_back(o);
    }
    try { entries_.insert_or_assign(id, std::move(e)); }
    catch (...) {
      std::lock_guard ledger_lock(ledger_->mutex_);
      ledger_->entries_.erase(std::remove(ledger_->entries_.begin(), ledger_->entries_.end(), o), ledger_->entries_.end());
      throw;
    }
    publish(entries_.at(id));
    return coordinator_status_e::ok;
  }
  coordinator_status_e session_display_coordinator_t::cleanup(entry_t &e) {
    e.record.state = session_display_state_e::restoring;
    const bool done = recovery_ledger_t::cleanup(*e.obligation);
    e.record.state = done ? session_display_state_e::released : session_display_state_e::unresolved;
    if (done) {
      e.record.allocated_resource.clear();
      std::lock_guard ledger_lock(ledger_->mutex_);
      ledger_->entries_.erase(std::remove(ledger_->entries_.begin(), ledger_->entries_.end(), e.obligation), ledger_->entries_.end());
    }
    else { try { display_->on_unresolved_resource(e.obligation->resource); } catch (...) {} }
    publish(e);
    return done ? coordinator_status_e::ok : coordinator_status_e::unresolved_resource;
  }
  coordinator_status_e session_display_coordinator_t::start_session(const std::string &id, uint64_t generation) {
    std::lock_guard lock(operations_);
    auto e = find(id, generation);
    if (!e || e->record.state != session_display_state_e::prepared) { return coordinator_status_e::invalid_state; }
    try {
      e->started = clock_->now();
      auto allocation = display_->allocate_display(e->record.mode);
      if (!allocation || allocation->empty()) { cleanup(*e); return coordinator_status_e::allocation_failed; }
      // noexcept move into pre-registered ownership BEFORE any fallible snapshot copy.
      e->obligation->resource = std::move(*allocation);
      e->obligation->allocation = true; e->obligation->topology = true;
      e->record.allocated_resource = e->obligation->resource;
      e->record.state = session_display_state_e::pending;
      publish(*e);
      return finish(*e);
    } catch (...) { cleanup(*e); return coordinator_status_e::allocation_failed; }
  }
  coordinator_status_e session_display_coordinator_t::finish(entry_t &e) {
    coordinator_status_e failure = coordinator_status_e::probe_failed;
    try {
      if (clock_->now() - e.started >= timeout_) { failure = coordinator_status_e::timeout; }
      else {
        auto status = display_->publication(e.obligation->resource);
        if (status == publication_e::pending) { return coordinator_status_e::ok; }
        if (status == publication_e::ready && display_->probe_display(e.obligation->resource)) {
          failure = coordinator_status_e::app_start_failed;
          if (e.action == action_e::launch) {
            e.obligation->app_live = true; // even a throwing start may have created a process
            if (!app_->start_app(e.record.session_id, e.obligation->resource)) { cleanup(e); return failure; }
          }
          e.record.state = session_display_state_e::streaming; publish(e); return coordinator_status_e::ok;
        }
      }
    } catch (...) {}
    cleanup(e); return failure;
  }
  coordinator_status_e session_display_coordinator_t::poll_session(const std::string &id, uint64_t generation) {
    std::lock_guard lock(operations_);
    auto e = find(id, generation);
    return e && e->record.state == session_display_state_e::pending ? finish(*e) : coordinator_status_e::invalid_state;
  }
  coordinator_status_e session_display_coordinator_t::stop_session(const std::string &id, uint64_t generation) {
    std::lock_guard lock(operations_);
    auto e = find(id, generation);
    return e ? cleanup(*e) : coordinator_status_e::invalid_state;
  }
  coordinator_status_e session_display_coordinator_t::app_exit(const std::string &id, uint64_t generation) { return stop_session(id, generation); }
  coordinator_status_e session_display_coordinator_t::disconnect_session(const std::string &id, uint64_t generation, bool retain) {
    std::lock_guard lock(operations_);
    auto e = find(id, generation);
    if (!e) { return coordinator_status_e::invalid_state; }
    if (retain && e->record.state == session_display_state_e::streaming && e->obligation->app_live) { e->record.state = session_display_state_e::app_retained; publish(*e); return coordinator_status_e::ok; }
    return cleanup(*e);
  }
  coordinator_status_e session_display_coordinator_t::close() {
    std::lock_guard lock(operations_);
    auto result = coordinator_status_e::ok;
    for (auto &[id, e] : entries_) {
      (void)id;
      try { if (cleanup(e) != coordinator_status_e::ok) { result = coordinator_status_e::unresolved_resource; } }
      catch (...) { result = coordinator_status_e::unresolved_resource; }
    }
    return result;
  }
}
