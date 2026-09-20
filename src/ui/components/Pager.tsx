import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { pages, pageCount } from '../stores/PageStore'
import './Pager.css'

export const EDITOR_PAGES = 0
export const PRESET_PAGES = 1

function step(list: number, delta: number) {
  if (list === EDITOR_PAGES) pages.stepEditor(delta, store.parameterNames.length)
  else pages.stepPresets(delta, store.presets.length)
}

// ‹ › page buttons. Disabled ends stay drawn so the layout never shifts.
//
// Every store read below sits INSIDE the expression that uses it, never in a
// local the whole template shares. A local is evaluated once: the native build
// censuses the store fields each reactive position reads and subscribes the
// node to those signals, and a value already lifted out of the store carries no
// such provenance, so `const page = pages.editor` produced a pager that drew the
// right number once and then never moved -- ‹ and › ran, `pages.editor` changed,
// and nothing on screen was subscribed to it. Chain.tsx says the same thing
// about its dots, and Amplifiers.tsx's pager was only ever right because it
// happened to be written this way.
export class Pager extends Component<GeaElement, { list: number }> {
  template({ list }: { list: number }) {
    return (
      <div class="page-arrows">
        <button
          class="page-arrow previous-page"
          aria-label="Previous page"
          aria-disabled={(list === EDITOR_PAGES ? pages.editor : pages.presets) === 0}
          onClick={() => step(list, -1)}
        >
          <span class="page-chevron">{'‹'}</span>
        </button>
        <small class="page-count">
          {(list === EDITOR_PAGES ? pages.editor : pages.presets) + 1} /{' '}
          {pageCount(list === EDITOR_PAGES ? store.parameterNames.length : store.presets.length)}
        </small>
        <button
          class="page-arrow next-page"
          aria-label="Next page"
          aria-disabled={
            (list === EDITOR_PAGES ? pages.editor : pages.presets) + 1 >=
            pageCount(list === EDITOR_PAGES ? store.parameterNames.length : store.presets.length)
          }
          onClick={() => step(list, 1)}
        >
          <span class="page-chevron">{'›'}</span>
        </button>
      </div>
    )
  }
}
