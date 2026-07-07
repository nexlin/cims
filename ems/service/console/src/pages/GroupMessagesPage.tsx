import { useState, useEffect, useCallback } from 'react'
import { messagesApi, type GroupMessagesResponse } from '@core/api/messages'
import { useToast } from '@core/components/Toast'

const TYPE_LABEL: Record<string, string> = { sds: '메시지', fd: '파일', text: '평문' }

function fmtBytes(n?: number): string {
  if (!n) return '—'
  if (n >= 1048576) return `${(n / 1048576).toFixed(1)}MB`
  if (n >= 1024) return `${(n / 1024).toFixed(1)}KB`
  return `${n}B`
}

/** 그룹 메시지 이력 — CSP MCDATA-AS 보관(messages.jsonl) 조회 (mcdata_messaging.md). */
export default function GroupMessagesPage() {
  const { show } = useToast()
  const [data, setData] = useState<GroupMessagesResponse | null>(null)
  const [date, setDate] = useState(new Date().toISOString().substring(0, 10))
  const [group, setGroup] = useState('')
  const [q, setQ] = useState('')
  const [loading, setLoading] = useState(false)

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await messagesApi.list({ date, group_id: group, q, limit: 500 })) }
    catch (e: unknown) { show(String(e), 'err') }
    finally { setLoading(false) }
  }, [date, group, q, show])

  useEffect(() => { load() }, [load])

  return (
    <div>
      <div className="toolbar">
        <input type="date" className="form-input" value={date} onChange={e => setDate(e.target.value)} style={{ width: 150 }} />
        <select className="form-input" value={group} onChange={e => setGroup(e.target.value)} style={{ width: 150 }}>
          <option value="">전체 그룹</option>
          {(data?.groups || []).map(g => <option key={g} value={g}>{g}</option>)}
        </select>
        <input className="form-input" placeholder="본문·발신자·파일명 검색" value={q}
          onChange={e => setQ(e.target.value)} onKeyDown={e => { if (e.key === 'Enter') load() }} style={{ width: 220 }} />
        <button className="btn btn--primary btn--sm" onClick={load}>조회</button>
        {data && <span className="ts" style={{ marginLeft: 'auto' }}>총 {data.total}건{data.total > data.items.length ? ` (표시 ${data.items.length})` : ''}</span>}
      </div>

      {loading ? <div className="empty">로딩 중...</div> : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 150 }}>시각</th>
              <th style={{ width: 90 }}>그룹</th>
              <th style={{ width: 130 }}>발신자</th>
              <th style={{ width: 60 }}>유형</th>
              <th>내용</th>
              <th style={{ width: 70 }}>크기</th>
              <th style={{ width: 60 }} title="배포된 수신자 수">수신</th>
              <th style={{ width: 70 }} title="delivered/read 확인 요청">전달요청</th>
            </tr>
          </thead>
          <tbody>
            {(data?.items || []).map((m, i) => (
              <tr key={`${m.msg_id || i}-${m.ts}`}>
                <td className="ts">{m.ts?.replace('T', ' ')}</td>
                <td style={{ fontSize: 12 }}>{m.group}</td>
                <td style={{ fontSize: 12 }}>{m.from}</td>
                <td><span className={`badge ${m.msg_type === 'fd' ? 'badge--blue' : ''}`} style={{ fontSize: 11 }}>{TYPE_LABEL[m.msg_type] || m.msg_type}</span></td>
                <td style={{ fontSize: 12, maxWidth: 420, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                  title={m.msg_type === 'fd' ? `${m.file_name} (${m.file_type || ''})` : m.text}>
                  {m.msg_type === 'fd' ? `📎 ${m.file_name || '(파일)'} · ${fmtBytes(m.file_size)}` : m.text}
                </td>
                <td className="ts" style={{ textAlign: 'right' }}>{fmtBytes(m.msg_type === 'fd' ? m.file_size : m.size)}</td>
                <td className="ts" style={{ textAlign: 'right' }}>{m.fanout}</td>
                <td className="ts">{m.disposition_req ? (m.disposition_req === 1 ? '전달' : m.disposition_req === 2 ? '읽음' : '전달+읽음') : '—'}</td>
              </tr>
            ))}
            {(!data || data.items.length === 0) && <tr><td colSpan={8} className="empty-cell">메시지 없음</td></tr>}
          </tbody>
        </table>
      )}
    </div>
  )
}
