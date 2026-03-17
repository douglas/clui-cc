/**
 * Standalone Vite config for building the CLUI renderer on Linux
 * without electron-vite. Outputs to dist/linux/renderer/.
 *
 * Usage:
 *   npx vite build --config linux/vite.config.ts
 *   npx vite dev --config linux/vite.config.ts
 */

import { readFileSync } from 'fs'
import { resolve } from 'path'
import { defineConfig, type Plugin } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

/**
 * Injects bridge.js as an inline <script> in <head> so it runs
 * synchronously before React. WebKitGTK's UserContentManager
 * user-script injection is unreliable, so we serve it as part
 * of the HTML instead.
 */
function bridgeInjectPlugin(): Plugin {
  const bridgePath = resolve(__dirname, 'bridge.js')
  let bridgeSource: string

  return {
    name: 'clui-bridge-inject',
    configResolved() {
      try {
        bridgeSource = readFileSync(bridgePath, 'utf-8')
      } catch {
        console.warn(`[clui-bridge-inject] bridge.js not found at ${bridgePath}`)
        bridgeSource = ''
      }
    },
    transformIndexHtml(html) {
      if (!bridgeSource) return html
      return html.replace(
        '<head>',
        `<head>\n    <script>${bridgeSource}</script>`,
      )
    },
  }
}

export default defineConfig({
  root: resolve(__dirname, '../src/renderer'),
  plugins: [bridgeInjectPlugin(), react(), tailwindcss()],
  build: {
    outDir: resolve(__dirname, '../dist/linux/renderer'),
    emptyOutDir: true,
    rollupOptions: {
      input: {
        index: resolve(__dirname, '../src/renderer/index.html'),
      },
    },
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
})
