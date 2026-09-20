import { Component } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './Setup.css'

export class Setup extends Component {
  template() {
    return (
      <div>
        <Header title="Setup" subtitle="" />
        <div class="screen-caption">Wi-Fi and BLE are off</div>
        <button class="wide first" onClick={() => store.enterMaintenance(false)}>
          Maintenance mode
        </button>
        <div class="maintenance-description">Stops audio and enables updates</div>
      </div>
    )
  }
}

export class Maintenance extends Component {
  template() {
    return (
      <div>
        <div class="header centered-title">
          <span>Maintenance</span>
        </div>
        <div class="screen-caption">Audio stopped / Wi-Fi + BLE</div>
        <div class="network-name">{store.network}</div>
        <div class="network-address">{store.address}</div>
        <button class="wide third" onClick={() => store.command(13)}>
          Return to pedalboard
        </button>
      </div>
    )
  }
}

export class MaintenanceConfirmation extends Component {
  template() {
    return (
      <div>
        <Header title="Unsaved edits" subtitle="" />
        <div class="screen-caption">Before entering maintenance</div>
        <button class="wide first" onClick={() => store.enterMaintenance(true)}>
          Save and enter
        </button>
        <button class="wide second" onClick={() => store.enterMaintenance(false)}>
          Discard and enter
        </button>
        <button class="wide third" onClick={() => store.show(8)}>
          Cancel
        </button>
      </div>
    )
  }
}
