// 알람 원문 — 모듈이 보낸 값 중 `*_raw`(한국어 구절로 옮기기 전 원문)를 상세 보기에 그대로 보인다.
//   감시창 메시지는 한국어 구절(예: 「디스크 공간 부족」)이고, 원문(예: `write failed: No space
//   left on device`)은 여기서 복사해 검색한다 (alarm_catalog.md `message` 규칙, alarm_pipeline.md §8.2).
import { useState } from 'react'
import { Button } from '@core/components/ui/button'
import { copyText } from '../utils/clipboard'

function RawLine({ label, text }: { label: string; text: string }) {
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    if (await copyText(text)) { setCopied(true); setTimeout(() => setCopied(false), 1200) }
  }
  return (
    <div className="flex gap-2 items-center text-sm">
      <span className="text-muted-foreground min-w-[90px] shrink-0">{label}</span>
      <code className="font-mono text-xs break-all select-all">{text}</code>
      <Button variant="ghost" onClick={copy}>{copied ? '복사됨' : '복사'}</Button>
    </div>
  )
}

// 원문 항목([키, 값]) — 상세 표시와 검색 칸이 같은 기준을 쓴다.
function rawEntries(params?: Record<string, unknown>): [string, unknown][] {
  return Object.entries(params || {}).filter(([k, v]) => k.endsWith('_raw') && v != null && String(v) !== '')
}

/** 검색 대상 원문 값 — 감시창 검색 칸이 원문을 붙여 넣어도 그 알람을 찾게 한다. */
export function alarmRawValues(params?: Record<string, unknown>): string[] {
  return rawEntries(params).map(([, v]) => String(v))
}

export default function AlarmRawText({ params }: { params?: Record<string, unknown> }) {
  const raws = rawEntries(params)
  if (raws.length === 0) return null
  return (
    <>
      {raws.map(([k, v]) => (
        <RawLine key={k} label={raws.length > 1 ? `원문 · ${k.slice(0, -4)}` : '원문'} text={String(v)} />
      ))}
    </>
  )
}
