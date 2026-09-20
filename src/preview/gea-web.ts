import { Component, Store } from '@geajs/core'

export { Component, Store }
export function mount(App: new () => Component) {
  const root = document.getElementById('app')
  if (!root) throw new Error('Missing #app mount point')
  new App().render(root)
}
