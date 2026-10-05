# Streaming Input Permissions & Ingress Gating

## Overview

This slice wires authenticated streaming-session permissions and certificate-derived identity retention:
- `nvhttp::make_launch_session` sets the session permission snapshot directly from the authenticated `client_principal_t` (TLS peer certificate matched against server pairing registry).
- `rtsp_stream::launch_session_t` stores the permission bitmask (defaulting backwards-compatibly to `crypto::PERM::_all`).
- `stream::session::alloc` captures this immutable permission snapshot, computes a cryptographically safe identity key using the SHA-256 certificate fingerprint (`crypto::cert_fingerprint`), and allocates input via `input::alloc` with the captured mask and fingerprint. Client-supplied query keys such as `uniqueid` are never trusted for retained identity.
- On reconnect/resume with unchanged permissions, the retained session is reused. If permissions in the pairing registry change during an active session or before resume, the resume allocation fails closed (`nullptr`) and returns HTTP 500 without silently escalating permissions or concurrently terminating the old active context.
- Control channel input callbacks (`IDX_INPUT_DATA` and `IDX_ENCRYPTED`) enforce the allocated context gate using the captured immutable permission mask.

## Security Scope & Limitations (Honest Assessment)

- **No Live Revocation**: Permission changes in the pairing registry while a stream is running DO NOT dynamically revoke privileges on the active stream; the active stream continues operating with its initial snapshot until disconnection.
- **No Endpoint Action Gating Yet**: Per-endpoint action enforcement (such as gating specific HTTP actions like app listing or launching) remains a separate stage.
- **Fail-Closed on Permission Conflict**: A resumed connection with a permission mismatch fails closed orderly, requiring explicit session reset before reuse.
- **No Secure-Full Claim**: This integration does not claim complete, end-to-end active revocation. Full revocation requires cooperative stream termination and input queue draining upon admin permission modification.

## Input Permission Bitmask Domains

When supplied a session permission snapshot, the new ingress gate checks the Apollo-compatible bitmask schema defined in `crypto::PERM` before packets enter the queue:

| Domain | Permission Bit | Bitmask Value | Governed Input Packet Magics |
|---|---|---|---|
| **Gamepad / Controller** | `input_controller` | `0x00000100` | `MULTI_CONTROLLER_MAGIC_GEN5`<br>`SS_CONTROLLER_ARRIVAL_MAGIC`<br>`SS_CONTROLLER_TOUCH_MAGIC`<br>`SS_CONTROLLER_MOTION_MAGIC`<br>`SS_CONTROLLER_BATTERY_MAGIC` |
| **Mouse** | `input_mouse` | `0x00000800` | `MOUSE_MOVE_REL_MAGIC_GEN5`<br>`MOUSE_MOVE_ABS_MAGIC`<br>`MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5`<br>`MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5`<br>`SCROLL_MAGIC_GEN5`<br>`SS_HSCROLL_MAGIC` |
| **Keyboard** | `input_kbd` | `0x00001000` | `KEY_DOWN_EVENT_MAGIC`<br>`KEY_UP_EVENT_MAGIC`<br>`UTF8_TEXT_EVENT_MAGIC` |
| **Touch** | `input_touch` | `0x00000200` | `SS_TOUCH_MAGIC` |
| **Pen / Stylus** | `input_pen` | `0x00000400` | `SS_PEN_MAGIC` |

Packets with unknown or unmapped magic numbers are denied by policy (`default: return std::nullopt`).

## Packet Validation at Ingress Boundary

Before inspecting domain permissions, `validate_input_packet()` enforces strict size and buffer boundary checks:

1. **Buffer Size Check**: Buffer must contain at least `sizeof(NV_INPUT_HEADER)` (8 bytes).
2. **Safe Header Copy**: Header fields are extracted using byte copy (`std::memcpy`) and converted from network endianness to host endianness.
3. **Declared Size Bounds**:
   - `header.size >= sizeof(header.magic)` (minimum 4 bytes payload).
   - `header.size <= packet.size() - sizeof(header.size)` (declared payload cannot exceed buffer bytes).
4. **Typed Packet Validation**:
   - Fixed-size packets (`NV_REL_MOUSE_MOVE_PACKET`, `NV_KEYBOARD_PACKET`, etc.) must have exact matching declared size `sizeof(T) - sizeof(header.size)` and exact buffer size `sizeof(T)`.
   - Variable-length UTF-8 text packets (`NV_UNICODE_PACKET`) must not exceed the maximum allowed text buffer limit (`UTF8_TEXT_EVENT_MAX_COUNT = 32`).

Packets failing validation are dropped immediately and logged with a warning; malformed packets never enter `input_queue`.

## Ingress Permission Gating

Once a packet passes structural validation:

1. **Magic-to-Domain Mapping**: `required_permission_for_magic()` determines the required domain bit from `header.magic`.
2. **Explicit Intersection**: The effective permission is computed as the bitwise AND intersection of the per-call permission snapshot and the session's allocated permission mask:
   ```cpp
   const auto effective_permission = permission & input->permissions;
   ```
3. **Domain Gate**: If `!(effective_permission & *required_perm)`, the packet is rejected and dropped without queueing.
4. **Queue & Scheduling**: Permitted packets are appended to `input->input_queue` and scheduled on the worker thread pool if no task is currently active.

## Retained Session Lifecycle & Invariants

Sunshine supports session pausing and resuming across client disconnects via `retained_input_state()`:

1. **Immutable Allocation**: A session's permission mask is established when the session is first allocated via `input::alloc()` and remains immutable throughout the session lifetime.
2. **Resume Contract**: Reuse requires the same permission snapshot. A conflicting input mask must be rejected, not silently replaced or returned with stale higher privileges. The caller must handle a rejected allocation and quiesce/reset the old session before recreation; streaming integration is still pending.
3. **Lifecycle Termination**: Retained gamepad allocations and input states are freed upon explicit termination via `input::terminate_gamepads()` or when the application terminates.

Permission reductions on active streams must eventually drain queued work and release held input before a lower-privilege context is established. This is an integration requirement, not a verified guarantee of this admin API or input-ingress slice. Touch/pen/controller cleanup and concurrency behavior still require explicit tests. The retained-context key must also be bound to an authenticated certificate rather than a caller-supplied `uniqueid`.

## Testing & Safe Mock Sinks

Unit testing of input processing (`test_input_permissions.cpp`) avoids injecting artificial keyboard or mouse events into the host developer environment:

- `input::testing::set_keyboard_sink()` redirects emitted keyboard events into an in-memory recorder vector instead of calling platform `platf::keyboard_update()`.
- `input::testing::set_input_task_sink()` intercepts async background task dispatch, allowing deterministic and synchronous execution via `input::testing::process_next_message_sync()`.
