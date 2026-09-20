// The amp browser walks the capture library as folders, a page at a time,
// because a row per capture does not fit: the UI tree holds a few hundred nodes
// and a row costs about five, and Tree::createNode() returns -1 past the cap
// without logging, so an oversized list silently never appears.
//
// So what these tests pin is that the number of rows the component renders is
// bounded by the page size and never a function of how large the library is.
// They also pin the walk itself: the top holds the factory captures and the SD
// card, the card's own folders come next, a folder holding a single subfolder is
// passed through, and a capture row loads that capture.
import test, { after, beforeEach } from 'node:test'
import assert from 'node:assert/strict'
import { createServer } from 'vite'
import { resolve } from 'node:path'
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
  requestAnimationFrame: (callback) => callback(0),
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

const PAGE = 4

// The store keeps its row arrays PAGE long whatever the page holds, and says how
// many of them are rows in browseRows -- because a component that resized them
// would read one past the end on the frame a row leaves, which is fatal rather
// than undefined. So a test that wants "the rows on this page" asks for that
// many. See the note on Amplifiers.tsx.
const shown = (field) => store[field].slice(0, store.browseRows)

// The browser keeps its place across visits, which is the point of a folder
// walk, so a test that wants a known place starts from the top.
const openTop = () => {
  store.show(1)
  while (store.browseLevel > 0) store.back()
  assert.equal(store.browseLevel, 0)
}

const enterFolder = (name) => {
  for (;;) {
    const row = store.browseTitles.indexOf(name)
    if (row >= 0) {
      assert.ok(store.browseFolder[row], `${name} is not a folder`)
      store.browseEnter(row)
      return
    }
    assert.ok(store.browsePageIndex + 1 < store.browsePageCount, `${name} not found`)
    store.browsePage(1)
  }
}

test('the top holds the factory captures and the SD card', () => {
  openTop()
  assert.equal(store.screen, 1)
  assert.equal(store.browseTitle, 'Amplifiers')
  // Folders come first, and say how many captures they hold.
  assert.equal(store.browseTitles[0], 'SD card')
  assert.equal(store.browseFolder[0], true)
  assert.match(store.browseSubtitles[0], /^\d+ captures$/)
  assert.deepEqual(shown('browseTitles').slice(1), ['Diezel Herbert C1 V30', 'Ampete One C4 V30'])
  assert.deepEqual(shown('browseFolder').slice(1), [false, false])
})

test('a folder descends and the header walks back up', () => {
  openTop()
  enterFolder('SD card')
  assert.equal(store.browseLevel, 1)
  assert.equal(store.browseTitle, 'SD card')
  enterFolder('Crunch')
  assert.equal(store.browseLevel, 2)
  assert.equal(store.browseTitle, 'Crunch')
  enterFolder('Marshall JCM800')
  assert.equal(store.browseLevel, 3)
  assert.deepEqual(shown('browseTitles'), ['C1 G12', 'C1 G65', 'C1 V30'])
  assert.ok(shown('browseFolder').every((folder) => !folder))

  // back() unwinds the folders before it leaves the screen.
  store.back()
  assert.equal(store.browseTitle, 'Crunch')
  store.back()
  assert.equal(store.browseTitle, 'SD card')
  store.back()
  assert.equal(store.browseLevel, 0)
  store.back()
  assert.equal(store.screen, 0)
})

test('a folder holding a single subfolder is passed through both ways', () => {
  openTop()
  enterFolder('SD card')
  // "High gain" holds only "Peavey 5150", so entering it lands on the captures.
  enterFolder('High gain')
  assert.equal(store.browseTitle, 'Peavey 5150')
  assert.equal(store.browseLevel, 3)
  store.back()
  assert.equal(store.browseTitle, 'SD card')
})

test('activating a capture loads the one the row names', () => {
  openTop()
  enterFolder('SD card')
  enterFolder('Rigs')
  const name = store.browseTitles[1]
  store.browseEnter(1)
  assert.equal(store.screen, 0)
  assert.equal(store.ampName, name)
  assert.equal(store.loading, false)
  store.show(1)
  // The playing capture is marked where it sits.
  assert.equal(store.browseActive[1], true)
  assert.equal(store.browseSubtitles[1], '')
})

test('a page never renders more rows than the page size', () => {
  openTop()
  const walk = () => {
    for (let page = 0; page < store.browsePageCount; page++) {
      assert.ok(store.browseRows <= PAGE)
      assert.equal(store.browseTitles.length, PAGE)
      assert.equal(store.browseSubtitles.length, PAGE)
      assert.equal(store.browseFolder.length, PAGE)
      store.browsePage(1)
    }
  }
  walk()
  enterFolder('SD card')
  walk()
  enterFolder('Crunch')
  walk()
})

test('paging is clamped at both ends and covers every row exactly once', () => {
  openTop()
  enterFolder('SD card')
  enterFolder('Crunch')
  assert.ok(store.browsePageCount > 1)
  store.browsePage(-1)
  assert.equal(store.browsePageIndex, 0)

  const seen = []
  for (let page = 0; page < store.browsePageCount; page++) {
    seen.push(...shown('browseTitles'))
    store.browsePage(1)
  }
  // One past the last page stays on the last page rather than emptying the list.
  store.browsePage(1)
  assert.equal(store.browsePageIndex, store.browsePageCount - 1)
  assert.ok(store.browseRows > 0)

  assert.equal(seen.length, store.browseTotal)
  assert.equal(new Set(seen).size, seen.length)
})
