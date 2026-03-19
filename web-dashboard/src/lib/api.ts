// ─────────────────────────────────────────────────────────────────
// StreaMonitor Dashboard — API Client
// Connects to the C++ embedded REST API with authentication
// ─────────────────────────────────────────────────────────────────

const API_BASE = typeof window !== 'undefined'
  ? `${window.location.protocol}//${window.location.host}/api`
  : '/api'

// Token stored in memory (also sent via HttpOnly cookie)
let authToken: string | null = null

export interface BotState {
  username: string
  site: string
  siteSlug: string
  status: string
  statusCode: number
  prevStatus: string
  running: boolean
  recording: boolean
  quitting: boolean
  mobile: boolean
  gender: string
  country: string
  websiteUrl: string
  previewUrl: string
  groupName: string
  roomId: string
  consecutiveErrors: number
  fileCount: number
  totalBytes: number
  currentFile: string
  uptimeSeconds: number
  timeSinceStatusChange: number
  recording_stats: {
    bytesWritten: number
    segmentsRecorded: number
    stallsDetected: number
    restartsPerformed: number
    currentSpeed: number
    currentFile: string
  }
}

export interface ServerStatus {
  version: string
  totalModels: number
  onlineModels: number
  recordingModels: number
  wsPort: number
  disk: {
    totalBytes: number
    freeBytes: number
    downloadDirBytes: number
    fileCount: number
  }
}

export interface SiteInfo {
  name: string
  slug: string
}

export interface GroupInfo {
  name: string
  members: { site: string; username: string }[]
  linkRecording: boolean
  linkStatus: boolean
  state?: {
    running: boolean
    recording: boolean
    activePairingIdx: number
    activeUsername: string
    activeSite: string
    activeStatus: string
    activeMobile: boolean
    pairings?: {
      site: string
      username: string
      status: string
      mobile: boolean
    }[]
  }
}

export interface DiskUsage {
  totalBytes: number
  freeBytes: number
  downloadDirBytes: number
  fileCount: number
  usedPercent: number
}

export interface AppConfig {
  downloadsDir: string
  container: string
  wantedResolution: number
  webEnabled: boolean
  webHost: string
  webPort: number
  debug: boolean
  enablePreviewCapture: boolean
  minFreeDiskPercent: number
  httpTimeoutSec: number
  userAgent: string
}

export interface AuthResponse {
  success: boolean
  token?: string
  error?: string
  message?: string
  authenticated?: boolean
  username?: string
}

// ── Auth Functions ────────────────────────────────────────────────
export function setAuthToken(token: string | null) {
  authToken = token
  if (typeof window !== 'undefined') {
    if (token) {
      localStorage.setItem('sm_token', token)
    } else {
      localStorage.removeItem('sm_token')
    }
  }
}

export function getAuthToken(): string | null {
  // Always try localStorage first on client (in case page was refreshed)
  if (typeof window !== 'undefined') {
    const stored = localStorage.getItem('sm_token')
    if (stored) {
      authToken = stored  // Sync in-memory with localStorage
    }
  }
  return authToken
}

export function isAuthenticated(): boolean {
  // On server-side rendering, assume not authenticated
  if (typeof window === 'undefined') return false
  return !!getAuthToken()
}

// ── Base fetch with auth ──────────────────────────────────────────
async function apiFetch<T>(path: string, options?: RequestInit): Promise<T> {
  const token = getAuthToken()
  const headers: HeadersInit = {
    'Content-Type': 'application/json',
    ...options?.headers,
  }
  if (token) {
    (headers as Record<string, string>)['Authorization'] = `Bearer ${token}`
  }

  const res = await fetch(`${API_BASE}${path}`, {
    ...options,
    headers,
    credentials: 'include', // Include cookies
  })

  if (res.status === 401) {
    // Clear invalid token
    setAuthToken(null)
    throw new Error('AUTH_REQUIRED')
  }

  if (!res.ok) {
    const err = await res.json().catch(() => ({ error: res.statusText }))
    throw new Error(err.error || `API Error: ${res.status}`)
  }
  return res.json()
}

