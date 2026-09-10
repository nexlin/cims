# 안드로이드 UE 로그인·자동 프로비저닝 설계

> **목적**: 단말에서 서버/계정을 수동 입력하지 않도록, **로그인 1회 → 서버가 접속·계정 정보를 내려주고
> 단말이 자동 구성**한다. VoLTE(CSP)와 PTT(PSP)가 **다른 서버**일 수 있으므로 응답은 **서비스별 프로파일**
> 목록으로 구성한다. 신원·설정 플레인은 **CSC(IdMS)** 한 곳이며, 시그널링은 서비스별 서버로 분기한다.
>
> 본 기능은 **클라이언트 + 서버** 양쪽으로 구현돼 있다. 클라이언트는 contract 에 맞춰 동작하고
> (실패 시 수동설정 fallback), 서버는 CSC 가 `GET /provisioning/me` 를 제공한다(아래 §4). 서비스별
> 시그널링 도메인/주소는 `access_services` 확장 대신 CSC 설정 `Provisioning.Services.<kind>` 로 내려준다.

---

## 1. 흐름 — CIMS 단일 SSO

로그인은 **CIMS 오너앱 1회**(AccountManager 공유 계정, accountType `com.cims.ue`). CIMS-Phone/CIMS-McPtt 는 자체 로그인이 없다.

```
CIMS 앱 [로그인 화면]  (CSC 주소 + 아이디 + 비번)
  → IdMS OAuth2 PKCE 인증(TS 33.180) → refresh_token 을 공유 계정에 보관
  → 로그인 성공 즉시 CIMS-Phone/McPtt 등록유지 서비스 기동(startForegroundService,
     exported 서비스 + signature 권한 `com.cims.ue.permission.CIMS_SUITE`)
      → 각 앱 서비스가 공유 계정 토큰으로 GET /provisioning/me (Bearer)
      → 자기 service kind 프로파일로 SipAccountConfig 자동 구성·저장 → SIP REGISTER
  → 이후 앱을 열지 않아도 백그라운드 착신/문자 수신 가능 (부팅 후엔 각 앱 BootReceiver 가 동일 수행)
```

- 로그인은 **CSC(IdMS) 한 곳**. CSP/PSP 시그널링 서버 주소는 프로비저닝 응답으로 받는다.
- **volte-client** 는 `kind=="volte"`, **ptt-client** 는 `kind=="ptt"` 프로파일을 사용. 앱 진입 시 항상 재프로비저닝(GATE)해 서버 설정 변경(포트 등)을 자동 반영. 수동 설정은 **수동 설정 모드**(§5-1) 한정.

### 1-1. 로그아웃 — 스위트 연동 종료

CIMS 로그인 화면의 **로그아웃**(로그인 상태에서만 노출, 확인 다이얼로그) 흐름:

```
CIMS 앱 [로그아웃]
  → 공유 계정 제거(removeAccountExplicitly — 캐시 토큰도 함께 소멸)
  → 스위트 로그아웃 브로드캐스트 CimsSuite.ACTION_LOGOUT
     (setPackage 명시 + signature 권한 CIMS_SUITE + FLAG_INCLUDE_STOPPED_PACKAGES)
      → 각 앱 SuiteLogoutReceiver(정적 등록, core CimsLogoutReceiver 서브클래스):
         ① ConfigStore.clear() — 프로비저닝 설정·자격증명 제거(수동 설정 모드면 전체 무시)
         ② 서비스 종료: 등록 해제(un-REGISTER Expires:0) + FGS 정리(stopSip)
         ③ 2s 후 프로세스 종료(killProcess) — "앱 종료" 계약 + PJSIP 프로세스 내 재부팅
            (libDestroy 후 Endpoint 재생성) 취약성 회피: 다음 로그인은 항상 신규 프로세스 첫 부팅
```

- 정적 리시버라 앱 프로세스가 죽어 있어도 배달돼 캐시 설정이 항상 제거된다(다음 기동 시
  stale 자격증명 재등록 방지). 각 서비스 `ensureRegistered` 에도 로그아웃 게이트(계정 없음
  +수동 모드 아님 → 미등록·종료)가 있어 브로드캐스트 유실 시의 안전망이 된다.
- PTT 접근성 서비스(PttKeyService)는 시스템 바인딩이라 프로세스가 자동 재기동될 수 있으나,
  설정·계정이 비어 등록 FGS 는 뜨지 않는다(무해).
- **미로그인 상태에서 Phone/PTT 실행** 시 각 MainActivity(onCreate/onResume)가
  `CimsAccounts.redirectToLoginIfLoggedOut` 로 CIMS 로그인 화면(`ACTION_LOGIN` 명시 인텐트)으로
  전환하고 자신을 finish 한다. CIMS 앱 미설치(해석 실패)면 기존 자체 안내 화면(GATE/Splash) 폴백.
  수동 설정 모드는 전환하지 않는다.
- 재로그인 시 §1 흐름이 그대로 재실행돼 등록·affiliation 이 복원되고, 서버에 그룹 세션이
  살아 있으면 그룹콜도 자동 재조인된다(실기기 확인).

## 2. 신원 계층 (혼동 방지)

| 식별자 | 용도 | 규격 |
|---|---|---|
| IMSI | Digest username `IMSI@domain`(IMPI 역할) | 23.003 |
| IMPU/공개ID = `sip:msisdn@domain` | From/To/Contact(AOR) | 24.229 |
| **MCPTT ID** (`tel:`/`sip:` URI) | **MCPTT 서비스 신원** — GMS/CMS XCAP 키, mcptt-info calling-user-id, floor User ID, IdMS 토큰 `mcptt_id` 클레임 | 23.379/24.379/33.180 |

MCPTT ID 는 IMS 신원과 **별개 정의**(규격). 따라서 **PTT 서비스 프로파일에만** `mcpttId` 를 둔다(VoLTE 엔 없음). 값은 같아도(예 `tel:+msisdn`) 개념·필드는 분리.

## 3. Contract — `GET /provisioning/me`

요청: `Authorization: Bearer <CSC access_token>`. 토큰의 `mcptt_id`(또는 sub)로 사용자를 식별.

