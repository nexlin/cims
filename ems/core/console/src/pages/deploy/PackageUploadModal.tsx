import { useEffect, useMemo, useState } from 'react'
import Modal from '../../components/Modal'
import { useToast } from '../../components/Toast'
import { ApiError } from '../../api/client'
import { deploymentApi } from '../../api/deployment'
import { fmtSize, fmtSpeed, fmtEta } from './deployHelpers'

interface UploadRow {
  id: string
  file: File
  state: 'pending' | 'uploading' | 'done' | 'failed' | 'aborted' | 'skipped'
  pct: number
  loaded: number
  speedBps: number
  msg?: string
}

export default function PackageUploadModal({ onClose, onDone }: {
  onClose: () => void
  onDone: () => void
}) {
  const { show } = useToast()
  const [rows, setRows] = useState<UploadRow[]>([])
  const [busy, setBusy] = useState(false)
  const [aborts] = useState<Map<string, () => void>>(() => new Map())

  function update(id: string, patch: Partial<UploadRow>) {
    setRows(rs => rs.map(r => r.id === id ? { ...r, ...patch } : r))
  }

  function addFiles(fl: FileList | null) {
    if (!fl) return
    const next: UploadRow[] = []
    for (const f of Array.from(fl)) {
      next.push({
        id: `${f.name}-${f.size}-${f.lastModified}-${Math.random().toString(36).slice(2, 8)}`,
        file: f, state: 'pending', pct: 0, loaded: 0, speedBps: 0,
      })
    }
    setRows(prev => [...prev, ...next])
  }

  function abortOne(id: string) {
    const a = aborts.get(id)
    if (a) { a(); aborts.delete(id) }
  }
  function abortAll() {
    for (const a of aborts.values()) { try { a() } catch { /* */ } }
    aborts.clear()
  }

  useEffect(() => () => abortAll(), [])   // eslint-disable-line react-hooks/exhaustive-deps

  async function uploadOne(row: UploadRow, force: boolean): Promise<void> {
    update(row.id, { state: 'uploading', pct: 0, loaded: 0, speedBps: 0, msg: undefined })
    let lastPct = 0
    let lastTs = performance.now()
    let lastLoaded = 0
    const handle = deploymentApi.uploadPackageFile(row.file, force, p => {
      const now = performance.now()
      if (p.pct - lastPct >= 5 || now - lastTs >= 150 || p.pct === 100) {
        const dt = Math.max(now - lastTs, 1)
        const speed = ((p.loaded - lastLoaded) * 1000) / dt
        lastPct = p.pct; lastTs = now; lastLoaded = p.loaded
        update(row.id, { pct: p.pct, loaded: p.loaded, speedBps: speed })
      }
    })
    aborts.set(row.id, handle.abort)
    try {
      await handle.promise
      update(row.id, { state: 'done', pct: 100, loaded: row.file.size, msg: '완료' })
    } catch (e) {
      if (e instanceof ApiError && e.data?.error === 'aborted') {
        update(row.id, { state: 'aborted', msg: '취소됨' })
        return
      }
      if (e instanceof ApiError && e.status === 409) {
        update(row.id, { state: 'uploading', msg: '덮어쓰는 중' })
        try {
          const h2 = deploymentApi.uploadPackageFile(row.file, true, p => {
            update(row.id, { pct: p.pct, loaded: p.loaded })
          })
          aborts.set(row.id, h2.abort)
          await h2.promise
          update(row.id, { state: 'done', pct: 100, loaded: row.file.size, msg: '덮어씀' })
        } catch (e2) {
          if (e2 instanceof ApiError && e2.data?.error === 'aborted') {
            update(row.id, { state: 'aborted', msg: '취소됨' })
          } else {
            update(row.id, { state: 'failed', msg: (e2 as Error).message })
          }
        }
      } else {
        update(row.id, { state: 'failed', msg: (e as Error).message })
      }
    } finally {
      aborts.delete(row.id)
    }
  }

  async function uploadAll() {
    setBusy(true)
    try {
      for (const r of rows) {
        if (r.state === 'pending') {
          await uploadOne(r, false)
        }
      }
      await onDone()
      show('업로드 처리 완료', 'ok')
    } finally {
      setBusy(false)
    }
  }

  function closeModal() {
    abortAll()
    onClose()
  }

  const stats = useMemo(() => {
    const pending = rows.filter(r => r.state === 'pending').length
    const done    = rows.filter(r => r.state === 'done').length
    const failed  = rows.filter(r => r.state === 'failed').length
    return { pending, done, failed }
  }, [rows])

  return (
    <Modal title="패키지 업로드" onClose={closeModal} width={760}>
      <div style={{ display: 'flex', gap: 10, alignItems: 'center', flexWrap: 'wrap' }}>
        <label htmlFor="pkg-files" className="btn btn--outline" style={{ cursor: 'pointer' }}>
          📁 파일 선택 (여러 개 가능)
        </label>
        <input id="pkg-files" type="file" accept=".tar.gz,.tgz" multiple
          style={{ display: 'none' }}
          onChange={e => { addFiles(e.target.files); e.target.value = '' }} />
        {rows.length > 0 && (
          <span className="text-muted" style={{ fontSize: 12 }}>
            총 {rows.length} · 대기 {stats.pending} · 완료 {stats.done} · 실패 {stats.failed}
          </span>
        )}
      </div>

      <div style={{ marginTop: 10, fontSize: 12, color: 'var(--muted-foreground)' }}>
        ℹ 각 파일 내부의 <code>meta.json</code> 으로 이름/버전/설명/빌드정보 자동 추출.
        동일 (모듈명, 버전) 이 이미 있으면 자동으로 덮어씁니다.
      </div>

      {rows.length === 0 ? (
        <div className="empty" style={{ marginTop: 16 }}>업로드할 파일을 선택하세요</div>
      ) : (
        <table className="data-table" style={{ marginTop: 12 }}>
          <thead>
            <tr>
              <th>파일</th>
              <th style={{ width: 80 }}>크기</th>
              <th>진행</th>
              <th style={{ width: 100 }}>상태</th>
              <th style={{ width: 70 }}>작업</th>
            </tr>
          </thead>
          <tbody>
            {rows.map(r => <UploadProgressRow key={r.id} row={r}
              onAbort={() => abortOne(r.id)}
              onRemove={() => setRows(rs => rs.filter(x => x.id !== r.id))}
              onRetry={() => uploadOne(r, false)} />)}
          </tbody>
        </table>
      )}

      <div className="modal-footer" style={{ marginTop: 16 }}>
        <button className="btn btn--outline" onClick={closeModal}>닫기</button>
        <button className="btn btn--primary" disabled={busy || stats.pending === 0}
          onClick={uploadAll}>
          {busy ? '업로드 중...' : stats.pending > 0 ? `업로드 (${stats.pending}개)` : '완료'}
        </button>
      </div>
    </Modal>
  )
}

