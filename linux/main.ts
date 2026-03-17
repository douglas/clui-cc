#!/usr/bin/env node
/**
 * main.ts — Linux entry point for CLUI-CC
 *
 * Starts the Node.js IPC server and spawns the GTK4 layer-shell wrapper.
 * The GTK shell connects back to the IPC server over a Unix socket.
 */

import { spawn } from 'child_process'
import { existsSync, unlinkSync } from 'fs'
import { join } from 'path'
import { startIPCServer } from './ipc-server'
import { log, flushLogs } from '../src/main/logger'

const SOCKET_PATH = process.env.CLUI_SOCKET || '/tmp/clui-shell.sock'
const CONTENT_URL = process.env.CLUI_CONTENT_URL || 'http://localhost:5173'

// Resolve GTK shell binary — check common locations
function findShellBinary(): string {
  const candidates = [
    join(__dirname, 'shell', 'builddir', 'clui-shell'),
    join(__dirname, '..', 'linux', 'shell', 'builddir', 'clui-shell'),
    '/usr/local/bin/clui-shell',
    '/usr/bin/clui-shell',
  ]

  // Also check CLUI_SHELL env var
  const envPath = process.env.CLUI_SHELL
  if (envPath && existsSync(envPath)) return envPath

  for (const c of candidates) {
    if (existsSync(c)) return c
  }

  console.error('clui-shell binary not found. Build it with:')
  console.error('  cd linux/shell && meson setup builddir && meson compile -C builddir')
  process.exit(1)
}

function main(): void {
  log('main', `Starting CLUI-CC Linux (socket=${SOCKET_PATH})`)

  // Start IPC server first — GTK shell will connect to it
  startIPCServer(SOCKET_PATH)

  // Spawn GTK4 shell
  const shellBin = findShellBinary()
  log('main', `Spawning GTK shell: ${shellBin}`)

  const shellProc = spawn(shellBin, [], {
    env: {
      ...process.env,
      CLUI_SOCKET: SOCKET_PATH,
      CLUI_CONTENT_URL: CONTENT_URL,
      WEBKIT_DISABLE_COMPOSITING_MODE: '1',
    },
    stdio: ['ignore', 'pipe', 'pipe'],
  })

  shellProc.stdout?.on('data', (data: Buffer) => {
    const lines = data.toString().trim().split('\n')
    for (const line of lines) {
      log('shell', line)
    }
  })

  shellProc.stderr?.on('data', (data: Buffer) => {
    const lines = data.toString().trim().split('\n')
    for (const line of lines) {
      log('shell:err', line)
    }
  })

  shellProc.on('exit', (code, signal) => {
    log('main', `GTK shell exited (code=${code}, signal=${signal})`)
    flushLogs()
    try { unlinkSync(SOCKET_PATH) } catch {}
    process.exit(code ?? 1)
  })

  // Handle our own signals
  const cleanup = () => {
    log('main', 'Shutting down...')
    shellProc.kill('SIGTERM')
    // IPC server cleanup happens via its own signal handler
    setTimeout(() => process.exit(0), 1000)
  }

  process.on('SIGTERM', cleanup)
  process.on('SIGINT', cleanup)
}

main()
