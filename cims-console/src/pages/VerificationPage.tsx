import { useState } from 'react'
import { api } from '../api/client'
import { useToast } from '../components/Toast'

interface VerResult {
  total: number
  pass: number
  fail: number
  skip: number
  elapsed: number
  modules: Array<{
    module: string
    total: number
    pass: number
    fail: number
    skip: number
    results: Array<{
      id: string
      name: string
      status: string
      detail: string
      elapsed_ms: number
    }>
  }>
}

export default function VerificationPage() {
  const { show } = useToast()
  const [running, setRunning] = useState(false)
  const [result, setResult] = useState<VerResult | null>(null)
  const [expandedModule, setExpandedModule] = useState<string | null>(null)
  const [reportMd, setReportMd] = useState('')

  async function runVerification() {
    setRunning(true)
    setResult(null)
    setReportMd('')
    try {
      const r = await api.post<VerResult>('/verification/run', {})
      setResult(r)
      show(`검증 완료: ${r.pass}/${r.total} PASS (${((r.pass / r.total) * 100).toFixed(1)}%)`, r.fail === 0 ? 'ok' : 'err')
    } catch (e: unknown) {
      show(String(e), 'err')
    } finally {
      setRunning(false)
    }
  }

  async function loadReport() {
    try {
      const r = await api.get<{ content: string }>('/verification/report')
      setReportMd(r.content)
    } catch (e: unknown) {
      show(String(e), 'err')
    }
  }

  const rate = result ? ((result.pass / result.total) * 100).toFixed(1) : '—'

  return (
    <div className="page">
      <div className="toolbar">
        <button className="btn btn--primary" onClick={runVerification} disabled={running}>
          {running ? '검증 실행 중...' : '검증 실행'}
        </button>
        <button className="btn btn--outline" onClick={loadReport}>리포트 보기</button>
        {result && (
          <span style={{ marginLeft: 'auto', fontSize: 14 }}>
            <strong>{result.pass}</strong>/{result.total} PASS
            <span style={{ color: result.fail > 0 ? 'var(--danger)' : 'var(--success, #22c55e)', fontWeight: 600, marginLeft: 8 }}>
              ({rate}%)
            </span>
            <span className="ts" style={{ marginLeft: 12 }}>{result.elapsed?.toFixed(1)}초</span>
          </span>
        )}
      </div>

      {running && <div className="empty" style={{ padding: 40 }}>검증 실행 중... (약 3~4분 소요)</div>}

      {result && !running && (
        <>
          {/* 모듈별 요약 */}
          <div className="table-wrap" style={{ marginBottom: 16 }}>
            <table className="data-table">
              <thead><tr><th>모듈</th><th style={{ width: 60 }}>전체</th><th style={{ width: 60 }}>PASS</th><th style={{ width: 60 }}>FAIL</th><th style={{ width: 80 }}>합격률</th></tr></thead>
              <tbody>
                {result.modules.map(m => (
                  <tr key={m.module} style={{ cursor: 'pointer' }} onClick={() => setExpandedModule(expandedModule === m.module ? null : m.module)}>
                    <td style={{ fontWeight: 600 }}>
                      <span style={{ marginRight: 6 }}>{expandedModule === m.module ? '▼' : '▶'}</span>
                      {m.module}
                    </td>
                    <td>{m.total}</td>
                    <td style={{ color: 'var(--success, #22c55e)' }}>{m.pass}</td>
                    <td style={{ color: m.fail > 0 ? 'var(--danger)' : undefined }}>{m.fail}</td>
                    <td><span className={`badge ${m.fail === 0 ? 'badge--green' : 'badge--red'}`}>
                      {m.total > 0 ? ((m.pass / m.total) * 100).toFixed(0) : 0}%
                    </span></td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {/* 선택된 모듈 상세 */}
          {expandedModule && (() => {
            const mod = result.modules.find(m => m.module === expandedModule)
            if (!mod) return null
            return (
              <div className="table-wrap">
                <table className="data-table">
                  <thead><tr><th>ID</th><th>항목</th><th style={{ width: 60 }}>결과</th><th style={{ width: 60 }}>시간</th><th>상세</th></tr></thead>
                  <tbody>
                    {mod.results.map(r => (
                      <tr key={r.id}>
                        <td className="ts">{r.id}</td>
                        <td style={{ fontSize: 12 }}>{r.name}</td>
                        <td><span className={`badge ${r.status === 'PASS' ? 'badge--green' : r.status === 'FAIL' ? 'badge--red' : 'badge--gray'}`}>{r.status}</span></td>
                        <td className="ts">{r.elapsed_ms}ms</td>
                        <td style={{ fontSize: 11, maxWidth: 300, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}
                          title={r.detail}>{r.detail || '—'}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )
          })()}
        </>
      )}

      {/* 리포트 마크다운 */}
      {reportMd && (
        <div style={{ marginTop: 16 }}>
          <div style={{ fontWeight: 600, marginBottom: 8 }}>검증 리포트</div>
          <pre style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 6,
            padding: 16, fontSize: 12, lineHeight: 1.6, maxHeight: 400, overflowY: 'auto', whiteSpace: 'pre-wrap' }}>
            {reportMd}
          </pre>
        </div>
      )}
    </div>
  )
}
