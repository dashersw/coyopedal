import { Store } from '@geastack/core'

// No screen scrolls: lists show a fixed number of rows and page with ‹ ›.
export const ROWS_PER_PAGE = 3

export function pageCount(items: number) {
  return Math.max(1, Math.ceil(items / ROWS_PER_PAGE))
}

export class PageStore extends Store {
  editor = 0
  presets = 0

  stepEditor(delta: number, items: number) {
    this.editor = Math.max(0, Math.min(pageCount(items) - 1, this.editor + delta))
  }
  stepPresets(delta: number, items: number) {
    this.presets = Math.max(0, Math.min(pageCount(items) - 1, this.presets + delta))
  }
}

export const pages = new PageStore()
