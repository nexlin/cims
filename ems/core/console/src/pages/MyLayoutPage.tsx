// 내 대시보드 구성 — 콘솔 D1 개인화 (oam_base_service_split §6).
//   프로파일 템플릿 선택 + 위젯 추가/제거/순서 → 서버(OAM /console/layouts/me) 저장.
//   카탈로그는 서버가 RBAC 로 필터(권한 밖 위젯은 애초에 안 옴) + 서비스 가용성(available)을
//   표기 — 미설치/불가 위젯은 "설치 후 사용 가능"으로 안내하고 추가 비활성(장애격리 UX, D7).
//   저장 PUT 도 서버가 RBAC 재검증(레이아웃이 보안 아님 — 심층방어).
import { useState, useEffect, useCallback, useMemo } from 'react'
import { useToast } from '../components/Toast'
import {
  consoleLayoutsApi, widgetUnavailableNote,
  type CatalogWidget, type ProfileTemplate, type WidgetArea,
} from '../api/consoleLayouts'

const AREA_LABEL: Record<WidgetArea, string> = { ops: '운용', admin: '관리' }

export default function MyLayoutPage() {
  const { show } = useToast()
  const [loading, setLoading] = useState(true)
  const [catalog, setCatalog] = useState<CatalogWidget[]>([])
  const [installed, setInstalled] = useState<string[]>([])
  const [profiles, setProfiles] = useState<ProfileTemplate[]>([])
  const [baseProfile, setBaseProfile] = useState<string>('')
  const [source, setSource] = useState<'override' | 'profile'>('profile')
  const [dashboard, setDashboard] = useState<string[]>([])
  const [dirty, setDirty] = useState(false)
  const [saving, setSaving] = useState(false)
  const [addId, setAddId] = useState('')

  const byId = useMemo(() => {
    const m: Record<string, CatalogWidget> = {}
    for (const w of catalog) m[w.id] = w
    return m
  }, [catalog])

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const [cat, prof, mine] = await Promise.all([
        consoleLayoutsApi.getCatalog(),
        consoleLayoutsApi.getProfiles(),
        consoleLayoutsApi.getMyLayout(),
      ])
      setCatalog(cat.widgets)
      setInstalled(cat.installed_services)
      setProfiles(prof.profiles)
      setBaseProfile(mine.base_profile)
      setSource(mine.source)
      setDashboard(mine.layout?.widgets?.dashboard ?? mine.layout?.pages?.[0]?.widgets ?? [])
      setDirty(false)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  useEffect(() => { load() }, [load])

  const add = (id: string) => {
    if (!id || dashboard.includes(id)) return
    setDashboard(d => [...d, id]); setDirty(true); setAddId('')
  }
  const remove = (i: number) => { setDashboard(d => d.filter((_, k) => k !== i)); setDirty(true) }
  const move = (i: number, dir: -1 | 1) => setDashboard(d => {
    const j = i + dir
    if (j < 0 || j >= d.length) return d
    const next = [...d]; [next[i], next[j]] = [next[j], next[i]]; return next
  })
  const applyProfile = (pid: string) => {
    const p = profiles.find(x => x.id === pid)
    if (!p) return
    setBaseProfile(pid)
    setDashboard([...p.dashboard])
    setDirty(true)
  }

  const save = async () => {
    setSaving(true)
    try {
      await consoleLayoutsApi.saveMyLayout({
        base_profile: baseProfile,
        layout: { pages: [{ slug: '/dashboard', widgets: dashboard }], widgets: { dashboard } },
      })
      show('내 대시보드 구성 저장됨', 'ok')
      setSource('override'); setDirty(false)
    } catch (e) {
      show((e as Error).message, 'err')   // 서버 RBAC 거부(403)/미존재(400) 포함
    } finally { setSaving(false) }
  }
  const reset = async () => {
    if (!confirm('개인 구성을 삭제하고 프로파일 기본값으로 되돌릴까요?')) return
    setSaving(true)
    try {
      await consoleLayoutsApi.resetMyLayout()
      show('프로파일 기본값으로 초기화됨', 'ok')
      await load()
    } catch (e) {
      show((e as Error).message, 'err')
    } finally { setSaving(false) }
  }

  // 추가 가능 후보 = 카탈로그 중 미배치. area 그룹핑, 가용 먼저.
  const addable = useMemo(() => {
    const remaining = catalog.filter(w => !dashboard.includes(w.id))
    const groups: { area: WidgetArea; widgets: CatalogWidget[] }[] = []
    for (const area of ['ops', 'admin'] as WidgetArea[]) {
      const ws = remaining.filter(w => w.area === area)
        .sort((a, b) => Number(b.available) - Number(a.available))
      if (ws.length) groups.push({ area, widgets: ws })
    }
    return groups
  }, [catalog, dashboard])

  if (loading) return <div className="empty" style={{ padding: 40 }}>불러오는 중…</div>

  return (
    <div style={{ display: 'grid', gap: 16, maxWidth: 980 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 12, flexWrap: 'wrap' }}>
        <h2 style={{ margin: 0 }}>내 대시보드 구성</h2>
        <span className="badge" style={{ background: source === 'override' ? 'var(--primary)' : 'var(--surface-2)' }}>
          {source === 'override' ? '개인 구성' : '프로파일 기본'}
        </span>
        {dirty && <span style={{ color: 'var(--warning)', fontSize: 12 }}>● 저장되지 않은 변경</span>}
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 6 }}>
          <button className="btn btn--sm btn--primary" onClick={save} disabled={saving || !dirty}>저장</button>
          <button className="btn btn--sm" onClick={() => load()} disabled={saving}>되돌리기</button>
          <button className="btn btn--sm" onClick={reset} disabled={saving} title="개인 구성 삭제 → 프로파일 기본">초기화</button>
        </div>
      </div>

      {/* 프로파일 템플릿 */}
      <section className="panel" style={{ padding: 14 }}>
        <div style={{ fontWeight: 600, marginBottom: 8 }}>프로파일</div>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
          <select className="form-input" value={baseProfile}
                  onChange={e => setBaseProfile(e.target.value)} style={{ width: 200 }}>
            {profiles.map(p => <option key={p.id} value={p.id}>{p.label}</option>)}
          </select>
          <button className="btn btn--sm" onClick={() => applyProfile(baseProfile)}
                  title="선택한 프로파일의 기본 위젯 세트로 교체">이 프로파일 적용</button>
          <span style={{ fontSize: 12, color: 'var(--text-muted)' }}>
            설치된 서비스: {installed.length ? installed.join(', ') : '없음'}
          </span>
        </div>
      </section>

      {/* 현재 대시보드 위젯 */}
      <section className="panel" style={{ padding: 14 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 10 }}>
          <div style={{ fontWeight: 600 }}>대시보드 위젯 ({dashboard.length})</div>
          <select className="form-input" value={addId} onChange={e => add(e.target.value)}
                  style={{ width: 240, marginLeft: 'auto', fontSize: 13 }}>
            <option value="">+ 위젯 추가…</option>
            {addable.map(g => (
              <optgroup key={g.area} label={AREA_LABEL[g.area]}>
                {g.widgets.map(w => (
                  <option key={w.id} value={w.id} disabled={!w.available}>
                    {w.title}{w.available ? '' : ' — 설치 후 사용 가능'}
                  </option>
                ))}
              </optgroup>
            ))}
          </select>
        </div>

        {dashboard.length === 0 ? (
          <div className="empty" style={{ padding: 24, textAlign: 'center', color: 'var(--text-muted)' }}>
            위젯이 없습니다 — 위 [+ 위젯 추가] 또는 프로파일을 적용하세요.
          </div>
        ) : (
          <ul style={{ listStyle: 'none', margin: 0, padding: 0, display: 'grid', gap: 6 }}>
            {dashboard.map((id, i) => {
              const w = byId[id]
              const note = w ? widgetUnavailableNote(w) : '카탈로그에 없는 위젯(권한/서비스 변경)'
              return (
                <li key={`${id}-${i}`} style={{
                  display: 'flex', alignItems: 'center', gap: 8, padding: '8px 10px',
                  border: '1px solid var(--border)', borderRadius: 6,
                  opacity: w && !w.available ? 0.7 : 1,
                }}>
                  <span style={{ fontWeight: 500 }}>{w?.title ?? id}</span>
                  {w && <span className="badge" style={{ fontSize: 11 }}>{AREA_LABEL[w.area]}</span>}
                  {w?.requires_service && <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>{w.requires_service}</span>}
                  {note && <span style={{ fontSize: 11, color: 'var(--warning)' }}>⚠ {note}</span>}
                  <span style={{ fontSize: 11, color: 'var(--text-muted)' }}>({id})</span>
                  <div style={{ marginLeft: 'auto', display: 'flex', gap: 4 }}>
                    <button className="btn btn--sm" onClick={() => move(i, -1)} disabled={i === 0} title="위로">↑</button>
                    <button className="btn btn--sm" onClick={() => move(i, 1)} disabled={i === dashboard.length - 1} title="아래로">↓</button>
                    <button className="btn btn--sm" onClick={() => remove(i)} title="제거" style={{ color: 'var(--danger)' }}>✕</button>
                  </div>
                </li>
              )
            })}
          </ul>
        )}
      </section>

      <p style={{ fontSize: 12, color: 'var(--text-muted)', margin: 0 }}>
        구성은 서버(계정별)에 저장되어 기기·세션을 넘어 따라갑니다. 위젯 가용성은 서비스 설치/상태에
        따릅니다 — 모든 위젯 API 는 서버에서 권한을 재확인합니다.
      </p>
    </div>
  )
}
