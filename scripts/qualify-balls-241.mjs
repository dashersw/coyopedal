// Preserve the exact image, linked layouts, and generated workload before
// another variant reuses the CLI's native build directory. Never flashes.
import { createHash } from 'node:crypto'
import { copyFileSync, readFileSync, readdirSync, writeFileSync } from 'node:fs'
import { spawnSync } from 'node:child_process'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const audit = path.join(root, 'build/css-feature-audit')
const [label] = process.argv.slice(2)
if (!label || path.basename(label) !== label) throw new Error('Supply one output filename prefix')
const inputs = JSON.parse(readFileSync(path.join(audit, `${label}-build-inputs.json`)))
const examples =
  inputs.examplesRoot || path.join(path.dirname(inputs.overrides.GEA_COMPILER_DIR), 'examples')
const native =
  inputs.nativeBuild ||
  path.join(
    audit,
    `candidate-build/${inputs.target || 'esp32-s3-touch-amoled-2.41'}/app-builds/bouncing-balls-jsx`,
  )
const gate = await import(path.join(examples, 'scripts/check-bouncing-balls-fps.mjs'))
const app = path.join(examples, 'apps/bouncing-balls-jsx')
gate.checkWorkload(
  readFileSync(path.join(app, 'index.tsx'), 'utf8'),
  readFileSync(path.join(app, 'constants.tsx'), 'utf8'),
)
gate.checkWorkloadFiles(
  JSON.parse(readFileSync(path.join(examples, 'scripts/test/bouncing-balls-workload.json'))),
  (file) => readFileSync(path.join(app, file)),
)
const image = readFileSync(path.join(audit, `${label}.bin`))
if (!image.equals(readFileSync(path.join(native, 'gea_embedded.bin'))))
  throw new Error('Image differs from current native build')
for (const [source, suffix] of [
  ['gea_embedded.elf', '.elf'],
  ['gea_embedded.map', '.map'],
  ['sdkconfig', '-sdkconfig'],
  ['compile_commands.json', '-compile-commands.json'],
])
  copyFileSync(path.join(native, source), path.join(audit, label + suffix))
const python = path.join(process.env.HOME, '.espressif/python_env/idf6.0_py3.11_env/bin/python')
const inspected = spawnSync(
  python,
  [path.join(root, 'tools/esp32/check_ui_layout.py'), path.join(audit, `${label}.elf`)],
  { encoding: 'utf8' },
)
if (inspected.status !== 0)
  throw new Error(inspected.stderr || inspected.stdout || 'Linked layout inspection failed')
const layout = JSON.parse(inspected.stdout)
writeFileSync(path.join(audit, `${label}-linked-layout.json`), inspected.stdout)
gate.checkLinkedStorage(layout, inputs.shared)
writeFileSync(
  path.join(audit, `${label}-native-hashes.json`),
  JSON.stringify(
    gate.fingerprintGeneratedNative(path.join(native, 'apps/bouncing-balls-jsx')),
    null,
    2,
  ) + '\n',
)
// Keep the maintained engine/target inputs beside the emitted app fingerprints.
// Shared and inline builds must differ only in their intended storage define.
const frameworkHashes = {}
for (const [component, directory] of Object.entries(inputs.packageRoots || inputs.overrides)) {
  if (
    ![
      'GEA_CORE_DIR',
      'GEA_ENGINE_DIR',
      'GEA_HOST_DIR',
      'GEA_ELEMENTS_DIR',
      'GEA_TARGETS_ROOT',
    ].includes(component)
  )
    continue
  const visit = (relative = '') => {
    for (const entry of readdirSync(path.join(directory, relative), { withFileTypes: true })) {
      if (
        entry.name.startsWith('.') ||
        ['node_modules', 'build', 'dist', 'managed_components'].includes(entry.name)
      )
        continue
      const file = path.join(relative, entry.name)
      if (entry.isDirectory()) visit(file)
      else if (
        entry.isFile() &&
        (/\.(?:c|cpp|h|hpp|S|cmake)$/.test(file) || entry.name === 'CMakeLists.txt')
      )
        frameworkHashes[`${component}/${file}`] = createHash('sha256')
          .update(readFileSync(path.join(directory, file)))
          .digest('hex')
    }
  }
  visit()
}
writeFileSync(
  path.join(audit, `${label}-framework-hashes.json`),
  JSON.stringify(frameworkHashes, null, 2) + '\n',
)
const report = {
  qualified: true,
  bytes: image.length,
  sha256: createHash('sha256').update(image).digest('hex'),
  shared: inputs.shared,
  node: layout.records.Node[0].bytes,
  style: layout.records.ComputedStyle[0].bytes,
  tree: layout.records.TreeState[0].bytes,
}
writeFileSync(
  path.join(audit, `${label}-qualification.json`),
  JSON.stringify(report, null, 2) + '\n',
)
console.log(JSON.stringify(report))
