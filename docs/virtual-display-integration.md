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
4. `proc_t::terminate()` calls `session_virtual_display_end()`, releasing the display.

The seam is deliberately the only Windows-aware piece, so `process.cpp` stays portable and needs a
single `#ifdef _WIN32` around two calls.

## Behaviour when something is wrong

Every failure mode is designed so that streaming continues rather than dies:

| Situation | What happens |
|---|---|
| Feature disabled | Nothing is allocated; behaviour is identical to an unpatched host. |
| Driver not installed | The session proceeds on the existing display, with a log line saying so. |
| Driver installed but incompatible | Handled at open time; the session proceeds without a virtual display. |
| Requested display rejected | Returns `unavailable`; the session proceeds. |
| Client requests an absurd size | Clamped to a normal panel, aspect preserved, before the driver sees it. |
| Display cannot be removed | Retained and reported, never silently forgotten; retried on shutdown. |
| Same session starts twice | The existing display is reused rather than a second one being created. |

## Testing

- `tests/unit/platform/windows/test_sudovda_driver.cpp` — 8 tests, including one that performs a
  real read-only handshake (open, protocol version, watchdog ping) against an installed driver and
  skips cleanly when none is present.
- `tests/unit/platform/windows/test_virtual_display_geometry.cpp` — 12 tests on the resolution
  policy; no driver involved.
- `tests/unit/platform/windows/test_virtual_display_manager.cpp` — 15 tests on the lifecycle, using
  a recording backend so no real display is created or destroyed.

Parent-verified totals: **811 native tests passed, 5 pre-existing environmental skips, 0 failures**,
plus **120 frontend tests** and a production Vite build.

## Not verified

- No real display has been created or destroyed on the development host by this change.
- Whether the touch keyboard docks correctly on a real high-DPI virtual display is untested.
- No hardware streaming acceptance, no GPU binding checks, no install/packaging validation.
