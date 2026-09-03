import { useCallback, useEffect, useState } from 'react'
import { mcpttApi, type McpttServiceConfig } from '@core/api/mcptt'
import { useToast } from '@core/components/Toast'
import { InfoDot } from '@core/components/InfoDot'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'

// ── MCPTT 정책 (TS 24.484 service-config) ────────────────────────────────────
//  시스템 전역 1건. 단말이 XCAP 으로 이 문서를 받아 **시스템 정책 게이트**로 쓰고, 사용자별 인가
//  (user-profile — 가입자 화면)와 AND 로 판정한다. 즉 여기서 끈 기능은 프로파일이 허용해도 열리지
//  않는다. 최종 판정은 서버(403 / Floor Deny)이며 단말 게이트는 UX 선차단이다.

/** 편집 항목 — 규격 태그명을 title 로 노출해 문서(TS 24.484)와 대조 가능하게 한다. */
const TOGGLES: { key: keyof McpttServiceConfig; label: string; tag: string; desc: string }[] = [
  { key: 'allow_private_call',       label: '1:1 통화',      tag: 'allow-private-call',
    desc: '1:1(private call) 발신. 착신은 막지 않는다 — 서버가 성립시킨 세션은 받는다.' },
  { key: 'allow_emergency_call',     label: '긴급통화',      tag: 'allow-emergency-call',
    desc: '긴급 그룹통화 개시. 사용자 프로파일의 개시 인가와 AND.' },
  { key: 'allow_alert',              label: '긴급경보',      tag: 'allow-alert',
    desc: '긴급경보 발신. 이미 걸린 경보의 취소는 항상 허용된다.' },
  { key: 'allow_transmit_request',   label: '발언권 요청',   tag: 'on-network/allow-transmit-request',
    desc: '키업(floor 요청). 끄면 단말이 요청을 보내지 않고 거부음으로 알린다.' },
  { key: 'allow_create_delete_group', label: '그룹 생성/삭제', tag: 'allow-create-delete-group',
    desc: '사용자에 의한 그룹 생성·삭제 허용 여부(관리자 편성과 별개).' },
]

const NUMBERS: { key: keyof McpttServiceConfig; label: string; tag: string; min: number; max: number; desc: string }[] = [
  { key: 'max_affiliations_n2', label: '동시 제휴 상한 N2', tag: 'max-affiliations-N2', min: 1, max: 1000,
    desc: '한 사용자가 동시에 제휴(편성)할 수 있는 채널 수. 집행은 서버가 하고, 단말은 초과를 로그로만 남긴다.' },
  { key: 'num_levels_group_hierarchy', label: '그룹 계층 깊이', tag: 'num-levels-group-hierarchy', min: 1, max: 10,
    desc: '그룹 계층(regroup) 최대 깊이.' },
  { key: 'num_levels_user_hierarchy', label: '사용자 계층 깊이', tag: 'num-levels-user-hierarchy', min: 1, max: 10,
    desc: '사용자 계층 최대 깊이.' },
]

