// 시험 > 미디어 샘플 — 샘플 라이브러리(test_instrument.md §4). 원음은 16-bit PCM 16 kHz WAV 마스터로 관리하고 코덱 파일은 컨트롤러가 뽑는다.
//   ① 표 = id·종류·출처·설명·길이·P.56 레벨/활동률·DTX 채널 활동·코덱 배지 + [▶ 청취](마스터를 인증 fetch → Blob → <audio>) + 워커 보유 상태(토폴로지 선택 시)
//   ② [샘플 등록…] = WAV(PCM 8~48 kHz mono/stereo) 업로드 창 — id·종류·설명·DTX·P.56 정규화(-26 dBov 권장) → 컨트롤러 cims-sample-conv 변환·측정
//   ③ [워커 동기화] = 선택한 토폴로지의 워커 전부에 코덱 파일을 맞춘다(없는 것·크기 다른 것만). run 시작도 참조한 파일은 자동 배포
//   ④ 선택 행 아래 상세 = 발화 패턴(활동률·talk-spurt/pause 평균)·DTX 프레임 집계·파일 목록·짝(conv_p59_a/b)
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { RefreshCw, Play, Square, Upload, Trash2, Send, Music2 } from 'lucide-react'
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
import { testerApi, sampleMasterPath, type SampleRow, type SampleKind, type SamplePresence, type TopologyRow, type SamplesResult } from '@tester/api/tester'

const KIND_LABEL: Record<SampleKind, string> = { tone: '신호음', announcement: '안내음', music: '음악', speech: '통화 음성', other: '기타' }
const KINDS: SampleKind[] = ['speech', 'announcement', 'tone', 'music', 'other']
const CODECS = ['pcmu', 'pcma', 'g722', 'amr-wb'] as const
const PRESENCE: Record<SamplePresence, { label: string; cls: string }> = {
  ok: { label: '보유', cls: 'bg-success-soft text-success' },
  partial: { label: '일부', cls: 'bg-warning-soft text-warning' },
  missing: { label: '없음', cls: 'bg-dangersoft text-destructive' },
  unreachable: { label: '미응답', cls: 'bg-muted text-muted-foreground' },
}
const NONE = '__none__'
const fmt = (v: number | null | undefined, d = 1, unit = '') => (v == null ? '—' : `${v.toFixed(d)}${unit}`)

