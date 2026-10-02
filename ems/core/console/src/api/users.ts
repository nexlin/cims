import { api } from './client'

export type SipTransport = 'UDP' | 'TCP' | 'TLS'
// 회선 종류 = 가입 테이블 = 접속환경 kind (sip_service_model.md §2-9) — 관리 API 경로 세그먼트 /users/{pid}/<svc>.
//   call = VoLTE(이동, volte_subscriptions) / voip = 유선 VoIP(voip_subscriptions) / ptt(ptt_subscriptions)
export type LineSvc = 'call' | 'voip' | 'ptt'
// 인증 체계 (sip_access_security.md §8.2) — digest=SIP Digest(H(A1)) / aka=IMS AKA(K/OPc, 보호 채널 강제)
export type AuthScheme = 'digest' | 'aka'

export interface Subscription {
  id: string          // MSISDN of this line
  auth_id?: string    // legacy — P8 에서 제거됨(백엔드 미반환). imsi 로 대체.
  passwd?: string
  icb_all?: boolean                  // 착신 차단 — 전체(TS 24.611 ICB, 모든 착신 603). 전화 회선만 — ptt 회선 응답에는 없다
  forward_id: string                 // 착신전환 CFU 대상(TS 24.604 — 서버측 전환, volte_supplementary_services.md §6A)
  // 조건부 전환(§6A.4, migrate_subscription_cdiv.sql — 미적용 DB 는 응답에 없다): 통화중 / 무응답(시한 초, 0 = 서버 기본) / 미등록 / 도달불가
  forward_busy_id?: string
  forward_no_reply_id?: string
  forward_no_reply_sec?: number
  forward_not_logged_in_id?: string
  forward_not_reachable_id?: string   // CFNRc 도달 불가(Q.850 20 · 링잉 없는 480/408)
  ringback_media?: string | null      // 가입자 링백 음원 id sys:|op:|sub: (announcements.md §6.3)
  service_ref?: string | null   // 소속 서비스(access_services.name, 예: volte/mcptt) — 도메인 결정
  service_id?: number | null    // (구) 숫자 service_id 호환
  imsi?: string | null          // SIM IMSI — 인증 username 의 user 파트. 번호 add 시 필수.
  // 채널 정책 — TLS=서버 집행(비-TLS 채널의 이 번호 요청은 REGISTER 포함 403) / UDP·TCP=프로비저닝 힌트 /
  //   null = ANY(단말 선택 — 서버 정책 없음, 광고된 transport 중 단말이 고른다). 서버는 입력 "ANY" 도 null 로 받는다.
  sip_transport?: SipTransport | null
  // 인증 체계 — 응답은 auth_scheme + aka_provisioned(K/OPc 보관 여부)만. K/OPc 는 입력 전용(응답에 절대 미포함),
  //   보내면 SQN 이 0 으로 리셋된다. AKA 컬럼 미적용 DB 에서는 두 키가 응답에 없다.
  auth_scheme?: AuthScheme
  aka_provisioned?: boolean
  // 당겨받기 그룹 키 — 같은 값끼리 픽업 가능 (volte_supplementary_services.md §5.1).
  //   빈 값/미지정=org_id 폴백. 반영은 다음 REGISTER 갱신부터. 마이그레이션 전 DB 는 응답에 없다.
  //   전화 그룹(dispatch_center.md §3.2) 귀속 person 의 회선은 이 값이 그룹 id(pg-…/전환 전 dg-…)로 파생된다 — 직접 편집 409
  //   derived_from_phone_group. 빈 값 = 어떤 픽업·BLF 축에도 속하지 않음(org 폴백 없음).
  pickup_group?: string | null
  k?: string        // hex32, 입력 전용
  opc?: string      // hex32, 입력 전용
  register_time?: string | null
  logout_time?: string | null
  mcptt_profile?: McpttProfile | null   // PTT 번호에만 (상세 응답 동봉, 미설정=null → 기본값)
  mcvideo_profile?: McVideoProfile | null  // PTT 번호에만 (상세 응답 동봉) — MCVideo 이용 자격, null = 자격 없음
}

// PTT 회선의 MCVideo 이용 자격 (MCVideo user profile — TS 24.484 §9.3, mcvideo_user_profile 행 = 자격).
//   자격이 있으면 user profile 문서·IdMS scope 3gpp:mc:video_*·토큰 mcvideo_id claim 이 따른다(admin_api.md §5.4).
export interface McVideoProfile {
  max_video_streams: number   // MaxSimultaneousVideoStreams — 동시에 받는 영상 수 C9 (1~16, CMP max_rx_streams)
  max_calls_n6: number        // MaxSimultaneousCallsN6 — 동시 MCVideo 호 수 N6 (1~16, 넘으면 486)
  // MaxAffiliationsN2 — 동시 MCVideo 제휴 그룹 수 N2 (1~1000, 기본 4 — MCPTT N2 와 따로). 넘는 제휴 요청은 줄이고, chat 개시의
  //   암묵적 제휴는 486 102. 옛 CSC 응답에는 없다
  max_affiliations_n2?: number
}
/** 자격 상한 범위 — CSC services/mcvideo.py PROFILE_LIMITS 와 같은 값. */
export const MCVIDEO_PROFILE_MAX = 16
export const MCVIDEO_N2_MAX = 1000

