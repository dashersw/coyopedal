// Capture the original completed-frame benchmark over the 2.41's Wi-Fi link.
// Reads logs only. The existing gate remains responsible for pass/fail.
import { spawn } from 'node:child_process'
import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const audit = path.join(root, 'build/css-feature-audit')
const [label] = process.argv.slice(2)
if (!label || path.basename(label) !== label) throw new Error('Supply one output filename prefix')
const inputs = JSON.parse(readFileSync(path.join(audit, `${label}-build-inputs.json`)))
const geastack = path.dirname(inputs.overrides.GEA_COMPILER_DIR)
const examples = path.join(geastack, 'examples')
const logFile = path.join(audit, `${label}-device.log`)
const deadline = Date.now() + 600_000
let log = process.argv.includes('--resume') ? readFileSync(logFile, 'utf8') : ''
let resultSeen = false
while (!resultSeen && Date.now() < deadline) {
  await new Promise((resolve, reject) => {
    // This Mac grants local-network access to system Python. Node's direct
    // connection reports EHOSTUNREACH even while the same endpoint is live.
    // Decode the documented Gea diagnostics framing, exactly as the CLI does.
    const child = spawn(
      '/usr/bin/python3',
      [
        '-u',
        '-c',
        `
import socket, sys
with socket.create_connection(('192.168.178.159', 8081), timeout=5) as stream:
    stream.settimeout(None)
    stream.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    pending = b''
    while True:
        chunk = stream.recv(65536)
        if not chunk: break
        pending += chunk
        while len(pending) >= 4:
            length = int.from_bytes(pending[2:4], 'little')
            if len(pending) < 4 + length: break
            if pending[0] == 1:
                sys.stdout.buffer.write(pending[4:4+length])
                sys.stdout.buffer.flush()
            pending = pending[4+length:]
`,
      ],
      {
        cwd: examples,
        stdio: ['ignore', 'pipe', 'pipe'],
      },
    )
    let stopTimer
    const hardTimer = setTimeout(() => child.kill('SIGTERM'), Math.max(1, deadline - Date.now()))
    const collect = (chunk) => {
      log += chunk.toString()
      writeFileSync(logFile, log)
      if (!resultSeen && /RESULT[^\n]*\n/.test(log)) {
        resultSeen = true
        stopTimer = setTimeout(() => child.kill('SIGINT'), 3000)
      }
    }
    child.stdout.on('data', collect)
    child.stderr.on('data', collect)
    child.on('error', reject)
    child.on('close', () => {
      clearTimeout(hardTimer)
      clearTimeout(stopTimer)
      resolve()
    })
  })
  if (!resultSeen) await new Promise((resolve) => setTimeout(resolve, 2000))
}
writeFileSync(logFile, log)
const { checkLog } = await import(path.join(examples, 'scripts/check-bouncing-balls-fps.mjs'))
let report
const resultLine = log.split(/\r?\n/).find((line) => /(?:^|\s)RESULT\s+frames=/.test(line))
const sample = resultLine
  ? Object.fromEntries(
      [...resultLine.matchAll(/\b([a-z][a-z0-9_]*)=(\d+)/g)].map(([, key, value]) => [
        key,
        Number(value),
      ]),
    )
  : undefined
try {
  report = { passed: true, ...checkLog(log, inputs.shared) }
} catch (error) {
  report = { passed: false, failure: error.message, sample }
}
writeFileSync(path.join(audit, `${label}-result.json`), JSON.stringify(report, null, 2) + '\n')
console.log(JSON.stringify(report, null, 2))
process.exitCode = report.passed ? 0 : 1
