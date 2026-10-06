# Paired-Client Permission Compatibility Schema

## Overview and Background

This slice introduces Apollo-compatible client permission definitions, persistence serialization, and request-principal snapshotting into Sunshine's paired-client registry without prematurely claiming enforcement guarantees before runtime enforcement points and administrative interfaces are implemented.

## Permission Enum Bitmask (Exact Apollo Values)

The permissions enum `crypto::PERM` directly mirrors the exact upstream Apollo definition:

| Group | Enumerator | Bit / Shift | Hex | Decimal | Description |
|---|---|---|---|---|---|
| Reserved | `_reserved` | `1` | `0x00000001` | 1 | Reserved bit |
| Input | `_input` | `_reserved << 8` | `0x00000100` | 256 | Base shift for input permissions |
| Input | `input_controller` | `_input << 0` | `0x00000100` | 256 | Allow gamepad/controller input |
| Input | `input_touch` | `_input << 1` | `0x00000200` | 512 | Allow touch input |
| Input | `input_pen` | `_input << 2` | `0x00000400` | 1024 | Allow pen/stylus input |
| Input | `input_mouse` | `_input << 3` | `0x00000800` | 2048 | Allow mouse input |
| Input | `input_kbd` | `_input << 4` | `0x00001000` | 4096 | Allow keyboard input |
| Input Mask | `_all_inputs` | Bits 8–12 | `0x00001F00` | 7936 | All input permissions combined |
| Operation | `_operation` | `_input << 8` | `0x00010000` | 65536 | Base shift for operation permissions |
| Operation | `clipboard_set` | `_operation << 0` | `0x00010000` | 65536 | Allow setting host clipboard from client |
| Operation | `clipboard_read` | `_operation << 1` | `0x00020000` | 131072 | Allow reading host clipboard |
| Operation | `file_upload` | `_operation << 2` | `0x00040000` | 262144 | Allow uploading files to host |
| Operation | `file_dwnload` | `_operation << 3` | `0x00080000` | 524288 | Allow downloading files from host |
| Operation | `server_cmd` | `_operation << 4` | `0x00100000` | 1048576 | Allow executing server command |
| Operation Mask | `_all_opeiations` | Bits 16–20 | `0x001F0000` | 2031616 | All operation permissions combined (Apollo typo preserved) |
| Action | `_action` | `_operation << 8` | `0x01000000` | 16777216 | Base shift for action permissions |
| Action | `list` | `_action << 0` | `0x01000000` | 16777216 | Allow listing applications |
| Action | `view` | `_action << 1` | `0x02000000` | 33554432 | Allow viewing streams |
| Action | `launch` | `_action << 2` | `0x04000000` | 67108864 | Allow launching applications |
| Action Combined | `_allow_view` | `view \| launch` | `0x06000000` | 100663296 | View or launch (required for resume/display view) |
| Action Mask | `_all_actions` | Bits 24–26 | `0x07000000` | 117440512 | All action permissions combined |
| Defaults / Meta | `_default` | `view \| list` | `0x03000000` | 50331648 | Upstream default permission preset |
| Defaults / Meta | `_no` | `0` | `0x00000000` | 0 | No permissions granted |
| Total Mask | `_all` | `_all_inputs \| _all_opeiations \| _all_actions` | `0x071F1F00` | 119480064 | All permissions combined |

### Note on Mask Calculation
The composite value `_all` is `0x071F1F00` (`119480064`). Prior interim reports citing `119479984` were incorrect due to omitted bit positions.

## Independence of Enabled State
The `enabled` boolean flag in `named_cert_t` represents whether a client certificate is allowed to complete TLS authorization and establish connections with Sunshine. It is completely independent of the client's permission bitmask:
- A client with `perm == crypto::PERM::_no` (0) remains authorized to connect if `enabled == true`.
- A client with `enabled == false` is rejected during per-request TLS dispatch regardless of any permissions in `perm`.

## Persistence and Safe Schema Migration

1. **Legacy Record Compatibility (No Lockout)**:
   Persisted records in `sunshine_state.json` without an explicit `"perm"` field default to `crypto::PERM::_all` (`119480064`). This ensures existing paired clients retain full access upon upgrading and are never locked out.
2. **Unknown Bits Stripping**:
   Persisted values are masked against `crypto::PERM::_all` (`0x071F1F00`) to strip unknown bits while preserving recognized permissions.
