/** @file main.cpp
 * @brief Dependency-free tests of the actual native boundary implementation.
 */
#include "src/platform/windows/session_driver_boundary.h"
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <new>

/** @brief Thread-local, one-shot failure of the next actual heap allocation. */
thread_local bool fail_next_allocation = false;
/** @brief Inject failure into real snapshot/map allocations, not a mirrored model. */
void *operator new(std::size_t size) {
  if (fail_next_allocation) { fail_next_allocation = false; throw std::bad_alloc(); }
  if (void *p = std::malloc(size ? size : 1)) { return p; }
  throw std::bad_alloc();
}
/** @brief Match test allocator. */
void operator delete(void *p) noexcept { std::free(p); }
/** @brief Match sized test allocator. */
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

using namespace platf::windows;
/** @brief Fail even in release builds. */
void check(bool value) { if (!value) { throw std::runtime_error("check failed"); } }
/** @brief Controllable policy. */
struct policy : isession_authorizer_t {
  bool allow = true;
  bool authorize_session(const std::string &, const session_display_spec_t &) override { return allow; }
};
/** @brief Distinct resource backend with deterministic failures. */
struct backend : idriver_backend_t {
  int allocated = 0, attempted = 0, unresolved = 0;
  bool fail_allocate = false, fail_release = false, throw_release = false;
  bool fail_snapshot = false;
  std::function<void()> on_allocate;
  bool is_driver_ready() override { return true; }
  std::optional<std::wstring> allocate_display(const session_display_spec_t &) override {
    if (on_allocate) { on_allocate(); }
    if (fail_allocate) { return {}; }
    auto result = L"display-" + std::to_wstring(++allocated);
    fail_next_allocation = fail_snapshot;
    return std::optional<std::wstring> {std::move(result)};
  }
  bool release_display(const std::wstring &) override {
    ++attempted;
    if (throw_release) { throw std::runtime_error("release"); }
    return !fail_release;
  }
  bool ping_watchdog() override { return true; }
  void unresolved_cleanup(const std::wstring &) noexcept override { ++unresolved; }
};
/** @brief Exercise duplicate, retention, shutdown, and independent heartbeat paths. */
int main() {
  auto b = std::make_shared<backend>();
  auto p = std::make_shared<policy>();
  session_display_spec_t spec;
  spec.client_uid = "client";
  int destructor_attempts = 0;
  {
    session_driver_boundary_t boundary(p, b);
    auto first = boundary.acquire_session_display("a", spec);
    check(first.second.has_value());
    p->allow = false;
    check(boundary.acquire_session_display("a", spec).first == driver_boundary_status_e::unauthorized);
    p->allow = true;
    b->fail_allocate = true;
    check(boundary.acquire_session_display("a", spec).first == driver_boundary_status_e::duplicate_session);
    check(b->allocated == 1 && b->attempted == 0);
    check(!boundary.acquire_session_display("b", spec).second);
    b->fail_allocate = false;
    check(boundary.acquire_session_display("b", spec).second->display_name != first.second->display_name);
    b->throw_release = true;
    check(!boundary.release_session_display("a"));
    check(boundary.get_active_session_count() == 2);
    auto report = boundary.close();
    check(report.attempted == 2 && report.unresolved == 2);
    check(!boundary.acquire_session_display("c", spec).second);
    b->throw_release = false;
    b->fail_release = true;
    check(boundary.close().unresolved == 2);
    b->fail_release = false;
    check(boundary.close().unresolved == 0);
  }
  {
    session_driver_boundary_t boundary(p, b);
    check(boundary.acquire_session_display("a", spec).second.has_value());
    check(boundary.acquire_session_display("b", spec).second.has_value());
    b->throw_release = true;
    destructor_attempts = b->attempted;
    // Destructor must attempt both and transfer unresolved identifiers.
  }
  check(b->unresolved == 2 && b->attempted == destructor_attempts + 2);
  b->throw_release = false;
  for (bool cleanup_fails : {false, true}) {
    session_driver_boundary_t boundary(p, b);
    const std::string id(128, 'x');
    b->fail_snapshot = true;
    b->throw_release = cleanup_fails;
    bool threw = false;
    try { boundary.acquire_session_display(id, spec); }
    catch (const std::bad_alloc &) { threw = true; }
    check(threw && boundary.has_session(id) == cleanup_fails);
    b->fail_snapshot = false;
    b->throw_release = false;
    check(boundary.close().unresolved == 0);
  }
  {
    session_driver_boundary_t boundary(p, b);
    const std::string id(128, 'y');
    int before = b->allocated;
    fail_next_allocation = true;
    bool threw = false;
    try { boundary.acquire_session_display(id, spec); }
    catch (const std::bad_alloc &) { threw = true; }
    check(threw && b->allocated == before && !boundary.has_session(id));
  }
  {
    session_driver_boundary_t boundary(p, b);
    std::promise<void> entered, resume;
    auto resumed = resume.get_future();
    b->on_allocate = [&] { entered.set_value(); resumed.wait(); };
    auto allocation = std::async(std::launch::async, [&] { return boundary.acquire_session_display("slow", spec); });
    entered.get_future().wait();
    auto heartbeat = std::async(std::launch::async, [&] { return boundary.tick_heartbeat(); });
    bool independent = heartbeat.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    resume.set_value();
    check(allocation.get().second.has_value());
    check(heartbeat.get() && independent);
    b->on_allocate = {};
  }
  std::cout << "native boundary tests passed\n";
}