```json
{
  "user":  { "displayName": "테스트001", "loginId": "test001" },
  "csc":   { "host": "<CSC host>", "port": 4430 },      // McpttServer.PublicUrl 정본 (비면 요청 Host)
  "countryCode": "82",
  "services": [
    {
      "kind": "volte",
      "capabilities": { "smsGateway": false },
      "sip":     { "host": "<CSP host>", "port": 15060, "transport": "UDP",
                   "transports": [ { "transport": "UDP", "port": 15060 },
                                   { "transport": "TCP", "port": 15060 },
                                   { "transport": "TLS", "port": 15061 } ],
                   "default": "UDP",
                   "domain": "volte.cims.example.kr" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000001",
                   "authId": "", "sipHa1": "5f4dcc3b5aa765d61d8327deb882cf99", "sipPassword": null }
    },
    {
      "kind": "ptt",
      "capabilities": { "smsGateway": false },
      "sip":     { "host": "<PSP host>", "port": 15061, "transport": "TLS",
                   "transports": [ { "transport": "TLS", "port": 15061 } ],
                   "default": "TLS", "enforced": true, "mediaSecurity": "optional",
                   "domain": "ptt.cims.example.kr" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000002",
                   "authId": "", "sipHa1": null, "sipPassword": null, "mcpttId": "tel:+821300000001" }
    }
  ],
  "phoneGroup": {                                        // 전화 그룹 소속일 때만 (없으면 키 자체 생략)
    "groupId": "pg-dispatch01", "groupName": "관제 1조", "pilotId": "+821310001000",
    "members": [
      { "userId": 5020, "name": "관제1석", "volteAor": "tel:+821310001001",
        "pttId": "tel:+82510001001", "extension": "1001" },
      { "userId": 5021, "name": "관제2석", "volteAor": "tel:+821310001002",
        "pttId": "", "extension": "1002" }
    ],
    "etag": "\"8a2f…\""
  },
  "dispatch": {                                          // 관제 역할이 있을 때만 (없으면 키 자체 생략)
    "roleId": "role-dispatch01", "roleName": "관제 1조 감독",
    "monitorCall": "own", "pttListen": "listed", "listenVisibility": "hidden",
    "directoryWrite": "own", "orgCode": "TEAM01",
    "members": [
      { "userId": 5020, "name": "관제1석", "volteAor": "tel:+821310001001",
        "pttId": "tel:+82510001001", "extension": "1001", "groupId": "pg-dispatch01" }
    ],
    "pttTargets": [ { "id": "g002", "uri": "tel:g002", "name": "음성그룹2" } ],
    // 전환기 합성 필드(구 앱) — groupId/groupName/pilotId 는 phoneGroup, monitorScope=monitorCall, directoryAdmin=directoryWrite
    "groupId": "pg-dispatch01", "groupName": "관제 1조", "pilotId": "+821310001000",
    "monitorScope": "own", "directoryAdmin": "own",
    "etag": "\"3f1c…\""
  }  }
}
```

응답 헤더 `ETag`(응답 전체의 내용 해시) — 단말이 `If-None-Match` 로 같은 값을 보내면 **304**(본문 없음).
관제 앱의 주기 재조회(발견 목록 갱신 감지)는 이 경로로 전송 없이 끝난다.

필드 규칙:
- `services[].kind`: `volte`(이동 VoLTE) · `voip`(유선 VoIP — 데스크폰·소프트폰·관제 앱의 전화 회선) · `ptt`. `volte` 와
  `voip` 는 단말에서 같은 전화 회선 종류다 — 관제 앱은 `voip` 를 우선 잡고 없으면 `volte`, 이동 앱은 `volte` 만 본다
  ([sip_service_model.md §2-9](sip_service_model.md)). 구 서버는 `voip` 를 내리지 않는다.
- `sip.host/domain`: 단말이 접속할 **서비스별 시그널링 서버**. VoLTE/VoIP=CSP, PTT=PSP (다를 수 있음).
- `sip.transports`/`sip.default`: **가용 transport 목록과 기본값(권장)** — 단말이 이 중에서 고른다.
  transport 마다 포트가 다르므로 목록에 포트가 함께 실린다(같은 포트로 평문과 TLS 를 겸하지 않는다).
  TLS 포트 미설정(`tls_port=0`)이면 TLS 항목이 실리지 않는다. 선택·유지·반영 규칙은
  [sip_tls_signaling.md §7.1](sip_tls_signaling.md) 이 정본.
- `sip.port`/`sip.transport`: **기본값의 유효 쌍** — 목록을 모르는 구 APK 가 이 두 필드만 읽으므로 유지된다.
- `sip.enforced`: `true` 면 서버가 그 transport 를 **집행**한다 — 가입자 `sip_transport=TLS`
  또는 `auth_scheme=aka`(서버 게이트와 같은 술어, [sip_access_security.md §3·§8.2](sip_access_security.md)).
  목록은 TLS 하나로 좁혀지고, 다른 채널의 요청은 REGISTER 포함 403 이다. `false` 면 목록 안에서
  단말이 고른다. 예외: 서비스가 `ipsec-3gpp` 를 제시하면 AKA 가입자도 좁히지 않는다(IPsec
  부트스트랩 = 평문 초기 REGISTER — Android 범위 밖).
- `sip.mediaSecurity`: 미디어 SRTP(SDES) 정책 `off|optional|required` — 서버 접속서비스
  `media_srtp` 와 같은 값(한 SoT, 설정 `Provisioning.Services.<kind>.media_srtp` — CSP 와 운영자
  동기). 단말은 TLS 접속일 때만 pjsua `srtpUse` 로 반영하고 REGISTER `Security-Client` 에
  `sdes-srtp;mediasec` 능력을 병기한다([media_security.md §7.2](media_security.md)). 구 서버
  응답에 없으면 `off`.
- `account.imsi`: Digest username = `imsi@sip.domain`(서버 CscfModule 강제). 서비스별로 다를 수 있음.
- `account.msisdn`: 공개 ID(AOR user part). `authId`: 전체 IMPI 직접지정(보통 빈값 → imsi@domain 합성).
- `account.sipHa1`: **서비스 가입(subscription) 의 SIP Digest H(A1)**(`*_subscriptions.ha1` =
  `MD5(imsi@domain:realm:password)`). CIMS 로그인(IdMS `users.passwd`)과 **별개 자격증명** — 단말은 이 값을
  pjsip `PJSIP_CRED_DATA_DIGEST` 자격으로 넣어 원문 없이 response 를 계산한다.
- `account.authScheme`: `digest`(기본) | `aka`. `aka` 면 `account.aka = {k, opc, amf}`(hex) 가 함께 오고 `sipHa1` 은
  `null` — **소프트-K 프로비저닝**(단말이 USIM 역할, [sip_access_security.md §8.2](sip_access_security.md)). 단말은 이
  값을 pjsip AKA 자격(`PJSIP_CRED_DATA_EXT_AKA`)에 넣어 `AKAv1-MD5` 챌린지에 답하고, 그 가입은 TLS 로만 등록한다
  (Android 연결은 후속). 서버가 키를 못 풀면(`AuC.Kek` 불일치) `k`/`opc` 가 빈 문자열로 온다.
