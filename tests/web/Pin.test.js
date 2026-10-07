import { mount, flushPromises } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div data-testid="navbar" />' },
}))

import Pin, {
  PERM_BITS,
  EDITABLE_PERM_MASK,
  permToFlags,
  flagsToMask,
  computeOutgoingPerm,
  isReductionOrDisable,
} from '../../src_assets/common/assets/web/Pin.vue'

function mockResponse(data, { status = 200, ok = status >= 200 && status < 300, isText = false } = {}) {
  return {
    ok,
    status,
    json: async () => (isText ? JSON.parse(data) : data),
    text: async () => (isText ? data : (typeof data === 'string' ? data : JSON.stringify(data))),
  }
}

function createClientRecord(overrides = {}) {
  return {
    uuid: 'client-uuid-001',
    name: 'Moonlight Deck',
    enabled: true,
    perm: 119480064, // crypto::PERM::_all
    ...overrides,
  }
}

async function mountPin({
  pairings = [],
  clients = [createClientRecord()],
  listStatus = 200,
  listData = null,
  csrfToken = 'csrf-token-12345',
} = {}) {
  const fetchMock = vi.fn(async (url, options = {}) => {
    if (url === './api/pin') {
      if (options.method === 'POST') {
        return mockResponse({ status: true })
      }
      if (options.method === 'DELETE') {
        return mockResponse({ status: true })
      }
      return mockResponse({ status: true, pairings })
    }

    if (url === './api/clients/list') {
      if (listStatus !== 200) {
        return mockResponse({ status: false, error: 'Unauthorized' }, { status: listStatus, ok: false })
      }
      return mockResponse(listData || { status: true, named_certs: clients })
    }

    if (url === './api/csrf-token') {
      return mockResponse({ status: true, csrf_token: csrfToken })
    }

    if (url === './api/clients/update') {
      return mockResponse({ status: true })
    }

    if (url === './api/clients/unpair') {
      return mockResponse({ status: true })
    }

    return mockResponse({ status: false, error: `Unhandled url: ${url}` }, { status: 404, ok: false })
  })

  vi.stubGlobal('fetch', fetchMock)

  const wrapper = mount(Pin, {
    global: {
      provide: {
        i18n: {
          t: key => key,
        },
      },
      mocks: {
        $t: key => key,
      },
      stubs: {
        Navbar: true,
      },
    },
  })

  await flushPromises()
  return { wrapper, fetchMock }
}

afterEach(() => {
  vi.unstubAllGlobals()
  vi.clearAllTimers()
})

