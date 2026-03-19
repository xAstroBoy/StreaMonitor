'use client'

import { useState, useEffect, useCallback, useRef } from 'react'
import { useRouter } from 'next/navigation'
import { clsx } from 'clsx'
import {
  getStatus, getModels, getSites, getDiskUsage, getGroups, getLogs, getConfig,
  addModel, removeModel, startModel, stopModel, restartModel,
  startAll, stopAll, saveConfig, updateConfig, checkAuth, logout, isAuthenticated,
  createGroup, deleteGroup, startGroup, stopGroup, addGroupMember, removeGroupMember,
  formatBytes, formatDuration, getStatusColor, getStatusBg, getPreviewUrl,
  type BotState, type ServerStatus, type SiteInfo, type DiskUsage, type GroupInfo, type AppConfig,
} from '@/lib/api'

// ── Icons (inline SVG) ──────────────────────────────────────────
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
  link: 'M13.828 10.172a4 4 0 00-5.656 0l-4 4a4 4 0 105.656 5.656l1.102-1.101m-.758-4.899a4 4 0 005.656 0l4-4a4 4 0 00-5.656-5.656l-1.1 1.1',
  log: 'M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2',
  cog: 'M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z M15 12a3 3 0 11-6 0 3 3 0 016 0z',
  x: 'M6 18L18 6M6 6l12 12',
}

type TabId = 'models' | 'groups' | 'logs' | 'settings'

// ══════════════════════════════════════════════════════════════════
// Preview Thumbnail — auto-refreshes every 30 seconds
// ══════════════════════════════════════════════════════════════════
function PreviewThumb({ username, siteSlug }: { username: string; siteSlug: string }) {
  const [ts, setTs] = useState(Date.now())
  const [errored, setErrored] = useState(false)

  useEffect(() => {
    const iv = setInterval(() => { setTs(Date.now()); setErrored(false) }, 30000)
    return () => clearInterval(iv)
  }, [])

  if (errored) {
    return (
      <div className="w-10 h-10 rounded-lg bg-[var(--bg-primary)] flex items-center justify-center text-zinc-700 text-xs flex-shrink-0">
        📷
      </div>
    )
  }

  return (
    // eslint-disable-next-line @next/next/no-img-element
    <img
      src={`${getPreviewUrl(username, siteSlug)}?t=${ts}`}
      alt=""
      className="w-10 h-10 rounded-lg object-cover bg-[var(--bg-primary)] flex-shrink-0"
      onError={() => setErrored(true)}
      loading="lazy"
    />
  )
}

