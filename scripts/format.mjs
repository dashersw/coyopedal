import { execFileSync } from 'node:child_process'
import { readFileSync, writeFileSync, existsSync } from 'node:fs'
import { extname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = fileURLToPath(new URL('../', import.meta.url))
process.chdir(root)
const check = process.argv.includes('--check')
const files = execFileSync(
  'git',
  ['ls-files', '-z', '--cached', '--others', '--exclude-standard'],
  {
    encoding: 'utf8',
  },
)
  .split('\0')
  .filter(Boolean)
  .filter((file) => existsSync(file))
  .filter((file) => !file.startsWith('third_party/'))

function run(command, args) {
  execFileSync(command, args, { stdio: 'inherit', cwd: root })
}

const native = files.filter((file) => /\.(c|cc|cpp|h|hpp|tpp|inc)$/.test(file))
run(resolve('build/format-tools/bin/clang-format'), [
  check ? '--dry-run' : '-i',
  '--Werror',
  ...native,
])
const web = files.filter((file) => /\.(js|mjs|cjs|ts|tsx|css|html|json|md|ya?ml|sh)$/.test(file))
run(resolve('node_modules/.bin/prettier'), [check ? '--check' : '--write', ...web])
const python = files.filter((file) => extname(file) === '.py')
run(resolve('build/format-tools/bin/ruff'), ['format', ...(check ? ['--check'] : []), ...python])
const cmake = files.filter((file) => file.endsWith('CMakeLists.txt') || file.endsWith('.cmake'))
run(resolve('build/format-tools/bin/cmake-format'), [check ? '--check' : '-i', ...cmake])

// Xtensa assembly has no clang-format parser. Preserve instructions, labels,
// alignment and preprocessor directives, normalizing only trailing whitespace.
for (const file of files.filter((file) => extname(file) === '.S')) {
  const before = readFileSync(file, 'utf8')
  const after = `${before.replace(/[\t ]+$/gm, '').trimEnd()}\n`
  if (before !== after) {
    if (check) throw new Error(`Assembly whitespace: ${file}`)
    writeFileSync(file, after)
  }
}
