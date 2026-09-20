import test, { after, beforeEach } from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'vite'
import { resolve } from 'node:path'
import { readFile } from 'node:fs/promises'
import { createServer as createHttpServer } from 'node:http'
import { createPanelPreviewStore } from '../src/preview/board-fixture.mjs'

let fixture
let store
const storage = new Map()
Object.assign(globalThis, {
  pbGet: (key) => fixture.get(key),
  pbSet: (key, value) => fixture.set(key, value),
  pbLabel: (kind, index) => fixture.label(kind, index),
  pbAction: (action, index, value) => fixture.action(action, index, value),
  pbPresetName: (action, text) => fixture.nameEdit(action, text),
})
const httpServer = createHttpServer()
const server = await createServer({
  configFile: resolve('src/ui/vite.config.ts'),
  server: { middlewareMode: true, hmr: { server: httpServer }, watch: null },
  appType: 'custom',
})
const { PedalboardStore } = await server.ssrLoadModule(resolve('src/ui/stores/PedalboardStore.ts'))
after(() => server.close())
beforeEach(() => {
  storage.clear()
  fixture = createPanelPreviewStore({
    getItem: (key) => storage.get(key),
    setItem: (key, value) => storage.set(key, value),
  })
  fixture.reset()
  store = new PedalboardStore()
  store.sync()
})

test('Gea store switches presets with explicit save/discard choices', () => {
  store.command(7)
  assert.equal(store.ampEnabled, false)
  store.choosePreset(1)
  assert.equal(store.screen, 7)
  store.confirm(true)
  assert.equal(store.activePreset, 1)
  assert.equal(store.edited, false)
  store.choosePreset(0)
  assert.equal(store.ampEnabled, false)
})

test('new preset captures the current sound and keyboard capitalizes word starts', () => {
  store.command(7)
  store.create()
  assert.equal(store.keyboardMode, 1)
  store.editName(1, 'H')
  assert.equal(store.keyboardMode, 0)
  store.editName(1, 'eavy ')
  assert.equal(store.keyboardMode, 1)
  store.editName(1, 'Lead')
  store.editName(3)
  assert.equal(store.presetName, 'Heavy Lead')
  assert.equal(store.ampEnabled, false)
  assert.equal(store.presets.length, 5)
  assert.equal(store.edited, false)
})

test('maintenance protects unsaved changes and returns to the selected preset', () => {
  store.choosePreset(1)
  store.command(7)
  store.show(8)
  store.enterMaintenance(false)
  assert.equal(store.screen, 10)
  store.enterMaintenance(true)
  assert.equal(store.screen, 9)
  store.back()
  assert.equal(store.screen, 0)
  assert.equal(store.activePreset, 1)
  assert.equal(store.ampEnabled, false)
})

test('parameter editing clamps values and the tuner leaves preset state untouched', () => {
  store.show(2, 4)
  store.parameter(1, 2)
  assert.equal(store.parameterValues[1], 1)
  store.command(9)
  store.command(4)
  fixture.set(62, 2)
  store.sync()
  assert.equal(store.tuner, true)
  assert.equal(store.inTune, true)
  assert.equal(store.edited, false)
  store.back()
  assert.equal(store.tuner, false)
})

// On the board a store array is a native container, and its indexed read is a
// checked access: an index at or past the end aborts the app outright -- "gea:
// read of array hole or out-of-range index ...; absence has no carrier here" --
// where JS quietly answers undefined. A change guard that compares before it
// writes therefore has to skip the growing edge, or the first sync kills the
// firmware before a single frame is painted. That is what a black screen on
// every screen was, with all of the tests above green.
//
// It is checked against the source rather than at runtime because the two reads
// cannot be told apart at runtime: the reactive layer under these tests fetches
// the previous value inside its own element write, so a trap on the array sees
// the framework's read and the store's read identically. The board's native
// write does neither -- it appends at length without reading -- so the only
// honest question is what the compiler is handed, and the invariant is a shape:
// an indexed comparison is guarded by a length check on the same list.
test('every list comparison in sync() skips the index the board has not got', async () => {
  const source = await readFile(resolve('src/ui/stores/PedalboardStore.ts'), 'utf8')
  const compared = [...source.matchAll(/this\.(\w+)\[(\w+)\] !== /g)]
  assert.ok(
    compared.length >= 7,
    `expected the list comparisons to be found, saw ${compared.length}`,
  )
  for (const match of compared) {
    const [, list, index] = match
    const start = source.lastIndexOf('\n', match.index) + 1
    const line = source.slice(start, source.indexOf('\n', match.index))
    // The guard may sit on this line, joined by ||, or on the line above as the
    // `if` half of an if/else -- which is the shape the store settled on, because
    // one `||` over two operands lowered to a single condition the census read as
    // one, and two statements cannot be got wrong.
    const before = source.slice(source.lastIndexOf('\n', start - 2) + 1, start)
    // A list the store itself sizes once, and indexes within that size, has no
    // growing edge to skip -- `enabled` is six blocks, initialised as six, and
    // the browse rows are four, initialised as four.
    if (new RegExp(`${list}\\b[^=\\n]*=\\s*\\[[^\\]]+\\]`).test(source)) continue
    const guard = `${index} >= this.${list}.length`
    assert.ok(
      line.includes(`${guard} ||`) || before.includes(guard),
      `this.${list}[${index}] is compared without a length guard, which aborts on the board:\n  ${line.trim()}`,
    )
  }
})