// ══════════════════════════════════════════════════════════════════
// Model Row
// ══════════════════════════════════════════════════════════════════
function ModelRow({ bot, groups, onStart, onStop, onRestart, onRemove }: {
  bot: BotState
  groups: GroupInfo[]
  onStart: () => void
  onStop: () => void
  onRestart: () => void
  onRemove: () => void
}) {
  const groupName = bot.groupName || groups.find(g =>
    g.members?.some(m => m.username === bot.username && (m.site === bot.site || m.site === bot.siteSlug))
  )?.name || ''

  return (
    <div className={clsx(
      'group flex items-center gap-3 px-4 py-3 border-b border-[var(--border)]/50',
      'hover:bg-[var(--bg-hover)] transition-colors',
      bot.recording && 'bg-red-500/5'
    )}>
      {/* Status dot */}
      <div className="w-2.5 h-2.5 rounded-full flex-shrink-0">
        {bot.recording ? (
          <div className="w-2.5 h-2.5 rounded-full bg-red-500 animate-recording" />
        ) : bot.running ? (
          <div className={clsx('w-2.5 h-2.5 rounded-full',
            bot.status === 'Public' ? 'bg-emerald-400' :
            bot.status === 'Online' ? 'bg-cyan-400' :
            bot.status === 'Offline' ? 'bg-yellow-400' :
            bot.consecutiveErrors > 0 ? 'bg-orange-400' :
            'bg-zinc-600'
          )} />
        ) : (
          <div className="w-2.5 h-2.5 rounded-full bg-zinc-700" />
        )}
      </div>

      {/* Preview thumb */}
      <PreviewThumb username={bot.username} siteSlug={bot.siteSlug} />

      {/* Info */}
      <div className="flex-1 min-w-0">
        <div className="flex items-center gap-2">
          <a href={bot.websiteUrl || '#'} target="_blank" rel="noopener noreferrer"
             className="font-medium text-white hover:text-[var(--accent)] transition-colors truncate">
            {bot.username}
          </a>
          {bot.mobile && <span className="text-[10px] px-1.5 py-0.5 rounded bg-blue-500/20 text-blue-400">📱 M</span>}
          {groupName && <span className="text-[10px] px-1.5 py-0.5 rounded bg-yellow-500/15 text-yellow-400">{groupName}</span>}
        </div>
        <div className="text-xs text-[var(--text-secondary)]">{bot.site} [{bot.siteSlug}]</div>
      </div>

      {/* Status */}
      <div className={clsx(
        'hidden sm:block px-2.5 py-1 rounded-full text-xs font-medium border',
        getStatusBg(bot.status), getStatusColor(bot.status)
      )}>
        {bot.recording ? '● REC' : bot.status}
      </div>

      {/* Recording stats */}
      <div className="hidden md:flex items-center gap-4 text-xs text-[var(--text-secondary)] w-40 justify-end">
        {bot.recording && bot.recording_stats ? (
          <>
            <span className="text-red-400 font-medium">{formatBytes(bot.recording_stats.bytesWritten)}</span>
            {bot.recording_stats.currentSpeed > 0 && (
              <span className="text-zinc-500">{bot.recording_stats.currentSpeed.toFixed(1)}x</span>
            )}
          </>
        ) : bot.totalBytes > 0 ? (
          <span>{formatBytes(bot.totalBytes)}</span>
        ) : null}
        {bot.running && bot.uptimeSeconds > 0 && (
          <span className="text-zinc-600">{formatDuration(bot.uptimeSeconds)}</span>
        )}
      </div>

      {/* Actions */}
      <div className="flex items-center gap-1 sm:opacity-0 sm:group-hover:opacity-100 transition-opacity">
        {bot.running ? (
          <button onClick={onStop} title="Stop"
            className="p-2 sm:p-1.5 rounded-lg hover:bg-yellow-500/20 text-yellow-400 transition-colors">
            <Icon d={icons.stop} />
          </button>
        ) : (
          <button onClick={onStart} title="Start"
            className="p-2 sm:p-1.5 rounded-lg hover:bg-emerald-500/20 text-emerald-400 transition-colors">
            <Icon d={icons.play} />
          </button>
        )}
        <button onClick={onRestart} title="Restart"
          className="p-2 sm:p-1.5 rounded-lg hover:bg-blue-500/20 text-blue-400 transition-colors">
          <Icon d={icons.refresh} />
        </button>
        <button onClick={onRemove} title="Remove"
          className="p-2 sm:p-1.5 rounded-lg hover:bg-red-500/20 text-red-400 transition-colors">
          <Icon d={icons.trash} />
        </button>
      </div>
    </div>
  )
}

