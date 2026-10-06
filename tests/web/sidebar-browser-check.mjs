/**
 * @file tests/web/sidebar-browser-check.mjs
 * @brief Check the built SPA shell against intercepted, read-only fixtures.
 * @details PLAYWRIGHT_PACKAGE may reference an existing project-local Playwright installation.
 * No request reaches an Ares backend or external service. Results remain in a build directory.
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
const failures = []
const observed = []
try {
  const origin = await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('Fixture startup timed out')), 10000)
    let buffer = ''
    server.stdout.on('data', (chunk) => {
      buffer += chunk.toString()
      const match = buffer.match(/http:\/\/127\.0\.0\.1:\d+/)
      if (match) { clearTimeout(timer); resolve(match[0]) }
    })
    server.once('error', (error) => { clearTimeout(timer); reject(error) })
    server.once('exit', (code) => { clearTimeout(timer); reject(new Error(`Fixture exited: ${code}`)) })
  })
  browser = await chromium.launch({ headless: true, ...(process.env.BROWSER_EXECUTABLE ? { executablePath: process.env.BROWSER_EXECUTABLE } : {}) })
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } })
  const page = await context.newPage()
  page.on('pageerror', (error) => failures.push(error.message))
  page.on('console', (message) => {
    if (message.type() === 'error') failures.push(`Console error: ${message.text()}`)
  })
  const release = { tag_name: 'v2026.929.125923', name: 'Fixture release', prerelease: false, body: '', assets: [] }
  const preRelease = { ...release, tag_name: 'v2026.1001.120000', name: 'Fixture prerelease', prerelease: true }
  const fixtures = new Map([
    ['/api/configLocale', { locale: 'en' }],
    ['/api/config', { platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.929.125923', locale: 'en' }],
    ['/api/virtual-input/status', { virtualhid: { installed: true, development_version: false, version_compatible: true }, vigembus: { installed: false } }],
    ['/api/virtual-input/license', { licensed: true, state: 'licensed', service_available: true }],
    ['/api/permissions', { permissions: [] }],
    ['/api/clients/list', { status: true, named_certs: [] }],
  ])
  await context.route('**/*', async (route) => {
    const request = route.request()
    const url = new URL(request.url())
    if (url.origin !== origin) {
      if (request.method() === 'GET' && url.origin === 'https://api.github.com' && url.pathname.startsWith('/repos/LizardByte/Sunshine/releases')) {
        return route.fulfill({ json: url.pathname.endsWith('/latest') ? release : [preRelease, release] })
      }
      failures.push(`Blocked external request: ${request.url()}`)
      return route.abort('blockedbyclient')
    }
    observed.push(`${request.method()} ${url.pathname}`)
    if (url.pathname.startsWith('/api/')) {
      if (request.method() !== 'GET') {
        failures.push(`Unexpected mutation: ${request.method()} ${url.pathname}`)
        return route.abort('blockedbyclient')
      }
      if (url.pathname === '/api/logs') return route.fulfill({ contentType: 'text/plain', body: '' })
      if (fixtures.has(url.pathname)) return route.fulfill({ json: fixtures.get(url.pathname) })
      failures.push(`Unhandled API fixture: ${url.pathname}`)
      return route.fulfill({ status: 501, json: { status: false } })
    }
    return route.continue()
  })
  await page.goto(origin)
  await page.locator('.ares-sidebar').waitFor()
  await page.evaluate(() => localStorage.removeItem('ares-sidebar-collapsed'))
  await page.reload()
  await page.locator('.ares-sidebar').waitFor()
  await page.waitForTimeout(300)
  const desktop = await page.evaluate(() => ({
    viewport: innerWidth, scrollWidth: document.documentElement.scrollWidth,
    sidebar: document.querySelector('.ares-sidebar').getBoundingClientRect().width,
    content: document.querySelector('#content').getBoundingClientRect().toJSON(),
    logoLoaded: document.querySelector('.ares-sidebar__logo').naturalWidth > 0,
  }))
  assert.equal(desktop.logoLoaded, true)
  assert.ok(desktop.scrollWidth <= desktop.viewport + 1, JSON.stringify(desktop))
  await page.screenshot({ path: path.join(output, 'sidebar-desktop.png'), fullPage: true })

  await page.locator('.ares-sidebar__collapse-btn').click()
  assert.equal(await page.evaluate(() => localStorage.getItem('ares-sidebar-collapsed')), 'true')
  await page.setViewportSize({ width: 390, height: 844 })
  await page.locator('.ares-sidebar__mobile-trigger').click()
  await page.waitForTimeout(300)
  assert.equal(await page.locator('#content').getAttribute('inert'), '')
  assert.equal(await page.evaluate(() => document.body.style.overflow), 'hidden')
  const mobile = await page.evaluate(() => ({
    viewport: innerWidth, scrollWidth: document.documentElement.scrollWidth,
    labelsVisible: [...document.querySelectorAll('.ares-sidebar__nav .ares-sidebar__label')].every((node) => getComputedStyle(node).display !== 'none'),
  }))
  assert.equal(mobile.labelsVisible, true)
  assert.ok(mobile.scrollWidth <= mobile.viewport + 1)
  await page.locator('#bd-theme').focus()
  await page.keyboard.press('Enter')
  await page.locator('.theme-menu.show').waitFor()
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('.theme-menu.show').count(), 0)
  assert.equal(await page.locator('.ares-sidebar-backdrop').count(), 1)
  await page.screenshot({ path: path.join(output, 'sidebar-mobile.png') })
  await page.keyboard.press('Escape')
  assert.equal(await page.locator('.ares-sidebar-backdrop').count(), 0)
  assert.equal(await page.locator('#content').getAttribute('inert'), null)
  assert.equal(await page.locator('.ares-sidebar__mobile-trigger').evaluate((node) => node === document.activeElement), true)
  await page.goto(`${origin}/welcome`)
  await page.locator('.ares-sidebar').waitFor({ state: 'detached' })
  assert.equal(await page.evaluate(() => document.body.classList.contains('has-ares-sidebar')), false)
  assert.deepEqual(failures, [])
  await writeFile(path.join(output, 'results.json'), JSON.stringify({ desktop, mobile, failures, observed, fixtureOnly: true }, null, 2))
  console.log('SPA sidebar desktop/mobile fixture checks passed; no live backend acceptance claimed.')
} finally {
  await browser?.close()
  server.kill()
}
