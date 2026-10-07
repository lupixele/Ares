import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/ResourceCard.vue', () => ({
  default: { template: '<div />' },
}))

import Home from '../../src_assets/common/assets/web/Home.vue'

function mockResponse(data, { status = 200, ok = status >= 200 && status < 300, isText = false } = {}) {
  return {
    ok,
    status,
    json: async () => (isText ? JSON.parse(data) : data),
    text: async () => (isText ? data : (typeof data === 'string' ? data : JSON.stringify(data))),
  }
}

async function mountHome(platform, { developmentVersion = true, licensed = true, serviceAvailable = true, gamepadDriver = 'virtualhid', permissions = [] } = {}) {
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/config') {
      return mockResponse({ platform, controller: 'enabled', gamepad_driver: gamepadDriver, version: '2026.927.1200' })
    }
    if (url === './api/virtual-input/status') {
      return mockResponse({
        virtualhid: { installed: true, development_version: developmentVersion, version_compatible: true },
        vigembus: { installed: false, version_compatible: false },
      })
    }
    if (url === './api/permissions') {
      return mockResponse({ permissions })
    }
    if (url === './api/virtual-input/license') {
      return mockResponse({ licensed, service_available: serviceAvailable })
    }
    if (url === './api/logs') {
      return mockResponse('', { isText: true })
    }
    if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
      return mockResponse({ tag_name: 'v2026.927.1200', html_url: 'https://github.com/LizardByte/Sunshine/releases/tag/v2026.927.1200' })
    }
    if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') {
      return mockResponse([{ tag_name: 'v2026.927.1200', prerelease: true, html_url: 'https://github.com/LizardByte/Sunshine/releases/tag/v2026.927.1200' }])
    }
    throw new Error(`Unexpected fetch: ${url}`)
  }))

  const wrapper = mount(Home, {
    global: {
      mocks: { $t: key => key },
      stubs: { RouterLink: true },
    },
  })
  await flushPromises()
  // Local diagnostics are independently launched after configuration resolves.
  // Flush their nested safeFetch/body promises before mutating fixture state.
  await flushPromises()
  return wrapper
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('Overview diagnostic evidence boundaries', () => {
  it('does not claim macOS readiness when license evidence is missing', async () => {
    const wrapper = await mountHome('macos')
    await wrapper.setData({ virtualhidLicense: null })
    expect(wrapper.vm.driverStatusState).toBe('unknown')
    wrapper.vm.virtualhidLicense = {}
    await wrapper.vm.$nextTick()
    expect(wrapper.vm.driverStatusState).toBe('unavailable')
    wrapper.unmount()
  })

  it('does not claim Windows readiness from an empty license response', async () => {
    const wrapper = await mountHome('windows')
    wrapper.vm.virtualhidLicense = {}
    await wrapper.vm.$nextTick()
    expect(wrapper.vm.driverStatusState).toBe('unavailable')
    wrapper.unmount()
  })

  it('keeps readiness unknown until permissions have been assessed', async () => {
    const wrapper = await mountHome('windows')
    await wrapper.setData({ permissionsLoading: true })
    expect(wrapper.vm.driverStatusState).toBe('unknown')
    wrapper.unmount()
  })

  it('rejects a successful HTTP response that is not configuration', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => url === './api/config'
      ? mockResponse({ status: false, message: 'Unavailable' })
      : mockResponse([])))
    const wrapper = mount(Home, { global: { mocks: { $t: key => key }, stubs: { RouterLink: true } } })
    await flushPromises()
    expect(wrapper.vm.configError).toBe(true)
    expect(wrapper.vm.driverStatusState).toBe('offline')
    wrapper.unmount()
  })
})

