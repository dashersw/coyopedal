import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { pages, ROWS_PER_PAGE } from '../stores/PageStore'
import { Header } from './Header'
import { Pager, PRESET_PAGES } from './Pager'
import './Presets.css'

class PresetRow extends Component<GeaElement, { index: number }> {
  template({ index }: { index: number }) {
    return (
      <button class="preset-row" onClick={() => store.choosePreset(index)}>
        <span class="preset-row-name">{store.presets[index]}</span>
        {index === store.activePreset && <span class="status-dot dot-on" />}
      </button>
    )
  }
}

// Three fixed rows and ‹ › instead of a scrolling list.
export class Presets extends Component {
  template() {
    const first = pages.presets * ROWS_PER_PAGE
    const count = store.presets.length
    return (
      <div>
        <Header
          title="Presets"
          subtitle={store.status || (store.edited ? 'Current preset edited' : '')}
        />
        <button class="pill header-action" onClick={() => store.command(9)}>
          Save
        </button>
        <div class="preset-list">
          {first < count && <PresetRow index={first} />}
          {first + 1 < count && <PresetRow index={first + 1} />}
          {first + 2 < count && <PresetRow index={first + 2} />}
        </div>
        <button class="new-preset" onClick={() => store.create()}>
          + New
        </button>
        <div class="preset-pager">
          <Pager list={PRESET_PAGES} />
        </div>
        <button class="setup-button" onClick={() => store.show(8)}>
          Setup
        </button>
      </div>
    )
  }
}