- `account.sipPassword`: 항상 `null`(서버가 평문을 배포하지 않는다 — 키는 단말 호환으로 유지).
  단말은 `sipHa1`(DIGEST cred) → 평문 cred(`sipPassword`, 구 서버 호환) 순으로 쓰고, 둘 다 없으면 SIP 계정을
  구성하지 않는다(`SipAccountConfig.isComplete()` 미완성 → 등록 시도 없음, 앱 상태 "로그인 필요"). **로그인
  비밀번호는 IdMS 자격이라 SIP Digest 에 쓰지 않는다** — 두 비밀번호는 별개다(sip_access_security.md §4.7).
  평문 cred 는 pjsip 이 challenge realm 로 그때 ha1 을 계산하므로 realm 결박이 없다.
- `account.mcpttId`: PTT 프로파일에만. GMS/CMS/affiliation/floor 에서 사용.
- `capabilities`: **접속서비스 능력** — 단말이 기능 노출을 결정하는 서버측 사실. `smsGateway`(bool) = 이 서비스에 외부망
  휴대전화 SMS/LMS 게이트웨이(IBCF→SMSC TS 24.341 / SMPP)가 연결돼 있는가 — 관제 앱은 `false` 면 외부 번호의 [문자] 를
  비활성 + 툴팁으로 두고 팝오버 머리에 게이트웨이 상태 배지를 그린다([dispatch_desktop_ui.md §4.3](dispatch_desktop_ui.md)).
  등록 가입자 간 `MESSAGE` 전달은 이 값과 무관. SoT = csc.json `Provisioning.Services.<kind>.sms_gateway`(기본 `false` —
  CIMS 는 게이트웨이를 내장하지 않으므로 외부 게이트웨이 연동 시 운영자가 켠다). 구 서버 응답에 없으면 전부 `false`.
- `countryCode`: 홈 국가코드(E.164 digits, `+` 없음. 예 `"82"`) — 단말 번호 로컬 표기(§3-1)의 **SoT**.
  CSC 설정 `Provisioning.CountryCode` 우선, 미설정이면 로그인 msisdn 에서 서버가 유도. 판정 불가면
  빈 문자열(`""`) — 명시적 `null` 은 보내지 않는다(Android `org.json` 이 `"null"` 문자열로 오독).
- `phoneGroup`: **전화 그룹**([dispatch_center.md §3.1·§8.4](dispatch_center.md)) — 사용자의 회선이 전화 그룹
  (`phone_group_members`) 소속일 때만 실린다(미소속·테이블 미적용 DB 는 키 생략, `null` 없음). 유선 전화 기능이며 관제 권한과 무관.
  - `groupId/groupName/pilotId`: 소속 그룹의 속성 그대로.
  - `members[]`: **같은 전화 그룹원** = 그룹원 상태 띠·BLF(dialog 구독, `CanWatch` 규칙 1)·지정 픽업 대상. 항목 = `userId`(`users.id`) ·
    `name` · `volteAor`(`tel:+E.164` — 이름은 **전화 가족 축**: 유선 `voip` 회선도 이 필드로 온다. SDK C++·C API·.NET·Android 넷에 걸친 이름이라 바꾸지 않는다) · `pttId`(첫 PTT 가입 `tel:`, 미가입 `""`) · `extension`(가입 번호 끝자리 N — 설정
    `Provisioning.ExtensionDigits`, 기본 4. 망 주소가 아닌 **표시 라벨**). 정렬 = `alert_order`.
  - `etag`: 블록 내용 파생.
- `dispatch`: **관제 역할**([dispatch_center.md §3.3·§8.4](dispatch_center.md), 정본 [mcptt_authorization.md](mcptt_authorization.md)) —
  사용자(person)에게 역할이 배정돼 있을 때만 실린다(미배정·테이블 미적용 DB 는 키 생략).
  - `roleId/roleName/monitorCall/pttListen/listenVisibility`: 역할 속성 그대로(`monitor_call` `none|own|listed|all`, `ptt_listen`
    `none|listed|all`, `listen_visibility` `hidden|visible`).
  - `directoryWrite`(`none|own|all`)·`orgCode`: 관제 앱의 조직/구성원/번호·PTT 그룹·전화 그룹 **관리 범위**(§3-3) — `own` 의 루트가
    `orgCode`(역할 `org_id` 의 코드, 없으면 `""`). 앱은 `none` 이면 관리 탭을 잠근다.
  - `members[]`: **dialog 감시(RFC 4235) 대상** = 서버가 `monitorCall` 을 CSP `CanWatch` 규칙 2 와 같은 규칙으로 해석한 가입자 목록 —
    `own` 은 자기 전화 그룹원, `listed` 는 대상 전화 그룹원, `all` 은 전 VoLTE/VoIP 가입자 **+ 회선 없는 PTT 전용 가입자**
    (`volteAor=""`, 목록 끝). 항목 = `phoneGroup.members[]` 와 같은 열 + `groupId`(그 가입자의 전화 그룹, 무소속 `""`). 앱은
    `volteAor`(통화 상태)와 `pttId`(PTT 세션 참가 — 사설콜·애드혹·그룹, [dispatch_center.md §5.6a](dispatch_center.md)) **둘 다**
    비어 있지 않은 것마다 dialog 를 구독한다. 앱은 enum 을 해석하지 않는다.
  - `pttTargets[]`: **conference 구독·청취 대상** = `pttListen` 을 `CanListenPtt` 와 같은 규칙으로 해석한 PTT
    그룹(`listed` 대상, `all` 전 그룹, `none` `[]`). 항목 = `id`(`mcptt_group_id`) · `uri`(시스템 관례 `tel:` 형) ·
    `name`. GMS 멤버 그룹과 겹칠 수 있다 — 앱은 `id` 로 병합한다.
  - **전환기 합성 필드**(구 `dispatch` 블록 모양을 아는 앱용): `groupId/groupName/pilotId`(= `phoneGroup`), `monitorScope`(= `monitorCall`),
    `directoryAdmin`(= `directoryWrite`). 전화 그룹만 있고 역할이 없으면 `dispatch` 블록은 합성 필드와 `members[]`(그룹원)만 싣고 범위는
    `none` 이다. 새 앱은 두 블록을 따로 읽는다.
  - `etag`: 블록 내용 파생(따옴표 포함) — 재조회 결과에서 대상 변경 여부만 볼 때 비교한다. 304 판정은 응답
    헤더 `ETag`(전체) 로만 한다.

