import { Component } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './Tuner.css'

export class Tuner extends Component {
  template() {
    return (
      <div>
        <Header title="Tuner" subtitle="" />
        <button class="pill header-action" onClick={() => store.back()}>
          Done
        </button>
        <div class="tuner-reference">Muted / A4 440 Hz</div>
        <div class="tuner-note">{store.note}</div>
        <div class="tuner-frequency">{store.frequency}</div>
        <div class="tuner-meter">
          <div class={`tuner-center ${store.inTune ? 'in-tune' : ''}`} />
          <div class="ticks">
            <span class="major" />
            <span />
            <span />
            <span />
            <span />
            <span class="major" />
            <span />
            <span />
            <span />
            <span />
            <span class="major" />
          </div>
          {store.voiced && (
            <div
              class="tuner-needle"
              style={{ left: `${store.needlePercent}%`, backgroundColor: store.tunerColor }}
            />
          )}
        </div>
        <div class="tuner-scale">
          <span>-50</span>
          <span>0</span>
          <span>+50</span>
        </div>
        <div class="tuner-status" style={{ color: store.tunerColor }}>
          {store.tunerStatus}
        </div>
      </div>
    )
  }
}
