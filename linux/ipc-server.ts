/**
 * ipc-server.ts — Unix socket IPC server replacing Electron's ipcMain
 *
 * Sits between the GTK4 shell (which relays WebKitGTK messages) and the
 * existing Node.js backend (ControlPlane, RunManager, etc.).
 *
 * Protocol: newline-delimited JSON over Unix socket.
 *
 * Invoke (request/response):
 *   → { "type": "invoke", "id": 1, "channel": "clui:start", "args": [...] }
 *   ← { "type": "resolve", "id": 1, "result": {...} }
 *   ← { "type": "reject",  "id": 1, "error": "message" }
 *
 * Send (fire-and-forget):
 *   → { "type": "send", "channel": "clui:init-session", "args": ["tab-1"] }
 *
 * Backend → renderer (broadcast):
 *   ← { "type": "broadcast", "channel": "clui:normalized-event", "args": [...] }
 */

import { createServer, type Socket } from 'net'
import { existsSync, readFileSync, readdirSync, statSync, createReadStream, unlinkSync, writeFileSync } from 'fs'
import { join, basename, extname } from 'path'
import { homedir, tmpdir } from 'os'
import { execFileSync, execFile as execFileCb, spawn, type ChildProcess } from 'child_process'
import { createInterface } from 'readline'
import { randomUUID } from 'crypto'

import { ControlPlane } from '../src/main/claude/control-plane'
import { ensureSkills, type SkillStatus } from '../src/main/skills/installer'
import { fetchCatalog, listInstalled, installPlugin, uninstallPlugin } from '../src/main/marketplace/catalog'
import { log as _log, LOG_FILE, flushLogs } from '../src/main/logger'
import { IPC } from '../src/shared/types'
import type { RunOptions, NormalizedEvent, EnrichedError } from '../src/shared/types'

const DEBUG_MODE = process.env.CLUI_DEBUG === '1'

function log(msg: string): void {
  _log('ipc-server', msg)
}

/* ─── Safe exec helpers ─── */

function execFileQuiet(cmd: string, args: string[], opts: { encoding: 'utf-8'; timeout?: number }): string {
  return execFileSync(cmd, args, { ...opts, stdio: ['pipe', 'pipe', 'pipe'] })
}

/* ─── State ─── */

const INTERACTIVE_PTY = process.env.CLUI_INTERACTIVE_PERMISSIONS_PTY === '1'
const controlPlane = new ControlPlane(INTERACTIVE_PTY)

let clientSocket: Socket | null = null
let screenshotCounter = 0
let pasteCounter = 0
let shellProcess: ChildProcess | null = null

/* ─── Broadcast to renderer (via GTK shell eval) ─── */

function broadcast(channel: string, ...args: unknown[]): void {
  if (!clientSocket || clientSocket.destroyed) return
  const msg = JSON.stringify({ type: 'broadcast', channel, args })
  clientSocket.write(msg + '\n')
}

/* ─── Wire ControlPlane events → renderer ─── */

controlPlane.on('event', (tabId: string, event: NormalizedEvent) => {
  broadcast('clui:normalized-event', tabId, event)
})

controlPlane.on('tab-status-change', (tabId: string, newStatus: string, oldStatus: string) => {
  broadcast('clui:tab-status-change', tabId, newStatus, oldStatus)
})

controlPlane.on('error', (tabId: string, error: EnrichedError) => {
  broadcast('clui:enriched-error', tabId, error)
})

/* ─── IPC Handlers ─── */

type Handler = (...args: any[]) => any | Promise<any>
const invokeHandlers = new Map<string, Handler>()
const sendHandlers = new Map<string, Handler>()

