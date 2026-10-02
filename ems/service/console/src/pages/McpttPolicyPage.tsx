import { useCallback, useEffect, useState } from 'react'
import { mcpttApi, type McpttServiceConfig } from '@core/api/mcptt'
import { useToast } from '@core/components/Toast'
import { InfoDot } from '@core/components/InfoDot'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { Button } from '@core/components/ui/button'
import { Input } from '@core/components/ui/input'

// ── MCPTT 정책 (TS 24.484 §8.4 service-config) ──────────────────────────────
//  시스템 전역 서비스 설정 1건. 인가(1:1·긴급·경보·그룹 생성)는 이 문서에 없다 — 가입자 화면의 user-profile(사람별)과
//  PTT 그룹 편집(그룹 능력)이 정본이다. 여기 값은 N2 기본값·N6 두 값(관제/그 밖)과 broadcast-group 계층 수이고, floor 타이머·
//  Resource-Priority 는 CSC 배포 설정(ServiceConfig.*)이다.

const NUMBERS: { key: keyof McpttServiceConfig; label: string; tag: string; min: number; max: number; desc: string }[] = [
  { key: 'max_affiliations_n2', label: '동시 제휴 상한(N2)', tag: 'user-profile MaxAffiliationsN2', min: 1, max: 1000,
    desc: '한 사용자가 동시에 제휴(편성)할 수 있는 채널 수 — 각 가입자 user-profile 에 실린다. 집행은 서버가 하고, 단말은 초과를 로그로만 남긴다.' },
  { key: 'max_calls_n6', label: '동시 그룹 호 상한(N6)', tag: 'user-profile MaxSimultaneousCallsN6', min: 1, max: 255,
    desc: '한 사용자가 동시에 받는 그룹 호 수 — 관제 역할이 없는 사용자의 값. 넘는 개시·합류는 서버가 486(Warning 103)으로 거절한다.' },
  { key: 'max_calls_n6_dispatch', label: '동시 그룹 호 상한(N6) — 관제', tag: 'user-profile MaxSimultaneousCallsN6', min: 1, max: 255,
    desc: '역할이 배정된 사용자(관제사)의 값. 판정은 역할 배정 하나다 — 배정·해제하면 그 사람의 PTT 회선 문서가 바로 바뀐다.' },
  { key: 'num_levels_group_hierarchy', label: '그룹 계층 깊이', tag: 'common/broadcast-group/num-levels-group-hierarchy', min: 1, max: 10,
    desc: '브로드캐스트 그룹 계층 최대 깊이.' },
  { key: 'num_levels_user_hierarchy', label: '사용자 계층 깊이', tag: 'common/broadcast-group/num-levels-user-hierarchy', min: 1, max: 10,
    desc: '브로드캐스트 사용자 계층 최대 깊이.' },
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

  const dirty = !!form && !!cfg && NUMBERS.some(f => form[f.key] !== cfg[f.key])

  const save = async () => {
    if (!form) return
    setSaving(true)
    try {
      const body: Partial<McpttServiceConfig> = {}
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

  if (!form) return <div className="p-4 text-muted-foreground">불러오는 중…</div>

  return (
    <div className="p-4 max-w-[860px] flex flex-col gap-4">
      <div>
        <h2 className="mt-0 mx-0 mb-1 flex items-center gap-1.5">
          MCPTT 정책
          {/* 화면의 뜻은 한 번 읽으면 되는 설명이라 ⓘ 로 접는다 — 상태(아래)는 매번 봐야 하므로 남긴다. */}
          <InfoDot label="MCPTT 정책이란?">
            시스템 전역 서비스 설정(TS 24.484 §8.4 <code>service-config</code>)입니다. 기능 허용(1:1·긴급·경보·그룹 생성)은
            이 문서가 아니라 <b>사용자별 인가</b>(가입자 &gt; PTT &gt; 프로파일)와 <b>그룹 능력</b>(PTT 그룹 편집)에서 정합니다.
            floor 타이머·Resource-Priority 는 CSC 배포 설정(<code>ServiceConfig.*</code>)입니다.
          </InfoDot>
        </h2>
        <div className="text-muted-foreground text-md leading-[1.6]">
          {cfg && !cfg.exists && <span className="text-warning">DB 행이 없어 기본값을 표시합니다(저장하면 생성됩니다). </span>}
          {cfg?.update_time && <span>최근 변경 {new Date(cfg.update_time).toLocaleString()}</span>}
        </div>
      </div>

      <section className="flex flex-col gap-2">
        {NUMBERS.map(f => (
          <div className="flex items-start gap-2.5 py-2.5 px-3 border border-border rounded-sm" key={String(f.key)} title={f.tag}>
            <Input className="w-[90px]" type="number" min={f.min} max={f.max} disabled={!canEdit}
               value={Number(form[f.key])}
              onChange={e => setForm({ ...form, [f.key]: Number(e.target.value) })}/>
            <span className="flex flex-col gap-0.5 min-w-0">
              <span className="font-semibold">{f.label}
                <code className="ml-2 text-xs text-muted-foreground">{f.tag}</code>
                <span className="ml-2 text-xs text-muted-foreground">({f.min}~{f.max})</span>
              </span>
              <span className="text-sm text-muted-foreground">{f.desc}</span>
            </span>
          </div>
        ))}
      </section>

      <div className="flex gap-2 items-center">
        <Button variant="default" disabled={!canEdit || !dirty || saving} onClick={save}>
          {saving ? '저장 중…' : '저장'}
        </Button>
        <Button variant="ghost" disabled={!dirty || saving} onClick={() => cfg && setForm(cfg)}>
          되돌리기
        </Button>
        {!canEdit && <span className="text-sm text-muted-foreground">변경 권한이 없습니다(manager 이상).</span>}
      </div>

      <div className="text-sm text-muted-foreground leading-[1.6]">
        저장하면 XCAP <code>service-config</code> 문서가 즉시 새 값·새 ETag 로 바뀌고, 서버가 cms
        구독 중인 전 단말에 변경을 push 해 곧바로 재조회·반영됩니다. 구독이 없는 단말은 채널 목록
        갱신·재로그인 계기에 반영됩니다.
      </div>
    </div>
  )
}
