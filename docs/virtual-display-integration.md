# Session virtual display — integration

## What this is

A streaming session can create a virtual display for its own use and remove it when it ends. The
display is created by the SudoVDA driver **that is already installed on the host**. No driver
binary, INF, signature, or install script is added, changed, or shipped by Ares.

## How to enable it

`virtual_display = enabled` in the configuration file, or the *Create a Virtual Display per Session*
checkbox in the **Audio/Video** tab of the web UI. Windows only, and **off by default**, so an
existing install streams the physical desktop exactly as before the option existed.

## What happens on a stream

1. `proc_t::execute()` calls `platf::windows::session_virtual_display_begin()` just before the
   application is launched, so the display exists before the first frame is captured.
2. The seam computes the requested resolution through `resolve_virtual_display_geometry`, which
   defaults unset values, applies client scaling, enforces even dimensions and a usable minimum, and
   clamps out-of-range refresh rates and oversized panels.
3. `virtual_display_manager_t` asks the driver boundary for the display and records it against the
   session's client identity.
4. A single watchdog keep-alive (`session_heartbeat_t`) starts when the first display is allocated and
   stops when the last one is released.
5. `proc_t::terminate()` calls `session_virtual_display_end()`, releasing the display and stopping the
   watchdog.

The seam is deliberately the only Windows-aware piece, so `process.cpp` stays portable and needs a
single `#ifdef _WIN32` around two calls.

## Why the watchdog matters

The SudoVDA driver can run a watchdog that removes a virtual display that nobody keeps alive. Without
a keep-alive, a long stream could lose the display it is capturing mid-session. The manager therefore
owns one `session_heartbeat_t` for the process: it pings through the boundary while any session holds a
display and stops when the last is released, so a reconnect restarts it rather than leaving a stale
loop behind. A ping interval is derived from the driver's own watchdog timeout — one third of it —
clamped to a sane range, so two missed pings are tolerated before the driver acts.

## Behaviour when something is wrong

| Situation | What happens |
|---|---|
| Feature disabled | Nothing is allocated; behaviour is identical to an unpatched host. |
| Driver not installed | The session proceeds on the existing display, with a log line saying so. |
| Driver installed but incompatible | Handled at open time; the session proceeds without a virtual display. |
| Requested display rejected | Returns `unavailable`; the session proceeds. |
| Client requests an absurd size | Clamped to a normal panel, aspect preserved, before the driver sees it. |
| Display cannot be removed | Retained and reported, never silently forgotten; retried on shutdown. |
| Same session starts twice | The existing display is reused rather than a second one being created. |
| Watchdog stops answering | Failures counted and logged; the loop never crashes or hangs. |

## Testing

- `tests/unit/platform/windows/test_sudovda_driver.cpp` — 8 tests, including one that performs a
  real read-only handshake (open, protocol version, watchdog ping) against an installed driver and
  skips cleanly when none is present.
- `tests/unit/platform/windows/test_virtual_display_geometry.cpp` — 12 tests on the resolution
  policy; no driver involved.
- `tests/unit/platform/windows/test_session_heartbeat.cpp` — 11 tests on the keep-alive: interval
  derivation, start/stop lifecycle, one-loop guarantees, failure counting, and a throwing callback.
- `tests/unit/platform/windows/test_virtual_display_manager.cpp` — 19 tests on the lifecycle, using
  a recording backend so no real display is created or destroyed.

Parent-verified totals: **811 native tests passed, 5 pre-existing environmental skips, 0 failures**,
plus **120 frontend tests** and a production Vite build. The watchdog additions are verified by a
full parent re-run, recorded separately below.

## Verified on real hardware

**Test A, 2026-10-22** — the driver path was exercised against the installed SudoVDA driver with the
stream stopped: open, protocol version **0.2.1 compatible**, allocate 1920×1080@60, display
**appeared** (1 → 2 active), release, display **gone** (2 → 1). Exit status reported a clean create
and remove with **no leak**. Independently confirmed afterwards: exactly one display remained (the
physical monitor), both SudoMaker and StarDesk adapters reported OK, and no new driver fault events.

This validates the driver contract and teardown hygiene at a safe geometry. It does **not** validate
the clamped or high-DPI resolution paths, GPU capture, or keyboard docking.

## Not verified

- The touch-keyboard docking fix has not been observed on a real high-DPI virtual display.
- The full streaming integration (`virtual_display` enabled, a real client attaching) has not been run.
- No GPU binding checks, no install/packaging validation, no supported driver distribution.