describe('Client permissions arithmetic and bitmask logic', () => {
  it('correctly maps PERM bits and computes outgoing mask', () => {
    expect(PERM_BITS.input_controller).toBe(256)
    expect(PERM_BITS.input_touch).toBe(512)
    expect(PERM_BITS.input_pen).toBe(1024)
    expect(PERM_BITS.input_mouse).toBe(2048)
    expect(PERM_BITS.input_kbd).toBe(4096)
    expect(PERM_BITS.list).toBe(16777216)
    expect(PERM_BITS.view).toBe(33554432)
    expect(PERM_BITS.launch).toBe(67108864)

    expect(EDITABLE_PERM_MASK).toBe(117448448) // 0x07001F00

    const allFlags = permToFlags(119480064)
    expect(allFlags.controller).toBe(true)
    expect(allFlags.touch).toBe(true)
    expect(allFlags.pen).toBe(true)
    expect(allFlags.mouse).toBe(true)
    expect(allFlags.keyboard).toBe(true)
    expect(allFlags.list).toBe(true)
    expect(allFlags.view).toBe(true)
    expect(allFlags.launch).toBe(true)

    // Recomputed outgoing mask preserves all bits
    expect(computeOutgoingPerm(119480064, allFlags)).toBe(119480064)
  })

  it('preserves reserved and unimplemented operation bits', () => {
    // Clipboard set is 65536, file_upload is 262144
    const originalWithOps = 65536 | 262144 | PERM_BITS.input_controller
    const flags = permToFlags(originalWithOps)
    expect(flags.controller).toBe(true)
    expect(flags.mouse).toBe(false)

    // Uncheck controller
    flags.controller = false
    const outgoing = computeOutgoingPerm(originalWithOps, flags)
    // Controller bit is cleared, but clipboard_set (65536) and file_upload (262144) remain
    expect(outgoing & PERM_BITS.input_controller).toBe(0)
    expect(outgoing & 65536).toBe(65536)
    expect(outgoing & 262144).toBe(262144)
  })

  it('yields strict 0 when all editable bits are unchecked and non-editable bits are 0', () => {
    const zeroFlags = permToFlags(0)
    expect(computeOutgoingPerm(0, zeroFlags)).toBe(0)
  })

  it('detects permission reduction and disable accurately', () => {
    const baseline = {
      enabled: true,
      flags: permToFlags(119480064),
    }

    // Identical draft: no reduction
    expect(isReductionOrDisable(baseline, { enabled: true, flags: { ...baseline.flags } })).toBe(false)

    // Disabling client: reduction/disable
    expect(isReductionOrDisable(baseline, { enabled: false, flags: { ...baseline.flags } })).toBe(true)

    // Revoking view: reduction
    expect(isReductionOrDisable(baseline, { enabled: true, flags: { ...baseline.flags, view: false } })).toBe(true)

    // Revoking an input (e.g. keyboard): reduction
    expect(isReductionOrDisable(baseline, { enabled: true, flags: { ...baseline.flags, keyboard: false } })).toBe(true)

    // Adding permission from lower baseline: no reduction
    const lowerBaseline = {
      enabled: true,
      flags: { ...baseline.flags, mouse: false },
    }
    expect(isReductionOrDisable(lowerBaseline, { enabled: true, flags: { ...baseline.flags, mouse: true } })).toBe(false)
  })
})