오류: 토큰 무효 401. 사용자에 해당 서비스 없으면 `services` 에서 제외(빈 배열 가능). `If-None-Match` 일치 304.

## 3-1. Contract — `GET /provisioning/directory`

회사 전화번호부(단말 '회사 연락처' 탭의 읽기전용 소스). provisioning scope 토큰 필요.
조직 트리(`organizations` 의 `parent_id` 계층)와 전 VoLTE 가입자를 반환한다.

```json
{
  "orgs": [
    { "code": "CORP",   "name": "CIMS",   "parent": "",     "sort": 0 },
    { "code": "DIV1",   "name": "제1본부", "parent": "CORP", "sort": 1 },
    { "code": "TEAM01", "name": "팀01",    "parent": "DIV1", "sort": 1 }
  ],
  "entries": [
    { "org": "TEAM01", "name": "테스트001", "msisdn": "+821300000001" },
    { "org": "TEAM01", "name": "테스트002", "msisdn": "+821300000002" }
  ]
}
```

- `orgs[]`: 조직 트리. `parent` = 상위 조직 **코드**(루트는 빈 문자열). `organizations.parent_id`(id) 를 code 로 환산해 내려준다.
- `entries[].org`: 가입자 소속 조직 **코드**(`users.org_id`). `orgs[].code` 와 매칭.
- 단말 표시(연락처 탭 = 최상단 검색바 + 즐겨찾기/회사/개인 언더라인 탭):
  - **조직 범위 선택**: 리스트 위 "전체 조직 ▾"(선택 시 "CIMS > 제1본부" 경로) 버튼 → **바텀시트
    단계별 펼침 트리**(처음엔 최상위만, ▸/▾ 토글로 한 단계씩 펼침, 이름 탭=선택·닫힘, 인원수 병기).
  - **리스트**: 선택 범위(하위 포함) 구성원을 들여쓰기 없는 **평면 리스트**로, 소속 조직별
    **sticky 섹션 헤더**(전체 경로 `CIMS > 제1본부 > 팀01` + 인원수, 스크롤 시 상단 고정)로 그룹핑.
  - **편집 불가**(추가/수정/삭제는 '개인 연락처'만 — 추가=개인 탭 우측 아이콘, 삭제=행 좌측 스와이프).
  - 이름·번호 **검색**(최상단 검색바, 3개 탭 공통) 시 조직 무시·일치 가입자 평면 표시.
    캐시(`CompanyDirectoryStore`)로 오프라인 표시.
- **버전 기반 동기화**: 응답 `ETag`(내용 sha256) 를 단말이 보관 → 다음 동기화 시 `If-None-Match` 로 전송.
  서버 내용이 같으면 **304 Not Modified**(본문 없음) → 단말은 다운로드 없이 '마지막 동기화 시각'만 갱신.
  다르면 200+새 본문+새 ETag. 회사 탭 진입 시 자동 1회 + **당겨서 새로고침**(PullToRefreshBox,
  별도 동기화 버튼 없음 — PTT 앱 전체채널 탭과 동일 패턴).
- **즐겨찾기**: 회사/개인 행의 ★ 토글로 추가·삭제(`FavoriteStore`, 로컬). '즐겨찾기' 세그먼트에서 모아 본다.
- **상세/통화**: 행을 누르면 **전체화면 상세**(아바타/이름 중앙 + 음성통화·영상통화·메시지(SIP MESSAGE)·
  즐겨찾기 액션 + 휴대전화/소속 정보 행). 발신은 상세에서만(목록 행에 바로걸기 없음).
- **번호 표기(홈 국가코드 축약)**: 프로비저닝 응답 `countryCode`(§3)가 SoT — 같은 국가 번호는
  로컬 표기(`+821300000001` → `01300000001`)로 표시, 타국 번호는 그대로. `countryCode` 미수신
  (구서버)일 때만 단말이 내 msisdn 에서 유도(ITU 자릿수 규칙)하는 fallback.
  **표시 전용** — 발신·저장·즐겨찾기 매칭 키는 원본(+E.164) 유지.

## 3-2. Contract — `GET /provisioning/history` (관제 데스크 통합 이력)

관제 데스크(가입자=관제사)의 **지난 이력** 조회. 진행 중(live) 상태는 표준 구독(RFC 4235 dialog·
RFC 4575 conference)이 담당하고 이 API 는 대체하지 않는다 — ②PTT 내역·④통화 내역 패널과 메시지
모니터링이 같은 계약 하나로 최근 이력을 커서로 받는다([dispatch_center.md §5.6](dispatch_center.md)).

요청: `GET /provisioning/history?kind=call|ptt|message&since=<ISO8601|epoch>&until=<ISO8601|epoch>&limit=<n>` +
`Authorization: Bearer <provisioning access token>`(PKCE, `/provisioning/me` 와 같은 토큰).

- `kind`(필수): `call`(VoLTE) · `ptt`(PTT 그룹 세션) · `message`(SDS — 그룹 + 1:1). 미지 값 400.
- `since`(선택): 이 시각 **이후**(strict)만. 생략 시 최근 1시간. 이전 응답의 `nextSince` 를 그대로 넣어
  폴링한다(관제 앱 2~3초 주기). 스캔은 최대 48 시간 버킷으로 유계.
- `until`(선택): 이 시각 **이하**만 — 있으면 [since, until] **창 조회**(관제 앱 [이력] 화면 — 하루 단위로 나눠
  묻는다), 없으면 지금까지(폴링). 버킷 상한은 그대로.
- `limit`(선택, 기본 200, 최대 1000): 가장 최근 N 개.

응답 `200`(단말 `HistoryClient` 계약 — 필드 추가는 무시, 필수 `id`·`time` 없으면 스킵):
```json
{
  "items": [
    { "id": "<call_id>", "time": "2026-09-06T19:05:12+09:00", "kind": "call",
      "event": "call.answered", "from": "+82…", "to": "+82…", "group": "",
      "duration": 30, "emergency": false, "text": "",
      "recordingId": "volte/2026/09/06/19/010/01000000001/<call_id>.d", "hasRecording": true }
  ],
  "next": "2026-09-06T19:05:12"
}
```
- 항목 공통 필드: `id`(중복 제거 키) · `time`(ISO8601 + 로컬 offset — 앱 `DateTime` 파싱) · `kind`(call/ptt/message) ·
  `event`(아래 이름표) · `from` · `to` · `group`(그룹 URI `tel:<gid>`, 비그룹은 "") · `duration`(초) · `emergency`(bool) · `text`.
