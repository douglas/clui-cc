/**
 * Standalone Vite config for building the CLUI renderer on Linux
 * without electron-vite. Outputs to dist/linux/renderer/.
 *
 * Usage:
 *   npx vite build --config linux/vite.config.ts
 *   npx vite dev --config linux/vite.config.ts
 */

import { resolve } from 'path'
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  root: resolve(__dirname, '../src/renderer'),
  plugins: [react(), tailwindcss()],
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
