# Experimental per-request TLS principal

## Threat boundary and design

HTTP query strings, paths, headers, and client-provided names are untrusted. The
asset is the paired identity carried into a launch/resume session. A process-global
last-verified certificate lets another connection overwrite that identity between
handshake and request; caching authorization for a connection also misses disable
and unpair changes on keep-alive requests.

The adapter calls a server-owned virtual authorization hook before **any** parsed
request is routed (including upgrade handling, default handlers, and keep-alive).
The HTTPS implementation obtains the certificate from the session's actual TLS
socket after a successful handshake, then holds `client_auth_mutex()` while running
the existing `verify_client_certificate()` and selecting the matching record.
The helper requires successful existing chain verification **and** exactly one
enabled exact certificate match. Unknown, disabled, deleted and ambiguous records
fail closed. `get_client_status()`'s permissive unknown default is not used.

The resulting immutable certificate/name snapshot belongs to that request and is
passed explicitly to `make_launch_session`. Revocation affects subsequent dispatch,
not requests already authorized or streams already running. No new signed protocol
or client change is introduced.

## Pinned source provenance and reproducibility

Simple-Web-Server commit `546895a93a29062bb178367b46c7afb72da9881e`,
`server_http.hpp` SHA-256 after CRLF-to-LF normalization
`60381deda39ba9c9aa5b7d5b864b7e73370b3e0699b4fe0fafb43402efe37795`.
See `ServerBase::find_resource`, `write` (new keep-alive Session), and private
`Request::connection`. The protected dispatch hook is necessary because the pinned
dispatch is non-virtual and a derived same-named method cannot intercept it.

`cmake/dependencies/patches/simple-web-server-dispatch.patch` is a tracked minimal
patch. CMake validates the exact revision and original header bytes, copies headers
and upstream LICENSE into a build-tree overlay, and applies the tracked patch there.
It never edits the submodule or changes its gitlink. Each configure recreates the
overlay from pristine input; unexpected provenance or patch failure is fatal.
The added default hook returns true, preserving other HTTP servers' behavior; the
Sunshine HTTPS override denies absent resolver/identity. Upstream copyright and
license remain intact.

## OpenSSL verification semantics

Sources: [SSL_get_verify_result](https://docs.openssl.org/3.0/man3/SSL_get_verify_result/),
[SSL_CTX_set_verify](https://docs.openssl.org/3.0/man3/SSL_CTX_set_verify/),
[SSL_get_peer_certificate](https://docs.openssl.org/3.0/man3/SSL_get_peer_certificate/),
and pinned `src/crypto.cpp` / `src/nvhttp.cpp`.

The existing backend deliberately accepts verification callbacks so application
verification can use paired self-signed certificates. Consequently a raw
`SSL_get_verify_result() == X509_V_OK` requirement would break legitimate paired
clients; the result can retain a self-signed/clock error. Conversely X509_V_OK alone
does not establish presence of a certificate. The actual peer must be present,
handshake must be complete, and the authoritative application verifier must succeed.
Existing expiry/not-yet-valid tolerance and partial-chain behavior are preserved;
chain trust alone never substitutes for exact paired-leaf identity.

## Verification status

The parent independently built Sunshine and `test_sunshine` with
`BUILD_WERROR=ON` and `SUNSHINE_ENABLE_TRAY=OFF`, then reran 38 targeted native tests
on 2026-10-05: 10 loopback TLS tests and 28 mock-backed coordinator/boundary tests,
with zero failures. These are different scopes of verification; the 28 lifecycle
tests do not establish TLS security or real-driver behavior. The TLS fixtures use
bounded network operations and redirect persisted state into their fixture directory.
The parent then ran an expanded 69-test compatibility selection including existing
paired-client authorization and pairing suites: zero failures or errors. Two
successive CMake configurations regenerated the overlay successfully, and the
pinned Simple-Web-Server submodule remained clean. Upstream CMake deprecation
warnings are distinct from compiler warnings-as-errors and remain visible.

The launch-identity fixture invokes the production session constructor with valid
GameStream arguments and checks that spoofed query identity does not replace the
TLS principal. It does not execute an application or perform encoder probing.
Existing `/resume` behavior is preserved: the incoming request is authenticated
and its principal is recorded, but this patch does not check application/session
ownership against a previous client's certificate. That policy remains part of
subsequent permission/session integration and must not be claimed as implemented.

The overlay generator requires pristine pinned input and reconstructs build-tree
headers before patching. An independent bounded review found no must-fix issue in
its inspected scope; unsupported broader review claims were not adopted. This
checkpoint is not live Windows streaming, real-driver acceptance, an installer,
or cross-platform/OpenSSL-version verification.
The installed MSYS2 is `P:\msys64`, not the documented `C:\msys64`; commands used
the installed `msys2_shell.cmd -defterm -here -no-start -ucrt64 -c` entry point.
Primary-branch promotion remains separate from this experimental checkpoint.
No live host service, driver IOCTL, OS trust store, or hardware test is authorized.
