// ──────────────────────────────────────────────────────────────
//  SharedStorePanel — 관리평면 데이터(공유 store) 위치 설정
//
//  관리평면(OAM)은 서버·그룹·배포·계정을 **파일**로 들고 있다. 두 노드가 각자 자기 디스크에
//  들고 있으면 절체 시 새 Active 가 빈 데이터를 보여주므로, 그 파일들을 **양 노드가 같이
//  마운트하는 공유 스토리지**(NAS)에 두고 VIP 를 가진 노드만 소유권 리스로 write 한다.
//  마운트 자체는 여기서 만들지 않는다 — 서버별 [마운트 관리]가 fstab 에 영속시킨다.
//  설계: docs/design/features/oam_ha.md §4
// ──────────────────────────────────────────────────────────────

import { useState } from 'react'
import { btnPrimary, btnSecondary } from './styles'
import type { ServiceRow } from './types'
import type { HaSharedStore } from '../../api/ha_groups'

const EMPTY: HaSharedStore = { mount_point: '' }

/** 서버가 적용하는 조건과 동일한 검증 — 어긋나면 "미사용" 으로 정규화된다. */
function validate(v: HaSharedStore): string[] {
  const mp = v.mount_point || ''
  if (!mp.startsWith('/') || mp.includes('..')) return ['마운트 지점: 절대경로 (.. 불가)']
  return []
}

/** 모든 멤버에 **공통으로** 존재하는 마운트 지점만 후보다 — 한쪽에만 있으면 절체가 깨진다. */
function commonMounts(svc: ServiceRow): Array<{ target: string; fstype: string }> {
  const lists = (svc.servers || []).map(s => s.mountTargets || [])
  if (!lists.length || lists.some(l => l.length === 0)) return []
  const first = lists[0]
  return first
    .filter(m => lists.every(l => l.some(x => x.target === m.target)))
    .map(m => ({ target: m.target, fstype: m.fstype }))
}