// 사용자 MCPTT 프로파일 (ptt_user_profile — TS 24.484). SOS 대상 결정 + 개시 인가.
export interface McpttProfile {
  allow_emergency_call: boolean     // 긴급 그룹콜 개시 인가
  allow_emergency_alert: boolean    // 긴급경보 개시 인가
  allow_adhoc_call: boolean         // ad hoc 개시 인가 (시스템 정책과 AND)
  emergency_group_mode: 'DedicatedGroup' | 'UseCurrentlySelectedGroup'  // SOS 대상 결정
  emergency_group_id: string | null // 전용 긴급그룹 (DedicatedGroup 모드의 콜·경보 대상)
  allow_emergency_private_call: boolean  // 긴급 사설콜(1:1) 개시 인가 (TS 24.379 §11)
  private_emergency_mode: 'LocallyDetermined' | 'UsePreConfigured'  // 긴급 사설콜 대상 결정
  emergency_private_recipient: string | null // UsePreConfigured 의 지정 수신자 (PTT 번호 — 서버가 존재검증)
  // cims:allow-ambient-listening (CIMS 확장) — PTT 그룹콜 청취 수행 자격 (관제사, 기본 false).
  //   범위는 역할 ptt_listen, 값은 역할 배정의 결과로 CSC 가 동기(mcptt_authorization.md §2.4). 컬럼 미적용 DB 는 false.
  allow_ambient_listening?: boolean
  // allow-create-group (CIMS 확장) — GMS XCAP 그룹 생성 자격 (관제사, 기본 false, mcptt_authorization.md §3). 컬럼 미적용 DB 는 false.
  allow_create_group?: boolean
  // allow-to-receive-non-acknowledged-users-information (TS 24.484 ruleset anyExt) — 그룹 호 개시자일 때 확인 통화 설정이
  //   필수 멤버 없이 진행되면 응답하지 않은 멤버 목록(INFO)을 받는다 (TS 24.379 §6.3.3.3, 기본 false). 컬럼 미적용 DB 는 false.
  allow_non_ack_users_info?: boolean
  // 해제 인가 (TS 24.484 ruleset) — 컬럼 미적용 DB 는 부재 시 값(mcpttProfileOptDefault).
  //   allow-cancel-group-emergency — 그룹의 진행 중 긴급 상태 해제 (개시자는 항상 — TS 24.379 §6.3.3.1.13.4 local policy, 기본 false)
  allow_cancel_group_emergency?: boolean
  //   allow-cancel-imminent-peril — 임박 위험 해제 (개시자 예외 없음 — §6.3.3.1.13.6, 기본 true)
  allow_cancel_imminent_peril?: boolean
  //   allow-cancel-emergency-alert — 긴급 경보 취소 (남의 경보 포함 — §6.3.3.1.13.3, 기본 = allow_emergency_alert)
  allow_cancel_emergency_alert?: boolean
  // 개별 호 인가 (TS 24.484 ruleset — 요소가 없으면 false 라 문서에 늘 싣는다). 컬럼 미적용 DB 는 true.
  //   allow-private-call — 개별 호 발신 (false 면 서버 403 107 — TS 24.379 §11.1.1.3.1.1)
  allow_private_call?: boolean
  //   allow-private-call-to-any-user — 상대를 PrivateCallList(같은 그룹 동료)로 한정하지 않는다 (false 면 목록 밖 403 144)
  allow_private_call_to_any_user?: boolean
  //   allow-private-call-participation — 개별 호 착신 참가 (false 면 403 127 — §11.1.1.3.2)
  allow_private_call_participation?: boolean
}

// 선택 컬럼(마이그레이션 의존) 자격 — 컬럼 미적용 DB 에 키를 실으면 PUT 이 400 schema_not_migrated, 키가 없으면 서버가
//   부재 시 값(mcpttProfileOptDefault)으로 쓴다.
export const MCPTT_PROFILE_OPT_KEYS = ['allow_ambient_listening', 'allow_create_group', 'allow_non_ack_users_info',
  'allow_cancel_group_emergency', 'allow_cancel_imminent_peril', 'allow_cancel_emergency_alert',
  'allow_private_call', 'allow_private_call_to_any_user', 'allow_private_call_participation'] as const
export type McpttProfileOptKey = typeof MCPTT_PROFILE_OPT_KEYS[number]

