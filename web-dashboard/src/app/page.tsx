'use client'

import { useState, useEffect, useCallback, useRef } from 'react'
import { useRouter } from 'next/navigation'
import {
  getStatus, getModels, getSites, getDiskUsage, getGroups, getLogs, getConfig,
  addModel, removeModel, startModel, stopModel, restartModel,
  startAll, stopAll, saveConfig, updateConfig, checkAuth, logout, isAuthenticated,
  createGroup, deleteGroup, startGroup, stopGroup, addGroupMember, removeGroupMember,
  updateCredentials,
  formatBytes, formatDuration, getStatusColor, getPreviewUrl, getStreamUrl,
  type BotState, type ServerStatus, type SiteInfo, type DiskUsage, type GroupInfo, type AppConfig,
} from '@/lib/api'

// Inline SVG Icons
const Ico = ({ d, cls = '' }: { d: string; cls?: string }) => (
  <svg className={`w-4 h-4 ${cls}`} fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={2}>
    <path strokeLinecap="round" strokeLinejoin="round" d={d} />
  </svg>
)

const ic = {
  play: 'M14.752 11.168l-3.197-2.132A1 1 0 0010 9.87v4.263a1 1 0 001.555.832l3.197-2.132a1 1 0 000-1.664z',
  stop: 'M21 12a9 9 0 11-18 0 9 9 0 0118 0z M10 9v6 M14 9v6',
  refresh: 'M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15',
  trash: 'M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16',
  plus: 'M12 4v16m8-8H4',
  save: 'M8 7H5a2 2 0 00-2 2v9a2 2 0 002 2h14a2 2 0 002-2V9a2 2 0 00-2-2h-3m-1 4l-3 3m0 0l-3-3m3 3V4',
  globe: 'M21 12a9 9 0 01-9 9m9-9a9 9 0 00-9-9m9 9H3m9 9a9 9 0 01-9-9m9 9c1.657 0 3-4.03 3-9s-1.343-9-3-9m0 18c-1.657 0-3-4.03-3-9s1.343-9 3-9m-9 9a9 9 0 019-9',
  link: 'M13.828 10.172a4 4 0 00-5.656 0l-4 4a4 4 0 105.656 5.656l1.102-1.101m-.758-4.899a4 4 0 005.656 0l4-4a4 4 0 00-5.656-5.656l-1.1 1.1',
  log: 'M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2',
  cog: 'M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.066 2.573c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.573 1.066c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.066-2.573c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z M15 12a3 3 0 11-6 0 3 3 0 016 0z',
  x: 'M6 18L18 6M6 6l12 12',
  ext: 'M10 6H6a2 2 0 00-2 2v10a2 2 0 002 2h10a2 2 0 002-2v-4M14 4h6m0 0v6m0-6L10 14',
  eye: 'M15 12a3 3 0 11-6 0 3 3 0 016 0z M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z',
  rec: 'M15 10l4.553-2.276A1 1 0 0121 8.618v6.764a1 1 0 01-1.447.894L15 14M5 18h8a2 2 0 002-2V8a2 2 0 00-2-2H5a2 2 0 00-2 2v8a2 2 0 002 2z',
  logout: 'M17 16l4-4m0 0l-4-4m4 4H7m6 4v1a3 3 0 01-3 3H6a3 3 0 01-3-3V7a3 3 0 013-3h4a3 3 0 013 3v1',
}

type TabId = 'models' | 'groups' | 'logs' | 'settings'

function statusDotClass(bot: BotState): string {
  if (bot.recording) return 'recording'
  if (!bot.running) return 'stopped'
  if (bot.status === 'Public' || bot.status === 'Online') return 'online'
  if (bot.status === 'Offline' || bot.status === 'Long Offline') return 'offline'
  if (bot.consecutiveErrors > 0) return 'error'
  return 'stopped'
}

function statusBadge(bot: BotState): { label: string; color: string } {
  if (bot.recording) return { label: '\u25cf REC', color: 'bg-red-500/15 text-red-400 border-red-500/25' }
  switch (bot.status) {
    case 'Public': return { label: 'Public', color: 'bg-emerald-500/15 text-emerald-400 border-emerald-500/25' }
    case 'Online': return { label: 'Online', color: 'bg-cyan-500/15 text-cyan-400 border-cyan-500/25' }
    case 'Private': return { label: 'Private', color: 'bg-purple-500/15 text-purple-400 border-purple-500/25' }
    case 'Offline': return { label: 'Offline', color: 'bg-yellow-500/15 text-yellow-400 border-yellow-500/25' }
    case 'Long Offline': return { label: 'Offline', color: 'bg-zinc-500/15 text-zinc-500 border-zinc-500/25' }
    case 'Error': return { label: 'Error', color: 'bg-red-500/15 text-red-400 border-red-500/25' }
    default: return { label: bot.running ? bot.status || 'Unknown' : 'Stopped', color: 'bg-zinc-500/10 text-zinc-500 border-zinc-500/20' }
  }
}

