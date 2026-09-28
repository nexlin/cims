// 운용 › 서비스 설정 › SIP 연동 — CSP 연동(Local Node → Remote Node → Route → Route Set)과
// 규칙(Rule → Rule Set → Routing Policy → ACL Policy)을 한 페이지 8단계로 편집한다.
//   데이터 = CSP 배포의 컬렉션 9종 (api/sipPeering.ts). 참조는 전부 name — 기존 레코드의 name 은 바꾸지 않는다.
//   저장 = 저장바 하나로 묶음 검사 → 바뀐 컬렉션만 참조 순서대로 → CSP 재적재 신호 1번.
//   Local Node · access_services 는 읽기만 (소켓 개폐 · 단말 접속 — 이 화면 밖의 권한).
//   Rule field = <원천>[.<부분>] (CSP CspRuleField — sip_service_model.md §2-5). 옛 이름 12종도 읽고 보여준다.
import { Fragment, useCallback, useEffect, useMemo, useState, type ReactNode } from 'react'
import { useNavigate } from 'react-router-dom'
import { ChevronRight, Plus, Trash2, Search, ArrowRight, RefreshCw, Route as RouteIcon, X, Check,
         FileDiff, Link as LinkIcon, Pencil, ExternalLink } from 'lucide-react'
import Modal from '@core/components/Modal'
import { Button } from '@core/components/ui/button'
import { Badge } from '@core/components/ui/badge'
import { Input } from '@core/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@core/components/ui/select'
import { Checkbox } from '@core/components/ui/checkbox'
import { ToggleGroup, ToggleGroupItem } from '@core/components/ui/toggle-group'
import { Alert, AlertDescription } from '@core/components/ui/alert'
import { DataTable, Th, Td, TrLink, orDash } from '@core/components/custom/data-table'
import { SubSection } from '@core/components/custom/collapsible-section'
import { FormField } from '@core/components/custom/form-field'
import { StatusDot } from '@core/components/custom/status-dot'
import { StickySaveBar } from '@core/components/custom/sticky-save-bar'
import { EmptyState } from '@core/components/custom/empty-state'
import { useToast } from '@core/components/Toast'
import { useConfirm } from '@core/components/custom/confirm'
import { useAuth } from '@core/contexts/AuthContext'
import { hasRole } from '@core/utils/permissions'
import type { Deployment } from '@core/api/deployment'
import * as sp from '../api/sipPeering'
import type { Rec, Data } from '../api/sipPeering'

// ── 용어 · 한글 병기 ──
const KO: Record<string, string> = {
  'Local Node': '접속점', 'Remote Node': '상대 노드', Route: '경로', 'Route Set': '경로 묶음', Rule: '조건', 'Rule Set': '조건 묶음',
  'Routing Policy': '라우팅 정책', 'ACL Policy': '접근 제어',
  failover: '주·예비', round_robin: '번갈아 분배', weighted: '비율 분배', hash_by_caller: '발신자별 고정', options_ping: 'OPTIONS 감시',
  none: '안 함', invite_response: '미구현', reject: '거절', next_policy: '다음 정책', deny: '차단', allow: '허용', global: '전체',
  local_node: 'Local Node', route: 'Route', route_set: 'Route Set', AND: '모두', OR: '하나라도', digest: '계정 인증', access: '단말용',
  peering: '대국용', mgmt: '관리용',
}
const Ko = ({ t }: { t: string }) => KO[t] ? <span className="ml-1 text-xs font-normal text-muted-foreground">{KO[t]}</span> : null

type Tab = 'lns' | 'rnodes' | 'routes' | 'sets' | 'rules' | 'rsets' | 'policies' | 'acls'
const STEPS: Array<[Tab, string, sp.Col]> = [
  ['lns', 'Local Node', 'local_nodes'], ['rnodes', 'Remote Node', 'remote_nodes'], ['routes', 'Route', 'routes'], ['sets', 'Route Set', 'route_sets'],
  ['rules', 'Rule', 'rules'], ['rsets', 'Rule Set', 'rule_sets'], ['policies', 'Routing Policy', 'routing_policies'], ['acls', 'ACL Policy', 'acl_policies']]

// Rule field — <원천>[.<부분>] (csp/CspRuleField.cpp 와 같은 문법). 화면은 원천 · 부분 · (다른 헤더면) 헤더 이름으로 고른다.
//   옛 이름 12종은 뜻이 같은 원천·부분으로 보여주고, 손대면 새 문법으로 바뀐다. req_uri_user 는 '착신 번호' 그대로 둔다
//   (Routing = 번호 변환 뒤 +E.164, ACL = 받은 그대로 — 새 문법의 callee.e164 는 ACL 에서 빈 값이라 두 판정에 다 맞는 것은 이것뿐).
const ADDR_PARTS = ['user', 'host', 'port', 'scheme', 'display', 'param']
const SOURCES: Array<{ k: string; label: string; parts: string[]; hdr?: string; off?: string; note?: string }> = [
  { k: 'callee', label: '착신 번호', parts: [], note: 'Routing Policy 는 번호 변환 뒤 +E.164, ACL Policy 는 받은 그대로의 값을 본다' },
  { k: 'request_uri', label: 'Request-URI (받은 그대로)', parts: ['user', 'host', 'port', 'scheme', 'param'] },
  { k: 'from', label: 'From (발신)', parts: ADDR_PARTS },
  { k: 'to', label: 'To (수신)', parts: ADDR_PARTS },
  { k: 'contact', label: 'Contact', parts: ADDR_PARTS },
  { k: 'pai', label: 'P-Asserted-Identity', hdr: 'P-Asserted-Identity', parts: ['user', 'host', 'scheme', 'display'],
    note: '들어온 Route 로 식별된 피어가 보낸 것만 본다 (RFC 3325) — 단말이 보낸 값은 빈 값' },
  { k: 'via', label: 'Via (맨 위)', hdr: 'Via', parts: ['host', 'port', 'param'] },
  { k: 'ua', label: 'User-Agent', hdr: 'User-Agent', parts: [''] },
  { k: 'src', label: '보낸 곳', parts: ['ip', 'port'] },
  { k: 'local_node', label: '받은 Local Node', parts: [''] },
  { k: 'method', label: '메서드', parts: [''] },
  { k: 'header', label: '다른 헤더', parts: ['', ...ADDR_PARTS], note: '헤더 이름은 대소문자를 가리지 않는다 · 같은 헤더가 여러 개면 하나라도 맞으면 일치' },
]
const PART_KO: Record<string, string> = { '': '값 그대로', user: '번호·사용자', host: '도메인·호스트', port: '포트', scheme: 'scheme (sip·tel)',
  display: '표시 이름', param: '파라미터', ip: 'IP' }
const LEGACY: Record<string, string> = { from_uri_user: 'from.user', from_uri_host: 'from.host', to_uri_user: 'to.user', to_uri_host: 'to.host',
  req_uri_host: 'request_uri.host', src_ip: 'src.ip', user_agent: 'header:User-Agent', method: 'method',
  p_asserted_identity: 'header:P-Asserted-Identity.user', via_host: 'header:Via.host' }
