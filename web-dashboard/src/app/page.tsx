'use client'

import { useState, useEffect, useCallback } from 'react'
import { useRouter } from 'next/navigation'
import { clsx } from 'clsx'
import {
  getStatus, getModels, getSites, getDiskUsage,
  addModel, removeModel, startModel, stopModel, restartModel,
  startAll, stopAll, saveConfig, checkAuth, logout, isAuthenticated,
  formatBytes, formatDuration, getStatusColor, getStatusBg,
  type BotState, type ServerStatus, type SiteInfo, type DiskUsage,
} from '@/lib/api'

// ── Icons (inline SVG to avoid deps) ────────────────────────────
const Icon = ({ d, className = '' }: { d: string; className?: string }) => (
  <svg className={clsx('w-4 h-4', className)} fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
    <path strokeLinecap="round" strokeLinejoin="round" d={d} />
  </svg>
)

const icons = {
  play: 'M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z',
  stop: 'M21 12a9 9 0 11-18 0 9 9 0 0118 0z M10 9v6 M14 9v6',
  refresh: 'M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15',
  trash: 'M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16',
  plus: 'M12 4v16m8-8H4',
  save: 'M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4',
  globe: 'M21 12a9 9 0 01-9 9m9-9a9 9 0 00-9-9m9 9H3m9 9a9 9 0 01-9-9m9 9c1.657 0 3-4.03 3-9s-1.343-9-3-9m0 18c-1.657 0-3-4.03-3-9s1.343-9 3-9m-9 9a9 9 0 019-9',
  hdd: 'M3 12l2-2m0 0l7-7 7 7M5 10v10a1 1 0 001 1h3m10-11l2 2m-2-2v10a1 1 0 01-1 1h-3m-6 0a1 1 0 001-1v-4a1 1 0 011-1h2a1 1 0 011 1v4a1 1 0 001 1m-6 0h6',
  rec: 'M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z',
}

// ── Stat Card ───────────────────────────────────────────────────
function StatCard({ title, value, subtitle, color, icon }: {
  title: string; value: string | number; subtitle?: string; color: string; icon: string
}) {
  return (
    <div className="bg-[var(--bg-card)] rounded-xl border border-[var(--border)] p-5 hover:border-[var(--accent)]/30 transition-all">
      <div className="flex items-center justify-between mb-3">
        <span className="text-sm text-[var(--text-secondary)]">{title}</span>
        <div className={clsx('p-2 rounded-lg', color)}>
          <Icon d={icon} className="w-5 h-5" />
        </div>
      </div>
      <div className="text-3xl font-bold tracking-tight">{value}</div>
      {subtitle && <div className="text-xs text-[var(--text-secondary)] mt-1">{subtitle}</div>}
    </div>
  )
}