// Request-response handlers (equivalent to ipcMain.handle)
invokeHandlers.set(IPC.START, async () => {
  log('IPC START')
  let version = 'unknown'
  try { version = execFileQuiet('claude', ['-v'], { encoding: 'utf-8', timeout: 5000 }).trim() } catch {}

  let auth: { email?: string; subscriptionType?: string; authMethod?: string } = {}
  try { auth = JSON.parse(execFileQuiet('claude', ['auth', 'status'], { encoding: 'utf-8', timeout: 5000 }).trim()) } catch {}

  let mcpServers: string[] = []
  try {
    const raw = execFileQuiet('claude', ['mcp', 'list'], { encoding: 'utf-8', timeout: 5000 }).trim()
    if (raw) mcpServers = raw.split('\n').filter(Boolean)
  } catch {}

  return { version, auth, mcpServers, projectPath: process.cwd(), homePath: homedir() }
})

invokeHandlers.set(IPC.CREATE_TAB, () => {
  const tabId = controlPlane.createTab()
  log(`IPC CREATE_TAB → ${tabId}`)
  return { tabId }
})

invokeHandlers.set(IPC.PROMPT, async (_event: unknown, payload: { tabId: string; requestId: string; options: RunOptions }) => {
  const { tabId, requestId, options } = payload
  log(`IPC PROMPT: tab=${tabId} req=${requestId}`)
  if (!tabId) throw new Error('No tabId provided')
  if (!requestId) throw new Error('No requestId provided')
  await controlPlane.submitPrompt(tabId, requestId, options)
})

invokeHandlers.set(IPC.CANCEL, (_event: unknown, requestId: string) => {
  log(`IPC CANCEL: ${requestId}`)
  return controlPlane.cancel(requestId)
})

invokeHandlers.set(IPC.STOP_TAB, (_event: unknown, tabId: string) => {
  log(`IPC STOP_TAB: ${tabId}`)
  return controlPlane.cancelTab(tabId)
})

invokeHandlers.set(IPC.RETRY, async (_event: unknown, payload: { tabId: string; requestId: string; options: RunOptions }) => {
  log(`IPC RETRY: tab=${payload.tabId} req=${payload.requestId}`)
  return controlPlane.retry(payload.tabId, payload.requestId, payload.options)
})

invokeHandlers.set(IPC.STATUS, () => controlPlane.getHealth())
invokeHandlers.set(IPC.TAB_HEALTH, () => controlPlane.getHealth())

invokeHandlers.set(IPC.CLOSE_TAB, (_event: unknown, tabId: string) => {
  log(`IPC CLOSE_TAB: ${tabId}`)
  controlPlane.closeTab(tabId)
})

invokeHandlers.set(IPC.RESPOND_PERMISSION, (_event: unknown, payload: { tabId: string; questionId: string; optionId: string }) => {
  log(`IPC RESPOND_PERMISSION: tab=${payload.tabId}`)
  return controlPlane.respondToPermission(payload.tabId, payload.questionId, payload.optionId)
})

