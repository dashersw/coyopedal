import { Component, type GeaElement } from '@geastack/core'
import { pedalboard as store } from '../stores/PedalboardStore'
import { Header } from './Header'
import './Amplifiers.css'

class BrowseRow extends Component<GeaElement, { row: number }> {
  template({ row }: { row: number }) {
    return (
      <button class="model-row" onClick={() => store.browseEnter(row)}>
        <span>{store.browseTitles[row]}</span>
        {store.browseSubtitles[row] && <small>{store.browseSubtitles[row]}</small>}
        {store.browseActive[row] && store.ampEnabled && <span class="status-dot dot-on" />}
        {store.browseActive[row] && !store.ampEnabled && <span class="status-dot dot-off" />}
        {store.browseFolder[row] && <span class="row-chevron">{'›'}</span>}
      </button>
    )
  }
}

// The capture library as folders, a page at a time: the factory captures, the
// imported ones and the SD card's own directory tree. Native owns the walk (host
// keys 27-35 and 44-47, labels 17-20, actions 14-16), so this renders whatever
// page it is handed and never sees the catalogue's size or the tree's depth.
//
// Four rows, each gated on the row existing, rather than one map over the row
// list -- the shape Editor and Presets already use. Four because that is
// kBrowseRowsPerPage in src/native/ui/board_bridge.cpp, which decides how many
// rows a page has, and the store keeps its row arrays that long whatever the
// page holds. Both halves of that matter: a row on its way out is asked for its
// own title one last time, and reading a store array past its end is fatal
// rather than undefined -- "gea: read of array hole or out-of-range index 1
// (length 1); absence has no carrier here", every time a folder with one
// capture was opened from a folder with three. See PedalboardStore.
export class Amplifiers extends Component {
  template() {
    return (
      <div>
        <Header title={store.browseTitle} subtitle={store.browseSubtitle} />
        <div class="model-list">
          {store.browseRows > 0 && <BrowseRow row={0} />}
          {store.browseRows > 1 && <BrowseRow row={1} />}
          {store.browseRows > 2 && <BrowseRow row={2} />}
          {store.browseRows > 3 && <BrowseRow row={3} />}
        </div>
        {store.browsePageCount > 1 && (
          <div class="pager">
            <button
              class="pager-step"
              aria-disabled={store.browsePageIndex === 0}
              onClick={() => store.browsePage(-1)}
            >
              Prev
            </button>
            <small class="pager-count">
              {store.browsePageIndex + 1} / {store.browsePageCount}
            </small>
            <button
              class="pager-step"
              aria-disabled={store.browsePageIndex + 1 >= store.browsePageCount}
              onClick={() => store.browsePage(1)}
            >
              Next
            </button>
          </div>
        )}
        {store.browseTotal === 0 && <div class="screen-caption empty-library">No captures</div>}
      </div>
    )
  }
}