// ── Model Row ───────────────────────────────────────────────────
function ModelRow({ bot, onStart, onStop, onRestart, onRemove }: {
  bot: BotState
  onStart: () => void
  onStop: () => void
  onRestart: () => void
  onRemove: () => void
}) {
  return (
    <div className={clsx(
      'group flex items-center gap-4 px-5 py-3.5 border-b border-[var(--border)]/50',
      'hover:bg-[var(--bg-hover)] transition-colors animate-slide-in',
      bot.recording && 'bg-red-500/5'
    )}>
      {/* Recording indicator */}
      <div className="w-2.5 h-2.5 rounded-full flex-shrink-0">
        {bot.recording ? (
          <div className="w-2.5 h-2.5 rounded-full bg-red-500 animate-recording" />
        ) : bot.running ? (
          <div className={clsx('w-2.5 h-2.5 rounded-full',
            bot.status === 'Public' ? 'bg-emerald-400' :
            bot.status === 'Online' ? 'bg-cyan-400' :
            bot.status === 'Offline' ? 'bg-yellow-400' :
            'bg-zinc-600'
          )} />
        ) : (
          <div className="w-2.5 h-2.5 rounded-full bg-zinc-700" />
        )}
      </div>

      {/* Username */}
      <div className="flex-1 min-w-0">
        <div className="flex items-center gap-2">
          <a href={bot.websiteUrl} target="_blank" rel="noopener noreferrer"
             className="font-medium text-white hover:text-[var(--accent)] transition-colors truncate">
            {bot.username}
          </a>
          {bot.mobile && (
            <span className="text-[10px] px-1.5 py-0.5 rounded bg-blue-500/20 text-blue-400 font-medium">📱</span>
          )}
        </div>
        <div className="text-xs text-[var(--text-secondary)]">{bot.site}</div>
      </div>

      {/* Status badge */}
      <div className={clsx(
        'px-2.5 py-1 rounded-full text-xs font-medium border',
        getStatusBg(bot.status),
        getStatusColor(bot.status)
      )}>
        {bot.status}
      </div>

      {/* Stats */}
      <div className="hidden md:flex items-center gap-6 text-xs text-[var(--text-secondary)]">
        {bot.recording && (
          <div className="flex items-center gap-1.5">
            <Icon d={icons.rec} className="w-3.5 h-3.5 text-red-400" />
            <span className="text-red-400 font-medium">
              {formatBytes(bot.recording_stats.bytesWritten)}
            </span>
            {bot.recording_stats.currentSpeed > 0 && (
              <span className="text-zinc-500">
                {bot.recording_stats.currentSpeed.toFixed(1)}x
              </span>
            )}
          </div>
        )}
        <span>{formatDuration(bot.uptimeSeconds)}</span>
      </div>

      {/* Actions */}
      <div className="flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
        {bot.running ? (
          <button onClick={onStop} title="Stop"
            className="p-1.5 rounded-lg hover:bg-yellow-500/20 text-yellow-400 transition-colors">
            <Icon d={icons.stop} className="w-4 h-4" />
          </button>
        ) : (
          <button onClick={onStart} title="Start"
            className="p-1.5 rounded-lg hover:bg-emerald-500/20 text-emerald-400 transition-colors">
            <Icon d={icons.play} className="w-4 h-4" />
          </button>
        )}
        <button onClick={onRestart} title="Restart"
          className="p-1.5 rounded-lg hover:bg-blue-500/20 text-blue-400 transition-colors">
          <Icon d={icons.refresh} className="w-4 h-4" />
        </button>
        <button onClick={onRemove} title="Remove"
          className="p-1.5 rounded-lg hover:bg-red-500/20 text-red-400 transition-colors">
          <Icon d={icons.trash} className="w-4 h-4" />
        </button>
      </div>
    </div>
  )
}

// ── Add Model Dialog ────────────────────────────────────────────
function AddModelDialog({ sites, onAdd, onClose }: {
  sites: SiteInfo[]
  onAdd: (username: string, site: string) => void
  onClose: () => void
}) {
  const [username, setUsername] = useState('')
  const [site, setSite] = useState(sites[0]?.name || '')

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 backdrop-blur-sm"
         onClick={onClose}>
      <div className="bg-[var(--bg-card)] border border-[var(--border)] rounded-2xl p-6 w-full max-w-md shadow-2xl"
           onClick={e => e.stopPropagation()}>
        <h2 className="text-xl font-bold mb-5">Add Model</h2>

        <div className="space-y-4">
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Username</label>
            <input
              type="text"
              value={username}
              onChange={e => setUsername(e.target.value)}
              placeholder="Enter username..."
              autoFocus
              className="w-full px-4 py-2.5 bg-[var(--bg-primary)] border border-[var(--border)] rounded-xl
                         text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]
                         transition-colors"
            />
          </div>
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Site</label>
            <select
              value={site}
              onChange={e => setSite(e.target.value)}
              className="w-full px-4 py-2.5 bg-[var(--bg-primary)] border border-[var(--border)] rounded-xl
                         text-white focus:outline-none focus:border-[var(--accent)] transition-colors"
            >
              {sites.map(s => (
                <option key={s.name} value={s.name}>{s.name} [{s.slug}]</option>
              ))}
            </select>
          </div>
        </div>

        <div className="flex gap-3 mt-6">
          <button onClick={onClose}
            className="flex-1 px-4 py-2.5 rounded-xl border border-[var(--border)]
                       text-[var(--text-secondary)] hover:bg-[var(--bg-hover)] transition-colors">
            Cancel
          </button>
          <button
            onClick={() => { if (username.trim()) onAdd(username.trim(), site) }}
            disabled={!username.trim()}
            className="flex-1 px-4 py-2.5 rounded-xl bg-[var(--accent)] text-white font-medium
                       hover:bg-indigo-500 transition-colors disabled:opacity-40 disabled:cursor-not-allowed">
            Add Model
          </button>
        </div>
      </div>
    </div>
  )
}