3. **PropertyTree Lexical Validation and Fail-Safe Schema Enforcement**:
   - Sunshine parses persisted state using Boost PropertyTree (`boost::property_tree::read_json`), which treats all scalar JSON values as untyped lexical strings stored in `ptree::data()`. Consequently, PropertyTree cannot differentiate between a quoted numeric string (`"256"`) and an unquoted JSON numeric literal (`256`), nor between booleans, null literals, and textual strings.
   - The parser intentionally uses `std::from_chars` rather than `boost::lexical_cast` to enforce strict numeric boundaries without throwing exceptions.
   - **Containers (Objects / Arrays)**: Non-empty JSON objects (`{"nested": 1}`) and non-empty arrays (`[1, 2]`) contain child nodes in PropertyTree (`!perm_node->empty()`). These are detected and fail safe to `crypto::PERM::_no` (`0`). Empty JSON containers (`{}` and `[]`) have no child nodes and empty data strings (`perm_node->data().empty()`), failing safe to `crypto::PERM::_no`.
   - **Null Literals and Text**: JSON `null` literals are loaded as the string `"null"`. Non-integer text and `null` fail `std::from_chars` parsing and fail safe to `crypto::PERM::_no`.
   - **Negative Integers**: Any string with a leading `-` (e.g. `-1` or `"-256"`) is rejected and fails safe to `crypto::PERM::_no`.
   - **Floating Point Numbers**: Floating point values (e.g. `12.34`) are rejected because `std::from_chars` does not consume the fractional component (`ptr != end`), failing safe to `crypto::PERM::_no`.
   - **Numeric Overflow**: Quoted or unquoted numeric strings exceeding `std::numeric_limits<uint32_t>::max()` (e.g. `"4294967296"` or `"99999999999999999999"`) fail `val > max` or `std::errc::result_out_of_range` checks and fail safe to `crypto::PERM::_no`.
   - **Valid Quoted/Unquoted Numbers**: Valid numeric representations within 32-bit range (e.g. `256` or `"256"`) are parsed identically and masked with `_all`.

## Re-Pairing Lifecycle and Privilege Preservation

1. **Exact Certificate Deduplication**:
   When a client pairs with Sunshine, `add_authorized_client()` parses the client certificate and compares it against all registered clients using OpenSSL's `X509_cmp()`. This matches exact certificate identity across formatting differences (such as CRLF vs LF newlines). Any duplicate records matching the certificate are removed.
2. **Preserving Permissions on Re-Pair**:
   Re-pairing with the same certificate MUST NOT escalate a client's privileges. If an existing record exists for the certificate, its existing permission bitmask (`perm`) is preserved during re-pairing. For example, a client restricted to `crypto::PERM::_no` (0) or `crypto::PERM::_default` retains its restricted permissions. First-time pairings (a certificate not currently registered) default to full permissions (`crypto::PERM::_all`).
3. **Preserving Disabled Status on Re-Pair**:
   If an administrator disabled a client (`enabled == false`), re-pairing the same certificate preserves the disabled status. Re-pairing cannot re-enable a revoked or disabled client without an explicit administrative action in the Sunshine Web UI or configuration file. In legacy corrupt states containing multiple duplicate records, if any duplicate record was disabled, the preserved state remains disabled (`enabled == false`), and permissions are intersected.
4. **Friendly Name Updates**:
   When re-pairing an existing certificate with a new client name, the updated name is applied to the client record while preserving permissions and disabled status.
