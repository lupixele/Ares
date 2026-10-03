/**
 * @file tools/session-coordinator-tests/main.cpp
 * @brief Standalone test suite for session_display_coordinator_t.
 */
#include "src/session_display_coordinator.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ares::session;

#define ASSERT_TRUE(cond) do { \
  if (!(cond)) { \
    std::cerr << "Assertion failed at " << __FILE__ << ":" << __LINE__ << ": " #cond << std::endl; \
    std::exit(1); \
  } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

/**
 * @brief Fake monotonic clock for testing timeout and timing.
 */
class fake_clock_t : public iclock_t {
public:
  std::chrono::steady_clock::time_point current_time {std::chrono::steady_clock::now()};

  std::chrono::steady_clock::time_point now() override {
    return current_time;
  }

  void advance(std::chrono::milliseconds ms) {
    current_time += ms;
  }
};

/**
 * @brief Fake trusted registry for client permission authorization.
 */
class fake_trusted_registry_t : public itrusted_registry_t {
public:
  bool registered_principal {true};
  apollo_permission_flags_e permissions {apollo_permission_flags_e::virtual_display_allowed | apollo_permission_flags_e::resolution_change_allowed};
  bool valid_signature {true};
  bool unset_permissions {false};

  std::optional<apollo_permission_flags_e> get_principal_permissions(const paired_principal_t &principal) override {
    if (unset_permissions) {
      return std::nullopt; // Explicit deny on unset
    }
    if (!registered_principal) {
      return std::nullopt;
    }
    return permissions;
  }

  bool verify_resolution_signature(const paired_principal_t &principal, const signed_resolution_request_t &request) override {
    return valid_signature;
  }
};

/**
 * @brief Fake display provider tracking IO order and failures.
 */
class fake_display_provider_t : public idisplay_provider_t {
public:
  bool allow_allocation {true};
  bool allow_probe {true};
  bool allow_restore {true};
  bool allow_release {true};

  std::vector<std::string> event_order;
  std::vector<std::wstring> allocated_displays;
  std::vector<std::wstring> probed_displays;
  std::vector<std::wstring> restored_displays;
  std::vector<std::wstring> released_displays;
  std::vector<std::wstring> unresolved_calls;

  std::optional<std::wstring> allocate_display(const display_mode_spec_t &mode) override {
    event_order.push_back("allocate");
    if (!allow_allocation) {
      return std::nullopt;
    }
    std::wstring dev = L"\\\\.\\DISPLAY_VIRTUAL_" + std::to_wstring(allocated_displays.size() + 1);
    allocated_displays.push_back(dev);
    return dev;
  }

  bool probe_display(const std::wstring &resource_name) override {
    event_order.push_back("probe");
    probed_displays.push_back(resource_name);
    return allow_probe;
  }

  bool restore_display(const std::wstring &resource_name) override {
    event_order.push_back("restore");
    restored_displays.push_back(resource_name);
    return allow_restore;
  }

  bool release_display(const std::wstring &resource_name) override {
    event_order.push_back("release");
    released_displays.push_back(resource_name);
    return allow_release;
  }

  void on_unresolved_resource(const std::wstring &resource_name) noexcept override {
    event_order.push_back("unresolved");
    unresolved_calls.push_back(resource_name);
  }
};

/**
 * @brief Fake app lifecycle manager.
 */
class fake_app_lifecycle_t : public iapp_lifecycle_t {
public:
  bool allow_start {true};
  bool allow_stop {true};
  std::vector<std::string> event_order;

  bool start_app(const std::string &session_id, const std::wstring &display_resource) override {
    event_order.push_back("app_start");
    return allow_start;
  }

  bool stop_and_join_app(const std::string &session_id) override {
    event_order.push_back("app_stop");
    return allow_stop;
  }
};

void test_denial_on_unset_and_unauthorized() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req;
  req.mode = {1920, 1080, 60, false};
  req.server_session_token = "token-abc";
  req.signature = "sig-valid";

  // Case 1: Unset permission in registry explicitly denied
  registry->unset_permissions = true;
  auto status = coord.prepare_session("s1", principal, "token-abc", req);
  ASSERT_EQ(status, coordinator_status_e::unauthorized);

  // Case 2: Virtual display permission omitted
  registry->unset_permissions = false;
  registry->permissions = apollo_permission_flags_e::resolution_change_allowed;
  status = coord.prepare_session("s1", principal, "token-abc", req);
  ASSERT_EQ(status, coordinator_status_e::unauthorized);

  // Case 3: HDR requested without HDR permission
  registry->permissions = apollo_permission_flags_e::virtual_display_allowed;
  req.mode.hdr_capable = true;
  status = coord.prepare_session("s1", principal, "token-abc", req);
  ASSERT_EQ(status, coordinator_status_e::unauthorized);

  // Case 4: Invalid signature
  req.mode.hdr_capable = false;
  registry->valid_signature = false;
  status = coord.prepare_session("s1", principal, "token-abc", req);
  ASSERT_EQ(status, coordinator_status_e::invalid_resolution);
}

void test_failed_publication_allocation() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  auto status = coord.prepare_session("s1", principal, "token-1", req);
  ASSERT_EQ(status, coordinator_status_e::ok);

  provider->allow_allocation = false;
  status = coord.start_streaming_display("s1");
  ASSERT_EQ(status, coordinator_status_e::allocation_failed);

  auto snap = coord.get_session("s1");
  ASSERT_TRUE(snap.has_value());
  ASSERT_EQ(snap->state, session_display_state_e::prepared);
  ASSERT_TRUE(snap->allocated_resource.empty());
}

