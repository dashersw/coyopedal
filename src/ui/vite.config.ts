import { defineConfig } from 'vite'
import { geaPlugin } from '@geajs/vite-plugin'
import { fileURLToPath } from 'node:url'

export default defineConfig({
  root: fileURLToPath(new URL('../preview/', import.meta.url)),
  base: './',
  publicDir: false,
  plugins: [geaPlugin()],
  // The plugin's compiled components import @geajs/core's compiler runtime
  // straight from node_modules. Pre-bundling the package gives the stores a
  // second copy of the reactive core, and the screen then never re-renders.
  optimizeDeps: { exclude: ['@geajs/core'] },
  resolve: {
    alias: {
      '@geastack/core': fileURLToPath(new URL('../preview/gea-web.ts', import.meta.url)),
    },
  },
  build: {
    outDir: '../../build/ui-preview',
    emptyOutDir: false,
  },
})
