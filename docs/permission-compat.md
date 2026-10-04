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

## Staged Implementation Boundary

- **New Client Default**: Newly paired clients continue to default to `crypto::PERM::_all`.
- **Enforcement Readiness**: Input passthrough gating and administrative UI management remain unported. Restricting permissions at this stage would present a false sense of security while low-level input backends remain ungated.
- **Endpoint Policy**: Endpoint-level policies (`/launch`, `/resume`, `/applist`) are maintained without behavioral change until full action and stream input enforcement slices are ratified.
