// SIP 사다리 드로어 — 실패 이벤트 행에서 연다. GET /runs/{id}/sip/{call_id} = 그 Call-ID 의 실패 이벤트 + SIP 덤프(있을 때).
// 워커 SIP 덤프 이전은 후속이라 지금은 이벤트 열과 덤프 텍스트만 — 사다리는 덤프의 요청/응답 첫 줄을 방향 화살표로 그린다.
import { useEffect, useMemo, useState } from 'react'
import { X } from 'lucide-react'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { testerApi, type CallDump } from '@tester/api/tester'
import { fmtUnix, fmtNum } from '@tester/lib/fmt'

function ladder(dump: string): { dir: 'out' | 'in'; line: string }[] {
  // 덤프 규약(후속 워커): 메시지 블록 앞 줄 `>>> ` 송신 / `<<< ` 수신, 첫 줄 = 요청/상태 줄
  const out: { dir: 'out' | 'in'; line: string }[] = []
  for (const raw of dump.split('\n')) {
    const m = raw.match(/^(>>>|<<<)\s*(.*)$/)
    if (m) out.push({ dir: m[1] === '>>>' ? 'out' : 'in', line: m[2] })
  }
  return out
}

export default function SipDrawer({ runId, callId, onClose }: { runId: string; callId: string; onClose: () => void }) {
  const [doc, setDoc] = useState<CallDump | null>(null)
  const [err, setErr] = useState<string | null>(null)
  useEffect(() => {
    let alive = true
    setDoc(null); setErr(null)
    testerApi.callDump(runId, callId).then(d => { if (alive) setDoc(d) }).catch(e => { if (alive) setErr(String(e)) })
    return () => { alive = false }
  }, [runId, callId])
  const rungs = useMemo(() => (doc?.dump ? ladder(doc.dump) : []), [doc])
  return (
    <div className="fixed inset-y-0 right-0 z-[90] flex w-[min(560px,100vw)] flex-col border-l border-border bg-background shadow-lg" style={{ top: 'var(--header-h, 48px)' }}>
      <div className="flex items-center gap-2 border-b border-border px-3 py-2">
        <span className="text-sm font-semibold">SIP 사다리</span>
        <span className="truncate font-mono text-xs text-muted-foreground">{callId}</span>
        <Button variant="ghost" size="iconSm" className="ml-auto" onClick={onClose} aria-label="닫기"><X size={14} /></Button>
      </div>
      <div className="min-h-0 flex-1 overflow-auto p-3 text-xs">
        {err && <div className="text-destructive">{err}</div>}
        {doc && (
          <>
            {doc.events.length > 0 && (
              <div className="mb-3 flex flex-col gap-1">
                <div className="font-semibold text-muted-foreground">실패 이벤트 ({doc.events.length})</div>
                {doc.events.map((e, i) => (
                  <div key={i} className="flex flex-wrap items-center gap-1.5 rounded-sm border border-border px-2 py-1">
                    <span className="font-mono">{fmtUnix(e.t)}</span><span>{e.worker}</span>
                    {e.role && <Badge variant="neutralSoft">{e.role}</Badge>}{e.step && <Badge variant="infoSoft">{e.step}</Badge>}
                    {e.code != null && <Badge variant={e.code >= 500 ? 'dangerSoft' : 'warningSoft'}>{e.code}</Badge>}
                    {e.metric && <span className="font-mono">{e.metric}={fmtNum(e.observed)}</span>}
                    <span className="w-full break-all text-muted-foreground">{e.detail}</span>
                  </div>
                ))}
              </div>
            )}
            {rungs.length > 0 && (
              <div className="mb-3">
                <div className="mb-1 font-semibold text-muted-foreground">사다리 <span className="font-normal">워커 ⇄ 대상</span></div>
                <div className="flex flex-col gap-0.5">
                  {rungs.map((r, i) => (
                    <div key={i} className={`flex items-center gap-2 font-mono ${r.dir === 'out' ? '' : 'flex-row-reverse text-right'}`}>
                      <span className={r.dir === 'out' ? 'text-primary' : 'text-success'}>{r.dir === 'out' ? '→' : '←'}</span>
                      <span className="flex-1 truncate">{r.line}</span>
                    </div>
                  ))}
                </div>
              </div>
            )}
            {doc.dump ? <pre className="whitespace-pre-wrap rounded-sm border border-border bg-muted p-2 font-mono">{doc.dump}</pre>
              : <div className="text-muted-foreground">{doc.note ?? 'SIP 덤프 없음'}</div>}
          </>
        )}
      </div>
    </div>
  )
}