function UploadProgressRow({ row, onAbort, onRemove, onRetry }: {
  row: UploadRow
  onAbort: () => void
  onRemove: () => void
  onRetry: () => void
}) {
  const bar = (color: string) => ({
    width: `${row.pct}%`, height: '100%', background: color,
    transition: 'width 0.15s',
  })
  const remain = row.file.size - row.loaded
  const eta = row.speedBps > 0 ? remain / row.speedBps : 0
  const stateBadge: Record<UploadRow['state'], { bg: string; label: string }> = {
    pending:   { bg: '#bbb',    label: '대기' },
    uploading: { bg: '#3498db', label: '업로드' },
    done:      { bg: '#2ecc71', label: '완료' },
    failed:    { bg: '#e74c3c', label: '실패' },
    aborted:   { bg: '#95a5a6', label: '취소' },
    skipped:   { bg: '#7f8c8d', label: '건너뜀' },
  }
  const sb = stateBadge[row.state]

  return (
    <tr>
      <td style={{ fontSize: 12, fontFamily: 'monospace' }}>{row.file.name}</td>
      <td style={{ fontSize: 12 }}>{fmtSize(row.file.size)}</td>
      <td>
        {(row.state === 'uploading' || row.state === 'done') && (
          <>
            <div style={{ width: 240, height: 8, background: 'var(--muted)', borderRadius: 4, overflow: 'hidden' }}>
              <div style={bar(row.state === 'done' ? '#2ecc71' : '#3498db')} />
            </div>
            <span className="text-muted" style={{ fontSize: 11 }}>
              {row.pct}% · {fmtSize(row.loaded)}/{fmtSize(row.file.size)}
              {row.speedBps > 0 && row.state === 'uploading' && (
                <> · {fmtSpeed(row.speedBps)} · ETA {fmtEta(eta)}</>
              )}
            </span>
          </>
        )}
        {row.state === 'failed' && (
          <span style={{ color: '#e74c3c', fontSize: 12 }}>{row.msg}</span>
        )}
      </td>
      <td>
        <span className="tag" style={{
          background: sb.bg, color: '#fff', fontSize: 10, padding: '1px 6px', borderRadius: 3,
        }}>{sb.label}</span>
        {row.msg && row.state === 'done' && (
          <div style={{ fontSize: 10, color: 'var(--muted-foreground)' }}>{row.msg}</div>
        )}
      </td>
      <td>
        {row.state === 'uploading' && (
          <button className="btn btn--sm btn--outline" onClick={onAbort}>✕ 취소</button>
        )}
        {row.state === 'failed' && (
          <button className="btn btn--sm" onClick={onRetry}>재시도</button>
        )}
        {(row.state === 'pending' || row.state === 'aborted') && (
          <button className="btn btn--sm btn--outline" onClick={onRemove}>제거</button>
        )}
      </td>
    </tr>
  )
}
