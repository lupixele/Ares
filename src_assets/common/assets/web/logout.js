/**
 * @brief Preserve Sunshine's BasicAuth logout probe followed by hard navigation.
 * @param {object} dependencies Optional browser dependencies for isolated tests.
 * @return {XMLHttpRequest} Probe request; completion, failure and timeout navigate.
 */
export function logoutWithBasicAuth({ location = globalThis.location, XMLHttpRequest = globalThis.XMLHttpRequest } = {}) {
  const logoutPageUrl = new URL('/logout', location.href)
  const request = new XMLHttpRequest()
  const finish = () => location.replace(logoutPageUrl.toString())
  request.open('GET', '/', true, 'sunshine-logout', Date.now().toString())
  request.setRequestHeader('Cache-Control', 'no-store')
  request.onload = finish
  request.onerror = finish
  request.ontimeout = finish
  request.timeout = 5000
  request.send()
  return request
}
