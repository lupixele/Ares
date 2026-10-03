/** @file @brief Deterministic actual-source regressions; CTest bounds every case. */
#include "src/session_display_coordinator.h"
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace ares::session;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { throw std::runtime_error(#x); } } while (false)

/** @brief Injectable dependencies with observable ordering and exception points. */
struct fake_t : itrusted_registry_t, idisplay_provider_t, iapp_lifecycle_t, iclock_t {
  policy_decision_t policy{true,true,true,true};
  publication_e published{publication_e::ready};
  bool allocate{true}, probe{true}, start{true}, stop{true}, restore{true}, release{true};
  std::string throws;
  std::vector<std::string> events;
  std::function<void(const std::string &)> callback;
  std::chrono::steady_clock::time_point time{};
  void event(const std::string &s) { events.push_back(s); if (callback) { callback(s); } if (throws == s) { throw std::runtime_error(s); } }
  policy_decision_t authorize(const paired_principal_t &, action_e) override { event("authorize"); return policy; }
  std::optional<std::wstring> allocate_display(const display_mode_spec_t &) override { event("allocate"); return allocate ? std::optional<std::wstring>(L"display") : std::nullopt; }
  publication_e publication(const std::wstring &) override { event("publication"); return published; }
  bool probe_display(const std::wstring &) override { event("probe"); return probe; }
  bool restore_display(const std::wstring &) override { event("restore"); return restore; }
  bool release_display(const std::wstring &) override { event("release"); return release; }
  void on_unresolved_resource(const std::wstring &) override { event("notify"); }
  bool start_app(const std::string &, const std::wstring &) override { event("start"); return start; }
  bool stop_and_join_app(const std::string &) override { event("stop"); return stop; }
  std::chrono::steady_clock::time_point now() override { event("clock"); return time; }
  size_t count(const std::string &s) const { size_t n=0; for (auto &e:events) { n += e==s; } return n; }
};
/** @brief Owner keeps recovery ledger alive after the coordinator is destroyed. */
struct fixture_t {
  std::shared_ptr<fake_t> f=std::make_shared<fake_t>();
  std::shared_ptr<recovery_ledger_t> ledger=std::make_shared<recovery_ledger_t>();
  std::unique_ptr<session_display_coordinator_t> c=std::make_unique<session_display_coordinator_t>(f,f,f,f,ledger,100ms);
  uint64_t prepare(action_e a=action_e::launch) { CHECK(c->prepare_session("s", {"client", "", ""}, {}, a)==coordinator_status_e::ok); return c->get_session("s")->generation; }
  uint64_t run() { auto g=prepare(); CHECK(c->start_session("s",g)==coordinator_status_e::ok); return g; }
  void state(session_display_state_e s) { CHECK(c->get_session("s")->state==s); }
};
/** @brief Bounded rendezvous, never sleeps; timeout also bounded by CTest. */
struct gate_t {
  std::promise<void> arrived, proceed;
  std::shared_future<void> go=proceed.get_future().share();
  void wait() { arrived.set_value(); CHECK(go.wait_for(3s)==std::future_status::ready); }
  void entered() { CHECK(arrived.get_future().wait_for(3s)==std::future_status::ready); }
};
static void denial() {
  fixture_t x; x.f->policy={};
  CHECK(x.c->prepare_session("s",{"client","",""},{})==coordinator_status_e::unauthorized);
  CHECK(x.f->count("allocate")==0);
  for (auto decision : {policy_decision_t{false,true,true,true}, {true,false,true,true}, {true,true,false,true}}) {
    x.f->policy=decision;
    CHECK(x.c->prepare_session("s",{"client","",""},{})==coordinator_status_e::unauthorized);
  }
  x.f->policy={true,true,true,false}; display_mode_spec_t m; m.hdr_capable=true;
  CHECK(x.c->prepare_session("s",{"client","",""},m)==coordinator_status_e::unauthorized);
}
static void bounds() {
  fixture_t x;
  for (auto m : {display_mode_spec_t{-1,1080,60,false}, {1920,-1,60,false}, {1920,1080,-1,false}, {7681,1080,60,false}, {1920,4321,60,false}, {1920,1080,241,false}}) {
    CHECK(x.c->prepare_session("s",{"client","",""},m)==coordinator_status_e::invalid_resolution);
  }
  CHECK(x.f->count("authorize")==0);
}
static void allocation_failure() { fixture_t x; auto g=x.prepare(); x.f->allocate=false; CHECK(x.c->start_session("s",g)==coordinator_status_e::allocation_failed); CHECK(x.f->count("release")==0); x.state(session_display_state_e::released); }
static void publication_failure() { fixture_t x; auto g=x.prepare(); x.f->published=publication_e::failed; CHECK(x.c->start_session("s",g)==coordinator_status_e::probe_failed); CHECK(x.f->count("release")==1); CHECK(x.f->count("probe")==0); }
static void probe_failure() { fixture_t x; auto g=x.prepare(); x.f->probe=false; CHECK(x.c->start_session("s",g)==coordinator_status_e::probe_failed); CHECK(x.f->count("release")==1); CHECK(x.f->count("start")==0); }
static void app_failure() { fixture_t x; auto g=x.prepare(); x.f->start=false; CHECK(x.c->start_session("s",g)==coordinator_status_e::app_start_failed); CHECK(x.f->count("stop")==1); CHECK(x.f->count("release")==1); }
static void active_stop() {
  fixture_t x; auto g=x.run(); x.f->events.clear(); CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok);
  CHECK((x.f->events==std::vector<std::string>{"stop","restore","release"}));
  CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); CHECK(x.f->count("release")==1);
}
static void pending_timeout() {
  fixture_t x; x.f->published=publication_e::pending; auto g=x.run(); x.state(session_display_state_e::pending);
  CHECK(x.f->count("allocate")==1); CHECK(x.f->count("start")==0);
  x.f->time+=101ms; CHECK(x.c->poll_session("s",g)==coordinator_status_e::timeout);
  CHECK(x.f->count("restore")==1); CHECK(x.f->count("release")==1); x.state(session_display_state_e::released);
}
static void pending_ready() {
  fixture_t x; x.f->published=publication_e::pending; auto g=x.run(); x.f->published=publication_e::ready;
  CHECK(x.c->poll_session("s",g)==coordinator_status_e::ok); x.state(session_display_state_e::streaming); CHECK(x.f->count("start")==1);
}
static void retained() {
  fixture_t x; auto g=x.run(); CHECK(x.c->disconnect_session("s",g,true)==coordinator_status_e::ok); x.state(session_display_state_e::app_retained); CHECK(x.f->count("release")==0);
  CHECK(x.c->start_session("s",g)==coordinator_status_e::invalid_state); // explicit no resume API, cannot overwrite ownership
  CHECK(x.c->app_exit("s",g)==coordinator_status_e::ok); CHECK(x.f->count("stop")==1); CHECK(x.f->count("release")==1);
}
static void stale_generation() {
  fixture_t x; auto g=x.run(); CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); auto fresh=x.prepare(); CHECK(fresh>g);
  CHECK(x.c->start_session("s",g)==coordinator_status_e::invalid_state); CHECK(x.c->stop_session("s",g)==coordinator_status_e::invalid_state);
  CHECK(x.c->start_session("s",fresh)==coordinator_status_e::ok); x.state(session_display_state_e::streaming);
  CHECK(x.c->poll_session("s",g)==coordinator_status_e::invalid_state);
  fixture_t y; CHECK(y.prepare()>fresh);
}
static void restore_retry() {
  fixture_t x; auto g=x.run(); x.f->restore=false;
  CHECK(x.c->stop_session("s",g)==coordinator_status_e::unresolved_resource); CHECK(x.f->count("release")==0);
  CHECK(!x.c->get_session("s")->allocated_resource.empty()); x.f->restore=true;
  CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); CHECK(x.f->count("stop")==1); CHECK(x.f->count("release")==1);
}
static void release_retry() {
  fixture_t x; auto g=x.run(); x.f->release=false; CHECK(x.c->stop_session("s",g)==coordinator_status_e::unresolved_resource); x.f->release=true;
  CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); CHECK(x.f->count("restore")==1); CHECK(x.f->count("release")==2);
}
static void stop_failure() {
  fixture_t x; auto g=x.run(); x.f->stop=false; CHECK(x.c->stop_session("s",g)==coordinator_status_e::unresolved_resource); CHECK(x.f->count("restore")==0); CHECK(x.f->count("release")==0);
  x.f->stop=true; CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok);
}
static void queries() {
  fixture_t x; x.f->callback=[&](const auto &) { (void)x.c->get_session("s"); }; auto g=x.run(); CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); x.f->callback={};
}
static void exceptions() {
  for (const std::string point : {"authorize","clock","allocate","publication","probe","start","stop","restore","release","notify"}) {
    fixture_t x; auto g=x.prepare(); x.f->throws=point;
    if (point=="authorize") { CHECK(x.c->prepare_session("other",{"c","",""},{})==coordinator_status_e::unauthorized); }
    else if (point=="stop" || point=="restore" || point=="release" || point=="notify") {
      CHECK(x.c->start_session("s",g)==coordinator_status_e::ok);
      if (point=="notify") { x.f->restore=false; }
      CHECK(x.c->stop_session("s",g)==coordinator_status_e::unresolved_resource);
    } else { CHECK(x.c->start_session("s",g)!=coordinator_status_e::ok); }
    x.f->throws.clear(); x.f->restore=true; CHECK(x.c->close()==coordinator_status_e::ok);
    CHECK(x.f->count("release") <= 2);
  }
}
static void destructor_handoff() {
  fixture_t x; x.run(); x.f->restore=false; x.f->throws="notify"; x.c.reset();
  CHECK(x.f->count("stop")==1); CHECK(x.f->count("release")==0); CHECK(x.ledger->pending()==1);
  x.f->throws.clear(); x.f->restore=true; CHECK(x.ledger->retry()==0); CHECK(x.f->count("release")==1);
  CHECK(x.ledger->retry()==0); CHECK(x.f->count("release")==1);
}
static void concurrent_retry() {
  fixture_t x; x.run(); x.f->restore=false; x.c.reset(); x.f->restore=true; gate_t gate;
  x.f->callback=[&](const auto &s) { if (s=="restore") { gate.wait(); } };
  auto a=std::async(std::launch::async,[&]{return x.ledger->retry();}); gate.entered();
  // entered.set_value() ensures second thread launched before proceed is signaled; not a proof of OS-level lock suspension.
  std::promise<void> entered; auto b=std::async(std::launch::async,[&]{entered.set_value(); return x.ledger->retry();});
  CHECK(entered.get_future().wait_for(3s)==std::future_status::ready); gate.proceed.set_value();
  CHECK(a.get()==0); CHECK(b.get()==0); CHECK(x.f->count("release")==1); x.f->callback={};
}
static void stop_during_start() {
  fixture_t x; auto g=x.prepare(); gate_t gate; x.f->callback=[&](const auto &s){if(s=="allocate"){gate.wait();}};
  auto a=std::async(std::launch::async,[&]{return x.c->start_session("s",g);}); gate.entered();
  CHECK(x.c->get_session("s")->generation==g);
  std::promise<void> entered; auto b=std::async(std::launch::async,[&]{entered.set_value();return x.c->stop_session("s",g);});
  CHECK(entered.get_future().wait_for(3s)==std::future_status::ready); gate.proceed.set_value();
  CHECK(a.get()==coordinator_status_e::ok); CHECK(b.get()==coordinator_status_e::ok); x.state(session_display_state_e::released); CHECK(x.f->count("release")==1); x.f->callback={};
  auto fresh=x.prepare(); CHECK(fresh>g); CHECK(x.c->start_session("s",g)==coordinator_status_e::invalid_state);
}
static void view_policy() { fixture_t x; auto g=x.prepare(action_e::view); CHECK(x.c->start_session("s",g)==coordinator_status_e::ok); CHECK(x.f->count("start")==0); CHECK(x.c->stop_session("s",g)==coordinator_status_e::ok); CHECK(x.f->count("stop")==0); }
static void view_disconnect_retain() {
  fixture_t x; auto g=x.prepare(action_e::view);
  CHECK(x.c->start_session("s",g)==coordinator_status_e::ok);
  CHECK(x.f->count("start")==0);
  CHECK(x.c->disconnect_session("s",g,true)==coordinator_status_e::ok);
  x.state(session_display_state_e::released);
  CHECK(x.f->count("start")==0);
  CHECK(x.f->count("stop")==0);
  CHECK(x.f->count("restore")==1);
  CHECK(x.f->count("release")==1);
}
static void destructor_attempt_all() {
  fixture_t x; x.run();
  CHECK(x.c->prepare_session("second",{"client","",""},{})==coordinator_status_e::ok);
  CHECK(x.c->start_session("second",x.c->get_session("second")->generation)==coordinator_status_e::ok);
  x.f->restore=false; x.f->throws="notify"; x.c.reset();
  CHECK(x.f->count("stop")==2); CHECK(x.f->count("restore")==2); CHECK(x.f->count("release")==0); CHECK(x.ledger->pending()==2);
  x.f->throws.clear(); x.f->restore=true; CHECK(x.ledger->retry()==0); CHECK(x.f->count("release")==2);
}
/** @brief Dispatch one independently registered regression. */
int main(int argc, char **argv) {
  const std::vector<std::pair<std::string,void(*)()>> tests={
    {"destructor_attempt_all",destructor_attempt_all},
    {"denial",denial},{"bounds",bounds},{"allocation_failure",allocation_failure},{"publication_failure",publication_failure},{"probe_failure",probe_failure},{"app_failure",app_failure},{"active_stop",active_stop},{"pending_timeout",pending_timeout},{"pending_ready",pending_ready},{"retained",retained},{"stale_generation",stale_generation},{"restore_retry",restore_retry},{"release_retry",release_retry},{"stop_failure",stop_failure},{"queries",queries},{"exceptions",exceptions},{"destructor_handoff",destructor_handoff},{"concurrent_retry",concurrent_retry},{"stop_during_start",stop_during_start},{"view_policy",view_policy},{"view_disconnect_retain",view_disconnect_retain}};
  try { for (auto &[name,fn]:tests) { if (argc==2 && name==argv[1]) { fn(); std::cout<<name<<" passed\n"; return 0; } } }
  catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
  return 2;
}
