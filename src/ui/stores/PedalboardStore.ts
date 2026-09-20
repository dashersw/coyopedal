import { Store } from '@geastack/core'
import { board } from '../board'

export const categories = ['Gate', 'Compressor', 'Drive', 'Reverb', 'Modulation', 'Delay']
export const colors = ['#e5c875', '#91bdd0', '#82bca9', '#d39aad', '#b9a6d8', '#da8864']

export class PedalboardStore extends Store {
  sliderIndex = -1
  sliderDragging = false
  sliderStartX = 0
  sliderStartY = 0
  loading = false
  screen = 0
  selected = 0
  presetName = ''
  ampName = ''
  engaged = true
  ampEnabled = true
  enabled = [true, true, true, false, false, false]
  edited = false
  tuner = false
  voiced = false
  cents = 0
  note = ''
  frequency = ''
  inTune = false
  tunerColor = '#93a0aa'
  tunerStatus = 'Play a string'
  needlePercent = 50
  error = ''
  status = ''
  draft = ''
  keyboardMode = 1
  creating = false
  deleting = false
  pendingName = ''
  activePreset = 0
  activeModel = 0
  network = ''
  address = ''
  presets: string[] = []
  // One page of the amp browser, never the whole catalogue: the UI tree holds
  // a few hundred nodes and a row costs about five, so a card full of captures
  // could not be rendered at all. Native owns the folder walk and the paging
  // (host keys 27-31, 32-35 and 44-47), including which rows are folders.
  //
  // Four rows, ALWAYS four, padded with nothing when the page has fewer. These
  // arrays are the one place the shrink is not allowed to happen, because a row
  // already on screen is asked for its own title once more before the condition
  // that holds it goes false -- and a store array's indexed read past the end is
  // fatal, not undefined (see the note in sync()). Walking into a folder with
  // one capture from a folder with three aborted the runtime exactly there,
  // every time, on the row that was about to disappear. Padded rows make that
  // last read legal; browseRows is what says how many of the four are real, and
  // it is four because that is kBrowseRowsPerPage in board_bridge.cpp.
  browseRows = 0
  browseTitles: string[] = ['', '', '', '']
  browseSubtitles: string[] = ['', '', '', '']
  browseActive: boolean[] = [false, false, false, false]
  browseFolder: boolean[] = [false, false, false, false]
  // The row that asks for a card. It is not a folder and not a capture: see
  // browseEnter, which is the only thing that reads it.
  browseCard: boolean[] = [false, false, false, false]
  browseLevel = 0
  browsePageIndex = 0
  browsePageCount = 1
  browseTotal = 0
  browseTitle = ''
  browseSubtitle = ''
  parameterNames: string[] = []
  parameterLabels: string[] = []
  parameterValues: number[] = []