describe('development broker home notice', () => {
  it('links to permission controls when macOS is missing required access', async () => {
    const wrapper = await mountHome('macos', { permissions: [
      { id: 'screen_recording', status: 'denied', required: true, verifiable: true },
      { id: 'notifications', status: 'denied', required: false, verifiable: true },
      { id: 'local_network', status: 'on_use', required: true, verifiable: false },
    ] })
    expect(wrapper.text()).toContain('index.permissions_missing_title')
    expect(wrapper.findAll('.alert-warning a, .alert-warning router-link-stub').length).toBeGreaterThan(0)
    wrapper.unmount()
  })

  it('links to permission guidance when Windows cannot access its config directory', async () => {
    const wrapper = await mountHome('windows', { permissions: [
      { id: 'config_directory', status: 'denied', required: true, verifiable: true },
    ] })
    expect(wrapper.text()).toContain('index.permissions_missing_title')
    wrapper.unmount()
  })

  it.each(['windows', 'macos'])('shows the development card on %s', async platform => {
    const wrapper = await mountHome(platform)

    expect(fetch).toHaveBeenCalledWith('./api/virtual-input/status', expect.anything())
    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_development_title')
    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_development_desc')
    wrapper.unmount()
  })

  it('prioritizes a macOS license warning over the development card', async () => {
    const wrapper = await mountHome('macos', { licensed: false })

    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_macos_license_title')
    expect(wrapper.get('.alert.my-4').text()).not.toContain('index.virtualhid_development_title')
    wrapper.unmount()
  })

  it('shows the macOS broker warning when the license service is unavailable', async () => {
    const wrapper = await mountHome('macos', { licensed: false, serviceAvailable: false })

    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_broker_unavailable_title')
    expect(wrapper.get('.alert.my-4').text()).not.toContain('index.virtualhid_development_title')
    wrapper.unmount()
  })

  it('does not show a development card for a stable macOS broker', async () => {
    const wrapper = await mountHome('macos', { developmentVersion: false })

    expect(wrapper.find('.alert.my-4').exists()).toBe(false)
    wrapper.unmount()
  })

  it('does not show a broker card when gamepads are disabled', async () => {
    const wrapper = await mountHome('macos', { gamepadDriver: 'none' })

    expect(wrapper.find('.alert.my-4').exists()).toBe(false)
    wrapper.unmount()
  })
})

