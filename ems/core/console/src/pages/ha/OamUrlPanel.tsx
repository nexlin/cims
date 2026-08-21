// ──────────────────────────────────────────────────────────────
//  OamUrlPanel — 이 서버 agent 가 OAM 에 접속(보고)하는 주소
//
//  agent 는 2초마다 `POST {oam_url}/api/agent/heartbeat` 로 자기 상태를, job 이 끝나면
//  `/api/agent/report` 로 결과를 보낸다. 즉 **보내는 주체가 agent** 이므로 이 주소는
//  그 서버 agent 의 설정이고, 값도 그 노드에 있다(`<state-dir>/oam_url`). 그래서 여기
//  — 서버 단위 인프라 화면 — 에 둔다.
//
//  이중화에서 이 값이 **구 Active 의 노드 IP** 면, 절체 자체는 정상 동작하지만(HA 판정은
//  노드 로컬이라 무관) 신 Active 가 heartbeat 를 한 건도 받지 못해 **콘솔이 fleet 을 잃는다**
//  — 전 노드 offline, 모듈 상태는 절체 직전 값으로 고착(실측). 그래서 VIP 여야 한다.
//
//  적용은 agent 가 새 주소로 `/health` **도달 확인 후에만** 한다 — 도달 불가면 주소를
//  바꾸지 않고 job 이 실패하므로, VIP 가 아직 없을 때 눌러도 fleet 이 끊기지 않는다.
//  설계: docs/design/features/oam_ha.md §8·§9.4.1
// ──────────────────────────────────────────────────────────────

import { useState } from 'react'
import { ImeSafeInput } from './ImeSafeInput'

export function OamUrlPanel({ title, current, vipCandidate, applying, onApply, onApplyAll }: {
  title: string
  /** heartbeat 로 보고된 **실제** 접속 주소 (구 버전 agent 는 없음) */
  current?: string | null
  /** 이 서버가 속한 관리평면 그룹의 VIP 주소 (있으면 권장값으로 제시) */
  vipCandidate?: string | null
  applying?: boolean
  onApply: (url: string) => void | Promise<void>
  /** 전 agent 에 같은 주소 적용 — 같은 설정의 대량 편집 */
  onApplyAll?: (url: string) => void | Promise<void>
}) {
  const cur = (current || '').trim()
  const suggested = vipCandidate ? `https://${vipCandidate}:4419` : ''
  const [draft, setDraft] = useState(cur || suggested)
  const norm = draft.trim().replace(/\/+$/, '')
  const valid = /^https?:\/\/[^/\s]+/.test(norm)
  const mismatch = !!suggested && !!cur && cur.replace(/\/+$/, '') !== suggested
  const loopback = /^https?:\/\/(127\.|localhost)/i.test(cur)

  return (
    <div style={{ border: '1px solid #e0e0e0', borderRadius: 4, padding: 12 }}>
      <div style={{ fontWeight: 600, marginBottom: 8 }}>{title}</div>
      <div style={{ fontSize: 12, color: 'var(--text-muted)', lineHeight: 1.6, marginBottom: 10 }}>
        이 서버의 agent 가 heartbeat·job 결과를 보내는 주소입니다. 관리평면이 이중화면
        <b> VIP</b> 여야 합니다 — 노드 IP 로 두면 절체 후 이 agent 가 OAM 과 단절되고,
        콘솔에서는 이 서버가 offline 으로 보입니다.
      </div>
      <div style={{ fontSize: 12, marginBottom: 8 }}>
        현재 보고 주소:{' '}
        {cur ? (
          <code style={{ color: mismatch || loopback ? '#c0392b' : '#27ae60', fontWeight: 600 }}>
            {cur}
          </code>
        ) : (
          <span style={{ color: 'var(--text-muted)' }}>
            보고 없음 (heartbeat 대기 또는 구 버전 agent)
          </span>
        )}
        {loopback && (
          <span style={{ color: '#c0392b', marginLeft: 6 }}>
            — loopback 은 이 노드 자신의 OAM 을 가리킵니다(절체 시 끊김)
          </span>
        )}
        {!loopback && mismatch && (
          <span style={{ color: '#c0392b', marginLeft: 6 }}>— VIP 가 아닙니다</span>
        )}
      </div>
      <div style={{ display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap' }}>
        <ImeSafeInput value={draft} onCommit={setDraft}
                      placeholder={suggested || 'https://<OAM 또는 VIP>:4419'}
                      style={{ width: 300, fontSize: 12, padding: '3px 6px',
                               fontFamily: 'monospace' }} />
        {suggested && draft.trim() !== suggested && (
          <button className="btn btn--sm" onClick={() => setDraft(suggested)}
                  title="이 서버가 속한 관리평면 그룹의 VIP">VIP 채우기</button>
        )}
        <button className="btn btn--sm btn--primary"
                disabled={!!applying || !valid || norm === cur}
                onClick={() => onApply(norm)}
                title="이 서버 agent 만 변경 — agent 가 새 주소로 /health 도달 확인 후 적용">
          이 서버 적용
        </button>
        {onApplyAll && (
          <button className="btn btn--sm" disabled={!!applying || !valid}
                  onClick={() => {
                    if (!window.confirm(
                        `전 agent 의 OAM 접속 주소를 아래로 바꿉니다.\n\n  ${norm}\n\n` +
                        `각 agent 가 그 주소로 /health 도달을 확인한 뒤에만 적용합니다 — ` +
                        `도달 불가면 주소를 바꾸지 않고 실패로 남습니다(fleet 단절 방지).\n\n` +
                        `진행할까요?`)) return
                    onApplyAll(norm)
                  }}
                  title="같은 주소를 전 agent 에 일괄 적용 (CSP/CMP 등 모든 노드 포함)">
            전체 적용
          </button>
        )}
      </div>
      {!valid && draft.trim() !== '' && (
        <div style={{ color: '#c0392b', fontSize: 11, marginTop: 6 }}>
          http(s)://호스트[:포트] 형식이어야 합니다.
        </div>
      )}
      <div style={{ fontSize: 11, color: 'var(--text-muted)', marginTop: 8, lineHeight: 1.6 }}>
        적용하면 agent 가 재기동되어 새 주소로 붙습니다(수 초). 새로 설치되는 agent 가 받는
        초기 주소는 별도로 <b>oam 설정 &gt; Agent→OAM URL</b> 이 정합니다.
      </div>
    </div>
  )
}