// Preview Thumbnail — HTTP/2 multiplexed previews (unlimited concurrent streams!)
// Recording: uses /api/stream/:user/:site (MJPEG) — HTTP/2 handles multiplexing
// Not recording: uses /api/preview/:user/:site (JPEG snapshot) or native CDN thumbnail
//
// 🚀 HTTP/2 multiplexes ALL streams over a single TCP connection.
// No WebSocket needed. No 6-connection limit. Just native h2 magic.
function PreviewThumb({ username, siteSlug, large, isRecording, nativePreviewUrl }: {
  username: string; siteSlug: string; large?: boolean; isRecording?: boolean; nativePreviewUrl?: string
}) {
  const [errored, setErrored] = useState(false)
  const [retryKey, setRetryKey] = useState(0)
  const [useNativeFallback, setUseNativeFallback] = useState(false)
  const imgRef = useRef<HTMLImageElement>(null)

  // For recording models: use MJPEG stream (multiplexed over HTTP/2)
  // For non-recording: use snapshot or native CDN thumbnail
  const src = isRecording
    ? `${getStreamUrl(username, siteSlug)}?t=${retryKey}`
    : useNativeFallback && nativePreviewUrl
      ? nativePreviewUrl
      : `${getPreviewUrl(username, siteSlug)}?t=${retryKey}`

  // Retry errored state after delay
  useEffect(() => {
    if (!errored) return
    const t = setTimeout(() => { setErrored(false); setRetryKey(k => k + 1) }, 8000)
    return () => clearTimeout(t)
  }, [errored])

  // Reset fallback & force new stream when recording status changes
  useEffect(() => {
    setUseNativeFallback(false)
    setErrored(false)
    setRetryKey(k => k + 1)
  }, [isRecording])

  const handleError = () => {
    if (!isRecording && !useNativeFallback && nativePreviewUrl) {
      setUseNativeFallback(true)
    } else {
      setErrored(true)
    }
  }

  if (errored) {
    return (
      <div className={large ? 'preview-large flex items-center justify-center text-zinc-700' : 'preview-card-img flex items-center justify-center text-zinc-700 text-xs'}>
        <svg className={large ? 'w-12 h-12 opacity-20' : 'w-10 h-10 opacity-20'} fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={1.5}>
          <path strokeLinecap="round" strokeLinejoin="round" d="M2.25 15.75l5.159-5.159a2.25 2.25 0 013.182 0l5.159 5.159m-1.5-1.5l1.409-1.41a2.25 2.25 0 013.182 0l2.909 2.91m-18 3.75h16.5a1.5 1.5 0 001.5-1.5V6a1.5 1.5 0 00-1.5-1.5H3.75A1.5 1.5 0 002.25 6v12a1.5 1.5 0 001.5 1.5zm10.5-11.25h.008v.008h-.008V8.25zm.375 0a.375.375 0 11-.75 0 .375.375 0 01.75 0z" />
        </svg>
      </div>
    )
  }

  return (
    // eslint-disable-next-line @next/next/no-img-element
    <img
      ref={imgRef}
      key={`${retryKey}-${isRecording}-${useNativeFallback}`}
      src={src}
      alt=""
      className={large ? 'preview-large' : 'preview-card-img'}
      onError={handleError}
      loading="lazy"
    />
  )
}

// Live Stream Viewer — MJPEG video player (true live video, only used in overlay)
function LiveStreamViewer({ username, siteSlug, onClose }: {
  username: string; siteSlug: string; onClose: () => void
}) {
  const [errored, setErrored] = useState(false)
  const [loading, setLoading] = useState(true)
  const [retryKey, setRetryKey] = useState(0)
  const [timedOut, setTimedOut] = useState(false)
  const loadedRef = useRef(false)

  // Auto-retry on error after delay
  useEffect(() => {
    if (!errored) return
    const t = setTimeout(() => { setErrored(false); setTimedOut(false); setLoading(true); loadedRef.current = false; setRetryKey(k => k + 1) }, 3000)
    return () => clearTimeout(t)
  }, [errored])

  // Connection timeout — if no frame loads within 10s, show error
  useEffect(() => {
    if (!loading) return
    loadedRef.current = false
    const t = setTimeout(() => {
      if (!loadedRef.current) {
        setTimedOut(true)
        setLoading(false)
        setErrored(true)
      }
    }, 10000)
    return () => clearTimeout(t)
  }, [loading, retryKey])

  const streamUrl = `${getStreamUrl(username, siteSlug)}?t=${retryKey}`

  return (
    <div className="fixed inset-0 z-[60] flex items-center justify-center animate-fade-in" onClick={onClose}>
      <div className="absolute inset-0 bg-black/90" />
      <div className="relative w-full max-w-4xl mx-4" onClick={e => e.stopPropagation()}>
        <button onClick={onClose}
          className="absolute -top-10 right-0 z-10 p-2 text-zinc-400 hover:text-white transition-colors">
          <Ico d={ic.x} cls="w-6 h-6" />
        </button>
        <div className="relative bg-black rounded-xl overflow-hidden shadow-2xl border border-[var(--border)]">
          {!errored ? (
            <>
              {loading && (
                <div className="absolute inset-0 flex items-center justify-center z-10">
                  <div className="text-center">
                    <div className="w-8 h-8 border-2 border-zinc-600 border-t-white rounded-full animate-spin mx-auto mb-3" />
                    <p className="text-sm text-zinc-500">Connecting to stream...</p>
                  </div>
                </div>
              )}
              {/* eslint-disable-next-line @next/next/no-img-element */}
              <img
                key={retryKey}
                src={streamUrl}
                alt="Live stream"
                className="w-full aspect-video object-contain bg-black"
                onLoad={() => { loadedRef.current = true; setLoading(false) }}
                onError={() => { setLoading(false); setErrored(true) }}
              />
            </>
          ) : (
            <div className="w-full aspect-video flex items-center justify-center text-zinc-600">
              <div className="text-center">
                <Ico d={ic.rec} cls="w-12 h-12 mx-auto mb-3 opacity-30" />
                <p className="text-sm">{timedOut ? 'Connection timed out' : 'Stream not available'}</p>
                <p className="text-xs text-zinc-700 mt-1">Retrying...</p>
              </div>
            </div>
          )}
          <div className="absolute top-3 left-3 flex items-center gap-2">
            <span className="flex items-center gap-1.5 bg-red-600/90 text-white text-xs font-bold px-2.5 py-1 rounded-md">
              <span className="w-2 h-2 bg-white rounded-full animate-recording" /> LIVE
            </span>
          </div>
          <div className="absolute bottom-3 right-3 text-xs text-zinc-500 bg-black/60 px-2 py-1 rounded">
            {username} [{siteSlug}]
          </div>
        </div>
      </div>
    </div>
  )
}

