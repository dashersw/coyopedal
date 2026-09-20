import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store, categories } from '../stores/PedalboardStore'
import { pages, ROWS_PER_PAGE } from '../stores/PageStore'
import './Chain.css'

// The amp rides in the chain as a seventh slot; effects keep their board slots.
const AMP = 6
// Short enough for a 32px column.
const labels = ['Gate', 'Comp', 'Drive', 'Verb', 'Mod', 'Delay', 'Amp']

// A tap switches a block; holding it opens the block. Nothing on this screen
// animates, so the hold is timed off animation frames only while a finger is down.
// The switch itself rides on click, which the panel delivers only for a tap that
// neither dragged nor outlived the hold; any pointer up disarms the hold, in
// whatever order the dot and the screen receive it.
const HOLD_MS = 450
let heldSlot = -1
let heldSince = -1
let holdOpened = false

function watchHold(now: number) {
  if (heldSlot < 0) return
  if (heldSince < 0) heldSince = now
  if (now - heldSince < HOLD_MS) {
    requestAnimationFrame(watchHold)
    return
  }
  const slot = heldSlot
  heldSlot = -1
  holdOpened = true
  if (slot === AMP) {
    store.show(1)
    return
  }
  pages.editor = 0
  store.show(2, slot)
}

function openPresets() {
  pages.presets = Math.floor(store.activePreset / ROWS_PER_PAGE)
  store.show(3)
}

function press(slot: number) {
  heldSlot = slot
  heldSince = -1
  holdOpened = false
  requestAnimationFrame(watchHold)
}

function toggle(slot: number) {
  heldSlot = -1
  if (holdOpened) return
  if (slot === AMP) store.command(7)
  else store.command(1, slot)
}

function step(delta: number) {
  const count = store.presets.length
  if (count < 2) return
  store.choosePreset((store.activePreset + delta + count) % count)
}

function twoDigits(n: number) {
  return n < 10 ? `0${n}` : `${n}`
}

class BlockDot extends Component<GeaElement, { slot: number }> {
  template({ slot }: { slot: number }) {
    // One element tree, toggled by a single `lit` class. Swapping elements (or
    // swapping between two COLOUR classes) rebuilds the subtree, and a
    // structural rebuild makes the engine repaint the whole viewport -- one tap
    // used to cost all 502x410 pixels because every dot shares the `enabled`
    // revision. Toggling one class instead lets the engine diff the recomputed
    // style, see that only paint changed, and dirty just this dot's box. The
    // slot's colours ride custom properties on the row, so the rules below stay
    // slot-agnostic. The state is read from the store inside each table,
    // because the native build evaluates a local derived from the store once.
    return (
      <div
        class={`block-dot slot-${slot}`}
        role="switch"
        aria-checked={slot === AMP ? store.ampEnabled : store.enabled[slot]}
        aria-label={slot === AMP ? 'Amp' : categories[slot]}
        onPointerDown={() => press(slot)}
        onClick={() => toggle(slot)}
      >
        <div
          class={{ 'dot-halo': true, lit: slot === AMP ? store.ampEnabled : store.enabled[slot] }}
        >
          <div class={{ dot: true, lit: slot === AMP ? store.ampEnabled : store.enabled[slot] }} />
        </div>
        <span
          class={{
            'block-label': true,
            lit: slot === AMP ? store.ampEnabled : store.enabled[slot],
          }}
        >
          {labels[slot]}
        </span>
      </div>
    )
  }
}

export class Chain extends Component {
  template() {
    return (
      // A finger lifted off every block must not leave a hold armed.
      <div onPointerUp={() => (heldSlot = -1)}>
        <button class="preset-step previous" aria-label="Previous preset" onClick={() => step(-1)}>
          <span class="step-chevron">{'‹'}</span>
        </button>
        <button class="preset-step next" aria-label="Next preset" onClick={() => step(1)}>
          <span class="step-chevron">{'›'}</span>
        </button>
        <button class="preset-hero" onClick={() => openPresets()}>
          {/* One expression, so the native side gets a single text leaf it can
              center; three runs would become a row of left-aligned children. */}
          <span class="preset-count">
            {`${twoDigits(store.activePreset + 1)} / ${twoDigits(store.presets.length)}`}
          </span>
          <span class="preset-hero-name">{store.presetName}</span>
          <span class="preset-hero-amp">{store.ampName}</span>
        </button>
        <div class="signal-line" />
        <div class="dots">
          <BlockDot slot={0} />
          <BlockDot slot={1} />
          <BlockDot slot={2} />
          <BlockDot slot={AMP} />
          <BlockDot slot={4} />
          <BlockDot slot={5} />
          <BlockDot slot={3} />
        </div>
        <button class="home-pill tune" onClick={() => store.command(4)}>
          Tuner
        </button>
        <button class="home-pill master" onClick={() => store.command(3)}>
          <span class={{ 'status-dot': true, lit: store.engaged }} />
          <span>{store.engaged ? 'On' : 'Off'}</span>
        </button>
      </div>
    )
  }
}