- `items` 는 `time` **오름차순**(오래된→최근). 앱은 `next` 를 다음 폴링의 `since` 로 그대로 넣어 그 이후만 받는다(표시는 뒤집는다).
- `event` 이름표(앱 switch 와 1:1): call = `call.answered`(응답됨)/`call.missed`(무응답) · ptt = `ptt.session.start`(진행 중)/
  `ptt.session.end`(종료) · message = `message.sds`(그룹 SDS → ② 패널)/`message.sms`(1:1 → ④ 패널). `group`·`duration`·`text`
  는 종류에 따라 채워진다(call `group=""`·`duration`=통화초, ptt `group=tel:<gid>`·`duration`=세션초, message `text`=본문).
- `recordingId`(종료분 call·ptt): 녹취 식별자 = 세션 디렉터리의 `ServiceLogging.Dir` 상대 경로(OAM `/api/v1/recordings/{id}`
  와 같은 키, `/` 구분 — 세그먼트별 percent-encoding). live 항목·message 는 `""`. `hasRecording` = `segments.jsonl` 존재.
  재생은 §3-4.
- **종류별 확장 필드**(관제 앱 [이력] 화면이 콘솔 VoLTE/PTT 이력과 같은 열·카드를 그리는 데 쓴다. 폴링 병합은 읽지 않는다.
  값이 없으면 `""`/`0`/`[]`):
  - call: `callType`(volte|volte_video) · `state`(ended|active|ringing) · `inviteTime` · `answerTime` · `endTime`(ISO8601+offset) ·
    `endReason`(normal|no_answer|busy|rejected|error|timeout|incomplete — 문구는 앱 사전) · `sipStatus`.
  - ptt: `sessionKind`(group|private|adhoc) · `state`(ended|active) · `startTime` · `endTime` · `groupName` · `memberCount` · `people[]`(참여자) ·
    `turnCount`(발언 턴) · `speakerCount` · `totalSpeechMs`(발화 구간 합, 겹침 1회) · `talkMs`(화자별 누적) · `maxConcurrent` ·
    `floorControl`(on|off|"") · `floorPolicy`(single|dual|multi) · `maxTalkers`.
- 최상위 `hours`: 시간대(HH) → 건수 — 통화는 INVITE, PTT 는 세션 시작 시각 기준(콘솔 `/call/logs`·`/ptt/sessions` 의 `hours` 와 같은 축),
  `limit` 절삭 **전** 창 안 전체 행으로 센다(앱 시간대 밴드 = 그날의 분포이자 필터).
- **PTT 창 조회의 백엔드**(`kind=ptt` + `until`): 발언 지표는 CMP `segments.jsonl` 을 집계한 OAM 세션 인덱스(ptt_index — 콘솔 PTT 이력의
  읽기 모델)에만 있으므로, CSC 가 OAM `GET /api/v1/ptt/sessions?date|from,to&group_key=<청취 그룹의 ptt_groups.id 목록>` 을 프록시해
  같은 항목 형태로 바꾼다(`services/dispatch_history.ptt_row_from_oam`, `recordingId` 는 콘솔 `recIdOf` 와 같은 규칙, `id` 는 스캔 경로와
  같은 우선순위 sesid→call_id→dir). OAM 에 닿지 않으면 파일 스캔으로 폴백한다(지표 0). 폴링(until 없음)은 파일 스캔이다.
  파일 스캔의 **진행 중 판정**은 `state/ptt/*.json`(CSP 가 참가자마다 쓰고 떠나면 지운다)에 세션이 있는지로 한다 — 시간 버킷의
  `session.json` 은 세션 시작 스냅샷이라 `state`/`end_time` 이 비어 있어도 진행 중이 아니다(콘솔 ptt_index 와 같은 기준). 종료 시각은
  `events.jsonl` 의 `session_end`(없으면 마지막 이벤트) 또는 `segments.jsonl` 의 마지막 `end_time`.
  OAM 주소 = csc.json `Recording.OamUrl`(비면 `https://{Fm.OamIp}:4419`) — 녹취 프록시(§3-4)와 같은 설정.
- 응답 헤더 `ETag`. 단말이 `If-None-Match` 로 같은 값을 보내면 **304**(본문 없음, 폴링 대역 절약). 변경 없는 304 는
  감사하지 않는다 — 실제 열람(새 항목/최초)만 `E-AUD-016` 로 남긴다.

**범위(scope) 게이트** — 역할 속성으로 서버가 거른다(CSP `CanWatch` 규칙 2/`CanListenPtt` 와 같은 규칙):
- `call` · 1:1 `message` = 역할 `monitor_call` 로 해석한 감시 대상 가입자(`own` 자기 전화 그룹원 / `listed`
  대상 그룹원 / `all` 전 가입자)가 발신 또는 수신인 것.
- `ptt` · 그룹 `message` = 역할 `ptt_listen` 으로 해석한 청취 대상 PTT 그룹.
- **역할이 없거나 두 범위가 모두 `none` = `403 no_monitor_scope`**. 열람은 감사(`E-AUD-016 call_monitored`, `tap_mode=history`) —
  당사자 모르게 이력을 여는 동작이라 감사 대상(manager 열람, [dispatch_center.md §5.7](dispatch_center.md)).

> 백엔드는 CSP/CSC 가 공유 NAS(`ServiceLogging.Dir`)에 남기는 파일 SoT(콘솔 `flow_logger` 가 읽는 것과 같은
> 파일)를 역할 범위로만 걸러 주는 얇은 구독자 뷰다 — 콘솔 이력 API(oam-svc)를 재구현하지 않는다(집계가 필요한 PTT
> 창 조회·세션 상세는 그 API 를 범위 게이트 뒤에서 **프록시**한다).
> 1:1 SDS/SMS 는 CSP `Setup.McData.StoreOneToOneSds` 를 켜야 보관된다([mcdata_messaging.md §4.3](mcdata_messaging.md)).

### 3-2a. `GET /provisioning/history/ptt/{recordingId}` — PTT 세션 상세

관제 앱 [이력] 화면이 PTT 세션을 고르면 부르는 상세(참여자·입퇴장 이벤트·floor 타임라인). `recordingId` 는 §3-2 항목의 것
(`ptt/{저장키}/{Y}/{M}/{D}/{H}[/{세션키}]`, 세그먼트별 percent-encoding). 범위 게이트는 §3-4 녹취와 같다(저장키 → 청취 그룹,
`session.json` 대조 폴백 · 범위 밖 403 · 경로 이탈 400 · 관제 미소속 403 · 세션 디렉터리 없음 404). 본문은 OAM
`GET /api/v1/ptt/history/{group_key}/{session}` + `…/floor` 를 합친 것(구 녹취형은 session = 시간창 `YYYYMMDDHH`):