// ══════════════════════════════════════════════════════════════════
// Groups Tab
// ══════════════════════════════════════════════════════════════════
function GroupsTab({ groups, sites, onRefresh }: {
  groups: GroupInfo[]
  sites: SiteInfo[]
  onRefresh: () => void
}) {
  const [newName, setNewName] = useState('')
  const [memberInputs, setMemberInputs] = useState<Record<string, { user: string; site: string }>>({})

  const handleCreate = async () => {
    if (!newName.trim()) return
    await createGroup(newName.trim())
    setNewName('')
    onRefresh()
  }

  const handleDelete = async (name: string) => {
    if (!confirm(`Delete group "${name}"?`)) return
    await deleteGroup(name)
    onRefresh()
  }

  const handleStart = async (name: string) => { await startGroup(name); onRefresh() }
  const handleStop = async (name: string) => { await stopGroup(name); onRefresh() }

  const handleAddMember = async (groupName: string) => {
    const input = memberInputs[groupName]
    if (!input?.user?.trim() || !input?.site) return
    await addGroupMember(groupName, input.site, input.user.trim())
    setMemberInputs(prev => ({ ...prev, [groupName]: { user: '', site: prev[groupName]?.site || '' } }))
    onRefresh()
  }

  const handleRemoveMember = async (groupName: string, site: string, username: string) => {
    await removeGroupMember(groupName, site, username)
    onRefresh()
  }

  return (
    <div className="p-4 sm:p-6 space-y-4 overflow-y-auto flex-1">
      {/* Create group form */}
      <div className="flex flex-col sm:flex-row gap-3">
        <input type="text" value={newName} onChange={e => setNewName(e.target.value)}
          placeholder="New group name..." onKeyDown={e => e.key === 'Enter' && handleCreate()}
          className="flex-1 sm:max-w-xs px-4 py-2.5 bg-[var(--bg-primary)] border border-[var(--border)] rounded-xl
                     text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)] transition-colors" />
        <button onClick={handleCreate}
          className="px-4 py-2.5 rounded-xl bg-[var(--accent)] text-white font-medium hover:bg-indigo-500 transition-colors">
          Create Group
        </button>
      </div>

      {groups.length === 0 ? (
        <div className="text-center py-16 text-[var(--text-secondary)]">
          <div className="text-4xl mb-3">🔗</div>
          <div>No cross-register groups. Create one above.</div>
        </div>
      ) : groups.map(g => {
        const st = g.state
        const running = st?.running || false
        const recording = st?.recording || false

        let statusLabel = 'Stopped'
        let statusCls = 'text-zinc-500'
        if (recording) {
          statusLabel = `● Recording: ${st?.activeUsername || ''} on ${st?.activeSite || ''}`
          statusCls = 'text-red-400'
        } else if (running && st?.activePairingIdx !== undefined && st.activePairingIdx >= 0) {
          statusLabel = `Checking: ${st?.activeUsername || ''} on ${st?.activeSite || ''}`
          statusCls = 'text-emerald-400'
        } else if (running) {
          statusLabel = 'All Offline — Sleeping'
          statusCls = 'text-yellow-400'
        }

        return (
          <div key={g.name} className="bg-[var(--bg-card)] border border-[var(--border)] rounded-xl overflow-hidden">
            {/* Header */}
            <div className="flex items-center justify-between px-5 py-4 border-b border-[var(--border)] flex-wrap gap-3">
              <div>
                <span className="font-bold text-base">{g.name}</span>
                <span className={clsx('ml-3 text-sm', statusCls)}>{statusLabel}</span>
              </div>
              <div className="flex gap-2">
                {running ? (
                  <button onClick={() => handleStop(g.name)}
                    className="px-3 py-1.5 text-sm rounded-lg bg-yellow-500/10 text-yellow-400 border border-yellow-500/20 hover:bg-yellow-500/20">
                    ⏹ Stop
                  </button>
                ) : (
                  <button onClick={() => handleStart(g.name)}
                    className="px-3 py-1.5 text-sm rounded-lg bg-emerald-500/10 text-emerald-400 border border-emerald-500/20 hover:bg-emerald-500/20">
                    ▶ Start
                  </button>
                )}
                <button onClick={() => handleDelete(g.name)}
                  className="px-3 py-1.5 text-sm rounded-lg bg-red-500/10 text-red-400 border border-red-500/20 hover:bg-red-500/20">
                  🗑
                </button>
              </div>
            </div>

            {/* Members */}
            <div className="px-5 py-2">
              {g.members?.length ? g.members.map((m, mi) => {
                const pairing = st?.pairings?.[mi]
                const isActive = running && st?.activePairingIdx === mi
                let dotCls = 'bg-zinc-600'
                let pStatusText = '—'
                if (pairing) {
                  pStatusText = pairing.status
                  if (recording && isActive) { dotCls = 'bg-red-500 animate-recording'; pStatusText = 'Recording' }
                  else if (pairing.status === 'Public' || pairing.status === 'Online') dotCls = 'bg-emerald-400'
                  else if (pairing.status === 'Error' || pairing.status === 'RateLimit') dotCls = 'bg-orange-400'
                  if (pairing.mobile) pStatusText += ' (M)'
                }

                return (
                  <div key={`${m.site}-${m.username}`} className="flex items-center gap-3 py-2 text-sm">
                    <span className="w-4 text-center text-yellow-400 flex-shrink-0">
                      {mi === 0 ? '★' : ''}
                    </span>
                    <div className={clsx('w-2 h-2 rounded-full flex-shrink-0', dotCls)} />
                    <span className="font-medium">{m.username}</span>
                    <span className="text-[var(--text-secondary)] text-xs">[{m.site}]</span>
                    {isActive && <span className="text-emerald-400 text-xs">◄ active</span>}
                    <span className="ml-auto text-xs text-[var(--text-secondary)]">{pStatusText}</span>
                    <button onClick={() => handleRemoveMember(g.name, m.site, m.username)}
                      className="text-red-400/60 hover:text-red-400 transition-colors text-sm">✕</button>
                  </div>
                )
              }) : (
                <div className="py-3 text-sm text-[var(--text-secondary)]">No members</div>
              )}
            </div>

            {/* Add member */}
            <div className="flex flex-col sm:flex-row gap-2 px-5 py-3 border-t border-[var(--border)] items-stretch sm:items-center">
              <input type="text" placeholder="Username"
                value={memberInputs[g.name]?.user || ''}
                onChange={e => setMemberInputs(prev => ({
                  ...prev, [g.name]: { ...prev[g.name], user: e.target.value, site: prev[g.name]?.site || sites[0]?.name || '' }
                }))}
                onKeyDown={e => e.key === 'Enter' && handleAddMember(g.name)}
                className="flex-1 px-3 py-1.5 text-sm bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg
                           text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]" />
              <select value={memberInputs[g.name]?.site || sites[0]?.name || ''}
                onChange={e => setMemberInputs(prev => ({
                  ...prev, [g.name]: { ...prev[g.name], site: e.target.value, user: prev[g.name]?.user || '' }
                }))}
                className="px-3 py-1.5 text-sm bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg
                           text-white focus:outline-none focus:border-[var(--accent)]">
                {sites.map(s => <option key={s.name} value={s.name}>{s.name} [{s.slug}]</option>)}
              </select>
              <button onClick={() => handleAddMember(g.name)}
                className="px-3 py-1.5 text-sm rounded-lg bg-[var(--accent)] text-white hover:bg-indigo-500">
                + Add
              </button>
            </div>
          </div>
        )
      })}
    </div>
  )
}

