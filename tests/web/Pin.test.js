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

  it.each([{}, { status: null }, { status: 'true' }])('requires explicit update success before accepting a baseline: %j', async body => {
    const { wrapper, fetchMock } = await mountPin()
    const original = fetchMock.getMockImplementation()
    fetchMock.mockImplementation(async (url, options) => url === './api/clients/update'
      ? mockResponse(body) : original(url, options))
    const client = wrapper.vm.clients[0]
    client.draft.controller = false
    await wrapper.vm.saveClient(client)
    expect(wrapper.vm.isDirty(client)).toBe(true)
    expect(client.error).toBeTruthy()
  })

  it('does not remove a client when unpair lacks explicit success', async () => {
    const { wrapper, fetchMock } = await mountPin()
    const original = fetchMock.getMockImplementation()
    fetchMock.mockImplementation(async (url, options) => url === './api/clients/unpair'
      ? mockResponse({}) : original(url, options))
    const client = wrapper.vm.clients[0]
    await wrapper.vm.unpairClient(client)
    expect(wrapper.vm.clients).toHaveLength(1)
    expect(client.error).toBeTruthy()
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

  describe('View-wide mutation guard and serialization', () => {
    function createDeferred() {
      let resolve
      let reject
      const promise = new Promise((res, rej) => {
        resolve = res
        reject = rej
      })
      return { promise, resolve, reject }
    }

    it('holds Save A response and blocks Save B and Unpair B without beginning a second mutation or token fetch', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const clientB = createClientRecord({ uuid: 'uuid-b', name: 'Client B', enabled: true, perm: 119480064 })

      const deferredSaveA = createDeferred()
      const { wrapper, fetchMock } = await mountPin({ clients: [clientA, clientB] })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/clients/update') {
          const body = JSON.parse(options.body)
          if (body.uuid === 'uuid-a') {
            return deferredSaveA.promise
          }
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      const cardB = wrapper.find('[data-client-uuid="uuid-b"]')

      // Make Client A dirty and trigger Save A via DOM actions
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      // Save A is currently in-flight
      expect(wrapper.vm.isMutating).toBe(true)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('true')
      expect(cardA.find('.btn-save-client').attributes('disabled')).toBeDefined()
      expect(cardB.find('.btn-save-client').attributes('disabled')).toBeDefined()
      expect(cardB.find('.btn-unpair-client').attributes('disabled')).toBeDefined()

      const csrfCallsBefore = fetchMock.mock.calls.filter(([url]) => url === './api/csrf-token').length
      const updateCallsBefore = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update').length
      const unpairCallsBefore = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair').length

      // Make Client B dirty in the DOM and attempt Save B and Unpair B via DOM
      await cardB.find('#perm-pen-uuid-b').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[1])).toBe(true)

      await cardB.find('.btn-save-client').trigger('click')
      await cardB.find('.btn-unpair-client').trigger('click')
      await flushPromises()

      // Neither a second mutation nor token fetch may begin
      const csrfCallsAfter = fetchMock.mock.calls.filter(([url]) => url === './api/csrf-token').length
      const updateCallsAfter = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update').length
      const unpairCallsAfter = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair').length

      expect(csrfCallsAfter).toBe(csrfCallsBefore)
      expect(updateCallsAfter).toBe(updateCallsBefore)
      expect(unpairCallsAfter).toBe(unpairCallsBefore)

      // Clean up deferred promise so it doesn't leak
      deferredSaveA.resolve(mockResponse({ status: true }))
      await flushPromises()
    })

    it('prevents PIN approval and cancellation while a client mutation is held', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const deferredSaveA = createDeferred()
      const { wrapper, fetchMock } = await mountPin({
        clients: [clientA],
        pairings: [{ id: 'req-pin-1', name: 'Deck', address: '192.168.1.50' }],
      })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/clients/update') {
          return deferredSaveA.promise
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(true)
      const sendButton = wrapper.find('button[type="submit"]')
      const cancelButton = wrapper.find('button[title="pin.cancel_pairing"]')
      expect(sendButton.attributes('disabled')).toBeDefined()
      expect(cancelButton.attributes('disabled')).toBeDefined()

      // Attempt PIN approval and cancellation through DOM
      await wrapper.find('#pin-input').setValue('5678')
      await wrapper.find('#name-input').setValue('My Deck')
      await wrapper.find('#form').trigger('submit')
      await cancelButton.trigger('click')
      await flushPromises()

      const pinPostCalls = fetchMock.mock.calls.filter(([url, opts]) => url === './api/pin' && opts.method === 'POST')
      const deletePinCalls = fetchMock.mock.calls.filter(([url, opts]) => url === './api/pin' && opts.method === 'DELETE')
      expect(pinPostCalls).toHaveLength(0)
      expect(deletePinCalls).toHaveLength(0)

      deferredSaveA.resolve(mockResponse({ status: true }))
      await flushPromises()
    })

    it('prevents client mutations while PIN approval is held', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const deferredPin = createDeferred()
      const { wrapper, fetchMock } = await mountPin({
        clients: [clientA],
        pairings: [{ id: 'req-pin-2', name: 'Deck 2', address: '192.168.1.51' }],
      })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/pin' && options.method === 'POST') {
          return deferredPin.promise
        }
        return original(url, options)
      })

      // Trigger PIN approval via DOM
      await wrapper.find('#pin-input').setValue('9999')
      await wrapper.find('#form').trigger('submit')
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(true)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('true')

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      expect(cardA.find('.btn-save-client').attributes('disabled')).toBeDefined()
      expect(cardA.find('.btn-unpair-client').attributes('disabled')).toBeDefined()

      // Attempt Save A and Unpair A while PIN approval is held
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-unpair-client').trigger('click')
      await flushPromises()

      const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
      const unpairCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair')
      expect(updateCalls).toHaveLength(0)
      expect(unpairCalls).toHaveLength(0)

      deferredPin.resolve(mockResponse({ status: true }))
      await flushPromises()
      expect(wrapper.vm.isMutating).toBe(false)
    })

    it('prevents client mutations while PIN cancellation is held', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const deferredPinCancel = createDeferred()
      const { wrapper, fetchMock } = await mountPin({
        clients: [clientA],
        pairings: [{ id: 'req-pin-cancel', name: 'Deck 3', address: '192.168.1.52' }],
      })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/pin' && options.method === 'DELETE') {
          return deferredPinCancel.promise
        }
        return original(url, options)
      })

      // Select pairing and click cancel button
      await wrapper.find('#pairing-input').setValue('req-pin-cancel')
      await wrapper.find('button[title="pin.cancel_pairing"]').trigger('click')
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(true)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('true')

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      expect(cardA.find('.btn-save-client').attributes('disabled')).toBeDefined()
      expect(cardA.find('.btn-unpair-client').attributes('disabled')).toBeDefined()

      // Attempt Save A and Unpair A while PIN cancellation is held
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-unpair-client').trigger('click')
      await flushPromises()

      const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
      const unpairCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair')
      expect(updateCalls).toHaveLength(0)
      expect(unpairCalls).toHaveLength(0)

      deferredPinCancel.resolve(mockResponse({ status: true }))
      await flushPromises()
      expect(wrapper.vm.isMutating).toBe(false)
    })

    it('releases guard when first request completes allowing the next action to proceed', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const clientB = createClientRecord({ uuid: 'uuid-b', name: 'Client B', enabled: true, perm: 119480064 })

      const deferredSaveA = createDeferred()
      const { wrapper, fetchMock } = await mountPin({ clients: [clientA, clientB] })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/clients/update') {
          const body = JSON.parse(options.body)
          if (body.uuid === 'uuid-a') {
            return deferredSaveA.promise
          }
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      const cardB = wrapper.find('[data-client-uuid="uuid-b"]')

      // Start Save A
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(true)

      // Release Save A
      deferredSaveA.resolve(mockResponse({ status: true }))
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(false)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('false')

      // Next action: Save B becomes available and proceeds
      await cardB.find('#perm-pen-uuid-b').setValue(false)
      expect(cardB.find('.btn-save-client').attributes('disabled')).toBeUndefined()

      await cardB.find('.btn-save-client').trigger('click')
      await cardB.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
      expect(updateCalls).toHaveLength(2)
      expect(JSON.parse(updateCalls[1][1].body).uuid).toBe('uuid-b')
    })

    it('releases guard and preserves drafts intact when the first request fails', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const deferredSaveA = createDeferred()
      const { wrapper, fetchMock } = await mountPin({ clients: [clientA] })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/clients/update') {
          return deferredSaveA.promise
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(true)

      // Fail Save A with an error
      deferredSaveA.reject(new Error('Network disconnected'))
      await flushPromises()

      // Guard released and accessible busy state cleared
      expect(wrapper.vm.isMutating).toBe(false)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('false')

      // Draft remains intact and dirty
      const clientInVm = wrapper.vm.clients[0]
      expect(wrapper.vm.isDirty(clientInVm)).toBe(true)
      expect(clientInVm.draft.pen).toBe(false)
      expect(cardA.find('#perm-pen-uuid-a').element.checked).toBe(false)
      expect(clientInVm.error).toBe('Network disconnected')

      // Next action is available again
      expect(cardA.find('.btn-save-client').attributes('disabled')).toBeUndefined()
    })

    it('acquires mutation guard before CSRF token fetch begins', async () => {
      const clientA = createClientRecord({ uuid: 'uuid-a', name: 'Client A', enabled: true, perm: 119480064 })
      const clientB = createClientRecord({ uuid: 'uuid-b', name: 'Client B', enabled: true, perm: 119480064 })
      const deferredCsrf = createDeferred()
      const { wrapper, fetchMock } = await mountPin({ clients: [clientA, clientB] })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options) => {
        if (url === './api/csrf-token') {
          return deferredCsrf.promise
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-a"]')
      const cardB = wrapper.find('[data-client-uuid="uuid-b"]')

      // Start Save A
      await cardA.find('#perm-pen-uuid-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      // While CSRF token fetch is pending, view guard is already held
      expect(wrapper.vm.isMutating).toBe(true)
      expect(wrapper.find('#content').attributes('aria-busy')).toBe('true')
      expect(cardB.find('.btn-unpair-client').attributes('disabled')).toBeDefined()

      // Attempt Unpair B while token fetch is held
      await cardB.find('.btn-unpair-client').trigger('click')
      await flushPromises()

      const unpairCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/unpair')
      expect(unpairCalls).toHaveLength(0)

      // Release CSRF
      deferredCsrf.resolve(mockResponse({ csrf_token: 'new-token' }))
      await flushPromises()

      expect(wrapper.vm.isMutating).toBe(false)
    })
  })

  describe('Client list request sequencing and authoritative post-save reconciliation', () => {
    function createDeferred() {
      let resolve
      let reject
      const promise = new Promise((res, rej) => {
        resolve = res
        reject = rej
      })
      return { promise, resolve, reject }
    }

    it('resolves two GET requests in reverse order; only the newest wins', async () => {
      const deferredFirst = createDeferred()
      const deferredSecond = createDeferred()
      let getCallCount = 0

      const { wrapper, fetchMock } = await mountPin({
        clients: [createClientRecord({ uuid: 'client-1', name: 'Initial Name' })],
      })

      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/list' && (options.method === 'GET' || !options.method)) {
          getCallCount++
          if (getCallCount === 1) {
            return deferredFirst.promise
          }
          if (getCallCount === 2) {
            return deferredSecond.promise
          }
        }
        return original(url, options)
      })

      // Dispatch request 1 and request 2
      const req1 = wrapper.vm.loadClients()
      const req2 = wrapper.vm.loadClients()

      // Resolve in reverse order: request 2 resolves first
      deferredSecond.resolve(mockResponse({
        status: true,
        named_certs: [createClientRecord({ uuid: 'client-1', name: 'Newest Name from Second GET' })],
      }))
      await req2
      await flushPromises()

      expect(wrapper.vm.clients[0].name).toBe('Newest Name from Second GET')
      expect(wrapper.find('[data-client-uuid="client-1"] h3').text()).toContain('Newest Name from Second GET')

      // Resolve request 1 second (older superseded request)
      deferredFirst.resolve(mockResponse({
        status: true,
        named_certs: [createClientRecord({ uuid: 'client-1', name: 'Stale Name from First GET' })],
      }))
      await req1
      await flushPromises()

      // The older response must be ignored; the newest response must remain
      expect(wrapper.vm.clients[0].name).toBe('Newest Name from Second GET')
      expect(wrapper.find('[data-client-uuid="client-1"] h3').text()).toContain('Newest Name from Second GET')
    })

    it('holds an old GET across a successful mutation; it cannot overwrite the accepted result', async () => {
      const deferredOldGet = createDeferred()
      const { wrapper, fetchMock } = await mountPin({
        clients: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: 119480064 })],
      })

      const original = fetchMock.getMockImplementation()
      let oldGetDispatched = false
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/list' && !oldGetDispatched) {
          oldGetDispatched = true
          return deferredOldGet.promise
        }
        return original(url, options)
      })

      // Dispatch old GET before mutation
      const oldGetPromise = wrapper.vm.loadClients()

      // Operator edits client-1 through DOM: uncheck controller
      const clientCard = wrapper.find('[data-client-uuid="client-1"]')
      await clientCard.find('#perm-ctrl-client-1').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

      // Post-save GET will return controller cleared (119479808)
      const updatedPerm = computeOutgoingPerm(119480064, wrapper.vm.clients[0].draft)
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          return mockResponse({ status: true })
        }
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: updatedPerm })],
          })
        }
        return original(url, options)
      })

      // Operator saves client-1 via DOM
      await clientCard.find('.btn-save-client').trigger('click')
      if (clientCard.find('.btn-confirm-save').exists()) {
        await clientCard.find('.btn-confirm-save').trigger('click')
      }
      await flushPromises()

      // Mutation succeeded and baseline was updated
      expect(wrapper.vm.clients[0].persisted.flags.controller).toBe(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(false)

      // Now resolve the old pre-mutation GET with old perm (controller: true)
      deferredOldGet.resolve(mockResponse({
        status: true,
        named_certs: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: 119480064 })],
      }))
      await oldGetPromise
      await flushPromises()

      // The pre-mutation GET must be ignored and must NOT overwrite the accepted baseline
      expect(wrapper.vm.clients[0].persisted.flags.controller).toBe(false)
      expect(wrapper.vm.clients[0].draft.controller).toBe(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(false)
    })

    it('backend normalizes the saved mask; its returned value becomes the baseline', async () => {
      const { wrapper, fetchMock } = await mountPin({
        clients: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: 119480064 })],
      })

      const clientCard = wrapper.find('[data-client-uuid="client-1"]')
      // Operator unchecks touch in DOM
      await clientCard.find('#perm-touch-client-1').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

      // Server normalizes the mask: strips touch (512) and also mouse (2048)
      const normalizedPerm = (119480064 & ~512 & ~2048) >>> 0
      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          return mockResponse({ status: true })
        }
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: normalizedPerm })],
          })
        }
        return original(url, options)
      })

      // Save via DOM
      await clientCard.find('.btn-save-client').trigger('click')
      if (clientCard.find('.btn-confirm-save').exists()) {
        await clientCard.find('.btn-confirm-save').trigger('click')
      }
      await flushPromises()

      const client = wrapper.vm.clients[0]
      // Backend-normalized value is now the persisted baseline
      expect(client.persisted.perm).toBe(normalizedPerm)
      expect(client.persisted.flags.touch).toBe(false)
      expect(client.persisted.flags.mouse).toBe(false)
      // Draft has accepted the normalized value
      expect(client.draft.mouse).toBe(false)
      expect(wrapper.vm.isDirty(client)).toBe(false)
    })

    it('edit a field while save is pending; the newer edit remains dirty after authoritative reconciliation', async () => {
      const deferredUpdate = createDeferred()
      const { wrapper, fetchMock } = await mountPin({
        clients: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: 119480064 })],
      })

      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          return deferredUpdate.promise
        }
        return original(url, options)
      })

      const clientCard = wrapper.find('[data-client-uuid="client-1"]')
      // Edit 1: uncheck controller
      await clientCard.find('#perm-ctrl-client-1').setValue(false)

      // Trigger save via DOM
      await clientCard.find('.btn-save-client').trigger('click')
      if (clientCard.find('.btn-confirm-save').exists()) {
        await clientCard.find('.btn-confirm-save').trigger('click')
      }
      await flushPromises()
      expect(wrapper.vm.isMutating).toBe(true)
      expect(wrapper.vm.clients[0].saving).toBe(true)

      // While save is pending in-flight: Edit 2: uncheck keyboard in DOM
      await clientCard.find('#perm-kbd-client-1').setValue(false)
      expect(wrapper.vm.clients[0].draft.keyboard).toBe(false)

      // Setup authoritative GET response for saved snapshot (controller is false, keyboard is true)
      const savedPerm = (119480064 & ~256) >>> 0
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: savedPerm })],
          })
        }
        return original(url, options)
      })

      // Resolve pending save
      deferredUpdate.resolve(mockResponse({ status: true }))
      await flushPromises()

      const client = wrapper.vm.clients[0]
      expect(wrapper.vm.isMutating).toBe(false)
      expect(client.saving).toBe(false)

      // Baseline reconciled from server: controller false, keyboard true
      expect(client.persisted.flags.controller).toBe(false)
      expect(client.persisted.flags.keyboard).toBe(true)

      // Draft preserved newer edit: keyboard false
      expect(client.draft.controller).toBe(false)
      expect(client.draft.keyboard).toBe(false)

      // Newer edit remains dirty!
      expect(wrapper.vm.isDirty(client)).toBe(true)
      expect(clientCard.find('.badge.bg-warning').exists()).toBe(true)
    })

    it('saving client A preserves client B dirty draft', async () => {
      const clientA = createClientRecord({ uuid: 'client-a', name: 'Client A', perm: 119480064 })
      const clientB = createClientRecord({ uuid: 'client-b', name: 'Client B', perm: 119480064 })

      const { wrapper, fetchMock } = await mountPin({
        clients: [clientA, clientB],
      })

      const cardA = wrapper.find('[data-client-uuid="client-a"]')
      const cardB = wrapper.find('[data-client-uuid="client-b"]')

      // Modify Client B via DOM (uncheck keyboard)
      await cardB.find('#perm-kbd-client-b').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[1])).toBe(true)

      // Modify Client A via DOM (uncheck controller)
      await cardA.find('#perm-ctrl-client-a').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(true)

      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          return mockResponse({ status: true })
        }
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [
              createClientRecord({ uuid: 'client-a', name: 'Client A', perm: (119480064 & ~256) >>> 0 }),
              createClientRecord({ uuid: 'client-b', name: 'Client B', perm: 119480064 }),
            ],
          })
        }
        return original(url, options)
      })

      // Save Client A
      await cardA.find('.btn-save-client').trigger('click')
      if (cardA.find('.btn-confirm-save').exists()) {
        await cardA.find('.btn-confirm-save').trigger('click')
      }
      await flushPromises()

      // Client A reconciled and clean
      expect(wrapper.vm.isDirty(wrapper.vm.clients[0])).toBe(false)

      // Client B dirty draft is preserved
      const clientBvm = wrapper.vm.clients[1]
      expect(clientBvm.draft.keyboard).toBe(false)
      expect(clientBvm.persisted.flags.keyboard).toBe(true)
      expect(wrapper.vm.isDirty(clientBvm)).toBe(true)
      expect(cardB.find('#perm-kbd-client-b').element.checked).toBe(false)
      expect(cardB.find('.badge.bg-warning').exists()).toBe(true)
    })

    it('confirmed save followed by GET failure reports save success plus refresh failure distinctly', async () => {
      const { wrapper, fetchMock } = await mountPin({
        clients: [createClientRecord({ uuid: 'client-1', name: 'Client 1', perm: 119480064 })],
      })

      const clientCard = wrapper.find('[data-client-uuid="client-1"]')
      await clientCard.find('#perm-ctrl-client-1').setValue(false)

      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          return mockResponse({ status: true })
        }
        if (url === './api/clients/list') {
          return mockResponse({ status: false, error: 'Reconciliation network fault' }, { status: 500, ok: false })
        }
        return original(url, options)
      })

      await clientCard.find('.btn-save-client').trigger('click')
      if (clientCard.find('.btn-confirm-save').exists()) {
        await clientCard.find('.btn-confirm-save').trigger('click')
      }
      await flushPromises()

      const client = wrapper.vm.clients[0]
      // Mutation is no longer in progress
      expect(wrapper.vm.isMutating).toBe(false)
      expect(client.saving).toBe(false)

      // Save was successful: client.error is not set
      expect(client.error).toBeNull()

      // Save success is reported
      expect(client.successMessage).toBe('pin.client_updated_success')
      expect(clientCard.find('.alert-success').exists()).toBe(true)
      expect(clientCard.find('.alert-success').text()).toContain('pin.client_updated_success')

      // Refresh failure is reported distinctly
      expect(client.refreshError || wrapper.vm.clientsError).toBeTruthy()
      expect(clientCard.find('.alert-refresh').exists()).toBe(true)
      expect(clientCard.find('.alert-refresh').text()).toContain('HTTP 500')
      expect(wrapper.find('.alert-danger').exists()).toBe(true)
      expect(wrapper.find('.alert-danger').text()).toContain('HTTP 500')

      // Unobserved server fields were NOT claimed as baseline
      expect(client.persisted.flags.controller).toBe(true)
      expect(wrapper.vm.isDirty(client)).toBe(true)
    })

    it('removing a successfully unpaired UUID does not discard other client drafts', async () => {
      const clientA = createClientRecord({ uuid: 'client-a', name: 'Client A', perm: 119480064 })
      const clientB = createClientRecord({ uuid: 'client-b', name: 'Client B', perm: 119480064 })

      const { wrapper, fetchMock } = await mountPin({
        clients: [clientA, clientB],
      })

      const cardA = wrapper.find('[data-client-uuid="client-a"]')
      const cardB = wrapper.find('[data-client-uuid="client-b"]')

      // Operator makes Client B dirty
      await cardB.find('#perm-touch-client-b').setValue(false)
      expect(wrapper.vm.isDirty(wrapper.vm.clients[1])).toBe(true)

      const original = fetchMock.getMockImplementation()
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/unpair') {
          return mockResponse({ status: true })
        }
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [createClientRecord({ uuid: 'client-b', name: 'Client B', perm: 119480064 })],
          })
        }
        return original(url, options)
      })

      // Unpair Client A via DOM
      await cardA.find('.btn-unpair-client').trigger('click')
      await flushPromises()

      // Client A is removed: only Client B remains
      expect(wrapper.vm.clients).toHaveLength(1)
      expect(wrapper.vm.clients[0].uuid).toBe('client-b')

      // Client B dirty draft is preserved
      const remainingClient = wrapper.vm.clients[0]
      expect(remainingClient.draft.touch).toBe(false)
      expect(remainingClient.persisted.flags.touch).toBe(true)
      expect(wrapper.vm.isDirty(remainingClient)).toBe(true)
      expect(wrapper.find('[data-client-uuid="client-b"] #perm-touch-client-b').element.checked).toBe(false)
    })
  })

  describe('Client-disconnect confirmation reviewed draft snapshot integrity', () => {
    function createDeferred() {
      let resolve, reject
      const promise = new Promise((res, rej) => {
        resolve = res
        reject = rej
      })
      return { promise, resolve, reject }
    }

    it('submits only the reviewed snapshot when draft is modified while confirmation is visible, keeping later edits dirty', async () => {
      const client = createClientRecord({
        uuid: 'uuid-snapshot-1',
        name: 'Client Snapshot 1',
        enabled: true,
        perm: 119480064,
      })
      const { wrapper, fetchMock } = await mountPin({ clients: [client] })
      const original = fetchMock.getMockImplementation()

      let updatePayload = null
      const deferredUpdate = createDeferred()

      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          updatePayload = JSON.parse(options.body)
          return deferredUpdate.promise
        }
        if (url === './api/clients/list') {
          return mockResponse({
            status: true,
            named_certs: [createClientRecord({ uuid: 'uuid-snapshot-1', name: 'Client Snapshot 1', perm: 119478016 })],
          })
        }
        return original(url, options)
      })

      const clientCard = wrapper.find('[data-client-uuid="uuid-snapshot-1"]')

      // Step 1: Revoke mouse via DOM
      await clientCard.find('#perm-mouse-uuid-snapshot-1').setValue(false)
      expect(wrapper.vm.clients[0].draft.mouse).toBe(false)
      expect(wrapper.vm.clients[0].draft.touch).toBe(true)

      // Step 2: Open confirmation via DOM
      await clientCard.find('.btn-save-client').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)
      expect(wrapper.vm.clients[0].confirmingSave).toBe(true)

      // Step 3: Change touch while confirmation is visible via DOM
      await clientCard.find('#perm-touch-uuid-snapshot-1').setValue(false)
      expect(wrapper.vm.clients[0].draft.touch).toBe(false)

      // Step 4: Confirm save via DOM
      await clientCard.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      // Step 5: Verify POST /api/clients/update received only reviewed snapshot (mouse revoked, touch enabled)
      expect(updatePayload).toEqual({
        uuid: 'uuid-snapshot-1',
        enabled: true,
        perm: 119478016, // mouse revoked (2048 subtracted), touch bit (512) preserved
      })

      // Resolve update request
      deferredUpdate.resolve(mockResponse({ status: true }))
      await flushPromises()

      // Step 6: Reconciliation preserves later touch edit as dirty
      const clientModel = wrapper.vm.clients[0]
      expect(wrapper.vm.isMutating).toBe(false)
      expect(clientModel.saving).toBe(false)
      expect(clientModel.persisted.flags.mouse).toBe(false)
      expect(clientModel.persisted.flags.touch).toBe(true)
      expect(clientModel.draft.mouse).toBe(false)
      expect(clientModel.draft.touch).toBe(false)
      expect(wrapper.vm.isDirty(clientModel)).toBe(true)
      expect(clientCard.find('#perm-touch-uuid-snapshot-1').element.checked).toBe(false)
    })

    it('uses newly reviewed snapshot after operator cancels confirmation, edits draft, and reopens confirmation', async () => {
      const client = createClientRecord({
        uuid: 'uuid-cancel-reopen',
        name: 'Client Cancel Reopen',
        enabled: true,
        perm: 119480064,
      })
      const { wrapper, fetchMock } = await mountPin({ clients: [client] })
      const original = fetchMock.getMockImplementation()

      let updatePayload = null
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          updatePayload = JSON.parse(options.body)
          return mockResponse({ status: true })
        }
        return original(url, options)
      })

      const clientCard = wrapper.find('[data-client-uuid="uuid-cancel-reopen"]')

      // Step 1: Revoke mouse via DOM and open confirmation
      await clientCard.find('#perm-mouse-uuid-cancel-reopen').setValue(false)
      await clientCard.find('.btn-save-client').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)

      // Step 2: Cancel confirmation via DOM
      await clientCard.find('.btn-cancel-confirm').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(false)
      expect(wrapper.vm.clients[0].confirmingSave).toBe(false)
      expect(wrapper.vm.clients[0].pendingSnapshot).toBeNull()

      // No network mutation occurred on cancel
      const updateCalls = fetchMock.mock.calls.filter(([url]) => url === './api/clients/update')
      expect(updateCalls).toHaveLength(0)

      // Step 3: Change draft again via DOM (re-enable mouse, revoke keyboard)
      await clientCard.find('#perm-mouse-uuid-cancel-reopen').setValue(true)
      await clientCard.find('#perm-kbd-uuid-cancel-reopen').setValue(false)

      // Step 4: Reopen confirmation
      await clientCard.find('.btn-save-client').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)

      // Step 5: Confirm save
      await clientCard.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      // Step 6: Verify newly reviewed snapshot was submitted (keyboard revoked = 4096, mouse preserved)
      expect(updatePayload).toEqual({
        uuid: 'uuid-cancel-reopen',
        enabled: true,
        perm: 119475968, // 119480064 - 4096
      })
    })

    it('guards confirmation while another mutation is held, preventing overlapping calls and retaining reviewed state', async () => {
      const clientA = createClientRecord({
        uuid: 'uuid-client-a',
        name: 'Client A',
        enabled: true,
        perm: 119480064,
      })
      const clientB = createClientRecord({
        uuid: 'uuid-client-b',
        name: 'Client B',
        enabled: true,
        perm: 119480064,
      })
      const deferredMutationA = createDeferred()
      const { wrapper, fetchMock } = await mountPin({ clients: [clientA, clientB] })
      const original = fetchMock.getMockImplementation()

      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          const body = JSON.parse(options.body)
          if (body.uuid === 'uuid-client-a') {
            return deferredMutationA.promise
          }
        }
        return original(url, options)
      })

      const cardA = wrapper.find('[data-client-uuid="uuid-client-a"]')
      const cardB = wrapper.find('[data-client-uuid="uuid-client-b"]')

      // Step 1: Client B opens confirmation for mouse revocation
      await cardB.find('#perm-mouse-uuid-client-b').setValue(false)
      await cardB.find('.btn-save-client').trigger('click')
      expect(cardB.find('.btn-confirm-save').exists()).toBe(true)
      expect(wrapper.vm.clients[1].confirmingSave).toBe(true)
      const bSnapshot = { ...wrapper.vm.clients[1].pendingSnapshot }
      expect(bSnapshot.mouse).toBe(false)

      // Step 2: Client A begins mutation (revoke pen)
      await cardA.find('#perm-pen-uuid-client-a').setValue(false)
      await cardA.find('.btn-save-client').trigger('click')
      await cardA.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      // Mutation guard is held by Client A
      expect(wrapper.vm.isMutating).toBe(true)
      expect(cardB.find('.btn-confirm-save').attributes('disabled')).toBeDefined()

      // Step 3: Attempt confirmation on Client B while mutation is held
      await cardB.find('.btn-confirm-save').trigger('click')
      wrapper.vm.confirmSave(wrapper.vm.clients[1])
      await flushPromises()

      // Verify no token fetch or update was dispatched for Client B
      const bUpdates = fetchMock.mock.calls.filter(
        ([url, options = {}]) => url === './api/clients/update' && JSON.parse(options.body).uuid === 'uuid-client-b'
      )
      expect(bUpdates).toHaveLength(0)

      // Verify Client B's reviewed state is not silently discarded
      expect(wrapper.vm.clients[1].confirmingSave).toBe(true)
      expect(wrapper.vm.clients[1].pendingSnapshot).toEqual(bSnapshot)
      expect(cardB.find('.btn-confirm-save').exists()).toBe(true)

      // Step 4: Release Client A mutation
      deferredMutationA.resolve(mockResponse({ status: true }))
      await flushPromises()
      expect(wrapper.vm.isMutating).toBe(false)

      // Step 5: Now confirm Client B; proceeds with reviewed snapshot
      await cardB.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      const bUpdatesAfter = fetchMock.mock.calls.filter(
        ([url, options = {}]) => url === './api/clients/update' && JSON.parse(options.body).uuid === 'uuid-client-b'
      )
      expect(bUpdatesAfter).toHaveLength(1)
      expect(JSON.parse(bUpdatesAfter[0][1].body)).toEqual({
        uuid: 'uuid-client-b',
        enabled: true,
        perm: 119478016, // mouse revoked
      })
    })

    it('preserves live draft edits on failed confirmed save and allows a new confirmation', async () => {
      const client = createClientRecord({
        uuid: 'uuid-fail-retry',
        name: 'Client Fail Retry',
        enabled: true,
        perm: 119480064,
      })
      const { wrapper, fetchMock } = await mountPin({ clients: [client] })
      const original = fetchMock.getMockImplementation()

      let updateAttempt = 0
      let lastPayload = null
      fetchMock.mockImplementation(async (url, options = {}) => {
        if (url === './api/clients/update') {
          updateAttempt++
          lastPayload = JSON.parse(options.body)
          if (updateAttempt === 1) {
            return mockResponse({ status: false, error: 'Database transaction lock failed' })
          }
          return mockResponse({ status: true })
        }
        return original(url, options)
      })

      const clientCard = wrapper.find('[data-client-uuid="uuid-fail-retry"]')

      // Step 1: Revoke mouse and open confirmation
      await clientCard.find('#perm-mouse-uuid-fail-retry').setValue(false)
      await clientCard.find('.btn-save-client').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)

      // Step 2: Make another edit (revoke touch) while confirmation is visible
      await clientCard.find('#perm-touch-uuid-fail-retry').setValue(false)

      // Step 3: Confirm first save (submits reviewed snapshot with mouse only)
      await clientCard.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      expect(lastPayload).toEqual({
        uuid: 'uuid-fail-retry',
        enabled: true,
        perm: 119478016, // only mouse
      })

      // Error feedback is rendered
      expect(clientCard.text()).toContain('Database transaction lock failed')

      // Live draft edits are preserved
      const clientModel = wrapper.vm.clients[0]
      expect(clientModel.draft.mouse).toBe(false)
      expect(clientModel.draft.touch).toBe(false)
      expect(wrapper.vm.isDirty(clientModel)).toBe(true)
      expect(clientCard.find('#perm-mouse-uuid-fail-retry').element.checked).toBe(false)
      expect(clientCard.find('#perm-touch-uuid-fail-retry').element.checked).toBe(false)

      // Old confirmation alert is cleared
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(false)

      // Step 4: Operator re-saves to open a new confirmation for current live draft
      await clientCard.find('.btn-save-client').trigger('click')
      expect(clientCard.find('.btn-confirm-save').exists()).toBe(true)
      expect(clientModel.pendingSnapshot.mouse).toBe(false)
      expect(clientModel.pendingSnapshot.touch).toBe(false)

      // Step 5: Confirm new save (succeeds with both mouse and touch revoked)
      await clientCard.find('.btn-confirm-save').trigger('click')
      await flushPromises()

      expect(updateAttempt).toBe(2)
      expect(lastPayload).toEqual({
        uuid: 'uuid-fail-retry',
        enabled: true,
        perm: 119477504, // mouse (2048) and touch (512) both revoked: 119480064 - 2560
      })
      expect(clientModel.error).toBeNull()
      expect(clientCard.text()).toContain('pin.client_updated_success')
    })
  })
})