export function SharedStorePanel({ svc, onChange, onMigrate }: {
  svc: ServiceRow
  onChange: (v: HaSharedStore | Record<string, never>) => void
  /** 이관 실행 — 경로 저장 + oam 설정 갱신 + 정지/복사/기동까지 서버가 처리. */
  onMigrate?: (mountPoint: string) => void
}) {
  const cur = svc.sharedStore
  const [draft, setDraft] = useState<HaSharedStore>(cur ? { ...EMPTY, ...cur } : EMPTY)
  const [dirty, setDirty] = useState(false)
  const errs = validate(draft)
  const lbl = { fontSize: 12, color: 'var(--text-muted)', display: 'block', marginBottom: 2 }
  const inp = { width: '100%', fontSize: 12, padding: '3px 6px' }

  const excluded = Object.entries(svc.haExcluded || {})
  const mounts = commonMounts(svc)
  return (
    <div style={{ fontSize: 12 }}>
      <div style={{ color: 'var(--text-muted)', marginBottom: 8, lineHeight: 1.6 }}>
        관리평면(OAM) 데이터를 <b>양 노드가 같이 마운트한 공유 경로</b>에 두고, VIP 를 가진
        노드만 씁니다(소유권 리스). 미설정이면 데이터가 노드별로 따로 있어 이중화되지 않습니다.
        <br />
        마운트는 먼저 <b>시스템/인프라 &gt; 서버 &gt; 마운트 관리</b> 에서 양 노드에 같은 경로로
        추가하세요 — 여기서는 그 경로를 지정만 합니다.
      </div>
      {/* 선언 집행 결과 — 전제 미충족으로 HA 편입에서 빠진 모듈. 조용히 빠지면 운영자는
          이중화가 되는 줄 알기 때문에 여기서 이유를 명시한다. */}
      {excluded.length > 0 && (
        <div role="alert" style={{
          background: '#7c2d12', color: '#fff', padding: '8px 10px', borderRadius: 4,
          marginBottom: 10, lineHeight: 1.6,
        }}>
          <b>이 모듈은 이중화되지 않습니다</b> — {excluded.map(([m]) => m).join(', ')}
          <div style={{ marginTop: 4 }}>
            공유 store 가 없으면 관리 데이터가 노드마다 따로 존재해서, 절체하면 콘솔이 보는
            내용(서버·그룹·배포)이 통째로 바뀝니다. 그래서 <b>절체 대상에서 제외</b>했습니다.
            아래에 경로를 지정하면 자동으로 편입됩니다.
          </div>
        </div>
      )}
      <div style={{ maxWidth: 640 }}>
        <label style={lbl}>공유 마운트 지점 (양 노드 공통)</label>
        {/* 자유 입력 금지 — mount guard 는 /proc/mounts 와 **정확히 일치**하는 경로만
            통과시킨다. 하위 디렉터리를 적으면 OAM 이 기동을 거부해 콘솔이 사라진다
            (실측 사고). 그래서 agent 가 보고한 실제 마운트에서만 고르게 한다. */}
        {mounts.length > 0 ? (
          <select style={inp} value={draft.mount_point}
                  onChange={e => { setDraft({ mount_point: e.target.value }); setDirty(true) }}>
            <option value="">(선택)</option>
            {mounts.map(m => (
              <option key={m.target} value={m.target}>{m.target} ({m.fstype})</option>
            ))}
          </select>
        ) : (
          <>
            <input style={inp} value={draft.mount_point}
                   placeholder="/NAS"
                   onChange={e => { setDraft({ mount_point: e.target.value }); setDirty(true) }} />
            <div style={{ color: '#c0392b', marginTop: 4 }}>
              멤버 노드의 마운트 정보를 아직 못 받았습니다(agent heartbeat 대기). 직접
              입력하면 <b>실제 마운트 지점</b>이어야 합니다 — <code>findmnt</code> 로 확인하세요.
            </div>
          </>
        )}
        <div style={{ color: 'var(--text-muted)', marginTop: 6, lineHeight: 1.6 }}>
          관리 데이터는 이 마운트 <b>하위</b>(<code>{(draft.mount_point || '&lt;마운트&gt;')}/runtime</code>)에
          저장됩니다. 마운트 지점 자체가 <code>/proc/mounts</code> 에 있어야 하므로, NAS 안의
          하위 폴더(<code>…/oam_store</code> 같은)를 마운트 지점으로 지정하면 기동이 거부됩니다.
        </div>
      </div>
      {errs.length > 0 && dirty && (
        <ul style={{ color: '#c0392b', margin: '8px 0 0 16px', padding: 0 }}>
          {errs.map(e => <li key={e}>{e}</li>)}
        </ul>
      )}
      <div style={{ marginTop: 10, display: 'flex', gap: 6, alignItems: 'center', flexWrap: 'wrap' }}>
        <button style={btnPrimary()} disabled={!dirty || errs.length > 0}
                onClick={() => { onChange(draft); setDirty(false) }}>경로만 저장</button>
        {/* 이관까지 한 번에 — 관리 데이터는 지금 노드 로컬에 있으므로 경로만 바꾸면
            빈 콘솔이 된다. 서버(agent)가 정지 → 복사 → 기동 순서로 처리한다. */}
        {onMigrate && (
          <button style={btnPrimary()} disabled={errs.length > 0 || !draft.mount_point}
                  onClick={() => {
                    if (!confirm(
                      `관리 데이터를 이 경로로 이관합니다.\n\n` +
                      `  ${draft.mount_point}/runtime\n\n` +
                      `OAM 이 정지 → 복사 → 재기동되므로 콘솔이 30초 내외 끊깁니다.\n` +
                      `돌아오면 새 경로로 동작하고, oam/oam-svc 가 HA 편입됩니다.\n` +
                      `대상에 이전 데이터가 있으면 .stale-<시각> 으로 보관하고 덮어씁니다.\n\n` +
                      `진행할까요?`)) return
                    onMigrate(draft.mount_point)
                  }}>
            이 경로로 이관 (권장)
          </button>
        )}
        <button style={btnSecondary()}
                onClick={() => { onChange({}); setDraft(EMPTY); setDirty(false) }}>
          공유 store 사용 안 함
        </button>
        {cur?.mount_point && !dirty && (
          <span style={{ color: 'var(--text-muted)' }}>현재: {cur.mount_point}</span>
        )}
      </div>
      <div style={{ marginTop: 8, color: 'var(--text-muted)' }}>
        저장 후 oam 배포설정의 <code>런타임 store</code>(= 위 경로 하위 <code>/runtime</code>) 과
        <code> 런타임 마운트 지점</code>(= 위 경로) 을 함께 맞춰야 합니다 — 마운트 없이 OAM 이
        떠서 로컬 디스크에 빈 데이터를 만드는 것을 막는 guard 기준입니다.
      </div>
    </div>
  )
}