```json
{ "recordingId": "ptt/1/2026/09/08/09/S20260908091000000000_1",
  "session": { "...session.json 스냅샷 + session_id, windows[]" },
  "participants": [ { "msisdn": "+82…", "role": "initiator|member", "join_time": "…", "leave_time": null } ],
  "events":       [ { "ts": "…", "type": "session_start|session_end|member_join|member_leave|member_invite|config_change", "member": "+82…", "role": "initiator" } ],
  "floor":        [ { "ts": "…", "op": "GRANT|RELEASE|IDLE|REVOKE|REVOKE_END|QUEUE|QUEUE_CANCEL|DENY", "user": "+82…", "slot": 0, "talkers": 1,
                      "policy": "dual", "preempt": false, "reason": "recv_only", "owner": "+82…", "pos": 1, "qsize": 1, "grace_sec": 3, "idle_ms": 300 } ],
  "hasRecording": true }
```
시각은 파일의 naive-local ISO 그대로(앱은 로컬로 해석). 발언 턴은 이 응답이 아니라 §3-4 녹취 세그먼트의 `tracks[].speakers[]`
(콘솔 `segTurns` 와 같은 해석)에서 만든다. 열람은 감사 `E-AUD-016 call_monitored`(`tap_mode=history`, `hist_kind=ptt_session`, `recording`).
OAM 미도달 502 `oam_unreachable`. 구현 `csc/src/handlers/dispatch_recordings.py` `handle_ptt_session_detail`(경로 등록은 가장 긴 접두 우선 —
`/provisioning/history` 목록보다 먼저 잡힌다).

## 3-3. Contract — `/provisioning/directory/{admin,orgs,members,groups}` (관제 앱 관리 평면)

관제사(가입자)가 관제 앱에서 조직 트리·구성원·VoLTE/PTT 번호·전화 그룹·PTT 그룹을 관리한다. 같은 PKCE provisioning 토큰.
인가 = 역할 `directory_write`(§3 `dispatch.directoryWrite`, [dispatch_center.md §3.4](dispatch_center.md),
[mcptt_authorization.md §2.3](mcptt_authorization.md) `can(principal, directory.write, org)`) — 없으면 전부
`403 {"error":"no_directory_admin"}`, 범위 밖 조직·구성원은 `403 {"error":"out_of_scope"}`. 콘솔 관리 API 와 **같은 쓰기 코드·같은
판정**을 지난다(서버 구현 `csc/src/handlers/dispatch_directory.py`). 역할·배정은 이 평면에 없다(콘솔 `authz.manage`).

| 메서드·경로 | 본문 / 응답 |
|---|---|
| `GET /provisioning/directory/admin` | 관리 화면 한 벌 `{ "scope": {groupId, directoryAdmin, orgCode}, "services": {"volte":[{name,domain}], "ptt":[…]}, "orgs": [{code,name,parent,sort}](범위 안), "members": [{userId, name, loginId, org, title, "volte": {msisdn,imsi,serviceRef,sipTransport,authScheme}\|null, "ptt": {…, "profile": {allowCreateGroup, allowAmbientListening, allowEmergencyCall, allowEmergencyAlert, allowAdhocCall, allowEmergencyPrivateCall}}\|null}] }` + `ETag`/`If-None-Match` 304 |
| `POST /provisioning/directory/orgs` | `{code, name, parent, sort}` → `201 {code, id}`. `parent` 는 범위 안 코드(`own` 은 필수 — 루트 신설 불가). `409 code_exists`·`400 unknown_parent` |
| `PUT /provisioning/directory/orgs/{code}` | `{name?, parent?, sort?}` → `200 {code}`. 범위 루트 이동 불가(403)·`400 cyclic_parent` |
| `DELETE /provisioning/directory/orgs/{code}` | `200 {code}`. 하위 조직·구성원이 남아 있으면 `409 not_empty`, 범위 루트 삭제 불가(403) |
| `POST /provisioning/directory/members` | `{name, org, title?, loginId?, password?, volte?: {msisdn, imsi?, serviceRef?, sipTransport?, password}, ptt?: {…}}` → `201 {userId}`. `imsi` 비면 번호 숫자(USIM 없는 관제 소프트폰 규약), 회선은 `password` 필수(H(A1)). 회선 개설 실패는 그 코드 + `{userId, kind}` |
| `PUT /provisioning/directory/members/{userId}` | `{name?, org?, title?, loginId?, password?}` → `200 {id}` |
| `DELETE /provisioning/directory/members/{userId}` | `200 {id}` — 회선 함께 삭제(USER_CHANGED). 자기 자신 `409 self_delete` |
| `POST /provisioning/directory/members/import` | 구성원 일괄 가져오기 — 본문 `text/csv`(UTF-8, 머리행. 열 = `name, org, title, login_id, password, volte_msisdn, volte_imsi, volte_service_ref, volte_sip_transport, volte_password, ptt_msisdn, ptt_…`; 대소문자·`_`·`-` 무시) 또는 `application/json` `{"rows":[<POST members 본문>…]}`(최대 500행) → `200 {created, failed, results:[{row, status, userId\|error…}]}`. 행마다 `POST members` 와 같은 경로(범위 게이트·감사 E-AUD-006·회선 규약)라 한 행의 실패(403/409/400)가 다른 행을 막지 않는다. 빈 CSV `400 empty_csv`, 초과 `413 too_many_rows` |
| `PUT /provisioning/directory/members/{userId}/volte\|ptt` | `{msisdn, imsi?, serviceRef?, sipTransport?, password?}` — 같은 번호면 갱신(`200`), 다른 번호면 종전 회선 삭제 + 개설(`201`, `password` 필수). 타인 번호 `409 number_exists`, `pickup_group` 파생 충돌 `409 derived_from_dispatch_group` |
| `DELETE /provisioning/directory/members/{userId}/volte\|ptt` | `200 {userId, kind, deleted[]}` |
| `PUT /provisioning/directory/members/{userId}/ptt/profile` | `{allowCreateGroup?, allowEmergencyCall?, allowEmergencyAlert?, allowAdhocCall?, allowEmergencyPrivateCall?}`(없는 키는 현재값 유지) → `200 {msisdn, profile}`. **`allowAmbientListening` 은 바꿀 수 없다** — 현재값과 다른 값이 실려 오면 `400 {"error":"not_editable","key":"allowAmbientListening"}`, 같은 값은 무시(구 앱 호환). 청취 자격은 역할 배정의 결과([mcptt_authorization.md §2.4](mcptt_authorization.md)) |
| `GET /provisioning/directory/groups` | `{ "scope": {directoryWrite, orgCode}, "groups": [{id, uri, name, memberCount, isOwner, canManage, inListenScope, isMember, orgCode, sessionType, etag}] }` — 관리 범위 안 ∪ 내 소유 ∪ 청취 범위 ∪ 내 멤버 PTT 그룹. 문서 GET/PUT/DELETE 는 GMS XCAP 그대로([mcptt_api.md §2](../../api/mcptt_api.md) — 관리 범위 안이면 소유자가 아니어도 허용) |
| `GET|POST /provisioning/directory/phone-groups` · `GET|PUT|DELETE …/phone-groups/{id}` · `POST|DELETE …/phone-groups/{id}/members[/{userId}]` | 관리 범위 안 조직의 **전화 그룹**(대표번호·호출 방식·멤버) — 콘솔 `/api/v1/phone-groups` 와 같은 본문·같은 판정([dispatch_center.md §8.2](dispatch_center.md)) |