// ══════════════════════════════════════════════════════════════════
// Logs Tab
// ══════════════════════════════════════════════════════════════════
function LogsTab() {
  const [lines, setLines] = useState<string[]>([])
  const [filter, setFilter] = useState<'all' | 'info' | 'warn' | 'error'>('all')
  const [search, setSearch] = useState('')
  const [autoScroll, setAutoScroll] = useState(true)
  const bodyRef = useRef<HTMLDivElement>(null)

  const fetchLogs = useCallback(async () => {
    try {
      const data = await getLogs()
      setLines(data)
    } catch { /* ignore */ }
  }, [])

  useEffect(() => {
    fetchLogs()
    const iv = setInterval(fetchLogs, 2000)
    return () => clearInterval(iv)
  }, [fetchLogs])

  useEffect(() => {
    if (autoScroll && bodyRef.current) {
      bodyRef.current.scrollTop = bodyRef.current.scrollHeight
    }
  }, [lines, autoScroll])

  const filtered = lines.filter(l => {
    const lo = l.toLowerCase()
    if (filter === 'warn' && !lo.includes('[warning]') && !lo.includes('[warn]')) return false
    if (filter === 'error' && !lo.includes('[error]') && !lo.includes('[critical]')) return false
    if (filter === 'info' && !lo.includes('[info]')) return false
    if (search && !lo.includes(search.toLowerCase())) return false
    return true
  })

  return (
    <div className="flex flex-col flex-1 overflow-hidden">
      {/* Toolbar */}
      <div className="flex items-center gap-2 px-5 py-3 border-b border-[var(--border)] flex-wrap">
        {(['all', 'info', 'warn', 'error'] as const).map(f => (
          <button key={f} onClick={() => setFilter(f)}
            className={clsx('px-3 py-1 text-xs rounded-lg border transition-colors capitalize',
              filter === f
                ? 'bg-[var(--accent)]/10 border-[var(--accent)]/30 text-[var(--accent)]'
                : 'border-[var(--border)] text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]'
            )}>
            {f === 'warn' ? '⚠ Warn' : f === 'error' ? '❌ Error' : f}
          </button>
        ))}
        <input type="text" value={search} onChange={e => setSearch(e.target.value)}
          placeholder="Filter logs..."
          className="ml-2 flex-1 max-w-xs px-3 py-1 text-sm bg-[var(--bg-primary)] border border-[var(--border)]
                     rounded-lg text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]" />
        <label className="ml-auto flex items-center gap-1.5 text-xs text-[var(--text-secondary)] cursor-pointer select-none">
          <input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)}
            className="accent-[var(--accent)]" />
          Auto-scroll
        </label>
        <button onClick={() => setLines([])}
          className="px-3 py-1 text-xs rounded-lg border border-[var(--border)] text-[var(--text-secondary)]
                     hover:bg-[var(--bg-hover)] transition-colors">
          Clear
        </button>
      </div>

      {/* Log lines */}
      <div ref={bodyRef}
        className="flex-1 overflow-y-auto font-mono text-[11.5px] leading-relaxed px-4 py-2 select-text">
        {filtered.length === 0 ? (
          <div className="text-[var(--text-secondary)] py-8 text-center">No logs</div>
        ) : filtered.map((line, i) => {
          const lo = line.toLowerCase()
          const isWarn = lo.includes('[warning]') || lo.includes('[warn]')
          const isErr = lo.includes('[error]') || lo.includes('[critical]')
          return (
            <div key={i} className={clsx(
              'py-0.5 break-all whitespace-pre-wrap cursor-pointer hover:bg-[var(--bg-hover)] rounded px-1 -mx-1',
              isErr ? 'text-red-400' : isWarn ? 'text-orange-400' : 'text-[var(--text-secondary)]'
            )}
            onClick={() => navigator.clipboard?.writeText(line)}
            title="Click to copy">
              {line}
            </div>
          )
        })}
      </div>
    </div>
  )
}

