/**
 * @file src/platform/windows/virtual_display_manager.cpp
 * @brief Implementation of the session-scoped virtual display lifecycle manager.
 */

// located in src/platform/windows

// standard includes
#include <algorithm>
#include <string_view>

// local includes
#include "src/logging.h"
#include "src/platform/windows/virtual_display_manager.h"

namespace platf::windows {
  using namespace std::literals;

  virtual_display_manager_t::virtual_display_manager_t(std::shared_ptr<session_driver_boundary_t> boundary,
                                                       std::shared_ptr<isession_policy_t> policy) :
      _boundary(std::move(boundary)),
      _policy(policy ? std::move(policy) : std::make_shared<allow_all_policy_t>()) {}

  virtual_display_result_e virtual_display_manager_t::acquire(const std::string &session_id,
                                                             int client_width,
                                                             int client_height,
                                                             int client_fps,
                                                             int scale_factor) {
    if (session_id.empty()) {
      return virtual_display_result_e::rejected;
    }
    if (!_policy || !_policy->allows_virtual_display()) {
      return virtual_display_result_e::rejected;
    }

    {
      std::lock_guard lock(_mutex);
      // A session that already owns a display keeps it. Recreating would leave the old display
      // behind on the desktop, which is exactly the failure mode we cannot recover from.
      if (_sessions.count(session_id) != 0) {
        return virtual_display_result_e::already_present;
      }
    }

    // Resolution policy runs first: it needs no driver and must not be skipped on a bad client.
    const auto geometry = resolve_virtual_display_geometry(client_width, client_height, client_fps, scale_factor);

    if (!_boundary) {
      // No boundary configured: nothing to allocate against, but the session is not broken.
      return virtual_display_result_e::unavailable;
    }

    session_display_spec_t spec;
    spec.client_uid = session_id;
    spec.client_name = session_id;
    spec.width = geometry.width;
    spec.height = geometry.height;
    spec.fps = geometry.fps;

    const auto [status, allocation] = _boundary->acquire_session_display(session_id, spec);
    if (status != driver_boundary_status_e::ok || !allocation.has_value()) {
      BOOST_LOG(warning) << "Virtual display unavailable for session ["sv << session_id << "]; continuing without one"sv;
      return virtual_display_result_e::unavailable;
    }

    virtual_display_session_t record;
    record.session_id = session_id;
    record.display_name = allocation->display_name;
    record.geometry = geometry;

    {
      std::lock_guard lock(_mutex);
      _sessions[session_id] = std::move(record);
    }

    BOOST_LOG(info) << "Virtual display ["sv << allocation->display_name << "] allocated for session ["
                    << session_id << "] at "sv << geometry.width << 'x' << geometry.height << '@'
                    << geometry.fps;
    return virtual_display_result_e::created;
  }

  virtual_display_manager_t::teardown_report_t virtual_display_manager_t::release(const std::string &session_id) {
    teardown_report_t report;
    report.session_id = session_id;

    std::optional<std::wstring> display_name;
    {
      std::lock_guard lock(_mutex);
      const auto it = _sessions.find(session_id);
      if (it == _sessions.end()) {
        // Not held here. This is normal and must not be an error: callers release on session end
        // without knowing whether a display was ever allocated.
        report.released = false;
        return report;
      }
      display_name = it->second.display_name;
    }

    bool released = false;
    if (_boundary && display_name.has_value()) {
      try {
        released = _boundary->release_session_display(session_id);
      } catch (...) {
        // Never let teardown escape. The boundary retains ownership of a failed release itself.
        released = false;
      }
    }

    if (released) {
      std::lock_guard lock(_mutex);
      _sessions.erase(session_id);
    }

    report.released = released;
    if (released) {
      BOOST_LOG(info) << "Virtual display released for session ["sv << session_id << ']';
    } else if (display_name.has_value()) {
      // Keep the record so the obligation is not lost; the boundary already retained the
      // underlying display, and this record lets shutdown retry it.
      report.retained = display_name;
      BOOST_LOG(warning) << "Virtual display ["sv << *display_name << "] for session ["sv << session_id
                         << "] could not be confirmed released; retained for retry"sv;
    }

    return report;
  }

  std::vector<virtual_display_manager_t::teardown_report_t> virtual_display_manager_t::release_all() {
    std::vector<std::string> sessions;
    {
      std::lock_guard lock(_mutex);
      sessions.reserve(_sessions.size());
      for (const auto &entry : _sessions) {
        sessions.push_back(entry.first);
      }
    }

    std::vector<teardown_report_t> reports;
    reports.reserve(sessions.size());
    for (const auto &session : sessions) {
      reports.push_back(release(session));
    }
    return reports;
  }

  bool virtual_display_manager_t::has_display(const std::string &session_id) const {
    std::lock_guard lock(_mutex);
    return _sessions.count(session_id) != 0;
  }

  std::vector<virtual_display_session_t> virtual_display_manager_t::active_sessions() const {
    std::lock_guard lock(_mutex);
    std::vector<virtual_display_session_t> out;
    out.reserve(_sessions.size());
    for (const auto &entry : _sessions) {
      out.push_back(entry.second);
    }
    return out;
  }
}  // namespace platf::windows
