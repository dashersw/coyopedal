import { createPanelPreviewStore } from './board-fixture.mjs'
import './preview.css'
import { installMouseScroll } from './mouse-scroll'

let storage: Storage | undefined
try {
  storage = window.localStorage
} catch {
  /* The preview reports unavailable storage when saving. */
}
const fixture = createPanelPreviewStore(storage)
fixture.reset()
const root = document.getElementById('app')!
// Keys 1 and 2 take a pointer's page position and hold it in panel points.
function set(key: number, value: number) {
  const bounds = root.getBoundingClientRect()
  if (key === 1) value = ((value - bounds.left) * 251) / bounds.width
  if (key === 2) value = ((value - bounds.top) * 205) / bounds.height
  fixture.set(key, value)
}
Object.assign(globalThis, {
  pbGet: fixture.get,
  pbSet: set,
  pbLabel: fixture.label,
  pbAction: fixture.action,
  pbPresetName: fixture.nameEdit,
  // The browser paces its own frames; the board's display pacing is a no-op here.
  __gea_Display: { setVSync() {}, setFrameRate() {} },
})
await import('../ui/index')
const { pedalboard } = await import('../ui/stores/PedalboardStore')

installMouseScroll(root)
new ResizeObserver(() => {
  root.style.setProperty('--panel-scale', String(root.clientWidth / 251))
  root.style.height = `${(root.clientWidth * 205) / 251}px`
}).observe(root)

function time(now: number) {
  fixture.set(13, now)
  requestAnimationFrame(time)
}
requestAnimationFrame(time)
document.getElementById('reset')!.onclick = () => {
  fixture.reset()
  pedalboard.sync()
}
document.getElementById('pitch')!.onchange = (event) => {
  fixture.set(62, Number((event.target as HTMLSelectElement).value))
  pedalboard.sync()
}
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    pedalboard.back()
    event.preventDefault()
    return
  }
  if (fixture.get(4) === 6 && !event.metaKey && !event.ctrlKey) {
    if (event.key === 'Backspace') fixture.nameEdit(2, '')
    else if (event.key === 'Enter') fixture.nameEdit(3, '')
    else if (event.key.length === 1) fixture.nameEdit(1, event.key)
    else return
    pedalboard.sync()
    event.preventDefault()
  }
})
