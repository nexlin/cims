// 서비스 > 안내음성 — 서비스 안내음성·신호음·보류 음악 라이브러리(announcements.md §7). 계측기 '미디어 샘플'과는 별개 자원.
//   ① 표 = id(sys:/op:)·종류·출처·설명·길이·레벨·코덱 배지 + [▶ 청취](마스터 WAV 를 인증 fetch → Blob → <audio>)
//   ② [음원 등록…] = WAV 업로드 → OAM 이 변환기(cims-sample-conv)로 16 kHz 마스터 + pcmu/pcma/g722/amrwb(DTX 끔) 를 만든다 → op:<id>
//   ③ [CMP 배포] = CMP 노드 전부에 운영자 음원 파일·카탈로그를 맞추고 SIGUSR1(재적재). 노드별 보유 상태(sha256 대조)를 표 위에 띠로
//   ④ 삭제(manager) = 라이브러리에서 지우고 노드 파일도 걷는다. CSP 프로파일(Setup.Announcement.Rules)이 참조 중이면 그 안내는 응답 코드만 나간다
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { RefreshCw, Play, Square, Upload, Trash2, Send, Volume2 } from 'lucide-react'
import Modal from '@core/components/Modal'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { Checkbox } from '@core/components/ui/checkbox'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { EmptyState } from '@core/components/custom/empty-state'
import { FormField } from '@core/components/custom/form-field'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import { authHeaders } from '@core/api/client'
import { usersApi, type LineSvc } from '@core/api/users'
import { announcementsApi, annMasterPath, type AnnRow, type AnnKind, type AnnListResult, type AnnNode, type NodePresence } from '../api/announcements'

const KIND_LABEL: Record<AnnKind, string> = { tone: '신호음', announcement: '안내음', music: '음악' }
const KINDS: AnnKind[] = ['announcement', 'tone', 'music']
const CODECS = ['pcmu', 'pcma', 'g722', 'amr-wb'] as const
const PRESENCE: Record<NodePresence, { label: string; cls: string }> = {
  ok: { label: '최신', cls: 'bg-success-soft text-success' },
  partial: { label: '일부', cls: 'bg-warning-soft text-warning' },
  missing: { label: '없음', cls: 'bg-dangersoft text-destructive' },
  unreachable: { label: '미응답', cls: 'bg-muted text-muted-foreground' },
}
const fmtS = (ms: number | null | undefined) => (ms == null ? '—' : `${(ms / 1000).toFixed(1)} s`)
const fmtDb = (v: number | null | undefined) => (v == null ? '—' : `${v.toFixed(1)} dBov`)

function usePlayer() {
  const audioRef = useRef<HTMLAudioElement | null>(null)
  const [playing, setPlaying] = useState<string | null>(null)
  const [loading, setLoading] = useState<string | null>(null)
  const abort = useRef<AbortController | null>(null)
  const stop = useCallback(() => {
    abort.current?.abort()
    const el = audioRef.current
    if (el) { el.pause(); const prev = el.getAttribute('src'); if (prev?.startsWith('blob:')) URL.revokeObjectURL(prev); el.removeAttribute('src') }
    setPlaying(null); setLoading(null)
  }, [])
  const play = useCallback(async (id: string) => {
    stop()
    const ac = new AbortController(); abort.current = ac
    setLoading(id)
    try {
      const res = await fetch(annMasterPath(id), { headers: authHeaders(), signal: ac.signal })
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const url = URL.createObjectURL(await res.blob())
      const el = audioRef.current
      if (!el || ac.signal.aborted) { URL.revokeObjectURL(url); return }
      el.src = url
      await el.play()
      setPlaying(id)
    } catch (e) {
      if ((e as Error).name !== 'AbortError') throw e
    } finally { setLoading(l => (l === id ? null : l)) }
  }, [stop])
  useEffect(() => () => stop(), [stop])
  const element = <audio className="hidden" ref={audioRef} onEnded={() => setPlaying(null)} />
  return { play, stop, playing, loading, element }
}

