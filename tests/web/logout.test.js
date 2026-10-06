import { describe, expect, it, vi } from 'vitest'
import { logoutWithBasicAuth } from '../../src_assets/common/assets/web/logout.js'

describe('BasicAuth logout compatibility', () => {
  it.each(['onload', 'onerror', 'ontimeout'])('uses hard navigation after %s', (callback) => {
    const location = { href: 'https://host.example/apps', replace: vi.fn() }
    const request = { open: vi.fn(), setRequestHeader: vi.fn(), send: vi.fn() }
    function XMLHttpRequest() { return request }
    logoutWithBasicAuth({ location, XMLHttpRequest })
    expect(request.open).toHaveBeenCalledWith('GET', '/', true, 'sunshine-logout', expect.any(String))
    expect(request.setRequestHeader).toHaveBeenCalledWith('Cache-Control', 'no-store')
    expect(request.timeout).toBe(5000)
    expect(request.send).toHaveBeenCalledOnce()
    request[callback]()
    expect(location.replace).toHaveBeenCalledWith('https://host.example/logout')
  })
})
