import { mount } from '@geastack/core'
import { App } from '../src/ui/components/App'
import { pedalboard } from '../src/ui/stores/PedalboardStore'
import { board } from '../src/ui/board'

// The browser entry runs the firmware's own UI — same components, same store,
// same C++ after geatsc — against the web board bridge in board_bridge_web.cpp.
// src/ui/index.tsx's __gea_Display.setVSync/setFrameRate calls stay out of it:
// they tune the QSPI panel's TE line and the frame cadence the audio graph
// leaves room for, and neither exists here.
pedalboard.sync()
mount(App)

// Same poll as the firmware entry: the board bumps a revision counter when
// anything changes underneath the UI, and the tuner needs a 10 Hz resample.
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