// ══════════════════════════════════════════════════════════════════
// Settings Tab
// ══════════════════════════════════════════════════════════════════
function SettingsTab() {
  const [config, setConfig] = useState<AppConfig | null>(null)
  const [saved, setSaved] = useState(false)

  useEffect(() => {
    getConfig().then(setConfig).catch(() => {})
  }, [])

  const handleSave = async () => {
    if (!config) return
    await updateConfig({
      downloadsDir: config.downloadsDir,
      container: config.container,
      wantedResolution: config.wantedResolution,
      debug: config.debug,
      minFreeDiskPercent: config.minFreeDiskPercent,
    })
    await saveConfig()
    setSaved(true)
    setTimeout(() => setSaved(false), 2000)
  }

  if (!config) return <div className="flex-1 flex items-center justify-center text-[var(--text-secondary)]">Loading...</div>

  return (
    <div className="p-6 space-y-6 overflow-y-auto flex-1 max-w-2xl">
      <h2 className="text-xl font-bold">Settings</h2>

      {/* Recording */}
      <div className="bg-[var(--bg-card)] border border-[var(--border)] rounded-xl p-5 space-y-4">
        <h3 className="font-semibold text-sm text-[var(--text-secondary)] uppercase tracking-wider">Recording</h3>
        <div>
          <label className="block text-sm mb-1">Downloads Directory</label>
          <input type="text" value={config.downloadsDir}
            onChange={e => setConfig({ ...config, downloadsDir: e.target.value })}
            className="w-full px-3 py-2 bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg text-white
                       focus:outline-none focus:border-[var(--accent)]" />
        </div>
        <div className="flex gap-4">
          <div className="flex-1">
            <label className="block text-sm mb-1">Container</label>
            <select value={config.container}
              onChange={e => setConfig({ ...config, container: e.target.value })}
              className="w-full px-3 py-2 bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg text-white
                         focus:outline-none focus:border-[var(--accent)]">
              <option value="mkv">MKV</option>
              <option value="mp4">MP4</option>
              <option value="ts">TS</option>
            </select>
          </div>
          <div className="flex-1">
            <label className="block text-sm mb-1">Max Resolution</label>
            <input type="number" value={config.wantedResolution}
              onChange={e => setConfig({ ...config, wantedResolution: parseInt(e.target.value) || 99999 })}
              className="w-full px-3 py-2 bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg text-white
                         focus:outline-none focus:border-[var(--accent)]" />
          </div>
        </div>
        <div className="flex gap-4">
          <div className="flex-1">
            <label className="block text-sm mb-1">Min Free Disk %</label>
            <input type="number" step="0.5" value={config.minFreeDiskPercent}
              onChange={e => setConfig({ ...config, minFreeDiskPercent: parseFloat(e.target.value) || 5 })}
              className="w-full px-3 py-2 bg-[var(--bg-primary)] border border-[var(--border)] rounded-lg text-white
                         focus:outline-none focus:border-[var(--accent)]" />
          </div>
          <div className="flex-1 flex items-end pb-1">
            <label className="flex items-center gap-2 text-sm cursor-pointer">
              <input type="checkbox" checked={config.debug}
                onChange={e => setConfig({ ...config, debug: e.target.checked })}
                className="accent-[var(--accent)]" />
              Debug Mode
            </label>
          </div>
        </div>
      </div>

      {/* Web Server info */}
      <div className="bg-[var(--bg-card)] border border-[var(--border)] rounded-xl p-5 space-y-3">
        <h3 className="font-semibold text-sm text-[var(--text-secondary)] uppercase tracking-wider">Web Server</h3>
        <div className="grid grid-cols-2 gap-4 text-sm">
          <div>
            <span className="text-[var(--text-secondary)]">Host:</span>
            <span className="ml-2 font-mono">{config.webHost}</span>
          </div>
          <div>
            <span className="text-[var(--text-secondary)]">Port:</span>
            <span className="ml-2 font-mono">{config.webPort}</span>
          </div>
        </div>
        <div className="text-xs text-[var(--text-secondary)]">
          Set <span className="font-mono">web_host</span> to <span className="font-mono">0.0.0.0</span> in config.json to allow LAN access.
        </div>
      </div>

      {/* Save button */}
      <button onClick={handleSave}
        className={clsx(
          'px-6 py-3 rounded-xl font-medium transition-all',
          saved
            ? 'bg-emerald-500/20 text-emerald-400 border border-emerald-500/30'
            : 'bg-[var(--accent)] text-white hover:bg-indigo-500'
        )}>
        {saved ? '✅ Saved!' : '💾 Save Configuration'}
      </button>
    </div>
  )
}