function RegisterDialog({ onClose, onDone, existing }: { onClose: () => void; onDone: (row: AnnRow) => void; existing: Set<string> }) {
  const toast = useToast()
  const [file, setFile] = useState<File | null>(null)
  const [id, setId] = useState('')
  const [kind, setKind] = useState<AnnKind>('announcement')
  const [desc, setDesc] = useState('')
  const [loop, setLoop] = useState(false)
  const [norm, setNorm] = useState<string>('-26')
  const [replace, setReplace] = useState(false)
  const [busy, setBusy] = useState(false)
  const idErr = !id ? '' : !/^[a-z0-9_]{1,40}$/.test(id) ? '소문자·숫자·_ 40자 이내' : existing.has(`op:${id}`) && !replace ? '이미 있는 id — 교체를 켜거나 다른 id' : ''
  const onFile = (f: File | null) => {
    setFile(f)
    if (f && !id) setId(f.name.replace(/\.[^.]+$/, '').toLowerCase().replace(/[^a-z0-9_]/g, '_').slice(0, 40))
  }
  const submit = async () => {
    if (!file || !id || idErr) return
    setBusy(true)
    try {
      const row = await announcementsApi.register(file, { id, kind, description: desc, loop, normalize: norm.trim() === '' ? null : Number(norm), replace })
      toast.show(`음원 ${row.id} 등록 — ${fmtS(row.duration_ms)}, ${fmtDb(row.level_dbov)}. [CMP 배포]로 노드에 내린다`)
      onDone(row)
    } catch (e) {
      toast.show(`등록 실패: ${(e as Error).message}`, 'err')
    } finally { setBusy(false) }
  }
  return (
    <Modal title="안내 음원 등록" onClose={onClose}>
      <div className="flex w-[520px] max-w-full flex-col gap-3 p-4">
        <div className="text-xs text-muted-foreground">
          WAV(PCM 8/16/24/32-bit·float, mono/stereo, 8~48 kHz)를 올리면 OAM 이 <span className="font-mono">16-bit PCM 16 kHz</span> 마스터와
          pcmu/pcma/g722/amrwb(DTX 끔) 를 만든다. id 는 <span className="font-mono">op:&lt;id&gt;</span> 로 저장되며 CSP 안내 프로파일(Setup.Announcement.Rules)의 tone/media 에 그 이름으로 쓴다.
        </div>
        <FormField label="WAV 파일" required>
          <label className="flex cursor-pointer items-center gap-2 rounded-sm border border-border px-2 py-1.5 text-sm hover:bg-accent">
            <Upload size={14} /><span className="truncate">{file ? `${file.name} (${(file.size / 1024 / 1024).toFixed(2)} MB)` : '파일 선택…'}</span>
            <input className="hidden" type="file" accept=".wav,audio/wav,audio/x-wav" onChange={e => onFile(e.target.files?.[0] ?? null)} />
          </label>
        </FormField>
        <div className="grid grid-cols-2 gap-3">
          <FormField label="id" required error={idErr || undefined} help="프로파일이 참조하는 이름 — op:<id>">
            <Input value={id} onChange={e => setId(e.target.value)} className="font-mono" placeholder="예: ann_holiday" />
          </FormField>
          <FormField label="종류">
            <Select value={kind} onValueChange={v => setKind(v as AnnKind)}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>{KINDS.map(k => <SelectItem key={k} value={k}>{KIND_LABEL[k]}</SelectItem>)}</SelectContent>
            </Select>
          </FormField>
        </div>
        <FormField label="설명"><Input value={desc} onChange={e => setDesc(e.target.value)} placeholder="문안·용도" /></FormField>
        <div className="grid grid-cols-2 gap-3">
          <FormField label="P.56 활성 레벨 정규화 (dBov)" help="비우면 원음 그대로. 안내 -26 · 신호음 -16 · 음악 -20 권장">
            <Input value={norm} onChange={e => setNorm(e.target.value)} className="font-mono" placeholder="비움 = 원음" />
          </FormField>
          <div className="flex flex-col gap-2 pt-5 text-sm">
            <label className="flex items-center gap-2"><Checkbox checked={loop} onCheckedChange={v => setLoop(v === true)} /> 반복 힌트(보류 음악·신호음)</label>
            <label className="flex items-center gap-2"><Checkbox checked={replace} onCheckedChange={v => setReplace(v === true)} /> 같은 id 교체</label>
          </div>
        </div>
        <div className="flex justify-end gap-2 border-t border-border pt-3">
          <Button variant="outline" onClick={onClose} disabled={busy}>취소</Button>
          <Button onClick={submit} disabled={busy || !file || !id || !!idErr}>{busy ? '변환 중…' : '등록'}</Button>
        </div>
      </div>
    </Modal>
  )
}

