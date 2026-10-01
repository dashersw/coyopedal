import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './Cabinets.css'

class CabinetRow extends Component<GeaElement, { row: number }> {
  template({ row }: { row: number }) {
    return (
      <button class="model-row" onClick={() => store.chooseCabinet(row)}>
        <span>{store.cabinetNames[row]}</span>
        {store.cabinetActive[row] && <span class="status-dot dot-on" />}
      </button>
    )
  }
}

export class Cabinets extends Component {
  template() {
    return (
      <div>
        <Header title="Cabinet IR" subtitle="1024 taps / 48 kHz" />
        <div class="model-list">
          {store.cabinetRows > 0 && <CabinetRow row={0} />}
          {store.cabinetRows > 1 && <CabinetRow row={1} />}
          {store.cabinetRows > 2 && <CabinetRow row={2} />}
          {store.cabinetRows > 3 && <CabinetRow row={3} />}
        </div>
        {store.cabinetPages > 1 ? (
          <div class="pager">
            <button class="pager-step" onClick={() => store.command(19, 0, -1)}>
              Prev
            </button>
            <small class="pager-count">
              {store.cabinetPage + 1} / {store.cabinetPages}
            </small>
            <button class="pager-step" onClick={() => store.command(19, 0, 1)}>
              Next
            </button>
          </div>
        ) : (
          <div class="cabinet-hint">Copy cabinet WAVs into /ir on the SD card</div>
        )}
      </div>
    )
  }
}
