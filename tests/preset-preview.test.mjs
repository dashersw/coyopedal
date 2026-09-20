import test from 'node:test'
import assert from 'node:assert/strict'
import { createPanelPreviewStore } from '../src/preview/board-fixture.mjs'

const memory = () => {
  const data = new Map()
  return { getItem: (key) => data.get(key), setItem: (key, value) => data.set(key, value) }
}
test('save and reload restores amp, bypass, effects and parameters independently of master/tuner', () => {
  const storage = memory(),
    s = createPanelPreviewStore(storage)
  s.action(0, 3)
  s.action(7)
  s.action(1, 5)
  s.set(7, 0)
  s.action(6, 2, 0.82)
  assert.equal(s.get(18), 1)
  s.action(9)
  assert.equal(s.get(18), 0)
  s.action(3)
  s.action(4)
  assert.equal(s.get(18), 0)
  s.action(8, 2)
  assert.equal(s.get(12), 1)
  assert.equal(s.get(16), 1)
  s.action(8, 0)
  assert.equal(s.get(12), 3)
  assert.equal(s.get(16), 0)
  assert.equal(s.get(25), 1)
  assert.equal(s.get(132), 0.82)
  assert.equal(s.get(10), 0)
  assert.equal(s.get(11), 1)
  const reopened = createPanelPreviewStore(storage)
  assert.equal(reopened.get(12), 3)
  assert.equal(reopened.get(132), 0.82)
  assert.equal(reopened.get(16), 0)
})

test('storage failure keeps edits and does not switch or rename', () => {
  const s = createPanelPreviewStore({
    getItem: () => null,
    setItem: () => {
      throw Error('quota')
    },
  })
  s.action(7)
  s.action(11, 1)
  assert.equal(s.get(17), 0)
  assert.equal(s.get(18), 1)
  assert.match(s.label(5), /Cannot save/)
  s.nameEdit(0, 'Changed')
  s.nameEdit(3, '')
  assert.equal(s.label(9), 'Rhythm')
})

test('rename validation and creating a preset preserve the original saved sound', () => {
  const storage = memory(),
    s = createPanelPreviewStore(storage)
  s.nameEdit(0, '   ')
  s.nameEdit(3, '')
  assert.match(s.label(5), /Enter/)
  s.nameEdit(0, 'A'.repeat(30))
  assert.equal(s.label(11).length, 23)
  s.nameEdit(0, 'Heavy rhythm')
  s.nameEdit(3, '')
  assert.equal(s.label(9), 'Heavy rhythm')
  s.action(7)
  s.set(56, 1)
  s.nameEdit(0, 'Heavy lead')
  s.nameEdit(3, '')
  assert.equal(s.get(17), 4)
  assert.equal(s.label(9), 'Heavy lead')
  assert.equal(s.get(16), 0)
  assert.equal(s.get(26), 5)
  assert.equal(createPanelPreviewStore(storage).get(26), 5)
  s.action(8, 0)
  assert.equal(s.get(16), 1)
  assert.equal(createPanelPreviewStore(storage).label(9), 'Heavy rhythm')
})

test('invalid persisted presets fall back to usable defaults', () => {
  for (const text of ['{', '[]', '[null,null,null,null]']) {
    const s = createPanelPreviewStore({ getItem: () => text })
    assert.equal(s.label(9), 'Rhythm')
    assert.equal(s.get(26), 4)
    assert.equal(s.get(18), 0)
  }
})
