import { useState, useEffect, useCallback } from 'react'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { useToast } from '../components/Toast'

type SvcState = { running: boolean; pid?: number }

const SERVICE_META: Array<{ name: ServiceName; label: string; critical?: boolean }> = [
  { name: 'cmp',     label: 'CMP (미디어)' },
  { name: 'csp',     label: 'CSP (SIP)' },
  { name: 'cwrtc',   label: 'cwrtc (WebRTC 브리지)' },
  { name: 'csc',     label: 'CSC (Admin API)', critical: true },
  { name: 'console', label: 'Console (웹 UI)' },
  { name: 'phone',   label: 'Phone (웹 단말)' },
]

export default function ServicesPage() {
  const { show } = useToast()
  const [states, setStates] = useState<Record<string, SvcState>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [lastOutput, setLastOutput] = useState<string>('')

  const load = useCallback(async () => {
    try {
      const r = await servicesApi.status()
      setStates(parseServiceStatus(r.output))
    } catch (e) {
      show(`상태 조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => {
    void load()
    const iv = setInterval(load, 5000)
    return () => clearInterval(iv)
  }, [load])

  async function act(name: ServiceName, action: ServiceAction, critical: boolean) {
    if (critical && action !== 'start') {
      const ok = confirm(
        action === 'stop'
          ? `⚠️ CSC 를 중지하면 Console UI 가 즉시 끊깁니다. 계속할까요?`
          : `⚠️ CSC 재시작 중 Console UI 일시 단절됩니다. 계속할까요?`
      )
      if (!ok) return
    } else if (action === 'stop' || action === 'restart') {
      const ok = confirm(`${name} 서비스를 ${action === 'stop' ? '중지' : '재시작'}할까요?`)
      if (!ok) return
    }

    setBusy(b => ({ ...b, [name]: true }))
    try {
      const r = await servicesApi.act(name, action)
      setLastOutput(r.stdout + (r.stderr ? `\n[stderr]\n${r.stderr}` : ''))
      if (r.returncode === 0) {
        show(`${name} ${action} 완료`, 'ok')
      } else {
        show(`${name} ${action} 실패 rc=${r.returncode}`, 'err')
      }
      // 잠시 대기 후 상태 재조회
      setTimeout(() => { void load() }, 1500)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>서비스 프로세스 제어</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          CMP/CSP/cwrtc/CSC/Console/Phone 프로세스의 start/stop/restart. cims.sh 드라이버 기반.
        </span>
        <div style={{ marginLeft: 'auto' }}>
          <button className="btn btn--outline" onClick={() => void load()}>↻ 새로고침</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 80 }}>서비스</th>
              <th>설명</th>
              <th style={{ width: 140 }}>상태</th>
              <th style={{ width: 100 }}>PID</th>
              <th style={{ width: 320 }}>제어</th>
            </tr>
          </thead>
          <tbody>
            {SERVICE_META.map(svc => {
              const s = states[svc.name]
              const running = s?.running ?? false
              const disabled = busy[svc.name]
              return (
                <tr key={svc.name}>
                  <td style={{ fontFamily: 'monospace', fontWeight: 'bold' }}>{svc.name}</td>
                  <td>{svc.label}{svc.critical && <span className="tag" style={{ marginLeft: 8, background: '#f39c12', color: '#fff' }}>critical</span>}</td>
                  <td>
                    <span className="tag" style={{
                      background: running ? '#2ecc71' : '#95a5a6',
                      color: '#fff',
                    }}>
                      {running ? '실행 중' : '중지됨'}
                    </span>
                  </td>
                  <td>{s?.pid ?? '—'}</td>
                  <td>
                    <button className="btn btn--sm" disabled={disabled || running}
                      onClick={() => act(svc.name, 'start', !!svc.critical)}>
                      ▶ start
                    </button>{' '}
                    <button className="btn btn--sm btn--outline" disabled={disabled || !running}
                      onClick={() => act(svc.name, 'restart', !!svc.critical)}>
                      ↻ restart
                    </button>{' '}
                    <button className="btn btn--sm btn--danger" disabled={disabled || !running}
                      onClick={() => act(svc.name, 'stop', !!svc.critical)}>
                      ■ stop
                    </button>
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}

      {lastOutput && (
        <div style={{ marginTop: 20 }}>
          <h4>최근 명령 출력</h4>
          <pre style={{
            background: '#0d1117', color: '#c9d1d9', padding: 12, borderRadius: 4,
            fontSize: 12, maxHeight: 240, overflow: 'auto',
            whiteSpace: 'pre-wrap',
          }}>
            {lastOutput.replace(/\u001b\[[0-9;]*m/g, '')}
          </pre>
        </div>
      )}
    </div>
  )
}
