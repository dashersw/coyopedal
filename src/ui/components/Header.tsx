import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import './Header.css'

export class Header extends Component<GeaElement, { title: string; subtitle: string }> {
  template({ title, subtitle }: { title: string; subtitle: string }) {
    return (
      <div class="header">
        <button class={`back ${subtitle ? 'with-subtitle' : ''}`} onClick={() => store.back()}>
          <span class="chevron">{'‹'}</span>
          <span>{title}</span>
        </button>
        {subtitle && <small class="header-subtitle">{subtitle}</small>}
      </div>
    )
  }
}