오류 본문은 `{"error": "<token>", "detail"?: …}`. 앱 문구 사전 = `ResponseText.ForManagementError`. 컬럼 미적용 DB 는
`403 no_directory_admin`(관리 기능 비활성). 감사 = `E-AUD-006 config_change`(actor `user:<users.id>`).

## 3-4. Contract — `GET /provisioning/recordings/{id}…` (관제 앱 녹취 재생)

`id` = §3-2 항목의 `recordingId`. 같은 토큰. CSC 는 역할 범위(§3-2 와 같은 집합)를 판정한 뒤 oam-svc 녹취 API
(`/api/v1/recordings/…`, [recording.md](recording.md))로 **프록시**한다 — 응답 본문·상태는 그대로.

| 경로 | 응답 |
|---|---|
| `GET /provisioning/recordings/{id}` | 세션 메타 + `segments[]`(`seq, type, speaker_id, speaker_ids[], start_time, end_time, duration_ms, has_video, status, talker_count, tracks[]`) |
| `GET /provisioning/recordings/{id}/segments/{seq}/audio?slot=<K>&retry=1` | `200 audio/mp4`(AAC 16k mono — 믹스, `slot` 은 단독 트랙) · `202 {status: transcoding\|recording}`(앱은 0.7초→1.5초 간격으로 최대 120초 재시도) · `500 {status: failed, reason}`(`retry=1` 로 표식 제거 후 재변환) · `404` |
| `GET /provisioning/recordings/{id}/segments/{seq}/peaks?slot=` | `{seq, slot, buckets, peaks[]}` |

오류: `401` · `403 no_monitor_scope`(역할 없음) · `403 out_of_scope` · `400 invalid_recording_id`(경로 이탈) · `404 not_found` ·
`502 oam_unreachable` · `503 service_log_unavailable`. 오디오 200 마다 감사 `E-AUD-016`(`tap_mode=recording`).
서버 설정 csc.json `Recording.OamUrl`(비면 `https://{Fm.OamIp}:4419`) · `Recording.VerifyTls`(기본 false).

## 4. 서버측 구현 (CSC)

1. **엔드포인트 `GET /provisioning/me`** (`csc/src/services/mcptt.py` `handle_provisioning_me`, CSC mcptt 서버 4430):
   - 인증: Bearer access_token → `mcptt_id`(또는 sub) → msisdn 추출.
   - 조회: 로그인 msisdn 으로 person(`user_id`) 확인 → 그 person 의 `volte_subscriptions`+`ptt_subscriptions`
     전 서비스를 반환(로그인 1회로 보유 서비스 모두). 계정: id(msisdn)/imsi/auth_id.
   - 사용자: `users.name` → displayName.
   - 전화 그룹·관제 역할: `dispatch_discovery` — `phoneGroup`(소속 전화 그룹 1행 + 그룹원) / `dispatch`(배정 역할 1행 + 범위 enum
     해석 질의 — members = `volte_subscriptions` ⋈ `users` ⟕ `phone_group_members`, WHERE 만 범위별 / pttTargets =
     `role_ptt_targets` ⋈ `ptt_groups` 또는 전 그룹) + 전환기 합성 필드. 범위 판정 규칙은 CSP `CCspRoleMap`(게이트)과 여기(목록)
     둘뿐이며 같아야 한다.
   - ETag: 응답 전체 정규화 JSON 의 sha256(앞 32 hex) — `If-None-Match` 일치 시 304.
2. **서비스 정의 + 시그널링 주소** — 두 겹을 합친다(`services/access_services.entry`, [sip_service_model.md §2-9](sip_service_model.md)):
   - **정의**(`name·kind·domain·auth_realm·media_srtp·sec_mechanisms·pickup_feature_code·transfer_allowed`) ← CSP `access_services`
     의 관리 store 미러(정본의 읽기 전용 복제). 서비스 선택은 가입 행의 **`service_ref`** 로 `name` 이 일치하는 레코드(가족 경계 안 —
     PTT 가입 행이 전화 서비스 name 을 가리켜도 ptt), 없으면 가입 종류의 kind(call→`volte`, ptt→`ptt`) 첫 enabled 레코드(priority
     오름차순 — CSP `GetForUser` 와 같은 순서). 같은 경로로 H(A1) 파생(`_service_realm`)도 도메인·realm 을 얻는다(유선 회선의 H(A1) 이
     이동 도메인으로 파생되지 않게). 와이어 `services[].kind` = 고른 레코드의 kind.
   - **단말 도달 정보**(`host,port,tcp_port,tls_port,transport,ipsec_port_ps/pc,sms_gateway,max_payload_*`) ← CSC 설정
     `Provisioning.Services.<kind>`(`volte` · `voip` · `ptt`) — 레코드 name 과 같은 `name` 의 항목, 없으면 레코드 kind 키. CSP 레코드에
     없는 값이라 CSC 설정이 가진다. **CSP/PSP 의 `local_nodes` bind_port 를 바꾸면 이 값도 같이 맞춘다.** 포트 3개가 가용 transport
     목록으로 조립된다(`tcp_port=0`→평문 포트 공용, `tls_port=0`→TLS 미광고). `host` 빈값이면 요청 Host(=UE 가 접속한 CSC IP)를
     사용(올인원 기본). 다중 노드면 volte=CSP, ptt=PSP 대표/VIP 주소로 채운다.
   - 미러가 없으면(미배포·store 비공유) `Provisioning.Services.<kind>` 의 `name·domain·media_srtp·sec_mechanisms` 를 **폴백**으로 쓴다 —
     운영 규약으로 CSP 와 맞추며, 미러와 도메인이 어긋나면 CSC 가 드리프트 경고를 남기고 미러를 쓴다. 관제 앱 관리 API
     `GET /provisioning/directory/admin` 의 `services.<kind>[].name` 후보도 미러 우선(패키지 기본 `volte`/`ptt` — csp/pkg.json.
     configure `--volte-service/--ptt-service`).

   설정 소유자는 `csc/config/config_template.json` 의 `provisioning` 섹션(`scope: service`)이다:
   - 콘솔 `관리 > 시스템 > 시스템/인프라` → 서버 선택 → **[패키지 설정] > csc > [설정]** 탭의
     `자동 프로비저닝 (단말 접속 정보)` 에서 편집. 전 필드 `restart: true` → 저장 후 csc 재기동.
   - configure.sh 경로(올인원 시험환경)는 `deploy_value` 로 `@VOLTE_DOMAIN@`/`@PTT_DOMAIN@`/
     `@COUNTRY_CODE@` 를 치환해 csc.json 에 기록하고, **SIP 포트(`port`/`tcp_port`/`tls_port`)는
     `local_nodes.jsonl` 의 access 리스너(UDP primary/TCP/TLS)에서 유도해 기록한다**(리스너 SoT 와
     단일화 — 템플릿 default 15060 은 운영 표준 배치용이라 dev 시드 5060/25061/5061 과 다르다).
     host 가 비어 있거나 CSP_IP 인 서비스만 정합하고, 다른 서버를 가리키면 운영자 값을 보존한다.
