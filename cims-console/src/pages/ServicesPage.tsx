import { useState, useEffect, useCallback, useMemo } from 'react'
import { Link } from 'react-router-dom'
import { servicesApi, parseServiceStatus, type ServiceName, type ServiceAction } from '../api/services'
import { deploymentApi, type SipPackage, type ConfigTemplate } from '../api/deployment'
import { useToast } from '../components/Toast'
import Modal from '../components/Modal'
import ModuleConfigModal from '../components/module/ModuleConfigModal'

type SvcState = { running: boolean; pid?: number }

const SERVICE_META: Array<{ name: ServiceName; label: string; critical?: boolean; managed?: boolean }> = [
  { name: 'cmp',     label: 'CMP (미디어)',            managed: true },
  { name: 'csp',     label: 'CSP (SIP)',                managed: true },
  { name: 'cwrtc',   label: 'cwrtc (WebRTC 브리지)',   managed: true },
  { name: 'csc',     label: 'CSC (Admin API)',          managed: true, critical: true },
  { name: 'console', label: 'Console (웹 UI)',          managed: true },
  { name: 'phone',   label: 'Phone (웹 단말)',          managed: true },
  { name: 'agent',   label: 'Agent (원격 관리 모듈)',   managed: false },
]

export default function ServicesPage() {
  const { show } = useToast()
  const [states, setStates] = useState<Record<string, SvcState>>({})
  const [loading, setLoading] = useState(true)
  const [busy, setBusy] = useState<Record<string, boolean>>({})
  const [lastOutput, setLastOutput] = useState<string>('')
  const [packages, setPackages] = useState<SipPackage[]>([])
  const [selectedVersion, setSelectedVersion] = useState<Record<string, string>>({})
  const [templateModal, setTemplateModal] = useState<{ module: string; pkg: SipPackage } | null>(null)
  const [editing, setEditing] = useState(false)
  const [editText, setEditText] = useState('')
  const [editError, setEditError] = useState('')
  const [saving, setSaving] = useState(false)
  const [configModule, setConfigModule] = useState<string | null>(null)
  const [needsRestart, setNeedsRestart] = useState<Record<string, boolean>>({})

  const load = useCallback(async () => {
    try {
      const r = await servicesApi.status()
      setStates(parseServiceStatus(r.output))
    } catch (e) {
      show(`상태 조회 실패: ${(e as Error).message}`, 'err')
    } finally {
      setLoading(false)
    }
  }, [show])

  const loadPackages = useCallback(async () => {
    try {
      const items = await deploymentApi.listPackages()
      setPackages(items)
    } catch {
      // 패키지 API 실패해도 기본 동작은 유지 (프로세스 제어만)
    }
  }, [])

  useEffect(() => {
    void load()
    void loadPackages()
    const iv = setInterval(load, 5000)
    return () => clearInterval(iv)
  }, [load, loadPackages])

  // 모듈별로 그룹화된 패키지 목록 (버전 내림차순)
  const packagesByModule = useMemo(() => {
    const map: Record<string, SipPackage[]> = {}
    for (const p of packages) {
      (map[p.name] ||= []).push(p)
    }
    for (const name of Object.keys(map)) {
      map[name].sort((a, b) => b.version.localeCompare(a.version, undefined, { numeric: true }))
    }
    return map
  }, [packages])

  async function act(name: ServiceName, action: ServiceAction, critical: boolean) {
    if (critical && action !== 'start') {
      const ok = confirm(
        action === 'stop'
          ? `⚠️ CSC 를 중지하면 Console UI 가 즉시 끊깁니다. 계속할까요?`
          : `⚠️ CSC 재시작 중 Console UI 일시 단절됩니다. 계속할까요?`
      )
      if (!ok) return
    } else if (action === 'stop' || action === 'restart') {
      const ok = confirm(`${name} 서비스를 ${action === 'stop' ? '중지' : '재시작'}할까요?`)
      if (!ok) return
    }

    setBusy(b => ({ ...b, [name]: true }))
    try {
      const r = await servicesApi.act(name, action)
      setLastOutput(r.stdout + (r.stderr ? `\n[stderr]\n${r.stderr}` : ''))
      if (r.returncode === 0) {
        show(`${name} ${action} 완료`, 'ok')
        if (action === 'restart' || action === 'start') {
          setNeedsRestart(s => { const n = { ...s }; delete n[name]; return n })
        }
      } else {
        show(`${name} ${action} 실패 rc=${r.returncode}`, 'err')
      }
      setTimeout(() => { void load() }, 1500)
    } catch (e) {
      show((e as Error).message, 'err')
    } finally {
      setBusy(b => ({ ...b, [name]: false }))
    }
  }

  function openTemplate(moduleName: string, edit = false) {
    const versions = packagesByModule[moduleName]
    if (!versions || versions.length === 0) { show('등록된 패키지 없음', 'err'); return }
    const v = selectedVersion[moduleName] || versions[0].version
    const pkg = versions.find(p => p.version === v) || versions[0]
    setTemplateModal({ module: moduleName, pkg })
    setEditError('')
    if (edit) {
      setEditText(JSON.stringify(pkg.config_template ?? { version: 1, sections: [] }, null, 2))
      setEditing(true)
    } else {
      setEditing(false)
    }
  }

  function startEdit() {
    if (!templateModal) return
    setEditText(JSON.stringify(templateModal.pkg.config_template ?? { version: 1, sections: [] }, null, 2))
    setEditError('')
    setEditing(true)
  }

  async function saveEdit() {
    if (!templateModal) return
    let parsed: ConfigTemplate
    try {
      parsed = JSON.parse(editText)
    } catch (e) {
      setEditError(`JSON 파싱 실패: ${(e as Error).message}`)
      return
    }
    if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
      setEditError('최상위는 object 여야 합니다')
      return
    }
    setSaving(true)
    try {
      const updated = await deploymentApi.updatePackage(templateModal.pkg.id, { config_template: parsed })
      setTemplateModal({ module: templateModal.module, pkg: updated })
      setEditing(false)
      show('설정 템플릿 저장 완료', 'ok')
      void loadPackages()
    } catch (e) {
      setEditError((e as Error).message)
    } finally {
      setSaving(false)
    }
  }

  return (
    <div>
      <div style={{ marginBottom: 16, display: 'flex', gap: 12, alignItems: 'center' }}>
        <h3 style={{ margin: 0 }}>모듈관리</h3>
        <span className="text-muted" style={{ fontSize: 13 }}>
          프로세스 제어 + 배포 버전/설정 템플릿 조회. 신규 패키지 등록/편집은{' '}
          <Link to="/deploy/packages">배포 &gt; 패키지</Link> 에서.
        </span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: 8 }}>
          <button className="btn btn--outline" onClick={() => { void load(); void loadPackages() }}>↻ 새로고침</button>
        </div>
      </div>

      {loading ? (
        <div className="empty">로딩 중...</div>
      ) : (
        <table className="data-table">
          <thead>
            <tr>
              <th style={{ width: 80 }}>모듈</th>
              <th>설명</th>
              <th style={{ width: 110 }}>상태</th>
              <th style={{ width: 80 }}>PID</th>
              <th style={{ width: 130 }}>버전</th>
              <th style={{ width: 140 }}>설정 템플릿</th>
              <th style={{ width: 90 }}>설정</th>
              <th style={{ width: 260 }}>제어</th>
            </tr>
          </thead>
          <tbody>
            {SERVICE_META.map(svc => {
              const s = states[svc.name]
              const running = s?.running ?? false
              const disabled = busy[svc.name]
              const versions = packagesByModule[svc.name] || []
              const curVer = selectedVersion[svc.name] || versions[0]?.version || ''
              return (
                <tr key={svc.name}>
                  <td style={{ fontFamily: 'monospace', fontWeight: 'bold' }}>{svc.name}</td>
                  <td>
                    {svc.label}
                    {svc.critical && <span className="tag" style={{ marginLeft: 8, background: '#f39c12', color: '#fff' }}>critical</span>}
                  </td>
                  <td>
                    {svc.managed === false ? (
                      <span className="tag" style={{ background: '#3498db', color: '#fff' }}>원격</span>
                    ) : (
                      <span className="tag" style={{ background: running ? '#2ecc71' : '#95a5a6', color: '#fff' }}>
                        {running ? '실행 중' : '중지됨'}
                      </span>
                    )}
                  </td>
                  <td>{svc.managed === false ? '—' : (s?.pid ?? '—')}</td>
                  <td>
                    {versions.length > 0 ? (
                      <select
                        className="form-input"
                        style={{ width: '100%', fontSize: 12, padding: '4px 6px' }}
                        value={curVer}
                        onChange={e => setSelectedVersion(v => ({ ...v, [svc.name]: e.target.value }))}
                      >
                        {versions.map(p => (
                          <option key={p.id} value={p.version}>{p.version}</option>
                        ))}
                      </select>
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>등록된 패키지 없음</span>
                    )}
                  </td>
                  <td>
                    {versions.length > 0 ? (
                      <div style={{ display: 'flex', gap: 6 }}>
                        <button className="btn btn--sm btn--outline" onClick={() => openTemplate(svc.name, false)}>보기</button>
                        <button className="btn btn--sm btn--outline" onClick={() => openTemplate(svc.name, true)}>편집</button>
                      </div>
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>
                        패키지 없음 · <Link to="/deploy/packages">등록</Link>
                      </span>
                    )}
                  </td>
                  <td>
                    {versions.length > 0 ? (
                      svc.managed === false ? (
                        <span className="text-muted" style={{ fontSize: 12 }} title="원격 모듈 — install-agent.sh 옵션으로 설정">
                          (원격)
                        </span>
                      ) : (
                        <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
                          <button className="btn btn--sm btn--outline" onClick={() => setConfigModule(svc.name)}>
                            설정
                          </button>
                          {needsRestart[svc.name] && (
                            <span className="tag" style={{ background: '#e74c3c', color: '#fff' }} title="설정 변경 후 재시작 필요">!</span>
                          )}
                        </div>
                      )
                    ) : (
                      <span className="text-muted" style={{ fontSize: 12 }}>—</span>
                    )}
                  </td>
                  <td>
                    {svc.managed === false ? (
                      <span className="text-muted" style={{ fontSize: 12 }}>원격 모듈 · 로컬 제어 불가</span>
                    ) : (
                      <>
                        <button className="btn btn--sm" disabled={disabled || running}
                          onClick={() => act(svc.name, 'start', !!svc.critical)}>
                          ▶ start
                        </button>{' '}
                        <button className="btn btn--sm btn--outline" disabled={disabled || !running}
                          onClick={() => act(svc.name, 'restart', !!svc.critical)}>
                          ↻ restart
                        </button>{' '}
                        <button className="btn btn--sm btn--danger" disabled={disabled || !running}
                          onClick={() => act(svc.name, 'stop', !!svc.critical)}>
                          ■ stop
                        </button>
                      </>
                    )}
                  </td>
                </tr>
              )
            })}
          </tbody>
        </table>
      )}

      {lastOutput && (
        <div style={{ marginTop: 20 }}>
          <h4>최근 명령 출력</h4>
          <pre style={{
            background: '#0d1117', color: '#c9d1d9', padding: 12, borderRadius: 4,
            fontSize: 12, maxHeight: 240, overflow: 'auto', whiteSpace: 'pre-wrap',
          }}>
            {lastOutput.replace(/\[[0-9;]*m/g, '')}
          </pre>
        </div>
      )}

      {templateModal && (
        <Modal
          fullscreen
          title={`설정 템플릿 — ${templateModal.module} (v${templateModal.pkg.version})${editing ? ' · 편집 중' : ''}`}
          onClose={() => { if (!saving) setTemplateModal(null) }}
        >
          <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
            <div style={{ flex: 1, overflow: 'auto', padding: 20 }}>
              {editing ? (
                <>
                  <textarea
                    className="form-input"
                    style={{
                      width: '100%', height: 'calc(100vh - 260px)', minHeight: 400,
                      fontFamily: 'monospace', fontSize: 12, lineHeight: 1.5,
                      background: '#0d1117', color: '#c9d1d9',
                    }}
                    value={editText}
                    onChange={e => setEditText(e.target.value)}
                    spellCheck={false}
                  />
                  {editError && <div className="auth-error" style={{ marginTop: 8 }}>{editError}</div>}
                  <div className="text-muted" style={{ fontSize: 12, marginTop: 8 }}>
                    최상위 object. `sections[]`, `collections[]` 스키마. 저장 시 재배포되는 deployment 가 새 템플릿으로 overlay 됩니다.
                  </div>
                </>
              ) : templateModal.pkg.config_template ? (
                <pre style={{
                  background: '#0d1117', color: '#c9d1d9', padding: 12, borderRadius: 4,
                  fontSize: 12, overflow: 'auto', margin: 0,
                }}>
                  {JSON.stringify(templateModal.pkg.config_template, null, 2)}
                </pre>
              ) : (
                <div className="empty">이 패키지에 config_template 이 포함되어 있지 않습니다. 편집 버튼으로 생성할 수 있습니다.</div>
              )}
            </div>
            <div className="modal-footer" style={{ flex: '0 0 auto' }}>
              {editing ? (
                <>
                  <button className="btn btn--outline" onClick={() => { setEditing(false); setEditError('') }} disabled={saving}>취소</button>
                  <button className="btn btn--primary" onClick={saveEdit} disabled={saving}>
                    {saving ? '저장 중...' : '저장'}
                  </button>
                </>
              ) : (
                <>
                  <button className="btn btn--outline" onClick={() => setTemplateModal(null)}>닫기</button>
                  <button className="btn btn--primary" onClick={startEdit}>편집</button>
                </>
              )}
            </div>
          </div>
        </Modal>
      )}

      {configModule && (() => {
        const versions = packagesByModule[configModule] || []
        const v = selectedVersion[configModule] || versions[0]?.version
        return (
          <ModuleConfigModal
            source={{ type: 'module', name: configModule, version: v }}
            onClose={() => setConfigModule(null)}
            onDone={() => { setNeedsRestart(s => ({ ...s, [configModule]: true })) }}
          />
        )
      })()}
    </div>
  )
}
