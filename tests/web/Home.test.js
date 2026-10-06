import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/ResourceCard.vue', () => ({
  default: { template: '<div />' },
}))

import Home from '../../src_assets/common/assets/web/Home.vue'

async function mountHome(platform, { developmentVersion = true, licensed = true, serviceAvailable = true, gamepadDriver = 'virtualhid', permissions = [] } = {}) {
  vi.stubGlobal('fetch', vi.fn(async url => {
    if (url === './api/config') {
      return { json: async () => ({ platform, controller: 'enabled', gamepad_driver: gamepadDriver, version: '2026.927.1200' }) }
    }
    if (url === './api/virtual-input/status') {
      return {
        json: async () => ({
          virtualhid: { installed: true, development_version: developmentVersion, version_compatible: true },
          vigembus: { installed: false, version_compatible: false },
        }),
      }
    }
    if (url === './api/permissions') {
      return { json: async () => ({ permissions }) }
    }
    if (url === './api/virtual-input/license') {
      return { json: async () => ({ licensed, service_available: serviceAvailable }) }
    }
    if (url === './api/logs') {
      return { text: async () => '' }
    }
    if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
      return { json: async () => ({ tag_name: 'v2026.927.1200' }) }
    }
    if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') {
      return { json: async () => [{ tag_name: 'v2026.927.1200', prerelease: true }] }
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
  return wrapper
}

afterEach(() => {
  vi.unstubAllGlobals()
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

    expect(fetch).toHaveBeenCalledWith('./api/virtual-input/status')
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
        return { json: async () => ({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' }) }
      }
      if (url === './api/virtual-input/status') {
        return {
          json: async () => ({
            virtualhid: { installed: true, development_version: true, version_compatible: true },
            vigembus: { installed: false, version_compatible: false },
          }),
        }
      }
      if (url === './api/permissions') {
        return { json: async () => ({ permissions: [] }) }
      }
      if (url === './api/virtual-input/license') {
        return { json: async () => ({ licensed: true, service_available: true }) }
      }
      if (url === './api/logs') {
        return { text: async () => '' }
      }
      if (url === './api/clients/list') {
        return { json: async () => ({ status: true, named_certs: [{ name: 'TestDevice' }] }) }
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
        return { json: async () => ({ platform: 'windows', controller: 'enabled', gamepad_driver: 'virtualhid', version: '2026.927.1200' }) }
      }
      if (url === './api/virtual-input/status') {
        return {
          json: async () => ({
            virtualhid: { installed: true, development_version: false, version_compatible: true },
            vigembus: { installed: false, version_compatible: false },
          }),
        }
      }
      if (url === './api/permissions') {
        return { json: async () => ({ permissions: [] }) }
      }
      if (url === './api/virtual-input/license') {
        return { json: async () => ({ licensed: true, service_available: true }) }
      }
      if (url === './api/logs') {
        return { text: async () => '' }
      }
      if (url === './api/clients/list') {
        return { json: async () => ({ status: true, named_certs: [] }) }
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases/latest') {
        return { json: async () => ({ tag_name: 'v2026.1005.0000', name: 'Sunshine v2026.1005', html_url: 'https://github.com/LizardByte/Sunshine/releases/tag/v2026.1005.0000', body: 'Release notes' }) }
      }
      if (url === 'https://api.github.com/repos/LizardByte/Sunshine/releases') {
        return { json: async () => [] }
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
})