// Model Detail Modal
function ModelDetailModal({ bot, onClose, onAction }: {
  bot: BotState
  onClose: () => void
  onAction: (action: () => Promise<unknown>) => void
}) {
  const badge = statusBadge(bot)
  const [showStream, setShowStream] = useState(false)

  return (
    <>
      <div className="fixed inset-0 z-50 flex items-end sm:items-center justify-center detail-overlay"
           onClick={onClose}>
        <div className="absolute inset-0 bg-black/70 backdrop-blur-sm" />
        <div className="relative w-full max-w-lg mx-auto sm:mx-4 bg-[var(--bg-card)] border border-[var(--border)] rounded-t-2xl sm:rounded-2xl shadow-2xl detail-panel max-h-[90vh] overflow-y-auto"
             onClick={e => e.stopPropagation()}>

          <button onClick={onClose} className="absolute top-4 right-4 z-10 p-2 rounded-xl bg-black/40 text-zinc-400 hover:text-white transition-colors">
            <Ico d={ic.x} />
          </button>

          <div className="p-4 pb-0">
            <PreviewThumb username={bot.username} siteSlug={bot.siteSlug} large isRecording={bot.recording} nativePreviewUrl={bot.previewUrl} />
          </div>

          <div className="p-5 space-y-4">
            <div className="flex items-start justify-between gap-3">
              <div>
                <h2 className="text-xl font-bold">{bot.username}</h2>
                <p className="text-sm text-[var(--text-secondary)] mt-0.5">{bot.site} [{bot.siteSlug}]</p>
              </div>
              <span className={`badge-status ${badge.color} flex-shrink-0`}>{badge.label}</span>
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Status</div>
                <div className="flex items-center gap-2">
                  <div className={`status-dot ${statusDotClass(bot)}`} />
                  <span className="text-sm font-medium">{bot.recording ? 'Recording' : bot.status || (bot.running ? 'Running' : 'Stopped')}</span>
                </div>
              </div>
              <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Uptime</div>
                <span className="text-sm font-medium">{bot.running && bot.uptimeSeconds > 0 ? formatDuration(bot.uptimeSeconds) : '\u2014'}</span>
              </div>
              {bot.recording && bot.recording_stats && (
                <>
                  <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                    <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Recorded</div>
                    <span className="text-sm font-medium text-red-400">{formatBytes(bot.recording_stats.bytesWritten)}</span>
                  </div>
                  <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                    <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Speed</div>
                    <span className="text-sm font-medium">{bot.recording_stats.currentSpeed > 0 ? `${bot.recording_stats.currentSpeed.toFixed(1)}x` : '\u2014'}</span>
                  </div>
                </>
              )}
              {bot.totalBytes > 0 && !bot.recording && (
                <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                  <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Total Size</div>
                  <span className="text-sm font-medium">{formatBytes(bot.totalBytes)}</span>
                </div>
              )}
            </div>

            {bot.recording && bot.recording_stats?.currentFile && (
              <div className="bg-[var(--bg-primary)] rounded-xl p-3 border border-[var(--border-subtle)]">
                <div className="text-[10px] uppercase tracking-wider text-[var(--text-dim)] mb-1">Current File</div>
                <p className="text-xs text-[var(--text-secondary)] font-mono truncate">{bot.recording_stats.currentFile}</p>
              </div>
            )}

            <div className="flex gap-2 flex-wrap">
              {bot.recording && (
                <button onClick={() => setShowStream(true)}
                   className="btn btn-primary flex-1">
                  <Ico d={ic.rec} /> Watch Stream
                </button>
              )}
              {bot.websiteUrl && (
                <a href={bot.websiteUrl} target="_blank" rel="noopener noreferrer"
                   className="btn btn-ghost flex-1">
                  <Ico d={ic.ext} /> Open Site
                </a>
              )}
            </div>
            <div className="flex gap-2">
              {bot.running ? (
                <button onClick={() => onAction(() => stopModel(bot.username, bot.siteSlug))}
                  className="btn btn-warning flex-1">
                  <Ico d={ic.stop} /> Stop
                </button>
              ) : (
                <button onClick={() => onAction(() => startModel(bot.username, bot.siteSlug))}
                  className="btn btn-success flex-1">
                  <Ico d={ic.play} /> Start
                </button>
              )}
              <button onClick={() => onAction(() => restartModel(bot.username, bot.siteSlug))}
                className="btn btn-ghost flex-1 text-sm">
                <Ico d={ic.refresh} /> Restart
              </button>
              <button onClick={() => {
                if (confirm(`Remove ${bot.username} [${bot.siteSlug}]?`))
                  onAction(() => removeModel(bot.username, bot.siteSlug))
              }} className="btn btn-danger flex-1 text-sm">
                <Ico d={ic.trash} /> Remove
              </button>
            </div>
          </div>
        </div>
      </div>

      {showStream && (
        <LiveStreamViewer
          username={bot.username}
          siteSlug={bot.siteSlug}
          onClose={() => setShowStream(false)}
        />
      )}
    </>
  )
}

