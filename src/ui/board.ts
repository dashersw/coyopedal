import { Audio } from '@geastack/core'

// The native compiler resolves these calls through scripts/panel-host-plugin.mjs.
// The browser preview installs the same contract before mounting the app.
declare function pbGet(key: number): number
declare function pbSet(key: number, value: number): void
declare function pbLabel(kind: number, index: number): string
declare function pbAction(action: number, index: number, value: number): void
declare function pbPresetName(action: number, text: string): void

export const board = {
  speaker(enabled: boolean): void {
    // The public audio binding also tells Gea to include the board codec.
    Audio.setVolume(enabled ? 100 : 0)
    pbSet(80, enabled ? 1 : 0)
  },
  get(key: number): number {
    return pbGet(key)
  },
  set(key: number, value: number): void {
    pbSet(key, value)
  },
  label(kind: number, index: number): string {
    return pbLabel(kind, index)
  },
  action(action: number, index: number, value: number): void {
    pbAction(action, index, value)
  },
  // Where a pointer event landed, from its clientX and clientY, read back
  // through keys 1 and 2 in logical CSS pixels; key 3 is the viewport width. Each host does the
  // mapping: the board divides out its pixel ratio, the preview its page scale.
  pointer(x: number, y: number): void {
    pbSet(1, x)
    pbSet(2, y)
  },
  nameEdit(action: number, text: string): void {
    pbPresetName(action, text)
  },
}
