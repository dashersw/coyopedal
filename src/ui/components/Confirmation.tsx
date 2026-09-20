import { Component } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './Confirmation.css'

export class Confirmation extends Component {
  template() {
    return (
      <div>
        <Header
          title={store.deleting ? 'Delete preset?' : 'Unsaved edits'}
          subtitle={
            store.deleting ? 'This cannot be undone' : 'Keep your changes before switching?'
          }
        />
        <div class="confirmation-name">{store.pendingName}</div>
        <button class="wide first" onClick={() => store.confirm(true)}>
          {store.deleting ? 'Delete preset' : 'Save and switch'}
        </button>
        {!store.deleting && (
          <button class="wide second" onClick={() => store.confirm(false)}>
            Discard and switch
          </button>
        )}
        <button class="wide third" onClick={() => store.show(3)}>
          Cancel
        </button>
      </div>
    )
  }
}