// ── Auth API ──────────────────────────────────────────────────────
export async function login(username: string, password: string): Promise<AuthResponse> {
  const res = await fetch(`${API_BASE}/auth/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password }),
    credentials: 'include',
  })
  const data = await res.json()
  if (data.success && data.token) {
    setAuthToken(data.token)
  }
  return data
}

export async function logout(): Promise<void> {
  try {
    await apiFetch('/auth/logout', { method: 'POST' })
  } finally {
    setAuthToken(null)
  }
}

export async function checkAuth(): Promise<AuthResponse> {
  try {
    return await apiFetch<AuthResponse>('/auth/check')
  } catch {
    return { success: false, authenticated: false }
  }
}

export async function updateCredentials(
  currentPassword: string,
  newUsername?: string,
  newPassword?: string
): Promise<AuthResponse> {
  return apiFetch('/auth/credentials', {
    method: 'PUT',
    body: JSON.stringify({
      currentPassword,
      username: newUsername,
      password: newPassword,
    }),
  })
}

// ── Status ────────────────────────────────────────────────────────
export const getStatus = () => apiFetch<ServerStatus>('/status')
export const getModels = () => apiFetch<BotState[]>('/models')
export const getSites = () => apiFetch<SiteInfo[]>('/sites')
export const getDiskUsage = () => apiFetch<DiskUsage>('/disk')
export const getConfig = () => apiFetch<AppConfig>('/config')
export const getGroups = () => apiFetch<GroupInfo[]>('/groups')

// ── Actions ───────────────────────────────────────────────────────
export const addModel = (username: string, site: string, autoStart = true) =>
  apiFetch('/models', {
    method: 'POST',
    body: JSON.stringify({ username, site, autoStart }),
  })

export const removeModel = (username: string, siteSlug: string) =>
  apiFetch(`/models/${username}/${siteSlug}`, { method: 'DELETE' })

export const startModel = (username: string, siteSlug: string) =>
  apiFetch(`/models/${username}/${siteSlug}/start`, { method: 'POST' })

export const stopModel = (username: string, siteSlug: string) =>
  apiFetch(`/models/${username}/${siteSlug}/stop`, { method: 'POST' })

export const restartModel = (username: string, siteSlug: string) =>
  apiFetch(`/models/${username}/${siteSlug}/restart`, { method: 'POST' })

export const startAll = () =>
  apiFetch('/start-all', { method: 'POST' })

export const stopAll = () =>
  apiFetch('/stop-all', { method: 'POST' })

export const saveConfig = () =>
  apiFetch('/save', { method: 'POST' })

export const updateConfig = (config: Partial<AppConfig>) =>
  apiFetch('/config', { method: 'PUT', body: JSON.stringify(config) })

export const createGroup = (name: string, members: { site: string; username: string }[] = []) =>
  apiFetch('/groups', { method: 'POST', body: JSON.stringify({ name, members }) })

export const deleteGroup = (name: string) =>
  apiFetch(`/groups/${encodeURIComponent(name)}`, { method: 'DELETE' })

export const startGroup = (name: string) =>
  apiFetch(`/groups/${encodeURIComponent(name)}/start`, { method: 'POST' })

export const stopGroup = (name: string) =>
  apiFetch(`/groups/${encodeURIComponent(name)}/stop`, { method: 'POST' })

export const addGroupMember = (groupName: string, site: string, username: string) =>
  apiFetch(`/groups/${encodeURIComponent(groupName)}/members`, {
    method: 'POST',
    body: JSON.stringify({ site, username }),
  })

export const removeGroupMember = (groupName: string, site: string, username: string) =>
  apiFetch(`/groups/${encodeURIComponent(groupName)}/members`, {
    method: 'DELETE',
    body: JSON.stringify({ site, username }),
  })

export const getLogs = () => apiFetch<string[]>('/logs')

export function getPreviewUrl(username: string, siteSlug: string): string {
  return `${API_BASE.replace('/api', '')}/api/preview/${encodeURIComponent(username)}/${encodeURIComponent(siteSlug)}`
}

export function getStreamUrl(username: string, siteSlug: string): string {
  return `${API_BASE.replace('/api', '')}/api/stream/${encodeURIComponent(username)}/${encodeURIComponent(siteSlug)}`
}

// ── WebSocket Preview Manager ─────────────────────────────────────
// Multiplexes all preview streams over a single WebSocket connection
// to bypass browser's 6-connection HTTP/1.1 limit. Unlimited live previews!

type FrameCallback = (jpeg: Blob) => void

class PreviewWebSocket {
  private ws: WebSocket | null = null
  private wsPort: number | null = null
  private subscriptions = new Map<string, Set<FrameCallback>>()
  private reconnectTimeout: ReturnType<typeof setTimeout> | null = null
  private connecting = false

  private makeKey(username: string, site: string): string {
    return `${username}:${site}`
  }

  async connect(wsPort: number): Promise<void> {
    if (this.ws && this.ws.readyState === WebSocket.OPEN && this.wsPort === wsPort) {
      return // Already connected
    }

    if (this.connecting) return
    this.connecting = true
    this.wsPort = wsPort

    return new Promise((resolve, reject) => {
      const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
      const wsUrl = `${wsProtocol}//${window.location.hostname}:${wsPort}`

      this.ws = new WebSocket(wsUrl)
      this.ws.binaryType = 'arraybuffer'

      this.ws.onopen = () => {
        this.connecting = false
        console.log('[WS] Connected to preview server')

        // Re-subscribe to all existing subscriptions
        for (const key of this.subscriptions.keys()) {
          const [username, site] = key.split(':')
          this.sendSubscribe(username, site)
        }
        resolve()
      }

      this.ws.onclose = () => {
        this.connecting = false
        console.log('[WS] Disconnected, will reconnect...')
        this.scheduleReconnect()
      }

      this.ws.onerror = (err) => {
        this.connecting = false
        console.error('[WS] Error:', err)
        reject(err)
      }

      this.ws.onmessage = (event) => {
        this.handleMessage(event.data)
      }
    })
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimeout) return
    this.reconnectTimeout = setTimeout(() => {
      this.reconnectTimeout = null
      if (this.wsPort && this.subscriptions.size > 0) {
        this.connect(this.wsPort).catch(() => {})
      }
    }, 2000)
  }

  private handleMessage(data: ArrayBuffer | string): void {
    if (typeof data === 'string') {
      // JSON message (ack, etc.)
      return
    }

    // Binary frame: 1 byte type + 2 bytes username len + username + 2 bytes site len + site + jpeg
    const view = new DataView(data)
    const type = view.getUint8(0)
    if (type !== 0x01) return // Not a frame

    let offset = 1
    const usernameLen = view.getUint16(offset)
    offset += 2
    const username = new TextDecoder().decode(new Uint8Array(data, offset, usernameLen))
    offset += usernameLen

    const siteLen = view.getUint16(offset)
    offset += 2
    const site = new TextDecoder().decode(new Uint8Array(data, offset, siteLen))
    offset += siteLen

    const jpeg = new Blob([new Uint8Array(data, offset)], { type: 'image/jpeg' })

    // Dispatch to subscribers
    const key = this.makeKey(username, site)
    const callbacks = this.subscriptions.get(key)
    if (callbacks) {
      for (const cb of callbacks) {
        cb(jpeg)
      }
    }
  }

  private sendSubscribe(username: string, site: string): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ cmd: 'subscribe', username, site }))
    }
  }

  private sendUnsubscribe(username: string, site: string): void {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ cmd: 'unsubscribe', username, site }))
    }
  }

  subscribe(username: string, site: string, callback: FrameCallback): () => void {
    const key = this.makeKey(username, site)

    let callbacks = this.subscriptions.get(key)
    if (!callbacks) {
      callbacks = new Set()
      this.subscriptions.set(key, callbacks)
      this.sendSubscribe(username, site)
    }
    callbacks.add(callback)

    // Return unsubscribe function
    return () => {
      const cbs = this.subscriptions.get(key)
      if (cbs) {
        cbs.delete(callback)
        if (cbs.size === 0) {
          this.subscriptions.delete(key)
          this.sendUnsubscribe(username, site)
        }
      }
    }
  }

  disconnect(): void {
    if (this.reconnectTimeout) {
      clearTimeout(this.reconnectTimeout)
      this.reconnectTimeout = null
    }
    if (this.ws) {
      this.ws.close()
      this.ws = null
    }
    this.subscriptions.clear()
  }
}

