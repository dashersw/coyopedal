// Flash a layout-checked image to the registered 1.8 board and capture its
// first completed-frame benchmark. Never selects the separate 2.06 board.
import { createHash } from 'node:crypto'
import { closeSync, openSync, readFileSync, writeFileSync } from 'node:fs'
import { spawn, spawnSync } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const audit = path.join(root, 'build/css-feature-audit')
const [label] = process.argv.slice(2)
if (!label || path.basename(label) !== label) throw new Error('Supply one output filename prefix')
const read = (suffix) => JSON.parse(readFileSync(path.join(audit, label + suffix)))
const inputs = read('-build-inputs.json')
const qualification = read('-qualification.json')
const examples =
  inputs.examplesRoot || path.join(path.dirname(inputs.overrides.GEA_COMPILER_DIR), 'examples')
const cli = inputs.cli || path.join(path.dirname(examples), 'cli/bin/gea.mjs')
const board = JSON.parse(readFileSync(path.join(examples, '.gea/boards.json')))['amoled-18']
if (
  inputs.board !== 'amoled-18' ||
  inputs.target !== 'esp32-s3-touch-amoled-1.8' ||
  inputs.usbSerial !== '30:ED:A0:AC:90:DC' ||
  board?.target !== inputs.target ||
  board?.transports?.usbSerial?.serial !== inputs.usbSerial
)
  throw new Error('Refusing to flash: expected the registered 1.8 board 30:ED:A0:AC:90:DC')
const image = path.join(audit, label + '.bin')
if (
  !qualification.qualified ||
  qualification.shared !== inputs.shared ||
  createHash('sha256').update(readFileSync(image)).digest('hex') !== qualification.sha256
)
  throw new Error('Image must match its linked-layout qualification')
const env = {
  ...Object.fromEntries(
    Object.entries(process.env).filter(
      ([key]) => !key.startsWith('GEA_') && !key.startsWith('GEATSC'),
    ),
  ),
  ...inputs.overrides,
  TMPDIR: audit,
  CCACHE_DISABLE: '1',
}
const flashLog = openSync(path.join(audit, label + '-flash.log'), 'w')
let flashed
try {
  flashed = spawnSync(
    process.execPath,
    [
      cli,
      'flash',
      '--board',
      'amoled-18',
      '--app',
      'bouncing-balls-jsx',
      '--no-build',
      '--image',
      image,
    ],
    { cwd: examples, env, stdio: ['ignore', flashLog, flashLog], timeout: 180_000 },
  )
} finally {
  closeSync(flashLog)
}
if (flashed.status !== 0) throw new Error(`USB flash failed: ${flashed.error || flashed.status}`)
console.log('Flashed qualified image to amoled-18; capturing its first benchmark.')
const logFile = path.join(audit, label + '-device.log')
let log = ''
const capture = await new Promise((resolve, reject) => {
  const child = spawn(
    process.execPath,
    [cli, 'logs', '--board', 'amoled-18', '--transport', 'usb'],
    {
      cwd: examples,
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
    },
  )
  let resultTimer,
    killTimer,
    timedOut = false,
    stopped = false
  const stop = () => {
    stopped = true
    child.kill('SIGINT')
    killTimer = setTimeout(() => child.kill('SIGKILL'), 5000)
  }
  const timer = setTimeout(() => {
    timedOut = true
    stop()
  }, 600_000)
  const collect = (chunk) => {
    log += chunk.toString()
    writeFileSync(logFile, log)
    if (!resultTimer && /(?:Guru Meditation|abort\(\) was called|assert failed)/.test(log))
      resultTimer = setTimeout(stop, 1000)
    if (!resultTimer && /RESULT[^\n]*\n/.test(log)) resultTimer = setTimeout(stop, 3000)
  }
  child.stdout.on('data', collect)
  child.stderr.on('data', collect)
  child.on('error', reject)
  child.on('close', (code, signal) => {
    clearTimeout(timer)
    clearTimeout(resultTimer)
    clearTimeout(killTimer)
    resolve({ code, signal, stopped, timedOut })
  })
})
const resultLine = log.split(/\r?\n/).find((line) => /(?:^|\s)RESULT\s+frames=/.test(line))
const sample = resultLine
  ? Object.fromEntries(
      [...resultLine.matchAll(/\b([a-z][a-z0-9_]*)=(\d+)/g)].map(([, k, v]) => [k, Number(v)]),
    )
  : undefined
const { checkLog } = await import(path.join(examples, 'scripts/check-bouncing-balls-fps.mjs'))
let report
try {
  if (
    capture.timedOut ||
    !capture.stopped ||
    (![0, 130].includes(capture.code) && capture.signal !== 'SIGINT')
  )
    throw new Error(`Device capture failed: ${JSON.stringify(capture)}`)
  report = { passed: true, ...checkLog(log, inputs.shared) }
} catch (error) {
  report = { passed: false, failure: error.message, sample }
}
writeFileSync(path.join(audit, label + '-result.json'), JSON.stringify(report, null, 2) + '\n')
console.log(JSON.stringify(report, null, 2))
process.exitCode = report.passed ? 0 : 1
