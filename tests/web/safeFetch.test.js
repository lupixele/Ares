import { afterEach, describe, expect, it, vi } from 'vitest'
import { safeFetch } from '../../src_assets/common/assets/web/fetch_utils.js'

afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers() })

describe('safeFetch production request boundaries', () => {
  it('rejects non-OK JSON without parsing it as successful data', async () => {
    const json = vi.fn()
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: false, status: 503, statusText: 'Unavailable', json })))
    expect(await safeFetch('/fixture')).toMatchObject({ ok: false, status: 503, data: null })
    expect(json).not.toHaveBeenCalled()
  })

  it('validates the parsed response shape', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => ({ ok: true, status: 200, json: async () => ({ message: 'not configuration' }) })))
    expect(await safeFetch('/fixture', { validate: data => typeof data.version === 'string' })).toMatchObject({ ok: false, data: null })
  })

  it('aborts a stalled request at the deadline and clears the timer', async () => {
    vi.useFakeTimers()
    let receivedSignal
    vi.stubGlobal('fetch', vi.fn((_url, { signal }) => {
      receivedSignal = signal
      return new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true }))
    }))
    const request = safeFetch('/fixture', { timeout: 50 })
    await vi.advanceTimersByTimeAsync(50)
    expect(await request).toMatchObject({ ok: false, data: null })
    expect(receivedSignal.aborted).toBe(true)
    expect(vi.getTimerCount()).toBe(0)
  })

  it('forwards caller cancellation and removes the pending deadline', async () => {
    vi.useFakeTimers()
    const caller = new AbortController()
    vi.stubGlobal('fetch', vi.fn((_url, { signal }) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true })
    })))
    const request = safeFetch('/fixture', { signal: caller.signal })
    caller.abort()
    expect(await request).toMatchObject({ ok: false, data: null })
    expect(vi.getTimerCount()).toBe(0)
  })
})