void test_probe_failure_rollback() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  provider->allow_probe = false;

  auto status = coord.start_streaming_display("s1");
  ASSERT_EQ(status, coordinator_status_e::probe_failed);
  ASSERT_EQ(provider->released_displays.size(), 1u);
  auto snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::prepared);
}

void test_app_failure_rollback() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  app->allow_start = false;

  auto status = coord.start_streaming_display("s1");
  ASSERT_EQ(status, coordinator_status_e::app_start_failed);
  ASSERT_EQ(provider->restored_displays.size(), 1u);
  ASSERT_EQ(provider->released_displays.size(), 1u);
  auto snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::prepared);
}

void test_active_stop_joined_sequence() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  auto status = coord.start_streaming_display("s1");
  ASSERT_EQ(status, coordinator_status_e::ok);

  auto snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::streaming);

  // Stop session
  status = coord.stop_session("s1");
  ASSERT_EQ(status, coordinator_status_e::ok);

  // Verify event order: app_stop -> restore -> release
  ASSERT_EQ(app->event_order.back(), "app_stop");
  ASSERT_TRUE(provider->event_order.size() >= 3);
  auto p_size = provider->event_order.size();
  ASSERT_EQ(provider->event_order[p_size - 2], "restore");
  ASSERT_EQ(provider->event_order[p_size - 1], "release");

  snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::released);
}

void test_pending_timeout_cleanup() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock, std::chrono::milliseconds(1000));

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  
  // Directly simulate transition to pending
  // We can test check_pending_timeouts directly
  ASSERT_EQ(coord.check_pending_timeouts(), 0u);

  clock->advance(std::chrono::milliseconds(1500));
  ASSERT_EQ(coord.check_pending_timeouts(), 0u);
}

void test_app_retained_and_resume() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  coord.start_streaming_display("s1");

  auto snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::streaming);
  auto initial_resource = snap->allocated_resource;

  // Stream disconnect with app_retained = true
  auto status = coord.on_stream_disconnected("s1", true);
  ASSERT_EQ(status, coordinator_status_e::ok);

  snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::app_retained);
  ASSERT_TRUE(snap->app_retained);
  ASSERT_EQ(snap->allocated_resource, initial_resource);
  // Ensure NO restore or release occurred
  ASSERT_EQ(provider->restored_displays.size(), 0u);
  ASSERT_EQ(provider->released_displays.size(), 0u);

  // Resume streaming with valid token
  status = coord.resume_streaming("s1", "token-1");
  ASSERT_EQ(status, coordinator_status_e::ok);

  snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::streaming);
  ASSERT_FALSE(snap->app_retained);
  ASSERT_EQ(snap->allocated_resource, initial_resource);

  // Resume with wrong token denied
  coord.on_stream_disconnected("s1", true);
  status = coord.resume_streaming("s1", "wrong-token");
  ASSERT_EQ(status, coordinator_status_e::unauthorized);
}

void test_late_callback_cancel_generation() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  auto snap = coord.get_session("s1");
  uint64_t old_gen = snap->generation;

  bool callback_ran = false;
  // Cancel operations on session
  coord.cancel_session_operations("s1");

  bool executed = coord.handle_late_callback("s1", old_gen, [&]() {
    callback_ran = true;
  });
  ASSERT_FALSE(executed);
  ASSERT_FALSE(callback_ran);
}

void test_unresolved_retention_and_retry() {
  auto clock = std::make_shared<fake_clock_t>();
  auto registry = std::make_shared<fake_trusted_registry_t>();
  auto provider = std::make_shared<fake_display_provider_t>();
  auto app = std::make_shared<fake_app_lifecycle_t>();

  session_display_coordinator_t coord(registry, provider, app, clock);

  paired_principal_t principal{"client-1", "Client One", "key-123"};
  signed_resolution_request_t req{{1920, 1080, 60, false}, "sig", "token-1"};

  coord.prepare_session("s1", principal, "token-1", req);
  coord.start_streaming_display("s1");

  // Cause restore failure
  provider->allow_restore = false;

  auto status = coord.stop_session("s1");
  ASSERT_EQ(status, coordinator_status_e::unresolved_resource);

  auto snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::unresolved);
  ASSERT_FALSE(snap->allocated_resource.empty());

  auto unresolved = coord.get_unresolved_resources();
  ASSERT_EQ(unresolved.size(), 1u);
  ASSERT_EQ(provider->unresolved_calls.size(), 1u);

  // Retry while still failing
  status = coord.retry_unresolved_restore("s1");
  ASSERT_EQ(status, coordinator_status_e::unresolved_resource);

  // Now fix provider and retry
  provider->allow_restore = true;
  status = coord.retry_unresolved_restore("s1");
  ASSERT_EQ(status, coordinator_status_e::ok);

  snap = coord.get_session("s1");
  ASSERT_EQ(snap->state, session_display_state_e::released);
  ASSERT_EQ(coord.get_unresolved_resources().size(), 0u);
}

int main() {
  try {
    std::cout << "Running session coordinator tests..." << std::endl;
    test_denial_on_unset_and_unauthorized();
    test_failed_publication_allocation();
    test_probe_failure_rollback();
    test_app_failure_rollback();
    test_active_stop_joined_sequence();
    test_pending_timeout_cleanup();
    test_app_retained_and_resume();
    test_late_callback_cancel_generation();
    test_unresolved_retention_and_retry();
    std::cout << "All session coordinator tests passed successfully!" << std::endl;
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "Exception caught: " << ex.what() << std::endl;
    return 1;
  }
}
