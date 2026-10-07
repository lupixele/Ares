/**
 * @file tests/web/pin-browser-check.mjs
 * @brief Check client permission edits against the built Vue view and intercepted APIs.
 * @details No administrative request reaches a real host. Artifacts remain in a build directory.
 */
import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { createRequire } from 'node:module'
import { mkdir, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const require = createRequire(import.meta.url)
const { chromium } = require(process.env.PLAYWRIGHT_PACKAGE ?? 'playwright')
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const output = path.join(root, 'cmake-build-ui-browser')
await mkdir(output, { recursive: true })
const server = spawn(process.execPath, [path.join(root, 'tests/web/preview_server.cjs')], {
  cwd: root, env: { ...process.env, PORT: '0' }, stdio: ['ignore', 'pipe', 'pipe'],
})
let browser
const errors = []
const updates = []
let updateMode = 'reject'
let releaseUpdate
let stored = { uuid: 'fixture-client-a', name: 'Fixture Android', enabled: true, perm: 119480064 }
try {
  const origin = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Fixture startup timed out')), 10000)
    let text = ''
    server.stdout.on('data', chunk => {
      text += chunk.toString()
      const match = text.match(/http:\/\/127\.0\.0\.1:\d+/)
      if (match) { clearTimeout(timer); resolve(match[0]) }
    })
    server.once('error', error => { clearTimeout(timer); reject(error) })
    server.once('exit', code => { clearTimeout(timer); reject(new Error(`Fixture exited: ${code}`)) })
  })
  browser = await chromium.launch({ headless: true, ...(process.env.BROWSER_EXECUTABLE ? { executablePath: process.env.BROWSER_EXECUTABLE } : {}) })
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } })
  await context.route('**/*', async route => {
    const request = route.request()
    const url = new URL(request.url())
    if (url.origin !== origin) {
      errors.push(`Unexpected external request: ${url.href}`)
      return route.abort('blockedbyclient')
    }
    if (!url.pathname.startsWith('/api/')) return route.continue()
    const method = request.method()
    if (method === 'GET' && url.pathname === '/api/configLocale') return route.fulfill({ json: { locale: 'en' } })
    if (method === 'GET' && url.pathname === '/api/pin') return route.fulfill({ json: { status: true, pending: [] } })
    if (method === 'GET' && url.pathname === '/api/csrf-token') return route.fulfill({ json: { csrf_token: 'fixture-csrf-token' } })
    if (method === 'GET' && url.pathname === '/api/clients/list') return route.fulfill({ json: { status: true, named_certs: [{ ...stored }] } })
    if (method === 'POST' && url.pathname === '/api/clients/update') {
      const payload = request.postDataJSON()
      assert.equal(request.headers()['x-csrf-token'], 'fixture-csrf-token')
      assert.equal(typeof payload.perm, 'number')
      assert.equal(typeof payload.enabled, 'boolean')
      updates.push(payload)
      if (updateMode === 'reject') return route.fulfill({ json: { status: false, error: 'Fixture rejected update' } })
      await new Promise(resolve => { releaseUpdate = resolve })
      stored = { ...stored, enabled: payload.enabled, perm: payload.perm }
      return route.fulfill({ json: { status: true } })
    }
    if (method === 'POST' && url.pathname === '/api/clients/unpair') return route.fulfill({ json: { status: false, error: 'Fixture rejected unpair' } })
    errors.push(`Unexpected API: ${method} ${url.pathname}`)
    return route.abort('blockedbyclient')
  })
  const page = await context.newPage()
  page.on('pageerror', error => errors.push(error.message))
  page.on('dialog', dialog => dialog.accept())
  await page.goto(`${origin}/pin`)
  const card = page.locator('[data-client-uuid="fixture-client-a"]')
  await card.waitFor()
  await page.screenshot({ path: path.join(output, 'pin-permissions-desktop.png'), fullPage: true })
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1))

  // A rejected revocation must preserve both its draft and dirty state.
  await card.locator('#perm-mouse-fixture-client-a').uncheck()
  await card.locator('.btn-save-client').click()
  assert.equal(updates.length, 0)
  await card.locator('.btn-confirm-save').click()
  await card.getByText('Fixture rejected update', { exact: true }).waitFor()
  assert.equal(await card.locator('#perm-mouse-fixture-client-a').isChecked(), false)
  assert.ok(await card.getByText('Unsaved changes', { exact: true }).isVisible())
  assert.equal(updates[0].perm, 119480064 & ~2048)

  // Confirmation must submit the reviewed snapshot, not subsequent checkbox edits.
  updateMode = 'accept'
  await card.locator('.btn-save-client').click()
  await card.locator('#perm-touch-fixture-client-a').uncheck()
  await card.locator('.btn-confirm-save').click()
  await page.waitForFunction(() => document.querySelector('.btn-save-client')?.disabled === true)
  assert.equal(await card.locator('.btn-unpair-client').isDisabled(), true)
  const updateDeadline = Date.now() + 5000
  while (!releaseUpdate && Date.now() < updateDeadline) await new Promise(resolve => setTimeout(resolve, 10))
  assert.ok(releaseUpdate, 'Intercepted update did not arrive within its test deadline')
  assert.equal(updates[1].perm, 119480064 & ~2048)
  releaseUpdate()
  await card.locator('.alert-success').waitFor()
  assert.equal(await card.locator('#perm-touch-fixture-client-a').isChecked(), false)
  assert.ok(await card.getByText('Unsaved changes', { exact: true }).isVisible())
  assert.equal(stored.perm & 512, 512)

  await card.locator('.btn-unpair-client').click()
  await card.getByText('Fixture rejected unpair', { exact: true }).waitFor()
  assert.equal(await card.count(), 1)
  await page.setViewportSize({ width: 390, height: 844 })
  assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1))
  await page.screenshot({ path: path.join(output, 'pin-permissions-mobile.png'), fullPage: true })
  assert.deepEqual(errors, [])
  await writeFile(path.join(output, 'pin-results.json'), JSON.stringify({ fixtureOnly: true, updates, stored, errors, checks: ['CSRF header', 'failed update retains draft', 'reviewed confirmation snapshot', 'busy actions', 'newer edit remains dirty', 'unpair rejection', 'desktop/mobile overflow'] }, null, 2))
  console.log('Pin browser mutation fixture passed; no live host mutations were performed.')
} finally {
  releaseUpdate?.()
  await browser?.close()
  server.kill()
}
