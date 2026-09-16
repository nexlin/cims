// YAML/JSON 문서 편집기 — textarea + 검증 결과 옆칸 (AutoDeployPage 의 편집기와 같은 짜임).
// 검증은 컨트롤러 POST /validate(pydantic 스키마) — 타이핑 뒤 600 ms 멈추면 호출한다. 저장은 호출부.
import { useEffect, useRef, useState } from 'react'
import { CircleCheck, CircleX } from 'lucide-react'
import { testerApi } from '@tester/api/tester'

export default function YamlEditor({ value, onChange, kind, disabled, placeholder, minHeight = 320, mode = 'yaml', onValid }: {
  value: string
  onChange: (v: string) => void
  kind: 'scenario' | 'profile' | 'topology'
  disabled?: boolean
  placeholder?: string
  minHeight?: number
  /** yaml = 원문을 그대로 검증 / json = JSON.parse 뒤 doc 으로 검증 (토폴로지) */
  mode?: 'yaml' | 'json'
  onValid?: (ok: boolean) => void
}) {
  const [issues, setIssues] = useState<string[]>([])
  const [checked, setChecked] = useState<'idle' | 'checking' | 'ok' | 'bad'>('idle')
  const timer = useRef<number | null>(null)
  const seq = useRef(0)

  useEffect(() => {
    if (timer.current) window.clearTimeout(timer.current)
    if (!value.trim()) { setIssues([]); setChecked('idle'); onValid?.(false); return }
    const my = ++seq.current
    setChecked('checking')
    timer.current = window.setTimeout(async () => {
      try {
        let res: { ok: boolean; errors: string[] }
        if (mode === 'json') {
          let doc: unknown
          try { doc = JSON.parse(value) } catch (e) { res = { ok: false, errors: [`JSON 파싱 실패: ${String(e)}`] }; doc = undefined }
          res = doc === undefined ? res! : await testerApi.validate(kind, { doc })
        } else {
          res = await testerApi.validate(kind, { yaml: value })
        }
        if (my !== seq.current) return
        setIssues(res.errors); setChecked(res.ok ? 'ok' : 'bad'); onValid?.(res.ok)
      } catch (e) {
        if (my !== seq.current) return
        setIssues([String(e)]); setChecked('bad'); onValid?.(false)
      }
    }, 600)
    return () => { if (timer.current) window.clearTimeout(timer.current) }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [value, kind, mode])

  return (
    <div className="flex flex-col overflow-hidden rounded-md border border-border bg-card">
      <div className="flex min-h-0 flex-1">
        <textarea value={value} onChange={e => onChange(e.target.value)} disabled={disabled} placeholder={placeholder}
                  spellCheck={false} wrap="off"
                  className="flex-1 resize-y border-0 bg-transparent p-2 font-mono text-sm leading-[18px] text-foreground outline-none disabled:text-muted-foreground"
                  style={{ minHeight }} />
        <div className="flex w-[260px] flex-none flex-col gap-1 overflow-auto border-l border-border p-2 text-xs">
          <div className="flex items-center gap-1.5 font-medium">
            {checked === 'ok' && <><CircleCheck size={13} className="text-success" /> 스키마 검증 통과</>}
            {checked === 'bad' && <><CircleX size={13} className="text-destructive" /> 검증 실패 {issues.length}건</>}
            {checked === 'checking' && <span className="text-muted-foreground">검증 중…</span>}
            {checked === 'idle' && <span className="text-muted-foreground">내용을 입력하면 검증합니다</span>}
          </div>
          {issues.map((i, n) => <div key={n} className="whitespace-pre-wrap break-all text-destructive">{i}</div>)}
        </div>
      </div>
    </div>
  )
}
