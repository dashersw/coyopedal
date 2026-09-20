// Browser-only fixture adapter. No writes to the board or production presets.
export function createPanelPreviewStore(storage) {
  const key = 'pedalboard-preset-preview-v1'
  const names = ['Hard Gate', 'Studio VCA', 'Klon', 'Spring', 'Chorus', 'Digital']
  const paramNames = [
    ['Threshold', 'Attack', 'Release'],
    ['Threshold', 'Ratio', 'Attack', 'Release', 'Makeup'],
    ['Gain', 'Tone', 'Level'],
    ['Mix', 'Decay', 'Tone', 'Width'],
    ['Rate', 'Depth', 'Mix'],
    ['Time', 'Feedback', 'Mix'],
  ]
  const ranges = [
    [
      [-90, 0],
      [0, 10],
      [10, 500],
    ],
    [
      [-60, 0],
      [1, 20],
      [1, 100],
      [10, 500],
      [0, 20],
    ],
    [
      [0, 100],
      [0, 100],
      [0, 100],
    ],
    [
      [0, 100],
      [0, 100],
      [0, 100],
      [0, 100],
    ],
    [
      [0.1, 10],
      [0, 100],
      [0, 100],
    ],
    [
      [10, 1000],
      [0, 95],
      [0, 100],
    ],
  ]
  // The catalogue, as the firmware's browser sees it: the factory captures at
  // the top and the SD card's own folders under "SD card", so the preview walks
  // the same tree -- paging and the pass through a folder holding a single
  // subfolder included.
  const catalogue = [
    { folder: '', name: 'Diezel Herbert C1 V30' },
    { folder: '', name: 'Ampete One C4 V30' },
  ]
  const card = (folder, ...names) => {
    for (const name of names) catalogue.push({ folder: `SD card/${folder}/`, name })
  }
  card('Clean/Deluxe Reverb', 'C1 G12', 'C1 G65', 'C1 V30')
  card('Clean/Vox AC30', 'C1 G12', 'C1 V30', 'C2 G12', 'C2 V30')
  card('Crunch/Marshall JCM800', 'C1 G12', 'C1 G65', 'C1 V30')
  card('Crunch/Orange OR120', 'C1 V30')
  card('Crunch/Bogner XTC', 'C1 V30')
  card('Crunch/Friedman BE', 'C1 V30')
  card('Crunch/Plexi', 'C1 V30')
  card('High gain/Peavey 5150', 'C2 V30', 'C3 V30')
  card('Rigs/Mesa Mark', 'Full rig 1', 'Full rig 2', 'Full rig 3')
  const captures = catalogue.map((c) => c.name)

  const browsePageSize = 4
  const browse = { prefix: '', page: 0 }

  const childOf = (folder) => folder.slice(browse.prefix.length).split('/')[0]
  // Subfolders first, then captures, each in catalogue order.
  const browseRows = () => {
    const folders = []
    const files = []
    catalogue.forEach((c, model) => {
      if (!c.folder.startsWith(browse.prefix)) return
      if (c.folder === browse.prefix) {
        files.push({ model, folder: false, under: 0 })
        return
      }
      const name = childOf(c.folder)
      const row = folders.find((r) => childOf(catalogue[r.model].folder) === name)
      if (row) row.under += 1
      else folders.push({ model, folder: true, under: 1 })
    })
    return [...folders, ...files]
  }
  const browsePageCount = () => Math.max(1, Math.ceil(browseRows().length / browsePageSize))
  const browsePage = () => Math.min(browse.page, browsePageCount() - 1)
  const browseWindow = () =>
    browseRows().slice(
      browsePage() * browsePageSize,
      browsePage() * browsePageSize + browsePageSize,
    )
  const browseDepth = () => browse.prefix.split('/').length - 1
  const singleFolder = () => {
    const rows = browseRows()
    return rows.length === 1 && rows[0].folder
  }
  const enter = (model) => {
    browse.prefix += `${childOf(catalogue[model].folder)}/`
    browse.page = 0
  }
  const browseDescend = (row) => {
    const entry = browseWindow()[row]
    if (!entry?.folder) return
    enter(entry.model)
    while (singleFolder()) enter(browseRows()[0].model)
  }
  const browseAscend = () => {
    do {
      browse.prefix = browse.prefix.slice(0, -1)
      browse.prefix = browse.prefix.slice(0, browse.prefix.lastIndexOf('/') + 1)
      browse.page = 0
    } while (browse.prefix && singleFolder())
  }
  const browseTitle = () =>
    browse.prefix ? browse.prefix.slice(0, -1).split('/').at(-1) : 'Amplifiers'
  const defaults = [
    [-60, 0.2, 180],
    [-18, 4, 10, 150, 0],
    [40, 55, 70],
    [28, 45, 52, 60],
    [1.2, 45, 35],
    [380, 30, 28],
  ]
  const copy = (value) => JSON.parse(JSON.stringify(value))
  const fresh = (name, model, enabled) => ({
    name,
    model,
    ampEnabled: true,
    enabled,
    params: copy(defaults),
  })
  let presets = [
    fresh('Rhythm', 0, [1, 1, 1, 0, 0, 0]),
    fresh('Lead', 0, [1, 1, 1, 0, 0, 1]),
    fresh('Clean', 1, [1, 1, 0, 0, 1, 0]),
    fresh('Ambient', 2, [1, 0, 0, 1, 1, 1]),
  ]
  presets[1].params[5] = [440, 36, 24]
  presets[3].params[3] = [52, 75, 45, 85]
  let error = '',
    status = '',
    draft = '',
    params = copy(defaults)
  const state = {}
  const valid = (p) =>
    p &&
    typeof p.name === 'string' &&
    /^[\x20-\x7e]{1,23}$/.test(p.name) &&
    p.name.trim() &&
    Number.isInteger(p.model) &&
    p.model >= 0 &&
    p.model < captures.length &&
    typeof p.ampEnabled === 'boolean' &&
    Array.isArray(p.enabled) &&
    p.enabled.length === 6 &&
    p.enabled.every((v) => v === 0 || v === 1) &&
    Array.isArray(p.params) &&
    p.params.length === ranges.length &&
    p.params.every(
      (row, s) =>
        Array.isArray(row) &&
        row.length === ranges[s].length &&
        row.every((v, i) => Number.isFinite(v) && v >= ranges[s][i][0] && v <= ranges[s][i][1]),
    )
  try {
    const saved = JSON.parse(storage?.getItem(key) || 'null')
    if (Array.isArray(saved) && saved.length >= 1 && saved.every(valid)) presets = saved
  } catch {
    // Missing or invalid browser storage starts with the factory presets.
  }
  function config(p) {
    return { model: p.model, ampEnabled: p.ampEnabled, enabled: p.enabled, params: p.params }
  }
  function live() {
    return {
      model: state[12],
      ampEnabled: !!state[16],
      enabled: Array.from({ length: 6 }, (_, i) => (state[20 + i] ? 1 : 0)),
      params: copy(params),
    }
  }
  function edited() {
    return JSON.stringify(live()) !== JSON.stringify(config(presets[state[17]]))
  }
  function load(index) {
    if (!Number.isInteger(index) || !presets[index]) return
    const p = presets[index]
    state[17] = index
    state[12] = p.model
    state[16] = p.ampEnabled ? 1 : 0
    p.enabled.forEach((v, i) => (state[20 + i] = v))
    params = copy(p.params)
    state[4] = 0
    state[31] = 0
    state[19] = 0
    state[15] = (state[15] || 0) + 1
  }
  function persist(next) {
    try {
      if (!storage) throw Error('unavailable')
      storage.setItem(key, JSON.stringify(next))
      presets = next
      return true
    } catch {
      error = 'Cannot save in this browser'
      return false
    }
  }
  function save(index) {
    if (!presets[index]) return false
    const next = copy(presets)
    next[index] = { name: next[index].name, ...live() }
    if (!persist(next)) return false
    state[17] = index
    status = 'Saved'
    return true
  }
  function reset() {
    for (const k of Object.keys(state)) delete state[k]
    Object.assign(state, { 8: captures.length, 10: 1, 13: 0, 14: 16, 54: 0, 55: 0 })
    error = ''
    status = ''
    load(0)
  }
  function get(k) {
    if (k === 60) return tunerReading().voiced ? 1 : 0
    if (k === 61) return tunerReading().cents
    if (k === 9) return paramNames[state[5] || 0].length
    if (k === 18) return edited() ? 1 : 0
    if (k === 26) return presets.length
    if (k === 27) return browseWindow().length
    if (k === 28) return browseRows().length
    if (k === 29) return browsePage()
    if (k === 30) return browsePageCount()
    if (k === 31) return browseDepth()
    if (k >= 32 && k < 32 + browsePageSize) {
      const row = browseWindow()[k - 32]
      return row && !row.folder && row.model === state[12] ? 1 : 0
    }
    if (k >= 44 && k < 44 + browsePageSize) return browseWindow()[k - 44]?.folder ? 1 : 0
    if (k >= 100 && k < 196) {
      const s = Math.floor((k - 100) / 16),
        p = (k - 100) % 16,
        r = ranges[s][p]
      return r ? (params[s][p] - r[0]) / (r[1] - r[0]) : 0
    }
    return state[k] || 0
  }
  function label(k, i) {
    if (k === 0) return captures[state[12]]
    if (k === 1) return names[i]
    if (k === 2) return captures[i]
    if (k === 4) return 'Listening / audio muted'
    if (k === 5) return error
    if (k === 8) return i ? 'SD card' : 'Factory capture'
    if (k === 9) return presets[state[17]].name
    if (k === 10) return presets[i]?.name || ''
    if (k === 11) return draft
    if (k === 15) return 'Preview Wi-Fi'
    if (k === 16) return '192.168.1.42 / demo'
    if (k === 17) {
      const row = browseWindow()[i]
      if (!row) return ''
      return row.folder ? childOf(catalogue[row.model].folder) : captures[row.model]
    }
    if (k === 18) {
      const row = browseWindow()[i]
      if (!row) return ''
      if (row.folder) return `${row.under} capture${row.under === 1 ? '' : 's'}`
      return ''
    }
    if (k === 19) return browseTitle()
    if (k === 20) {
      const n = catalogue.filter((c) => c.folder.startsWith(browse.prefix)).length
      return `${n} capture${n === 1 ? '' : 's'}`
    }
    if (k === 12) return status
    if (k === 13) return 'E2'
    if (k === 14) return (82.406889 * Math.pow(2, tunerReading().cents / 1200)).toFixed(1) + ' Hz'
    const s = Math.floor(i / 16),
      p = i % 16
    if (k === 6) return paramNames[s]?.[p] || ''
    if (k === 7) {
      const v = params[s]?.[p]
      if (v === undefined) return ''
      if ((s < 2 && p === 0) || (s === 1 && p === 4)) return v.toFixed(1) + ' dB'
      if (s === 1 && p === 1) return v.toFixed(1) + ':1'
      if ((s === 5 && p === 0) || (s < 2 && p > 0)) return Math.round(v) + ' ms'
      if (s === 4 && p === 0) return v.toFixed(1) + ' Hz'
      return Math.round(v) + '%'
    }
    return ''
  }
  function action(a, i, v) {
    error = ''
    status = ''
    if (a === 0) {
      state[12] = i
      state[4] = 0
      state[31] = 0
      state[19] = 0
    }
    if (a === 7) state[16] = state[16] ? 0 : 1
    if (a === 1) state[20 + i] = state[20 + i] ? 0 : 1
    if (a === 3) state[10] = state[10] ? 0 : 1
    if (a === 4) {
      state[11] = state[11] ? 0 : 1
      state[63] = state[13]
      state[31] = 0
      state[19] = 0
    }
    if (a === 6) {
      const p = state[7] || 0,
        r = ranges[i][p]
      params[i][p] = r[0] + v * (r[1] - r[0])
    }
    if (a === 8) load(i)
    if (a === 9) save(state[17])
    if (a === 11 && save(state[17])) load(i) // Save current edits before switching.
    if (a === 13) {
      if (state[4] === 9) {
        load(state[17])
      } else {
        state[4] = 9
        state[31] = 0
        state[19] = 0
      }
    }
    if (a === 14) {
      const row = browseWindow()[i]
      if (row?.folder) browseDescend(i)
      else if (row) {
        state[12] = row.model
        state[4] = 0
      }
    }
    if (a === 15) {
      if (!browse.prefix) state[4] = 0
      else browseAscend()
    }
    if (a === 16) browse.page = Math.max(0, Math.min(browsePageCount() - 1, browsePage() + v))
    if (a === 12 && presets[i]) {
      if (presets.length === 1) {
        error = 'Keep at least one preset'
        return
      }
      const active = state[17],
        next = copy(presets)
      next.splice(i, 1)
      if (persist(next)) {
        if (i === active) load(Math.min(i, next.length - 1))
        else if (i < active) state[17] = active - 1
        state[59] = Math.min(state[59] || 0, Math.max(0, next.length * 33 - 99))
        state[4] = 3
        status = 'Deleted'
      }
    }
    state[15] = (state[15] || 0) + 1
  }
  function tunerReading() {
    const mode = state[62] || 0
    if (mode) return { voiced: mode !== 4, cents: mode === 1 ? -18 : mode === 3 ? 18 : 0 }
    const elapsed = ((state[13] || 0) - (state[63] || 0)) % 12000
    if (elapsed < 1200 || elapsed > 10500) return { voiced: false, cents: 0 }
    if (elapsed < 6500) return { voiced: true, cents: -24 * (1 - (elapsed - 1200) / 5300) }
    if (elapsed < 8500) return { voiced: true, cents: Math.sin(elapsed / 350) }
    return { voiced: true, cents: 16 }
  }
  function nameEdit(a, text) {
    error = ''
    status = ''
    if (a === 0) {
      draft = text.slice(0, 23)
      state[55] = 1
    }
    if (a === 1 || a === 2) {
      const previous = draft
      draft =
        a === 1 ? (draft + text.replace(/[^\x20-\x7e]/g, '')).slice(0, 23) : draft.slice(0, -1)
      if (draft !== previous) {
        if (!draft || draft.endsWith(' ')) state[55] = 1
        else if (state[55] !== 2) state[55] = 0
      }
    }
    if (a === 3) {
      const name = draft.trim()
      if (!name) {
        error = 'Enter a preset name'
        return
      }
      const next = copy(presets)
      if (state[56]) next.push({ name, ...live() })
      else {
        if (!next[state[54]]) return
        next[state[54]].name = name
      }
      if (persist(next)) {
        if (state[56]) load(next.length - 1)
        else {
          state[4] = 3
          status = 'Renamed'
        }
        state[56] = 0
        state[15] = (state[15] || 0) + 1
      }
    }
  }
  reset()
  return { state, get, set: (k, v) => (state[k] = v), label, action, nameEdit, reset }
}