/** 가입자 링백(컬러링) 등록 — WAV 를 sub:<가입 번호> 로 올린 뒤 그 회선의 ringback_media 를 가리키게 한다(announcements.md §6.3).
 *  스위치는 발신자 서비스 프로파일의 ringback(내장 `ringback` 프로파일)이고, 음원은 이 가입자 값이다. */
function SubscriberRingbackDialog({ onClose, onDone, existing }: { onClose: () => void; onDone: (row: AnnRow) => void; existing: Set<string> }) {
  const toast = useToast()
  const [file, setFile] = useState<File | null>(null)
  const [msisdn, setMsisdn] = useState('')
  const [desc, setDesc] = useState('')
  const [norm, setNorm] = useState<string>('-20')
  const [busy, setBusy] = useState(false)
  const digits = msisdn.replace(/^\+/, '')
  const idErr = !msisdn ? '' : !/^\+?[0-9]{3,20}$/.test(msisdn) ? '가입 번호(E.164, 예: +821012345678)' : ''
  const replace = existing.has(`sub:${digits}`)
  const submit = async () => {
    if (!file || !msisdn || idErr) return
    setBusy(true)
    try {
      const row = await announcementsApi.register(file, { id: digits, kind: 'music', description: desc || `가입자 링백 ${msisdn}`, loop: true, normalize: norm.trim() === '' ? null : Number(norm), replace, scope: 'sub' })
      // 회선의 ringback_media 를 이 음원으로 — 번호로 사람·회선 종류(call|voip)를 찾는다
      const users = await usersApi.list()
      let bound = false
      for (const u of users) {
        for (const svc of ['call', 'voip'] as LineSvc[]) {
          const subs = (svc === 'call' ? u.call_subscriptions : (u.voip_subscriptions || [])) || []
          if (subs.some(s => s.id === msisdn || s.id.replace(/^\+/, '') === digits)) {
            await usersApi.updateSub(u.id, svc, subs.find(s => s.id === msisdn || s.id.replace(/^\+/, '') === digits)!.id, { ringback_media: row.id })
            bound = true
          }
        }
      }
      toast.show(bound ? `${row.id} 등록 — 회선 ${msisdn} 의 링백 음원으로 지정. [CMP 배포]로 노드에 내린다` : `${row.id} 등록 — 번호 ${msisdn} 의 전화 회선을 찾지 못해 ringback_media 는 지정하지 않았다`, bound ? 'ok' : 'err')
      onDone(row)
    } catch (e) {
      toast.show(`등록 실패: ${(e as Error).message}`, 'err')
    } finally { setBusy(false) }
  }
  return (
    <Modal title="가입자 링백 음원 등록" onClose={onClose}>
      <div className="flex w-[520px] max-w-full flex-col gap-3 p-4">
        <div className="text-xs text-muted-foreground">
          가입자가 고른 WAV 를 <span className="font-mono">sub:&lt;가입 번호&gt;</span> 로 등록하고 그 회선의 <span className="font-mono">ringback_media</span> 를 가리키게 한다.
          발신자가 이 음원을 들으려면 발신자 접속서비스의 announcement_profile 이 서버 링백(<span className="font-mono">ringback</span>)이어야 한다.
        </div>
        <FormField label="WAV 파일" required>
          <label className="flex cursor-pointer items-center gap-2 rounded-sm border border-border px-2 py-1.5 text-sm hover:bg-accent">
            <Upload size={14} /><span className="truncate">{file ? `${file.name} (${(file.size / 1024 / 1024).toFixed(2)} MB)` : '파일 선택…'}</span>
            <input className="hidden" type="file" accept=".wav,audio/wav,audio/x-wav" onChange={e => setFile(e.target.files?.[0] ?? null)} />
          </label>
        </FormField>
        <div className="grid grid-cols-2 gap-3">
          <FormField label="가입 번호" required error={idErr || undefined} help={replace ? '같은 번호의 음원이 있어 교체한다' : 'E.164 — 회선 id'}>
            <Input value={msisdn} onChange={e => setMsisdn(e.target.value.trim())} className="font-mono" placeholder="+821012345678" />
          </FormField>
          <FormField label="P.56 활성 레벨 정규화 (dBov)" help="음악 -20 권장 · 비우면 원음">
            <Input value={norm} onChange={e => setNorm(e.target.value)} className="font-mono" />
          </FormField>
        </div>
        <FormField label="설명"><Input value={desc} onChange={e => setDesc(e.target.value)} placeholder="비우면 '가입자 링백 <번호>'" /></FormField>
        <div className="flex justify-end gap-2 border-t border-border pt-3">
          <Button variant="outline" onClick={onClose} disabled={busy}>취소</Button>
          <Button onClick={submit} disabled={busy || !file || !msisdn || !!idErr}>{busy ? '변환 중…' : '등록'}</Button>
        </div>
      </div>
    </Modal>
  )
}