// Model Card — Stripchat-style grid card with preview thumbnail
function ModelCard({ bot, groups, selected, onClick, onStart, onStop }: {
  bot: BotState
  groups: GroupInfo[]
  selected: boolean
  onClick: (e: React.MouseEvent) => void
  onStart: () => void
  onStop: () => void
}) {
  const groupName = bot.groupName || groups.find(g =>
    g.members?.some(m => m.username === bot.username && (m.site === bot.site || m.site === bot.siteSlug))
  )?.name || ''

  const badge = statusBadge(bot)
  const isLive = bot.status === 'Public' || bot.status === 'Online'
  const cardCls = [
    'grid-card',
    bot.recording ? 'recording' : isLive ? 'online' : '',
    selected ? 'selected' : '',
  ].filter(Boolean).join(' ')

  return (
    <div className={cardCls} onClick={onClick}>
      {/* Preview Image — HTTP/2 multiplexed MJPEG for recording, snapshot otherwise */}
      <div className="grid-card-preview">
        <PreviewThumb username={bot.username} siteSlug={bot.siteSlug} isRecording={bot.recording} nativePreviewUrl={bot.previewUrl} />

        {/* Status overlay top-left */}
        {bot.recording && (
          <div className="absolute top-2 left-2 flex items-center gap-1 bg-red-600/90 text-white text-[10px] font-bold px-1.5 py-0.5 rounded">
            <span className="w-1.5 h-1.5 bg-white rounded-full animate-recording" /> REC
          </div>
        )}
        {!bot.recording && isLive && (
          <div className="absolute top-2 left-2 flex items-center gap-1 bg-emerald-600/90 text-white text-[10px] font-bold px-1.5 py-0.5 rounded">
            <span className="w-1.5 h-1.5 bg-white rounded-full" /> LIVE
          </div>
        )}

        {/* Quick action overlay top-right */}
        <div className="grid-card-actions" onClick={e => e.stopPropagation()}>
          {bot.running ? (
            <button onClick={onStop} title="Stop" className="grid-card-action-btn">
              <Ico d={ic.stop} />
            </button>
          ) : (
            <button onClick={onStart} title="Start" className="grid-card-action-btn text-emerald-400">
              <Ico d={ic.play} />
            </button>
          )}
        </div>

        {/* Recording stats overlay bottom-right */}
        {bot.recording && bot.recording_stats && (
          <div className="absolute bottom-8 right-2 text-[10px] text-white/80 bg-black/60 px-1.5 py-0.5 rounded font-mono">
            {formatBytes(bot.recording_stats.bytesWritten)}
          </div>
        )}

        {/* Selection indicator */}
        {selected && (
          <div className="absolute top-2 right-2 w-5 h-5 bg-[var(--accent)] rounded-full flex items-center justify-center z-10">
            <svg className="w-3 h-3 text-white" fill="none" viewBox="0 0 24 24" stroke="currentColor" strokeWidth={3}>
              <path strokeLinecap="round" strokeLinejoin="round" d="M5 13l4 4L19 7" />
            </svg>
          </div>
        )}
      </div>

      {/* Info bar at bottom — status text + username + site */}
      <div className="grid-card-info">
        <div className="flex items-center gap-1.5 min-w-0 flex-1">
          <span className={`badge-status ${badge.color} text-[10px] py-0 px-1.5`}>{badge.label}</span>
          <span className="font-semibold text-sm truncate">{bot.username}</span>
        </div>
        <div className="flex items-center gap-1.5 flex-shrink-0">
          {groupName && <span className="text-[10px] bg-yellow-500/10 text-yellow-500 px-1.5 py-0.5 rounded truncate max-w-[60px]">{groupName}</span>}
          <span className="text-[10px] text-[var(--text-dim)]">{bot.siteSlug}</span>
        </div>
      </div>
    </div>
  )
}

