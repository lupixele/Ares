# Client permission administration — engineering checkpoint

The SPA Pin view retains the existing pending-PIN approval/cancellation workflow
and adds controls for paired-client enabled state, five input domains and three
action bits. It uses the backend's actual `GET /api/clients/list` and
`POST /api/clients/update` contracts. Clipboard, transfer and server-command bits
are preserved when editing, but are not exposed as supported capabilities.

## Mutation and reconciliation guarantees

- One mounted view serializes all mutations before CSRF-token retrieval. Buttons
  expose busy/disabled state. This does not coordinate separate browser tabs.
- Transport success and explicit JSON `status: true` are both required before
  accepting a client update or unpair. Failures retain drafts and visible errors.
- Disconnect confirmation captures the reviewed boolean fields. Later edits do
  not change that payload and remain dirty after authoritative reconciliation.
- Older list results cannot supersede newer completed results or a confirmed
  mutation. The post-save list updates the saved baseline without replacing newer
  edits or another client's dirty draft.
- A confirmed save followed by a failed refresh is reported separately; the UI
  does not invent backend-normalized fields it has not observed.
- Enabled state is independent of permission mask zero. New pairing defaults are
  still full access; no restricted-default policy change is included.

## Verification

The parent independently ran the full frontend suite: **120 tests passed across
10 files**, with coverage collection enabled. Vite production build passed.
Component tests exercise the actual view, deferred responses and DOM checkbox/
button actions, not a duplicate state model.

A Chrome/Playwright fixture against the built page passed at desktop 1440×900 and
mobile 390×844. It verifies the CSRF header, numeric mask/boolean types, failed
update draft retention, reviewed confirmation payload, busy actions, newer edits
remaining dirty, unpair rejection and absence of horizontal overflow. All API
requests are intercepted; no live host mutation or real streaming disconnect is
performed. Artifacts are under `cmake-build-ui-browser/pin-results.json` and
`pin-permissions-*.png`.

```powershell
npm test -- --maxWorkers=2
npm run build
$env:PLAYWRIGHT_PACKAGE = '<project-local node_modules\playwright>'
$env:BROWSER_EXECUTABLE = '<installed chrome.exe>' # optional with managed browsers
node tests/web/pin-browser-check.mjs
```

## Remaining limits

Cross-tab CSRF rotation, broader UI visual approval and real hardware streaming
acceptance remain separate. Permission reductions are supported by the prior
native teardown checkpoint, but this browser fixture does not prove physical
device cleanup. Existing host-global keyboard/mouse state is not per-client
isolated. No drivers, trust stores or services are installed by this feature.
