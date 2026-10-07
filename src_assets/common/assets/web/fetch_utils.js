import { notifyKey } from './Notification.vue'

/**
 * The set of error messages that indicate a CSRF validation failure.
 */
const CSRF_ERRORS = new Set(['Missing CSRF token', 'Invalid CSRF token', 'CSRF token expired'])

/**
 * Wrapper around the native fetch that automatically detects CSRF errors
 * (HTTP 400 with a known CSRF error message) and displays a notification.
 *
 * @param {string} url - The URL to fetch.
 * @param {RequestInit} [options] - Standard fetch options.
 * @returns {Promise<Response>} The fetch Response.
 */
export async function apiFetch(url, options) {
  const response = await fetch(url, options)

  if (response.status === 400) {
    let body = null
    try {
      body = await response.clone().json()
    } catch (e) {
      console.debug('apiFetch: response body is not JSON', e)
    }

    if (body && CSRF_ERRORS.has(body.error)) {
      notifyKey.error('_common.csrf_error_desc', '_common.csrf_error')
    }
  }

  return response
}

/**
 * Safe fetch helper verifying response.ok status, supporting AbortController signal
 * with bounded timeout, and validating response structure.
 *
 * @param {string} url - The URL to fetch.
 * @param {object} [options]
 * @param {'json'|'text'} [options.type='json'] - Expected response body type.
 * @param {number} [options.timeout=5000] - Request deadline in milliseconds.
 * @param {AbortSignal} [options.signal] - Optional external AbortSignal.
 * @param {Function} [options.validate] - Optional shape validator for parsed data.
 * @returns {Promise<{ ok: boolean, status: number, data: any, error: any }>}
 */
export async function safeFetch(url, { type = 'json', timeout = 5000, signal, validate } = {}) {
  const controller = new AbortController()
  let timer = null
  let onAbort = null

  if (signal) {
    if (signal.aborted) {
      controller.abort()
    } else {
      onAbort = () => controller.abort()
      signal.addEventListener('abort', onAbort, { once: true })
    }
  }

  if (timeout > 0) {
    timer = setTimeout(() => {
      controller.abort()
    }, timeout)
  }

  try {
    const res = await fetch(url, { signal: controller.signal })
    if (timer) clearTimeout(timer)
    if (signal && onAbort) signal.removeEventListener('abort', onAbort)

    if (!res || !res.ok) {
      const status = res ? res.status : 0
      return { ok: false, status, data: null, error: new Error(`HTTP error ${status}`) }
    }

    let data
    if (type === 'text') {
      data = await res.text()
    } else {
      data = await res.json()
    }

    if (validate && !validate(data)) {
      return { ok: false, status: res.status, data: null, error: new Error('Invalid response shape') }
    }

    return { ok: true, status: res.status, data, error: null }
  } catch (err) {
    if (timer) clearTimeout(timer)
    if (signal && onAbort) signal.removeEventListener('abort', onAbort)
    return { ok: false, status: 0, data: null, error: err }
  }
}