describe('Pin.vue Authenticated Paired Clients Administration', () => {
  it('mounts and renders GET fixture shape correctly', async () => {
    const client = createClientRecord({
      uuid: 'uuid-alpha',
      name: 'Android Artemis',
      enabled: true,
      perm: 119480064,
    })

    const { wrapper } = await mountPin({
      clients: [client],
      pairings: [{ id: 'p-1', name: 'New Client', address: '192.168.1.100' }],
    })

    // Pairing workflow rendered
    expect(wrapper.find('#pairing-input').exists()).toBe(true)
    expect(wrapper.find('option[value="p-1"]').text()).toContain('New Client')

    // Paired client section rendered
    const clientCard = wrapper.find('[data-client-uuid="uuid-alpha"]')
    expect(clientCard.exists()).toBe(true)
    expect(clientCard.text()).toContain('Android Artemis')
    expect(clientCard.text()).toContain('uuid-alpha')
    expect(clientCard.text()).toContain('_common.enabled')

    // All checkboxes initialized to true
    expect(clientCard.find('#client-enabled-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-ctrl-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-touch-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-pen-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-mouse-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-kbd-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-view-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-list-uuid-alpha').element.checked).toBe(true)
    expect(clientCard.find('#perm-launch-uuid-alpha').element.checked).toBe(true)
  })

  it('sends exact outgoing mask, types, and current CSRF token on save', async () => {
    const client = createClientRecord({
      uuid: 'uuid-beta',
      name: 'Laptop Client',
      enabled: true,
      perm: 119480064,
    })

    const { wrapper, fetchMock } = await mountPin({
      clients: [client],
      csrfToken: 'secure-token-998877',
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-beta"]')

    // Uncheck mouse only (2048) -> expected outgoing perm = 119480064 - 2048 = 119478016
    const mouseInput = clientCard.find('#perm-mouse-uuid-beta')
    await mouseInput.setValue(false)

    // Save button should be enabled
    const saveBtn = clientCard.find('.btn-save-client')
    expect(saveBtn.attributes('disabled')).toBeUndefined()

    // Since revoking mouse is an input reduction, confirmation is triggered
    await saveBtn.trigger('click')
    expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)

    // Confirm save
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    // Verify GET /api/csrf-token was called
    const csrfCalls = fetchMock.mock.calls.filter(([url]) => url === './api/csrf-token')
    expect(csrfCalls.length).toBeGreaterThanOrEqual(1)

    // Verify POST /api/clients/update was called
    const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCalls.length).toBe(1)

    const [updateUrl, updateOptions] = updateCalls[0]
    expect(updateUrl).toBe('./api/clients/update')
    expect(updateOptions.method).toBe('POST')
    expect(updateOptions.headers['X-CSRF-Token']).toBe('secure-token-998877')
    expect(updateOptions.headers['Content-Type']).toBe('application/json')

    const body = JSON.parse(updateOptions.body)
    expect(body).toEqual({
      uuid: 'uuid-beta',
      enabled: true,
      perm: 119478016,
    })
    expect(typeof body.uuid).toBe('string')
    expect(typeof body.enabled).toBe('boolean')
    expect(typeof body.perm).toBe('number')
  })

  it('handles authenticated context clientfetch failures gracefully without login fallback', async () => {
    const { wrapper } = await mountPin({
      listStatus: 401,
    })

    expect(wrapper.find('.alert-danger').exists()).toBe(true)
    expect(wrapper.vm.clientsError).toContain('HTTP 401')
  })

  it('retains draft when server returns 200 with status: false', async () => {
    const client = createClientRecord({
      uuid: 'uuid-gamma',
      enabled: true,
      perm: 119480064,
    })

    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    // Override /api/clients/update to return 200 status false
    fetchMock.mockImplementation(async (url, opts = {}) => {
      if (url === './api/clients/update') {
        return mockResponse({ status: false, error: 'Database transaction lock failed' })
      }
      if (url === './api/csrf-token') {
        return mockResponse({ csrf_token: 'token' })
      }
      return mockResponse({ status: true })
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-gamma"]')
    await clientCard.find('#perm-touch-uuid-gamma').setValue(false)

    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    // Error is displayed
    expect(clientCard.text()).toContain('Database transaction lock failed')

    // Draft is retained and client remains dirty
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)
    expect(clientCard.find('#perm-touch-uuid-gamma').element.checked).toBe(false)
  })

  it('retains draft and shows accessible error on HTTP 403 CSRF error', async () => {
    const client = createClientRecord({ uuid: 'uuid-delta', enabled: true, perm: 119480064 })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    fetchMock.mockImplementation(async (url) => {
      if (url === './api/clients/update') {
        return mockResponse({ error: 'CSRF token forbidden' }, { status: 403, ok: false })
      }
      if (url === './api/csrf-token') return mockResponse({ csrf_token: 'invalid' })
      return mockResponse({ status: true })
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-delta"]')
    await clientCard.find('#perm-kbd-uuid-delta').setValue(false)

    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    expect(clientCard.text()).toContain('CSRF token forbidden')
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)
  })

  it('sends valid 0 mask without falling back when all permissions are cleared', async () => {
    const client = createClientRecord({
      uuid: 'uuid-zero',
      enabled: true,
      perm: 0,
    })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    const clientCard = wrapper.find('[data-client-uuid="uuid-zero"]')

    // Initially all checkboxes are false
    expect(clientCard.find('#perm-ctrl-uuid-zero').element.checked).toBe(false)

    // Check controller then uncheck controller to make it dirty
    await clientCard.find('#perm-ctrl-uuid-zero').setValue(true)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

    // Uncheck it back to 0, but toggle enabled to false to have a dirty change with 0 perm
    await clientCard.find('#perm-ctrl-uuid-zero').setValue(false)
    await clientCard.find('#client-enabled-uuid-zero').setValue(false)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCalls.length).toBe(1)
    const body = JSON.parse(updateCalls[0][1].body)
    expect(body.perm).toBe(0)
    expect(body.enabled).toBe(false)
  })

  it('preserves reserved existing bits when updating editable fields', async () => {
    // 0x1 (reserved bit 1) + 65536 (clipboard_set) + 256 (controller)
    const existingPerm = 1 | 65536 | 256
    const client = createClientRecord({
      uuid: 'uuid-reserved',
      perm: existingPerm,
    })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    const clientCard = wrapper.find('[data-client-uuid="uuid-reserved"]')

    // Uncheck controller
    await clientCard.find('#perm-ctrl-uuid-reserved').setValue(false)

    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCalls.length).toBe(1)
    const body = JSON.parse(updateCalls[0][1].body)
    // 1 and 65536 are preserved, controller (256) is cleared
    expect(body.perm).toBe(65537)
  })

  it('keeps later edits dirty when save was initiated with an earlier snapshot', async () => {
    const client = createClientRecord({
      uuid: 'uuid-inflight',
      enabled: true,
      perm: 119480064,
    })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    let resolveSavePromise
    fetchMock.mockImplementation(async (url) => {
      if (url === './api/clients/update') {
        return new Promise(resolve => {
          resolveSavePromise = () => resolve(mockResponse({ status: true }))
        })
      }
      if (url === './api/csrf-token') return mockResponse({ csrf_token: 'tok' })
      return mockResponse({ status: true })
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-inflight"]')

    // Edit 1: uncheck controller
    await clientCard.find('#perm-ctrl-uuid-inflight').setValue(false)

    // Trigger save
    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')

    // While save is in flight: Edit 2: uncheck touch
    await clientCard.find('#perm-touch-uuid-inflight').setValue(false)

    // Resolve the in-flight save
    resolveSavePromise()
    await flushPromises()

    // The client should STILL be dirty because touch was changed during flight and not saved yet!
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)
  })

  it('requires confirmation when disabling a client (enabledfalseconfirm)', async () => {
    const client = createClientRecord({
      uuid: 'uuid-toggle',
      enabled: true,
      perm: 119480064,
    })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    const clientCard = wrapper.find('[data-client-uuid="uuid-toggle"]')

    // Toggle enabled to false
    await clientCard.find('#client-enabled-uuid-toggle').setValue(false)

    // Disconnect warning appears
    expect(clientCard.text()).toContain('pin.stream_disconnect_warning')

    // Click Save
    await clientCard.find('.btn-save-client').trigger('click')

    // Confirmation banner is shown; NO POST /api/clients/update sent yet
    expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)
    const updateCallsBefore = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCallsBefore.length).toBe(0)

    // Cancel confirmation
    await clientCard.find('.btn-cancel-confirm').trigger('click')
    expect(clientCard.find('.btn-confirm-save').exists()).toBe(false)
    const updateCallsCancelled = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCallsCancelled.length).toBe(0)

    // Click Save again, then Confirm
    await clientCard.find('.btn-save-client').trigger('click')
    await clientCard.find('.btn-confirm-save').trigger('click')
    await flushPromises()

    const updateCallsConfirmed = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
    expect(updateCallsConfirmed.length).toBe(1)
    expect(JSON.parse(updateCallsConfirmed[0][1].body).enabled).toBe(false)
  })

  it('does not wipe dirty drafts when refreshing client list', async () => {
    const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
    const clientB = createClientRecord({ uuid: 'uuid-b', name: 'Client B', enabled: true, perm: 119480064 })

    const { wrapper } = await mountPin({ clients: [clientA, clientB] })

    // Make client A dirty
    const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
    await cardA.find('#perm-mouse-uuid-a').setValue(false)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

    // Trigger syncClientsList with fresh server data
    wrapper.vm.syncClientsList([
      { uuid: 'uuid-a', name: 'Client A Renamed', enabled: true, perm: 119480064 },
      { uuid: 'uuid-b', name: 'Client B', enabled: false, perm: 0 },
    ])

    // Client A should retain its dirty mouse=false draft!
    expect(wrapper.vm.clients[0].draft.mouse).toBe(false)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)
    expect(wrapper.vm.clients[0].name).toBe('Client A Renamed')

    // Clean client B synced to new server state
    expect(wrapper.vm.clients[1].draft.enabled).toBe(false)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[1])).toBe(false)
  })

  it('resets draft when Reset button is clicked', async () => {
    const client = createClientRecord({ uuid: 'uuid-reset', enabled: true, perm: 119480064 })
    const { wrapper } = await mountPin({ clients: [client] })

    const clientCard = wrapper.find('[data-client-uuid="uuid-reset"]')
    await clientCard.find('#perm-pen-uuid-reset').setValue(false)
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

    await clientCard.find('.btn-reset-draft').trigger('click')
    expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(false)
    expect(clientCard.find('#perm-pen-uuid-reset').element.checked).toBe(true)
  })

  it('unpairs client with CSRF token and refreshes client list', async () => {
    const client = createClientRecord({ uuid: 'uuid-unpair', name: 'Old Client' })
    const { wrapper, fetchMock } = await mountPin({
      clients: [client],
      csrfToken: 'csrf-unpair-token',
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-unpair"]')
    await clientCard.find('.btn-unpair-client').trigger('click')
    await flushPromises()

    const unpairCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair')
    expect(unpairCalls.length).toBe(1)
    const [unpairUrl, unpairOpts] = unpairCalls[0]
    expect(unpairUrl).toBe('./api/clients/unpair')
    expect(unpairOpts.headers['X-CSRF-Token']).toBe('csrf-unpair-token')
    expect(JSON.parse(unpairOpts.body)).toEqual({ uuid: 'uuid-unpair' })
  })

  it('handles unpair failure gracefully displaying accessible error', async () => {
    const client = createClientRecord({ uuid: 'uuid-unpair-fail', name: 'Faulty Client' })
    const { wrapper, fetchMock } = await mountPin({ clients: [client] })

    fetchMock.mockImplementation(async (url) => {
      if (url === './api/clients/unpair') {
        return mockResponse({ status: false, error: 'Unpair permission denied' })
      }
      if (url === './api/csrf-token') return mockResponse({ csrf_token: 'tok' })
      return mockResponse({ status: true })
    })

    const clientCard = wrapper.find('[data-client-uuid="uuid-unpair-fail"]')
    await clientCard.find('.btn-unpair-client').trigger('click')
    await flushPromises()

    expect(clientCard.text()).toContain('Unpair permission denied')
  })

  it('preserves existing PIN pairing workflow and cancellation', async () => {
    const { wrapper, fetchMock } = await mountPin({
      pairings: [{ id: 'req-99', name: 'SteamDeck', address: '192.168.1.44' }],
    })

    // Pairing workflow
    await wrapper.find('#pairing-input').setValue('req-99')
    await wrapper.find('#pin-input').setValue('1234')
    await wrapper.find('#name-input').setValue('Custom Name')
    await wrapper.find('#form').trigger('submit')
    await flushPromises()

    const postPinCalls = fetchMock.mock.calls.filter(([url, opts]) => url === './api/pin' && opts.method === 'POST')
    expect(postPinCalls.length).toBe(1)
    expect(JSON.parse(postPinCalls[0][1].body)).toEqual({
      pairing_id: 'req-99',
      pin: '1234',
      name: 'Custom Name',
    })

    // Pairing cancellation
    await wrapper.find('#pairing-input').setValue('req-99')
    await wrapper.find('button[title="pin.cancel_pairing"]').trigger('click')
    await flushPromises()

    const deletePinCalls = fetchMock.mock.calls.filter(([url, opts]) => url === './api/pin' && opts.method === 'DELETE')
    expect(deletePinCalls.length).toBe(1)
    expect(JSON.parse(deletePinCalls[0][1].body)).toEqual({
      pairing_id: 'req-99',
    })
  })
})
