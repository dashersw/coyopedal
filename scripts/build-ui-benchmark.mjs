// Use the normal Gea build, with the pedal's OTA service and partition map.
// Always restore the production manifest, including after a failed build.
import fs from 'node:fs'
import { spawnSync } from 'node:child_process'

const original = fs.readFileSync('package.json', 'utf8')
const manifest = JSON.parse(original)
manifest.gea.entry = 'src/ui/bouncing-balls-benchmark.tsx'
manifest.gea.nativeSources.push('src/native/diagnostics/ui_frame_benchmark.cpp')
manifest.gea.defines.GEA_EMBEDDED_FRAME_BENCHMARK = 1
manifest.gea.defines.GEA_EMBEDDED_PERF = 0
manifest.gea.defines.GEA_EMBEDDED_DEFAULT_FRAME_INTERVAL_US = 16667
manifest.gea.defines.GEA_EMBEDDED_SHARED_STYLES = Number(process.env.GEA_BENCH_SHARED_STYLES || 0)
let result
try {
  fs.writeFileSync('package.json', JSON.stringify(manifest, null, 2) + '\n')
  result = spawnSync(
    process.execPath,
    [
      process.env.GEA_CLI_BIN || 'node_modules/@geastack/cli/bin/gea.mjs',
      'build',
      '--board',
      'amoled-241',
    ],
    { stdio: 'inherit', env: process.env },
  )
} finally {
  fs.writeFileSync('package.json', original)
}
if (result.error) throw result.error
process.exitCode = result.status ?? 1
