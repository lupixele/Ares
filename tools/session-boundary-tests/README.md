# Native seam repair — experimental / uncompiled

This target compiles the actual `src/platform/windows/session_driver_boundary.cpp`,
not a model and not `tests/tests_common.h`. It needs C++17, the standard library,
CMake >= 3.16 and threads; no gtest or Sunshine dependencies. The existing Windows
gtest TU is `_WIN32` guarded because application source linkage is Windows-only
while test discovery uses a universal glob.

## Reproduce

From the worktree inside the approved MSYS2 UCRT64 shell (or on Unix):

```sh
cmake -S tools/session-boundary-tests -B cmake-build-session-boundary
cmake --build cmake-build-session-boundary
ctest --test-dir cmake-build-session-boundary --output-on-failure
```

Without CMake:

```sh
mkdir -p cmake-build-session-boundary
g++ -std=c++17 -pthread -Wall -Wextra -I. tools/session-boundary-tests/main.cpp src/platform/windows/session_driver_boundary.cpp -o cmake-build-session-boundary/session_boundary_tests.exe
./cmake-build-session-boundary/session_boundary_tests.exe
```

Usual Windows prefix: `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64 -c`.
This machine only has `P:\msys64\msys2_shell.cmd`, owned by the separately authorized
installer worker. One discovery check found neither g++ nor cmake in that shell.
No packages installed, no polling, and no passing compile/test result claimed.
Tests preceded repair, but executable RED/GREEN remains blocked by the compiler.

## Ownership and concurrency

- Authorize then reject duplicates without touching originals, including retained
  failed cleanup. Fresh allocation failure cannot replace an existing resource.
- Reserve map/key/spec before external allocation. No-throw move the returned name
  into that record and immediately attach a cleanup guard. Snapshot-copy exceptions
  attempt release, retain failed cleanup identifiers, and propagate.
- Backend must self-reconcile when throwing before returning an identifier. Caller
  RAII cannot recover an undisclosed resource. Names must be unique and nonempty;
  release retries after ambiguous failure must be safe/idempotent.
- `close()` permanently stops acquisition, attempts all releases, reports counts,
  and retains failures for retry. Destruction attempts all, contains release throws,
  and hands residual responsibility to mandatory `unresolved_cleanup()` (noexcept,
  durable reconciliation rather than silent discard). No complete rollback promise.
- Callers retain shared lifecycle ownership for each call and stop/join callers
  before destruction. Backend/policy calls must be bounded and nonreentrant.
  Lifecycle operations serialize in their own lane; heartbeat has an independent
  lock and may overlap them. Backend must support concurrent watchdog/lifecycle I/O.
  This avoids generation/pending cancellation complexity by prohibiting overlapping
  lifecycle mutation; bounded backend calls are still required for bounded shutdown.

## Checks and remaining gates

Harness exercises distinct resources; denied/failed duplicates; false/throw release
retention/retry; close rejection; destructor all-attempt/unresolved handoff; actual
heap failure before external allocation and during snapshot copying with successful
or throwing cleanup; and heartbeat progress during blocked allocation. Heap failure
is a one-shot thread-local allocator override, not a model. Retained failures remain
visible through session queries and close counts, not a public detailed state API.

Still required: actual compile/run; sanitizer/race checks; formatting/full Doxygen
validation; full Windows gtest; concurrent stress; genuine IOCTL backend with durable
reconciliation and independent bounded watchdog; nvhttp authorization/launch/resume;
driver signing/provenance; HDR/refresh/topology parity; physical Windows/Android tests.
No native IOCTL or nvhttp wiring was added. Do not integrate before native tests pass.