3. SIP 자격: 응답 `account.sipHa1`(H(A1)) 로 인증한다 — 평문 SIP 비밀번호는 망에 실리지 않고 단말도 갖지
   않는다. `sipHa1` 이 없는 가입(H(A1) 미생성)은 단말이 등록을 시도하지 않는다.
4. **홈 국가코드** ← CSC 설정 `Provisioning.CountryCode`(템플릿 default 82, configure.sh `--country-code`).
   미설정 시 로그인 msisdn 에서 유도(`_country_code_of`, 단말 fallback 과 동일한 ITU 자릿수 규칙).
   응답 `countryCode` 로 내려주며 단말은 이 값을 번호 로컬 표기의 SoT 로 저장(`SipAccountConfig.countryCode`).
5. **내선 라벨 자릿수** ← CSC 설정 `Provisioning.ExtensionDigits`(템플릿 default 4, 0=전체 digits) —
   `dispatch.members[].extension` 이 가입 번호 끝자리 몇 자리인가([volte_supplementary_services.md](volte_supplementary_services.md) §4
   의 "내선 대역은 프로비저닝 규약" 축).

> 참고: 이는 TS 24.484 CMS 설정 플레인의 **확장**으로 볼 수 있다(표준 user-profile/service-config 는 SIP 코어 접속 주소를 담지 않으므로 본 프로젝트 전용 프로비저닝 문서로 정의). 서버 정합 갭은 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) 와 함께 관리.

## 5. 클라이언트 구현 (core + 각 앱)

- **core `provision/`** (공유): `Pkce`(PKCE S256), `ProvisioningClient`(IdMS 로그인 + `/provisioning/me` 조회, OkHttp), `ProvisioningModels`(ProvisioningProfile/ServiceProfile/SipServer/AccountInfo/TokenSet), `ServiceProfile.toSipAccountConfig(loginId, displayName, countryCode)`.
- **volte-client / ptt-client**: 첫 진입 = `LoginScreen` → `ProvisioningClient` → 자기 kind 프로파일을 `ConfigStore` 에 저장 → 홈. 수동 설정은 §5-1 수동 설정 모드.
- 토큰: access_token 보관, 만료 시 재로그인(또는 refresh). SIP 자격(`sipHa1`) 미수신 시 등록하지 않는다.
- 서버 엔드포인트 준비 전: 로그인/프로비저닝 실패 시 **수동설정으로 graceful fallback**.

### 5-1. 설정 화면·수동 설정 모드 (volte-client)

- 설정 탭 = **안드로이드 설정 스타일**(`SettingsScreen`): 카테고리(구성/서버/계정/고급) + 항목 행
  (제목+현재값 요약), 항목 탭 = 편집 다이얼로그(텍스트/라디오), 변경 즉시 저장·재등록(별도 저장 버튼 없음).
- **SSO 자동 구성 상태에선 전 항목 읽기 전용**(흐림 처리) — 값의 SoT 는 CIMS 프로비저닝이며 앱
  진입 시 재프로비저닝이 덮어쓰므로 편집을 허용하지 않는다.
- **예외: 전송 프로토콜** — 서버가 가용 목록을 2개 이상 알렸으면 SSO 상태에서도 고를 수 있다.
  선택지는 그 목록으로 한정되고(라벨 = `TLS · 15061`), 고른 값은 재프로비저닝에도 유지된다.
  transport 는 서버가 강제하는 값이 아니라 단말이 고르는 값이기 때문이다
  ([sip_tls_signaling.md §7.1](sip_tls_signaling.md) 정본).
- **수동 설정 모드**(구성 카테고리 스위치, `ConfigStore.isManual`): 테스트용으로 켜면
  ①SSO 재프로비저닝(GATE·SipService autostart)이 저장값을 덮어쓰지 않고 ②전 항목 편집 가능.
  끄면 즉시 재프로비저닝으로 서버 값 복원(실패 시 다음 진입에서 복원). GATE "수동 설정 (고급)"
  진입(프로비저닝 실패/계정 없음 fallback)도 같은 화면(standalone, 완료/취소 버튼)이며 수동 모드를 켠다.
- CIMS 계정이 아예 없는 단말은 수동 구성으로 동작(스위치 없이 편집 가능).

**ptt-client** 는 수동 설정 화면을 두지 않고 설정 탭에 두 항목만 노출한다 — `통신 설정` 의
**SIP 전송 프로토콜**(가용 목록에서 선택, 2개 미만이면 행 숨김)과 `기타` 의 **서버 설정 다시 받기**
(`/provisioning/me` 재취득 — 포트·가용 목록·비번 최신화). 전송 프로토콜을 바꾸면 계정을 다시 만들어야
하므로 un-REGISTER 후 프로세스가 재시작되고(2초) 참여 채널은 자동 복원된다.

## 6. 미해결/후속

- CSC 주소 기본값(빌드 설정) vs 입력 — 현재 입력(기본값 채움). 사내 배포 시 기본값 고정 가능.
- 다중 서비스 동시(한 단말이 VoLTE+PTT 둘 다) — 현재는 앱별 단일 서비스. 통합 앱 시 확장.
- refresh_token 회전·EncryptedSharedPreferences(토큰/비번 보관). (로그아웃은 §1-1 로 구현됨.)
