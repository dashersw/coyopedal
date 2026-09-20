import './App.css'
import { Component } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Chain } from './Chain'
import { Presets } from './Presets'
import { Amplifiers } from './Amplifiers'
import { Editor } from './Editor'
import { PresetName } from './PresetName'
import { Confirmation } from './Confirmation'
import { Setup, Maintenance, MaintenanceConfirmation } from './Setup'
import { Tuner } from './Tuner'

export class App extends Component {
  template() {
    return (
      <div class="pedalboard">
        {store.tuner ? (
          <Tuner />
        ) : store.screen === 0 ? (
          <Chain />
        ) : store.screen === 1 ? (
          <Amplifiers />
        ) : store.screen === 2 ? (
          <Editor />
        ) : store.screen === 3 ? (
          <Presets />
        ) : store.screen === 6 ? (
          <PresetName />
        ) : store.screen === 7 ? (
          <Confirmation />
        ) : store.screen === 8 ? (
          <Setup />
        ) : store.screen === 9 ? (
          <Maintenance />
        ) : (
          <MaintenanceConfirmation />
        )}
        {store.loading && (
          <div class="loading-model">
            <span>Preparing amp…</span>
          </div>
        )}
        {store.error && store.screen !== 6 && <div class="error-message">{store.error}</div>}
      </div>
    )
  }
}