// Groups Tab
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
      <div className="flex flex-col sm:flex-row gap-3">
        <input type="text" value={newName} onChange={e => setNewName(e.target.value)}
          placeholder="New group name..." onKeyDown={e => e.key === 'Enter' && handleCreate()}
          className="input-field flex-1 sm:max-w-xs" />
        <button onClick={handleCreate} className="btn btn-primary">Create Group</button>
      </div>

      {groups.length === 0 ? (
        <div className="text-center py-20 text-[var(--text-secondary)]">
          <div className="text-4xl mb-3 opacity-30">\ud83d\udd17</div>
          <div className="font-medium">No cross-register groups</div>
          <div className="text-sm mt-1 text-[var(--text-dim)]">Create one above to link models across sites</div>
        </div>
      ) : groups.map(g => {
        const st = g.state
        const running = st?.running || false
        const recording = st?.recording || false

        let statusLabel = 'Stopped'
        let statusCls = 'text-zinc-500'
        if (recording) {
          statusLabel = `\u25cf Recording: ${st?.activeUsername || ''} on ${st?.activeSite || ''}`
          statusCls = 'text-red-400'
        } else if (running && st?.activePairingIdx !== undefined && st.activePairingIdx >= 0) {
          statusLabel = `Checking: ${st?.activeUsername || ''} on ${st?.activeSite || ''}`
          statusCls = 'text-emerald-400'
        } else if (running) {
          statusLabel = 'All Offline \u2014 Sleeping'
          statusCls = 'text-yellow-400'
        }

        return (
          <div key={g.name} className="bg-[var(--bg-card)] border border-[var(--border-subtle)] rounded-xl overflow-hidden">
            <div className="flex items-center justify-between px-5 py-4 border-b border-[var(--border-subtle)] flex-wrap gap-3">
              <div>
                <span className="font-bold text-base">{g.name}</span>
                <span className={`ml-3 text-sm ${statusCls}`}>{statusLabel}</span>
              </div>
              <div className="flex gap-2">
                {running ? (
                  <button onClick={() => handleStop(g.name)} className="btn btn-sm btn-warning">\u23f9 Stop</button>
                ) : (
                  <button onClick={() => handleStart(g.name)} className="btn btn-sm btn-success">\u25b6 Start</button>
                )}
                <button onClick={() => handleDelete(g.name)} className="btn btn-sm btn-danger">\ud83d\uddd1</button>
              </div>
            </div>

            <div className="px-5 py-2">
              {g.members?.length ? g.members.map((m, mi) => {
                const pairing = st?.pairings?.[mi]
                const isActive = running && st?.activePairingIdx === mi
                let dotCls = 'stopped'
                let pStatusText = '\u2014'
                if (pairing) {
                  pStatusText = pairing.status
                  if (recording && isActive) { dotCls = 'recording'; pStatusText = 'Recording' }
                  else if (pairing.status === 'Public' || pairing.status === 'Online') dotCls = 'online'
                  else if (pairing.status === 'Error' || pairing.status === 'RateLimit') dotCls = 'error'
                  if (pairing.mobile) pStatusText += ' (M)'
                }

                return (
                  <div key={`${m.site}-${m.username}`} className="flex items-center gap-3 py-2.5 text-sm border-b border-[var(--border-subtle)] last:border-0">
                    <span className="w-4 text-center text-yellow-400 flex-shrink-0 text-xs">
                      {mi === 0 ? '\u2605' : `${mi + 1}`}
                    </span>
                    <div className={`status-dot ${dotCls}`} />
                    <span className="font-medium">{m.username}</span>
                    <span className="text-[var(--text-dim)] text-xs">[{m.site}]</span>
                    {isActive && <span className="text-emerald-400 text-xs font-medium">\u25c4 active</span>}
                    <span className="ml-auto text-xs text-[var(--text-secondary)]">{pStatusText}</span>
                    <button onClick={() => handleRemoveMember(g.name, m.site, m.username)}
                      className="text-red-400/40 hover:text-red-400 transition-colors text-sm p-1">\u2715</button>
                  </div>
                )
              }) : (
                <div className="py-3 text-sm text-[var(--text-dim)]">No members</div>
              )}
            </div>

            <div className="flex flex-col sm:flex-row gap-2 px-5 py-3 border-t border-[var(--border-subtle)] items-stretch sm:items-center">
              <input type="text" placeholder="Username"
                value={memberInputs[g.name]?.user || ''}
                onChange={e => setMemberInputs(prev => ({
                  ...prev, [g.name]: { ...prev[g.name], user: e.target.value, site: prev[g.name]?.site || sites[0]?.name || '' }
                }))}
                onKeyDown={e => e.key === 'Enter' && handleAddMember(g.name)}
                className="input-field flex-1 text-sm" />
              <select value={memberInputs[g.name]?.site || sites[0]?.name || ''}
                onChange={e => setMemberInputs(prev => ({
                  ...prev, [g.name]: { ...prev[g.name], site: e.target.value, user: prev[g.name]?.user || '' }
                }))}
                className="input-field text-sm sm:w-auto">
                {sites.map(s => <option key={s.name} value={s.name}>{s.name}</option>)}
              </select>
              <button onClick={() => handleAddMember(g.name)} className="btn btn-sm btn-primary">+ Add</button>
            </div>
          </div>
        )
      })}
    </div>
  )
}

// Logs Tab
function LogsTab() {
  const [lines, setLines] = useState<string[]>([])
  const [filter, setFilter] = useState<'all' | 'info' | 'warn' | 'error'>('all')
  const [search, setSearch] = useState('')
  const [autoScroll, setAutoScroll] = useState(true)
  const bodyRef = useRef<HTMLDivElement>(null)

  const fetchLogs = useCallback(async () => {
    try { setLines(await getLogs()) } catch { /* ignore */ }
  }, [])

  useEffect(() => {
    fetchLogs()
    const iv = setInterval(fetchLogs, 2000)
    return () => clearInterval(iv)
  }, [fetchLogs])

  useEffect(() => {
    if (autoScroll && bodyRef.current) bodyRef.current.scrollTop = bodyRef.current.scrollHeight
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
      <div className="flex items-center gap-2 px-5 py-3 border-b border-[var(--border)] flex-wrap">
        {(['all', 'info', 'warn', 'error'] as const).map(f => (
          <button key={f} onClick={() => setFilter(f)}
            className={`filter-pill ${filter === f ? 'active' : ''}`}>
            {f === 'warn' ? '\u26a0 Warn' : f === 'error' ? '\u274c Error' : f === 'info' ? '\u2139 Info' : 'All'}
          </button>
        ))}
        <input type="text" value={search} onChange={e => setSearch(e.target.value)}
          placeholder="Filter logs..." className="input-field ml-2 flex-1 max-w-xs text-sm" />
        <label className="ml-auto flex items-center gap-1.5 text-xs text-[var(--text-secondary)] cursor-pointer select-none">
          <input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)}
            className="accent-[var(--accent)]" />
          Auto-scroll
        </label>
        <button onClick={() => setLines([])} className="btn btn-sm btn-ghost">Clear</button>
      </div>

      <div ref={bodyRef} className="flex-1 overflow-y-auto font-mono text-[11px] leading-relaxed px-4 py-2 select-text">
        {filtered.length === 0 ? (
          <div className="text-[var(--text-dim)] py-12 text-center text-sm">No logs</div>
        ) : filtered.map((line, i) => {
          const lo = line.toLowerCase()
          const isErr = lo.includes('[error]') || lo.includes('[critical]')
          const isWarn = lo.includes('[warning]') || lo.includes('[warn]')
          return (
            <div key={i} className={`py-0.5 break-all whitespace-pre-wrap cursor-pointer hover:bg-[var(--bg-hover)] rounded px-1.5 -mx-1.5 ${isErr ? 'text-red-400' : isWarn ? 'text-orange-400' : 'text-[var(--text-secondary)]'}`}
              onClick={() => navigator.clipboard?.writeText(line)} title="Click to copy">
              {line}
            </div>
          )
        })}
      </div>
    </div>
  )
}

