// Build the original gallery app on the registered USB AMOLED 2.06.
// App source and cadence stay unchanged; restore the instrumentation manifest.
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'

const gallery = process.env.GEA_BENCH_GALLERY
if (!gallery) throw new Error('Set GEA_BENCH_GALLERY to the Gea examples project')
const app = path.join(gallery, 'apps/bouncing-balls-jsx')
const manifestPath = path.join(app, 'package.json')
const originalManifest = fs.readFileSync(manifestPath, 'utf8')
const manifest = JSON.parse(originalManifest)
manifest.gea.defines = {
  ...manifest.gea.defines,
  GEA_EMBEDDED_FRAME_BENCHMARK: 2,
  GEA_EMBEDDED_FRAME_SCHEDULER_FPS_LOG: 0,
  GEA_EMBEDDED_SHARED_STYLES: Number(process.env.GEA_BENCH_SHARED_STYLES || 0),
}
const childEnv = { ...process.env }
for (const [key, name] of Object.entries({
  GEA_CORE_DIR: 'core',
  GEA_COMPILER_DIR: 'compiler',
  GEA_HOST_DIR: 'host',
  GEA_CHIPS_DIR: 'chips',
  GEA_GEAOS_PACKAGE_DIR: 'geaos',
}))
  childEnv[key] ||= path.resolve(`node_modules/@geastack/${name}`)
let result
try {
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n')
  result = spawnSync(
    process.execPath,
    [
      process.env.GEA_CLI_BIN || path.resolve('node_modules/@geastack/cli/bin/gea.mjs'),
      'build',
      '--board',
      'amoled',
      '--app',
      'bouncing-balls-jsx',
    ],
    { cwd: gallery, stdio: 'inherit', env: childEnv },
  )
} finally {
  fs.writeFileSync(manifestPath, originalManifest)
}
if (result.error) throw result.error
process.exitCode = result.status ?? 1