/** 마스터 WAV 하나를 재생하는 공용 <audio> — 인증 헤더가 필요하므로 Blob URL 로 */
function useSamplePlayer() {
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
      const res = await fetch(sampleMasterPath(id), { headers: authHeaders(), signal: ac.signal })
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

function RegisterDialog({ onClose, onDone, existing }: { onClose: () => void; onDone: (row: SampleRow) => void; existing: Set<string> }) {
  const toast = useToast()
  const [file, setFile] = useState<File | null>(null)
  const [id, setId] = useState('')
  const [kind, setKind] = useState<SampleKind>('speech')
  const [desc, setDesc] = useState('')
  const [dtx, setDtx] = useState(true)
  const [norm, setNorm] = useState<string>('-26')
  const [replace, setReplace] = useState(false)
  const [busy, setBusy] = useState(false)
  const idErr = !id ? '' : !/^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$/.test(id) ? '영숫자로 시작, 영숫자·_.- 64자 이내' : existing.has(id) && !replace ? '이미 있는 id — 교체를 켜거나 다른 id' : ''
  const onFile = (f: File | null) => {
    setFile(f)
    if (f && !id) setId(f.name.replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9_.-]/g, '_').slice(0, 64))
  }
  const submit = async () => {
    if (!file || !id || idErr) return
    setBusy(true)
    try {
      const row = await testerApi.registerSample(file, { id, kind, description: desc, dtx, normalize: norm.trim() === '' ? null : Number(norm), replace })
      toast.show(`샘플 ${row.id} 등록 — ${fmt(row.duration_s, 1, ' s')}, ${fmt(row.p56_active_level_dbov, 1, ' dBov')}`)
      onDone(row)
    } catch (e) {
      toast.show(`등록 실패: ${(e as Error).message}`, 'err')
    } finally { setBusy(false) }
  }
  return (
    <Modal title="샘플 등록" onClose={onClose}>
      <div className="flex w-[520px] max-w-full flex-col gap-3 p-4">
        <div className="text-xs text-muted-foreground">
          WAV(PCM 8/16/24/32-bit·float, mono/stereo, 8~48 kHz)를 올리면 컨트롤러가 <span className="font-mono">16-bit PCM 16 kHz</span> 마스터로 만들고
          pcmu/pcma/g722/amrwb(+ DTX) 를 뽑아 P.56 레벨·활동률을 잰다. 마스터는 <span className="font-mono">Tester.DataDir/samples/</span> 에 남는다.
        </div>
        <FormField label="WAV 파일" required>
          <label className="flex cursor-pointer items-center gap-2 rounded-sm border border-border px-2 py-1.5 text-sm hover:bg-accent">
            <Upload size={14} /><span className="truncate">{file ? `${file.name} (${(file.size / 1024 / 1024).toFixed(2)} MB)` : '파일 선택…'}</span>
            <input className="hidden" type="file" accept=".wav,audio/wav,audio/x-wav" onChange={e => onFile(e.target.files?.[0] ?? null)} />
          </label>
        </FormField>
        <div className="grid grid-cols-2 gap-3">
          <FormField label="id" required error={idErr || undefined} help="시나리오·토폴로지가 참조하는 이름">
            <Input value={id} onChange={e => setId(e.target.value)} className="font-mono" placeholder="예: ann_welcome" />
          </FormField>
          <FormField label="종류">
            <Select value={kind} onValueChange={v => setKind(v as SampleKind)}>
              <SelectTrigger><SelectValue /></SelectTrigger>
              <SelectContent>{KINDS.map(k => <SelectItem key={k} value={k}>{KIND_LABEL[k]}</SelectItem>)}</SelectContent>
            </Select>
          </FormField>
        </div>
        <FormField label="설명"><Input value={desc} onChange={e => setDesc(e.target.value)} placeholder="출처·용도" /></FormField>
        <div className="grid grid-cols-2 gap-3">
          <FormField label="P.56 활성 레벨 정규화 (dBov)" help="비우면 원음 그대로. 통화 음성은 -26(3GPP 특성화 입력), 신호음 -16, 음악 -20 권장">
            <Input value={norm} onChange={e => setNorm(e.target.value)} className="font-mono" placeholder="비움 = 원음" />
          </FormField>
          <div className="flex flex-col gap-2 pt-5 text-sm">
            <label className="flex items-center gap-2"><Checkbox checked={dtx} onCheckedChange={v => setDtx(v === true)} /> AMR-WB DTX 파일도 생성(<span className="font-mono">_dtx.amrwb</span>)</label>
            <label className="flex items-center gap-2"><Checkbox checked={replace} onCheckedChange={v => setReplace(v === true)} /> 같은 id 운영자본 교체</label>
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

export default function TesterSamplesPage() {
  const toast = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canWrite = hasRole(user, 'operator')
  const canDelete = hasRole(user, 'manager')
  const [data, setData] = useState<SamplesResult | null>(null)
  const [loading, setLoading] = useState(false)
  const [topologies, setTopologies] = useState<TopologyRow[]>([])
  const [topoId, setTopoId] = useState<string>(() => { try { return localStorage.getItem('tester.samples.topology') || NONE } catch { return NONE } })
  const [q, setQ] = useState('')
  const [kindF, setKindF] = useState<SampleKind | 'all'>('all')
  const [sel, setSel] = useState<string | null>(null)
  const [reg, setReg] = useState(false)
  const [syncing, setSyncing] = useState(false)
  const player = useSamplePlayer()

  const load = useCallback(async () => {
    setLoading(true)
    try {
      const r = await testerApi.samples(topoId === NONE ? undefined : topoId)
      setData(r)
    } catch (e) {
      toast.show(`샘플 목록 조회 실패: ${(e as Error).message}`, 'err')
    } finally { setLoading(false) }
  }, [topoId, toast])
  useEffect(() => { void load() }, [load])
  useEffect(() => { testerApi.topologies().then(r => setTopologies(r.topologies)).catch(() => setTopologies([])) }, [])
  useEffect(() => { try { localStorage.setItem('tester.samples.topology', topoId) } catch { /* noop */ } }, [topoId])

  const rows = useMemo(() => {
    const list = data?.samples ?? []
    const needle = q.trim().toLowerCase()
    return list.filter(r => (kindF === 'all' || r.kind === kindF) && (!needle || r.id.toLowerCase().includes(needle) || r.description.toLowerCase().includes(needle)))
  }, [data, q, kindF])
  const selected = useMemo(() => rows.find(r => r.id === sel) ?? null, [rows, sel])
  const workerNames = useMemo(() => (data?.workers ?? []).map(w => w.name), [data])

  const del = async (r: SampleRow) => {
    if (!(await confirm({ title: '샘플 삭제', body: `${r.id} 의 마스터·코덱 파일을 지웁니다. 워커에 배포된 사본은 남습니다.`, confirmLabel: '삭제', tone: 'danger' }))) return
    try { await testerApi.deleteSample(r.id); toast.show(`${r.id} 삭제`); if (sel === r.id) setSel(null); void load() }
    catch (e) { toast.show(`삭제 실패: ${(e as Error).message}`, 'err') }
  }
  const sync = async () => {
    if (topoId === NONE) return
    setSyncing(true)
    try {
      const r = await testerApi.syncSamples(topoId)
      const lines = Object.entries(r.result).map(([w, v]) => `${w}: ${v.error ? `실패(${v.error})` : v.pushed.length ? `${v.pushed.length}개 배포` : '이미 최신'}${v.errors?.length ? ` · 오류 ${v.errors.length}` : ''}`)
      toast.show(`워커 동기화 — ${lines.join(' / ')}`, Object.values(r.result).some(v => v.error || v.errors?.length) ? 'err' : 'ok')
      void load()
    } catch (e) { toast.show(`동기화 실패: ${(e as Error).message}`, 'err') }
    finally { setSyncing(false) }
  }

  return (
    <div className="flex h-full flex-col">
      {player.element}
      <div className="toolbar flex flex-wrap items-center gap-2 border-b border-border px-3 py-2">
        <Input value={q} onChange={e => setQ(e.target.value)} placeholder="id·설명 검색" className="h-8 w-56" />
        <Select value={kindF} onValueChange={v => setKindF(v as SampleKind | 'all')}>
          <SelectTrigger className="h-8 w-32"><SelectValue /></SelectTrigger>
          <SelectContent><SelectItem value="all">전체 종류</SelectItem>{KINDS.map(k => <SelectItem key={k} value={k}>{KIND_LABEL[k]}</SelectItem>)}</SelectContent>
        </Select>
        <Select value={topoId} onValueChange={setTopoId}>
          <SelectTrigger className="h-8 w-56" title="선택하면 그 토폴로지 워커의 보유 상태를 보이고 [워커 동기화] 대상이 된다"><SelectValue placeholder="토폴로지(워커 보유 확인)" /></SelectTrigger>
          <SelectContent><SelectItem value={NONE}>토폴로지 선택 안 함</SelectItem>{topologies.map(t => <SelectItem key={t.id} value={String(t.id)}>{t.name}</SelectItem>)}</SelectContent>
        </Select>
        <Button variant="outline" size="sm" onClick={() => void load()} disabled={loading}><RefreshCw size={14} className={loading ? 'animate-spin' : ''} /> 새로고침</Button>
        <span className="ml-auto flex items-center gap-2">
          {data && !data.converter && <Badge variant="warningSoft" title="패키지 native/cims-sample-conv 또는 Tester.SampleConv">변환기 없음 — 등록 불가</Badge>}
          <Button variant="outline" size="sm" onClick={() => void sync()} disabled={!canWrite || topoId === NONE || syncing} title={topoId === NONE ? '토폴로지를 먼저 고른다' : '토폴로지 워커 전부에 코덱 파일을 맞춘다'}><Send size={14} /> {syncing ? '동기화 중…' : '워커 동기화'}</Button>
          <Button size="sm" onClick={() => setReg(true)} disabled={!canWrite || !data?.converter}><Upload size={14} /> 샘플 등록…</Button>
        </span>
      </div>
      <div className="scroll-fill flex-1 overflow-auto">
        {rows.length === 0 ? (
          <EmptyState title={data ? '샘플이 없습니다' : '불러오는 중…'} description={data ? '패키지 동봉 샘플이 보이지 않으면 컨트롤러 samples/ 가 비어 있는 것입니다. [샘플 등록…] 으로 WAV 를 올릴 수 있습니다.' : undefined} />
        ) : (
          <DataTable sticky>
            <thead><tr>
              <Th width={36}></Th><Th>id</Th><Th width={80}>종류</Th><Th width={64}>출처</Th><Th>설명</Th>
              <Th align="right" width={64}>길이</Th><Th align="right" width={84} title="ITU-T P.56 활성 음성 레벨">레벨</Th><Th align="right" width={72} title="P.56 활동률(hangover 200 ms)">활동률</Th>
              <Th align="right" width={84} title="AMR-WB DTX 채널 활동 = (SPEECH+SID)/전체 프레임">DTX 채널</Th><Th width={180}>코덱</Th>
              {workerNames.map(w => <Th key={w} width={72} title={`워커 ${w} 보유 상태`}>{w}</Th>)}
              <Th width={44}></Th>
            </tr></thead>
            <tbody>
              {rows.map(r => {
                const isPlaying = player.playing === r.id
                const isLoading = player.loading === r.id
                return (
                  <TrLink key={r.id} selected={sel === r.id} onClick={() => setSel(s => (s === r.id ? null : r.id))}>
                    <Td><Button variant="ghost" size="sm" className="h-6 w-6 p-0" title={isPlaying ? '정지' : '마스터(16 kHz PCM) 청취'} onClick={e => { e.stopPropagation(); if (isPlaying) player.stop(); else player.play(r.id).catch(err => toast.show(`재생 실패: ${(err as Error).message}`, 'err')) }}>
                      {isPlaying ? <Square size={12} /> : <Play size={12} className={isLoading ? 'animate-pulse' : ''} />}</Button></Td>
                    <Td mono>{r.id}</Td>
                    <Td><Badge variant="neutralSoft">{KIND_LABEL[r.kind] ?? r.kind}</Badge></Td>
                    <Td><span className={r.source === 'user' ? 'text-info' : 'text-muted-foreground'}>{r.source === 'user' ? '운영자' : '동봉'}</span></Td>
                    <Td className="max-w-[360px] truncate" title={r.description}>{orDash(r.description)}</Td>
                    <Td align="right" mono>{fmt(r.duration_s, 1, ' s')}</Td>
                    <Td align="right" mono>{fmt(r.p56_active_level_dbov, 1, ' dBov')}</Td>
                    <Td align="right" mono>{fmt(r.p56_activity_pct, 1, ' %')}</Td>
                    <Td align="right" mono>{r.dtx_channel_activity_pct != null ? fmt(r.dtx_channel_activity_pct, 1, ' %') : '—'}</Td>
                    <Td><span className="flex flex-wrap gap-1">{CODECS.filter(c => r.files[c]).map(c => <Badge key={c} variant="neutralSoft" className="font-mono">{c}</Badge>)}{r['amr-wb-dtx'] && <Badge variant="infoSoft" className="font-mono">dtx</Badge>}</span></Td>
                    {workerNames.map(w => { const p = data?.presence?.[r.id]?.[w]; return <Td key={w}>{p ? <span className={`rounded-sm px-1.5 py-0.5 text-[11px] ${PRESENCE[p].cls}`}>{PRESENCE[p].label}</span> : '—'}</Td> })}
                    <Td>{r.source === 'user' && <Button variant="ghost" size="sm" className="h-6 w-6 p-0" disabled={!canDelete} title={canDelete ? '삭제' : 'manager 이상'} onClick={e => { e.stopPropagation(); void del(r) }}><Trash2 size={12} /></Button>}</Td>
                  </TrLink>
                )
              })}
            </tbody>
          </DataTable>
        )}
      </div>
      {selected && (
        <div className="shrink-0 border-t border-border bg-card px-4 py-3 text-xs">
          <div className="mb-2 flex items-center gap-2"><Music2 size={14} className="text-muted-foreground" /><b className="font-mono text-sm">{selected.id}</b>
            <Badge variant="neutralSoft">{KIND_LABEL[selected.kind] ?? selected.kind}</Badge>
            {selected.pair && <span className="text-muted-foreground">짝 <span className="font-mono">{selected.pair}</span> — 같은 시간축의 상대 화자(발신자 a · 착신자 b)</span>}
            <span className="ml-auto font-mono text-muted-foreground">{selected.master}{selected.created ? ` · ${selected.created}` : ''}</span></div>
          <div className="grid grid-cols-2 gap-x-8 gap-y-1 md:grid-cols-4">
            <div><span className="text-muted-foreground">RMS</span> <span className="font-mono">{fmt(selected.rms_dbov, 1, ' dBov')}</span></div>
            {selected.pattern && <>
              <div><span className="text-muted-foreground">on/off 활동률(20 ms 에너지+hangover)</span> <span className="font-mono">{fmt(selected.pattern.activity_pct, 1, ' %')}</span></div>
              <div><span className="text-muted-foreground">talk-spurt 평균</span> <span className="font-mono">{fmt(selected.pattern.talkspurt_mean_s, 2, ' s')}</span> <span className="text-muted-foreground">× {selected.pattern.talkspurts}</span></div>
              <div><span className="text-muted-foreground">pause 평균</span> <span className="font-mono">{fmt(selected.pattern.pause_mean_s, 2, ' s')}</span></div>
            </>}
            {selected.dtx_frames && <div className="col-span-2"><span className="text-muted-foreground">DTX 프레임</span> <span className="font-mono">SPEECH {selected.dtx_frames.speech} · SID {selected.dtx_frames.sid} · NO_DATA {selected.dtx_frames.no_data} / {selected.dtx_frames.total}</span></div>}
            <div className="col-span-2 md:col-span-4"><span className="text-muted-foreground">파일</span> <span className="font-mono">{[selected.master, ...CODECS.map(c => selected.files[c]).filter(Boolean), selected['amr-wb-dtx']].filter(Boolean).join(' · ')}</span></div>
          </div>
        </div>
      )}
      {reg && data && <RegisterDialog onClose={() => setReg(false)} existing={new Set(data.samples.filter(s => s.source === 'user').map(s => s.id))} onDone={() => { setReg(false); void load() }} />}
    </div>
  )
}