// Singleton instance
export const previewWs = new PreviewWebSocket()

// ── Helpers ───────────────────────────────────────────────────────
export function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return `${(bytes / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`
}

export function formatDuration(seconds: number): string {
  if (seconds < 60) return `${seconds}s`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return `${h}h ${m}m`
}

export function getStatusColor(status: string): string {
  switch (status) {
    case 'Public': return 'text-emerald-400'
    case 'Private': return 'text-purple-400'
    case 'Offline': return 'text-yellow-400'
    case 'Long Offline': return 'text-zinc-500'
    case 'Online': return 'text-cyan-400'
    case 'Error': return 'text-red-400'
    case 'Rate Limited': return 'text-orange-400'
    case 'Not Found': return 'text-red-500'
    case 'Deleted': return 'text-red-600'
    case 'Not Running': return 'text-zinc-600'
    default: return 'text-zinc-400'
  }
}

export function getStatusBg(status: string): string {
  switch (status) {
    case 'Public': return 'bg-emerald-500/10 border-emerald-500/20'
    case 'Private': return 'bg-purple-500/10 border-purple-500/20'
    case 'Offline': return 'bg-yellow-500/10 border-yellow-500/20'
    case 'Online': return 'bg-cyan-500/10 border-cyan-500/20'
    case 'Error': return 'bg-red-500/10 border-red-500/20'
    case 'Rate Limited': return 'bg-orange-500/10 border-orange-500/20'
    default: return 'bg-zinc-500/10 border-zinc-500/20'
  }
}
