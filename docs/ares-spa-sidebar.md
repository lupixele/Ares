# Ares navigation adaptation — experimental SPA checkpoint

The approved icon-rail sidebar from the Apollo-based Ares checkout is adapted to
Sunshine's Vue SPA. Existing authenticated views still mount `Navbar.vue`; its
wrapper mounts the sidebar, which includes the existing notification component
exactly once. No second shell is mounted around `RouterView`.

## Preserved behavior

- Existing Vue Router paths, including Featured Apps, Password and Logout.
- BasicAuth logout probe followed by hard navigation to `/logout`, as upstream
  previously implemented. A client-side router transition is not substituted.
- Existing theme menu, API/CSRF utilities, notifications and page diagnostics.
- Welcome/Logout retain `NavbarSimple` and no authenticated sidebar/content offset.
- Keyboard-focusable theme trigger, mobile focus trap, layered Escape, scroll
  locking and restoration of pre-existing inert/scroll state on unmount.

The Apollo logo is retained as a temporary reference asset, not represented as
new Ares artwork. Home still contains upstream Sunshine wording and release logic;
overview branding and the remaining screen redesigns are subsequent slices.

## Verification

The parent ran the full Vitest suite with coverage enabled and a production Vite
build. Coverage output is measured, not a claim of 100% behavior coverage. The
read-only browser fixture checks the actual built SPA at 1440×900 and 390×844,
intercepts every API/external request, rejects unexpected mutations/routes, and
records console/runtime errors. It checks logo delivery, overflow, collapse
storage, mobile labels, keyboard theme menu, two-stage Escape, focus return and
pre-auth isolation. BasicAuth hard-navigation callbacks have direct unit tests;
the fixture does not establish real browser credential-cache behavior against a
live authenticated host.

Commands from the isolated worktree:

```powershell
npm test -- --maxWorkers=2
npm run build
# Optional browser fixture: use a project-local Playwright installation.
$env:PLAYWRIGHT_PACKAGE = '<project-local node_modules\playwright>'
$env:BROWSER_EXECUTABLE = '<installed chrome.exe>' # omit with Playwright-managed browsers
node tests/web/sidebar-browser-check.mjs
```

Screenshots and JSON observations are emitted only to `cmake-build-ui-browser/`.
The preview binds an ephemeral loopback port and is stopped by the browser harness.
No driver, certificate store, streaming service or personal browser profile is used.

This is shell/UI fixture acceptance, not native hardware streaming acceptance or
completion of Ares/Athena UI modernization.
