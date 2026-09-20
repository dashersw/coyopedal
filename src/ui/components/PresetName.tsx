import { Component } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './PresetName.css'

export class PresetName extends Component {
  template() {
    return (
      <div>
        <Header
          title={store.creating ? 'New preset' : 'Rename'}
          subtitle={store.creating ? '' : 'Up to 23 characters'}
        />
        <button class="pill header-action" onClick={() => store.editName(3)}>
          {store.creating ? 'Add' : 'Save'}
        </button>
        <div class="name-field">{store.error || store.draft || 'Preset name'}</div>
        <div class="keyboard">
          <div class="key-row">
            {(store.keyboardMode === 2
              ? '1234567890'
              : store.keyboardMode === 1
                ? 'QWERTYUIOP'
                : 'qwertyuiop'
            )
              .split('')
              .map((key) => (
                <button key={key} onClick={() => store.editName(1, key)}>
                  {key}
                </button>
              ))}
          </div>
          <div class="key-row">
            {(store.keyboardMode === 2
              ? '-/:;()$&@'
              : store.keyboardMode === 1
                ? 'ASDFGHJKL'
                : 'asdfghjkl'
            )
              .split('')
              .map((key) => (
                <button key={key} onClick={() => store.editName(1, key)}>
                  {key}
                </button>
              ))}
          </div>
          <div class="key-row last-keys">
            {(store.keyboardMode === 2
              ? '.,?!\'"_'
              : store.keyboardMode === 1
                ? 'ZXCVBNM'
                : 'zxcvbnm'
            )
              .split('')
              .map((key) => (
                <button key={key} onClick={() => store.editName(1, key)}>
                  {key}
                </button>
              ))}
            <button
              class="numbers"
              onClick={() => store.changeKeyboard(store.keyboardMode === 2 ? 0 : 2)}
            >
              {store.keyboardMode === 2 ? 'abc' : '123'}
            </button>
          </div>
          <div class="keyboard-footer">
            <button onClick={() => store.changeKeyboard(store.keyboardMode === 1 ? 0 : 1)}>
              {store.keyboardMode === 1 ? 'abc' : 'ABC'}
            </button>
            <button class="space" onClick={() => store.editName(1, ' ')}>
              Space
            </button>
            <button onClick={() => store.editName(2)}>Delete</button>
          </div>
        </div>
      </div>
    )
  }
}
