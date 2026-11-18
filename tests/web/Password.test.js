import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

import Password from '../../src_assets/common/assets/web/Password.vue'

/**
 * Mount the page with the minimum the component needs: translation lookup and a stub navbar.
 * @returns {import('@vue/test-utils').VueWrapper} The mounted page.
 */
function mountPassword() {
  return mount(Password, {
    global: {
      mocks: { $t: key => key },
      stubs: { Navbar: true },
    },
  })
}

/**
 * Build a response shaped like the one `apiFetch` resolves to.
 * @param {object} body Parsed JSON body of the response.
 * @param {number} status HTTP status code.
 * @returns {object} The response.
 */
function jsonResponse(body, status = 200) {
  return { status, json: async () => body }
}

/**
 * Fill the form and submit it.
 * @param {import('@vue/test-utils').VueWrapper} wrapper Mounted page.
 * @param {object} [values] Values to type; sensible defaults for the rest.
 * @returns {Promise<void>} Resolves once the submit event has been handled.
 *
 * @note Settling is left to the caller: `flushPromises` waits on a timeout, which never fires
 *       while fake timers are installed.
 */
async function submitForm(wrapper, values = {}) {
  await wrapper.find('#currentUsername').setValue(values.currentUsername ?? 'operator')
  await wrapper.find('#currentPassword').setValue(values.currentPassword ?? 'old-secret')
  await wrapper.find('#newUsername').setValue(values.newUsername ?? '')
  await wrapper.find('#newPassword').setValue(values.newPassword ?? 'new-secret')
  await wrapper.find('#confirmNewPassword').setValue(values.confirmNewPassword ?? 'new-secret')
  await wrapper.find('form').trigger('submit')
}

/**
 * Let promise continuations run without advancing the clock, so the delayed reload never fires.
 * @returns {Promise<void>} Resolves after the queued microtasks have run.
 */
async function settleMicrotasks() {
  for (let tick = 0; tick < 10; tick++) {
    await Promise.resolve()
  }
}

afterEach(() => {
  vi.clearAllTimers()
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('Password page', () => {
  it('labels every field it renders', () => {
    const wrapper = mountPassword()

    const fields = ['currentUsername', 'currentPassword', 'newUsername', 'newPassword', 'confirmNewPassword']
    for (const id of fields) {
      expect(wrapper.find(`#${id}`).exists()).toBe(true)
    }

    const labelled = wrapper.findAll('label').map(label => label.attributes('for'))
    expect(labelled).toEqual(fields)
  })

  it('sends the typed credentials and reports success', async () => {
    vi.useFakeTimers()
    const fetchMock = vi.fn(async () => jsonResponse({ status: true }))
    vi.stubGlobal('fetch', fetchMock)

    const wrapper = mountPassword()
    await submitForm(wrapper, { newUsername: 'operator-2' })

    expect(fetchMock).toHaveBeenCalledTimes(1)
    const [url, options] = fetchMock.mock.calls[0]
    expect(url).toBe('./api/password')
    expect(options.method).toBe('POST')
    expect(JSON.parse(options.body)).toEqual({
      currentUsername: 'operator',
      currentPassword: 'old-secret',
      newUsername: 'operator-2',
      newPassword: 'new-secret',
      confirmNewPassword: 'new-secret',
    })

    await settleMicrotasks()
    expect(wrapper.text()).toContain('password.success_msg')
    expect(wrapper.text()).not.toContain('_common.error')
  })

  it('shows the reason the host rejected the change', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ status: false, error: 'Invalid credentials' }))
    vi.stubGlobal('fetch', fetchMock)

    const wrapper = mountPassword()
    await submitForm(wrapper)
    await flushPromises()

    expect(wrapper.text()).toContain('Invalid credentials')
    expect(wrapper.find('.alert-danger').exists()).toBe(true)
    expect(wrapper.find('.alert-success').exists()).toBe(false)
  })

  it('reports a failed request as a server error', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({}, 500))
    vi.stubGlobal('fetch', fetchMock)

    const wrapper = mountPassword()
    await submitForm(wrapper)
    await flushPromises()

    expect(wrapper.text()).toContain('Internal Server Error')
    expect(wrapper.find('.alert-success').exists()).toBe(false)
  })
})