export default function McpttPolicyPage() {
  const { show } = useToast()
  const { user: me } = useAuth()
  const canEdit = hasRole(me, 'manager')

  const [cfg, setCfg] = useState<McpttServiceConfig | null>(null)
  const [form, setForm] = useState<McpttServiceConfig | null>(null)
  const [saving, setSaving] = useState(false)

  const load = useCallback(() => {
    mcpttApi.getServiceConfig()
      .then(r => { setCfg(r); setForm(r) })
      .catch(e => show(`정책 조회 실패: ${e.message}`, 'err'))
  }, [show])

  useEffect(() => { load() }, [load])

  const dirty = !!form && !!cfg && TOGGLES.concat(NUMBERS as never[])
    .some(f => form[f.key] !== cfg[f.key])

  const save = async () => {
    if (!form) return
    setSaving(true)
    try {
      const body: Partial<McpttServiceConfig> = {}
      for (const f of TOGGLES) (body as Record<string, unknown>)[f.key] = !!form[f.key]
      for (const f of NUMBERS) (body as Record<string, unknown>)[f.key] = Number(form[f.key])
      const r = await mcpttApi.updateServiceConfig(body)
      setCfg(r); setForm(r)
      show('MCPTT 정책을 저장했습니다', 'ok')
    } catch (e) {
      show(`저장 실패: ${(e as Error).message}`, 'err')
    } finally {
      setSaving(false)
    }
  }

  if (!form) return <div style={{ padding: 16, color: 'var(--text-muted)' }}>불러오는 중…</div>

  return (
    <div style={{ padding: 16, maxWidth: 860, display: 'flex', flexDirection: 'column', gap: 16 }}>
      <div>
        <h2 style={{ margin: '0 0 4px', display: 'flex', alignItems: 'center', gap: 6 }}>
          MCPTT 정책
          {/* 화면의 뜻은 한 번 읽으면 되는 설명이라 ⓘ 로 접는다 — 상태(아래)는 매번 봐야 하므로 남긴다. */}
          <InfoDot label="MCPTT 정책이란?">
            시스템 전역 서비스 설정(TS 24.484 <code>service-config</code>)입니다. 단말이 XCAP 으로 받아
            기능 게이트로 쓰며, <b>사용자별 인가</b>(가입자 &gt; PTT &gt; 프로파일)와 <b>AND</b> 로 판정합니다 —
            여기서 끈 기능은 프로파일이 허용해도 열리지 않습니다.
          </InfoDot>
        </h2>
        <div style={{ color: 'var(--text-muted)', fontSize: 13, lineHeight: 1.6 }}>
          {cfg && !cfg.exists && <span style={{ color: 'var(--warning)' }}>DB 행이 없어 기본값을 표시합니다(저장하면 생성됩니다). </span>}
          {cfg?.update_time && <span>최근 변경 {new Date(cfg.update_time).toLocaleString()}</span>}
        </div>
      </div>

      <section style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
        {TOGGLES.map(f => (
          <label key={String(f.key)} title={f.tag}
            style={{ display: 'flex', alignItems: 'flex-start', gap: 10, padding: '10px 12px',
                     border: '1px solid var(--border)', borderRadius: 6 }}>
            <input type="checkbox" style={{ marginTop: 3 }} disabled={!canEdit}
              checked={!!form[f.key]}
              onChange={e => setForm({ ...form, [f.key]: e.target.checked })} />
            <span style={{ display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 }}>
              <span style={{ fontWeight: 600 }}>{f.label}
                <code style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)' }}>{f.tag}</code>
              </span>
              <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{f.desc}</span>
            </span>
          </label>
        ))}
      </section>

      <section style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
        {NUMBERS.map(f => (
          <div key={String(f.key)} title={f.tag}
            style={{ display: 'flex', alignItems: 'flex-start', gap: 10, padding: '10px 12px',
                     border: '1px solid var(--border)', borderRadius: 6 }}>
            <input className="form-input" type="number" min={f.min} max={f.max} disabled={!canEdit}
              style={{ width: 90 }} value={Number(form[f.key])}
              onChange={e => setForm({ ...form, [f.key]: Number(e.target.value) })} />
            <span style={{ display: 'flex', flexDirection: 'column', gap: 2, minWidth: 0 }}>
              <span style={{ fontWeight: 600 }}>{f.label}
                <code style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)' }}>{f.tag}</code>
                <span style={{ marginLeft: 8, fontSize: 11, color: 'var(--text-muted)' }}>({f.min}~{f.max})</span>
              </span>
              <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>{f.desc}</span>
            </span>
          </div>
        ))}
      </section>

      <div style={{ display: 'flex', gap: 8, alignItems: 'center' }}>
        <button className="btn btn--sm btn--primary" disabled={!canEdit || !dirty || saving} onClick={save}>
          {saving ? '저장 중…' : '저장'}
        </button>
        <button className="btn btn--sm btn--ghost" disabled={!dirty || saving} onClick={() => cfg && setForm(cfg)}>
          되돌리기
        </button>
        {!canEdit && <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>변경 권한이 없습니다(manager 이상).</span>}
      </div>

      <div style={{ fontSize: 12, color: 'var(--text-muted)', lineHeight: 1.6 }}>
        저장하면 XCAP <code>service-config</code> 문서가 즉시 새 값·새 ETag 로 바뀌고, 서버가 cms
        구독 중인 전 단말에 변경을 push 해 곧바로 재조회·반영됩니다. 구독이 없는 단말은 채널 목록
        갱신·재로그인 계기에 반영됩니다.
      </div>
    </div>
  )
}
