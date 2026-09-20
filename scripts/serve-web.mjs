// A static server for web/, with the two headers the WASM panel cannot run
// without.
//
// The audio thread and the UI share one wasm memory. Shared memory is a
// SharedArrayBuffer, and a SharedArrayBuffer only exists on a page the browser
// considers cross-origin isolated, which it decides from these two headers.
// Without them the constructor is simply absent, the worklet thread never
// starts, and the failure surfaces somewhere inside the module rather than at
// the point that caused it -- so the page checks for it and says so.
import { createReadStream, statSync } from 'node:fs'
import { createServer } from 'node:http'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const port = Number(process.env.PORT || 8791)

const types = new Map(
  Object.entries({
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.wasm': 'application/wasm',
    '.namb': 'application/octet-stream',
    '.otf': 'font/otf',
  }),
)

createServer((request, response) => {
  const url = new URL(request.url, `http://${request.headers.host}`)
  let file = path.join(root, decodeURIComponent(url.pathname))
  // Everything served is inside the repository; a path that escapes it is a
  // request for someone else's disk.
  if (!file.startsWith(root)) {
    response.writeHead(403).end('forbidden')
    return
  }
  try {
    if (statSync(file).isDirectory()) file = path.join(file, 'index.html')
  } catch {
    response.writeHead(404).end('not found')
    return
  }
  let size
  try {
    size = statSync(file).size
  } catch {
    response.writeHead(404).end('not found')
    return
  }
  response.writeHead(200, {
    'Content-Type': types.get(path.extname(file)) || 'application/octet-stream',
    'Content-Length': size,
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cache-Control': 'no-store',
  })
  createReadStream(file).pipe(response)
}).listen(port, '127.0.0.1', () => {
  console.log(`http://127.0.0.1:${port}/web/`)
})
