const http = require('http')
const fs = require('fs')
const path = require('path')

const PORT = Number(process.env.PORT ?? 48124)
const STATIC_DIR = path.resolve(__dirname, '../../build/assets/web')

const MIME_TYPES = {
  '.html': 'text/html; charset=UTF-8',
  '.js': 'application/javascript; charset=UTF-8',
  '.css': 'text/css; charset=UTF-8',
  '.json': 'application/json; charset=UTF-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.woff2': 'font/woff2',
}

const server = http.createServer((req, res) => {
  const urlPath = req.url.split('?')[0]
  if (req.method !== 'GET') {
    res.writeHead(405, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ status: false, error: 'Read-only UI fixture' }))
  }

  // Mock API endpoints
  if (urlPath === '/api/configLocale') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ locale: 'en' }))
  }
  if (urlPath === '/api/config' || urlPath === '/./api/config') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({
      platform: 'windows',
      controller: 'enabled',
      gamepad_driver: 'virtualhid',
      version: '2026.927.1200',
      locale: 'en',
    }))
  }
  if (urlPath === '/api/version' || urlPath === '/./api/version') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({
      version: '2026.927.1200',
    }))
  }
  if (urlPath === '/api/virtual-input/status' || urlPath === '/./api/virtual-input/status') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({
      virtualhid: { installed: true, development_version: false, version_compatible: true },
      vigembus: { installed: false, version_compatible: false },
    }))
  }
  if (urlPath === '/api/virtual-input/license' || urlPath === '/./api/virtual-input/license') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ licensed: true, service_available: true }))
  }
  if (urlPath === '/api/permissions' || urlPath === '/./api/permissions') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ permissions: [] }))
  }
  if (urlPath === '/api/logs' || urlPath === '/./api/logs') {
    res.writeHead(200, { 'Content-Type': 'text/plain' })
    return res.end('')
  }
  if (urlPath === '/api/clients/list' || urlPath === '/./api/clients/list') {
    res.writeHead(200, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ status: true, named_certs: [] }))
  }
  if (urlPath === '/logout') {
    res.writeHead(200, { 'Content-Type': 'text/plain' })
    return res.end('OK')
  }

  // Handle any other api route
  if (urlPath.startsWith('/api/') || urlPath.startsWith('/./api/')) {
    res.writeHead(501, { 'Content-Type': 'application/json' })
    return res.end(JSON.stringify({ status: false, error: `Unhandled fixture route: ${urlPath}` }))
  }

  // Static file resolution
  let cleanUrl = urlPath
  if (cleanUrl.startsWith('/public/')) {
    cleanUrl = cleanUrl.slice('/public'.length)
  }
  let filePath = path.resolve(STATIC_DIR, cleanUrl === '/' ? 'index.html' : `.${cleanUrl}`)
  if (!filePath.startsWith(`${STATIC_DIR}${path.sep}`)) {
    res.writeHead(400, { 'Content-Type': 'text/plain' })
    return res.end('Invalid fixture path')
  }
  if (!fs.existsSync(filePath) || fs.statSync(filePath).isDirectory()) {
    if (path.extname(cleanUrl)) {
      res.writeHead(404, { 'Content-Type': 'text/plain' })
      return res.end('Missing fixture asset')
    }
    filePath = path.join(STATIC_DIR, 'index.html')
  }

  const ext = path.extname(filePath)
  const contentType = MIME_TYPES[ext] || 'application/octet-stream'

  fs.readFile(filePath, (err, content) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain' })
      return res.end('Not Found')
    }
    res.writeHead(200, { 'Content-Type': contentType })
    res.end(content)
  })
})

server.listen(PORT, '127.0.0.1', () => {
  console.log(`Preview server listening on http://127.0.0.1:${server.address().port}`)
})
