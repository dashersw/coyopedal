import { inertPluginInstance } from '@geastack/compiler/plugin'

const names = ['pbGet', 'pbSet', 'pbLabel', 'pbAction', 'pbPresetName']

export function geatscPlugin() {
  return {
    name: 'pedalboard-board',
    instantiate: () => ({
      ...inertPluginInstance,
      capabilities: {
        ...inertPluginInstance.capabilities,
        hostFunctions: new Map(names.map((name) => [name, name])),
        hostPreambles: new Map(names.map((name) => [name, ['#include "board_bridge.hpp"']])),
      },
    }),
  }
}

export default geatscPlugin
