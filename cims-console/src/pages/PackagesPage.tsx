import { useState, useEffect, useCallback, useRef } from 'react'
import { deploymentApi, type SipPackage } from '../api/deployment'
import { ApiError } from '../api/client'
import { useToast } from '../components/Toast'

interface PackageMeta {
  build_date?: string | null
  git_sha?: string | null
  git_branch?: string | null
  packaged_at?: string | null
  packaged_by?: string | null
  changelog?: string | null
}
interface UploadResult extends SipPackage { meta?: PackageMeta }
interface ConflictInfo {
  error: 'version_conflict'
  name: string; version: string; existing_id: number
  existing_sha256: string; uploaded_at: string | null; uploaded_by: string | null
  hint?: string
}

type ItemStatus = 'pending' | 'uploading' | 'done' | 'conflict' | 'error' | 'skipped'

interface UploadItem {
  id: string
  file: File
  status: ItemStatus
  progress: number          // 0~100
  bytesLoaded: number       // 업로드 전송 완료 바이트
  speedBps: number          // bytes/sec (최근 샘플)
  startedAt?: number        // performance.now() 업로드 시작 시각
  result?: UploadResult
  conflict?: ConflictInfo
  error?: string
}

export default function PackagesPage() {
  const { show } = useToast()
  const [items, setItems] = useState<SipPackage[]>([])
  const [loading, setLoading] = useState(true)

  const [uploadOpen, setUploadOpen] = useState(false)
  const [queue, setQueue] = useState<UploadItem[]>([])
  const [busy, setBusy] = useState(false)
  // 진행 중 XHR 핸들 — id → abort 함수. resetModal/개별 취소에서 호출
  const handlesRef = useRef<Map<string, () => void>>(new Map())

  const load = useCallback(async () => {
    setLoading(true)
    try { setItems(await deploymentApi.listPackages()) }
    catch (e) { show((e as Error).message, 'err') }
    finally { setLoading(false) }
  }, [show])

  useEffect(() => { void load() }, [load])
  useEffect(() => {
    // 페이지 이탈/탭 닫기 시 진행 중 XHR 모두 abort — 연결 풀 선점 방지
    const cleanup = () => {
      for (const abort of handlesRef.current.values()) {
        try { abort() } catch { /* no-op */ }
      }
      handlesRef.current.clear()
    }
    window.addEventListener('beforeunload', cleanup)
    window.addEventListener('pagehide', cleanup)
    return () => {
      cleanup()
      window.removeEventListener('beforeunload', cleanup)
      window.removeEventListener('pagehide', cleanup)
    }
  }, [])

  function addFiles(files: FileList | null) {
    if (!files || files.length === 0) return
    const next: UploadItem[] = []
    for (const f of Array.from(files)) {
      next.push({
        id: `${f.name}-${f.size}-${f.lastModified}-${Math.random().toString(36).slice(2,8)}`,
        file: f,
        status: 'pending',
        progress: 0, bytesLoaded: 0, speedBps: 0,
      })
    }
    setQueue(q => [...q, ...next])
  }

  function updateItem(id: string, patch: Partial<UploadItem>) {
    setQueue(q => q.map(it => it.id === id ? { ...it, ...patch } : it))
  }

  function removeItem(id: string) {
    setQueue(q => q.filter(it => it.id !== id))
  }

  async function uploadOne(item: UploadItem, force: boolean): Promise<void> {
    const startedAt = performance.now()
    updateItem(item.id, {
      status: 'uploading', progress: 0, bytesLoaded: 0, speedBps: 0, startedAt,
      error: undefined, conflict: undefined,
    })
    // 진행률 이벤트는 초당 수백회 fire — setState 폭주 방지를 위해 200ms / 5% throttle
    let lastPct = 0
    let lastTs = startedAt
    let lastLoaded = 0
    const handle = deploymentApi.uploadPackageFile(item.file, force, (p) => {
      const now = performance.now()
      if (p.pct - lastPct >= 5 || now - lastTs >= 200 || p.pct === 100) {
        const dt = Math.max(now - lastTs, 1)
        const speedBps = ((p.loaded - lastLoaded) * 1000) / dt
        lastPct = p.pct; lastTs = now; lastLoaded = p.loaded
        updateItem(item.id, {
          progress: p.pct, bytesLoaded: p.loaded, speedBps,
        })
      }
    })
    handlesRef.current.set(item.id, handle.abort)
    try {
      const res = await handle.promise
      updateItem(item.id, {
        status: 'done', progress: 100, bytesLoaded: item.file.size,
        result: res as UploadResult,
      })
    } catch (e) {
      if (e instanceof ApiError && e.status === 409 && e.data?.error === 'version_conflict') {
        updateItem(item.id, { status: 'conflict', conflict: e.data as unknown as ConflictInfo })
        return
      }
      if (e instanceof ApiError && e.data?.error === 'aborted') {
        updateItem(item.id, { status: 'skipped', error: '취소됨' })
        return
      }
      updateItem(item.id, { status: 'error', error: (e as Error).message })
    } finally {
      handlesRef.current.delete(item.id)
    }
  }

  function abortAll() {
    for (const abort of handlesRef.current.values()) {
      try { abort() } catch { /* no-op */ }
    }
    handlesRef.current.clear()
  }

  function abortOne(id: string) {
    const a = handlesRef.current.get(id)
    if (a) { try { a() } catch { /* no-op */ } handlesRef.current.delete(id) }
  }

  function fmtSpeed(bps: number) {
    if (bps < 1024) return `${bps.toFixed(0)} B/s`
    if (bps < 1024*1024) return `${(bps/1024).toFixed(0)} KB/s`
    return `${(bps/1024/1024).toFixed(1)} MB/s`
  }
  function fmtEta(bytesRemaining: number, bps: number) {
    if (bps <= 0) return '—'
    const sec = bytesRemaining / bps
    if (sec < 1)  return '<1s'
    if (sec < 60) return `${sec.toFixed(0)}s`
    return `${Math.floor(sec/60)}m ${Math.floor(sec%60)}s`
  }

  // 최대 MAX_PARALLEL 개씩 동시 업로드 (서버/네트워크 과부하 방지)
  const MAX_PARALLEL = 3

  async function uploadAllPending() {
    setBusy(true)
    try {
      const pending = queue.filter(it => it.status === 'pending')
      const pool: Promise<void>[] = []
      let idx = 0
      const runner = async () => {
        while (idx < pending.length) {
          const it = pending[idx++]
          await uploadOne(it, false)
        }
      }
      for (let i = 0; i < Math.min(MAX_PARALLEL, pending.length); i++) {
        pool.push(runner())
      }
      await Promise.allSettled(pool)
      await load()
    } finally {
      setBusy(false)
    }
  }

  async function overwriteOne(item: UploadItem) {
    setBusy(true)
    try {
      await uploadOne(item, true)
      await load()
    } finally {
      setBusy(false)
    }
  }

  async function remove(p: SipPackage) {
    if (!confirm(`${p.name} ${p.version} 을 삭제할까요?`)) return
    try { await deploymentApi.deletePackage(p.id); show('삭제됨', 'ok'); await load() }
    catch (e) { show((e as Error).message, 'err') }
  }

  function fmtSize(n: number) {
    if (n < 1024) return `${n} B`
    if (n < 1024*1024) return `${(n/1024).toFixed(1)} KB`
    if (n < 1024*1024*1024) return `${(n/1024/1024).toFixed(1)} MB`
    return `${(n/1024/1024/1024).toFixed(2)} GB`
  }

  function resetModal() {
    abortAll()
    setUploadOpen(false); setQueue([]); setBusy(false)
  }

  const pendingCount  = queue.filter(it => it.status === 'pending').length
  const doneCount     = queue.filter(it => it.status === 'done').length
  const conflictCount = queue.filter(it => it.status === 'conflict').length
  const errorCount    = queue.filter(it => it.status === 'error').length

  function statusTag(s: ItemStatus) {
    const map: Record<ItemStatus, { bg: string; fg: string; label: string }> = {
      pending:   { bg: '#bbb',    fg: '#fff', label: '대기' },
      uploading: { bg: '#3498db', fg: '#fff', label: '업로드 중' },
      done:      { bg: '#2ecc71', fg: '#fff', label: '완료' },
      conflict:  { bg: '#e67e22', fg: '#fff', label: '충돌' },
      error:     { bg: '#e74c3c', fg: '#fff', label: '실패' },
      skipped:   { bg: '#7f8c8d', fg: '#fff', label: '건너뜀' },
    }
    const c = map[s]
    return <span className="tag" style={{ background: c.bg, color: c.fg }}>{c.label}</span>
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>배포 패키지</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          tarball 최상위의 <code>meta.json</code> 으로 이름/버전/설명/빌드정보 자동 인식.
          여러 파일 동시 업로드 가능.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻</button>{' '}
          <button className="btn btn--primary" onClick={() => setUploadOpen(true)}>＋ 업로드</button>
        </div>
      </div>

      {loading ? <div className="empty">로딩 중...</div> :
        items.length === 0 ? <div className="empty">등록된 패키지 없음</div> : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 40 }}>ID</th>
              <th>이름</th>
              <th>버전</th>
              <th>크기</th>
              <th>SHA256</th>
              <th>설명 / 빌드 / changelog</th>
              <th>업로드</th>
              <th style={{ width: 80 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {items.map(p => (
              <tr key={p.id}>
                <td>{p.id}</td>
                <td>{p.name}</td>
                <td>{p.version}</td>
                <td>{fmtSize(p.file_size)}</td>
                <td style={{ fontSize: 11, fontFamily: 'monospace' }}>{p.sha256.substring(0, 12)}…</td>
                <td style={{ fontSize: 12 }}>{p.description}</td>
                <td style={{ fontSize: 12 }}>{p.uploaded_at} <br/><span className="text-muted">{p.uploaded_by}</span></td>
                <td><button className="btn btn--sm btn--danger" onClick={() => remove(p)}>삭제</button></td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {uploadOpen && (
        <div className="modal-overlay" onClick={resetModal}>
          <div className="modal-box" style={{ minWidth: 720, maxHeight: '88vh', overflow: 'auto' }}
               onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">패키지 업로드 (다중 파일)</span>
              <button className="modal-close" onClick={resetModal}>✕</button>
            </div>
            <div className="modal-body">
              <div style={{ display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap' }}>
                <label htmlFor="pkg-file-input"
                  className="btn btn--outline" style={{ cursor: 'pointer' }}>
                  📁 파일 선택 (여러 개 가능)
                </label>
                <input id="pkg-file-input" type="file" accept=".tar.gz,.tgz" multiple
                  style={{ display: 'none' }}
                  onChange={e => { addFiles(e.target.files); e.target.value = '' }} />
                {queue.length > 0 && (
                  <span className="text-muted" style={{ fontSize: 12 }}>
                    총 {queue.length}개 · 대기 {pendingCount} · 완료 {doneCount} · 충돌 {conflictCount} · 실패 {errorCount}
                  </span>
                )}
              </div>

              <div style={{ marginTop: 12, fontSize: 12, color: '#888' }}>
                ℹ 이름·버전·설명·changelog 는 각 tarball 의 <code>meta.json</code> 에서 자동 추출됩니다.
                동일 (이름, 버전) 이 이미 있으면 해당 파일만 "충돌" 상태로 표시되고 건별로 덮어쓰기 가능합니다.
              </div>

              {queue.length === 0 ? (
                <div className="empty" style={{ marginTop: 16 }}>업로드할 파일을 선택하세요</div>
              ) : (
                <table className="data-table" style={{ marginTop: 12 }}>
                  <thead>
                    <tr>
                      <th>파일</th>
                      <th style={{ width: 80 }}>크기</th>
                      <th style={{ width: 90 }}>상태</th>
                      <th>결과 / 충돌</th>
                      <th style={{ width: 200 }}>작업</th>
                    </tr>
                  </thead>
                  <tbody>
                    {queue.map(it => (
                      <tr key={it.id}>
                        <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{it.file.name}</td>
                        <td style={{ fontSize: 12 }}>{fmtSize(it.file.size)}</td>
                        <td>{statusTag(it.status)}</td>
                        <td style={{ fontSize: 12 }}>
                          {it.status === 'uploading' && (
                            <div>
                              <div style={{
                                width: 220, height: 8, background: '#eee',
                                borderRadius: 4, overflow: 'hidden',
                              }}>
                                <div style={{
                                  width: `${it.progress}%`, height: '100%',
                                  background: '#3498db', transition: 'width 0.2s',
                                }} />
                              </div>
                              <span className="text-muted" style={{ fontSize: 11 }}>
                                {it.progress}% · {fmtSize(it.bytesLoaded)} / {fmtSize(it.file.size)}
                                {it.speedBps > 0 && <> · {fmtSpeed(it.speedBps)} · ETA {fmtEta(it.file.size - it.bytesLoaded, it.speedBps)}</>}
                              </span>
                            </div>
                          )}
                          {it.status === 'done' && it.result && (
                            <span>
                              <b>{it.result.name}</b> {it.result.version}
                              {it.result.meta?.changelog && (
                                <span className="text-muted"> — {it.result.meta.changelog}</span>
                              )}
                            </span>
                          )}
                          {it.status === 'conflict' && it.conflict && (
                            <span style={{ color: '#e67e22' }}>
                              <b>{it.conflict.name} {it.conflict.version}</b> 이미 존재
                              {' '}(#{it.conflict.existing_id}, {it.conflict.uploaded_at})
                            </span>
                          )}
                          {it.status === 'error' && (
                            <span style={{ color: '#e74c3c' }}>{it.error}</span>
                          )}
                        </td>
                        <td>
                          {it.status === 'uploading' && (
                            <button className="btn btn--sm btn--outline"
                              onClick={() => abortOne(it.id)}>✕ 취소</button>
                          )}
                          {it.status === 'conflict' && (
                            <>
                              <button className="btn btn--sm btn--danger" disabled={busy}
                                onClick={() => overwriteOne(it)}>↻ 덮어쓰기</button>{' '}
                              <button className="btn btn--sm btn--outline" disabled={busy}
                                onClick={() => updateItem(it.id, { status: 'skipped' })}>건너뜀</button>
                            </>
                          )}
                          {it.status === 'error' && (
                            <button className="btn btn--sm" disabled={busy}
                              onClick={() => uploadOne(it, false)}>재시도</button>
                          )}
                          {(it.status === 'pending' || it.status === 'skipped') && (
                            <button className="btn btn--sm btn--outline" disabled={busy}
                              onClick={() => removeItem(it.id)}>제거</button>
                          )}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              )}
            </div>
            <div className="modal-footer">
              <button className="btn btn--outline" onClick={resetModal}>
                {queue.length > 0 && (doneCount === queue.length - errorCount) ? '닫기' : '취소'}
              </button>
              <button className="btn btn--primary" disabled={busy || pendingCount === 0}
                onClick={uploadAllPending}>
                {busy ? '업로드 중...' : pendingCount > 0 ? `업로드 (${pendingCount}개)` : '업로드'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