// ══════════════════════════════════════════════════════════════════
// Add Model Dialog
// ══════════════════════════════════════════════════════════════════
function AddModelDialog({ sites, onAdd, onClose }: {
  sites: SiteInfo[]
  onAdd: (username: string, site: string) => void
  onClose: () => void
}) {
  const [username, setUsername] = useState('')
  const [site, setSite] = useState(sites[0]?.name || '')

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 backdrop-blur-sm" onClick={onClose}>
      <div className="bg-[var(--bg-card)] border border-[var(--border)] rounded-2xl p-6 w-full max-w-md shadow-2xl"
           onClick={e => e.stopPropagation()}>
        <h2 className="text-xl font-bold mb-5">Add Model</h2>
        <div className="space-y-4">
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Username</label>
            <input type="text" value={username} onChange={e => setUsername(e.target.value)}
              placeholder="Enter username..." autoFocus
              onKeyDown={e => { if (e.key === 'Enter' && username.trim()) onAdd(username.trim(), site) }}
              className="w-full px-4 py-2.5 bg-[var(--bg-primary)] border border-[var(--border)] rounded-xl
                         text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]" />
          </div>
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Site</label>
            <select value={site} onChange={e => setSite(e.target.value)}
              className="w-full px-4 py-2.5 bg-[var(--bg-primary)] border border-[var(--border)] rounded-xl
                         text-white focus:outline-none focus:border-[var(--accent)]">
              {sites.map(s => <option key={s.name} value={s.name}>{s.name} [{s.slug}]</option>)}
            </select>
          </div>
        </div>
        <div className="flex gap-3 mt-6">
          <button onClick={onClose}
            className="flex-1 px-4 py-2.5 rounded-xl border border-[var(--border)] text-[var(--text-secondary)]
                       hover:bg-[var(--bg-hover)] transition-colors">
            Cancel
          </button>
          <button onClick={() => { if (username.trim()) onAdd(username.trim(), site) }}
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

