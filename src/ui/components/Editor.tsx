import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { pages, pageCount, ROWS_PER_PAGE } from '../stores/PageStore'
import { Header } from './Header'
import { Slider } from './Slider'
import { Pager, EDITOR_PAGES } from './Pager'
import './Editor.css'

class Parameter extends Component<GeaElement, { index: number }> {
  template({ index }: { index: number }) {
    return (
      <div class="parameter">
        <div class="parameter-labels">
          <span>{store.parameterNames[index]}</span>
          <span style={{ color: store.accent }}>{store.parameterLabels[index]}</span>
        </div>
        <Slider index={index} />
      </div>
    )
  }
}

// Three fixed rows and ‹ › instead of a scrolling list.
//
// `pages.editor` and `parameterNames.length` are read inside each row's own
// condition rather than hoisted into `first` and `count`. A local is evaluated
// once and the rows it gates subscribe to nothing, which is why paging used to
// do nothing at all: ‹ › wrote `pages.editor` and no node on screen was
// listening. See the note on Pager.
export class Editor extends Component {
  template() {
    return (
      <div>
        <Header title={store.title} subtitle="" />
        <button class="pill header-action" onClick={() => store.command(1, store.selected)}>
          {store.enabled[store.selected] && <span class={`status-dot lit-${store.selected}`} />}
          {!store.enabled[store.selected] && <span class="status-dot dot-off" />}
          <span>{store.enabled[store.selected] ? 'On' : 'Off'}</span>
        </button>
        <div class="parameter-list">
          {pages.editor * ROWS_PER_PAGE < store.parameterNames.length && (
            <Parameter index={pages.editor * ROWS_PER_PAGE} />
          )}
          {pages.editor * ROWS_PER_PAGE + 1 < store.parameterNames.length && (
            <Parameter index={pages.editor * ROWS_PER_PAGE + 1} />
          )}
          {pages.editor * ROWS_PER_PAGE + 2 < store.parameterNames.length && (
            <Parameter index={pages.editor * ROWS_PER_PAGE + 2} />
          )}
        </div>
        {pageCount(store.parameterNames.length) > 1 && (
          <div class="editor-pager">
            <Pager list={EDITOR_PAGES} />
          </div>
        )}
      </div>
    )
  }
}
