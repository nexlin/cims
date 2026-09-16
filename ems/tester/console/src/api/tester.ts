// 계측기 API — base 게이트웨이 경유 /api/v1/tester/* (oam-cims-tester 가 서빙).
import { api } from '@core/api/client'

export interface TesterHealth {
  module: string
  version: string
  data_dir: string
  scenarios: number
  profiles: number
  topologies: number
  runs: number
  workers: number
  stream_subscribers: number
  phase: string
}

export interface ScenarioRow {
  id: string
  title: string | null
  tags: string[]
  steps: number
  source: 'bundled' | 'user'
  path: string
  errors: string[]
}

export interface ProfileRow {
  name: string
  model: string | null
  source: 'bundled' | 'user'
  path: string
  errors: string[]
}

export interface TopologyRow {
  id: number
  name: string
  created_at: string
  updated_at: string
  doc: Record<string, unknown>
}

export type Verdict = 'running' | 'pass' | 'fail' | 'aborted' | 'error'

export interface RunRow {
  id: string
  scenario_id: string
  topology: string
  profile?: string
  started_at: string
  ended_at?: string
  verdict: Verdict
  workers?: string[]
  summary?: Record<string, number | string | null>
}

export const testerApi = {
  health: () => api.get<TesterHealth>('/tester/health'),
  scenarios: () => api.get<{ scenarios: ScenarioRow[] }>('/tester/scenarios'),
  scenario: (id: string) => api.get<{ id: string; doc: Record<string, unknown>; errors: string[]; valid: boolean }>(`/tester/scenarios/${encodeURIComponent(id)}`),
  profiles: () => api.get<{ profiles: ProfileRow[] }>('/tester/profiles'),
  topologies: () => api.get<{ topologies: TopologyRow[] }>('/tester/topologies'),
  runs: (limit = 100) => api.get<{ runs: RunRow[] }>(`/tester/runs?limit=${limit}`),
  startRun: (body: { scenario_id: string; profile?: string; topology_id?: number }) =>
    api.post<RunRow>('/tester/runs', body),
}

// ── 라이브 스트림 — fetch + ReadableStream (widgets/useAlarms.ts 와 같은 방식) ─────────────
// EventSource 는 Authorization 헤더를 못 붙이므로 쓰지 않는다. 게이트웨이는 Accept: text/event-stream
// 요청을 총 타임아웃 없이 업스트림 SSE 로 청크 통과시킨다.
export interface TesterStreamFrame {
  stream: 'hello' | 'runs' | 'workers' | 'agg'
  record: Record<string, unknown>
}

export function openTesterStream(
  onFrame: (f: TesterStreamFrame) => void,
  onState: (s: 'connecting' | 'open' | 'closed', detail?: string) => void,
): () => void {
  const ctrl = new AbortController()
  let stopped = false
  ;(async () => {
    while (!stopped) {
      const token = (() => { try { return localStorage.getItem('cims_token') } catch { return null } })()
      if (!token) { onState('closed', '로그인 필요'); return }
      onState('connecting')
      try {
        const res = await fetch('/api/v1/tester/events', {
          headers: { Authorization: `Bearer ${token}`, Accept: 'text/event-stream' },
          signal: ctrl.signal,
        })
        if (!res.ok || !res.body) { onState('closed', `HTTP ${res.status}`); }
        else {
          onState('open')
          const reader = res.body.getReader()
          const dec = new TextDecoder()
          let buf = ''
          for (;;) {
            const { value, done } = await reader.read()
            if (done) break
            buf += dec.decode(value, { stream: true })
            let idx
            while ((idx = buf.indexOf('\n\n')) >= 0) {
              const chunk = buf.slice(0, idx); buf = buf.slice(idx + 2)
              for (const line of chunk.split('\n')) {
                if (!line.startsWith('data:')) continue
                try { onFrame(JSON.parse(line.slice(5).trim()) as TesterStreamFrame) } catch { /* 무시 */ }
              }
            }
          }
          onState('closed', '서버 종료')
        }
      } catch (e) {
        if (stopped) return
        onState('closed', String(e))
      }
      // 재연결 backoff
      await new Promise(r => setTimeout(r, 3000))
    }
  })()
  return () => { stopped = true; ctrl.abort(); onState('closed', '해제') }
}