// ══════════════════════════════════════════════════════════════════
// Main Dashboard
// ══════════════════════════════════════════════════════════════════
export default function Dashboard() {
  const router = useRouter()
  const [tab, setTab] = useState<TabId>('models')
  const [status, setStatus] = useState<ServerStatus | null>(null)
  const [models, setModels] = useState<BotState[]>([])
  const [sites, setSites] = useState<SiteInfo[]>([])
  const [groups, setGroups] = useState<GroupInfo[]>([])
  const [disk, setDisk] = useState<DiskUsage | null>(null)
  const [filter, setFilter] = useState<'all' | 'recording' | 'online' | 'offline'>('all')
  const [search, setSearch] = useState('')
  const [showAdd, setShowAdd] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [connected, setConnected] = useState(true)
  const [authChecked, setAuthChecked] = useState(false)

  // Auth check
  useEffect(() => {
    const verify = async () => {
      if (!isAuthenticated()) { router.push('/login'); return }
      const res = await checkAuth()
      if (!res.authenticated) { router.push('/login'); return }
      setAuthChecked(true)
    }
    verify()
  }, [router])

  const refresh = useCallback(async () => {
    try {
      const [s, m, d, g] = await Promise.all([getStatus(), getModels(), getDiskUsage(), getGroups()])
      setStatus(s)
      setModels(m)
      setDisk(d)
      setGroups(g)
      setConnected(true)
      setError(null)
    } catch (e: unknown) {
      if (e instanceof Error && e.message === 'AUTH_REQUIRED') { router.push('/login'); return }
      setConnected(false)
    }
  }, [router])

  useEffect(() => {
    if (!authChecked) return
    refresh()
    getSites().then(setSites).catch(() => {})
    const iv = setInterval(refresh, 2000)
    return () => clearInterval(iv)
  }, [refresh, authChecked])

  const handleLogout = async () => { await logout(); router.push('/login') }

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
    .filter(m => !search ||
      m.username.toLowerCase().includes(search.toLowerCase()) ||
      m.site.toLowerCase().includes(search.toLowerCase()) ||
      m.siteSlug.toLowerCase().includes(search.toLowerCase()))
    .sort((a, b) => {
      if (a.recording !== b.recording) return a.recording ? -1 : 1
      if ((a.status === 'Public') !== (b.status === 'Public')) return a.status === 'Public' ? -1 : 1
      return a.username.localeCompare(b.username)
    })

  const handleAction = async (action: () => Promise<unknown>) => {
    try { await action(); setTimeout(refresh, 300) }
    catch (e: unknown) { setError(e instanceof Error ? e.message : 'Error') }
  }

  const handleAdd = async (username: string, site: string) => {
    try { await addModel(username, site); setShowAdd(false); refresh() }
    catch (e: unknown) { setError(e instanceof Error ? e.message : 'Error') }
  }

  const recCount = models.filter(m => m.recording).length
  const onCount = models.filter(m => m.status === 'Public' || m.status === 'Online').length

  const tabDefs: { id: TabId; label: string; icon: string; badge?: number }[] = [
    { id: 'models', label: 'Models', icon: icons.globe, badge: models.length },
    { id: 'groups', label: 'Groups', icon: icons.link, badge: groups.length },
    { id: 'logs', label: 'Logs', icon: icons.log },
    { id: 'settings', label: 'Settings', icon: icons.cog },
  ]

  return (
    <div className="h-screen flex flex-col bg-[var(--bg-primary)]">
      {/* Header */}
      <header className="flex items-center gap-2 sm:gap-4 px-3 sm:px-5 py-2 sm:py-3 bg-[var(--bg-secondary)] border-b border-[var(--border)] flex-shrink-0 flex-wrap">
        <div className="flex items-center gap-2 mr-1 sm:mr-2">
          <span className="text-lg sm:text-xl">🎬</span>
          <span className="font-bold text-base sm:text-lg tracking-tight hidden sm:inline">
            <span className="text-[var(--accent)]">Strea</span>Monitor
          </span>
          <div className={clsx('w-1.5 h-1.5 rounded-full ml-1', connected ? 'bg-emerald-400' : 'bg-red-400')} />
        </div>

        {/* Tabs */}
        <nav className="flex gap-0.5 ml-1 sm:ml-2 overflow-x-auto">
          {tabDefs.map(t => (
            <button key={t.id} onClick={() => setTab(t.id)}
              className={clsx(
                'flex items-center gap-1 sm:gap-1.5 px-2.5 sm:px-4 py-2 text-xs sm:text-sm font-medium rounded-lg transition-colors whitespace-nowrap',
                tab === t.id
                  ? 'bg-[var(--accent)]/10 text-[var(--accent)]'
                  : 'text-[var(--text-secondary)] hover:text-white hover:bg-[var(--bg-hover)]'
              )}>
              <Icon d={t.icon} />
              <span className="hidden sm:inline">{t.label}</span>
              {t.badge !== undefined && t.badge > 0 && (
                <span className="text-[10px] px-1.5 py-0.5 rounded-full bg-[var(--border)] text-[var(--text-secondary)]">
                  {t.badge}
                </span>
              )}
            </button>
          ))}
        </nav>

        {/* Right side */}
        <div className="ml-auto flex items-center gap-1.5 sm:gap-2">
          {/* Stats */}
          <div className="hidden lg:flex items-center gap-3 text-xs mr-3">
            <span className="text-[var(--text-secondary)]">
              <span className="text-emerald-400 font-bold">{onCount}</span> online
            </span>
            {recCount > 0 && (
              <span className="text-red-400 font-bold animate-recording">● {recCount} rec</span>
            )}
            {disk && (
              <span className="text-[var(--text-secondary)]">
                {formatBytes(disk.freeBytes)} free
              </span>
            )}
          </div>

          <button onClick={() => handleAction(startAll)}
            className="px-2.5 py-1.5 text-xs rounded-lg bg-emerald-500/10 text-emerald-400 border border-emerald-500/20
                       hover:bg-emerald-500/20 transition-colors">
            ▶ All
          </button>
          <button onClick={() => handleAction(stopAll)}
            className="px-2.5 py-1.5 text-xs rounded-lg bg-yellow-500/10 text-yellow-400 border border-yellow-500/20
                       hover:bg-yellow-500/20 transition-colors">
            ⏹ All
          </button>
          <button onClick={() => handleAction(saveConfig)}
            className="p-1.5 rounded-lg bg-blue-500/10 text-blue-400 border border-blue-500/20
                       hover:bg-blue-500/20 transition-colors" title="Save config">
            <Icon d={icons.save} />
          </button>
          <button onClick={handleLogout}
            className="p-1.5 rounded-lg text-zinc-500 hover:text-white hover:bg-[var(--bg-hover)] transition-colors"
            title="Logout">
            <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
              <path strokeLinecap="round" strokeLinejoin="round"
                d="M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1" />
            </svg>
          </button>
        </div>
      </header>

      {/* Error banner */}
      {error && (
        <div className="bg-red-500/10 border-b border-red-500/20 px-5 py-2 flex items-center justify-between flex-shrink-0">
          <span className="text-sm text-red-400">{error}</span>
          <button onClick={() => setError(null)} className="text-red-400 hover:text-red-300 text-sm">✕</button>
        </div>
      )}

      {/* Content */}
      <div className="flex-1 flex flex-col overflow-hidden">
        {/* Models tab */}
        {tab === 'models' && (
          <>
            {/* Toolbar */}
            <div className="flex items-center gap-2 px-5 py-3 border-b border-[var(--border)] flex-shrink-0 flex-wrap">
              {(['all', 'recording', 'online', 'offline'] as const).map(f => (
                <button key={f} onClick={() => setFilter(f)}
                  className={clsx(
                    'px-3 py-1 text-xs rounded-lg border transition-colors capitalize',
                    filter === f
                      ? 'bg-[var(--accent)]/10 border-[var(--accent)]/30 text-[var(--accent)]'
                      : 'border-[var(--border)] text-[var(--text-secondary)] hover:bg-[var(--bg-hover)]'
                  )}>
                  {f}
                  {f === 'recording' && recCount > 0 && <span className="ml-1">{recCount}</span>}
                </button>
              ))}
              <input type="text" value={search} onChange={e => setSearch(e.target.value)}
                placeholder="Search..."
                className="ml-2 flex-1 min-w-0 max-w-xs px-3 py-1 text-sm bg-[var(--bg-primary)] border border-[var(--border)]
                           rounded-lg text-white placeholder-zinc-600 focus:outline-none focus:border-[var(--accent)]" />
              <button onClick={() => setShowAdd(true)}
                className="ml-auto flex items-center gap-1 px-3 py-1.5 text-sm rounded-lg bg-[var(--accent)]
                           text-white font-medium hover:bg-indigo-500 transition-colors whitespace-nowrap">
                <Icon d={icons.plus} /> Add
              </button>
            </div>

            {/* Model list */}
            <div className="flex-1 overflow-y-auto">
              {filteredModels.length === 0 ? (
                <div className="text-center py-16 text-[var(--text-secondary)]">
                  {models.length === 0 ? (
                    <>
                      <div className="text-4xl mb-3">🎬</div>
                      <div className="font-medium">No models added yet</div>
                      <div className="text-sm mt-1">Click &quot;Add&quot; to start monitoring</div>
                    </>
                  ) : (
                    <div>No models match the current filter</div>
                  )}
                </div>
              ) : filteredModels.map(bot => (
                <ModelRow
                  key={`${bot.username}_${bot.siteSlug}`}
                  bot={bot}
                  groups={groups}
                  onStart={() => handleAction(() => startModel(bot.username, bot.siteSlug))}
                  onStop={() => handleAction(() => stopModel(bot.username, bot.siteSlug))}
                  onRestart={() => handleAction(() => restartModel(bot.username, bot.siteSlug))}
                  onRemove={() => {
                    if (confirm(`Remove ${bot.username} [${bot.siteSlug}]?`))
                      handleAction(() => removeModel(bot.username, bot.siteSlug))
                  }}
                />
              ))}
            </div>
          </>
        )}

        {/* Groups tab */}
        {tab === 'groups' && <GroupsTab groups={groups} sites={sites} onRefresh={refresh} />}

        {/* Logs tab */}
        {tab === 'logs' && <LogsTab />}

        {/* Settings tab */}
        {tab === 'settings' && <SettingsTab />}
      </div>

      {/* Status bar */}
      <div className="flex items-center gap-4 px-5 py-2 bg-[var(--bg-secondary)] border-t border-[var(--border)] text-xs
                      text-[var(--text-secondary)] flex-shrink-0">
        <span>StreaMonitor v{status?.version || '2.0.0'}</span>
        <span className="ml-auto">{models.length} models</span>
        {recCount > 0 && <span className="text-red-400">● {recCount} recording</span>}
      </div>

      {/* Add model dialog */}
      {showAdd && <AddModelDialog sites={sites} onAdd={handleAdd} onClose={() => setShowAdd(false)} />}
    </div>
  )
}
