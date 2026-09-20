// Writes the version of every file under web/dist into the page, so each one is
// asked for by a name that changes only when its bytes do. That is what lets
// web/_headers cache dist/ immutably: the page names the versions, the page is
// never cached, and nothing else has to be revalidated.
//
// module.js and module.wasm are hashed TOGETHER, under one version. They are one
// program in two files -- the wasm imports the EM_JS functions the glue defines
// -- so a browser holding a separately-valid version of each from two different
// builds would not degrade, it would fail to instantiate. Sharing the version
// makes the pair the unit, which is what it is. See the note next to ASSETS in
// web/index.html.
//
// It also stamps the commit the page was built from into the source link in the
// footer. Serving the page conveys module.wasm to every visitor, which the GPL
// asks be accompanied by the corresponding source -- the source these bytes
// were built from, which a link to a moving branch stops naming the moment
// anything else lands. A tree with uncommitted changes is not a commit anyone
// can be pointed at, so that case is left alone rather than stamped with a
// close-enough answer.
//
//   node scripts/stamp-web-build.mjs web/index.html web/dist

import { execFileSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { readFileSync, readdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'

const page = process.argv[2]
const dist = process.argv[3]

if (!page || !dist) {
  console.error('usage: node scripts/stamp-web-build.mjs <page.html> <dist-dir>')
  process.exit(1)
}

const walk = (dir) =>
  readdirSync(dir, { withFileTypes: true }).flatMap((entry) => {
    const full = path.join(dir, entry.name)
    return entry.isDirectory() ? walk(full) : [full]
  })

const short = (...files) => {
  const hash = createHash('sha256')
  for (const file of files) hash.update(readFileSync(file))
  return hash.digest('hex').slice(0, 12)
}

// Keys are what the page asks for: paths relative to the page's own directory,
// which is the directory dist/ sits in.
const root = path.dirname(page)
const key = (file) => path.relative(root, file).split(path.sep).join('/')

const files = walk(dist).sort()
const pair = files.filter((file) => /module\.(js|wasm)$/.test(file))
const pairVersion = pair.length === 2 ? short(...pair) : null

const versions = new Map()
for (const file of files) {
  versions.set(key(file), pair.includes(file) ? pairVersion : short(file))
}

const table = [...versions].map(([name, version]) => `        '${name}': '${version}',`).join('\n')
const text = readFileSync(page, 'utf8')
// Matched rather than compared: a rebuild that changes nothing writes the same
// table back, and that is a success, not a missing table.
const pattern = /( {6}const ASSETS = \{)[\s\S]*?(\n {6}\})/
if (!pattern.test(text)) {
  console.error(`${page} has no "const ASSETS = { ... }" table to stamp`)
  process.exit(1)
}
let stamped = text.replace(pattern, `$1\n${table}$2`)

const git = (...args) => {
  try {
    return execFileSync('git', args, { cwd: root, encoding: 'utf8' }).trim()
  } catch {
    return null
  }
}
// GITHUB_SHA is what a runner checks out, and it is right even where the
// checkout is a detached head with no branch to read.
const dirty = git('status', '--porcelain')
const commit = dirty === '' ? (process.env.GITHUB_SHA ?? git('rev-parse', 'HEAD')) : null
if (commit) {
  // Prettier puts each attribute on its own line, so the anchor is matched
  // across them rather than as one string.
  const sourceLink = /(<a\s+id="source"\s+href=")[^"]*("[\s\S]*?>)[\s\S]*?(<\/a)/
  if (!sourceLink.test(stamped)) {
    console.error(`${page} has no <a id="source"> to stamp the commit into`)
    process.exit(1)
  }
  stamped = stamped.replace(
    sourceLink,
    `$1https://github.com/dashersw/coyopedal/tree/${commit}$2github.com/dashersw/coyopedal @ ${commit.slice(0, 7)}$3`,
  )
}

writeFileSync(page, stamped)
console.log(
  `stamped ${versions.size} asset(s) into ${key(page)}, module ${pairVersion}` +
    (commit
      ? `, source ${commit.slice(0, 7)}`
      : // Name what is dirty. A tool that drops an untracked folder in the
        // workspace silently costs the page its source link, and "the working
        // tree is dirty" on its own does not say which file to look at.
        `, source link left as it was: ${
          dirty === null
            ? 'git could not read the working tree'
            : `uncommitted ${dirty
                .split('\n')
                .slice(0, 3)
                .map((line) => line.trim())
                .join(', ')}`
        }`),
)