// Settings Tab
function SettingsTab() {
  const [config, setConfig] = useState<AppConfig | null>(null)
  const [saved, setSaved] = useState(false)
  const [credMsg, setCredMsg] = useState('')
  const [creds, setCreds] = useState({ currentPass: '', newUser: '', newPass: '' })

  useEffect(() => { getConfig().then(setConfig).catch(() => {}) }, [])

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
    setTimeout(() => setSaved(false), 2500)
  }

  const handleCredentials = async () => {
    if (!creds.currentPass) { setCredMsg('Current password required'); return }
    try {
      await updateCredentials(creds.currentPass, creds.newUser || undefined, creds.newPass || undefined)
      setCredMsg('\u2705 Credentials updated!')
      setCreds({ currentPass: '', newUser: '', newPass: '' })
    } catch (e: unknown) { setCredMsg(e instanceof Error ? e.message : 'Error') }
    setTimeout(() => setCredMsg(''), 3000)
  }

  if (!config) return <div className="flex-1 flex items-center justify-center text-[var(--text-dim)]">Loading...</div>

  return (
    <div className="p-5 sm:p-6 space-y-5 overflow-y-auto flex-1 max-w-2xl mx-auto">
      <h2 className="text-lg font-bold">Settings</h2>

      <div className="bg-[var(--bg-card)] border border-[var(--border-subtle)] rounded-xl p-5 space-y-4">
        <h3 className="text-xs font-semibold uppercase tracking-wider text-[var(--text-dim)]">Recording</h3>
        <div>
          <label className="block text-sm mb-1.5 text-[var(--text-secondary)]">Downloads Directory</label>
          <input type="text" value={config.downloadsDir}
            onChange={e => setConfig({ ...config, downloadsDir: e.target.value })}
            className="input-field" />
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
          <div>
            <label className="block text-sm mb-1.5 text-[var(--text-secondary)]">Container</label>
            <select value={config.container}
              onChange={e => setConfig({ ...config, container: e.target.value })}
              className="input-field">
              <option value="mkv">MKV</option>
              <option value="mp4">MP4</option>
              <option value="ts">TS</option>
            </select>
          </div>
          <div>
            <label className="block text-sm mb-1.5 text-[var(--text-secondary)]">Max Resolution</label>
            <input type="number" value={config.wantedResolution}
              onChange={e => setConfig({ ...config, wantedResolution: parseInt(e.target.value) || 99999 })}
              className="input-field" />
          </div>
        </div>
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
          <div>
            <label className="block text-sm mb-1.5 text-[var(--text-secondary)]">Min Free Disk %</label>
            <input type="number" step="0.5" value={config.minFreeDiskPercent}
              onChange={e => setConfig({ ...config, minFreeDiskPercent: parseFloat(e.target.value) || 5 })}
              className="input-field" />
          </div>
          <div className="flex items-end pb-1">
            <label className="flex items-center gap-2 text-sm cursor-pointer">
              <input type="checkbox" checked={config.debug}
                onChange={e => setConfig({ ...config, debug: e.target.checked })}
                className="accent-[var(--accent)] w-4 h-4" />
              Debug Mode
            </label>
          </div>
        </div>
      </div>

      <div className="bg-[var(--bg-card)] border border-[var(--border-subtle)] rounded-xl p-5 space-y-4">
        <h3 className="text-xs font-semibold uppercase tracking-wider text-[var(--text-dim)]">Login Credentials</h3>
        <input type="password" placeholder="Current password" value={creds.currentPass}
          onChange={e => setCreds({...creds, currentPass: e.target.value})}
          className="input-field" />
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
          <input type="text" placeholder="New username (optional)" value={creds.newUser}
            onChange={e => setCreds({...creds, newUser: e.target.value})}
            className="input-field" />
          <input type="password" placeholder="New password (optional)" value={creds.newPass}
            onChange={e => setCreds({...creds, newPass: e.target.value})}
            className="input-field" />
        </div>
        <div className="flex items-center gap-3">
          <button onClick={handleCredentials} className="btn btn-primary">Update Credentials</button>
          {credMsg && <span className={`text-sm ${credMsg.startsWith('\u2705') ? 'text-emerald-400' : 'text-red-400'}`}>{credMsg}</span>}
        </div>
      </div>

      <div className="bg-[var(--bg-card)] border border-[var(--border-subtle)] rounded-xl p-5 space-y-3">
        <h3 className="text-xs font-semibold uppercase tracking-wider text-[var(--text-dim)]">Web Server</h3>
        <div className="grid grid-cols-2 gap-4 text-sm">
          <div>
            <span className="text-[var(--text-dim)]">Host:</span>
            <span className="ml-2 font-mono text-[var(--text-secondary)]">{config.webHost}</span>
          </div>
          <div>
            <span className="text-[var(--text-dim)]">Port:</span>
            <span className="ml-2 font-mono text-[var(--text-secondary)]">{config.webPort}</span>
          </div>
        </div>
      </div>

      <button onClick={handleSave}
        className={`btn w-full py-3 ${saved ? 'btn-success' : 'btn-primary'}`}>
        {saved ? '\u2705 Saved!' : '\ud83d\udcbe Save Configuration'}
      </button>
    </div>
  )
}