// 선택 컬럼의 부재 시 값 — 서버(CSC services.mcptt.user_profile_opt_default)와 같은 규칙: 대개 false, 임박 위험 해제·개별 호
//   인가 셋 true, 경보 취소 = 같은 프로파일의 발령 인가.
export function mcpttProfileOptDefault(k: McpttProfileOptKey, p: Pick<McpttProfile, 'allow_emergency_alert'>): boolean {
  if (k === 'allow_cancel_imminent_peril') return true
  if (k === 'allow_private_call' || k === 'allow_private_call_to_any_user' || k === 'allow_private_call_participation') return true
  if (k === 'allow_cancel_emergency_alert') return !!p.allow_emergency_alert
  return false
}

// 가입자(person). login_id/passwd = 단말(IdMS) 로그인 자격 — MCPTT ID 와 별개.
//   (콘솔 admin 계정은 별도 console_accounts. passwd 는 목록 응답에 미포함, 편집 입력만.)
export interface UserSummary {
  id: number          // person ID (auto-increment)
  name: string
  title?: string | null      // 직함 (예: 팀장) — 그룹문서 cims:user-title 확장으로 단말에 전달
  login_id?: string | null   // 단말/IdMS 로그인 ID (예: test001)
  org_id: string
  email?: string
  details?: string | null
  icb_identities: string[]           // 착신 차단 — 지정 번호(ICB cp:identity) — 그 사람의 모든 전화 회선에 적용
  call_subscriptions: Subscription[]   // VoLTE(이동) 회선 — volte_subscriptions
  voip_subscriptions: Subscription[]   // 유선 VoIP 회선 — voip_subscriptions (구 서버 응답에는 없다 → 소비자는 `|| []`)
  ptt_subscriptions: Subscription[]
  create_time?: string | null
  update_time?: string | null
}

// UserDetail is same shape as UserSummary (list API now includes subscriptions)
export type UserDetail = UserSummary

export type UserInput = {
  name: string; org_id: string; title?: string; email?: string; details?: string; icb_identities?: string[]
  login_id?: string; passwd?: string   // 단말 IdMS 로그인 자격 (passwd 는 변경 시에만 전송)
}

// Excel 가져오기 결과. credentials = password 칸을 비워 난수로 생성된 행 — 서버는 H(A1) 만 저장하므로
//   이 응답이 원문 비밀번호를 보는 유일한 기회다.
export interface ImportResult {
  // created_volte = VoLTE 시트, created_voip = 유선 VoIP 시트, created_ptt = PTT 시트 생성 회선 수
  total: number; created_users: number; created_volte: number; created_voip: number; created_ptt: number
  errors: Array<{ row: number; sheet: string; error: string }>
  credentials?: Array<{ sheet: string; row: number; msisdn: string; password: string }>
}

const enc = (s: string) => encodeURIComponent(s)

export const usersApi = {
  list:   ()                                                => api.get<{users: UserSummary[]}>('/users').then(r => r.users),
  get:    (id: number)                                      => api.get<UserDetail>(`/users/${id}`),
  create: (data: UserInput)                                 => api.post<{id:number}>('/users', data),
  update: (id: number, data: Partial<UserInput>)            => api.put<{id:number}>(`/users/${id}`, data),
  delete:      (id: number)                                  => api.delete<{id:number}>(`/users/${id}`),
  batchDelete: (ids: number[])                               => api.delete<{deleted:number, errors:Array<{id:number,error:string}>}>('/users/batch', {ids}),

  importExcel: (base64: string)                              => api.post<ImportResult>('/users/import', {file_base64: base64}),
  templateUrl: '/api/v1/users/import/template',

  addSub:     (pid: number, svc: LineSvc, sub: Partial<Subscription>)              => api.post<{id:string}>(`/users/${pid}/${svc}`, sub),
  updateSub:  (pid: number, svc: LineSvc, msisdn: string, data: Partial<Subscription>) => api.put<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`, data),
  deleteSub:  (pid: number, svc: LineSvc, msisdn: string)                          => api.delete<{id:string}>(`/users/${pid}/${svc}/${enc(msisdn)}`),

  getPttProfile:    (pid: number, msisdn: string)                        => api.get<McpttProfile & {id:string, exists:boolean}>(`/users/${pid}/ptt/${enc(msisdn)}/profile`),
  updatePttProfile: (pid: number, msisdn: string, data: McpttProfile)    => api.put<McpttProfile & {id:string}>(`/users/${pid}/ptt/${enc(msisdn)}/profile`, data),
  // MCVideo 이용 자격 — GET 404 {error:'not_entitled'} = 자격 없음 · PUT = 부여/상한 변경(준 키만) · DELETE = 회수
  getPttMcVideo:    (pid: number, msisdn: string)                        => api.get<McVideoProfile & {id:string}>(`/users/${pid}/ptt/${enc(msisdn)}/mcvideo`),
  putPttMcVideo:    (pid: number, msisdn: string, data: Partial<McVideoProfile>) => api.put<McVideoProfile & {id:string}>(`/users/${pid}/ptt/${enc(msisdn)}/mcvideo`, data),
  deletePttMcVideo: (pid: number, msisdn: string)                        => api.delete<{id:string}>(`/users/${pid}/ptt/${enc(msisdn)}/mcvideo`),
}