describe('Ares overview states and upstream independence', () => {
  it('handles backend config failure gracefully without breaking the page', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        throw new Error('500 Internal Server Error')
      }
      if (url === './api/logs') {
        return { text: async () => '' }
      }
      if (url.startsWith('https://api.github.com/')) {
        return { json: async () => ({ tag_name: 'v2026.927.1200' }) }
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('index.config_offline')
    expect(wrapper.find('.ares-stat-row').exists()).toBe(true)
    wrapper.unmount()
  })

  it('keeps Ares product status independent when upstream GitHub API fails or is offline', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({
          virtualhid: { installed: true, development_version: true, version_compatible: true },
          vigembus: { installed: false, version_compatible: false },
        })
      }
      if (url === './api/permissions') {
        return mockResponse({ permissions: [] })
      }
      if (url === './api/virtual-input/license') {
        return mockResponse({ licensed: true, service_available: true })
      }
      if (url === './api/logs') {
        return mockResponse('', { isText: true })
      }
      if (url === './api/clients/list') {
        return mockResponse({ status: true, named_certs: [{ name: 'TestDevice' }] })
      }
      if (url.startsWith('https://api.github.com/')) {
        throw new Error('API Rate limit or network offline')
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    // Ares version & platform are intact
    expect(wrapper.text()).toContain('2026.927.1200')
    expect(wrapper.text()).toContain('Windows')
    expect(wrapper.text()).toContain('1 index.paired_clients')

    // Upstream baseline shows unavailable status
    expect(wrapper.text()).toContain('index.upstream_baseline_unavailable')

    // Broker alert is still processed independently
    expect(wrapper.get('.alert.my-4').text()).toContain('index.virtualhid_development_title')
    wrapper.unmount()
  })

  it('verifies client pairing link semantics and separate upstream release link labeling', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({
          virtualhid: { installed: true, development_version: false, version_compatible: true },
          vigembus: { installed: false, version_compatible: false },
        })
      }
      if (url === './api/permissions') {
        return mockResponse({ permissions: [] })
      }
      if (url === './api/virtual-input/license') {
        return mockResponse({ licensed: true, service_available: true })
      }
      if (url === './api/logs') {
        return mockResponse('', { isText: true })
      }
      if (url === './api/clients/list') {
        return mockResponse({ status: true, named_certs: [] })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return mockResponse({ tag_name: 'v2026.1005.0000', name: 'Sunshine v2026.1005', html_url: 'https://github.com/LizardByte/Sunshine/releases/tag/v2026.1005.0000', body: 'Release notes' })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') {
        return mockResponse([])
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    // Upstream update alert is present and explicitly labels upstream Sunshine (never claims Ares update)
    expect(wrapper.text()).toContain('index.upstream_new_stable')
    expect(wrapper.text()).toContain('index.upstream_note')

    // Upstream download link points to LizardByte release
    const downloadLink = wrapper.find('a[href="https://github.com/LizardByte/Sunshine/releases/tag/v2026.1005.0000"]')
    expect(downloadLink.exists()).toBe(true)
    expect(downloadLink.text()).toContain('index.upstream_download')

    // Pairing links exist pointing to /pin
    const pinLinks = wrapper.findAll('router-link-stub').filter(r => r.attributes('to') === '/pin')
    expect(pinLinks.length).toBeGreaterThan(0)
    wrapper.unmount()
  })

  it('sanitizes release notes: remote HTML tags cannot create DOM elements or execute inline event handlers and text is preserved', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url === './api/clients/list') return mockResponse({ status: true, named_certs: [] })
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return mockResponse({
          tag_name: 'v2026.1005.0000',
          name: 'Sunshine v2026.1005',
          html_url: 'https://github.com/LizardByte/Sunshine/releases/tag/v2026.1005.0000',
          body: 'Exploit test <img src="x" onerror="window.pwned=true" /> and <svg onload="window.pwned=true"><desc>safe</desc></svg>',
        })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') return mockResponse([])
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.vm.convertMarkdownToHtml).toBeUndefined()
    expect(wrapper.text()).toContain('Exploit test <img src="x" onerror="window.pwned=true" /> and <svg onload="window.pwned=true"><desc>safe</desc></svg>')
    expect(wrapper.find('img[onerror]').exists()).toBe(false)
    expect(wrapper.find('svg[onload]').exists()).toBe(false)
    expect(window.pwned).toBeUndefined()
    wrapper.unmount()
  })

  it('validates official GitHub release URL and falls back safely on untrusted/malicious URL', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url === './api/clients/list') return mockResponse({ status: true, named_certs: [] })
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return mockResponse({
          tag_name: 'v2026.1005.0000',
          name: 'Sunshine v2026.1005',
          html_url: 'javascript:alert(1)',
          body: 'Notes',
        })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') return mockResponse([])
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.find('a[href^="javascript:"]').exists()).toBe(false)
    const downloadLink = wrapper.find('a.btn-success')
    expect(downloadLink.attributes('href')).toBe('https://github.com/LizardByte/Sunshine/releases')
    wrapper.unmount()
  })

  it('renders local overview, logs, and alerts while remote GitHub promises are held', async () => {
    let resolveGithubLatest
    const heldGithubPromise = new Promise(resolve => {
      resolveGithubLatest = resolve
    })

    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('[2026-10-07 10:00:00.000]: Fatal: GPU initialization failed\n', { isText: true })
      if (url === './api/clients/list') return mockResponse({ status: true, named_certs: [{ name: 'TestClient' }] })
      if (url.startsWith('https://api.github.com/')) {
        return heldGithubPromise
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('2026.927.1200')
    expect(wrapper.text()).toContain('Windows')
    expect(wrapper.text()).toContain('1 index.paired_clients')
    expect(wrapper.text()).toContain('GPU initialization failed')
    expect(wrapper.text()).toContain('index.loading_latest')

    resolveGithubLatest(mockResponse({ tag_name: 'v2026.927.1200' }))
    await flushPromises()
    wrapper.unmount()
  })

  it('handles realistic non-OK HTTP status (status: 500) and displays offline error states', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ error: 'Database locked' }, { status: 500, ok: false })
      }
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url.startsWith('https://api.github.com/')) return mockResponse([])
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('index.config_offline')
    expect(wrapper.text()).not.toContain('index.stat_driver_ready')
    wrapper.unmount()
  })

  it('does not label driver as ready when permissions fail or driver is unavailable', async () => {
    const wrapper1 = await mountHome('windows', {
      permissions: [{ id: 'config_directory', status: 'denied', required: true, verifiable: true }],
    })
    expect(wrapper1.text()).not.toContain('index.stat_driver_ready')
    wrapper1.unmount()

    const wrapper2 = await mountHome('windows', {
      licensed: false,
      serviceAvailable: true,
    })
    expect(wrapper2.text()).not.toContain('index.stat_driver_ready')
    wrapper2.unmount()
  })

  it('displays not reported when client list fetch returns backend error rather than falling back to 0', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url === './api/clients/list') {
        return mockResponse({ error: 'Internal failure' }, { status: 500, ok: false })
      }
      if (url.startsWith('https://api.github.com/')) {
        return mockResponse({ tag_name: 'v2026.927.1200' })
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('index.client_count_not_reported')
    expect(wrapper.text()).not.toContain('0 index.paired_clients')
    wrapper.unmount()
  })

  it('permits render safely when version string is malformed or unknown', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: 'unknown-custom-hash' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url === './api/clients/list') return mockResponse({ status: true, named_certs: [] })
      if (url.startsWith('https://api.github.com/')) {
        return mockResponse({ tag_name: 'v2026.927.1200' })
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('unknown-custom-hash')
    wrapper.unmount()
  })

  it('correctly masks GitHub API errors like rate-limit 403 or 200 { message: "..." }', async () => {
    vi.stubGlobal('fetch', vi.fn(async url => {
      if (url === './api/config') {
        return mockResponse({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' })
      }
      if (url === './api/virtual-input/status') {
        return mockResponse({ virtualhid: { installed: true, version_compatible: true }, vigembus: { installed: false } })
      }
      if (url === './api/permissions') return mockResponse({ permissions: [] })
      if (url === './api/virtual-input/license') return mockResponse({ licensed: true, service_available: true })
      if (url === './api/logs') return mockResponse('', { isText: true })
      if (url === './api/clients/list') return mockResponse({ status: true, named_certs: [] })
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return mockResponse({ message: 'API rate limit exceeded for 127.0.0.1' }, { status: 403, ok: false })
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') {
        return mockResponse({ message: 'Not Found' }, { status: 200, ok: true })
      }
      throw new Error(`Unexpected fetch: ${url}`)
    }))

    const wrapper = mount(Home, {
      global: {
        mocks: { $t: key => key },
        stubs: { RouterLink: true },
      },
    })
    await flushPromises()

    expect(wrapper.text()).toContain('index.upstream_baseline_unavailable')
    expect(wrapper.text()).not.toContain('API rate limit exceeded')
    wrapper.unmount()
  })

  it('renders Athena development client quicklink alongside Artemis and Moonlight', async () => {
    const wrapper = await mountHome('windows')
    const athenaLink = wrapper.find('a[href="https://github.com/lupixele/Athena"]')
    expect(athenaLink.exists()).toBe(true)
    expect(athenaLink.text()).toContain('index.athena_client')

    const artemisLink = wrapper.find('a[href="https://github.com/ClassicOldSong/moonlight-android"]')
    expect(artemisLink.exists()).toBe(true)

    const moonlightLink = wrapper.find('a[href="https://moonlight-stream.org"]')
    expect(moonlightLink.exists()).toBe(true)
    wrapper.unmount()
  })

  it('does not label driver as ready when driver is unconfigured or not assessed', async () => {
    const wrapper = await mountHome('windows', { gamepadDriver: '' })
    expect(wrapper.text()).not.toContain('index.stat_driver_ready')
    wrapper.unmount()
  })

  it('does not falsely infer upstream baseline is current based on matching version numbers', async () => {
    const wrapper = await mountHome('windows')
    expect(wrapper.text()).not.toContain('index.upstream_baseline_current')
    wrapper.unmount()
  })
})
