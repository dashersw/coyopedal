import { mount } from '@geastack/core'
import { App } from './components/App'
import { pedalboard } from './stores/PedalboardStore'
import { board } from './board'

// The host display object itself: importing core's Display facade compiles
// every member of it, and its invalidate() has no native spelling here.
declare const __gea_Display: {
  setVSync(on: boolean): void
  setFrameRate(fps: number): void
}

// The UI shares both cores with the audio graph, and vsync stays off so the
// panel's TE interrupt is never armed. The frame rate sets tap latency: a tap
// waits half a frame interval just to be noticed, so 15 fps spends ~33 ms there.
// Raising it to 60 was tried and reverted: the tap felt identical either way,
// because what actually made the panel unusable in audio mode was the flush
// pipeline sitting at its 1-row floor, not the frame cadence.
__gea_Display.setVSync(false)
__gea_Display.setFrameRate(15)

pedalboard.sync()
mount(App)

let revision = board.get(15)
let lastTunerSample = 0
function frame(now: number) {
  const nextRevision = board.get(15)
  if (nextRevision !== revision || (pedalboard.tuner && now - lastTunerSample >= 100)) {
    revision = nextRevision
    lastTunerSample = now
    pedalboard.sync()
  }
  requestAnimationFrame(frame)
}
requestAnimationFrame(frame)
