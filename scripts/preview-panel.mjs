import { build } from 'vite'
import { fileURLToPath } from 'node:url'

await build({ configFile: fileURLToPath(new URL('../src/ui/vite.config.ts', import.meta.url)) })
console.log('Gea app preview: build/ui-preview/index.html')
