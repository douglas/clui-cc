/**
 * bridge.js — Injected into WebKitGTK to provide window.clui API
 *
 * Replaces Electron's contextBridge/ipcRenderer with WebKitGTK message handlers.
 * Communication flow:
 *   renderer → window.webkit.messageHandlers.clui.postMessage(msg) → GTK shell → Node.js backend
 *   Node.js backend → GTK shell (eval cmd) → JS callback in renderer
 */
;(function () {
  'use strict'

  /* Guard against double-injection (UCM + evaluate_javascript fallback) */
  if (window.clui) {
    console.log('[clui bridge] already initialized, skipping')
    return
  }
  console.log('[clui bridge] initializing...')

  /* ─── Pending invoke callbacks ─── */
  const pendingInvokes = new Map() // id → { resolve, reject }
  let invokeCounter = 0

  /* ─── Event listeners ─── */
  const listeners = new Map() // channel → Set<callback>

  function addListener(channel, callback) {
    if (!listeners.has(channel)) listeners.set(channel, new Set())
    listeners.get(channel).add(callback)
    return () => listeners.get(channel)?.delete(callback)
  }

  /* Called by GTK shell via webkit_web_view_evaluate_javascript */
  window.__cluiDispatch = function (channel, ...args) {
    const cbs = listeners.get(channel)
    if (cbs) cbs.forEach((cb) => cb(...args))
  }

  /* Called by GTK shell when an invoke resolves */
  window.__cluiResolve = function (id, result) {
    const pending = pendingInvokes.get(id)
    if (pending) {
      pendingInvokes.delete(id)
      pending.resolve(result)
    }
  }

  /* Called by GTK shell when an invoke rejects */
  window.__cluiReject = function (id, error) {
    const pending = pendingInvokes.get(id)
    if (pending) {
      pendingInvokes.delete(id)
      pending.reject(new Error(error))
    }
  }

  /* Window shown callback (from toggle_visibility in GTK shell) */
  window.__cluiOnWindowShown = function () {
    window.__cluiDispatch('clui:window-shown')
  }

  /**
   * Central handler for all backend messages relayed through GTK shell.
   * The C shell calls this with a JSON string for resolve/reject/broadcast.
   */
  window.__cluiHandleBackendMessage = function (jsonStr) {
    try {
      const msg = JSON.parse(jsonStr)
      if (msg.type === 'resolve') {
        window.__cluiResolve(msg.id, msg.result)
      } else if (msg.type === 'reject') {
        window.__cluiReject(msg.id, msg.error)
      } else if (msg.type === 'broadcast') {
        window.__cluiDispatch(msg.channel, ...msg.args)
      }
    } catch (e) {
      console.error('[clui bridge] Failed to parse backend message:', e)
    }
  }

  /**
   * Send a message to the Node.js backend (invoke = request/response).
   * The GTK shell relays this over the Unix socket and routes the response back.
   */
  function invoke(channel, ...args) {
    const id = ++invokeCounter
    return new Promise((resolve, reject) => {
      pendingInvokes.set(id, { resolve, reject })
      window.webkit.messageHandlers.clui.postMessage({
        type: 'invoke',
        id: id,
        channel: channel,
        args: args,
      })
    })
  }

  /**
   * Send a fire-and-forget message to the backend (like ipcRenderer.send).
   */
  function send(channel, ...args) {
    window.webkit.messageHandlers.clui.postMessage({
      type: 'send',
      channel: channel,
      args: args,
    })
  }

  /**
   * Send a command directly to the GTK shell (window management).
   */
  function shellCmd(channel, ...args) {
    window.webkit.messageHandlers.clui.postMessage({
      channel: channel,
      args: args,
    })
  }

  /* ─── Build the CluiAPI matching src/preload/index.ts ─── */

  const api = {
    // ─── Request-response ───
    start: () => invoke('clui:start'),
    createTab: () => invoke('clui:create-tab'),
    prompt: (tabId, requestId, options) =>
      invoke('clui:prompt', { tabId, requestId, options }),
    cancel: (requestId) => invoke('clui:cancel', requestId),
    stopTab: (tabId) => invoke('clui:stop-tab', tabId),
    retry: (tabId, requestId, options) =>
      invoke('clui:retry', { tabId, requestId, options }),
    status: () => invoke('clui:status'),
    tabHealth: () => invoke('clui:tab-health'),
    closeTab: (tabId) => invoke('clui:close-tab', tabId),
    selectDirectory: () => invoke('clui:select-directory'),
    openExternal: (url) => invoke('clui:open-external', url),
    openInTerminal: (sessionId, projectPath) =>
      invoke('clui:open-in-terminal', { sessionId, projectPath }),
    attachFiles: () => invoke('clui:attach-files'),
    takeScreenshot: () => invoke('clui:take-screenshot'),
    pasteImage: (dataUrl) => invoke('clui:paste-image', dataUrl),
    transcribeAudio: (audioBase64) =>
      invoke('clui:transcribe-audio', audioBase64),
    getDiagnostics: () => invoke('clui:get-diagnostics'),
    respondPermission: (tabId, questionId, optionId) =>
      invoke('clui:respond-permission', { tabId, questionId, optionId }),
    initSession: (tabId) => send('clui:init-session', tabId),
    resetTabSession: (tabId) => send('clui:reset-tab-session', tabId),
    listSessions: (projectPath) => invoke('clui:list-sessions', projectPath),
    loadSession: (sessionId, projectPath) =>
      invoke('clui:load-session', { sessionId, projectPath }),
    fetchMarketplace: (forceRefresh) =>
      invoke('clui:marketplace-fetch', { forceRefresh }),
    listInstalledPlugins: () => invoke('clui:marketplace-installed'),
    installPlugin: (repo, pluginName, marketplace, sourcePath, isSkillMd) =>
      invoke('clui:marketplace-install', {
        repo,
        pluginName,
        marketplace,
        sourcePath,
        isSkillMd,
      }),
    uninstallPlugin: (pluginName) =>
      invoke('clui:marketplace-uninstall', { pluginName }),
    setPermissionMode: (mode) => send('clui:set-permission-mode', mode),
    getTheme: () => invoke('clui:get-theme'),
    onThemeChange: (callback) => addListener('clui:theme-changed', callback),

    // ─── Window management ───
    resizeHeight: () => {}, // no-op on Linux (fixed height)
    setWindowWidth: () => {}, // no-op on Linux (fixed width)
    animateHeight: () => Promise.resolve(), // no-op
    hideWindow: () => shellCmd('clui:hide-window'),
    isVisible: () => invoke('clui:is-visible'),

    // Click-through: on Linux we use input regions instead of setIgnoreMouseEvents.
    // The renderer sends the bounding rect of interactive UI.
    setIgnoreMouseEvents: (ignore, options) => {
      // This is called per-mousemove on macOS. On Linux, we use input regions
      // set from the renderer's layout. This is a compatibility shim.
      // The actual input region is managed by the renderer calling
      // window.__cluiSetInputRegion() below.
    },

    // ─── Event listeners ───
    onEvent: (callback) =>
      addListener('clui:normalized-event', (tabId, event) =>
        callback(tabId, event)
      ),
    onTabStatusChange: (callback) =>
      addListener('clui:tab-status-change', (tabId, newStatus, oldStatus) =>
        callback(tabId, newStatus, oldStatus)
      ),
    onError: (callback) =>
      addListener('clui:enriched-error', (tabId, error) =>
        callback(tabId, error)
      ),
    onSkillStatus: (callback) =>
      addListener('clui:skill-status', (status) => callback(status)),
    onWindowShown: (callback) =>
      addListener('clui:window-shown', () => callback()),
  }

  /**
   * Linux-specific: set the interactive input region directly.
   * Called by the renderer when its layout changes.
   */
  window.__cluiSetInputRegion = function (x, y, width, height) {
    shellCmd('clui:set-input-region', x, y, width, height)
  }

  /* Expose as window.clui — matches Electron's contextBridge.exposeInMainWorld */
  Object.defineProperty(window, 'clui', {
    value: Object.freeze(api),
    writable: false,
    configurable: false,
  })
})()