invokeHandlers.set(IPC.LIST_SESSIONS, async (_event: unknown, projectPath?: string) => {
  log('IPC LIST_SESSIONS')
  try {
    const cwd = projectPath || process.cwd()
    const encodedPath = cwd.replace(/\//g, '-')
    const sessionsDir = join(homedir(), '.claude', 'projects', encodedPath)
    if (!existsSync(sessionsDir)) return []
    const files = readdirSync(sessionsDir).filter((f: string) => f.endsWith('.jsonl'))

    const sessions: Array<{ sessionId: string; slug: string | null; firstMessage: string | null; lastTimestamp: string; size: number }> = []
    const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i

    for (const file of files) {
      const fileSessionId = file.replace(/\.jsonl$/, '')
      if (!UUID_RE.test(fileSessionId)) continue
      const filePath = join(sessionsDir, file)
      const stat = statSync(filePath)
      if (stat.size < 100) continue

      const meta: { validated: boolean; slug: string | null; firstMessage: string | null; lastTimestamp: string | null } = {
        validated: false, slug: null, firstMessage: null, lastTimestamp: null,
      }

      await new Promise<void>((resolve) => {
        const rl = createInterface({ input: createReadStream(filePath) })
        rl.on('line', (line: string) => {
          try {
            const obj = JSON.parse(line)
            if (!meta.validated && obj.type && obj.uuid && obj.timestamp) meta.validated = true
            if (obj.slug && !meta.slug) meta.slug = obj.slug
            if (obj.timestamp) meta.lastTimestamp = obj.timestamp
            if (obj.type === 'user' && !meta.firstMessage) {
              const content = obj.message?.content
              if (typeof content === 'string') meta.firstMessage = content.substring(0, 100)
              else if (Array.isArray(content)) {
                const textPart = content.find((p: any) => p.type === 'text')
                meta.firstMessage = textPart?.text?.substring(0, 100) || null
              }
            }
          } catch {}
        })
        rl.on('close', () => resolve())
      })

      if (meta.validated) {
        sessions.push({
          sessionId: fileSessionId, slug: meta.slug,
          firstMessage: meta.firstMessage,
          lastTimestamp: meta.lastTimestamp || stat.mtime.toISOString(),
          size: stat.size,
        })
      }
    }
    sessions.sort((a, b) => new Date(b.lastTimestamp).getTime() - new Date(a.lastTimestamp).getTime())
    return sessions.slice(0, 20)
  } catch (err) {
    log(`LIST_SESSIONS error: ${err}`)
    return []
  }
})

invokeHandlers.set(IPC.LOAD_SESSION, async (_event: unknown, arg: { sessionId: string; projectPath?: string } | string) => {
  const sessionId = typeof arg === 'string' ? arg : arg.sessionId
  const projectPath = typeof arg === 'string' ? undefined : arg.projectPath
  log(`IPC LOAD_SESSION ${sessionId}`)
  try {
    const cwd = projectPath || process.cwd()
    const encodedPath = cwd.replace(/\//g, '-')
    const filePath = join(homedir(), '.claude', 'projects', encodedPath, `${sessionId}.jsonl`)
    if (!existsSync(filePath)) return []

    const messages: Array<{ role: string; content: string; toolName?: string; timestamp: number }> = []
    await new Promise<void>((resolve) => {
      const rl = createInterface({ input: createReadStream(filePath) })
      rl.on('line', (line: string) => {
        try {
          const obj = JSON.parse(line)
          if (obj.type === 'user') {
            const content = obj.message?.content
            let text = ''
            if (typeof content === 'string') text = content
            else if (Array.isArray(content)) {
              text = content.filter((b: any) => b.type === 'text').map((b: any) => b.text).join('\n')
            }
            if (text) messages.push({ role: 'user', content: text, timestamp: new Date(obj.timestamp).getTime() })
          } else if (obj.type === 'assistant') {
            const content = obj.message?.content
            if (Array.isArray(content)) {
              for (const block of content) {
                if (block.type === 'text' && block.text) {
                  messages.push({ role: 'assistant', content: block.text, timestamp: new Date(obj.timestamp).getTime() })
                } else if (block.type === 'tool_use' && block.name) {
                  messages.push({ role: 'tool', content: '', toolName: block.name, timestamp: new Date(obj.timestamp).getTime() })
                }
              }
            }
          }
        } catch {}
      })
      rl.on('close', () => resolve())
    })
    return messages
  } catch (err) {
    log(`LOAD_SESSION error: ${err}`)
    return []
  }
})

invokeHandlers.set(IPC.SELECT_DIRECTORY, async () => {
  // Use zenity on Linux for directory selection
  try {
    const result = execFileQuiet('zenity',
      ['--file-selection', '--directory', '--title=Select Project Directory'],
      { encoding: 'utf-8', timeout: 60000 }
    ).trim()
    return result || null
  } catch {
    // Fall back to kdialog
    try {
      const result = execFileQuiet('kdialog',
        ['--getexistingdirectory', homedir()],
        { encoding: 'utf-8', timeout: 60000 }
      ).trim()
      return result || null
    } catch { return null }
  }
})

invokeHandlers.set(IPC.OPEN_EXTERNAL, async (_event: unknown, url: string) => {
  if (!/^https?:\/\//i.test(url)) return false
  try {
    execFileCb('xdg-open', [url], (err) => {
      if (err) log(`xdg-open failed: ${err.message}`)
    })
    return true
  } catch { return false }
})

invokeHandlers.set(IPC.ATTACH_FILES, async () => {
  try {
    const result = execFileQuiet('zenity',
      ['--file-selection', '--multiple', '--separator=\n', '--title=Attach Files'],
      { encoding: 'utf-8', timeout: 60000 }
    ).trim()
    if (!result) return null

    const IMAGE_EXTS = new Set(['.png', '.jpg', '.jpeg', '.gif', '.webp', '.svg'])
    const mimeMap: Record<string, string> = {
      '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
      '.gif': 'image/gif', '.webp': 'image/webp', '.svg': 'image/svg+xml',
      '.pdf': 'application/pdf', '.txt': 'text/plain', '.md': 'text/markdown',
      '.json': 'application/json', '.yaml': 'text/yaml', '.toml': 'text/toml',
    }

    return result.split('\n').filter(Boolean).map((fp: string) => {
      const ext = extname(fp).toLowerCase()
      const mime = mimeMap[ext] || 'application/octet-stream'
      const stat = statSync(fp)
      let dataUrl: string | undefined

      if (IMAGE_EXTS.has(ext) && stat.size < 2 * 1024 * 1024) {
        try {
          const buf = readFileSync(fp)
          dataUrl = `data:${mime};base64,${buf.toString('base64')}`
        } catch {}
      }

      return {
        id: randomUUID(),
        type: IMAGE_EXTS.has(ext) ? 'image' : 'file',
        name: basename(fp),
        path: fp,
        mimeType: mime,
        dataUrl,
        size: stat.size,
      }
    })
  } catch { return null }
})

invokeHandlers.set(IPC.TAKE_SCREENSHOT, async () => {
  // Hide overlay, take screenshot with grim+slurp, show overlay
  sendShellCommand({ cmd: 'hide' })
  await new Promise((r) => setTimeout(r, 300))

  try {
    const timestamp = Date.now()
    const screenshotPath = join(tmpdir(), `clui-screenshot-${timestamp}.png`)

    // Get selection with slurp first, then capture with grim
    const geometry = execFileQuiet('slurp', [], { encoding: 'utf-8', timeout: 30000 }).trim()
    if (!geometry) return null

    execFileSync('grim', ['-g', geometry, screenshotPath], { timeout: 10000 })

    if (!existsSync(screenshotPath)) return null

    const buf = readFileSync(screenshotPath)
    return {
      id: randomUUID(),
      type: 'image',
      name: `screenshot ${++screenshotCounter}.png`,
      path: screenshotPath,
      mimeType: 'image/png',
      dataUrl: `data:image/png;base64,${buf.toString('base64')}`,
      size: buf.length,
    }
  } catch { return null } finally {
    sendShellCommand({ cmd: 'show' })
  }
})

invokeHandlers.set(IPC.PASTE_IMAGE, async (_event: unknown, dataUrl: string) => {
  try {
    const match = dataUrl.match(/^data:(image\/(\w+));base64,(.+)$/)
    if (!match) return null
    const [, mimeType, ext, base64Data] = match
    const buf = Buffer.from(base64Data, 'base64')
    const filePath = join(tmpdir(), `clui-paste-${Date.now()}.${ext}`)
    writeFileSync(filePath, buf)
    return {
      id: randomUUID(),
      type: 'image',
      name: `pasted image ${++pasteCounter}.${ext}`,
      path: filePath,
      mimeType,
      dataUrl,
      size: buf.length,
    }
  } catch { return null }
})

invokeHandlers.set(IPC.TRANSCRIBE_AUDIO, async (_event: unknown, audioBase64: string) => {
  const tmpWav = join(tmpdir(), `clui-voice-${Date.now()}.wav`)
  try {
    const buf = Buffer.from(audioBase64, 'base64')
    writeFileSync(tmpWav, buf)

    // Find whisper binary
    const candidates = [
      '/usr/bin/whisper-cli',
      '/usr/local/bin/whisper-cli',
      '/usr/bin/whisper',
      '/usr/local/bin/whisper',
      join(homedir(), '.local/bin/whisper'),
    ]

    let whisperBin = ''
    for (const c of candidates) {
      if (existsSync(c)) { whisperBin = c; break }
    }

    if (!whisperBin) {
      try { whisperBin = execFileQuiet('which', ['whisper-cli'], { encoding: 'utf-8' }).trim() } catch {}
    }
    if (!whisperBin) {
      try { whisperBin = execFileQuiet('which', ['whisper'], { encoding: 'utf-8' }).trim() } catch {}
    }

    if (!whisperBin) {
      return { error: 'Whisper not found. Install whisper-cpp from your package manager.', transcript: null }
    }

    const isWhisperCpp = whisperBin.includes('whisper-cli')
    const modelCandidates = [
      join(homedir(), '.local/share/whisper/ggml-tiny.bin'),
      join(homedir(), '.local/share/whisper/ggml-base.bin'),
      '/usr/share/whisper-cpp/models/ggml-tiny.bin',
      '/usr/share/whisper-cpp/models/ggml-base.bin',
      join(homedir(), '.local/share/whisper/ggml-tiny.en.bin'),
      join(homedir(), '.local/share/whisper/ggml-base.en.bin'),
    ]

    let modelPath = ''
    for (const m of modelCandidates) {
      if (existsSync(m)) { modelPath = m; break }
    }

    const isEnglishOnly = modelPath.includes('.en.')
    log(`Transcribing with: ${whisperBin} (model: ${modelPath || 'default'})`)

    let output: string
    if (isWhisperCpp) {
      if (!modelPath) {
        return { error: 'Whisper model not found. Download: mkdir -p ~/.local/share/whisper && curl -L -o ~/.local/share/whisper/ggml-tiny.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin', transcript: null }
      }
      const langFlag = isEnglishOnly ? 'en' : 'auto'
      output = execFileSync(whisperBin,
        ['-m', modelPath, '-f', tmpWav, '--no-timestamps', '-l', langFlag],
        { encoding: 'utf-8', timeout: 30000 }
      )
    } else {
      const args = [tmpWav, '--model', 'tiny', '--output_format', 'txt', '--output_dir', tmpdir()]
      if (isEnglishOnly) args.push('--language', 'en')
      output = execFileSync(whisperBin, args, { encoding: 'utf-8', timeout: 30000 })
      const txtPath = tmpWav.replace('.wav', '.txt')
      if (existsSync(txtPath)) {
        const transcript = readFileSync(txtPath, 'utf-8').trim()
        try { unlinkSync(txtPath) } catch {}
        return { error: null, transcript }
      }
    }

    const transcript = output.replace(/\[[\d:.]+\s*-->\s*[\d:.]+\]\s*/g, '').trim()
    return { error: null, transcript: transcript || '' }
  } catch (err: any) {
    log(`Transcription error: ${err.message}`)
    return { error: `Transcription failed: ${err.message}`, transcript: null }
  } finally {
    try { unlinkSync(tmpWav) } catch {}
  }
})

invokeHandlers.set(IPC.GET_DIAGNOSTICS, () => {
  const health = controlPlane.getHealth()
  let recentLogs = ''
  if (existsSync(LOG_FILE)) {
    try {
      const content = readFileSync(LOG_FILE, 'utf-8')
      const lines = content.split('\n')
      recentLogs = lines.slice(-100).join('\n')
    } catch {}
  }

  return {
    health,
    logPath: LOG_FILE,
    recentLogs,
    platform: process.platform,
    arch: process.arch,
    nodeVersion: process.versions.node,
    transport: INTERACTIVE_PTY ? 'pty' : 'stream-json',
  }
})

invokeHandlers.set(IPC.OPEN_IN_TERMINAL, (_event: unknown, arg: string | null | { sessionId?: string | null; projectPath?: string }) => {
  let sessionId: string | null = null
  let projectPath: string = process.cwd()
  if (typeof arg === 'string') {
    sessionId = arg
  } else if (arg && typeof arg === 'object') {
    sessionId = arg.sessionId ?? null
    projectPath = arg.projectPath && arg.projectPath !== '~' ? arg.projectPath : process.cwd()
  }

  // Use $TERMINAL or fall back to common terminals
  const terminal = process.env.TERMINAL || 'alacritty'
  const claudeBin = 'claude'
  let shellArgs: string[]
  if (sessionId) {
    shellArgs = [`cd "${projectPath}" && ${claudeBin} --resume ${sessionId}`]
  } else {
    shellArgs = [`cd "${projectPath}" && ${claudeBin}`]
  }

  try {
    const child = spawn(terminal, ['-e', '/bin/sh', '-c', ...shellArgs], {
      detached: true,
      stdio: 'ignore',
    })
    child.unref()
    log(`Opened terminal: ${terminal}`)
    return true
  } catch (err: unknown) {
    log(`Failed to open terminal: ${err}`)
    return false
  }
})

invokeHandlers.set(IPC.IS_VISIBLE, () => true)

invokeHandlers.set(IPC.GET_THEME, () => {
  // Read from gsettings for dark/light theme detection
  try {
    const scheme = execFileQuiet('gsettings',
      ['get', 'org.gnome.desktop.interface', 'color-scheme'],
      { encoding: 'utf-8' }
    ).trim()
    return { isDark: scheme.includes('dark') }
  } catch {
    return { isDark: true }
  }
})

// Marketplace
invokeHandlers.set(IPC.MARKETPLACE_FETCH, async (_event: unknown, opts?: { forceRefresh?: boolean }) => {
  log('IPC MARKETPLACE_FETCH')
  return fetchCatalog(opts?.forceRefresh)
})

invokeHandlers.set(IPC.MARKETPLACE_INSTALLED, async () => listInstalled())

invokeHandlers.set(IPC.MARKETPLACE_INSTALL, async (_event: unknown, payload: { repo: string; pluginName: string; marketplace: string; sourcePath?: string; isSkillMd?: boolean }) => {
  log(`IPC MARKETPLACE_INSTALL: ${payload.pluginName}`)
  return installPlugin(payload.repo, payload.pluginName, payload.marketplace, payload.sourcePath, payload.isSkillMd)
})

invokeHandlers.set(IPC.MARKETPLACE_UNINSTALL, async (_event: unknown, payload: { pluginName: string }) => {
  log(`IPC MARKETPLACE_UNINSTALL: ${payload.pluginName}`)
  return uninstallPlugin(payload.pluginName)
})

// Fire-and-forget handlers (equivalent to ipcMain.on)
sendHandlers.set(IPC.INIT_SESSION, (_event: unknown, tabId: string) => {
  log(`IPC INIT_SESSION: ${tabId}`)
  controlPlane.initSession(tabId)
})

sendHandlers.set(IPC.RESET_TAB_SESSION, (_event: unknown, tabId: string) => {
  log(`IPC RESET_TAB_SESSION: ${tabId}`)
  controlPlane.resetTabSession(tabId)
})

sendHandlers.set(IPC.SET_PERMISSION_MODE, (_event: unknown, mode: string) => {
  if (mode !== 'ask' && mode !== 'auto') return
  log(`IPC SET_PERMISSION_MODE: ${mode}`)
  controlPlane.setPermissionMode(mode)
})

// No-ops (window management handled by GTK shell)
sendHandlers.set(IPC.RESIZE_HEIGHT, () => {})
sendHandlers.set(IPC.SET_WINDOW_WIDTH, () => {})
sendHandlers.set(IPC.HIDE_WINDOW, () => { sendShellCommand({ cmd: 'hide' }) })
invokeHandlers.set(IPC.ANIMATE_HEIGHT, () => {})

/* ─── Shell Communication ─── */

function sendShellCommand(cmd: Record<string, unknown>): void {
  if (!clientSocket || clientSocket.destroyed) return
  clientSocket.write(JSON.stringify(cmd) + '\n')
}

/* ─── Socket Server ─── */

function handleMessage(data: string): void {
  let parsed: any
  try {
    parsed = JSON.parse(data)
  } catch {
    log(`Invalid JSON from client: ${data.substring(0, 100)}`)
    return
  }

  if (parsed.type === 'invoke') {
    const handler = invokeHandlers.get(parsed.channel)
    if (!handler) {
      log(`No handler for invoke channel: ${parsed.channel}`)
      sendResponse({ type: 'reject', id: parsed.id, error: `Unknown channel: ${parsed.channel}` })
      return
    }

    try {
      const args = parsed.args || []
      // Handlers expect (event, ...args) — we pass null as event
      const result = handler(null, ...args)
      if (result && typeof result.then === 'function') {
        result.then(
          (val: any) => sendResponse({ type: 'resolve', id: parsed.id, result: val }),
          (err: any) => sendResponse({ type: 'reject', id: parsed.id, error: err?.message || String(err) })
        )
      } else {
        sendResponse({ type: 'resolve', id: parsed.id, result })
      }
    } catch (err: any) {
      sendResponse({ type: 'reject', id: parsed.id, error: err?.message || String(err) })
    }
  } else if (parsed.type === 'send') {
    const handler = sendHandlers.get(parsed.channel)
    if (handler) {
      const args = parsed.args || []
      handler(null, ...args)
    }
  }
}

function sendResponse(msg: Record<string, unknown>): void {
  if (!clientSocket || clientSocket.destroyed) return
  clientSocket.write(JSON.stringify(msg) + '\n')
}

/* ─── Exports ─── */

export function startIPCServer(socketPath: string): void {
  // Remove stale socket
  try { unlinkSync(socketPath) } catch {}

  const server = createServer((socket) => {
    log('GTK shell connected')
    clientSocket = socket

    let buffer = ''
    socket.on('data', (chunk) => {
      buffer += chunk.toString()
      const lines = buffer.split('\n')
      buffer = lines.pop() || ''
      for (const line of lines) {
        if (line.trim()) handleMessage(line)
      }
    })

    socket.on('close', () => {
      log('GTK shell disconnected')
      if (clientSocket === socket) clientSocket = null
    })

    socket.on('error', (err) => {
      log(`Socket error: ${err.message}`)
    })
  })

  server.listen(socketPath, () => {
    log(`IPC server listening on ${socketPath}`)
  })

  // Skill provisioning
  ensureSkills((status: SkillStatus) => {
    log(`Skill ${status.name}: ${status.state}${status.error ? ` — ${status.error}` : ''}`)
    broadcast(IPC.SKILL_STATUS, status)
  }).catch((err: Error) => log(`Skill provisioning error: ${err.message}`))

  // Theme change monitoring via gsettings
  try {
    const monitor = spawn('gsettings', ['monitor', 'org.gnome.desktop.interface', 'color-scheme'], {
      stdio: ['ignore', 'pipe', 'ignore'],
    })
    monitor.stdout?.on('data', (data: Buffer) => {
      const line = data.toString().trim()
      const isDark = line.includes('dark')
      broadcast(IPC.THEME_CHANGED, isDark)
    })
    monitor.unref()
  } catch {}

  // Cleanup on exit
  process.on('SIGTERM', () => shutdown(socketPath))
  process.on('SIGINT', () => shutdown(socketPath))
}

function shutdown(socketPath: string): void {
  controlPlane.shutdown()
  flushLogs()
  try { unlinkSync(socketPath) } catch {}
  process.exit(0)
}

export { shellProcess }