// Add Model Dialog
function AddModelDialog({ sites, onAdd, onClose }: {
  sites: SiteInfo[]
  onAdd: (username: string, site: string) => void
  onClose: () => void
}) {
  const [username, setUsername] = useState('')
  const [site, setSite] = useState(sites[0]?.name || '')

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in" onClick={onClose}>
      <div className="absolute inset-0 bg-black/60 backdrop-blur-sm" />
      <div className="relative bg-[var(--bg-card)] border border-[var(--border)] rounded-2xl p-6 w-full max-w-md shadow-2xl animate-slide-up"
           onClick={e => e.stopPropagation()}>
        <h2 className="text-lg font-bold mb-5">Add Model</h2>
        <div className="space-y-4">
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Username</label>
            <input type="text" value={username} onChange={e => setUsername(e.target.value)}
              placeholder="Enter username..." autoFocus
              onKeyDown={e => { if (e.key === 'Enter' && username.trim()) onAdd(username.trim(), site) }}
              className="input-field" />
          </div>
          <div>
            <label className="block text-sm text-[var(--text-secondary)] mb-1.5">Site</label>
            <select value={site} onChange={e => setSite(e.target.value)} className="input-field">
              {sites.map(s => <option key={s.name} value={s.name}>{s.name} [{s.slug}]</option>)}
            </select>
          </div>
        </div>
        <div className="flex gap-3 mt-6">
          <button onClick={onClose} className="btn btn-ghost flex-1">Cancel</button>
          <button onClick={() => { if (username.trim()) onAdd(username.trim(), site) }}
            disabled={!username.trim()}
            className="btn btn-primary flex-1 disabled:opacity-40 disabled:cursor-not-allowed">
            Add Model
          </button>
        </div>
      </div>
    </div>
  )
}

