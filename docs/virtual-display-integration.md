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
   application is launched, so the display exists — and is the display the desktop is using — before
   the first frame is captured or the application looks for a monitor.
2. The seam computes the requested resolution through `resolve_virtual_display_geometry`, which
   defaults unset values, applies client scaling, enforces even dimensions and a usable minimum, and
   clamps out-of-range refresh rates and oversized panels.
3. `virtual_display_manager_t` asks the driver boundary for the display and records it against the
   session's client identity.
4. The manager then hands the display to its display target, which switches the desktop to it (see
   below).
5. A single watchdog keep-alive (`session_heartbeat_t`) starts when the first display is allocated and
   stops when the last one is released.
6. `proc_t::terminate()` calls `session_virtual_display_end()`. The layout from before the session is
   put back, then the display is released and the watchdog stopped.

The seam is deliberately the only Windows-aware piece, so `process.cpp` stays portable and needs a
single `#ifdef _WIN32` around two calls.

## Pointing the session at the display

Creating a display is not the same as using it. A monitor that has just been created is not where
applications open, so without this step the application would still launch on the operator's monitor
and the client would capture that same physical desktop while the new display stayed dark.

The target (`virtual_display_target.h`) therefore makes the new display the **only active display**
for the duration of the session. Isolation is the point rather than a side effect: Sunshine's capture
enumeration only returns displays that are attached to the desktop, so an isolated session has exactly
one capture candidate, and an application launched after the switch opens on the display the client is
watching.

Switching happens in two steps, because a display that has never been used does not yet have the modes
a single call would have to name:

1. **Propose the layout.** The target submits one path — the virtual display, marked active, with every
   mode index cleared — and no modes, so Windows builds the layout and assigns the source and target
   modes itself. This is the step that works for a display the operating system has only just learned
   about.
2. **State the geometry.** The display is now active, so it has a source mode. The target reads the
   layout back, sets that source mode to the requested size, states the requested refresh rate, and
   clears the target index so Windows picks the target mode that matches the new resolution. The
   exact mode is requested first; adjustment is only allowed after a rejection.

A rejected layout is reported and the session continues on the existing display. If the switch succeeds
but the geometry cannot be set, the session keeps the switch and streams at the mode Windows selected —
a wrong size is a degraded session, while losing the display would be a broken one.

The switch is verified rather than assumed. Windows is known to accept a topology that names one
display and then activate a different, near-identical one, so after applying the layout the target
reads it back and confirms, by adapter and target identifier, that the requested display is present and
that no other display is still active. A switch that fails that check is rolled back and reported as a
failed activation, which means the session falls back to the physical display instead of streaming a
display the client cannot see.

Both transitions are synchronous and owned here: the pre-session layout is captured, with its modes,
before the switch and applied again before the display is removed. If Windows rejects the exact layout
on restore, the topology alone is restored, so the operator's displays always come back even when their
modes do not. The project's display-device persistence layer is deliberately *not* used for this,
because its revert is deferred by `dd_config_revert_delay` (three seconds by default) and would
therefore run after the virtual display no longer exists, against a topology that still references it.
Keeping the swap in memory also means this feature never touches Sunshine's own persisted display
state (`display_device.state`): the restore writes the operator's original layout back to the Windows
display database, where it was already the saved configuration, so the two cannot drift apart.

One isolation is possible at a time: a second concurrent session cannot also be made the only active
display, so the most recent session owns the desktop and a second client is not isolated. This matches
the single-client streaming case the feature is for.


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
| New display not visible to the OS yet | Reported as `device_missing`; the session proceeds on the existing display. |
| Operating system refuses the layout | Reported as `activation_failed`; the session proceeds on the existing display. |
| A different display is activated instead | Detected by identity, rolled back, and reported as `activation_failed`; the session proceeds on the existing display. |
| Layout accepted but the geometry rejected | The switch is kept and a warning is logged; the session streams at the mode Windows selected. |
| Exact layout rejected on restore | The saved topology is restored without its modes, so the operator's displays come back. |
| Layout cannot be put back | Logged as a warning, and the display is still released, so nothing is leaked. |
| Display cannot be removed | Retained and reported, never silently forgotten; retried on shutdown. |
| Same session starts twice | The existing display is reused rather than a second one being created. |
| Two sessions at once | Only one display can be isolated; the latest session owns the desktop. |
| Watchdog stops answering | Failures counted and logged; the loop never crashes or hangs. |

## Testing

- `tests/unit/platform/windows/test_sudovda_driver.cpp` — 8 tests, including one that performs a
  real read-only handshake (open, protocol version, watchdog ping) against an installed driver and
  skips cleanly when none is present.
- `tests/unit/platform/windows/test_virtual_display_geometry.cpp` — 12 tests on the resolution
  policy; no driver involved.
- `tests/unit/platform/windows/test_session_heartbeat.cpp` — 11 tests on the keep-alive: interval
  derivation, start/stop lifecycle, one-loop guarantees, failure counting, and a throwing callback.
- `tests/unit/platform/windows/test_virtual_display_manager.cpp` — 18 tests on the lifecycle, using
  a recording backend so no real display is created or destroyed.
- `tests/unit/platform/windows/test_virtual_display_target.cpp` — 14 tests: path selection as a pure
  function, plus the activation and restore contract driven through the manager with a recording
  target, so the display layout of the host running the tests is never changed.
- `tests/unit/platform/windows/test_virtual_display_topology.cpp` — 20 tests on the data handed to
  the operating system: what a proposed layout looks like, how geometry is stated on an active
  display, how mode indexes (source, target and desktop image) are compacted and rewritten for a
  restore, and how a display is identified and proved to be the only one in use. No display
  configuration is read or written by these tests.

Current totals: **864 native tests, 859 passed, 5 environmental skips, 0 failures** (native suite
re-run 2026-11-23 on the restamped build). The skips are a disabled system tray build, an unlicensed
virtual HID broker, two GPU encoders that this host does not have, and an external command test. The
frontend suite is at **131 tests plus a production Vite build**, including three that mount this
feature's checkbox and assert it is offered on Windows only.

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
- Capture targeting has not been observed on real hardware: no test has yet confirmed that an
  application launched after the switch opens on the virtual display, and the layout-restore path has
  not been run against a real desktop with the installed driver.
- The full streaming integration (`virtual_display` enabled, a real client attaching) has not been run.
- No GPU binding checks, no install/packaging validation, no supported driver distribution.