5. **UUID Identity Lifecycle Assessment**:
   Upstream Sunshine (PR #5737 / commit `0daeb0e3`) generates a new UUID upon completed re-pairing (`named_cert.uuid = uuid_util::uuid_t::generate().string()`). Analysis of Moonlight clients and existing tests reveals:
   - Moonlight client devices do not receive, store, or rely on Sunshine's internal UUID.
   - Sunshine's administrative Web UI tracks paired clients by UUID.
   - Existing test fixtures (`ClientAuthorizationTest.RePairingReplacesDuplicateCertificateIdentity`) explicitly verify that re-pairing generates a new UUID (`EXPECT_NE(repaired_uuid, original_uuid)`) to cleanly replace duplicate or stale registry records.
   - Preserving permissions and enabled status while generating a new UUID ensures administrative security policies are honored without breaking upstream UUID lifecycle conventions or existing test assertions.

## Per-Request TLS Principal Snapshot

Sunshine's per-request TLS dispatch mechanism binds an immutable `client_principal_t` instance to each verified request:
```cpp
struct client_principal_t {
  const std::string cert;
  const std::string name;
  const crypto::PERM perm;
};
```
On every HTTP request (including consecutive keep-alive requests on a persistent TLS connection), `resolve_tls_principal()` captures the latest permission bitmask from the server pairing registry under `client_auth_mutex()`. Registry modifications are immediately reflected on subsequent requests over existing keep-alive connections.

## Administrative Management Flow and REST API

Administrative mutation of client permissions and connection authorization is exposed through Sunshine's authenticated and CSRF-protected Web UI management API:

1. **Client Update Endpoint (`POST /api/clients/update`)**:
   - **Authentication and CSRF Protection**: Requires HTTP Basic Authentication and CSRF token verification on cross-origin requests.
   - **Payload Schema**:
     ```json
     {
       "uuid": "<client-uuid>",
       "enabled": true,
       "perm": 119480064
     }
     ```
   - **Partial Updates and Omitted Field Preservation**:
     - `uuid` is required. Missing, empty, or non-string UUIDs are rejected with `400 Bad Request`.
     - `enabled` is optional. When omitted, the client's current enabled status is preserved. When supplied, it must be a strict boolean.
     - `perm` is optional. When omitted, the client's current permission bitmask is preserved.
     - At least one update field (`enabled` or `perm`) must be provided.
   - **Strict HTTP Boundary Validation**:
     - Parsed directly with nlohmann::json.
     - Rejects negative integers (e.g. `-1`), floating point numbers (e.g. `256.5`), booleans (e.g. `true`), strings (e.g. `"256"`), arrays, objects, `null`, and values exceeding 32-bit unsigned integer range (`> 4294967295`) with `400 Bad Request`.
     - `perm: 0` (`crypto::PERM::_no`) is valid and accepted.
   - **Unknown Bits Stripping**:
     - Input bitmasks are masked against `crypto::PERM::_all` (`0x071F1F00`) to strip unassigned or future bits.
   - **Transactional Rollback on Persistence Failure**:
     - Mutates state under `client_auth_mutex()`.
     - If saving state to disk fails, in-memory modifications to `client_root` and certificate chains are rolled back to their previous values. The API reports failure (`status: false`), ensuring no transient privilege escalation occurs in memory.
   - **Separation of Concerns**:
     - `enabled` (certificate connection allowance) and `perm` (action/input permissions) are maintained independently. Granting `perm: 0` does not disable a client, and disabling a client preserves its assigned permissions.

2. **Client Listing Endpoint (`GET /api/clients/list`)**:
   - Authenticated endpoint returning all paired clients.
   - Each client record includes `name`, `uuid`, `enabled`, and `perm` (uint32 bitmask).

## Staged Implementation Boundary

- **New Client Default**: Newly paired clients continue to default to `crypto::PERM::_all` (`119480064`) to avoid accidental client lockout before pairing policy settings are introduced.
- **Initial-session enforcement**: Authenticated permission snapshots now reach stream input allocation and the controller, mouse, keyboard, touch and pen packet gate. `/applist` requires any `_all_actions` bit and returns Apollo's permission-denied placeholder otherwise; `/resume` requires `_allow_view`; `/launch` requires `launch`, except that a request for the already-running positive application ID uses `_allow_view` before the existing already-running response. Unsupported input-only launch behavior is not introduced.
- **Live Permission-Reduction Teardown & Ingress Quiescing**:
  - `nvhttp::update_client_tx` atomically returns transaction metadata (`cert`, `enabled_before`/`enabled_after`, `perm_before`/`perm_after`, `success`) under `client_auth_mutex()`, eliminating TOCTOU races between permission update and certificate lookup.
  - On successful persistence, revoking `_allow_view` or ANY input domain (`input_controller`, `input_touch`, `input_pen`, `input_mouse`, `input_kbd`), or disabling the client, immediately triggers session teardown via `rtsp_stream::terminate_sessions_by_cert(cert)`.
  - Teardown clears any pending RTSP launch session in `launch_event` for that certificate, stops and joins active streaming sessions, and destroys the client's retained input session from `retained_input_state()`.
  - Session stop drains `input_queue`, marks the input context stopped, quiesces the serial input worker (`task_pool`), and releases all tracked held keyboard keys and mouse buttons. Active touch contacts and pen buttons receive `LI_TOUCH_EVENT_CANCEL_ALL`.
  - RTSP admission revalidates the client against the paired registry prior to stream allocation; disabled clients or clients lacking `_allow_view` are rejected with `403 Forbidden`, and reduced permissions are synchronized to prevent stale privilege escalation.
- **Shutdown Limits & Known Scope**:
  - Permission grants (elevating bits) do not mutate live streams; running sessions preserve their immutable initial snapshot until reconnect.
  - Keyboard tracking (`key_press`) remains host-global; resetting keyboard keys on session teardown releases global keys rather than per-client virtual key states.
  - New pairings still default to full access (`crypto::PERM::_all`).
  - Clipboard and file-transfer permissions remain outside input packet domains and are not implemented by this slice.
- **Verification scope**: The parent independently built strict native targets and passed 108 selected tests including real loopback TLS action requests. Positive launch tests bypass encoder probing and process execution through a test-only seam after authorization; they are not live streaming acceptance. The seam is reset in fixture teardown.
- **Re-Pair Restrictions**: Re-pairing an existing client preserves both its configured permissions and its disabled state without privilege escalation.

## Permission-reduction concurrency checkpoint — 2026-10-06

- RTSP admission, registry revalidation, session insertion/start, and targeted
  teardown share a lifecycle lane. Teardown extracts sessions under the slot
  mutex, then stops/joins them outside that slot mutex. This is a synchronization
  boundary, not a wall-clock timeout guarantee for hardware/backend operations.
- Moonlight opens separate TCP connections for RTSP negotiation messages. Each
  accepted socket therefore receives a reusable, non-consuming pending-launch
  snapshot; accepting `OPTIONS` must not consume the ticket needed by `DESCRIBE`
  or later messages. Loopback wire tests cover those first two transactions.
- Control-thread launch cleanup is posted to the RTSP executor rather than taking
  the lifecycle mutex while holding the control-session mutex. Numeric launch IDs
  prevent a delayed clear from removing an unrelated pending launch.
- Timer callbacks carry a launch generation, so a completed old expiration cannot
  discard a replacement ticket. Startup failures and exceptions remove the slot
  and abandon pre-start resources without joining an unstarted session or changing
  the running-session counter. Partial worker startup signals shutdown and joins
  any created workers before releasing shared broadcast resources.
- The parent built both native targets with `BUILD_WERROR=ON`, tray disabled, and
  passed **158 selected tests** from 15 suites. These include real single-worker
  executor tests, deterministic admission/teardown barriers, fault injection and
  loopback TLS/RTSP requests. Hardware pipelines in admission tests are replaced
  with test-only hooks; real Windows streaming, POSIX behavior, coverage percentages
  and independently isolated per-client keyboard/mouse state remain unverified.

## Atomic Persistence and Filesystem Boundary Guarantees

Paired-client registry persistence through `nvhttp::save_state()` now uses `file_handler::write_file_atomic`. Other configuration/application-file writers are unchanged by this slice and are not covered by these guarantees:

- **Windows Same-Volume Rename Strategy**:
  - Replaces target files via `MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on the same volume. Cross-volume copies (`MOVEFILE_COPY_ALLOWED`) and intermediate target deletion are prohibited to prevent data loss.
   - Queries the existing target's DACL via `GetFileSecurityW(DACL_SECURITY_INFORMATION)` and supplies the returned descriptor, including its control flags, when exclusively creating the staged file. The restrictive protected-DACL fixture verifies that replacement does not broaden that DACL. This does not constitute an exhaustive ACL or adversarial-directory-race audit.
  - Newly created target files adhere to parent directory inheritance rules.
  - Note: NTFS alternate data streams and creation timestamps are not preserved by rename (unlike `ReplaceFileW`).
- **POSIX Secure Staging and Mode Preservation**:
  - Creates temporary files exclusively with mode `0600` (`S_IRUSR | S_IWUSR`) via `open(..., O_CREAT | O_EXCL, 0600)` to ensure sensitive state data is never readable by other users during staging.
  - Enforces write loop error checks (failing on 0-byte writes and retrying on `EINTR`), flushes data with `fsync()`, restores target permission bits via `fchmod()` prior to commit, and renames atomically via `rename()`.
  - File ownership (UID/GID) and umask restrictions are bounded by standard OS capability limits; full metadata preservation is not claimed.
- **Fail-Closed Target Validation & Exception Safety**:
  - Validates that target paths are regular non-reparse, non-symlink, non-directory files (`GetFileAttributesW` / `symlink_status` on Windows, `lstat` on POSIX). Rejects directory or symlink targets immediately without modifications.
  - Uses noexcept RAII guards (`HandleGuard` / `FdGuard` and `TempCleanupGuard` holding path by reference) to ensure all descriptors are closed and temporary files unlinked on any failure or exception without throwing.