const HDR_TOKEN = /^[A-Za-z0-9\-!%*_+`'~]+$/
interface FieldRef { k: string; part: string; hdr: string; param: string; ok: boolean; legacy?: boolean; off?: string }
/** field 문자열 → 원천·부분. ok=false 면 CSP 가 거절하는 문법 */
function parseField(field: string): FieldRef {
  const R = (k: string, part = '', o: Partial<FieldRef> = {}): FieldRef => ({ k, part, hdr: '', param: '', ok: true, ...o })
  if (field === 'req_uri_user') return R('callee', '', { legacy: true })
  if (field === 'dst_ip') return R('', '', { legacy: true, off: 'CSP 가 값을 채우지 않는다 (항상 빈 값)' })
  if (LEGACY[field]) return { ...parseField(LEGACY[field]), legacy: true }
  if (field === 'callee.e164') return R('callee', 'e164')
  let src: string, hdr = '', rest: string
  if (field.startsWith('header:')) { const t = field.slice(7), i = t.indexOf('.'); hdr = i < 0 ? t : t.slice(0, i); rest = i < 0 ? '' : t.slice(i + 1); src = 'header'
    if (!hdr || !HDR_TOKEN.test(hdr)) return R('header', '', { hdr, ok: false }) }
  else { const i = field.indexOf('.'); src = i < 0 ? field : field.slice(0, i); rest = i < 0 ? '' : field.slice(i + 1) }
  const part = rest.startsWith('param:') ? 'param' : rest, param = part === 'param' ? rest.slice(6) : ''
  if (part === 'param' && !param) return R(src, part, { hdr, ok: false })
  if (src === 'header') { const known = SOURCES.find(x => x.hdr && x.hdr.toLowerCase() === hdr.toLowerCase())
    const k = known?.k ?? 'header'
    return R(k, part, { hdr: known ? known.hdr! : hdr, param, ok: ['', ...ADDR_PARTS].includes(part) }) }
  // CSP _parse 와 같은 허용 범위 — 주소형 원천은 부분 생략·주소형 부분 전부, src 는 ip·port, local_node·method 는 부분 없음
  const ok = ['request_uri', 'from', 'to', 'contact'].includes(src) ? ['', ...ADDR_PARTS].includes(part)
    : src === 'src' ? part === 'ip' || part === 'port' : src === 'local_node' || src === 'method' ? part === '' : false
  return R(src, part, { param, ok })
}
/** 원천·부분 → field 문자열 */
function buildField(f: Pick<FieldRef, 'k' | 'part' | 'hdr' | 'param'>) {
  if (f.k === 'callee') return f.part === 'e164' ? 'callee.e164' : 'req_uri_user'
  const def = SOURCES.find(x => x.k === f.k)
  const part = f.part === 'param' ? `param:${f.param}` : f.part
  const base = f.k === 'header' || def?.hdr ? `header:${def?.hdr ?? f.hdr}` : f.k
  return part ? `${base}.${part}` : base
}
const fieldOk = (field: string) => parseField(field).ok
function fieldName(field: string) {
  const f = parseField(field), def = SOURCES.find(x => x.k === f.k)
  if (!def || !f.ok) return field
  const src = f.k === 'header' ? f.hdr : def.label.replace(/ \(.+\)$/, '')
  if (f.k === 'callee') return f.part === 'e164' ? '착신 번호 (변환 뒤)' : '착신 번호'
  if (f.part === 'param') return `${src} ;${f.param}`
  return f.part ? `${src} ${PART_KO[f.part] ?? f.part}` : src
}
/** VALUE_KIND · 기본 이름용 짧은 열쇠 (예 from.user · pai.user · header.param) */
const fieldKey = (field: string) => { const f = parseField(field); return f.part ? `${f.k}.${f.part}` : f.k }
const OPWORD: Record<string, string> = { eq: '같음', ne: '다름', prefix: '로 시작', suffix: '로 끝남', contains: '포함', regex: '정규식',
  in_cidr: 'IP 대역 안', in_list: '목록 중 하나', in_range: '번호 범위 안', exists: '있음', not_exists: '없음' }
const DIST = ['failover', 'round_robin', 'weighted', 'hash_by_caller']
// 분배 방식 설명 (CspRouteSetMap::Select) · members 첫 열의 뜻
const DIST_HELP: Record<string, string> = {
  failover: 'priority 가 가장 작은 Route 로만 보낸다. 그 Route 가 끊겼거나 5xx·408·410 으로 실패할 때만 다음 순위로 넘어간다.',
  round_robin: '호마다 다음 Route 로 번갈아 보낸다. 끊긴 Route 와 weight 0 은 건너뛴다.',
  weighted: 'weight 비율대로 나눠 보낸다 (예 3:1 이면 네 호 중 셋). 끊긴 Route 는 건너뛴다.',
  hash_by_caller: '같은 발신자(From 번호@도메인)는 늘 같은 Route 로 보낸다. 그 Route 가 끊기면 다음 Route.',
}
const MEMBER_COL_HELP: Record<string, string> = {
  failover: '작을수록 먼저 — 나머지는 예비', round_robin: '0 이면 제외', weighted: '비율', hash_by_caller: '0 이면 제외',
}

const PAGE = 25
const digits = (v: unknown) => String(v ?? '').replace(/[^0-9]/g, '')
const isIp = (s: string) => /^\d{1,3}(\.\d{1,3}){3}$/.test(s) && s.split('.').every(n => +n <= 255)
const NAME_RE = /^[a-z0-9][a-z0-9_.-]*$/i
// 포트 입력 — 지울 수 있게 빈칸은 '' 로 둔다. 저장(추가·적용) 전에 portErr 로 막는다.
const portVal = (v: string) => v.trim() === '' ? '' : Number(v)
const portErr = (v: unknown) => v === '' || v == null ? 'port 를 넣는다.' : !(Number.isInteger(+v) && +v > 0 && +v < 65536) ? `port ${v} 가 1~65535 범위 밖이다.` : ''
const byName = (rs: Rec[], n: string) => rs.find(r => r.name === n)
/** Route 기본 이름 = <Remote Node>-<Local Node> — (LN, RN) 쌍이 이름에 드러나고 겹치지 않는다 */
const routeName = (rn: string, ln: string) => rn.trim() && ln ? `${rn.trim()}-${ln}` : ''
/** Route Set 기본 이름 = to-<Remote Node> — 어디로 보내는 묶음인지가 이름에 드러난다 (분배 방식은 바뀌므로 넣지 않는다) */
const setName = (rn: string) => rn.trim() ? `to-${rn.trim()}` : 'to-'
// 규칙 쪽 기본 이름 — 이름만 보고 뜻이 드러나게. name 에 쓸 수 없는 기호(+ @ / 공백 …)는 - 로 바꾼다.
const slug = (x: string) => x.replace(/[^A-Za-z0-9_.-]+/g, '-').replace(/-{2,}/g, '-').replace(/^-|-$/g, '')
const FIELD_SHORT: Record<string, string> = { callee: 'callee', 'callee.e164': 'callee', 'request_uri.host': 'callee-host', 'from.user': 'caller', 'from.host': 'caller-host',
  'to.user': 'to', 'to.host': 'to-host', 'src.ip': 'src', ua: 'ua', method: 'method', 'pai.user': 'pai', 'via.host': 'via' }
const fieldShort = (field: string) => { const f = parseField(field)
  if (FIELD_SHORT[fieldKey(field)]) return FIELD_SHORT[fieldKey(field)]
  return slug([f.k === 'header' ? f.hdr.toLowerCase() : f.k.replace('_', '-'), f.part === 'param' ? f.param : f.part].filter(Boolean).join('-')) || slug(field) }
const OP_SHORT: Record<string, string> = { eq: 'is', ne: 'not', prefix: 'starts', suffix: 'ends', contains: 'has', regex: 're', in_cidr: 'in',
  in_list: 'in-list', in_range: 'range', exists: 'exists', not_exists: 'absent' }
/** Rule = <무엇을>-<어떻게>-<값> (예 callee-starts-82 · callee-host-is-ims.kt.test) */
const ruleNameOf = (f: string, op: string, v: string) => {
  const val = op === 'exists' || op === 'not_exists' || op === 'in_list' ? '' : slug(String(v ?? '')).slice(0, 40)
  return [fieldShort(f), OP_SHORT[op] ?? op, val].filter(Boolean).join('-') }
/** Routing Policy = <Rule Set>-<Route Set> ("이럴 때 → 여기로"), ACL Policy = <block|allow>-<Rule Set> */
// value 칸 — 고른 field·op 에 따라 무엇을 넣는지와 예시. 시험 기본값은 실제 호에 걸리지 않는 값.
const VALUE_KIND: Record<string, { what: string; ex: string; safe: string }> = {
  callee: { what: '착신 번호 (+ 로 시작하는 국제 형식 — Routing 은 국내형 0… 을 +82… 로 바꾼 뒤 비교)', ex: '+82310001234', safe: '+8299' },
  'request_uri.host': { what: '착신 도메인', ex: 'ims.kt.test', safe: 'ims.test.invalid' },
  'from.user': { what: '발신 번호 또는 사용자 이름 (단말이 From 에 적은 그대로)', ex: '+821012345678 · 1001', safe: '+8299' },
  'from.host': { what: '발신 도메인', ex: 'pbx.hq.test', safe: 'caller.test.invalid' },
  'to.user': { what: 'To 헤더의 번호 (보통 착신 번호와 같다 — 번호 변환 전)', ex: '02-2104-5012 · +82221045012', safe: '+8299' },
  'to.host': { what: 'To 헤더의 도메인', ex: 'pbx.local', safe: 'to.test.invalid' },
  'src.ip': { what: '패킷을 보낸 곳의 IP', ex: '10.50.0.3', safe: '192.0.2.99' },
  ua: { what: '상대 장비가 User-Agent 에 적은 문자열 (일부만 맞추려면 op 를 포함으로)', ex: 'Avaya-CM', safe: 'test-invalid-ua' },
  method: { what: 'SIP 요청 종류', ex: 'INVITE · REGISTER · OPTIONS · MESSAGE', safe: 'INVITE' },
  'callee.e164': { what: '번호 변환 뒤 착신 번호 (+E.164 — Routing Policy 에서만 값이 있다)', ex: '+82310001234', safe: '+8299' },
  'request_uri.user': { what: 'Request-URI 의 번호·사용자 (받은 그대로 — 번호 변환 전)', ex: '02-2104-5012 · +82221045012', safe: '+8299' },
  'pai.user': { what: '피어가 알려준 발신 번호 (P-Asserted-Identity)', ex: '+82215551001', safe: '+8299' },
  'via.host': { what: '맨 위 Via 의 호스트 (바로 앞 장비)', ex: '10.50.0.91 · pbx.hq.test', safe: '192.0.2.99' },
  'src.port': { what: '패킷을 보낸 곳의 포트', ex: '5060', safe: '1' },
  local_node: { what: '받은 Local Node 이름', ex: 'peering-udp', safe: 'test-invalid-ln' },
  '*.user': { what: '번호 또는 사용자 이름 (%xx 는 풀어서 비교)', ex: '+821012345678 · 1001', safe: '+8299' },
  '*.host': { what: '도메인 또는 IP (대소문자 무시)', ex: 'pbx.hq.test', safe: 'host.test.invalid' },
  '*.port': { what: '포트 번호', ex: '5060', safe: '1' },
  '*.scheme': { what: 'URI scheme', ex: 'sip · sips · tel', safe: 'test-invalid' },
  '*.display': { what: '표시 이름 (따옴표는 벗겨서 비교)', ex: 'Branch 3F', safe: 'test-invalid' },
  '*.param': { what: '파라미터 값', ex: 'user-busy · phone', safe: 'test-invalid' },
  '*': { what: '헤더 값 그대로 (쉼표로 나뉜 값마다 비교)', ex: 'Avaya-CM', safe: 'test-invalid' },
}
const kindOf = (field: string) => { const k = fieldKey(field), part = parseField(field).part
  return VALUE_KIND[k] ?? VALUE_KIND[part ? `*.${part}` : '*'] }
const OP_FORMAT: Record<string, (ex: string) => string> = {
  in_range: () => '"시작-끝" — 두 끝 자릿수가 같아야 한다 (예 +82310000000-+82319999999)',
  in_list: ex => `쉼표로 여러 개 (예 ${ex.split(' · ')[0]},…)`,
  in_cidr: () => 'IP 대역 (예 10.50.0.0/16)',
  prefix: ex => `앞부분만 (예 ${ex.split(' · ')[0].slice(0, 5)})`,
  suffix: () => '끝부분만',
  contains: () => '들어 있는 일부 문자열',
  regex: () => '정규식 (예 ^\\+8231)',
}
function valueHelp(field: string, op: string) {
  const k = kindOf(field); if (!k) return undefined
  const fmt = OP_FORMAT[op]?.(k.ex)
  return `${k.what} — ${fmt ?? `예 ${k.ex}`}`
}
const valueSafe = (field: string) => kindOf(field)?.safe ?? ''
const valuePh = (field: string) => kindOf(field)?.ex.split(' · ')[0] ?? ''
const NEW_COND_INIT = { cf: 'request_uri.host', cop: 'eq', cv: 'ims.test.invalid' }
const policyNameOf = (rset: string, target: string) => [rset, target].filter(Boolean).join('-') || 'rp-'
const aclNameOf = (rset: string, action = 'deny') => `${action === 'allow' ? 'allow' : 'block'}-${rset.replace(/^match-/, '')}`

function sentence(r: Rec | undefined, neg?: boolean) {
  if (!r) return '(없는 Rule)'
  const f = fieldName(r.field), v = String(r.value ?? '')
  let s: string
  if (r.op === 'in_range') { const [a, b] = v.split(/[-~]/); s = `${f} ${a || '?'} ~ ${b || '?'}` }
  else if (r.op === 'in_list') s = `${f} ${v.split(',').length}개 중 하나`
  else if (r.op === 'exists' || r.op === 'not_exists') s = `${f} ${OPWORD[r.op]}`
  else s = `${f} ${v} ${OPWORD[r.op] ?? r.op}`
  return neg ? `${s} — 아님` : s
}
const ruleExpr = (r: Rec | undefined, neg?: boolean) => r ? `${neg ? 'NOT ' : ''}${r.field} ${r.op} ${r.value ?? ''}` : '(missing)'

// ── 공통 부품 ──
function Seg({ value, onChange, opts, disabled }: { value: string; onChange: (v: string) => void; opts: Array<[string, ReactNode]>; disabled?: boolean }) {
  return (
    <ToggleGroup type="single" size="sm" value={value} disabled={disabled} className="justify-start"
                 onValueChange={(v: string) => v && onChange(v)}>
      {opts.map(([v, l]) => <ToggleGroupItem key={v} value={v}>{l}</ToggleGroupItem>)}
    </ToggleGroup>
  )
}
const NONE = '__none__'
const NEW_SET = '__new_route_set__'
const NEW_COND = '__new_condition__'
function Sel({ value, onChange, opts, placeholder, mono, className, disabled }: {
  value: string; onChange: (v: string) => void; opts: Array<[string, string, string?]>; placeholder?: string; mono?: boolean; className?: string; disabled?: boolean }) {
  return (
    <Select value={value === '' ? NONE : value} onValueChange={v => onChange(v === NONE ? '' : v)} disabled={disabled}>
      <SelectTrigger className={`${mono ? 'font-mono text-sm' : ''} ${className ?? ''}`}><SelectValue placeholder={placeholder} /></SelectTrigger>
      <SelectContent>
        {opts.map(([v, l, off]) => <SelectItem key={v || NONE} value={v === '' ? NONE : v} disabled={!!off}>{l}{off ? ` — ${off}` : ''}</SelectItem>)}
      </SelectContent>
    </Select>
  )
}
function Srch({ value, onChange, placeholder }: { value: string; onChange: (v: string) => void; placeholder: string }) {
  return (
    <label className="relative flex min-w-[200px] max-w-[320px] flex-1 items-center">
      <Search size={14} className="absolute left-2.5 text-muted-foreground" />
      <Input className="pl-8" value={value} placeholder={placeholder} aria-label={placeholder} onChange={e => onChange(e.target.value)} />
    </label>
  )
}
function Pager({ page, total, onPage }: { page: number; total: number; onPage: (p: number) => void }) {
  const pages = Math.max(1, Math.ceil(total / PAGE))
  if (total <= PAGE) return null
  return (
    <div className="flex items-center justify-end gap-1.5 text-sm text-muted-foreground tabular-nums">
      <span>{page * PAGE + 1}–{Math.min(total, page * PAGE + PAGE)} / {total}</span>
      <Button size="iconSm" disabled={page <= 0} onClick={() => onPage(page - 1)} aria-label="이전"><ChevronRight className="rotate-180" /></Button>
      <Button size="iconSm" disabled={page >= pages - 1} onClick={() => onPage(page + 1)} aria-label="다음"><ChevronRight /></Button>
    </div>
  )
}
const Panel = ({ children, className }: { children: ReactNode; className?: string }) =>
  <div className={`flex min-w-0 flex-col gap-2.5 rounded-lg border border-border bg-card p-3 ${className ?? ''}`}>{children}</div>
const Mono = ({ children }: { children: ReactNode }) => <span className="font-mono text-sm">{children}</span>
const NameCell = ({ r }: { r: Rec }) => <div><Mono>{r.name}</Mono>{r.note ? <div className="text-xs text-muted-foreground">{r.note}</div> : null}</div>
function DetailHead({ kind, name, onDelete, onRename, canEdit }: { kind: string; name: string; onDelete?: () => void; onRename?: () => void; canEdit: boolean }) {
  return (
    <div className="flex items-center gap-2 px-1 pt-1">
      <Badge variant="brandSoft">{kind} · {KO[kind]}</Badge>
      <span className="font-mono text-md font-semibold">{name}</span>
      <span className="flex-1" />
      {onRename && <Button variant="ghost" onClick={onRename} disabled={!canEdit} title={canEdit ? '' : 'operator 권한 필요'}><Pencil />이름 바꾸기</Button>}
      {onDelete && <Button onClick={onDelete} disabled={!canEdit} title={canEdit ? '' : 'operator 권한 필요'}><Trash2 />삭제</Button>}
    </div>
  )
}
function Uses({ items, empty }: { items: Array<{ tag: string; name: string; hint?: string; go?: () => void }>; empty: string }) {
  if (!items.length) return <div className="py-2 text-center text-sm text-muted-foreground">{empty}</div>
  return (
    <div className="flex flex-col gap-1">
      {items.map(u => (
        <div key={u.tag + u.name} className="flex items-center gap-2 rounded-sm border border-border px-2 py-1.5 text-md">
          <Badge variant="neutralSoft">{u.tag}</Badge><Mono>{u.name}</Mono>
          {u.hint && <span className="text-xs text-muted-foreground">{u.hint}</span>}
          <span className="flex-1" />
          {u.go && <Button variant="ghost" onClick={u.go}>보기<ArrowRight /></Button>}
        </div>
      ))}
    </div>
  )
}

// ── 변경 미리보기 — 운영자가 읽는 말로 "전 → 후". 레코드는 id(서버가 준 것)로 맞추고, 없으면 name 으로 ──
const CH_COLS: Array<[sp.WCol, string]> = [['remote_nodes', 'Remote Node'], ['routes', 'Route'], ['route_sets', 'Route Set'], ['rules', 'Rule'],
  ['rule_sets', 'Rule Set'], ['routing_policies', 'Routing Policy'], ['acl_policies', 'ACL Policy']]
const ruleText = (X: Data, n: string) => { const r = X.rules.find(x => x.name === n); return r ? sentence(r) : `(없는 Rule ${n})` }
function rsetText(X: Data, n: string | undefined) {
  if (!n) return '모든 호 (조건 없음)'
  const s = X.rule_sets.find(x => x.name === n); if (!s) return `(없는 Rule Set ${n})`
  const ms: Rec[] = s.members ?? []; if (!ms.length) return '모든 호 (빈 Rule Set)'
  return ms.map(m => `${ruleText(X, m.rule_ref)}${m.negate ? ' — 아님' : ''}`).join(s.combinator === 'OR' ? ' 또는 ' : ' 그리고 ')
}
const onOff = (v: unknown) => v === false ? '꺼짐' : '사용'
const koV = (v: unknown) => { const t = String(v ?? ''); return KO[t] ? `${KO[t]} (${t})` : (t || '—') }
/** [항목 이름, 값 문자열] — 컬렉션별로 운영자가 보는 항목만 */
function viewOf(col: sp.WCol, r: Rec, X: Data): Array<[string, string]> {
  const note: Array<[string, string]> = [['설명', r.note || '—'], ['사용', onOff(r.enabled)]]
  switch (col) {
    case 'remote_nodes': return [['주소', `${r.ip ?? '?'}:${r.port ?? '?'}`], ['프로토콜', r.protocol ?? 'UDP'], ['코덱 변환', (r.transcode_codecs ?? []).join(',') || '—'], ...note]
    case 'routes': return [['잇는 것', `${r.local_node_ref ?? '?'} → ${r.remote_node_ref ?? '?'}`], ['수신 인증', koV(r.inbound_auth ?? 'none')], ['국가 코드', r.country_code || '—'], ...note]
    case 'route_sets': return [['분배 방식', koV(r.distribution_policy ?? 'failover')],
        ['구성 Route', (r.members ?? []).map((m: Rec) => `${m.route_ref} (순위 ${m.priority ?? 100} · weight ${m.weight ?? 1})`).join(', ') || '없음'],
        ['생존 감시', (r.health_check_mode ?? 'options_ping') === 'none' ? '안 함' : `OPTIONS ${r.health_check_interval_sec ?? 30}초마다 · ${r.health_check_dead_threshold ?? 3}번 실패면 끊김`], ...note]
    case 'rules': return [['조건', sentence(r)], ...note]
    case 'rule_sets': return [['이럴 때 일치', rsetText(X, r.name)], ...note]
    case 'routing_policies': return [['우선순위', String(r.priority ?? 100)], ['이럴 때', `${r.match_rule_set_ref || '—'} : ${rsetText(X, r.match_rule_set_ref)}`],
      ['여기로', (r.target_type ?? 'route_set') === 'route_set' ? (r.target_ref || '—') : `${r.target_type} ${r.target_ref ?? ''}`],
      ['보낼 Route 가 없으면', (r.fail_action ?? 'next_policy') === 'reject' ? '거절' : '다음 정책으로'], ...note]
    case 'acl_policies': return [['우선순위', String(r.priority ?? 100)], ['이럴 때', `${r.match_rule_set_ref || '—'} : ${rsetText(X, r.match_rule_set_ref)}`],
      ['어디에', r.scope === 'global' || !r.scope ? '전체' : `${KO[r.scope] ?? r.scope} ${r.scope_ref ?? ''}`], ['동작', koV(r.action ?? 'deny')], ...note]
  }
}
interface Change { kind: 'add' | 'del' | 'chg'; name: string; oldName?: string; view: Array<[string, string]>; diffs: Array<[string, string, string]> }
function describeChanges(B: Data, A: Data) {
  const out: Array<{ col: sp.WCol; label: string; items: Change[] }> = []
  for (const [col, label] of CH_COLS) {
    const items: Change[] = []; const used = new Set<Rec>()
    const find = (r: Rec) => A[col].find(x => (r.id && x.id === r.id) || (!r.id && x.name === r.name)) ?? A[col].find(x => x.name === r.name && !x.id)
    for (const b of B[col]) {
      const a = find(b)
      if (!a) { items.push({ kind: 'del', name: b.name, view: viewOf(col, b, B), diffs: [] }); continue }
      used.add(a)
      const vb = viewOf(col, b, B), va = viewOf(col, a, A)
      const diffs = va.map(([k, v], i) => [k, vb[i]?.[1] ?? '', v] as [string, string, string]).filter(([, o, n]) => o !== n)
      if (diffs.length || a.name !== b.name) items.push({ kind: 'chg', name: a.name, oldName: a.name !== b.name ? b.name : undefined, view: va, diffs })
    }
    for (const a of A[col]) if (!used.has(a)) items.push({ kind: 'add', name: a.name, view: viewOf(col, a, A), diffs: [] })
    if (items.length) out.push({ col, label, items })
  }
  return out
}
const polOrder = (X: Data) => [...X.routing_policies].filter(p => p.enabled !== false)
  .sort((a, b) => (a.priority ?? 100) - (b.priority ?? 100) || String(a.name).localeCompare(String(b.name)))
/** 적용 후 라우팅 순서 + 바뀐 표시 */
function routingOutcome(B: Data, A: Data) {
  const bo = polOrder(B), ao = polOrder(A)
  const key = (p: Rec) => p.id ?? p.name
  const line = (X: Data, p: Rec) => `${rsetText(X, p.match_rule_set_ref)} → ${p.target_ref || '—'}`
  const rows = ao.map((p, i) => { const bi = bo.findIndex(x => key(x) === key(p)); const bp = bi >= 0 ? bo[bi] : undefined
    const now = line(A, p), was = bp ? line(B, bp) : ''
    const tag = !bp ? '새로' : (now !== was || bp.name !== p.name) ? '바뀜' : bi !== i ? '순서' : ''
    return { n: i + 1, was_n: bi >= 0 ? bi + 1 : null, name: p.name, now, was, tag } })
  const gone = bo.filter(p => !ao.some(x => key(x) === key(p))).map(p => ({ name: p.name, was: line(B, p) }))
  return { rows, gone }
}

// ── 삭제 안내 — 지우려는 것을 참조하는 곳을 "먼저 할 일" 순서로 (등록의 반대). 빼기로 끝나는 건 지우지 않는다 ──
const COL_TAB: Record<string, Tab> = { remote_nodes: 'rnodes', routes: 'routes', route_sets: 'sets', rules: 'rules', rule_sets: 'rsets', routing_policies: 'policies', acl_policies: 'acls' }
const COL_KIND: Record<string, string> = { remote_nodes: 'Remote Node', routes: 'Route', route_sets: 'Route Set', rules: 'Rule', rule_sets: 'Rule Set', routing_policies: 'Routing Policy', acl_policies: 'ACL Policy' }
interface DelStep { key: string; col: string; name: string; act: 'delete' | 'unlink' | 'delete_or_change'; member?: string; text: string }
function deleteSteps(D: Data, col: string, name: string, seen = new Set<string>()): DelStep[] {
  const out: DelStep[] = []
  const add = (x: DelStep) => { if (!seen.has(x.key)) { seen.add(x.key); out.push(x) } }
  if (col === 'remote_nodes') D.routes.filter(r => r.remote_node_ref === name).forEach(r => {
    out.push(...deleteSteps(D, 'routes', r.name, seen))   // 안쪽이 이미 seen 에 넣었다 — 그대로 잇는다
    add({ key: `del:routes:${r.name}`, col: 'routes', name: r.name, act: 'delete', text: `Route ${r.name} 지우기 — 이 Remote Node 를 잇는 경로` }) })
  if (col === 'routes') {
    D.route_sets.filter(x => (x.members ?? []).some((m: Rec) => m.route_ref === name)).forEach(x =>
      add({ key: `un:${x.name}:${name}`, col: 'route_sets', name: x.name, act: 'unlink', member: name, text: `Route Set ${x.name} 의 members 에서 Route ${name} 빼기 (Route Set 은 지우지 않는다)` }))
    D.acl_policies.filter(a => a.scope === 'route' && a.scope_ref === name).forEach(a =>
      add({ key: `acl:${a.name}`, col: 'acl_policies', name: a.name, act: 'delete_or_change', text: `ACL Policy ${a.name} 지우기 또는 범위(scope) 바꾸기 — 이 Route 에 걸려 있다` })) }
  if (col === 'route_sets') {
    D.routing_policies.filter(p => (p.target_type ?? 'route_set') === 'route_set' && p.target_ref === name).forEach(p =>
      add({ key: `pol:${p.name}`, col: 'routing_policies', name: p.name, act: 'delete_or_change', text: `Routing Policy ${p.name} 지우기 또는 대상(여기로) 바꾸기 — 호를 이 Route Set 으로 보낸다` }))
    D.acl_policies.filter(a => a.scope === 'route_set' && a.scope_ref === name).forEach(a =>
      add({ key: `acl:${a.name}`, col: 'acl_policies', name: a.name, act: 'delete_or_change', text: `ACL Policy ${a.name} 지우기 또는 범위(scope) 바꾸기 — 이 Route Set 에 걸려 있다` })) }
  if (col === 'rules') D.rule_sets.filter(x => (x.members ?? []).some((m: Rec) => m.rule_ref === name)).forEach(x =>
    add({ key: `un:${x.name}:${name}`, col: 'rule_sets', name: x.name, act: 'unlink', member: name, text: `Rule Set ${x.name} 의 members 에서 Rule ${name} 빼기 (Rule Set 은 지우지 않는다)` }))
  if (col === 'rule_sets') {
    D.routing_policies.filter(p => p.match_rule_set_ref === name || (p.transform_rule_set_refs ?? []).includes(name)).forEach(p =>
      add({ key: `pol:${p.name}`, col: 'routing_policies', name: p.name, act: 'delete_or_change', text: `Routing Policy ${p.name} 지우기 또는 조건(이럴 때) 바꾸기 — 이 Rule Set 을 쓴다` }))
    D.acl_policies.filter(a => a.match_rule_set_ref === name).forEach(a =>
      add({ key: `acl:${a.name}`, col: 'acl_policies', name: a.name, act: 'delete_or_change', text: `ACL Policy ${a.name} 지우기 또는 조건 바꾸기 — 이 Rule Set 을 쓴다` })) }
  return out
}

// ── 이름 바꾸기 — CSP 는 전부 name 으로 참조하므로 참조하는 곳을 같은 묶음 안에서 함께 바꾼다 ──
type RenameCol = 'remote_nodes' | 'routes' | 'route_sets' | 'rules' | 'rule_sets' | 'routing_policies' | 'acl_policies'
/** old 를 참조하는 곳 목록 [컬렉션, 레코드 name, 필드] */
function refsTo(D: Data, col: RenameCol, old: string): Array<[string, string, string]> {
  const out: Array<[string, string, string]> = []
  if (col === 'remote_nodes') D.routes.forEach(r => r.remote_node_ref === old && out.push(['routes', r.name, 'remote_node_ref']))
  if (col === 'routes') {
    D.route_sets.forEach(s => (s.members ?? []).some((m: Rec) => m.route_ref === old) && out.push(['route_sets', s.name, 'members.route_ref']))
    D.acl_policies.forEach(a => a.scope === 'route' && a.scope_ref === old && out.push(['acl_policies', a.name, 'scope_ref'])) }
  if (col === 'route_sets') {
    D.routing_policies.forEach(p => (p.target_type ?? 'route_set') === 'route_set' && p.target_ref === old && out.push(['routing_policies', p.name, 'target_ref']))
    D.acl_policies.forEach(a => a.scope === 'route_set' && a.scope_ref === old && out.push(['acl_policies', a.name, 'scope_ref'])) }
  if (col === 'rules') D.rule_sets.forEach(s => (s.members ?? []).some((m: Rec) => m.rule_ref === old) && out.push(['rule_sets', s.name, 'members.rule_ref']))
  if (col === 'rule_sets') {
    D.routing_policies.forEach(p => { if (p.match_rule_set_ref === old) out.push(['routing_policies', p.name, 'match_rule_set_ref'])
      if ((p.transform_rule_set_refs ?? []).includes(old)) out.push(['routing_policies', p.name, 'transform_rule_set_refs']) })
    D.acl_policies.forEach(a => a.match_rule_set_ref === old && out.push(['acl_policies', a.name, 'match_rule_set_ref'])) }
  return out
}
function renameIn(D: Data, col: RenameCol, old: string, nu: string): Data {
  const X: Data = { ...D, [col]: D[col].map(r => r.name === old ? { ...r, name: nu } : r) }
  const m = (c: sp.Col, f: (r: Rec) => Rec) => { X[c] = X[c].map(f) }
  if (col === 'remote_nodes') m('routes', r => r.remote_node_ref === old ? { ...r, remote_node_ref: nu } : r)
  if (col === 'routes') {
    m('route_sets', s => ({ ...s, members: (s.members ?? []).map((x: Rec) => x.route_ref === old ? { ...x, route_ref: nu } : x) }))
    m('acl_policies', a => a.scope === 'route' && a.scope_ref === old ? { ...a, scope_ref: nu } : a) }
  if (col === 'route_sets') {
    m('routing_policies', p => (p.target_type ?? 'route_set') === 'route_set' && p.target_ref === old ? { ...p, target_ref: nu } : p)
    m('acl_policies', a => a.scope === 'route_set' && a.scope_ref === old ? { ...a, scope_ref: nu } : a) }
  if (col === 'rules') m('rule_sets', s => ({ ...s, members: (s.members ?? []).map((x: Rec) => x.rule_ref === old ? { ...x, rule_ref: nu } : x) }))
  if (col === 'rule_sets') {
    m('routing_policies', p => ({ ...p, ...(p.match_rule_set_ref === old ? { match_rule_set_ref: nu } : {}),
      ...((p.transform_rule_set_refs ?? []).includes(old) ? { transform_rule_set_refs: p.transform_rule_set_refs.map((x: string) => x === old ? nu : x) } : {}) }))
    m('acl_policies', a => a.match_rule_set_ref === old ? { ...a, match_rule_set_ref: nu } : a) }
  return X
}
const RENAME_NOTE: Partial<Record<RenameCol, string>> = {
  remote_nodes: 'CSP 의 끊김 알람(A-COM-003) 대상 이름에 Remote Node 이름이 들어간다 — 알람 기록이 옛 이름과 새 이름으로 갈린다.',
  routes: 'CSP 가 Route 이름에 붙여 들고 있는 상태(생존 판정 · 분배 순서)가 처음부터 다시 쌓인다. 호는 끊기지 않는다.',
  route_sets: 'CSP 가 Route Set 이름에 붙여 들고 있는 분배 순서가 처음부터 다시 시작한다. 호는 끊기지 않는다.',
}

// ── 판정 (호 따라가기 — CSP ModuleDispatcher 의 순서를 화면에서 흉내) ──
// 흉내 낸 요청 — 옛 이름은 CSP 호출부가 채우는 값(legacy), 새 문법은 원천별 주소(addr)·헤더 값(hdr)
interface Addr { user: string; host: string; port?: string; scheme?: string; display?: string; params?: Record<string, string> }
interface Ctx { legacy: Record<string, string>; callee: string; addr: Record<string, Addr | undefined>; hdr: Record<string, string[]>
  src: { ip: string; port: string }; ln: string; method: string }
/** field 의 값들 (CspRuleField RuleFieldValues 흉내). host = 대소문자 무시 비교 */
function ctxValues(ctx: Ctx, field: string): { v: string[]; host: boolean } {
  if (field in LEGACY || field === 'req_uri_user' || field === 'dst_ip') return { v: [ctx.legacy[field] ?? ''], host: false }
  const f = parseField(field); if (!f.ok) return { v: [], host: false }
  if (f.k === 'callee') return { v: ctx.callee ? [ctx.callee] : [], host: false }
  if (f.k === 'src') return { v: [f.part === 'port' ? ctx.src.port : ctx.src.ip], host: false }
  if (f.k === 'local_node') return { v: [ctx.ln], host: false }
  if (f.k === 'method') return { v: [ctx.method], host: false }
  const key = f.k === 'header' || f.hdr ? f.hdr.toLowerCase() : f.k
  const a = ctx.addr[key]
  if (!f.part) return { v: ctx.hdr[key] ?? (a ? [`<${a.scheme ?? 'sip'}:${a.user}@${a.host}>`] : []), host: false }
  if (!a) return { v: [], host: f.part === 'host' }
  const one = f.part === 'param' ? a.params?.[f.param.toLowerCase()] : f.part === 'host' ? a.host.toLowerCase() : f.part === 'scheme' ? (a.scheme ?? 'sip') : (a as any)[f.part]
  return { v: one == null ? [] : [String(one)], host: f.part === 'host' }
}
function cidrHas(ip: string, c: string) {
  const [n, b] = c.split('/'); if (!isIp(ip) || !isIp(n)) return false
  const toI = (x: string) => x.split('.').reduce((a, o) => ((a << 8) + (+o)) >>> 0, 0)
  const bits = +b || 32, mask = bits === 0 ? 0 : (~0 << (32 - bits)) >>> 0
  return (toI(ip) & mask) === (toI(n) & mask)
}
function evalRule(r: Rec | undefined, ctx: Ctx) {
  if (!r || r.enabled === false) return false
  const { v: vs, host } = ctxValues(ctx, r.field), val0 = String(r.value ?? ''), val = host ? val0.toLowerCase() : val0
  const lc = (x: string) => host ? x.toLowerCase() : x
  if (r.op === 'exists') return vs.some(x => x !== '')
  if (r.op === 'not_exists') return !vs.some(x => x !== '')
  if (r.op === 'ne') return !vs.some(x => lc(x) === val)
  return (vs.length ? vs : ['']).some(x => evalOne(lc(x), r.op, val))
}
// 값 하나에 op — 여러 값의 결합은 evalRule (하나라도 맞으면 참, ne · not_exists 는 하나도 안 맞으면 참)
function evalOne(v: string, op: string, val: string) {
  switch (op) {
    case 'eq': return v === val
    case 'ne': return v !== val
    case 'prefix': return v.startsWith(val)
    case 'suffix': return v.endsWith(val)
    case 'contains': return v.includes(val)
    case 'regex': try { return new RegExp(val).test(v) } catch { return false }
    case 'in_cidr': return cidrHas(v, val)
    case 'in_list': return val.split(',').map(s => s.trim()).includes(v)
    case 'in_range': { const [a, b] = val.split(/[-~]/); const x = digits(v), lo = digits(a), hi = digits(b)
      return !!lo && !!hi && lo.length === hi.length && x.length === lo.length && x >= lo && x <= hi }
  }
  return false
}

export default function SipPeeringPage() {
  const toast = useToast()
  const confirm = useConfirm()
  const { user } = useAuth()
  const canEdit = hasRole(user, 'operator')
  const navigate = useNavigate()

  const [deps, setDeps] = useState<Deployment[]>([])
  const [depId, setDepId] = useState<number | null>(null)
  const [base, setBase] = useState<Data | null>(null)
  const [data, setData] = useState<Data | null>(null)
  const [ibcf, setIbcf] = useState<boolean | null>(null)
  const [loading, setLoading] = useState(false)
  const [err, setErr] = useState('')
  const [saving, setSaving] = useState(false)

  const [tab, setTab] = useState<Tab>('lns')
  const [sel, setSel] = useState<Record<Tab, string>>({ lns: '', rnodes: '', routes: '', sets: '', rules: '', rsets: '', policies: '', acls: '' })
  const [q, setQ] = useState<Record<string, string>>({})
  const [pg, setPg] = useState<Record<string, number>>({})
  const [chk, setChk] = useState<Record<string, string[]>>({})
  const [dlg, setDlg] = useState<Rec | null>(null)
  const [traceOpen, setTraceOpen] = useState(false)
  const [trace, setTrace] = useState({ from: '+821012345678', to: '+82310001234@ims.kt.test', ln: '', src: '203.0.113.77', ua: '' })

  // ── 로드 ──
  useEffect(() => {
    sp.listCspDeployments().then(ds => { setDeps(ds); if (ds.length) setDepId(ds[0].id) })
      .catch(e => setErr(`CSP 배포 목록을 읽지 못했다 — ${e?.message ?? e}`))
  }, [])
  const reload = useCallback(async (id: number) => {
    setLoading(true); setErr('')
    try {
      const [d, r] = await Promise.all([sp.loadAll(id), sp.loadIbcfRole(id)])
      setBase(d); setData(structuredClone(d)); setIbcf(r); setChk({})
      setTrace(t => ({ ...t, ln: t.ln || (d.local_nodes[0]?.name ?? '') }))
    } catch (e: any) { setErr(`CSP 컬렉션을 읽지 못했다 — ${e?.message ?? e}`) }
    finally { setLoading(false) }
  }, [])
  useEffect(() => { if (depId != null) reload(depId) }, [depId, reload])

  // Local Node 편집은 시스템/인프라 › 서버 › [패키지 설정] (HA 그룹이면 그룹 쪽 — collections 가 scope=service)
  const openLnEditor = () => { const dep = deps.find(d => d.id === depId); if (!dep) return
    navigate((dep as any).ha_group_id ? `/deploy/servers?group=${(dep as any).ha_group_id}&t=config&pkg=${encodeURIComponent(dep.package_name ?? 'csp')}&col=local_nodes` : `/deploy/servers?agent=${dep.agent_id}&t=config&dep=${dep.id}&col=local_nodes`) }
  // ── 편집 도우미 ──
  const tryDelete = (col: sp.Col, name: string) => {
    if (!data) return
    if (deleteSteps(data, col, name).length) { setDlg({ kind: 'blocked', col, name }); return }
    setData(d => d ? { ...d, [col]: d[col].filter(r => r.name !== name) } : d); setSel(x => ({ ...x, [COL_TAB[col]]: '' }))
  }
  const D = data
  const setCol = (c: sp.Col, f: (rs: Rec[]) => Rec[]) => setData(d => d ? { ...d, [c]: f(d[c]) } : d)
  const upd = (c: sp.Col, name: string, patch: Rec) => setCol(c, rs => rs.map(r => r.name === name ? { ...r, ...patch } : r))
  const delRec = (c: sp.Col, name: string) => setCol(c, rs => rs.filter(r => r.name !== name))
  const addRec = (c: sp.Col, r: Rec) => setCol(c, rs => [r, ...rs])
  const selName = (t: Tab) => sel[t]
  const pick = (t: Tab, n: string) => setSel(s => ({ ...s, [t]: n }))
  const go = (t: Tab, n?: string) => { setTab(t); if (n != null) pick(t, n) }
  const qv = (k: string) => q[k] ?? ''
  const setQv = (k: string, v: string) => { setQ(s => ({ ...s, [k]: v })); setPg(s => ({ ...s, [k]: 0 })) }
  const pgv = (k: string) => pg[k] ?? 0
  const chkv = (k: string) => chk[k] ?? []
  const toggleChk = (k: string, n: string, on: boolean) => setChk(s => ({ ...s, [k]: on ? [...new Set([...(s[k] ?? []), n])] : (s[k] ?? []).filter(x => x !== n) }))

  // ── 참조 ──
  const rel = useMemo(() => {
    if (!D) return null
    const routesOfRn = (n: string) => D.routes.filter(r => r.remote_node_ref === n)
    const routesOfLn = (n: string) => D.routes.filter(r => r.local_node_ref === n)
    const setsOfRoute = (n: string) => D.route_sets.filter(s => (s.members ?? []).some((m: Rec) => m.route_ref === n))
    const polsOfSet = (n: string) => D.routing_policies.filter(p => (p.target_type ?? 'route_set') === 'route_set' && p.target_ref === n)
    const aclsOfScope = (scope: string, n: string) => D.acl_policies.filter(a => a.scope === scope && a.scope_ref === n)
    const rsetsOfRule = (n: string) => D.rule_sets.filter(s => (s.members ?? []).some((m: Rec) => m.rule_ref === n))
    const usesOfRset = (n: string) => [...D.routing_policies.filter(p => p.match_rule_set_ref === n).map(p => ({ t: 'Routing Policy', r: p })),
                                       ...D.acl_policies.filter(a => a.match_rule_set_ref === n).map(a => ({ t: 'ACL Policy', r: a }))]
    const ordered = [...D.routing_policies].filter(p => p.enabled !== false).sort((a, b) => (a.priority ?? 100) - (b.priority ?? 100) || String(a.name).localeCompare(String(b.name)))
    return { routesOfRn, routesOfLn, setsOfRoute, polsOfSet, aclsOfScope, rsetsOfRule, usesOfRset, ordered }
  }, [D])

  const changed = useMemo(() => (base && D ? sp.changedCols(base, D) : []), [base, D])
  const nChanged = useMemo(() => (base && D ? changed.reduce((s, c) => { const x = sp.diffCol(base[c], D[c]); return s + x.add + x.chg + x.del }, 0) : 0), [base, D, changed])

  // ── 검사 ──
  const validate = useCallback(() => {
    const out: Array<{ lv: 'err' | 'warn' | 'info'; where: string; msg: string }> = []
    if (!D || !rel) return out
    const E = (lv: 'err' | 'warn' | 'info', where: string, msg: string) => out.push({ lv, where, msg })
    const names = (c: sp.Col) => { const seen = new Set<string>(); D[c].forEach(r => { if (!r.name) E('err', c, 'name 이 빈 레코드가 있다.'); else if (seen.has(r.name)) E('err', `${c} ${r.name}`, 'name 이 겹친다.'); seen.add(r.name) }) }
    sp.WRITE_ORDER.forEach(names)
    D.remote_nodes.forEach(n => {
      if (!isIp(String(n.ip ?? '')) && n.ip !== '0.0.0.0') E('warn', `remote_nodes ${n.name}`, `ip "${n.ip ?? ''}" 가 IP 리터럴이 아니다 — 들어오는 호를 이 Remote Node 로 식별하지 못한다.`)
      if (portErr(n.port)) E('err', `remote_nodes ${n.name}`, portErr(n.port))
      if (!rel.routesOfRn(n.name).length) E('info', `remote_nodes ${n.name}`, '쓰는 Route 가 없다.')
    })
    const pair = new Map<string, string>()
    D.routes.forEach(r => {
      const ln = byName(D.local_nodes, r.local_node_ref), rn = byName(D.remote_nodes, r.remote_node_ref)
      if (!ln) E('err', `routes ${r.name}`, `local_node_ref "${r.local_node_ref ?? ''}" 가 없다 — CSP 가 이 Route 를 끈다.`)
      if (!rn) E('err', `routes ${r.name}`, `remote_node_ref "${r.remote_node_ref ?? ''}" 가 없다 — CSP 가 이 Route 를 끈다.`)
      const k = `${r.local_node_ref}|${r.remote_node_ref}`
      if (pair.has(k)) E('err', `routes ${r.name}`, `(local_node_ref, remote_node_ref) 쌍이 ${pair.get(k)} 와 겹친다.`); else pair.set(k, r.name)
      if (ln && rn && ln.protocol && rn.protocol && ln.protocol !== rn.protocol) E('warn', `routes ${r.name}`, `Local Node ${ln.name} 는 ${ln.protocol}, Remote Node ${rn.name} 는 ${rn.protocol} — 들어오는 호 식별에서 맞지 않는다.`)
      if (!rel.setsOfRoute(r.name).length) E('info', `routes ${r.name}`, '어느 Route Set 에도 없어 이 Route 로 나가는 호가 없다.')
    })
    D.route_sets.forEach(s => {
      ;(s.members ?? []).forEach((m: Rec) => { if (!byName(D.routes, m.route_ref)) E('err', `route_sets ${s.name}`, `members 의 route_ref "${m.route_ref}" 가 없다.`) })
      if (!(s.members ?? []).length) E(rel.polsOfSet(s.name).length ? 'err' : 'warn', `route_sets ${s.name}`, 'members 가 비었다.')
    })
    D.rules.forEach(r => {
      const pf = parseField(r.field)
      if (!pf.ok) E('err', `rules ${r.name}`, `field "${r.field}" 는 CSP 가 모른다 — <원천>[.<부분>] 형식이다.`)
      else if (pf.off) E('warn', `rules ${r.name}`, `field ${r.field} 는 ${pf.off} — 이 Rule 은 늘 불일치로 판정된다.`)
      if (r.op !== 'exists' && r.op !== 'not_exists' && !String(r.value ?? '').trim()) E('err', `rules ${r.name}`, 'value 가 비었다.')
      if (r.op === 'in_range') { const [a, b] = String(r.value ?? '').split(/[-~]/); if (!b || digits(a).length !== digits(b).length) E('err', `rules ${r.name}`, `in_range "${r.value}" 의 두 끝 자릿수가 다르거나 "시작-끝" 형식이 아니다.`) }
      if (!rel.rsetsOfRule(r.name).length) E('info', `rules ${r.name}`, '어느 Rule Set 에서도 쓰지 않는다.')
    })
    D.rule_sets.forEach(s => (s.members ?? []).forEach((m: Rec) => { if (!byName(D.rules, m.rule_ref)) E('err', `rule_sets ${s.name}`, `members 의 rule_ref "${m.rule_ref}" 가 없다.`) }))
    D.routing_policies.forEach(p => {
      if (!p.match_rule_set_ref) E('warn', `routing_policies ${p.name}`, '조건이 비어 모든 호가 일치한다 — 내부 가입자 호까지 이 정책으로 간다.')
      else if (!byName(D.rule_sets, p.match_rule_set_ref)) E('err', `routing_policies ${p.name}`, `match_rule_set_ref "${p.match_rule_set_ref}" 가 없다.`)
      if ((p.target_type ?? 'route_set') === 'route_set' && !byName(D.route_sets, p.target_ref)) E('err', `routing_policies ${p.name}`, `target_ref "${p.target_ref ?? ''}" Route Set 이 없다.`)
      if ((p.target_type ?? 'route_set') === 'route_set' && ibcf === false) E('warn', `routing_policies ${p.name}`, 'IBCF 역할이 꺼져 있어 이 정책에 맞는 호는 403 이 된다.')
    })
    D.acl_policies.forEach(a => {
      if (!byName(D.rule_sets, a.match_rule_set_ref)) E('err', `acl_policies ${a.name}`, `match_rule_set_ref "${a.match_rule_set_ref ?? ''}" 가 없다 — CSP 가 이 정책을 건너뛴다.`)
      const ref = a.scope === 'local_node' ? D.local_nodes : a.scope === 'route' ? D.routes : a.scope === 'route_set' ? D.route_sets : null
      if (ref && !byName(ref, a.scope_ref)) E('err', `acl_policies ${a.name}`, `scope_ref "${a.scope_ref ?? ''}" 가 없다.`)
    })
    return out
  }, [D, rel, ibcf])

  // ── 저장 ──
  const apply = async () => {
    if (!base || !D || depId == null) return
    setSaving(true)
    try {
      const r = await sp.saveAll(depId, base, D)
      if (r.conflict) { toast.show(`${r.conflict} 를 그 사이 다른 곳에서 바꿨다 — 다시 읽은 뒤 고친다`, 'err'); return }
      toast.show(`적용했다 — ${r.written.join(', ')} 저장 · CSP 재적재 1번`)
      setDlg(null); await reload(depId)
    } catch (e: any) { toast.show(`저장 실패 — ${e?.message ?? e}`, 'err') }
    finally { setSaving(false) }
  }

  if (err) return <div className="p-5"><Alert variant="danger"><AlertDescription>{err}</AlertDescription></Alert></div>
  if (!D || !rel || !base) return <div className="p-5 text-md text-muted-foreground">{loading ? '읽는 중…' : deps.length ? '' : 'CSP 배포가 없다.'}</div>

  // ── 단계별 화면 ──
  const counts: Record<Tab, number> = { lns: D.local_nodes.length, rnodes: D.remote_nodes.length, routes: D.routes.length, sets: D.route_sets.length,
    rules: D.rules.length, rsets: D.rule_sets.length, policies: D.routing_policies.length, acls: D.acl_policies.length }
  const flow = (label: string, from: number, to: number) => (
    <div className="grid grid-cols-[64px_minmax(0,1fr)] items-center gap-2.5">
      <b className="text-md">{label}</b>
      <div className="flex flex-wrap items-stretch gap-1.5">
        {STEPS.slice(from, to).map(([k, t], j) => (
          <div key={k} className="flex min-w-[170px] flex-1 items-center gap-1.5">
            {j > 0 && <ChevronRight size={14} className="shrink-0 text-muted-foreground" />}
            <button type="button" onClick={() => setTab(k)}
                    className={`flex flex-1 items-center gap-2 rounded-md border px-3 py-2 text-left text-md ${tab === k ? 'border-primary bg-brandsoft text-brandsoft-on' : 'border-border bg-card hover:bg-accent'}`}>
              <span className={`inline-flex size-5 shrink-0 items-center justify-center rounded-full text-xs font-semibold ${tab === k ? 'bg-primary text-primary-foreground' : 'bg-neutral-soft text-neutral-on'}`}>{from + j + 1}</span>
              <span><b>{t}</b><Ko t={t} /> <span className="font-mono text-xs text-muted-foreground">({counts[k]})</span></span>
            </button>
          </div>
        ))}
      </div>
    </div>
  )

  const split = (left: ReactNode, right: ReactNode) => <div className="grid grid-cols-1 items-start gap-3.5 xl:grid-cols-[minmax(0,1fr)_460px]">{left}{right}</div>
  const empty = (t: string) => <Panel><EmptyState title={t} /></Panel>
  const filt = (rs: Rec[], k: string, keys: string[]) => { const s = qv(k).toLowerCase(); return s ? rs.filter(r => keys.some(x => String(r[x] ?? '').toLowerCase().includes(s))) : rs }
  const pageOf = (rs: Rec[], k: string) => rs.slice(pgv(k) * PAGE, pgv(k) * PAGE + PAGE)

  // 1. Local Node — 읽기
  const lnsView = () => {
    const l = byName(D.local_nodes, selName('lns')) ?? D.local_nodes[0]
    return split(
      <Panel>
        <DataTable><thead><tr><Th>name</Th><Th>edge</Th><Th>bind_ip:bind_port / protocol</Th><Th align="right">쓰는 Route</Th><Th>쓰는 access_services</Th></tr></thead><tbody>
          {D.local_nodes.map(x => { const n = rel.routesOfLn(x.name).length; const sv = D.access_services.filter(a => (a.allowed_local_node_refs ?? []).includes(x.name) || a.inbound_policy !== 'restricted')
            return <TrLink key={x.name} selected={l?.name === x.name} onClick={() => pick('lns', x.name)}>
              <Td><NameCell r={x} /></Td><Td>{x.edge}<Ko t={x.edge} /></Td><Td mono>{x.bind_ip}:{x.bind_port} {x.protocol}</Td><Td align="right" mono>{n || orDash('')}</Td>
              <Td>{sv.length ? sv.map(a => <Badge key={a.name} variant="neutralSoft" className="mr-1">{a.name}</Badge>) : orDash('')}</Td></TrLink> })}
        </tbody></DataTable>
        <div className="flex flex-wrap items-center gap-2"><p className="flex-1 text-xs text-muted-foreground">Local Node 는 CSP 가 SIP 를 받는 주소다. 소켓을 여닫는 설정이라 이 화면에서는 읽기만 한다 — Primary 나 마지막 UDP 접속점을 지우면 CSP 가 기동하지 못한다.</p>
          <Button onClick={openLnEditor}><ExternalLink />시스템/인프라에서 편집</Button></div>
      </Panel>,
      l ? <Panel>
        <div className="flex items-center gap-2"><div className="flex-1"><DetailHead kind="Local Node" name={l.name} canEdit={false} /></div><Button variant="ghost" onClick={openLnEditor}><ExternalLink />시스템/인프라에서 편집</Button></div>
        <SubSection level={1} title="local_nodes">
          <FormField label="edge"><span>{l.edge}<Ko t={l.edge} /></span></FormField>
          <FormField label="bind_ip:bind_port"><Mono>{l.bind_ip}:{l.bind_port}</Mono></FormField>
          <FormField label="protocol"><Mono>{l.protocol}</Mono></FormField>
          <FormField label="is_primary"><span>{l.is_primary ? '예' : orDash('')}</span></FormField>
        </SubSection>
        <SubSection level={1} title="이 Local Node 를 쓰는 Route" count={rel.routesOfLn(l.name).length}>
          <Uses empty="없음" items={rel.routesOfLn(l.name).slice(0, 10).map(r => ({ tag: 'Route', name: r.name, hint: `→ ${r.remote_node_ref}`, go: () => go('routes', r.name) }))} />
        </SubSection>
        <SubSection level={1} title="이 Local Node 를 범위로 쓰는 ACL Policy" count={rel.aclsOfScope('local_node', l.name).length}>
          <Uses empty="없음" items={rel.aclsOfScope('local_node', l.name).map(a => ({ tag: 'ACL', name: a.name, go: () => go('acls', a.name) }))} />
        </SubSection>
      </Panel> : empty('Local Node 가 없다'))
  }

  // 2. Remote Node
  const rnodesView = () => {
    const list = filt(D.remote_nodes, 'rnodes', ['name', 'note', 'ip']); const c = chkv('rnodes')
    const n = byName(D.remote_nodes, selName('rnodes'))
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('rnodes')} onChange={v => setQv('rnodes', v)} placeholder="name · note · ip" /><span className="flex-1" />
          <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newrn', name: '', note: '', ip: '', port: 5060, protocol: 'UDP', withRoute: true, ln: D.local_nodes[0]?.name ?? '', rset: '', err: '' })}><Plus />Remote Node</Button></div>
        {c.length > 0 && <div className="flex flex-wrap items-center gap-2 rounded-md bg-brandsoft px-2.5 py-1.5 text-sm font-semibold text-brandsoft-on">{c.length}개 선택<span className="flex-1" />
          <Button onClick={() => setDlg({ kind: 'bulk', col: 'remote_nodes', key: 'rnodes', field: 'protocol', value: 'UDP' })}><Pencil />값 한꺼번에 바꾸기…</Button>
          <Button onClick={() => { setCol('remote_nodes', rs => rs.map(r => c.includes(r.name) ? { ...r, enabled: false } : r)); setChk(s => ({ ...s, rnodes: [] })) }}>disable</Button>
          <Button onClick={() => { setCol('remote_nodes', rs => rs.map(r => c.includes(r.name) ? { ...r, enabled: true } : r)); setChk(s => ({ ...s, rnodes: [] })) }}>enable</Button></div>}
        {list.length ? <DataTable><thead><tr><Th className="w-9" /><Th>name</Th><Th>ip:port</Th><Th>protocol</Th><Th>transcode_codecs</Th><Th align="right">쓰는 Route</Th><Th>enabled</Th></tr></thead><tbody>
          {pageOf(list, 'rnodes').map(x => <TrLink key={x.name} selected={n?.name === x.name} onClick={() => pick('rnodes', x.name)}>
            <Td onClick={e => e.stopPropagation()}><Checkbox checked={c.includes(x.name)} onCheckedChange={v => toggleChk('rnodes', x.name, v === true)} aria-label="선택" /></Td>
            <Td><NameCell r={x} /></Td><Td mono>{x.ip}:{x.port}</Td><Td mono>{x.protocol}</Td><Td mono>{orDash((x.transcode_codecs ?? []).join(','))}</Td>
            <Td align="right" mono>{rel.routesOfRn(x.name).length || orDash('')}</Td><Td><StatusDot tone={x.enabled === false ? 'neutral' : 'success'} label={x.enabled === false ? 'disabled' : 'enabled'} /></Td></TrLink>)}
        </tbody></DataTable> : <EmptyState title="Remote Node 가 없다" description="상대 장비(IBCF·IP-PBX·MGCF)의 주소를 추가한다." />}
        <Pager page={pgv('rnodes')} total={list.length} onPage={p => setPg(s => ({ ...s, rnodes: p }))} />
      </Panel>,
      n ? <Panel>
        <DetailHead kind="Remote Node" name={n.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'remote_nodes', tab: 'rnodes', old: n.name, name: n.name, err: '' })} onDelete={() => tryDelete('remote_nodes', n.name)} />
        <SubSection level={1} title="remote_nodes">
          <FormField label="note"><Input value={n.note ?? ''} disabled={!canEdit} onChange={e => upd('remote_nodes', n.name, { note: e.target.value })} /></FormField>
          <FormField label="ip · port" help="IP 리터럴이어야 들어오는 호를 식별한다"><div className="flex gap-2"><Input className="font-mono" value={n.ip ?? ''} disabled={!canEdit} onChange={e => upd('remote_nodes', n.name, { ip: e.target.value.trim() })} />
            <Input className="w-24 font-mono" type="number" value={n.port ?? ''} disabled={!canEdit} aria-invalid={!!portErr(n.port)} onChange={e => upd('remote_nodes', n.name, { port: portVal(e.target.value) })} /></div></FormField>
          <FormField label="protocol"><Seg value={n.protocol ?? 'UDP'} disabled={!canEdit} onChange={v => upd('remote_nodes', n.name, { protocol: v })} opts={[['UDP', 'UDP'], ['TCP', 'TCP'], ['TLS', 'TLS']]} /></FormField>
          <FormField label="transcode_codecs" help="예 PCMA,PCMU — CMP 가 G.711↔AMR-WB 변환"><Input className="font-mono" value={(n.transcode_codecs ?? []).join(',')} disabled={!canEdit}
            onChange={e => upd('remote_nodes', n.name, { transcode_codecs: e.target.value.split(',').map(s => s.trim()).filter(Boolean) })} /></FormField>
          <FormField label="enabled"><Seg value={String(n.enabled !== false)} disabled={!canEdit} onChange={v => upd('remote_nodes', n.name, { enabled: v === 'true' })} opts={[['true', 'enabled'], ['false', 'disabled']]} /></FormField>
        </SubSection>
        <SubSection level={1} title="이 Remote Node 를 쓰는 Route" count={rel.routesOfRn(n.name).length}
                    right={<Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newroute', rn: n.name, ln: D.local_nodes[0]?.name ?? '', name: '', auth: 'none', cc: '', rset: '', err: '' })}><Plus />Route</Button>}>
          <Uses empty="Route 가 없다 — 이 Remote Node 로는 호가 오가지 않는다" items={rel.routesOfRn(n.name).map(r => ({ tag: 'Route', name: r.name, hint: `${r.local_node_ref} →`, go: () => go('routes', r.name) }))} />
        </SubSection>
      </Panel> : empty('Remote Node 를 고른다'))
  }

  // 3. Route
  const routesView = () => {
    const list = filt(D.routes, 'routes', ['name', 'note', 'remote_node_ref', 'local_node_ref']); const c = chkv('routes')
    const r = byName(D.routes, selName('routes'))
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('routes')} onChange={v => setQv('routes', v)} placeholder="name · note · Local/Remote Node" /><span className="flex-1" />
          <Button disabled={!canEdit || !D.remote_nodes.length} title={D.remote_nodes.length ? '' : 'Remote Node 를 먼저 만든다'}
                  onClick={() => setDlg({ kind: 'newroute', rn: D.remote_nodes[0]?.name ?? '', ln: D.local_nodes[0]?.name ?? '', name: '', auth: 'none', cc: '', rset: '', err: '' })}><Plus />Route</Button></div>
        {c.length > 0 && <div className="flex flex-wrap items-center gap-2 rounded-md bg-brandsoft px-2.5 py-1.5 text-sm font-semibold text-brandsoft-on">{c.length}개 선택<span className="flex-1" />
          <Button onClick={() => setDlg({ kind: 'pickset', q: '', sel: '' })}>Route Set 에 추가…</Button>
          <Button onClick={() => setDlg({ kind: 'bulk', col: 'routes', key: 'routes', field: 'local_node_ref', value: D.local_nodes[0]?.name ?? '' })}><Pencil />값 한꺼번에 바꾸기…</Button>
          <Button onClick={() => { setCol('routes', rs => rs.map(x => c.includes(x.name) ? { ...x, enabled: false } : x)); setChk(s => ({ ...s, routes: [] })) }}>disable</Button>
          <Button onClick={() => { setCol('routes', rs => rs.map(x => c.includes(x.name) ? { ...x, enabled: true } : x)); setChk(s => ({ ...s, routes: [] })) }}>enable</Button></div>}
        {list.length ? <DataTable><thead><tr><Th className="w-9" /><Th>name</Th><Th>local_node_ref</Th><Th>remote_node_ref</Th><Th>inbound_auth</Th><Th>country_code</Th><Th>Route Set</Th><Th>enabled</Th></tr></thead><tbody>
          {pageOf(list, 'routes').map(x => { const rn = byName(D.remote_nodes, x.remote_node_ref); return <TrLink key={x.name} selected={r?.name === x.name} onClick={() => pick('routes', x.name)}>
            <Td onClick={e => e.stopPropagation()}><Checkbox checked={c.includes(x.name)} onCheckedChange={v => toggleChk('routes', x.name, v === true)} aria-label="선택" /></Td>
            <Td><NameCell r={x} /></Td><Td mono>{x.local_node_ref}</Td><Td><Mono>{x.remote_node_ref}</Mono>{rn ? <div className="font-mono text-xs text-muted-foreground">{rn.ip}:{rn.port}</div> : <Badge variant="dangerSoft">없음</Badge>}</Td>
            <Td mono>{x.inbound_auth ?? 'none'}</Td><Td mono>{orDash(x.country_code)}</Td><Td mono>{orDash(rel.setsOfRoute(x.name).map(s => s.name).join(', '))}</Td>
            <Td><StatusDot tone={x.enabled === false ? 'neutral' : 'success'} label={x.enabled === false ? 'disabled' : 'enabled'} /></Td></TrLink> })}
        </tbody></DataTable> : <EmptyState title="Route 가 없다" description="Local Node 와 Remote Node 를 한 쌍으로 잇는다." />}
        <Pager page={pgv('routes')} total={list.length} onPage={p => setPg(s => ({ ...s, routes: p }))} />
      </Panel>,
      r ? <Panel>
        <DetailHead kind="Route" name={r.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'routes', tab: 'routes', old: r.name, name: r.name, err: '' })} onDelete={() => tryDelete('routes', r.name)} />
        <SubSection level={1} title="routes" hint="Local Node ↔ Remote Node 한 쌍">
          <FormField label="note"><Input value={r.note ?? ''} disabled={!canEdit} onChange={e => upd('routes', r.name, { note: e.target.value })} /></FormField>
          <FormField label="local_node_ref"><Sel mono value={r.local_node_ref ?? ''} disabled={!canEdit} onChange={v => upd('routes', r.name, { local_node_ref: v })} opts={D.local_nodes.map(l => [l.name, `${l.name} (${l.edge} · ${l.protocol})`])} /></FormField>
          <FormField label="remote_node_ref"><Sel mono value={r.remote_node_ref ?? ''} disabled={!canEdit} onChange={v => upd('routes', r.name, { remote_node_ref: v })} opts={D.remote_nodes.map(n => [n.name, `${n.name} · ${n.ip}:${n.port}`])} /></FormField>
          <FormField label="inbound_auth" help="none = 주소로 신뢰 · digest = 등록형 트렁크 계정"><Seg value={r.inbound_auth ?? 'none'} disabled={!canEdit} onChange={v => upd('routes', r.name, { inbound_auth: v })} opts={[['none', 'none'], ['digest', 'digest']]} /></FormField>
          <FormField label="country_code" help={r.country_code ? `국내형 010… → +${r.country_code}10…` : '비우면 번호 변환 안 함'}><Input className="w-24 font-mono" value={r.country_code ?? ''} disabled={!canEdit} onChange={e => upd('routes', r.name, { country_code: e.target.value.trim() })} /></FormField>
          <FormField label="enabled"><Seg value={String(r.enabled !== false)} disabled={!canEdit} onChange={v => upd('routes', r.name, { enabled: v === 'true' })} opts={[['true', 'enabled'], ['false', 'disabled']]} /></FormField>
        </SubSection>
        <SubSection level={1} title="이 Route 가 들어간 Route Set" count={rel.setsOfRoute(r.name).length}>
          <Uses empty="없음 — 이 Route 로 나가는 호가 없다" items={rel.setsOfRoute(r.name).map(s => ({ tag: 'Route Set', name: s.name, go: () => go('sets', s.name) }))} />
        </SubSection>
      </Panel> : empty('Route 를 고른다'))
  }

  // 4. Route Set
  const setsView = () => {
    const list = filt(D.route_sets, 'sets', ['name', 'note']); const s = byName(D.route_sets, selName('sets')); const c = chkv('members')
    const mem: Rec[] = s?.members ?? []
    const setMem = (f: (m: Rec[]) => Rec[]) => s && upd('route_sets', s.name, { members: f(mem) })
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('sets')} onChange={v => setQv('sets', v)} placeholder="name · note" /><span className="flex-1" />
          <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newname', col: 'route_sets', tab: 'sets', title: 'Route Set', name: 'to-', err: '',
            make: (name: string) => ({ name, enabled: true, distribution_policy: 'failover', members: [], health_check_mode: 'options_ping', health_check_interval_sec: 30, health_check_dead_threshold: 3, health_check_recovery_probes: 1 }) })}><Plus />Route Set</Button></div>
        {list.length ? <DataTable><thead><tr><Th>name</Th><Th>distribution_policy</Th><Th align="right">members</Th><Th>health_check_mode</Th><Th>Routing Policy</Th></tr></thead><tbody>
          {pageOf(list, 'sets').map(x => { const u = rel.polsOfSet(x.name).length; return <TrLink key={x.name} selected={s?.name === x.name} onClick={() => { pick('sets', x.name); setChk(v => ({ ...v, members: [] })) }}>
            <Td><NameCell r={x} /></Td><Td><Mono>{x.distribution_policy}</Mono><Ko t={x.distribution_policy} /></Td><Td align="right" mono>{(x.members ?? []).length}</Td>
            <Td mono>{x.health_check_mode ?? 'options_ping'}</Td><Td mono>{u || orDash('')}</Td></TrLink> })}
        </tbody></DataTable> : <EmptyState title="Route Set 이 없다" description="Route 를 묶고 분배 방식을 정한다. Routing Policy 는 Route Set 을 가리킨다." />}
        <Pager page={pgv('sets')} total={list.length} onPage={p => setPg(v => ({ ...v, sets: p }))} />
      </Panel>,
      s ? <Panel>
        <DetailHead kind="Route Set" name={s.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'route_sets', tab: 'sets', old: s.name, name: s.name, err: '' })} onDelete={() => tryDelete('route_sets', s.name)} />
        <SubSection level={1} title="route_sets" hint="분배 · 생존 감시">
          <FormField label="note"><Input value={s.note ?? ''} disabled={!canEdit} onChange={e => upd('route_sets', s.name, { note: e.target.value })} /></FormField>
          <FormField label="distribution_policy" help={DIST_HELP[s.distribution_policy ?? 'failover']}><Seg value={s.distribution_policy ?? 'failover'} disabled={!canEdit} onChange={v => upd('route_sets', s.name, { distribution_policy: v })} opts={DIST.map(d => [d, <>{d}<Ko t={d} /></>])} /></FormField>
          <FormField label="health_check_mode"><Seg value={s.health_check_mode ?? 'options_ping'} disabled={!canEdit} onChange={v => upd('route_sets', s.name, { health_check_mode: v })} opts={[['options_ping', 'options_ping'], ['none', 'none']]} /></FormField>
          {(s.health_check_mode ?? 'options_ping') === 'options_ping' &&
            <FormField label="interval_sec · dead_threshold" help="초마다 OPTIONS · 연속 실패 횟수면 down"><div className="flex gap-2">
              <Input className="w-20 font-mono" type="number" value={s.health_check_interval_sec ?? 30} disabled={!canEdit} onChange={e => upd('route_sets', s.name, { health_check_interval_sec: +e.target.value })} />
              <Input className="w-20 font-mono" type="number" value={s.health_check_dead_threshold ?? 3} disabled={!canEdit} onChange={e => upd('route_sets', s.name, { health_check_dead_threshold: +e.target.value })} /></div></FormField>}
        </SubSection>
        <SubSection level={1} title="members" count={mem.length} right={<Button disabled={!canEdit} onClick={() => setDlg({ kind: 'addmember', q: '', sel: [] })}><Plus />Route 추가</Button>}>
          {c.length > 0 && <div className="mb-2 flex items-center gap-2 rounded-md bg-brandsoft px-2.5 py-1.5 text-sm font-semibold text-brandsoft-on">{c.length}개 선택<span className="flex-1" />
            <Button onClick={() => { setMem(m => m.filter(x => !c.includes(x.route_ref))); setChk(v => ({ ...v, members: [] })) }}>members 에서 빼기</Button></div>}
          {mem.length ? <DataTable><thead><tr><Th className="w-9" /><Th>{s.distribution_policy === 'failover' || !s.distribution_policy ? 'priority' : 'weight'} <span className="font-normal">· {MEMBER_COL_HELP[s.distribution_policy ?? 'failover']}</span></Th><Th>route_ref</Th><Th>Local Node → Remote Node</Th></tr></thead><tbody>
            {mem.map((m, i) => { const r = byName(D.routes, m.route_ref), rn = r && byName(D.remote_nodes, r.remote_node_ref); const fo = s.distribution_policy === 'failover' || !s.distribution_policy
              return <tr key={m.route_ref + i}>
                <Td><Checkbox checked={c.includes(m.route_ref)} onCheckedChange={v => toggleChk('members', m.route_ref, v === true)} aria-label="선택" /></Td>
                <Td><Input className="h-[30px] w-20 font-mono" type="number" disabled={!canEdit} value={fo ? (m.priority ?? 100) : (m.weight ?? 1)}
                  onChange={e => setMem(ms => ms.map((x, j) => j === i ? { ...x, [fo ? 'priority' : 'weight']: +e.target.value } : x))} /></Td>
                <Td mono>{m.route_ref}{!r && <Badge variant="dangerSoft" className="ml-1">없음</Badge>}</Td>
                <Td className="text-muted-foreground" mono>{r ? `${r.local_node_ref} → ${rn ? `${rn.ip}:${rn.port}` : '?'}` : '—'}</Td></tr> })}
          </tbody></DataTable> : <EmptyState title="members 없음" />}
        </SubSection>
        <SubSection level={1} title="이 Route Set 을 쓰는 규칙" count={rel.polsOfSet(s.name).length + rel.aclsOfScope('route_set', s.name).length}
                    hint="Routing Policy 가 이 Route Set 을 target_ref 로 가리킨다">
          <div className="mb-2 flex flex-wrap gap-2">
            <Button disabled={!canEdit}
                    onClick={() => setDlg({ kind: 'polFromSet', rset: D.rule_sets[0]?.name ?? NEW_COND, ...NEW_COND_INIT, priority: 100, fail: 'reject', name: '', err: '' })}><Plus />이 Route Set 으로 보내는 Routing Policy 만들기</Button>
            <Button variant="ghost" disabled={!canEdit || !D.routing_policies.some(p => p.target_ref !== s.name)} title={D.routing_policies.some(p => p.target_ref !== s.name) ? '' : '대상을 바꿀 Routing Policy 가 없다'}
                    onClick={() => setDlg({ kind: 'retarget', sel: '' })}><LinkIcon />기존 Routing Policy 의 대상을 여기로…</Button>
            <Button variant="ghost" disabled={!canEdit}
                    onClick={() => setDlg({ kind: 'aclFromSet', rset: D.rule_sets[0]?.name ?? NEW_COND, ...NEW_COND_INIT, action: 'deny', name: '', err: '' })}><Plus />이 Route Set 범위 ACL Policy 만들기</Button></div>
          <Uses empty="target_ref 로 쓰는 Routing Policy 가 없다 — 나가는 호가 없다" items={[...rel.polsOfSet(s.name).map(p => ({ tag: 'Routing Policy', name: p.name, hint: `priority ${p.priority ?? 100}`, go: () => go('policies', p.name) })),
            ...rel.aclsOfScope('route_set', s.name).map(a => ({ tag: 'ACL Policy', name: a.name, go: () => go('acls', a.name) }))]} />
        </SubSection>
      </Panel> : empty('Route Set 을 고른다'))
  }

  // 5. Rule
  const fieldPicker = (field: string, onChange: (f: string) => void, disabled?: boolean) => {
    const f = parseField(field), def = SOURCES.find(x => x.k === f.k)
    const to = (p: Partial<FieldRef>) => onChange(buildField({ ...f, ...p }))
    return <div className="flex flex-col gap-1">
      <div className="flex flex-wrap gap-2">
        <Sel className="w-[220px]" value={def?.k ?? ''} disabled={disabled} placeholder="무엇을"
             onChange={k => { const x = SOURCES.find(y => y.k === k)!; to({ k, part: x.parts[0] ?? '', hdr: x.hdr ?? '', param: '' }) }}
             opts={SOURCES.map(x => [x.k, x.label, x.off])} />
        {f.k === 'header' && <Input className="w-[200px] font-mono" value={f.hdr} disabled={disabled} placeholder="헤더 이름 (예 Diversion)"
             onChange={e => to({ hdr: e.target.value.trim() })} />}
        {def && def.parts.length > 1 && <Sel className="w-[160px]" value={f.part} disabled={disabled} onChange={p => to({ part: p, param: p === 'param' ? f.param : '' })}
             opts={def.parts.map(p => [p, PART_KO[p] ?? p])} />}
        {f.part === 'param' && <Input className="w-[140px] font-mono" value={f.param} disabled={disabled} placeholder="이름 (예 reason)"
             onChange={e => to({ param: e.target.value.trim() })} />}
      </div>
      <span className="text-xs text-muted-foreground">field <Mono>{field || '—'}</Mono>{f.legacy && f.k !== 'callee' ? ' (옛 이름 — 뜻은 같다)' : ''}{def?.note ? ` · ${def.note}` : ''}</span>
      {!f.ok && <span className="text-xs text-destructive">{f.k === 'header' && !f.hdr ? '헤더 이름을 넣는다.' : f.part === 'param' && !f.param ? '파라미터 이름을 넣는다.' : 'CSP 가 모르는 field 다.'}</span>}
    </div>
  }
  const rulesView = () => {
    const list = filt(D.rules, 'rules', ['name', 'note', 'value', 'field']); const r = byName(D.rules, selName('rules'))
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('rules')} onChange={v => setQv('rules', v)} placeholder="name · note · field · value" /><span className="flex-1" />
          <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newrule', name: '', note: '', field: 'req_uri_user', op: 'prefix', value: '+8299', addTo: '', err: '' })}><Plus />Rule</Button></div>
        {list.length ? <DataTable><thead><tr><Th>name</Th><Th>무엇을 보나</Th><Th>어떻게</Th><Th>value</Th><Th align="right">쓰는 Rule Set</Th></tr></thead><tbody>
          {pageOf(list, 'rules').map(x => { const u = rel.rsetsOfRule(x.name).length; const off = parseField(x.field).off; return <TrLink key={x.name} selected={r?.name === x.name} onClick={() => pick('rules', x.name)}>
            <Td><NameCell r={x} /></Td><Td>{fieldName(x.field)}{off && <Badge variant="warningSoft" className="ml-1">늘 불일치</Badge>}<div className="font-mono text-xs text-muted-foreground">{x.field}</div></Td>
            <Td>{OPWORD[x.op] ?? x.op}<div className="font-mono text-xs text-muted-foreground">{x.op}</div></Td><Td mono>{orDash(x.value)}</Td><Td align="right" mono>{u || orDash('')}</Td></TrLink> })}
        </tbody></DataTable> : <EmptyState title="Rule 이 없다" description="조건 하나 — Rule Set 에 넣어야 판정에 쓰인다." />}
        <Pager page={pgv('rules')} total={list.length} onPage={p => setPg(v => ({ ...v, rules: p }))} />
      </Panel>,
      r ? <Panel>
        <DetailHead kind="Rule" name={r.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'rules', tab: 'rules', old: r.name, name: r.name, err: '' })} onDelete={() => tryDelete('rules', r.name)} />
        <SubSection level={1} title="rules">
          <FormField label="note"><Input value={r.note ?? ''} disabled={!canEdit} onChange={e => upd('rules', r.name, { note: e.target.value })} /></FormField>
          <FormField label="무엇을 보나 (field)">{fieldPicker(r.field, f => upd('rules', r.name, { field: f }), !canEdit)}</FormField>
          <FormField label="어떻게 (op)"><Sel value={r.op ?? 'eq'} disabled={!canEdit} onChange={v => upd('rules', r.name, { op: v })} opts={Object.entries(OPWORD).map(([k, v]) => [k, `${v} (${k})`])} /></FormField>
          {r.op !== 'exists' && r.op !== 'not_exists' && <FormField label="value (비교할 값)" help={valueHelp(r.field, r.op)}>
            <Input className="font-mono" value={r.value ?? ''} disabled={!canEdit} onChange={e => upd('rules', r.name, { value: e.target.value })} /></FormField>}
          <Alert variant="info"><AlertDescription>{sentence(r)}</AlertDescription></Alert>
        </SubSection>
        <SubSection level={1} title="이 Rule 을 쓰는 Rule Set" count={rel.rsetsOfRule(r.name).length}>
          <Uses empty="쓰는 Rule Set 이 없다 — 판정에 쓰이지 않는다" items={rel.rsetsOfRule(r.name).map(s => ({ tag: 'Rule Set', name: s.name, go: () => go('rsets', s.name) }))} />
        </SubSection>
      </Panel> : empty('Rule 을 고른다'))
  }

  // 6. Rule Set
  const rsetSummary = (s: Rec | undefined) => {
    if (!s) return <Badge variant="warningSoft">조건 없음 — 모든 호</Badge>
    const ms: Rec[] = s.members ?? []; if (!ms.length) return <Badge variant="warningSoft">빈 Rule Set — 모든 호</Badge>
    return <span>{ms.map((m, i) => <span key={i} title={ruleExpr(byName(D.rules, m.rule_ref), m.negate)}>{i > 0 && <span className="text-muted-foreground"> {s.combinator === 'OR' ? '또는' : '그리고'} </span>}{sentence(byName(D.rules, m.rule_ref), m.negate)}</span>)}</span>
  }
  const rsetBlock = (s: Rec, edit: boolean) => {
    const ms: Rec[] = s.members ?? []
    const setMs = (f: (m: Rec[]) => Rec[]) => upd('rule_sets', s.name, { members: f(ms) })
    return <div className="overflow-hidden rounded-sm border border-border-strong">
      <div className="bg-neutral-soft px-3 py-2 text-md">{ms.length ? (s.combinator === 'OR' ? '다음 중 하나라도 맞으면' : '다음이 모두 맞으면') : '조건이 없다 — 모든 호가 일치'} <span className="font-mono text-xs text-muted-foreground">combinator {s.combinator ?? 'AND'}</span></div>
      {ms.map((m, i) => { const r = byName(D.rules, m.rule_ref); return (
        <div key={i} className="grid grid-cols-[48px_minmax(140px,1.2fr)_minmax(80px,.6fr)_minmax(100px,1fr)_auto] items-center gap-2.5 border-t border-border px-3 py-2 text-md">
          {m.negate ? <Badge variant="warningSoft">아님</Badge> : <span />}
          <span><b>{r ? fieldName(r.field) : '?'}</b><div className="font-mono text-xs text-muted-foreground">{r?.field ?? m.rule_ref}</div></span>
          <span className="text-muted-foreground">{r ? OPWORD[r.op] ?? r.op : ''}</span>
          <span className="break-all font-mono text-sm">{r && r.op !== 'exists' && r.op !== 'not_exists' ? r.value : ''}{!r && <Badge variant="dangerSoft">없는 Rule</Badge>}</span>
          {edit ? <span className="flex items-center gap-1">
            <label className="flex items-center gap-1.5 text-sm"><Checkbox checked={!!m.negate} disabled={!canEdit} onCheckedChange={v => setMs(x => x.map((y, j) => j === i ? { ...y, negate: v === true } : y))} />아님</label>
            <Button variant="ghost" onClick={() => go('rules', m.rule_ref)}>Rule<ArrowRight /></Button>
            <Button size="iconSm" disabled={!canEdit} aria-label="빼기" onClick={() => setMs(x => x.filter((_, j) => j !== i))}><X /></Button></span> : <span />}
        </div>) })}
    </div>
  }
  const rsetsView = () => {
    const list = filt(D.rule_sets, 'rsets', ['name', 'note']); const s = byName(D.rule_sets, selName('rsets'))
    const uses = s ? rel.usesOfRset(s.name) : []
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('rsets')} onChange={v => setQv('rsets', v)} placeholder="name · note" /><span className="flex-1" />
          <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newname', col: 'rule_sets', tab: 'rsets', title: 'Rule Set', name: 'match-', err: '', help: '기본 = match-<무엇에 맞는가> (예 match-kt · match-intl) — 정책이 "이럴 때" 로 가리킨다', make: (name: string) => ({ name, enabled: true, combinator: 'OR', members: [] }) })}><Plus />Rule Set</Button></div>
        {list.length ? <DataTable><thead><tr><Th>name</Th><Th>이럴 때 일치</Th><Th align="right">쓰는 곳</Th></tr></thead><tbody>
          {pageOf(list, 'rsets').map(x => <TrLink key={x.name} selected={s?.name === x.name} onClick={() => pick('rsets', x.name)}>
            <Td><NameCell r={x} /></Td><Td className="whitespace-normal font-normal">{rsetSummary(x)}</Td><Td align="right" mono>{rel.usesOfRset(x.name).length || orDash('')}</Td></TrLink>)}
        </tbody></DataTable> : <EmptyState title="Rule Set 이 없다" description="Rule 을 AND / OR 로 묶는다. 정책이 Rule Set 하나를 가리킨다." />}
        <Pager page={pgv('rsets')} total={list.length} onPage={p => setPg(v => ({ ...v, rsets: p }))} />
      </Panel>,
      s ? <Panel>
        <DetailHead kind="Rule Set" name={s.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'rule_sets', tab: 'rsets', old: s.name, name: s.name, err: '' })} onDelete={() => tryDelete('rule_sets', s.name)} />
        <SubSection level={1} title="rule_sets" right={<Seg value={s.combinator ?? 'AND'} disabled={!canEdit} onChange={v => upd('rule_sets', s.name, { combinator: v })} opts={[['OR', '하나라도 (OR)'], ['AND', '모두 (AND)']]} />}>
          <FormField label="note"><Input value={s.note ?? ''} disabled={!canEdit} onChange={e => upd('rule_sets', s.name, { note: e.target.value })} /></FormField>
          {rsetBlock(s, true)}
          <div className="flex flex-wrap gap-2">
            <Sel className="w-[300px]" mono value="" placeholder="Rule 넣기…" disabled={!canEdit} onChange={v => v && upd('rule_sets', s.name, { members: [...(s.members ?? []), { rule_ref: v, negate: false }] })}
                 opts={[['', 'Rule 넣기…'], ...D.rules.filter(r => !(s.members ?? []).some((m: Rec) => m.rule_ref === r.name)).map(r => [r.name, `${r.name} · ${sentence(r)}`] as [string, string])]} />
            <Button variant="ghost" disabled={!canEdit} onClick={() => setDlg({ kind: 'newrule', name: '', note: '', field: 'req_uri_user', op: 'prefix', value: '+8299', addTo: s.name, err: '' })}><Plus />새 Rule 만들어 넣기</Button></div>
          <p className="text-xs text-muted-foreground">CSP 는 Rule Set 안에 Rule Set 을 넣지 못한다 — "공통 Rule 그리고 (A 또는 B)" 는 지금 표현할 수 없다.</p>
        </SubSection>
        <SubSection level={1} title="쓰는 곳" count={uses.length} hint="정책이 이 Rule Set 을 match_rule_set_ref 로 가리킨다">
          <div className="mb-2 flex flex-wrap gap-2">
            <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'mkpolicy', name: '', priority: 100, fail: 'reject', target: D.route_sets[0]?.name ?? '', err: '' })}><Plus />이 Rule Set 으로 Routing Policy 만들기</Button>
            <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'mkacl', name: '', err: '' })}><Plus />이 Rule Set 으로 ACL Policy 만들기</Button>
            <Button variant="ghost" disabled={!canEdit} onClick={() => setDlg({ kind: 'linkpol', sel: '' })}><LinkIcon />기존 정책에 연결…</Button></div>
          <Uses empty="쓰는 곳 없음" items={uses.map(u => ({ tag: u.t, name: u.r.name, go: () => go(u.t === 'Routing Policy' ? 'policies' : 'acls', u.r.name) }))} />
        </SubSection>
      </Panel> : empty('Rule Set 을 고른다'))
  }

  // 7. Routing Policy
  const policiesView = () => {
    const ord = rel.ordered; const idx = new Map(ord.map((p, i) => [p.name, i + 1]))
    const all = [...D.routing_policies].sort((a, b) => (idx.get(a.name) ?? 9999) - (idx.get(b.name) ?? 9999))
    const list = filt(all, 'policies', ['name', 'note', 'target_ref', 'match_rule_set_ref']); const p = byName(D.routing_policies, selName('policies'))
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><Srch value={qv('policies')} onChange={v => setQv('policies', v)} placeholder="name · note · Rule Set · Route Set" />
          <span className="text-xs text-muted-foreground">위에서부터 처음 맞는 것 하나 · 같은 priority 는 name 순</span><span className="flex-1" />
          <Button disabled={!canEdit} onClick={() => setDlg({ kind: 'newname', col: 'routing_policies', tab: 'policies', title: 'Routing Policy', name: policyNameOf(D.rule_sets[0]?.name ?? '', D.route_sets[0]?.name ?? ''), err: '', help: '기본 = <Rule Set>-<Route Set> — 이럴 때 → 여기로. 만든 뒤 조건·대상을 바꾼다',
            make: (name: string) => ({ name, enabled: true, priority: 100, match_rule_set_ref: D.rule_sets[0]?.name ?? '', target_type: 'route_set', target_ref: D.route_sets[0]?.name ?? '', transform_rule_set_refs: [], fail_action: 'reject' }) })}><Plus />Routing Policy</Button></div>
        {list.length ? <DataTable><thead><tr><Th align="right">평가 순서</Th><Th align="right">priority</Th><Th>name</Th><Th>이럴 때</Th><Th>여기로</Th></tr></thead><tbody>
          {pageOf(list, 'policies').map(x => <TrLink key={x.name} selected={p?.name === x.name} onClick={() => pick('policies', x.name)}>
            <Td align="right" mono>{idx.has(x.name) ? `#${idx.get(x.name)}` : <StatusDot tone="neutral" label="disabled" />}</Td><Td align="right" mono>{x.priority ?? 100}</Td><Td><NameCell r={x} /></Td>
            <Td className="whitespace-normal font-normal">{rsetSummary(byName(D.rule_sets, x.match_rule_set_ref))}</Td>
            <Td mono>{(x.target_type ?? 'route_set') === 'route_set' ? x.target_ref : `${x.target_type} ${x.target_ref ?? ''}`}</Td></TrLink>)}
          <tr><Td align="right" className="text-muted-foreground">끝</Td><Td /><Td className="text-muted-foreground">일치 없음</Td><Td className="font-normal text-muted-foreground">어느 Routing Policy 도 안 맞음</Td><Td className="text-muted-foreground">내부 가입자로</Td></tr>
        </tbody></DataTable> : <EmptyState title="Routing Policy 가 없다" description="모든 호가 내부 가입자로 간다." />}
        <Pager page={pgv('policies')} total={list.length} onPage={v => setPg(s => ({ ...s, policies: v }))} />
      </Panel>,
      p ? <Panel>
        <DetailHead kind="Routing Policy" name={p.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'routing_policies', tab: 'policies', old: p.name, name: p.name, err: '' })} onDelete={async () => {
          if (await confirm({ title: `Routing Policy ${p.name} 삭제`, body: '이 정책에 맞던 호는 다음 정책이나 내부 가입자로 간다. 적용 전까지는 되돌릴 수 있다.', confirmLabel: '삭제', tone: 'danger' })) { delRec('routing_policies', p.name); pick('policies', '') } }} />
        <Alert variant="info"><AlertDescription><b>{idx.has(p.name) ? `#${idx.get(p.name)}번째` : '꺼짐'}</b> — {rsetSummary(byName(D.rule_sets, p.match_rule_set_ref))} 이면 → <Mono>{p.target_ref}</Mono> 로 (보낼 Route 가 없으면 {(p.fail_action ?? 'next_policy') === 'reject' ? '거절' : '다음 정책'})</AlertDescription></Alert>
        <SubSection level={1} title="routing_policies">
          <FormField label="note"><Input value={p.note ?? ''} disabled={!canEdit} onChange={e => upd('routing_policies', p.name, { note: e.target.value })} /></FormField>
          <FormField label="priority" help="작을수록 먼저 · 같으면 name 순"><Input className="w-24 font-mono" type="number" value={p.priority ?? 100} disabled={!canEdit} onChange={e => upd('routing_policies', p.name, { priority: +e.target.value })} /></FormField>
          <FormField label="enabled"><Seg value={String(p.enabled !== false)} disabled={!canEdit} onChange={v => upd('routing_policies', p.name, { enabled: v === 'true' })} opts={[['true', 'enabled'], ['false', 'disabled']]} /></FormField>
        </SubSection>
        <SubSection level={1} title="이럴 때 — match_rule_set_ref">
          <FormField label="Rule Set"><Sel mono value={p.match_rule_set_ref ?? ''} disabled={!canEdit} onChange={v => upd('routing_policies', p.name, { match_rule_set_ref: v })} opts={[['', '"" — 모든 호 (catch-all)'], ...D.rule_sets.map(s => [s.name, s.name] as [string, string])]} /></FormField>
          {byName(D.rule_sets, p.match_rule_set_ref) && rsetBlock(byName(D.rule_sets, p.match_rule_set_ref)!, false)}
          {p.match_rule_set_ref && <div className="flex items-center gap-2 text-xs text-muted-foreground">{rel.usesOfRset(p.match_rule_set_ref).length > 1 ? `이 Rule Set 을 ${rel.usesOfRset(p.match_rule_set_ref).length}곳에서 쓴다 — 고치면 전부 바뀐다` : '이 정책만 쓰는 Rule Set'}
            <Button variant="ghost" onClick={() => go('rsets', p.match_rule_set_ref)}>Rule Set 편집<ArrowRight /></Button></div>}
        </SubSection>
        <SubSection level={1} title="여기로 — target_ref">
          {(p.target_type ?? 'route_set') !== 'route_set' && <Alert variant="warning"><AlertDescription>target_type 이 {p.target_type} 이다 — 이 화면은 route_set 대상만 편집한다.</AlertDescription></Alert>}
          <FormField label="Route Set"><Sel mono value={p.target_ref ?? ''} disabled={!canEdit} onChange={v => upd('routing_policies', p.name, { target_type: 'route_set', target_ref: v })} opts={D.route_sets.map(s => [s.name, `${s.name} (${(s.members ?? []).length})`])} /></FormField>
          {byName(D.route_sets, p.target_ref) && <Button variant="ghost" className="self-start" onClick={() => go('sets', p.target_ref)}>연동 4단계에서 보기<ArrowRight /></Button>}
          {(p.fail_action ?? 'next_policy') === 'reject'
            ? <p className="text-xs text-muted-foreground">이 Route Set 에 보낼 Route 가 없으면 거절(403)한다. 예비 경로는 Route Set 의 members 로 둔다(주·예비).</p>
            : <Alert variant="warning"><AlertDescription className="flex flex-wrap items-center gap-2">이 정책은 보낼 Route 가 없을 때 거절하지 않고 다음 정책으로 넘어간다(<Mono>fail_action=next_policy</Mono>).
                <Button disabled={!canEdit} onClick={() => upd('routing_policies', p.name, { fail_action: 'reject' })}>거절로 바꾸기</Button></AlertDescription></Alert>}
        </SubSection>
      </Panel> : empty('Routing Policy 를 고른다'))
  }

  // 8. ACL Policy
  const aclsView = () => {
    const list = [...D.acl_policies].sort((a, b) => (a.priority ?? 100) - (b.priority ?? 100)); const a = byName(D.acl_policies, selName('acls'))
    const refOpts = (scope: string): Array<[string, string]> => scope === 'local_node' ? D.local_nodes.map(l => [l.name, l.name]) : scope === 'route' ? D.routes.map(r => [r.name, r.name]) : scope === 'route_set' ? D.route_sets.map(s => [s.name, s.name]) : []
    return split(
      <Panel>
        <div className="flex flex-wrap items-center gap-2"><span className="text-xs text-muted-foreground">받은 요청마다 Routing Policy 보다 먼저 · 처음 맞는 것 하나 · 아무것도 안 맞으면 허용</span><span className="flex-1" />
          <Button disabled={!canEdit || !D.rule_sets.length} title={D.rule_sets.length ? '' : 'Rule Set 을 먼저 만든다'} onClick={() => setDlg({ kind: 'newname', col: 'acl_policies', tab: 'acls', title: 'ACL Policy', name: aclNameOf(D.rule_sets[0]?.name ?? ''), err: '', help: '기본 = block-<Rule Set> (deny) · allow-<Rule Set> (allow)',
            make: (name: string) => ({ name, enabled: true, priority: 100, match_rule_set_ref: D.rule_sets[0]?.name ?? '', scope: 'global', scope_ref: '', action: 'deny' }) })}><Plus />ACL Policy</Button></div>
        {list.length ? <DataTable><thead><tr><Th align="right">priority</Th><Th>name</Th><Th>이럴 때</Th><Th>scope</Th><Th>action</Th></tr></thead><tbody>
          {list.map(x => <TrLink key={x.name} selected={a?.name === x.name} onClick={() => pick('acls', x.name)}>
            <Td align="right" mono>{x.priority ?? 100}</Td><Td><NameCell r={x} /></Td><Td className="whitespace-normal font-normal">{rsetSummary(byName(D.rule_sets, x.match_rule_set_ref))}</Td>
            <Td><Mono>{x.scope}{x.scope_ref ? ` · ${x.scope_ref}` : ''}</Mono></Td><Td><Badge variant={x.action === 'allow' ? 'successSoft' : 'dangerSoft'}>{x.action} · {KO[x.action ?? 'deny']}</Badge></Td></TrLink>)}
        </tbody></DataTable> : <EmptyState title="ACL Policy 가 없다" description="모든 요청이 허용된다." />}
        <p className="text-xs text-muted-foreground">CSP 에 원래 있는 기능이다(acl_policies). REGISTER 를 포함한 모든 수신 요청에 적용되고 deny 면 403. route · route_set 범위는 대국에서 들어온 요청(들어온 Route 로 식별된 것)에만 걸린다.</p>
      </Panel>,
      a ? <Panel>
        <DetailHead kind="ACL Policy" name={a.name} canEdit={canEdit} onRename={() => setDlg({ kind: 'rename', col: 'acl_policies', tab: 'acls', old: a.name, name: a.name, err: '' })} onDelete={async () => {
          if (await confirm({ title: `ACL Policy ${a.name} 삭제`, body: '이 정책이 막던 요청이 허용된다. 적용 전까지는 되돌릴 수 있다.', confirmLabel: '삭제', tone: 'danger' })) { delRec('acl_policies', a.name); pick('acls', '') } }} />
        <SubSection level={1} title="acl_policies">
          <FormField label="note"><Input value={a.note ?? ''} disabled={!canEdit} onChange={e => upd('acl_policies', a.name, { note: e.target.value })} /></FormField>
          <FormField label="priority"><Input className="w-24 font-mono" type="number" value={a.priority ?? 100} disabled={!canEdit} onChange={e => upd('acl_policies', a.name, { priority: +e.target.value })} /></FormField>
          <FormField label="이럴 때 (match_rule_set_ref)"><Sel mono value={a.match_rule_set_ref ?? ''} disabled={!canEdit} onChange={v => upd('acl_policies', a.name, { match_rule_set_ref: v })} opts={D.rule_sets.map(s => [s.name, s.name])} /></FormField>
          {byName(D.rule_sets, a.match_rule_set_ref) && rsetBlock(byName(D.rule_sets, a.match_rule_set_ref)!, false)}
          <FormField label="scope"><Seg value={a.scope ?? 'global'} disabled={!canEdit} onChange={v => upd('acl_policies', a.name, { scope: v, scope_ref: refOpts(v)[0]?.[0] ?? '' })}
            opts={[['global', 'global'], ['local_node', 'local_node'], ['route', 'route'], ['route_set', 'route_set']]} /></FormField>
          {a.scope && a.scope !== 'global' && <FormField label="scope_ref"><Sel mono value={a.scope_ref ?? ''} disabled={!canEdit} onChange={v => upd('acl_policies', a.name, { scope_ref: v })} opts={refOpts(a.scope)} /></FormField>}
          <FormField label="action"><Seg value={a.action ?? 'deny'} disabled={!canEdit} onChange={v => upd('acl_policies', a.name, { action: v })} opts={[['deny', 'deny · 차단'], ['allow', 'allow · 허용']]} /></FormField>
        </SubSection>
      </Panel> : empty('ACL Policy 를 고른다'))
  }

  // ── 호 따라가기 ──
  const runTrace = () => {
    const steps: Array<{ c: 'success' | 'danger' | 'warning' | 'neutral' | 'info'; t: ReactNode; go?: () => void }> = []
    const [u, h] = trace.to.split('@'); const fu = trace.from.trim()
    const raw = (u ?? '').trim(), e164 = raw.startsWith('+') ? raw : /^0\d+$/.test(raw.replace(/-/g, '')) ? '+82' + raw.replace(/-/g, '').slice(1) : raw
    const dom = (h ?? '').trim()
    const base: Ctx = { legacy: { req_uri_user: raw, req_uri_host: dom, from_uri_user: fu, from_uri_host: 'caller.test', to_uri_user: raw, to_uri_host: dom, src_ip: trace.src, user_agent: trace.ua, method: 'INVITE' },
      callee: '', addr: { request_uri: { user: raw, host: dom }, from: { user: fu, host: 'caller.test' }, to: { user: raw, host: dom },
        contact: { user: fu, host: trace.src, port: '5060' }, via: { user: '', host: trace.src, port: '5060' } },
      hdr: { 'user-agent': trace.ua ? [trace.ua] : [] }, src: { ip: trace.src, port: '5060' }, ln: trace.ln, method: 'INVITE' }
    const ln = byName(D.local_nodes, trace.ln)
    steps.push({ c: 'success', t: <>Local Node <Mono>{trace.ln || '?'}</Mono> ({ln?.edge ?? '?'}) 로 수신 · 보낸 곳 <Mono>{trace.src}</Mono></>, go: () => go('lns', trace.ln) })
    let inRoute: Rec | undefined
    if (ln?.edge === 'peering') {
      inRoute = D.routes.find(r => r.enabled !== false && r.local_node_ref === ln.name && byName(D.remote_nodes, r.remote_node_ref)?.ip === trace.src)
      if (!inRoute) return { steps: [...steps, { c: 'danger' as const, t: 'peering Local Node 인데 보낸 곳과 맞는 Route 가 없다' }], v: { c: 'danger' as const, t: '403 — 모르는 상대' } }
      steps.push({ c: 'success', t: <>들어온 Route <Mono>{inRoute.name}</Mono> 로 식별</>, go: () => go('routes', inRoute!.name) })
    }
    const inSets = inRoute ? rel.setsOfRoute(inRoute.name).map(s => s.name) : []
    const evalSet = (n: string, ctx: Ctx) => { if (!n) return true; const s = byName(D.rule_sets, n); if (!s || s.enabled === false) return false; const ms: Rec[] = s.members ?? []; if (!ms.length) return true
      const rs = ms.map(m => evalRule(byName(D.rules, m.rule_ref), ctx) !== !!m.negate); return s.combinator === 'OR' ? rs.some(Boolean) : rs.every(Boolean) }
    for (const a of [...D.acl_policies].filter(x => x.enabled !== false).sort((x, y) => (x.priority ?? 100) - (y.priority ?? 100))) {
      const ok = a.scope === 'global' || (a.scope === 'local_node' && a.scope_ref === trace.ln) || (a.scope === 'route' && inRoute?.name === a.scope_ref) || (a.scope === 'route_set' && inSets.includes(a.scope_ref))
      if (!ok || !a.match_rule_set_ref || !evalSet(a.match_rule_set_ref, base)) continue
      steps.push({ c: a.action === 'allow' ? 'success' : 'danger', t: <>ACL Policy <Mono>{a.name}</Mono> 일치 — {a.action}</>, go: () => go('acls', a.name) })
      if (a.action !== 'allow') return { steps, v: { c: 'danger' as const, t: '403 — ACL Policy 차단' } }
      break
    }
    steps.push({ c: 'success', t: 'ACL Policy 통과' })
    steps.push({ c: 'success', t: <>착신 <Mono>{raw}</Mono> → <Mono>{e164}</Mono> (Routing 은 변환 뒤 값을 본다)</> })
    const rctx: Ctx = { ...base, legacy: { ...base.legacy, req_uri_user: e164 }, callee: e164 }
    let k = 0
    for (const p of rel.ordered) {
      k++; if (!evalSet(p.match_rule_set_ref ?? '', rctx)) continue
      steps.push({ c: 'success', t: <>Routing Policy <b>#{k}</b> <Mono>{p.name}</Mono> 일치</>, go: () => go('policies', p.name) })
      if ((p.target_type ?? 'route_set') === 'reject') return { steps, v: { c: 'danger' as const, t: '403 — reject 정책' } }
      if ((p.target_type ?? 'route_set') !== 'route_set') return { steps, v: { c: 'info' as const, t: `${p.target_type} — 내부 경로로` } }
      if (ibcf === false) return { steps: [...steps, { c: 'danger' as const, t: 'IBCF 역할이 꺼져 있다 (Setup.Roles.IBCF=false)' }], v: { c: 'danger' as const, t: '403 — IBCF 역할 꺼짐' } }
      const s = byName(D.route_sets, p.target_ref); if (!s) return { steps, v: { c: 'danger' as const, t: '403 — 대상 Route Set 없음' } }
      const ms = [...(s.members ?? [])].sort((a: Rec, b: Rec) => (s.distribution_policy ?? 'failover') === 'failover' ? (a.priority ?? 100) - (b.priority ?? 100) : 0)
      const up = ms.map((m: Rec) => byName(D.routes, m.route_ref)).filter((r): r is Rec => !!r && r.enabled !== false && byName(D.remote_nodes, r.remote_node_ref)?.enabled !== false)
      if (up.length) { const r = up[0], n = byName(D.remote_nodes, r.remote_node_ref)!
        steps.push({ c: 'info', t: <>Route Set <Mono>{s.name}</Mono> ({s.distribution_policy ?? 'failover'}) → <Mono>{r.name}</Mono> → <Mono>{n.ip}:{n.port}</Mono> <span className="text-xs text-muted-foreground">(생존 상태는 이 화면이 모른다)</span></>, go: () => go('sets', s.name) })
        return { steps, v: { c: 'success' as const, t: `${s.name} 로 나간다` } } }
      if ((p.fail_action ?? 'next_policy') === 'reject') return { steps, v: { c: 'danger' as const, t: '403 — 쓸 수 있는 Route 없음' } }
    }
    steps.push({ c: 'neutral', t: `Routing Policy ${rel.ordered.length}개 모두 불일치` })
    return { steps, v: { c: 'info' as const, t: '내부 가입자로 — 거절이 아니다' } }
  }
  const tracePanel = () => { const r = runTrace()
    return <Panel className="xl:sticky xl:top-3">
      <div className="flex items-center gap-2"><b className="text-base">호 따라가기</b><span className="text-xs text-muted-foreground">편집 중인 설정으로 판정 · 줄을 누르면 그 단계로</span><span className="flex-1" />
        <Button size="iconSm" variant="ghost" aria-label="닫기" onClick={() => setTraceOpen(false)}><X /></Button></div>
      <FormField label="From 번호"><Input className="font-mono" value={trace.from} onChange={e => setTrace(t => ({ ...t, from: e.target.value }))} /></FormField>
      <FormField label="착신 (번호@도메인)"><Input className="font-mono" value={trace.to} onChange={e => setTrace(t => ({ ...t, to: e.target.value }))} /></FormField>
      <div className="grid grid-cols-2 gap-2">
        <FormField label="수신 Local Node"><Sel mono value={trace.ln} onChange={v => setTrace(t => ({ ...t, ln: v }))} opts={D.local_nodes.map(l => [l.name, l.name])} /></FormField>
        <FormField label="보낸 곳 IP"><Input className="font-mono" value={trace.src} onChange={e => setTrace(t => ({ ...t, src: e.target.value }))} /></FormField></div>
      <div className="flex flex-col gap-1.5">{r.steps.map((s, i) => s.go
        ? <button key={i} type="button" onClick={s.go} className="rounded-sm border border-transparent px-1 py-0.5 text-left text-md hover:border-border hover:bg-accent"><StatusDot tone={s.c} label="" className="mr-1" />{s.t}</button>
        : <div key={i} className="px-1 py-0.5 text-md"><StatusDot tone={s.c} label="" className="mr-1" />{s.t}</div>)}</div>
      <Alert variant={r.v.c === 'success' ? 'success' : r.v.c === 'danger' ? 'danger' : 'info'}><AlertDescription>{r.v.t}</AlertDescription></Alert>
      <p className="text-xs text-muted-foreground">화면에서 CSP 판정 순서를 흉내 낸 것이다. 번호 변환은 국내형 0… → +82 만 반영한다.</p>
    </Panel> }

  // ── 대화상자 ──
  const newNameOk = (col: sp.Col, name: string) => !name.trim() ? 'name 을 넣는다.' : !NAME_RE.test(name.trim()) ? 'name 은 영문·숫자·- _ . 로 넣는다.' : byName(D[col], name.trim()) ? `${col} 에 ${name.trim()} 이 이미 있다.` : ''
  const dialog = () => { const d = dlg; if (!d) return null
    const close = () => setDlg(null); const set = (p: Rec) => setDlg(x => x ? { ...x, ...p, err: p.err ?? '' } : x)
    // Route Set 에 넣기 — 기존 묶음을 고르거나 그 자리에서 새로 만든다 (NEW_SET)
    const rsetPicker = (d: Rec, set: (p: Rec) => void) => <>
      <FormField label="Route Set 에 넣기" help={D.route_sets.length ? undefined : 'Route Set 이 아직 없다 — 여기서 새로 만들 수 있다'}>
        <Sel mono value={d.rset} onChange={v => set({ rset: v, newSet: v === NEW_SET && !d.newSet ? setName(d.rn || d.name || '') : d.newSet })}
             opts={[['', '(나중에)'], [NEW_SET, '+ 새 Route Set 만들기'], ...D.route_sets.map(s => [s.name, s.name] as [string, string])]} /></FormField>
      {d.rset === NEW_SET && <FormField label="새 Route Set name" required help="기본값 = to-<Remote Node> (이 상대로 보내는 경로 묶음) · failover · options_ping 으로 만든다 — 4단계에서 바꾼다">
        <Input className="font-mono" value={d.newSet ?? ''} onChange={e => set({ newSet: e.target.value })} /></FormField>}</>
    // "이럴 때" — 기존 Rule Set 을 고르거나 새 조건(Rule 1개 + Rule Set)을 그 자리에서 만든다
    const newCondNames = (d: Rec) => { const rn = ruleNameOf(d.cf, d.cop, d.cv); return { rule: rn, rset: `match-${rn}` } }
    const condPicker = (d: Rec, set: (p: Rec) => void) => <>
      <FormField label="이럴 때 (Rule Set)" help="조건 없는 정책(catch-all)은 여기서 만들지 않는다 — 내부 호까지 이리로 간다">
        <Sel mono value={d.rset} onChange={v => set({ rset: v })} opts={[[NEW_COND, '+ 새 조건 만들기'], ...D.rule_sets.map(x => [x.name, x.name] as [string, string])]} /></FormField>
      {d.rset === NEW_COND ? <div className="flex flex-col gap-3 rounded-sm border border-border p-3">
          <FormField label="무엇을 보나 (field)">{fieldPicker(d.cf, f => set({ cf: f, ...(d.cvTouched ? {} : { cv: valueSafe(f) }) }))}</FormField>
          <FormField label="어떻게 (op)"><Sel value={d.cop} onChange={v => set({ cop: v })} opts={Object.entries(OPWORD).map(([k, v]) => [k, `${v} (${k})`])} /></FormField>
          {d.cop !== 'exists' && d.cop !== 'not_exists' && <FormField label="value (비교할 값)" help={valueHelp(d.cf, d.cop)}><Input className="font-mono" value={d.cv} placeholder={valuePh(d.cf)} onChange={e => set({ cv: e.target.value, cvTouched: true })} /></FormField>}
          <p className="text-xs text-muted-foreground">만들면 Rule <Mono>{newCondNames(d).rule}</Mono> + Rule Set <Mono>{newCondNames(d).rset}</Mono> 이 함께 생긴다 — {sentence({ field: d.cf, op: d.cop, value: d.cv })}</p></div>
        : byName(D.rule_sets, d.rset) && rsetBlock(byName(D.rule_sets, d.rset)!, false)}</>
    const condErr = (d: Rec) => { if (d.rset !== NEW_COND) return d.rset ? '' : 'Rule Set 을 고른다.'
      if (!fieldOk(d.cf)) return 'field 를 마저 고른다.'
      if (d.cop !== 'exists' && d.cop !== 'not_exists' && !String(d.cv ?? '').trim()) return 'value 를 넣는다.'
      const n = newCondNames(d); return newNameOk('rules', n.rule) || newNameOk('rule_sets', n.rset) }
    /** 새 조건이면 Rule·Rule Set 을 만들고 쓸 Rule Set 이름을 돌려준다 */
    const condCommit = (d: Rec) => { if (d.rset !== NEW_COND) return d.rset as string
      const n = newCondNames(d); addRec('rules', { name: n.rule, enabled: true, field: d.cf, op: d.cop, value: d.cop === 'exists' || d.cop === 'not_exists' ? '' : d.cv })
      addRec('rule_sets', { name: n.rset, enabled: true, combinator: 'OR', members: [{ rule_ref: n.rule, negate: false }] }); return n.rset }
    const condLabel = (d: Rec) => d.rset === NEW_COND ? newCondNames(d).rset : d.rset
    const rsetErr = (d: Rec) => d.rset === NEW_SET ? newNameOk('route_sets', d.newSet ?? '') : ''
    const putInSet = (d: Rec, route: string) => {
      if (!d.rset) return
      const m = { route_ref: route, priority: 100, weight: 1 }
      if (d.rset === NEW_SET) addRec('route_sets', { name: d.newSet.trim(), enabled: true, distribution_policy: 'failover', members: [m], health_check_mode: 'options_ping', health_check_interval_sec: 30, health_check_dead_threshold: 3, health_check_recovery_probes: 1 })
      else setCol('route_sets', rs => rs.map(x => x.name === d.rset ? { ...x, members: [...(x.members ?? []), m] } : x))
    }
    const foot = (ok: ReactNode) => <div className="mt-2 flex justify-end gap-2 border-t border-border pt-3"><Button size="default" onClick={close}>취소</Button>{ok}</div>
    if (d.kind === 'rename') { const refs = refsTo(D, d.col, d.old); const same = d.name.trim() === d.old
      return <Modal title={`이름 바꾸기 — ${d.old}`} onClose={close} width={640}><div className="flex flex-col gap-3">
        <FormField label="새 name" required error={d.err}><Input className="font-mono" autoFocus value={d.name} onChange={e => set({ name: e.target.value })} /></FormField>
        <div className="flex flex-col gap-1"><span className="text-sm font-medium">함께 바뀌는 참조 {refs.length}곳</span>
          {refs.length ? <div className="flex max-h-[220px] flex-col overflow-auto rounded-sm border border-border-strong">{refs.map(([c, n, f], i) =>
            <div key={i} className="flex items-center gap-2 border-t border-border px-3 py-1.5 text-md first:border-t-0"><Badge variant="neutralSoft">{c}</Badge><Mono>{n}</Mono><span className="ml-auto font-mono text-xs text-muted-foreground">{f}</span></div>)}</div>
            : <span className="text-sm text-muted-foreground">이 이름을 참조하는 곳이 없다.</span>}</div>
        {RENAME_NOTE[d.col as RenameCol] && <Alert variant="warning"><AlertDescription>{RENAME_NOTE[d.col as RenameCol]}</AlertDescription></Alert>}
        <p className="text-xs text-muted-foreground">이름과 참조가 한 묶음으로 저장된다 — 적용 전까지는 되돌리기로 원래대로 돌린다. 변경 미리보기에는 옛 이름 삭제 + 새 이름 추가로 보인다.</p></div>
        {foot(<Button size="default" variant="default" disabled={same} title={same ? '이름이 같다' : ''} onClick={() => {
          const e = newNameOk(d.col, d.name); if (e) { set({ err: e }); return } const nu = d.name.trim()
          setData(x => x ? renameIn(x, d.col, d.old, nu) : x)
          setChk(c => Object.fromEntries(Object.entries(c).map(([k, v]) => [k, v.map(y => y === d.old ? nu : y)])))
          pick(d.tab, nu); close(); toast.show(`${d.old} → ${nu} · 참조 ${refs.length}곳 함께 바꿈 — 적용 전`) }}><Pencil />바꾸기</Button>)}</Modal> }
    if (d.kind === 'polFromSet') { const tgt = selName('sets'); const name = d.touched ? d.name : policyNameOf(condLabel(d), tgt)
      return <Modal title={`${tgt} 로 보내는 Routing Policy 만들기`} onClose={close} width={680}><div className="flex flex-col gap-3">
        {condPicker(d, set)}
        <FormField label="name" required help={d.touched ? '직접 고친 이름' : '기본값 = <Rule Set>-<Route Set> · 조건을 바꾸면 따라 바뀐다'}><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField>
        <FormField label="priority" help="작을수록 먼저"><Input className="w-24 font-mono" type="number" value={d.priority} onChange={e => set({ priority: +e.target.value })} /></FormField>
        {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
        {foot(<Button size="default" variant="default" onClick={() => { const e = condErr(d) || newNameOk('routing_policies', name); if (e) { set({ err: e }); return } const nm = name.trim()
          const rs = condCommit(d)
          addRec('routing_policies', { name: nm, enabled: true, priority: d.priority, match_rule_set_ref: rs, target_type: 'route_set', target_ref: tgt, transform_rule_set_refs: [], fail_action: 'reject' })
          go('policies', nm); close(); toast.show(`${d.rset === NEW_COND ? `Rule + Rule Set ${rs} + ` : ''}Routing Policy ${nm} — 적용 전`) }}><Plus />만들기</Button>)}</Modal> }
    if (d.kind === 'retarget') { const tgt = selName('sets'); const items = rel.ordered.concat(D.routing_policies.filter(p => p.enabled === false)).filter(p => p.target_ref !== tgt); const pk = items.find(p => p.name === d.sel)
      return <Modal title={`기존 Routing Policy 의 대상을 ${tgt} 로`} onClose={close} width={700}><div className="flex flex-col gap-3">
        <p className="text-sm text-muted-foreground">고른 정책의 target_ref 를 {tgt} 로 <b>바꾼다</b>. 그 정책에 맞는 호가 원래 대상 대신 이 Route Set 으로 간다.</p>
        <div className="flex max-h-[300px] flex-col overflow-auto rounded-sm border border-border-strong">{items.map(p => (
          <label key={p.name} className="flex cursor-pointer items-center gap-2.5 border-t border-border px-3 py-2 text-md first:border-t-0 hover:bg-accent">
            <Checkbox checked={d.sel === p.name} onCheckedChange={() => set({ sel: p.name })} /><Mono>{p.name}</Mono>
            <span className="ml-auto font-mono text-xs text-muted-foreground">지금 → {p.target_ref || '—'}</span></label>))}</div>
        {pk && <Alert variant="warning"><AlertDescription><Mono>{pk.name}</Mono> 의 대상이 <b>{pk.target_ref || '없음'}</b> 에서 <b>{tgt}</b> 로 바뀐다.{pk.target_ref && rel.polsOfSet(pk.target_ref).length === 1 ? ` 원래 Route Set ${pk.target_ref} 은 쓰는 정책이 없어진다.` : ''}</AlertDescription></Alert>}</div>
        {foot(<Button size="default" variant="default" disabled={!pk} onClick={() => { upd('routing_policies', pk!.name, { target_type: 'route_set', target_ref: tgt }); close(); toast.show(`${pk!.name} 의 대상을 ${tgt} 로 바꿨다 — 적용 전`) }}><LinkIcon />바꿔서 연결</Button>)}</Modal> }
    if (d.kind === 'aclFromSet') { const tgt = selName('sets'); const name = d.touched ? d.name : aclNameOf(condLabel(d), d.action)
      return <Modal title={`${tgt} 범위 ACL Policy 만들기`} onClose={close} width={680}><div className="flex flex-col gap-3">
        <p className="text-sm text-muted-foreground">scope = route_set · scope_ref = {tgt} — 이 Route Set 의 Route 로 <b>들어온</b> 요청에만 걸린다.</p>
        {condPicker(d, set)}
        <FormField label="action"><Seg value={d.action} onChange={v => set({ action: v })} opts={[['deny', 'deny · 차단'], ['allow', 'allow · 허용']]} /></FormField>
        <FormField label="name" required help={d.touched ? '직접 고친 이름' : '기본값 = block|allow-<Rule Set>'}><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField>
        {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
        {foot(<Button size="default" variant="default" onClick={() => { const e = condErr(d) || newNameOk('acl_policies', name); if (e) { set({ err: e }); return } const nm = name.trim()
          const rs = condCommit(d)
          addRec('acl_policies', { name: nm, enabled: true, priority: 100, match_rule_set_ref: rs, scope: 'route_set', scope_ref: tgt, action: d.action }); go('acls', nm); close() }}><Plus />만들기</Button>)}</Modal> }
    if (d.kind === 'blocked') { const steps = deleteSteps(D, d.col, d.name); const kind = COL_KIND[d.col]
      const unlink = (st: DelStep) => setCol(st.col as sp.Col, rs => rs.map(x => x.name !== st.name ? x : { ...x, members: (x.members ?? []).filter((m: Rec) => (m.route_ref ?? m.rule_ref) !== st.member) }))
      return <Modal title={`${kind} ${d.name} 지우기`} onClose={close} width={720}><div className="flex flex-col gap-3">
        {steps.length ? <>
          <Alert variant="warning"><AlertDescription>{kind} {d.name} 을(를) 참조하는 곳이 있다. 아래를 <b>위에서부터 차례로</b> 처리하면 지울 수 있다 — 등록의 반대 순서다.</AlertDescription></Alert>
          <div className="flex flex-col gap-1.5">{steps.map((st, i) => (
            <div key={st.key} className="flex items-center gap-2.5 rounded-sm border border-border px-3 py-2 text-md">
              <span className="inline-flex size-5 shrink-0 items-center justify-center rounded-full bg-neutral-soft text-xs font-semibold text-neutral-on">{i + 1}</span>
              <span className="min-w-0 flex-1">{st.text}</span>
              {st.act === 'unlink' && <Button disabled={!canEdit} onClick={() => unlink(st)}>빼기</Button>}
              <Button variant="ghost" onClick={() => { go(COL_TAB[st.col], st.name); close() }}>{COL_KIND[st.col]} 보기<ArrowRight /></Button>
            </div>))}
            <div className="flex items-center gap-2.5 px-3 py-2 text-md text-muted-foreground">
              <span className="inline-flex size-5 shrink-0 items-center justify-center rounded-full border border-border text-xs">{steps.length + 1}</span>그다음 {kind} {d.name} 을(를) 지울 수 있다</div></div>
          <p className="text-xs text-muted-foreground">[빼기] 는 이 자리에서 바로 처리된다. 지우기·바꾸기는 [보기] 로 그 단계에 가서 한다 — 돌아와 다시 [삭제] 를 누르면 남은 일만 보인다. 적용 전까지는 되돌리기로 전부 되돌릴 수 있다.</p></>
        : <Alert variant="success"><AlertDescription>이제 참조하는 곳이 없다 — {kind} {d.name} 을(를) 지울 수 있다.</AlertDescription></Alert>}</div>
        {foot(<Button size="default" variant="destructive" disabled={steps.length > 0 || !canEdit} title={steps.length ? '위의 일을 먼저 한다' : ''}
          onClick={() => { tryDelete(d.col, d.name); close(); toast.show(`${kind} ${d.name} 삭제 — 적용 전`) }}><Trash2 />삭제</Button>)}</Modal> }
    if (d.kind === 'newname') return <Modal title={`새 ${d.title}`} onClose={close}><div className="flex flex-col gap-3">
      <FormField label="name" required error={d.err} help={d.help ?? '참조 열쇠 — 만든 뒤에는 바꾸지 않는다'}><Input className="font-mono" autoFocus value={d.name} onChange={e => set({ name: e.target.value })} /></FormField></div>
      {foot(<Button size="default" variant="default" onClick={() => { const e = newNameOk(d.col, d.name); if (e) { set({ err: e }); return } addRec(d.col, d.make(d.name.trim())); go(d.tab, d.name.trim()); close() }}><Plus />만들기</Button>)}</Modal>
    if (d.kind === 'newrn') { const rtName = d.rtTouched ? d.rtName : routeName(d.name, d.ln)
      return <Modal title="Remote Node 추가" onClose={close} width={620}><div className="flex flex-col gap-3">
      <FormField label="name" required help="참조 열쇠 — 만든 뒤에는 바꾸지 않는다"><Input className="font-mono" value={d.name} onChange={e => set({ name: e.target.value })} /></FormField>
      <FormField label="note"><Input value={d.note} onChange={e => set({ note: e.target.value })} /></FormField>
      <FormField label="ip · port" error={d.port === '' ? 'port 를 넣는다.' : undefined}><div className="flex gap-2"><Input className="font-mono" value={d.ip} onChange={e => set({ ip: e.target.value.trim() })} /><Input className="w-24 font-mono" type="number" value={d.port} aria-invalid={!!portErr(d.port)} onChange={e => set({ port: portVal(e.target.value) })} /></div></FormField>
      <FormField label="protocol"><Seg value={d.protocol} onChange={v => set({ protocol: v })} opts={[['UDP', 'UDP'], ['TCP', 'TCP'], ['TLS', 'TLS']]} /></FormField>
      <FormField label="Route 도 만들기" inlineLabel><Checkbox checked={d.withRoute} onCheckedChange={v => set({ withRoute: v === true })} /></FormField>
      {d.withRoute && <>
        <FormField label="local_node_ref"><Sel mono value={d.ln} onChange={v => set({ ln: v })} opts={D.local_nodes.map(l => [l.name, `${l.name} (${l.edge} · ${l.protocol})`])} /></FormField>
        <FormField label="Route name" required help={d.rtTouched ? '직접 고친 이름' : '기본값 = <Remote Node>-<Local Node> · 고칠 수 있다'}>
          <Input className="font-mono" value={rtName} onChange={e => set({ rtName: e.target.value, rtTouched: true })} /></FormField>
        {rsetPicker(d, set)}</>}
      {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
      {foot(<Button size="default" variant="default" disabled={d.port === ''} title={d.port === '' ? 'port 를 넣는다' : ''} onClick={() => {
        const e = newNameOk('remote_nodes', d.name) || (!isIp(d.ip) ? 'ip 는 IP 리터럴로 넣는다.' : '') || portErr(d.port) || (d.withRoute ? newNameOk('routes', rtName) || rsetErr(d) : ''); if (e) { set({ err: e }); return }
        const nm = d.name.trim(), rt = rtName.trim(); addRec('remote_nodes', { name: nm, enabled: true, ip: d.ip, port: d.port, protocol: d.protocol, ...(d.note ? { note: d.note } : {}) })
        if (d.withRoute) { addRec('routes', { name: rt, enabled: true, local_node_ref: d.ln, remote_node_ref: nm, inbound_auth: 'none', ...(d.note ? { note: d.note } : {}) })
          putInSet(d, rt) }
        go('rnodes', nm); close(); toast.show(`Remote Node ${nm}${d.withRoute ? ` + Route ${rt}` : ''}${d.withRoute && d.rset === NEW_SET ? ` + Route Set ${d.newSet.trim()}` : ''} — 적용 전`) }}><Plus />추가</Button>)}</Modal> }
    if (d.kind === 'newroute') { const name = d.touched ? d.name : routeName(d.rn, d.ln)
      return <Modal title="Route 추가" onClose={close} width={620}><div className="flex flex-col gap-3">
      <FormField label="remote_node_ref"><Sel mono value={d.rn} onChange={v => set({ rn: v })} opts={D.remote_nodes.map(n => [n.name, `${n.name} · ${n.ip}:${n.port}`])} /></FormField>
      <FormField label="local_node_ref"><Sel mono value={d.ln} onChange={v => set({ ln: v })} opts={D.local_nodes.map(l => [l.name, `${l.name} (${l.edge} · ${l.protocol})`])} /></FormField>
      <FormField label="name" required help={d.touched ? '직접 고친 이름' : '기본값 = <Remote Node>-<Local Node> · 고칠 수 있다'}><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField>
      <FormField label="inbound_auth"><Seg value={d.auth} onChange={v => set({ auth: v })} opts={[['none', 'none'], ['digest', 'digest']]} /></FormField>
      {rsetPicker(d, set)}
      {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
      {foot(<Button size="default" variant="default" onClick={() => {
        const dup = D.routes.find(r => r.local_node_ref === d.ln && r.remote_node_ref === d.rn)
        const e = newNameOk('routes', name) || (dup ? `(${d.ln}, ${d.rn}) 쌍은 이미 ${dup.name} 가 있다.` : '') || rsetErr(d); if (e) { set({ err: e }); return }
        const nm = name.trim(), note = byName(D.remote_nodes, d.rn)?.note
        addRec('routes', { name: nm, enabled: true, local_node_ref: d.ln, remote_node_ref: d.rn, inbound_auth: d.auth, ...(note ? { note } : {}) })
        putInSet(d, nm)
        go('routes', nm); close() }}><Plus />추가</Button>)}</Modal> }
    if (d.kind === 'addmember' || d.kind === 'pickset') { const s = byName(D.route_sets, selName('sets'))
      const items = d.kind === 'addmember' ? D.routes.filter(r => !(s?.members ?? []).some((m: Rec) => m.route_ref === r.name)) : D.route_sets
      return <Modal title={d.kind === 'addmember' ? `${s?.name} members 에 Route 추가` : `Route ${chkv('routes').length}개를 Route Set 에 추가`} onClose={close} width={620}>
        <div className="flex max-h-[360px] flex-col overflow-auto rounded-sm border border-border-strong">{items.map(x => (
          <label key={x.name} className="flex cursor-pointer items-center gap-2.5 border-t border-border px-3 py-2 text-md first:border-t-0 hover:bg-accent">
            <Checkbox checked={d.kind === 'addmember' ? d.sel.includes(x.name) : d.sel === x.name} onCheckedChange={v => set({ sel: d.kind === 'addmember' ? (v === true ? [...d.sel, x.name] : d.sel.filter((y: string) => y !== x.name)) : x.name })} />
            <Mono>{x.name}</Mono><span className="text-xs text-muted-foreground">{d.kind === 'addmember' ? `${x.local_node_ref} → ${x.remote_node_ref}` : `members ${(x.members ?? []).length}`}</span></label>))}
          {!items.length && <div className="p-4 text-center text-sm text-muted-foreground">없음</div>}</div>
        {foot(<Button size="default" variant="default" disabled={d.kind === 'addmember' ? !d.sel.length : !d.sel} onClick={() => {
          const tgt = d.kind === 'addmember' ? s!.name : d.sel; const add: string[] = d.kind === 'addmember' ? d.sel : chkv('routes')
          setCol('route_sets', rs => rs.map(x => x.name !== tgt ? x : { ...x, members: [...(x.members ?? []), ...add.filter(n => !(x.members ?? []).some((m: Rec) => m.route_ref === n)).map(n => ({ route_ref: n, priority: 100, weight: 1 }))] }))
          if (d.kind === 'pickset') setChk(v => ({ ...v, routes: [] })); close(); toast.show(`Route ${add.length}개를 ${tgt} members 에 추가 — 적용 전`) }}>추가</Button>)}</Modal> }
    if (d.kind === 'bulk') { const defs: Record<string, Array<[string, string[] | null]>> = { remote_nodes: [['protocol', ['UDP', 'TCP', 'TLS']], ['port', null]], routes: [['local_node_ref', D.local_nodes.map(l => l.name)], ['inbound_auth', ['none', 'digest']], ['country_code', null]] }
      const fs = defs[d.col]; const f = fs.find(x => x[0] === d.field) ?? fs[0]; const n = chkv(d.key).length
      return <Modal title={`${n}개 — 값 한꺼번에 바꾸기`} onClose={close} width={600}><div className="flex flex-col gap-3">
        <FormField label="바꿀 필드"><Seg value={f[0]} onChange={v => set({ field: v, value: (fs.find(x => x[0] === v)?.[1] ?? [''])[0] ?? '' })} opts={fs.map(x => [x[0], x[0]])} /></FormField>
        <FormField label="새 값">{f[1] ? <Seg value={d.value} onChange={v => set({ value: v })} opts={f[1].map(v => [v, v])} /> : <Input className="w-48 font-mono" value={d.value} onChange={e => set({ value: e.target.value })} />}</FormField></div>
        {foot(<Button size="default" variant="default" disabled={f[0] === 'port' && !!portErr(d.value)} onClick={() => { const val = f[0] === 'port' ? +d.value : d.value; const names = chkv(d.key)
          setCol(d.col, rs => rs.map(r => names.includes(r.name) ? { ...r, [f[0]]: val } : r)); setChk(v => ({ ...v, [d.key]: [] })); close(); toast.show(`${n}개의 ${f[0]} = ${val} — 적용 전`) }}>{n}개 바꾸기</Button>)}</Modal> }
    if (d.kind === 'newrule') { const name = d.touched ? d.name : ruleNameOf(d.field, d.op, d.value)
      return <Modal title="새 Rule" onClose={close} width={640}><div className="flex flex-col gap-3">
      <FormField label="name" required help={d.touched ? '직접 고친 이름' : '기본값 = <무엇을>-<어떻게>-<값> · 아래를 고치면 따라 바뀐다'}><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField>
      <FormField label="note"><Input value={d.note} onChange={e => set({ note: e.target.value })} /></FormField>
      <FormField label="무엇을 보나 (field)">{fieldPicker(d.field, f => set({ field: f, ...(d.vTouched ? {} : { value: valueSafe(f) }) }))}</FormField>
      <FormField label="어떻게 (op)"><Sel value={d.op} onChange={v => set({ op: v })} opts={Object.entries(OPWORD).map(([k, v]) => [k, `${v} (${k})`])} /></FormField>
      {d.op !== 'exists' && d.op !== 'not_exists' && <FormField label="value (비교할 값)" help={valueHelp(d.field, d.op)}><Input className="font-mono" value={d.value} placeholder={valuePh(d.field)} onChange={e => set({ value: e.target.value, vTouched: true })} /></FormField>}
      <Alert variant="info"><AlertDescription>{sentence(d)}{d.addTo ? ` — 만들고 ${d.addTo} 에 넣는다` : ''}</AlertDescription></Alert>
      {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
      {foot(<Button size="default" variant="default" onClick={() => { const e = newNameOk('rules', name) || (fieldOk(d.field) ? '' : 'field 를 마저 고른다.'); if (e) { set({ err: e }); return } const nm = name.trim()
        addRec('rules', { name: nm, enabled: true, field: d.field, op: d.op, value: d.op === 'exists' || d.op === 'not_exists' ? '' : d.value, ...(d.note ? { note: d.note } : {}) })
        if (d.addTo) setCol('rule_sets', rs => rs.map(s => s.name === d.addTo ? { ...s, members: [...(s.members ?? []), { rule_ref: nm, negate: false }] } : s)); else go('rules', nm)
        close() }}><Plus />{d.addTo ? '만들고 넣기' : '만들기'}</Button>)}</Modal> }
    if (d.kind === 'mkpolicy') { const s = selName('rsets'); const name = d.touched ? d.name : policyNameOf(s, d.target)
      return <Modal title={`${s} 으로 Routing Policy 만들기`} onClose={close} width={620}><div className="flex flex-col gap-3">
        <Alert variant="info"><AlertDescription>이럴 때: {rsetSummary(byName(D.rule_sets, s))}</AlertDescription></Alert>
        <FormField label="name" required help={d.touched ? '직접 고친 이름' : '기본값 = <Rule Set>-<Route Set> · 여기로를 바꾸면 따라 바뀐다'}><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField>
        <FormField label="priority" help="작을수록 먼저"><Input className="w-24 font-mono" type="number" value={d.priority} onChange={e => set({ priority: +e.target.value })} /></FormField>
        <FormField label="여기로 (Route Set)"><Sel mono value={d.target} onChange={v => set({ target: v })} opts={D.route_sets.map(x => [x.name, `${x.name} (${(x.members ?? []).length})`])} placeholder="Route Set 이 없다" /></FormField>
        {d.err && <p className="text-sm text-destructive">{d.err}</p>}</div>
        {foot(<Button size="default" variant="default" disabled={!d.target} onClick={() => { const e = newNameOk('routing_policies', name); if (e) { set({ err: e }); return } const nm = name.trim()
          addRec('routing_policies', { name: nm, enabled: true, priority: d.priority, match_rule_set_ref: s, target_type: 'route_set', target_ref: d.target, transform_rule_set_refs: [], fail_action: 'reject' }); go('policies', nm); close() }}><Plus />만들기</Button>)}</Modal> }
    if (d.kind === 'mkacl') { const s = selName('rsets'); const name = d.touched ? d.name : aclNameOf(s)
      return <Modal title={`${s} 으로 ACL Policy 만들기`} onClose={close}><div className="flex flex-col gap-3">
        <FormField label="name" required error={d.err} help="기본값 = block-<Rule Set> — 만들면 8단계에서 scope 와 action 을 고른다 (기본 global · deny)"><Input className="font-mono" value={name} onChange={e => set({ name: e.target.value, touched: true })} /></FormField></div>
        {foot(<Button size="default" variant="default" onClick={() => { const e = newNameOk('acl_policies', name); if (e) { set({ err: e }); return } const nm = name.trim()
          addRec('acl_policies', { name: nm, enabled: true, priority: 100, match_rule_set_ref: s, scope: 'global', scope_ref: '', action: 'deny' }); go('acls', nm); close() }}><Plus />만들기</Button>)}</Modal> }
    if (d.kind === 'linkpol') { const s = selName('rsets')
      const items = [...rel.ordered.map(p => ({ t: 'Routing Policy', col: 'routing_policies' as sp.Col, r: p })), ...D.acl_policies.map(a => ({ t: 'ACL Policy', col: 'acl_policies' as sp.Col, r: a }))].filter(x => x.r.match_rule_set_ref !== s)
      const pickd = items.find(x => x.r.name === d.sel)
      return <Modal title={`${s} 을 기존 정책에 연결`} onClose={close} width={700}><div className="flex flex-col gap-3">
        <p className="text-sm text-muted-foreground">고른 정책의 match_rule_set_ref 를 {s} 로 <b>바꾼다</b>. 정책은 Rule Set 을 하나만 가리키므로 원래 조건은 빠진다.</p>
        <div className="flex max-h-[300px] flex-col overflow-auto rounded-sm border border-border-strong">{items.map(x => (
          <label key={x.t + x.r.name} className="flex cursor-pointer items-center gap-2.5 border-t border-border px-3 py-2 text-md first:border-t-0 hover:bg-accent">
            <Checkbox checked={d.sel === x.r.name} onCheckedChange={() => set({ sel: x.r.name })} /><Badge variant={x.t === 'ACL Policy' ? 'neutralSoft' : 'brandSoft'}>{x.t === 'ACL Policy' ? 'ACL' : 'Routing'}</Badge>
            <Mono>{x.r.name}</Mono><span className="ml-auto font-mono text-xs text-muted-foreground">지금 {x.r.match_rule_set_ref || '"" (모든 호)'}</span></label>))}
          {!items.length && <div className="p-4 text-center text-sm text-muted-foreground">연결할 정책이 없다</div>}</div>
        {pickd && <Alert variant="warning"><AlertDescription><Mono>{pickd.r.name}</Mono> 의 조건이 <b>{pickd.r.match_rule_set_ref || '모든 호'}</b> 에서 <b>{s}</b> 로 바뀐다.</AlertDescription></Alert>}</div>
        {foot(<Button size="default" variant="default" disabled={!pickd} onClick={() => { upd(pickd!.col, pickd!.r.name, { match_rule_set_ref: s }); close(); toast.show(`${pickd!.r.name} 의 조건을 ${s} 로 바꿨다 — 적용 전`) }}><LinkIcon />바꿔서 연결</Button>)}</Modal> }
    if (d.kind === 'diff') { const ch = describeChanges(base, D); const ro = routingOutcome(base, D)
      const TAG: Record<string, ['successSoft' | 'warningSoft' | 'dangerSoft' | 'infoSoft', string]> = { add: ['successSoft', '추가'], chg: ['warningSoft', '변경'], del: ['dangerSoft', '삭제'] }
      const routingTouched = ro.rows.some(r => r.tag) || ro.gone.length > 0
      return <Modal title="변경 미리보기" onClose={close} wide><div className="flex flex-col gap-4">
        <SubSection level={1} title="적용하면 라우팅이 이렇게 된다" hint={routingTouched ? '위에서부터 처음 맞는 것 하나' : '라우팅 순서·조건·대상은 그대로다'}>
          {ro.rows.length || ro.gone.length ? <DataTable><thead><tr><Th align="right">순서</Th><Th>Routing Policy</Th><Th>이럴 때 → 여기로</Th><Th /></tr></thead><tbody>
            {ro.rows.map(r => <tr key={r.name}><Td align="right" mono>#{r.n}{r.tag === '순서' && <div className="text-xs text-muted-foreground">전 #{r.was_n}</div>}</Td><Td mono>{r.name}</Td>
              <Td className="whitespace-normal font-normal">{r.now}{r.tag === '바뀜' && <div className="text-xs text-muted-foreground line-through">{r.was}</div>}</Td>
              <Td>{r.tag && <Badge variant={r.tag === '새로' ? 'successSoft' : r.tag === '바뀜' ? 'warningSoft' : 'infoSoft'}>{r.tag}</Badge>}</Td></tr>)}
            {ro.gone.map(g => <tr key={'g' + g.name}><Td align="right" className="text-muted-foreground">—</Td><Td mono className="text-muted-foreground line-through">{g.name}</Td>
              <Td className="whitespace-normal font-normal text-muted-foreground line-through">{g.was}</Td><Td><Badge variant="dangerSoft">없어짐</Badge></Td></tr>)}
            <tr><Td align="right" className="text-muted-foreground">끝</Td><Td className="text-muted-foreground">일치 없음</Td><Td className="font-normal text-muted-foreground">내부 가입자로</Td><Td /></tr>
          </tbody></DataTable> : <p className="text-sm text-muted-foreground">Routing Policy 가 없다 — 모든 호가 내부 가입자로 간다.</p>}
        </SubSection>
        <SubSection level={1} title="바뀌는 설정" count={ch.reduce((n, c) => n + c.items.length, 0)}>
          <div className="flex flex-col gap-3">{ch.map(c => <div key={c.col} className="flex flex-col gap-1.5">
            <b className="text-md">{c.label}<Ko t={c.label} /></b>
            {c.items.map(it => <div key={it.kind + it.name} className="rounded-sm border border-border px-3 py-2">
              <div className="flex flex-wrap items-center gap-2"><Badge variant={TAG[it.kind][0]}>{it.oldName ? '이름 바꿈' : TAG[it.kind][1]}</Badge>
                <Mono>{it.oldName ? `${it.oldName} → ${it.name}` : it.name}</Mono></div>
              {it.kind === 'chg' ? (it.diffs.length > 0 && <div className="mt-1.5 grid grid-cols-[140px_minmax(0,1fr)] gap-x-3 gap-y-1 text-md">
                  {it.diffs.map(([k, o, n]) => <Fragment key={k}><span className="text-muted-foreground">{k}</span>
                    <span><span className="text-muted-foreground line-through">{o}</span> <ArrowRight size={12} className="inline" /> <b>{n}</b></span></Fragment>)}</div>)
                : <div className={`mt-1.5 grid grid-cols-[140px_minmax(0,1fr)] gap-x-3 gap-y-1 text-md ${it.kind === 'del' ? 'text-muted-foreground line-through' : ''}`}>
                  {it.view.filter(([k, v]) => !(k === '설명' && v === '—') && !(k === '사용' && v === '사용')).map(([k, v]) => <Fragment key={k}><span className="text-muted-foreground">{k}</span><span>{v}</span></Fragment>)}</div>}
            </div>)}</div>)}
            {!ch.length && <EmptyState title="바뀐 것이 없다" />}</div>
        </SubSection>
        <details className="text-sm"><summary className="cursor-pointer text-muted-foreground">원문 보기 (설정 파일 jsonl · 엔지니어용)</summary>
          <div className="mt-2 flex flex-col gap-2">{changed.map(c => { const x = sp.diffCol(base[c], D[c]); return <div key={c} className="flex flex-col gap-1"><Mono>{c}.jsonl</Mono>
            <pre className="overflow-x-auto rounded-sm border border-border bg-muted p-2.5 font-mono text-xs">{x.lines.slice(0, 30).map(([t, v], i) => <div key={i} className={t === 'add' ? 'text-success-on' : 'text-dangersoft-on'}>{t === 'add' ? '+' : '-'} {v}</div>)}</pre></div> })}</div></details>
        <p className="text-xs text-muted-foreground">적용하면 바뀐 설정만 참조 순서대로 저장하고, CSP 재적재 신호는 마지막에 한 번 보낸다.</p>
      </div></Modal> }
    if (d.kind === 'check') { const is = validate(); const ne = is.filter(x => x.lv === 'err').length, nw = is.filter(x => x.lv === 'warn').length
      const T = { err: ['dangerSoft', '오류'], warn: ['warningSoft', '경고'], info: ['infoSoft', '참고'] } as const
      return <Modal title="검사 결과" onClose={close} wide><div className="flex flex-col gap-3">
        <div className="flex gap-2"><Badge variant={ne ? 'dangerSoft' : 'neutralSoft'}>오류 {ne}</Badge><Badge variant={nw ? 'warningSoft' : 'neutralSoft'}>경고 {nw}</Badge><Badge variant="neutralSoft">참고 {is.length - ne - nw}</Badge>
          <span className="text-sm text-muted-foreground">바뀐 레코드 {nChanged} · 컬렉션 {changed.join(', ') || '—'}</span></div>
        <div className="flex flex-col">{[...is].sort((a, b) => 'ewi'.indexOf(a.lv[0]) - 'ewi'.indexOf(b.lv[0])).slice(0, 60).map((x, i) =>
          <div key={i} className="flex items-start gap-2.5 border-t border-border py-2 text-md first:border-t-0"><Badge variant={T[x.lv][0]}>{T[x.lv][1]}</Badge><span><Mono>{x.where}</Mono> — {x.msg}</span></div>)}
          {!is.length && <EmptyState title="문제 없음" />}</div>
        <p className="text-xs text-muted-foreground">쓰기 직전에 대상 컬렉션을 다시 읽어, 그 사이 다른 곳에서 바뀌었으면 쓰지 않는다.</p></div>
        {foot(<Button size="default" variant="default" disabled={!!ne || !changed.length || saving || !canEdit} onClick={apply}><Check />{saving ? '적용 중…' : '적용'}</Button>)}</Modal> }
    return null
  }

  const views: Record<Tab, () => ReactNode> = { lns: lnsView, rnodes: rnodesView, routes: routesView, sets: setsView, rules: rulesView, rsets: rsetsView, policies: policiesView, acls: aclsView }
  return (
    <div className="flex min-h-full flex-col">
      <div className="flex flex-1 flex-col gap-3.5 p-5 pt-1">
        <div className="flex flex-wrap items-center gap-2.5">
          <h1 className="text-xl font-semibold tracking-tight">SIP 연동</h1>
          {deps.length > 1 ? <Sel className="w-[220px]" mono value={String(depId ?? '')} onChange={v => setDepId(+v)} opts={deps.map(d => [String(d.id), `csp · ${d.agent_name ?? d.id}`])} />
            : <Badge variant="neutralSoft" className="font-mono">csp · {deps[0]?.agent_name ?? depId}</Badge>}
          <span className="flex-1" />
          <Button variant="ghost" onClick={() => depId != null && reload(depId)} disabled={loading || nChanged > 0} title={nChanged ? '적용하지 않은 변경이 있다 — 되돌리기 먼저' : '다시 읽기'}><RefreshCw />다시 읽기</Button>
          <Button onClick={() => setTraceOpen(o => !o)} className={traceOpen ? 'border-primary text-primary' : ''}><RouteIcon />호 따라가기</Button>
        </div>
        {ibcf === false && <Alert variant="warning"><AlertDescription>이 CSP 는 IBCF 역할이 꺼져 있다(<Mono>Setup.Roles.IBCF=false</Mono>). Routing Policy 에 맞는 호는 전부 403 이 된다 — 조건 없는 정책을 만들면 내부 호까지 막힌다.</AlertDescription></Alert>}
        {!canEdit && <Alert variant="info"><AlertDescription>읽기 전용 — 편집은 operator 권한이 필요하다.</AlertDescription></Alert>}
        <div className="flex flex-col gap-2 rounded-lg border border-border bg-card px-3 py-2.5">{flow('연동', 0, 4)}{flow('규칙', 4, 8)}</div>
        <div className={traceOpen ? 'grid grid-cols-1 items-start gap-3.5 2xl:grid-cols-[minmax(0,1fr)_360px]' : ''}>
          <div className="min-w-0">{views[tab]()}</div>{traceOpen && tracePanel()}
        </div>
      </div>
      <StickySaveBar className="sticky bottom-0 z-[3]"
        badge={nChanged ? <Badge variant="warningSoft">적용 안 한 변경 {nChanged}건</Badge> : <Badge variant="neutralSoft">적용된 상태</Badge>}
        note={nChanged ? '1~8 어느 단계의 편집이든 한 묶음이다 — 검사를 통과하면 한 번에 적용한다' : '1~8 어느 단계를 고쳐도 여기서 한 번에 검사하고 적용한다'}
        saveLabel="검사 후 적용" disabled={!nChanged || saving} saving={saving}
        onRevert={() => { setData(structuredClone(base)); setChk({}) }} onSave={() => setDlg({ kind: 'check' })}
        extra={<Button size="default" disabled={!nChanged} onClick={() => setDlg({ kind: 'diff' })}><FileDiff />변경 미리보기</Button>} />
      {dialog()}
    </div>
  )
}
