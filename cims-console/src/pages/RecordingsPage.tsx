import { useState, useEffect, useCallback, useRef } from 'react'
import { recordingsApi, type Recording, type RecordingSegment, type RecordingsQuery } from '../api/recordings'
import { useToast } from '../components/Toast'

function fmtDuration(sec: number): string {
  const m = Math.floor(sec / 60)
  const s = sec % 60
  return `${m}:${String(s).padStart(2, '0')}`
}

function fmtTime(iso: string | null): string {
  if (!iso) return '-'
  const d = new Date(iso)
  return d.toLocaleTimeString('ko-KR', { hour: '2-digit', minute: '2-digit', second: '2-digit' })
}

function fmtDate(iso: string | null): string {
  if (!iso) return '-'
  return new Date(iso).toLocaleDateString('ko-KR')
}

function fmtSize(bytes: number): string {
  if (bytes <= 0) return '-'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / 1048576).toFixed(1)} MB`
}

function statusBadge(status: string) {
  switch (status) {
    case 'ready':       return <span className="badge badge--green">완료</span>
    case 'transcoding': return <span className="badge badge--blue">변환중</span>
    case 'raw':         return <span className="badge badge--gray">미변환</span>
    case 'failed':      return <span className="badge badge--red">실패</span>
    default:            return <span className="badge">{status}</span>
  }
}

export default function RecordingsPage() {
  const { show } = useToast()

  // 목록
  const [list, setList] = useState<Recording[]>([])
  const [total, setTotal] = useState(0)
  const [loading, setLoading] = useState(true)
  const [page, setPage] = useState(0)
  const PAGE_SIZE = 30

  // 필터
  const [callType, setCallType] = useState('')
  const [search, setSearch] = useState('')
  const [fromDt, setFromDt] = useState('')
  const [toDt, setToDt] = useState('')

  // 상세/재생
  const [detail, setDetail] = useState<Recording | null>(null)
  const [segments, setSegments] = useState<RecordingSegment[]>([])
  const [playUrl, setPlayUrl] = useState('')
  const [playType, setPlayType] = useState<'audio' | 'video'>('audio')
  const playerRef = useRef<HTMLAudioElement | HTMLVideoElement>(null)

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const q: RecordingsQuery = { limit: PAGE_SIZE, offset: page * PAGE_SIZE }
      if (callType) q.call_type = callType
      if (search) q.caller = search
      if (fromDt) q.from_dt = fromDt
      if (toDt) q.to_dt = toDt
      const res = await recordingsApi.list(q)
      setList(res.recordings)
      setTotal(res.total)
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setLoading(false)
    }
  }, [page, callType, search, fromDt, toDt, show])

  useEffect(() => { load() }, [load])

  async function openDetail(rec: Recording) {
    try {
      const full = await recordingsApi.get(rec.id)
      setDetail(full)
      if (full.call_type === 'ptt' && full.segments) {
        setSegments(full.segments)
      } else {
        setSegments([])
      }
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  function playRecording(rec: Recording) {
    if (rec.has_video) {
      setPlayUrl(recordingsApi.videoUrl(rec.id, 'a'))
      setPlayType('video')
    } else {
      setPlayUrl(recordingsApi.audioUrl(rec.id))
      setPlayType('audio')
    }
  }

  function playSegment(recId: number, seg: RecordingSegment) {
    setPlayUrl(recordingsApi.segmentAudioUrl(recId, seg.seq))
    setPlayType(seg.has_video ? 'video' : 'audio')
  }

  async function handleDelete(id: number) {
    if (!confirm('녹취를 삭제하시겠습니까?')) return
    try {
      await recordingsApi.delete(id)
      show('삭제 완료', 'ok')
      setDetail(null)
      load()
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const totalPages = Math.ceil(total / PAGE_SIZE)

  return (
    <div className="page">

      {/* 툴바 */}
      <div className="toolbar">
        <select className="form-input" style={{ width: 120 }}
          value={callType} onChange={e => { setCallType(e.target.value); setPage(0) }}>
          <option value="">전체</option>
          <option value="voip">VoIP</option>
          <option value="ptt">PTT</option>
        </select>
        <input className="search-input" placeholder="발신자/착신자 검색"
          value={search} onChange={e => setSearch(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && (setPage(0), load())} />
        <input className="form-input" type="date" value={fromDt}
          onChange={e => { setFromDt(e.target.value); setPage(0) }} />
        <span style={{ color: 'var(--text-muted)' }}>~</span>
        <input className="form-input" type="date" value={toDt}
          onChange={e => { setToDt(e.target.value); setPage(0) }} />
        <button className="btn btn--primary" onClick={() => { setPage(0); load() }}>검색</button>
        <span style={{ marginLeft: 'auto', color: 'var(--text-muted)', fontSize: 13 }}>
          총 {total}건
        </span>
      </div>

      {/* 테이블 */}
      <div className="panel">
        <table className="data-table">
          <thead>
            <tr>
              <th>유형</th>
              <th>발신</th>
              <th>착신/그룹</th>
              <th>날짜</th>
              <th>시간</th>
              <th>길이</th>
              <th>영상</th>
              <th>상태</th>
              <th>재생</th>
            </tr>
          </thead>
          <tbody>
            {loading ? (
              <tr><td colSpan={9} className="empty">로딩 중...</td></tr>
            ) : list.length === 0 ? (
              <tr><td colSpan={9} className="empty">녹취 데이터가 없습니다</td></tr>
            ) : list.map(rec => (
              <tr key={rec.id} style={{ cursor: 'pointer' }}
                onClick={() => openDetail(rec)}>
                <td>
                  <span className={`badge ${rec.call_type === 'voip' ? 'badge--blue' : 'badge--green'}`}>
                    {rec.call_type === 'voip' ? 'VoIP' : 'PTT'}
                  </span>
                </td>
                <td>{rec.caller}</td>
                <td>{rec.call_type === 'ptt' ? rec.group_id : rec.callee}</td>
                <td className="ts">{fmtDate(rec.start_time)}</td>
                <td className="ts">{fmtTime(rec.start_time)}</td>
                <td>
                  {rec.call_type === 'ptt'
                    ? `${rec.segment_count}건 / ${fmtDuration(Math.floor(rec.total_speech_ms / 1000))}`
                    : fmtDuration(rec.duration)}
                </td>
                <td style={{ textAlign: 'center' }}>{rec.has_video ? '○' : ''}</td>
                <td>{statusBadge(rec.status)}</td>
                <td>
                  <button className="btn btn--ghost btn--sm"
                    onClick={e => { e.stopPropagation(); playRecording(rec) }}
                    disabled={rec.call_type === 'ptt'}>
                    ▶
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        </table>

        {/* 페이지네이션 */}
        {totalPages > 1 && (
          <div style={{ display: 'flex', justifyContent: 'center', gap: 8, padding: 12 }}>
            <button className="btn btn--ghost btn--sm" disabled={page === 0}
              onClick={() => setPage(p => p - 1)}>◀</button>
            <span style={{ lineHeight: '32px', fontSize: 13, color: 'var(--text-muted)' }}>
              {page + 1} / {totalPages}
            </span>
            <button className="btn btn--ghost btn--sm" disabled={page >= totalPages - 1}
              onClick={() => setPage(p => p + 1)}>▶</button>
          </div>
        )}
      </div>

      {/* 플레이어 */}
      {playUrl && (
        <div className="panel" style={{ padding: 16 }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 8 }}>
            <strong>재생</strong>
            <button className="btn btn--ghost btn--sm" onClick={() => setPlayUrl('')}>✕ 닫기</button>
          </div>
          {playType === 'video' ? (
            <video ref={playerRef as React.RefObject<HTMLVideoElement>}
              src={playUrl} controls autoPlay
              style={{ width: '100%', maxHeight: 400, background: '#000', borderRadius: 'var(--radius)' }} />
          ) : (
            <audio ref={playerRef as React.RefObject<HTMLAudioElement>}
              src={playUrl} controls autoPlay style={{ width: '100%' }} />
          )}
        </div>
      )}

      {/* PTT/VoIP 상세 모달 */}
      {detail && (
        <div className="modal-overlay" onClick={() => setDetail(null)}>
          <div className="modal-box" style={{ maxWidth: 720 }} onClick={e => e.stopPropagation()}>
            <div className="modal-header">
              <span className="modal-title">
                {detail.call_type === 'ptt' ? `PTT 녹취 — 그룹 ${detail.group_id}` : `VoIP 녹취 — ${detail.caller} → ${detail.callee}`}
              </span>
              <button className="modal-close" onClick={() => setDetail(null)}>✕</button>
            </div>
            <div className="modal-body">
              {/* 메타 정보 */}
              <div className="form-grid" style={{ marginBottom: 16 }}>
                <label>유형</label>
                <span className={`badge ${detail.call_type === 'voip' ? 'badge--blue' : 'badge--green'}`}>
                  {detail.call_type === 'voip' ? 'VoIP' : 'PTT'}
                </span>
                <label>시작</label><span>{fmtDate(detail.start_time)} {fmtTime(detail.start_time)}</span>
                <label>종료</label><span>{detail.end_time ? `${fmtDate(detail.end_time)} ${fmtTime(detail.end_time)}` : '진행중'}</span>
                <label>길이</label><span>{fmtDuration(detail.duration)}</span>
                <label>영상</label><span>{detail.has_video ? '있음' : '없음'}</span>
                <label>크기</label><span>{fmtSize(detail.file_size)}</span>
                <label>상태</label><span>{statusBadge(detail.status)}</span>
              </div>

              {/* VoIP: 재생 버튼 */}
              {detail.call_type === 'voip' && (
                <div style={{ display: 'flex', gap: 8, marginBottom: 16 }}>
                  <button className="btn btn--primary"
                    onClick={() => { setPlayUrl(recordingsApi.audioUrl(detail.id)); setPlayType('audio') }}>
                    음성 재생
                  </button>
                  {detail.has_video && (
                    <>
                      <button className="btn btn--outline"
                        onClick={() => { setPlayUrl(recordingsApi.videoUrl(detail.id, 'a')); setPlayType('video') }}>
                        발신측 영상
                      </button>
                      <button className="btn btn--outline"
                        onClick={() => { setPlayUrl(recordingsApi.videoUrl(detail.id, 'b')); setPlayType('video') }}>
                        착신측 영상
                      </button>
                    </>
                  )}
                </div>
              )}

              {/* PTT: 세그먼트 목록 */}
              {detail.call_type === 'ptt' && segments.length > 0 && (
                <>
                  <div style={{ marginBottom: 8, fontWeight: 600, fontSize: 14 }}>
                    발언 목록 ({segments.length}건, 총 {fmtDuration(Math.floor(detail.total_speech_ms / 1000))})
                  </div>
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>#</th>
                        <th>화자</th>
                        <th>시작</th>
                        <th>길이</th>
                        <th>영상</th>
                        <th>상태</th>
                        <th>재생</th>
                      </tr>
                    </thead>
                    <tbody>
                      {segments.map(seg => (
                        <tr key={seg.seq}>
                          <td>{seg.seq}</td>
                          <td>{seg.speaker_id}</td>
                          <td className="ts">{fmtTime(seg.start_time)}</td>
                          <td>{fmtDuration(Math.floor(seg.duration_ms / 1000))}</td>
                          <td style={{ textAlign: 'center' }}>{seg.has_video ? '○' : ''}</td>
                          <td>{statusBadge(seg.status)}</td>
                          <td>
                            <button className="btn btn--ghost btn--sm"
                              onClick={() => playSegment(detail.id, seg)}>
                              ▶
                            </button>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </>
              )}
              {detail.call_type === 'ptt' && segments.length === 0 && (
                <div className="empty">발언 세그먼트가 없습니다</div>
              )}
            </div>
            <div className="modal-footer">
              <button className="btn btn--danger" onClick={() => handleDelete(detail.id)}>삭제</button>
              <button className="btn btn--outline" onClick={() => setDetail(null)}>닫기</button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
