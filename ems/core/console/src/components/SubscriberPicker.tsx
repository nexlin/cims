import { useMemo, useState, useEffect, useRef } from 'react'
import { usersApi, type UserSummary } from '../api/users'

// ── 가입자/번호 자동완성 피커 ─────────────────────────────────
//  PTT 그룹 멤버·착신전환 등에서 raw MSISDN 수기입력을 대체.
//  실제 등록된 가입자(번호)를 조직/이름/번호로 검색해 선택 → 오타·미검증 등록 방지.

export interface PickItem {
  /** 선택 결과 값 — kind='user' 면 user.id(문자열), 그 외엔 MSISDN */
  value: string
  label: string        // 표시: 이름
  sub: string          // 보조: 번호/조직
  orgCode: string
  userId: number
  userName: string
}

interface SubscriberPickerProps {
  /** ptt/call = 해당 서비스 번호 검색, user = 사용자(person) 검색 */
  kind: 'ptt' | 'call' | 'user'
  onPick: (item: PickItem) => void
  placeholder?: string
  /** code_path startsWith 필터 (조직 스코프) */
  orgScope?: string | null
  /** 이미 추가된 value 들 (목록에서 제외) */
  exclude?: Set<string>
  /** 외부에서 미리 만든 인덱스(워크벤치가 1회 fetch 후 공유). 미지정 시 내부 fetch */
  index?: PickItem[]
  /** 조직 code_path 조회용 (orgScope 비교). 미지정 시 orgCode 동등 비교만 */
  orgPathOf?: (orgCode: string) => string
  autoFocus?: boolean
}

/** usersApi.list 결과를 PickItem 배열로 평탄화 */
export function buildPickIndex(users: UserSummary[], kind: 'ptt' | 'call' | 'user'): PickItem[] {
  const out: PickItem[] = []
  for (const u of users) {
    if (kind === 'user') {
      out.push({ value: String(u.id), label: u.name, sub: u.org_id || '', orgCode: u.org_id || '', userId: u.id, userName: u.name })
      continue
    }
    const subs = kind === 'ptt' ? u.ptt_subscriptions : u.call_subscriptions
    for (const s of subs) {
      out.push({ value: s.id, label: u.name, sub: s.id, orgCode: u.org_id || '', userId: u.id, userName: u.name })
    }
  }
  return out
}

export default function SubscriberPicker({
  kind, onPick, placeholder, orgScope, exclude, index, orgPathOf, autoFocus,
}: SubscriberPickerProps) {
  const [q, setQ] = useState('')
  const [open, setOpen] = useState(false)
  const [internal, setInternal] = useState<PickItem[] | null>(null)
  const [activeIdx, setActiveIdx] = useState(0)
  const boxRef = useRef<HTMLDivElement>(null)

  // 내부 fetch (index 미제공 시 1회)
  useEffect(() => {
    if (index) return
    let alive = true
    usersApi.list().then(us => { if (alive) setInternal(buildPickIndex(us, kind)) }).catch(() => setInternal([]))
    return () => { alive = false }
  }, [index, kind])

  const all = useMemo(() => index ?? internal ?? [], [index, internal])

  // 외부 클릭 닫기
  useEffect(() => {
    function onDoc(e: MouseEvent) { if (boxRef.current && !boxRef.current.contains(e.target as Node)) setOpen(false) }
    document.addEventListener('mousedown', onDoc)
    return () => document.removeEventListener('mousedown', onDoc)
  }, [])

  const matches = useMemo(() => {
    const s = q.trim().toLowerCase()
    const inScope = (it: PickItem) => {
      if (!orgScope) return true
      const path = orgPathOf ? orgPathOf(it.orgCode) : it.orgCode
      return (path || '').startsWith(orgScope)
    }
    return all
      .filter(it => !exclude?.has(it.value))
      .filter(inScope)
      .filter(it => !s || it.label.toLowerCase().includes(s) || it.sub.toLowerCase().includes(s) || it.value.toLowerCase().includes(s))
      .slice(0, 30)
  }, [all, q, orgScope, exclude, orgPathOf])

  function choose(it: PickItem) {
    onPick(it)
    setQ('')
    setOpen(false)
    setActiveIdx(0)
  }

  return (
    <div ref={boxRef} style={{ position: 'relative', flex: 1, minWidth: 0 }}>
      <input
        className="form-input"
        style={{ width: '100%', fontSize: 12 }}
        placeholder={placeholder ?? (kind === 'user' ? '이름/조직 검색' : '이름·번호 검색')}
        value={q}
        autoFocus={autoFocus}
        onChange={e => { setQ(e.target.value); setOpen(true); setActiveIdx(0) }}
        onFocus={() => setOpen(true)}
        onKeyDown={e => {
          if (!open) return
          if (e.key === 'ArrowDown') { e.preventDefault(); setActiveIdx(i => Math.min(matches.length - 1, i + 1)) }
          else if (e.key === 'ArrowUp') { e.preventDefault(); setActiveIdx(i => Math.max(0, i - 1)) }
          else if (e.key === 'Enter') { e.preventDefault(); if (matches[activeIdx]) choose(matches[activeIdx]) }
          else if (e.key === 'Escape') setOpen(false)
        }}
      />
      {open && (
        <div style={{
          position: 'absolute', top: '100%', left: 0, right: 0, zIndex: 50,
          background: 'var(--card)', border: '1px solid var(--border)', borderRadius: 6,
          marginTop: 2, maxHeight: 240, overflowY: 'auto', boxShadow: '0 4px 16px rgba(0,0,0,0.18)',
        }}>
          {all.length === 0 ? (
            <div style={{ padding: 10, fontSize: 12, color: 'var(--muted-foreground)' }}>불러오는 중...</div>
          ) : matches.length === 0 ? (
            <div style={{ padding: 10, fontSize: 12, color: 'var(--muted-foreground)' }}>일치하는 가입자 없음</div>
          ) : matches.map((it, i) => (
            <div key={it.value + ':' + it.userId}
              onMouseDown={e => { e.preventDefault(); choose(it) }}
              onMouseEnter={() => setActiveIdx(i)}
              style={{
                display: 'flex', justifyContent: 'space-between', gap: 8, padding: '6px 10px', cursor: 'pointer', fontSize: 12,
                background: i === activeIdx ? 'rgba(74,144,217,0.12)' : undefined,
              }}>
              <span style={{ fontWeight: 500, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{it.label}</span>
              <span className="ts" style={{ color: 'var(--muted-foreground)', whiteSpace: 'nowrap' }}>
                {it.sub}{it.orgCode ? ` · ${it.orgCode}` : ''}
              </span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