  // A SCALAR is written straight, with no change guard. Every scalar field of a
  // store compiles to a `gea::embedded::ui::Signal<T>`, whose `operator=`
  // already returns without notifying when the value compares equal -- it
  // notifies on CHANGE, not on assignment. Guarding here only restates that,
  // and it was measured to change nothing: the frame's structural marks and its
  // setText counts came back identical with the guards and without them.
  //
  // The lists below are the exception, and the comment there says why.
  sync() {
    this.screen = board.get(4)
    this.selected = board.get(5)
    this.presetName = board.label(9, 0)
    this.ampName = board.label(0, 0)
    this.engaged = board.get(10) !== 0
    this.ampEnabled = board.get(16) !== 0
    for (let i = 0; i < 6; i++) {
      const on = board.get(20 + i) !== 0
      if (this.enabled[i] !== on) this.enabled[i] = on
    }
    this.edited = board.get(18) !== 0
    this.tuner = board.get(11) !== 0
    // Nothing outside the tuner screen reads any of this, and the status line
    // builds a string every time it is computed. sync() runs on every note, so
    // that was an allocation per note to feed a screen that is not on.
    if (this.tuner) {
      this.voiced = board.get(60) !== 0
      this.cents = board.get(61)
      this.note = this.voiced ? board.label(13, 0) : '—'
      this.frequency = this.voiced ? board.label(14, 0) : ''
      // Explicit fields keep these bindings reactive in the native compiler too.
      this.inTune = this.voiced && Math.abs(this.cents) <= 3
      this.tunerColor = this.inTune ? '#82bca9' : this.voiced ? '#e5c875' : '#93a0aa'
      this.tunerStatus = !this.voiced
        ? 'Play a string'
        : this.inTune
          ? 'In tune'
          : `${Math.round(Math.abs(this.cents))} cents ${this.cents < 0 ? 'flat' : 'sharp'}`
      this.needlePercent = 50 + Math.max(-50, Math.min(50, this.cents))
    }
    this.error = board.label(5, 0)
    this.status = board.label(12, 0)
    this.draft = board.label(11, 0)
    this.keyboardMode = board.get(55)
    this.creating = board.get(56) !== 0
    this.deleting = board.get(58) !== 0
    this.activePreset = board.get(17)
    this.activeModel = board.get(12)
    this.pendingName = board.label(10, this.deleting ? board.get(57) : this.activePreset)
    this.network = board.label(15, 0)
    this.address = board.label(16, 0)
    // Every list below is written element by element, and only where the element
    // actually changed.
    //
    // Both halves of that matter. A store field holding an array is rendered
    // through a signal the compiler notifies on element assignment -- and on
    // length -- but NOT on assignment of the whole field, so
    // `this.presets = presets` drew the list once and then never again: the
    // preset list stayed empty and the amp browser froze on its top level,
    // showing its top level however deep the walk had gone. The header kept
    // updating because a string field is bound directly rather than through a
    // list signal, which is exactly what it looked like from the outside.
    //
    // And writing unconditionally notifies on every sync, which rebuilds the
    // row nodes for nothing. That is wasted work rather than a correctness
    // problem -- sync() runs when host key 15 moves, and that revision tracks
    // real UI state, not the audio, so it does not move while a note decays.
    //
    // Each list is also read only by the screen that is showing it: a page of
    // the amp browser, the whole preset list and the editor's parameters were
    // read on every sync for three screens that are not on, and every label
    // crosses the host boundary and comes back as a std::string, so it is
    // allocation too. The count is the exception and is read every sync,
    // because something off this screen can add or remove a preset; the names,
    // which are the expensive half, wait for the screen or for the count to
    // move under it.
    //
    // The change guard compares before it writes, and the comparison itself has
    // to be legal. A store array is a native container on the board, and its
    // indexed read ABORTS on an index at or past the end -- "gea: read of array
    // hole or out-of-range index ...; absence has no carrier here" -- where JS
    // quietly answers undefined. So the growing edge is written, never
    // compared: at or past the end there is nothing to compare against, and
    // asking cost a black screen on the first sync, before a single frame.
    //
    // Written as `if (past the end) write; else if (changed) write` rather than
    // as one guarded comparison. `i >= length || array[i] !== value` is the same
    // thing in TypeScript and reads better, but it puts the illegal read inside
    // an expression whose left half is the only thing keeping it legal, and a
    // seam this sharp should not rest on where the short circuit ends up in the
    // lowered code. Two statements cannot be got wrong.
    //
    // The arrays in each group are also written in a deliberate order: the one a
    // screen MAPS OVER is grown last and shrunk first, so its length is never
    // the longest of the group. Amplifiers.tsx maps browseTitles and reads the
    // other three at that row; Editor.tsx maps parameterNames and reads the
    // other two. Grow the mapped one first and there is an instant where it has
    // three rows and its companions have one -- which is a render away from the
    // very abort described above, and cost one in testing on a folder that had
    // just been picked.
    const presetCount = board.get(26)
    if (this.screen === 3 || this.presets.length !== presetCount) {
      for (let i = 0; i < presetCount; i++) {
        const name = board.label(10, i)
        if (i >= this.presets.length) this.presets[i] = name
        else if (this.presets[i] !== name) this.presets[i] = name
      }
    }
    if (this.presets.length !== presetCount) this.presets.length = presetCount
    if (this.screen === 1) {
      this.browseLevel = board.get(31)
      this.browsePageIndex = board.get(29)
      this.browsePageCount = board.get(30)
      this.browseTotal = board.get(28)
      this.browseTitle = board.label(19, 0)
      this.browseSubtitle = board.label(20, 0)
    }
    // The count goes first and the rows follow it, so a row is never dropped
    // while the screen still believes in it. All four slots are written every
    // time, empty ones included: nothing is resized here, ever.
    const browseRows = this.screen === 1 ? board.get(27) : this.browseRows
    if (this.browseRows !== browseRows) this.browseRows = browseRows
    for (let i = 0; this.screen === 1 && i < 4; i++) {
      const title = i < browseRows ? board.label(17, i) : ''
      const subtitle = i < browseRows ? board.label(18, i) : ''
      const active = i < browseRows && board.get(32 + i) !== 0
      const folder = i < browseRows && board.get(44 + i) !== 0
      const card = i < browseRows && board.get(48 + i) !== 0
      if (this.browseTitles[i] !== title) this.browseTitles[i] = title
      if (this.browseSubtitles[i] !== subtitle) this.browseSubtitles[i] = subtitle
      if (this.browseActive[i] !== active) this.browseActive[i] = active
      if (this.browseFolder[i] !== folder) this.browseFolder[i] = folder
      if (this.browseCard[i] !== card) this.browseCard[i] = card
    }
    const parameters = this.screen === 2 ? board.get(9) : this.parameterNames.length
    for (let i = 0; this.screen === 2 && i < parameters; i++) {
      const key = this.selected * 16 + i
      const name = board.label(6, key)
      const label = board.label(7, key)
      const value = board.get(100 + key)
      if (i >= this.parameterLabels.length) this.parameterLabels[i] = label
      else if (this.parameterLabels[i] !== label) this.parameterLabels[i] = label
      if (i >= this.parameterValues.length) this.parameterValues[i] = value
      else if (this.parameterValues[i] !== value) this.parameterValues[i] = value
      if (i >= this.parameterNames.length) this.parameterNames[i] = name
      else if (this.parameterNames[i] !== name) this.parameterNames[i] = name
    }
    if (this.parameterNames.length !== parameters) this.parameterNames.length = parameters
    if (this.parameterLabels.length !== parameters) this.parameterLabels.length = parameters
    if (this.parameterValues.length !== parameters) this.parameterValues.length = parameters
  }