// ── Disk Usage Bar ──────────────────────────────────────────────
function DiskBar({ disk }: { disk: DiskUsage | null }) {
  if (!disk || disk.totalBytes === 0) return null
  const usedPct = disk.usedPercent
  const downloadPct = (disk.downloadDirBytes / disk.totalBytes) * 100

  return (
    <div className="bg-[var(--bg-card)] rounded-xl border border-[var(--border)] p-5">
      <div className="flex items-center justify-between mb-3">
        <span className="text-sm font-medium">Disk Usage</span>
        <span className="text-xs text-[var(--text-secondary)]">
          {formatBytes(disk.freeBytes)} free of {formatBytes(disk.totalBytes)}
        </span>
      </div>
      <div className="h-3 bg-[var(--bg-primary)] rounded-full overflow-hidden">
        <div className="h-full rounded-full bg-gradient-to-r from-indigo-500 to-purple-500 transition-all duration-500"
             style={{ width: `${Math.min(usedPct, 100)}%` }} />
      </div>
      <div className="flex items-center justify-between mt-2 text-xs text-[var(--text-secondary)]">
        <span>Downloads: {formatBytes(disk.downloadDirBytes)} ({disk.fileCount} files)</span>
        <span className={usedPct > 90 ? 'text-red-400 font-medium' : ''}>
          {usedPct.toFixed(1)}% used
        </span>
      </div>
    </div>
  )
}

