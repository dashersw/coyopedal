#!/usr/bin/env node
// Runs a Python tool with whichever interpreter this machine actually has.
// Linux and macOS call it `python3`; Windows installs `python` and the `py`
// launcher, and its `python3` is a Microsoft Store stub that prints an install
// hint and exits 9009. Rather than telling readers which name to type, the npm
// scripts and the prebuild go through here: the first candidate that answers
// `--version` with a Python 3 banner is used.
//
//   node scripts/python.mjs tools/esp32/amoled_remote.py discover
import { execFileSync, spawnSync } from 'node:child_process'
import { pathToFileURL } from 'node:url'

const candidates = process.platform === 'win32'
  ? [['py', ['-3']], ['python', []], ['python3', []]]
  : [['python3', []], ['python', []]]

export function resolvePython(env = process.env) {
  for (const [command, prefix] of candidates) {
    try {
      const banner = execFileSync(command, [...prefix, '--version'], { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] })
      if (/^Python 3\./.test(banner.trim())) return { command, prefix }
    } catch {
      // not installed, or the Store stub: try the next name
    }
  }
  throw new Error('No Python 3 interpreter found: install Python 3 and make sure it is on PATH.')
}

// Spawn `python <args>` with inherited stdio and return the exit status.
export function runPython(args, options = {}) {
  const python = resolvePython(options.env)
  const result = spawnSync(python.command, [...python.prefix, ...args], { stdio: 'inherit', ...options })
  if (result.error) throw result.error
  return result.status ?? 1
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exit(runPython(process.argv.slice(2)))
}