// Main Dashboard
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
  const [selectedBot, setSelectedBot] = useState<BotState | null>(null)
  const [selectedCards, setSelectedCards] = useState<Set<string>>(new Set())
  const [error, setError] = useState<string | null>(null)
  const [connected, setConnected] = useState(true)
  const [authChecked, setAuthChecked] = useState(false)

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

  // Keep selected bot updated
  useEffect(() => {
    if (selectedBot) {
      const updated = models.find(b => b.username === selectedBot.username && b.siteSlug === selectedBot.siteSlug)
      if (updated && JSON.stringify(updated) !== JSON.stringify(selectedBot)) {
        setSelectedBot(updated)
      }
    }
  }, [models, selectedBot])

  const handleLogout = async () => { await logout(); router.push('/login') }

  if (!authChecked) {
    return (
      <div className="min-h-screen bg-[var(--bg-primary)] flex items-center justify-center">
        <div className="text-center">
          <div className="w-10 h-10 border-2 border-[var(--accent)] border-t-transparent rounded-full animate-spin mx-auto mb-4" />
          <p className="text-[var(--text-dim)] text-sm">Connecting...</p>
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
    { id: 'models', label: 'Models', icon: ic.globe, badge: models.length },
    { id: 'groups', label: 'Groups', icon: ic.link, badge: groups.length },
    { id: 'logs', label: 'Logs', icon: ic.log },
    { id: 'settings', label: 'Settings', icon: ic.cog },
  ]

  return (
    <div className="h-screen flex flex-col bg-[var(--bg-primary)]">
      {/* Header */}
      <header className="flex items-center gap-2 sm:gap-3 px-3 sm:px-5 py-2 bg-[var(--bg-secondary)] border-b border-[var(--border-subtle)] flex-shrink-0">
        <div className="flex items-center gap-2 mr-2">
          <span className="text-lg">{'\ud83c\udfac'}</span>
          <span className="font-bold text-sm sm:text-base tracking-tight hidden sm:inline">
            <span className="text-[var(--accent)]">Strea</span>Monitor
          </span>
          <div className={`w-1.5 h-1.5 rounded-full ${connected ? 'bg-emerald-400' : 'bg-red-400'}`} />
        </div>

        <nav className="flex gap-0.5 overflow-x-auto">
          {tabDefs.map(t => (
            <button key={t.id} onClick={() => setTab(t.id)}
              className={`tab-button ${tab === t.id ? 'active' : ''} px-2.5 sm:px-4`}>
              <Ico d={t.icon} />
              <span className="hidden sm:inline">{t.label}</span>
              {t.badge !== undefined && t.badge > 0 && (
                <span className="text-[10px] px-1.5 py-0.5 rounded-full bg-[var(--border)] text-[var(--text-dim)]">{t.badge}</span>
              )}
            </button>
          ))}
        </nav>

        <div className="ml-auto flex items-center gap-1.5">
          <div className="hidden lg:flex items-center gap-3 text-xs mr-3">
            <span className="text-[var(--text-dim)]">
              <span className="text-emerald-400 font-bold">{onCount}</span> online
            </span>
            {recCount > 0 && (
              <span className="text-red-400 font-bold animate-recording">{'\u25cf'} {recCount}</span>
            )}
            {disk && <span className="text-[var(--text-dim)]">{formatBytes(disk.freeBytes)} free</span>}
          </div>

          <button onClick={() => handleAction(startAll)} className="btn btn-sm btn-success" title="Start All">{'\u25b6'} All</button>
          <button onClick={() => handleAction(stopAll)} className="btn btn-sm btn-warning" title="Stop All">{'\u23f9'} All</button>
          <button onClick={() => handleAction(saveConfig)} className="btn btn-sm btn-icon btn-ghost" title="Save">
            <Ico d={ic.save} />
          </button>
          <button onClick={handleLogout} className="btn btn-sm btn-icon btn-ghost" title="Logout">
            <Ico d={ic.logout} />
          </button>
        </div>
      </header>

      {error && (
        <div className="bg-red-500/8 border-b border-red-500/15 px-5 py-2 flex items-center justify-between flex-shrink-0 animate-fade-in">
          <span className="text-sm text-red-400">{error}</span>
          <button onClick={() => setError(null)} className="text-red-400/60 hover:text-red-400 text-sm p-1">{'\u2715'}</button>
        </div>
      )}

      <div className="flex-1 flex flex-col overflow-hidden">
        {tab === 'models' && (
          <>
            <div className="flex items-center gap-2 px-4 sm:px-5 py-3 border-b border-[var(--border-subtle)] flex-shrink-0 flex-wrap">
              {(['all', 'recording', 'online', 'offline'] as const).map(f => (
                <button key={f} onClick={() => setFilter(f)}
                  className={`filter-pill ${filter === f ? 'active' : ''}`}>
                  {f === 'recording' ? `\u25cf Rec${recCount > 0 ? ` ${recCount}` : ''}` : f.charAt(0).toUpperCase() + f.slice(1)}
                </button>
              ))}
              <input type="text" value={search} onChange={e => setSearch(e.target.value)}
                placeholder="Search models..."
                className="input-field ml-1 flex-1 min-w-0 max-w-xs text-sm" />
              <button onClick={() => setShowAdd(true)} className="btn btn-primary ml-auto">
                <Ico d={ic.plus} /> Add
              </button>
            </div>

            {/* Bulk action bar when multiple cards selected */}
            {selectedCards.size > 1 && (
              <div className="flex items-center gap-2 px-4 sm:px-5 py-2 border-b border-[var(--accent)]/30 bg-[var(--accent-soft)] flex-shrink-0 animate-fade-in">
                <span className="text-sm font-medium text-[var(--accent)]">{selectedCards.size} selected</span>
                <button onClick={() => {
                  selectedCards.forEach(key => {
                    const [u, s] = key.split('_')
                    handleAction(() => startModel(u, s))
                  })
                }} className="btn btn-sm btn-success">Start Selected</button>
                <button onClick={() => {
                  selectedCards.forEach(key => {
                    const [u, s] = key.split('_')
                    handleAction(() => stopModel(u, s))
                  })
                }} className="btn btn-sm btn-warning">Stop Selected</button>
                <button onClick={() => {
                  if (!confirm(`Remove ${selectedCards.size} models?`)) return
                  selectedCards.forEach(key => {
                    const [u, s] = key.split('_')
                    handleAction(() => removeModel(u, s))
                  })
                  setSelectedCards(new Set())
                }} className="btn btn-sm btn-danger">Remove Selected</button>
                <button onClick={() => setSelectedCards(new Set())} className="btn btn-sm btn-ghost ml-auto">
                  Clear Selection
                </button>
              </div>
            )}

            <div className="flex-1 overflow-y-auto p-3 sm:p-4">
              {filteredModels.length === 0 ? (
                <div className="text-center py-20 text-[var(--text-dim)]">
                  {models.length === 0 ? (
                    <>
                      <div className="text-5xl mb-4 opacity-20">{'\ud83c\udfac'}</div>
                      <div className="font-medium text-[var(--text-secondary)]">No models added yet</div>
                      <div className="text-sm mt-1">Click &quot;Add&quot; to start monitoring</div>
                    </>
                  ) : (
                    <div className="text-sm">No models match the current filter</div>
                  )}
                </div>
              ) : (
                <div className="model-grid">
                  {filteredModels.map(bot => {
                    const cardKey = `${bot.username}_${bot.siteSlug}`
                    return (
                      <ModelCard
                        key={cardKey}
                        bot={bot}
                        groups={groups}
                        selected={selectedCards.has(cardKey)}
                        onClick={(e: React.MouseEvent) => {
                          if (e.ctrlKey || e.metaKey) {
                            // Ctrl+click: toggle this card in selection
                            setSelectedCards(prev => {
                              const next = new Set(prev)
                              if (next.has(cardKey)) next.delete(cardKey)
                              else next.add(cardKey)
                              return next
                            })
                          } else {
                            // Plain click: select only this one, open detail
                            setSelectedCards(new Set([cardKey]))
                            setSelectedBot(bot)
                          }
                        }}
                        onStart={() => handleAction(() => startModel(bot.username, bot.siteSlug))}
                        onStop={() => handleAction(() => stopModel(bot.username, bot.siteSlug))}
                      />
                    )
                  })}
                </div>
              )}
            </div>
          </>
        )}

        {tab === 'groups' && <GroupsTab groups={groups} sites={sites} onRefresh={refresh} />}
        {tab === 'logs' && <LogsTab />}
        {tab === 'settings' && <SettingsTab />}
      </div>

      <div className="flex items-center gap-4 px-5 py-1.5 bg-[var(--bg-secondary)] border-t border-[var(--border-subtle)] text-[11px]
                      text-[var(--text-dim)] flex-shrink-0">
        <span>StreaMonitor v{status?.version || '2.0.0'}</span>
        <span className="ml-auto">{models.length} models</span>
        {recCount > 0 && <span className="text-red-400">{'\u25cf'} {recCount} recording</span>}
      </div>

      {showAdd && <AddModelDialog sites={sites} onAdd={handleAdd} onClose={() => setShowAdd(false)} />}
      {selectedBot && (
        <ModelDetailModal
          bot={selectedBot}
          onClose={() => setSelectedBot(null)}
          onAction={handleAction}
        />
      )}
    </div>
  )
}
