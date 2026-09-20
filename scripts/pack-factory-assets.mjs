#!/usr/bin/env node
// Prepares the two factory files the firmware needs. Declared as
// gea.targets.esp32.prebuild, so it runs before the board build reads them:
// `models` and `presets` are flashed into their own data partitions, and a copy
// of each is embedded in the application image so boot can repair a missing or
// stale partition.
//
// The model library is packed into an image. The presets are not packed into
// anything: presets.json is checked and copied, and what is flashed is that
// file. See tools/presets_to_json.py.
//
// This used to be two add_custom_command blocks in the ESP-IDF main component,
// which is why the repo had to own a board target to build at all.
import { execFileSync } from 'node:child_process'
import { mkdirSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = path.join(root, 'build', 'factory')
const manifest = path.join(root, 'assets', 'models', 'factory.json')

mkdirSync(out, { recursive: true })

const run = (script, args, output) => {
  process.stdout.write(`pack-factory-assets: ${script} -> ${path.relative(root, output)}\n`)
  execFileSync('python3', [path.join(root, 'tools', script), ...args], {
    cwd: root,
    stdio: 'inherit',
  })
}

const models = path.join(out, 'models.bin')
const presets = path.join(out, 'presets.json')

run('models_to_bin.py', [manifest, root, models], models)
run(
  'presets_to_json.py',
  [path.join(root, 'assets', 'presets.json'), presets, '--manifest', manifest],
  presets,
)