// ── Main Dashboard ──────────────────────────────────────────────
export default function Dashboard() {
  const router = useRouter()
  const [status, setStatus] = useState<ServerStatus | null>(null)
  const [models, setModels] = useState<BotState[]>([])
  const [sites, setSites] = useState<SiteInfo[]>([])
  const [disk, setDisk] = useState<DiskUsage | null>(null)
  const [filter, setFilter] = useState<'all' | 'recording' | 'online' | 'offline'>('all')
  const [search, setSearch] = useState('')
  const [showAdd, setShowAdd] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [connected, setConnected] = useState(true)
  const [authChecked, setAuthChecked] = useState(false)

  // Check authentication on mount
  useEffect(() => {
    const verifyAuth = async () => {
      // First check localStorage token
      if (!isAuthenticated()) {
        router.push('/login')
        return
      }
      // Then verify with server
      const res = await checkAuth()
      if (!res.authenticated) {
        router.push('/login')
        return
      }
      setAuthChecked(true)
    }
    verifyAuth()
  }, [router])

  const refresh = useCallback(async () => {
    try {
      const [s, m, d] = await Promise.all([getStatus(), getModels(), getDiskUsage()])
      setStatus(s)
      setModels(m)
      setDisk(d)
      setConnected(true)
      setError(null)
    } catch (e: unknown) {
      if (e instanceof Error && e.message === 'AUTH_REQUIRED') {
        router.push('/login')
        return
      }
      setConnected(false)
    }
  }, [router])

  useEffect(() => {
    if (!authChecked) return
    refresh()
    getSites().then(setSites).catch(() => {})
    const interval = setInterval(refresh, 2000)
    return () => clearInterval(interval)
  }, [refresh, authChecked])

  const handleLogout = async () => {
    await logout()
    router.push('/login')
  }

  // Don't render until auth is verified
  if (!authChecked) {
    return (
      <div className="min-h-screen bg-[var(--bg-primary)] flex items-center justify-center">
        <div className="text-center">
          <div className="w-12 h-12 border-4 border-[var(--accent)] border-t-transparent rounded-full animate-spin mx-auto mb-4" />
          <p className="text-[var(--text-secondary)]">Loading...</p>
        </div>
      </div>
    )
  }

  const filteredModels = models
    .filter(m => {
      if (filter === 'recording') return m.recording
      if (filter === 'online') return m.status === 'Public' || m.status === 'Online'
      if (filter === 'offline') return !m.running || m.status === 'Offline' || m.status === 'Long Offline'
      return true
    })
    .filter(m => !search || m.username.toLowerCase().includes(search.toLowerCase()) ||
                 m.site.toLowerCase().includes(search.toLowerCase()))
    .sort((a, b) => {
      // Recording first, then online, then rest
      if (a.recording !== b.recording) return a.recording ? -1 : 1
      if ((a.status === 'Public') !== (b.status === 'Public'))
        return a.status === 'Public' ? -1 : 1
      return a.username.localeCompare(b.username)
    })

  const handleAdd = async (username: string, site: string) => {
    try {
      await addModel(username, site)
      setShowAdd(false)
      refresh()
    } catch (e: any) {
      setError(e.message)
    }
  }

  const handleAction = async (action: () => Promise<any>) => {
    try {
      await action()
      setTimeout(refresh, 300)
    } catch (e: any) {
      setError(e.message)
    }
  }

  return (
    <div className="min-h-screen bg-[var(--bg-primary)]">
      {/* Header */}
      <header className="sticky top-0 z-40 bg-[var(--bg-primary)]/80 backdrop-blur-xl border-b border-[var(--border)]">
        <div className="max-w-7xl mx-auto px-6 py-4 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="text-2xl">🎬</div>
            <div>
              <h1 className="text-lg font-bold tracking-tight">StreaMonitor</h1>
              <div className="flex items-center gap-2">
                <div className={clsx('w-1.5 h-1.5 rounded-full', connected ? 'bg-emerald-400' : 'bg-red-400')} />
                <span className="text-xs text-[var(--text-secondary)]">
                  {connected ? `v${status?.version || '2.0'}` : 'Disconnected'}
                </span>
              </div>
            </div>
          </div>

          <div className="flex items-center gap-2">
            <button onClick={() => handleAction(startAll)}
              className="px-3 py-1.5 text-sm rounded-lg bg-emerald-500/10 text-emerald-400
                         hover:bg-emerald-500/20 border border-emerald-500/20 transition-colors">
              Start All
            </button>
            <button onClick={() => handleAction(stopAll)}
              className="px-3 py-1.5 text-sm rounded-lg bg-yellow-500/10 text-yellow-400
                         hover:bg-yellow-500/20 border border-yellow-500/20 transition-colors">
              Stop All
            </button>
            <button onClick={() => handleAction(saveConfig)}
              className="px-3 py-1.5 text-sm rounded-lg bg-blue-500/10 text-blue-400
                         hover:bg-blue-500/20 border border-blue-500/20 transition-colors">
              <Icon d={icons.save} />
            </button>
            <button onClick={handleLogout}
              className="px-3 py-1.5 text-sm rounded-lg bg-zinc-500/10 text-zinc-400
                         hover:bg-zinc-500/20 border border-zinc-500/20 transition-colors"
              title="Logout">
              <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
                <path strokeLinecap="round" strokeLinejoin="round" d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1" />
              </svg>
            </button>
          </div>
        </div>
      </header>

      <main className="max-w-7xl mx-auto px-6 py-6 space-y-6">
        {/* Error banner */}
        {error && (
          <div className="bg-red-500/10 border border-red-500/20 rounded-xl px-4 py-3 flex items-center justify-between">
            <span className="text-sm text-red-400">{error}</span>
            <button onClick={() => setError(null)} className="text-red-400 hover:text-red-300">✕</button>
          </div>
        )}

        {/* Stat cards */}
        <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
          <StatCard title="Total Models" value={status?.totalModels ?? 0}
            color="bg-indigo-500/10 text-indigo-400" icon={icons.globe} />
          <StatCard title="Online" value={status?.onlineModels ?? 0}
            subtitle={`${models.filter(m => m.status === 'Public').length} public`}
            color="bg-emerald-500/10 text-emerald-400" icon={icons.play} />
          <StatCard title="Recording" value={models.filter(m => m.recording).length}
            subtitle={formatBytes(models.reduce((a, m) => a + (m.recording ? m.recording_stats.bytesWritten : 0), 0))}
            color="bg-red-500/10 text-red-400" icon={icons.rec} />
          <StatCard title="Disk Free" value={disk ? formatBytes(disk.freeBytes) : '–'}
            subtitle={disk ? `${disk.usedPercent.toFixed(1)}% used` : ''}
            color="bg-purple-500/10 text-purple-400" icon={icons.hdd} />
        </div>

        {/* Disk bar */}
        <DiskBar disk={disk} />

        {/* Model list header */}
        <div className="flex flex-col sm:flex-row items-start sm:items-center justify-between gap-4">
          <div className="flex items-center gap-2 flex-wrap">
            {(['all', 'recording', 'online', 'offline'] as const).map(f => (
              <button key={f} onClick={() => setFilter(f)}
                className={clsx(
                  'px-3 py-1.5 text-sm rounded-lg border transition-colors capitalize',
                  filter === f
                    ? 'bg-[var(--accent)]/10 border-[var(--accent)]/30 text-[var(--accent)]'
                    : 'bg-transparent border-[var(--border)] text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]'
                )}>
                {f}
                {f === 'recording' && (
                  <span className="ml-1.5 text-xs">{models.filter(m => m.recording).length}</span>
                )}
                {f === 'online' && (
                  <span className="ml-1.5 text-xs">{models.filter(m => m.status === 'Public' || m.status === 'Online').length}</span>
                )}
              </button>
            ))}
          </div>

          <div className="flex items-center gap-3 w-full sm:w-auto">
            <input
              type="text"
              value={search}
              onChange={e => setSearch(e.target.value)}
              placeholder="Search models..."
              className="flex-1 sm:w-56 px-3 py-1.5 text-sm bg-[var(--bg-secondary)] border border-[var(--border)]
                         rounded-lg text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]/50"
            />
            <button onClick={() => setShowAdd(true)}
              className="flex items-center gap-1.5 px-3 py-1.5 text-sm rounded-lg bg-[var(--accent)]
                         text-white font-medium hover:bg-indigo-500 transition-colors whitespace-nowrap">
              <Icon d={icons.plus} className="w-4 h-4" />
              Add Model
            </button>
          </div>
        </div>

        {/* Model list */}
        <div className="bg-[var(--bg-card)] rounded-xl border border-[var(--border)] overflow-hidden">
          {/* Header */}
          <div className="flex items-center gap-4 px-5 py-3 border-b border-[var(--border)] text-xs
                          text-[var(--text-secondary)] font-medium uppercase tracking-wider">
            <div className="w-2.5" />
            <div className="flex-1">Model</div>
            <div className="w-24 text-center">Status</div>
            <div className="hidden md:block w-48 text-right">Stats</div>
            <div className="w-28" />
          </div>

          {/* Rows */}
          {filteredModels.length === 0 ? (
            <div className="px-5 py-12 text-center text-[var(--text-secondary)]">
              {models.length === 0 ? (
                <div>
                  <div className="text-4xl mb-3">🎬</div>
                  <div className="font-medium">No models added yet</div>
                  <div className="text-sm mt-1">Click &quot;Add Model&quot; to start monitoring</div>
                </div>
              ) : (
                <div>No models match the current filter</div>
              )}
            </div>
          ) : (
            filteredModels.map(bot => (
              <ModelRow
                key={`${bot.username}_${bot.siteSlug}`}
                bot={bot}
                onStart={() => handleAction(() => startModel(bot.username, bot.siteSlug))}
                onStop={() => handleAction(() => stopModel(bot.username, bot.siteSlug))}
                onRestart={() => handleAction(() => restartModel(bot.username, bot.siteSlug))}
                onRemove={() => {
                  if (confirm(`Remove ${bot.username} [${bot.siteSlug}]?`))
                    handleAction(() => removeModel(bot.username, bot.siteSlug))
                }}
              />
            ))
          )}
        </div>

        {/* Footer */}
        <div className="text-center text-xs text-[var(--text-secondary)] py-4">
          StreaMonitor v2.0 • C++ Engine • {models.length} models monitored
        </div>
      </main>

      {/* Add model dialog */}
      {showAdd && <AddModelDialog sites={sites} onAdd={handleAdd} onClose={() => setShowAdd(false)} />}
    </div>
  )
}