export default function AnnouncementsPage() {
  const toast = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [data, setData] = useState<AnnListResult | null>(null)
  const [loading, setLoading] = useState(false)
  const [q, setQ] = useState('')
  const [kindF, setKindF] = useState<AnnKind | 'all'>('all')
  const [sel, setSel] = useState<string | null>(null)
  const [reg, setReg] = useState(false)
  const [regSub, setRegSub] = useState(false)
  const [deploying, setDeploying] = useState(false)
  const player = usePlayer()

  const load = useCallback(async () => {
    setLoading(true)
    try { setData(await announcementsApi.list(true)) }
    catch (e) { toast.show(`목록 조회 실패: ${(e as Error).message}`, 'err') }
    finally { setLoading(false) }
  }, [toast])
  useEffect(() => { void load() }, [load])

  const rows = useMemo(() => {
    const list = data?.media ?? []
    const needle = q.trim().toLowerCase()
    return list.filter(r => (kindF === 'all' || r.kind === kindF) && (!needle || r.id.toLowerCase().includes(needle) || r.description.toLowerCase().includes(needle)))
  }, [data, q, kindF])
  const selected = useMemo(() => rows.find(r => r.id === sel) ?? null, [rows, sel])
  const nodes: AnnNode[] = data?.nodes ?? []
  const operatorCount = (data?.media ?? []).filter(r => r.source !== 'bundled').length

  const del = async (r: AnnRow) => {
    if (!(await confirm({ title: '음원 삭제', body: `${r.id} 를 라이브러리에서 지우고 CMP 노드의 파일도 걷습니다. 이 음원을 참조하는 안내 프로파일은 응답 코드만 내게 됩니다.`, confirmLabel: '삭제', tone: 'danger' }))) return
    try { await announcementsApi.remove(r.id, true); toast.show(`${r.id} 삭제`); if (sel === r.id) setSel(null); void load() }
    catch (e) { toast.show(`삭제 실패: ${(e as Error).message}`, 'err') }
  }
  const deploy = async () => {
    setDeploying(true)
    try {
      const r = await announcementsApi.deploy()
      const entries = Object.entries(r.result)
      if (entries.length === 0) { toast.show('CMP 배포가 없습니다(배포 레코드에 cmp 모듈 없음)', 'err'); return }
      const lines = entries.map(([n, v]) => `${n}: ${v.error ? `실패(${v.error})` : v.pushed.length ? `${v.pushed.length}개 배포` : '이미 최신'}${v.errors?.length ? ` · 오류 ${v.errors.length}` : ''}`)
      toast.show(`CMP 배포 — ${lines.join(' / ')}`, entries.some(([, v]) => v.error || v.errors?.length) ? 'err' : 'ok')
      void load()
    } catch (e) { toast.show(`배포 실패: ${(e as Error).message}`, 'err') }
    finally { setDeploying(false) }
  }

  return (
    <div className="flex h-full flex-col">
      {player.element}
      <div className="toolbar flex flex-wrap items-center gap-2 border-b border-border px-3 py-2">
        <Input value={q} onChange={e => setQ(e.target.value)} placeholder="id·설명 검색" className="h-8 w-56" />
        <Select value={kindF} onValueChange={v => setKindF(v as AnnKind | 'all')}>
          <SelectTrigger className="h-8 w-32"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value="all">전체 종류</SelectItem>{KINDS.map(k => <SelectItem key={k} value={k}>{KIND_LABEL[k]}</SelectItem>)}</SelectContent>
        </Select>
        <Button variant="outline" size="sm" onClick={() => void load()} disabled={loading}><RefreshCw size={14} className={loading ? 'animate-spin' : ''} /> 새로고침</Button>
        <span className="ml-auto flex items-center gap-2">
          {data && !data.converter && <Badge variant="warningSoft" title="OAM 패키지 native/cims-sample-conv 또는 Announcements.SampleConv">변환기 없음 — 등록 불가</Badge>}
          <Button variant="outline" size="sm" onClick={() => void deploy()} disabled={!canWrite || deploying || operatorCount === 0} title="CMP 노드 전부에 운영자 음원·카탈로그를 맞추고 재적재(SIGUSR1)"><Send size={14} /> {deploying ? '배포 중…' : 'CMP 배포'}</Button>
          <Button size="sm" onClick={() => setReg(true)} disabled={!canWrite || !data?.converter}><Upload size={14} /> 음원 등록…</Button>
          <Button variant="outline" size="sm" onClick={() => setRegSub(true)} disabled={!canWrite || !data?.converter} title="가입자 링백(컬러링) WAV — sub:<번호> 로 등록하고 그 회선 ringback_media 를 지정"><Upload size={14} /> 가입자 링백…</Button>
        </span>
      </div>
      {nodes.length > 0 && (
        <div className="flex flex-wrap items-center gap-2 border-b border-border bg-card px-3 py-1.5 text-xs">
          <span className="text-muted-foreground">CMP 노드 보유(운영자 음원 {operatorCount}종)</span>
          {nodes.map(n => (
            <span key={n.deployment_id} className="flex items-center gap-1" title={n.error || (n.missing?.length ? `누락: ${n.missing.join(', ')}` : n.install_path || '')}>
              <span className="font-mono">{n.node}</span>
              <span className={`rounded-sm px-1.5 py-0.5 text-[11px] ${PRESENCE[n.presence].cls}`}>{PRESENCE[n.presence].label}{n.presence === 'partial' && n.missing ? ` ${(n.expected ?? 0) - n.missing.length}/${n.expected}` : ''}</span>
            </span>
          ))}
        </div>
      )}
      <div className="scroll-fill flex-1 overflow-auto">
        {rows.length === 0 ? (
          <EmptyState title={data ? '음원이 없습니다' : '불러오는 중…'} description={data ? '동봉 세트(sys:)가 보이지 않으면 OAM 패키지의 announcements/sys_catalog.jsonl 이 없는 것입니다. [음원 등록…]으로 WAV 를 올릴 수 있습니다.' : undefined} />
        ) : (
          <DataTable sticky>
            <thead><tr>
              <Th width={36}></Th><Th>id</Th><Th width={80}>종류</Th><Th width={64}>출처</Th><Th>설명</Th>
              <Th align="right" width={72}>길이</Th><Th align="right" width={90} title="ITU-T P.56 활성 레벨">레벨</Th><Th width={56}>반복</Th><Th width={200}>코덱</Th><Th width={44}></Th>
            </tr></thead>
            <tbody>
              {rows.map(r => {
                const isPlaying = player.playing === r.id
                const isLoading = player.loading === r.id
                return (
                  <TrLink key={r.id} selected={sel === r.id} onClick={() => setSel(s => (s === r.id ? null : r.id))}>
                    <Td><Button variant="ghost" size="sm" className="h-6 w-6 p-0" disabled={!r.has_master} title={!r.has_master ? '마스터 없음' : isPlaying ? '정지' : '마스터(16 kHz PCM) 청취'} onClick={e => { e.stopPropagation(); if (isPlaying) player.stop(); else player.play(r.id).catch(err => toast.show(`재생 실패: ${(err as Error).message}`, 'err')) }}>
                      {isPlaying ? <Square size={12} /> : <Play size={12} className={isLoading ? 'animate-pulse' : ''} />}</Button></Td>
                    <Td mono>{r.id}</Td>
                    <Td><Badge variant="neutralSoft">{KIND_LABEL[r.kind] ?? r.kind}</Badge></Td>
                    <Td><span className={r.source === 'bundled' ? 'text-muted-foreground' : 'text-info'}>{r.source === 'operator' ? '운영자' : r.source === 'subscriber' ? '가입자' : '동봉'}</span></Td>
                    <Td className="max-w-[420px] truncate" title={r.description}>{orDash(r.description)}</Td>
                    <Td align="right" mono>{fmtS(r.duration_ms)}</Td>
                    <Td align="right" mono>{fmtDb(r.level_dbov)}</Td>
                    <Td>{r.loop ? <Badge variant="infoSoft">loop</Badge> : '—'}</Td>
                    <Td><span className="flex flex-wrap gap-1">{CODECS.filter(c => r.files[c]).map(c => <Badge key={c} variant="neutralSoft" className="font-mono">{c}</Badge>)}</span></Td>
                    <Td>{r.source !== 'bundled' && <Button variant="ghost" size="sm" className="h-6 w-6 p-0" disabled={!canDelete} title={canDelete ? '삭제' : 'manager 이상'} onClick={e => { e.stopPropagation(); void del(r) }}><Trash2 size={12} /></Button>}</Td>
                  </TrLink>
                )
              })}
            </tbody>
          </DataTable>
        )}
      </div>
      {selected && (
        <div className="shrink-0 border-t border-border bg-card px-4 py-3 text-xs">
          <div className="mb-2 flex items-center gap-2"><Volume2 size={14} className="text-muted-foreground" /><b className="font-mono text-sm">{selected.id}</b>
            <Badge variant="neutralSoft">{KIND_LABEL[selected.kind] ?? selected.kind}</Badge>
            <span className="ml-auto font-mono text-muted-foreground">{selected.registered_at ? `${selected.registered_at} · ${selected.registered_by || ''}` : selected.source === 'bundled' ? 'CMP 패키지 동봉 — 파일은 CMP announcements/sys' : ''}</span></div>
          <div className="grid grid-cols-1 gap-y-1 md:grid-cols-2">
            <div><span className="text-muted-foreground">프로파일에서 참조</span> <span className="font-mono">tone: {selected.id}</span> · <span className="font-mono">media: {selected.id}</span></div>
            <div><span className="text-muted-foreground">파일</span> <span className="font-mono">{CODECS.map(c => selected.files[c]).filter(Boolean).join(' · ')}</span></div>
          </div>
        </div>
      )}
      {reg && data && <RegisterDialog onClose={() => setReg(false)} existing={new Set(data.media.map(m => m.id))} onDone={() => { setReg(false); void load() }} />}
      {regSub && data && <SubscriberRingbackDialog onClose={() => setRegSub(false)} existing={new Set(data.media.map(m => m.id))} onDone={() => { setRegSub(false); void load() }} />}
    </div>
  )
}