  get title() {
    return categories[this.selected]
  }
  get accent() {
    return colors[this.selected]
  }
  show(screen: number, selected = 0) {
    this.sliderIndex = -1
    board.set(4, screen)
    board.set(5, selected)
    this.sync()
  }
  // Row activation: a folder descends, a capture loads, and the card row asks
  // for a card. Only the capture blocks, so only it needs the two-frame handoff
  // that lets "Preparing amp…" present before on-device preparation stalls the
  // render task.
  //
  // The card row must NOT take that handoff, and not only because there is no
  // amp to prepare. In a browser the card is a folder picker, and a picker
  // opens only from inside the gesture that asked for it: two animation frames
  // later WebKit has dropped the gesture, so the row flashed "Preparing amp…"
  // and nothing opened. Safari is where that shows, since it is the browser
  // with no other way in.
  browseEnter(row: number) {
    if (row >= this.browseRows) return
    if (this.browseFolder[row] || this.browseCard[row]) {
      this.command(14, row)
      return
    }
    if (this.loading) return
    this.loading = true
    requestAnimationFrame(() =>
      requestAnimationFrame(() => {
        this.command(14, row)
        this.loading = false
      }),
    )
  }
  browsePage(delta: number) {
    this.command(16, 0, delta)
  }
  command(action: number, index = 0, value = 0) {
    board.action(action, index, value)
    this.sync()
  }
  back() {
    if (this.tuner) this.command(4)
    if (this.screen === 1) this.command(15)
    else if (this.screen === 9) this.command(13)
    else
      this.show(
        this.screen === 6 || this.screen === 7 || this.screen === 8
          ? 3
          : this.screen === 10
            ? 8
            : 0,
      )
  }
  choosePreset(index: number) {
    if (index === this.activePreset) this.show(0)
    else if (this.edited) {
      board.set(57, index)
      board.set(58, 0)
      this.show(7)
    } else this.command(8, index)
  }
  rename(index: number) {
    board.set(54, index)
    board.set(56, 0)
    board.nameEdit(0, this.presets[index])
    this.show(6)
  }
  create() {
    board.set(56, 1)
    board.nameEdit(0, '')
    this.show(6)
  }
  remove(index: number) {
    if (this.presets.length <= 1) return
    board.set(57, index)
    board.set(58, 1)
    this.show(7)
  }
  confirm(save: boolean) {
    this.command(this.deleting ? 12 : save ? 11 : 8, board.get(57))
  }
  enterMaintenance(save: boolean) {
    if (this.edited && this.screen !== 10) {
      this.show(10)
      return
    }
    if (save) this.command(9)
    if (!this.error) this.command(13)
  }
  editName(action: number, text = '') {
    board.nameEdit(action, text)
    this.sync()
  }
  changeKeyboard(mode: number) {
    board.set(55, mode)
    this.sync()
  }
  sliderDown(index: number, x: number, y: number) {
    board.pointer(x, y)
    this.sliderIndex = index
    this.sliderDragging = false
    this.sliderStartX = board.get(1)
    this.sliderStartY = board.get(2)
  }
  sliderMove(index: number, x: number, y: number) {
    if (this.sliderIndex !== index) return
    board.pointer(x, y)
    const dx = Math.abs(board.get(1) - this.sliderStartX)
    const dy = Math.abs(board.get(2) - this.sliderStartY)
    if (!this.sliderDragging && dy > 6 && dy > dx) {
      this.sliderIndex = -1
      return
    }
    if (dx > 6) this.sliderDragging = true
    if (this.sliderDragging) this.parameter(index, (board.get(1) - 29) / 193)
  }
  sliderUp(index: number, x: number, y: number) {
    board.pointer(x, y)
    if (this.sliderIndex === index) this.parameter(index, (board.get(1) - 29) / 193)
    this.sliderIndex = -1
  }
  parameter(index: number, normalized: number) {
    board.set(7, index)
    this.command(6, this.selected, Math.max(0, Math.min(1, normalized)))
  }
}

export const pedalboard = new PedalboardStore()
