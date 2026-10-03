# Experimental coordinator audit repair

NOT linked into full Sunshine CMake, nvhttp, a real registry, or driver IOCTLs.

## Source contract, deliberately changed

The previous `apollo_permission_flags_e` and `signed_resolution_request_t`
were invented interfaces, not Apollo protocol. Both are removed. Actual local
`apollo/master:src/crypto.h`, `crypto::PERM`, declares `_action = 1 << 24`,
`view = _action << 1`, `launch = _action << 2`, `_allow_view = view | launch`.
`apollo/master:src/nvhttp.cpp` checks launch separately from `_allow_view`.
Future real authenticated-registry integration must map those semantics into
`action_e` / `policy_decision_t`; display/mode/HDR decisions are INTERNAL policy,
not invented permission bits. No resolution signature or crypto is claimed.
Signed 64-bit dimensions/rate are bounded before any unsigned driver conversion.

Lifecycle mutation is synchronous, serialized per coordinator, including across
allocation/start/stop. Stop during startup waits for the operation then quiesces
the app before restore acknowledgment before release. There is no stale async
allocation callback: allocation returns synchronously, publication is explicitly
pending/ready/failed and polled with the incarnation token. Tokens are globally
monotonic in this process and fail rather than wrap. All mutators except initial
prepare and whole-coordinator close require the token. Arbitrary callback execution
API removed. Reusing a released session ID cannot accept old-token mutations.

External calls NEVER run under the snapshot mutex. Callbacks may query snapshots;
callbacks MUST NOT reenter mutations or recovery retry. Specifically, provider callbacks
invoked during ledger cleanup/retry execute while recovery_ledger_t::mutex_ is held;
implementations MUST NOT call ledger.pending() or ledger.retry() synchronously, as
the non-recursive mutex will deadlock. Snapshot during an external
operation reflects the last completed publication, not partial mutable ownership.
Dependencies must return in bounded time. No forced cancellation of hung external
code is promised. Owner must quiesce all callers before destruction.

Allocation obligation is pre-registered in a caller-retained recovery ledger before
any external acquisition. A returned handle is moved without allocation into that
owner before fallible snapshot copies. If allocation throws before returning a
handle, the provider MUST clean its own provisional resource (no caller can recover
an undisclosed handle). Stop/join MUST cover partially started apps even when start
returns false/throws. Boolean/throwing cleanup failures MUST mean the obligation
is still retryable; providers cannot report ambiguous successful release as failure.
App, topology, and live-allocation flags are independent, so release failure retries
do not restore twice; failed restore NEVER releases. Notification exceptions are
contained. Memory-allocation failures in snapshots may propagate, but ownership is
already retained; this is not a no-allocation/fault-tolerant snapshot facility.

`close()` attempts every session in order-independent iteration; destructor attempts
close, then transfers existing obligations (no allocating handoff) to the ledger.
Caller MUST retain ledger and dependencies, call retry, and surface unresolved
obligations operationally. Destroying the last ledger with pending resources is NOT
guaranteed cleanup. Ledger retry exclusively claims detached obligations and cannot
touch live coordinator entries. No background worker/framework is introduced.
The former restart-to-resume behavior could overwrite a retained resource; restart
is now rejected. Reconnect/resume needs a separately reviewed API before integration.

## Regression preservation and extension

The prior source actually had nine named test functions (not eleven): denial,
allocation failure, probe rollback, app rollback, active stop, pending timeout,
retained/resume, late callback generation, unresolved/retry. All corresponding
behavioral cases remain, updated for the explicit contract. Invalid fake-signature
case becomes internal policy deny; arbitrary late callback becomes old-token
mutation rejection; unsafe retained restart is rejected rather than leaking.
The old timeout tested prepared/no-op; new test allocates, observes pending,
advances fake clock past deadline, polls and verifies restore/release.

21 independently registered CTest cases include signed bounds, publication failure
and readiness, partial app failure, ordered cleanup, stop/restore/release failures,
stale ID reuse and cross-coordinator monotonic tokens, reentrant snapshot queries,
ten injected external exception points, durable destructor handoff, destructor
attempt-all, concurrent exclusive retry and stop during blocked allocation.
Promise gates are bounded (3 seconds), every CTest has a 12-second timeout; no sleeps.
The serialized design eliminates overlapping allocator completion rather than
attempting to salvage a stale async callback abstraction.

## Actual validation

RED: new failed-restore/no-release assertion failed against original actual cpp.
Original strict build also failed on unused fake parameters under warnings-as-errors.
Toolchain is `P:/msys64/ucrt64`, GCC 16.2.0 (the AGENTS example C: install is absent).

```sh
cmake -S tools/session-coordinator-tests -B cmake-build-coordinator-audit-fixed \
  -G Ninja -DCMAKE_CXX_FLAGS='-Wall -Wextra -Werror'
cmake --build cmake-build-coordinator-audit-fixed
ctest --test-dir cmake-build-coordinator-audit-fixed --output-on-failure
```

Actual coordinator cpp is compiled, not a model. No full-target wiring, hardware,
TSAN, production policy adapter or live cleanup integration is validated here.
Independent review is still required before wiring.

Verified result: strict GCC 16.2.0 build succeeded; 21/21 CTests passed.
Reentrant queries, concurrent retry, and stop-during-start each also passed 20
consecutive runs (60 extra executions). `git diff --check` passed.
