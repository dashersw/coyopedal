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
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

import { resolvePython } from './python.mjs'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const out = path.join(root, 'build', 'factory')
const manifest = path.join(root, 'assets', 'models', 'factory.json')

mkdirSync(out, { recursive: true })

// remote_service.cpp and main.cpp read the Wi-Fi credentials from
// services/remote_config.h, which is gitignored and comes from
// tools/esp32/configure_remote.py. When it is absent they fall back to a
// disabled service, but a header that does not exist is invisible to the build
// system: neither ninja nor ccache learns of it, so generating it after a first
// build changed nothing until the objects were compiled again by hand. The
// stub written here makes the header always present, and its contents are
// then part of what both track, so replacing it with real credentials
// recompiles exactly the files that read it.
const remoteConfig = path.join(root, 'src', 'native', 'services', 'remote_config.h')
if (!existsSync(remoteConfig)) {
  writeFileSync(
    remoteConfig,
    [
      '#pragma once',
      '',
      '// COYOPEDAL_REMOTE_STUB: written by scripts/pack-factory-assets.mjs because no',
      '// remote_config.h existed. Maintenance mode, OTA and the remote tools are',
      '// compiled out. Replace it with real credentials:',
      '//   npm run remote:configure -- --ssid YOUR_WIFI',
      '',
      '#define COYOPEDAL_REMOTE_ENABLED 0',
      '#define COYOPEDAL_REMOTE_DISABLE_USB_AUDIO 0',
      '#define COYOPEDAL_REMOTE_WIFI_SSID ""',
      '#define COYOPEDAL_REMOTE_WIFI_PASSWORD ""',
      '#define COYOPEDAL_REMOTE_TOKEN ""',
      '',
    ].join('\n'),
  )
  process.stdout.write('pack-factory-assets: no remote_config.h, wrote a disabled stub (remote service compiled out)\n')
}

const python = resolvePython()

const run = (script, args, output) => {
  process.stdout.write(`pack-factory-assets: ${script} -> ${path.relative(root, output)}\n`)
  execFileSync(python.command, [...python.prefix, path.join(root, 'tools', script), ...args], {
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
