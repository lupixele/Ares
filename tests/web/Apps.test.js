import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))

import Apps from '../../src_assets/common/assets/web/Apps.vue'

/**
 * Build a response shaped like the ones the page consumes.
 * @param {object} body Parsed JSON body.
 * @returns {object} The response.
 */
function jsonResponse(body) {
  return { ok: true, json: async () => body }
}

/**
 * Mount the applications page with every request it makes on creation answered.
 * @param {Array<object>} apps Applications the host reports.
 * @returns {Promise<import('@vue/test-utils').VueWrapper>} The mounted page, after its data has settled.
 */
async function mountApps(apps) {
  const fetchMock = vi.fn(async (url) => {
    if (url === './api/apps') {
      return jsonResponse({ apps })
    }
    if (url === './api/config') {
      return jsonResponse({ platform: 'windows', version: '2026.929.125923' })
    }
    return jsonResponse({ tag_name: 'v2026.929.125923', html_url: 'https://example.com/release' })
  })
  vi.stubGlobal('fetch', fetchMock)

  const wrapper = mount(Apps, {
    global: {
      mocks: { $t: key => key },
      stubs: { Navbar: true },
    },
  })
  await flushPromises()
  return wrapper
}

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('Applications page', () => {
  it('renders its header with the number of applications', async () => {
    const wrapper = await mountApps([{ name: 'Desktop', 'image-path': '' }, { name: 'Media', 'image-path': '' }])

    expect(wrapper.find('.ares-apps__header').exists()).toBe(true)
    expect(wrapper.get('.ares-apps__title').text()).toBe('apps.applications_title (2)')
    expect(wrapper.get('.ares-apps__desc').text()).toBe('apps.applications_desc')
    wrapper.unmount()
  })

  it('omits the count when the host has no applications', async () => {
    const wrapper = await mountApps([])

    expect(wrapper.get('.ares-apps__title').text()).toBe('apps.applications_title')
    wrapper.unmount()
  })
})
