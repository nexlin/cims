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
  "csc":   { "host": "<CSC host>", "port": 4430 },
  "countryCode": "82",
  "services": [
    {
      "kind": "volte",
      "sip":     { "host": "<CSP host>", "port": 15060, "transport": "UDP",
                   "transports": [ { "transport": "UDP", "port": 15060 },
                                   { "transport": "TCP", "port": 15060 },
                                   { "transport": "TLS", "port": 15061 } ],
                   "default": "UDP",
                   "domain": "ims.mnc033.mcc450.3gppnetwork.org" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000001",
                   "authId": "", "sipHa1": "5f4dcc3b5aa765d61d8327deb882cf99", "sipPassword": null }
    },
    {
      "kind": "ptt",
      "sip":     { "host": "<PSP host>", "port": 15061, "transport": "TLS",
                   "transports": [ { "transport": "TLS", "port": 15061 } ],
                   "default": "TLS", "enforced": true,
                   "domain": "ptt.mnc033.mcc450.3gppnetwork.org" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000002",
                   "authId": "", "sipHa1": null, "sipPassword": null, "mcpttId": "tel:+821300000001" }
    }
  ]
}
```

필드 규칙:
- `sip.host/domain`: 단말이 접속할 **서비스별 시그널링 서버**. VoLTE=CSP, PTT=PSP (다를 수 있음).
- `sip.transports`/`sip.default`: **가용 transport 목록과 기본값(권장)** — 단말이 이 중에서 고른다.
  transport 마다 포트가 다르므로 목록에 포트가 함께 실린다(같은 포트로 평문과 TLS 를 겸하지 않는다).
  TLS 포트 미설정(`tls_port=0`)이면 TLS 항목이 실리지 않는다. 선택·유지·반영 규칙은
  [sip_tls_signaling.md §7.1](sip_tls_signaling.md) 이 정본.
- `sip.port`/`sip.transport`: **기본값의 유효 쌍** — 목록을 모르는 구 APK 가 이 두 필드만 읽으므로 유지된다.
- `sip.enforced`: `true` 면 서버가 그 transport 를 **집행**한다(가입자 `sip_transport=TLS` —
  [sip_access_security.md §3](sip_access_security.md)). 목록은 TLS 하나로 좁혀지고, 다른 채널의 요청은
  REGISTER 포함 403 이다. `false` 면 목록 안에서 단말이 고른다.
- `account.imsi`: Digest username = `imsi@sip.domain`(서버 CscfModule 강제). 서비스별로 다를 수 있음.
- `account.msisdn`: 공개 ID(AOR user part). `authId`: 전체 IMPI 직접지정(보통 빈값 → imsi@domain 합성).
- `account.sipHa1`: **서비스 가입(subscription) 의 SIP Digest H(A1)**(`*_subscriptions.ha1` =
  `MD5(imsi@domain:realm:password)`). CIMS 로그인(IdMS `users.passwd`)과 **별개 자격증명** — 단말은 이 값을
  pjsip `PJSIP_CRED_DATA_DIGEST` 자격으로 넣어 원문 없이 response 를 계산한다.
- `account.sipPassword`: 과도기 평문(`*_subscriptions.passwd`). DB 의 평문이 소거되면 항상 `null`.
  단말은 `sipHa1` → `sipPassword` → 로그인 비번(으로 ha1 계산) 순으로 쓴다.
- `account.mcpttId`: PTT 프로파일에만. GMS/CMS/affiliation/floor 에서 사용.
- `countryCode`: 홈 국가코드(E.164 digits, `+` 없음. 예 `"82"`) — 단말 번호 로컬 표기(§3-1)의 **SoT**.
  CSC 설정 `Provisioning.CountryCode` 우선, 미설정이면 로그인 msisdn 에서 서버가 유도. 판정 불가면
  빈 문자열(`""`) — 명시적 `null` 은 보내지 않는다(Android `org.json` 이 `"null"` 문자열로 오독).

오류: 토큰 무효 401. 사용자에 해당 서비스 없으면 `services` 에서 제외(빈 배열 가능).

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

## 4. 서버측 구현 (CSC)

1. **엔드포인트 `GET /provisioning/me`** (`csc/src/services/mcptt.py` `handle_provisioning_me`, CSC mcptt 서버 4430):
   - 인증: Bearer access_token → `mcptt_id`(또는 sub) → msisdn 추출.
   - 조회: 로그인 msisdn 으로 person(`user_id`) 확인 → 그 person 의 `volte_subscriptions`+`ptt_subscriptions`
     전 서비스를 반환(로그인 1회로 보유 서비스 모두). 계정: id(msisdn)/imsi/auth_id.
   - 사용자: `users.name` → displayName.
2. **시그널링 도메인/주소** ← CSC 설정 `Provisioning.Services.<kind>`
   `{host,port,tcp_port,tls_port,transport,domain}`. 포트 3개가 가용 transport 목록으로 조립된다
   (`tcp_port=0`→평문 포트 공용, `tls_port=0`→TLS 미광고).
   `host` 빈값이면 요청 Host(=UE 가 접속한 CSC IP)를 사용(올인원 기본). 다중 노드면 volte=CSP,
   ptt=PSP 대표/VIP 주소로 채운다.
   (표준 `access_services` 는 CSP 컬렉션이라 CSC 가 직접 못 읽으므로, 시그널링 매핑은 CSC 설정으로 둔다.
   **따라서 CSP/PSP 의 `local_nodes` bind_port 를 바꾸면 이 값도 같이 맞춰야 한다** — 두 값은 의도적 중복이다.)

   설정 소유자는 `csc/config/config_template.json` 의 `provisioning` 섹션(`scope: service`)이다:
   - 콘솔 `관리 > 시스템 > 시스템/인프라` → 서버 선택 → **[패키지 설정] > csc > [설정]** 탭의
     `자동 프로비저닝 (단말 접속 정보)` 에서 편집. 전 필드 `restart: true` → 저장 후 csc 재기동.
   - configure.sh 경로(올인원 시험환경)는 `deploy_value` 로 `@VOLTE_DOMAIN@`/`@PTT_DOMAIN@`/
     `@COUNTRY_CODE@` 를 치환해 csc.json 에 기록한다. 포트/host 는 템플릿 default(5060 / 빈값).
3. 비번: 응답 `sipPassword=null` → 단말이 로그인 비번을 SIP Digest 비번으로 재사용(망에 SIP 비번 미전송).
   서비스별 SIP 비번이 다르면 응답에 명시.
4. **홈 국가코드** ← CSC 설정 `Provisioning.CountryCode`(템플릿 default 82, configure.sh `--country-code`).
   미설정 시 로그인 msisdn 에서 유도(`_country_code_of`, 단말 fallback 과 동일한 ITU 자릿수 규칙).
   응답 `countryCode` 로 내려주며 단말은 이 값을 번호 로컬 표기의 SoT 로 저장(`SipAccountConfig.countryCode`).

> 참고: 이는 TS 24.484 CMS 설정 플레인의 **확장**으로 볼 수 있다(표준 user-profile/service-config 는 SIP 코어 접속 주소를 담지 않으므로 본 프로젝트 전용 프로비저닝 문서로 정의). 서버 정합 갭은 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) 와 함께 관리.

## 5. 클라이언트 구현 (core + 각 앱)

- **core `provision/`** (공유): `Pkce`(PKCE S256), `ProvisioningClient`(IdMS 로그인 + `/provisioning/me` 조회, OkHttp), `ProvisioningModels`(ProvisioningProfile/ServiceProfile/SipServer/AccountInfo/TokenSet), `ServiceProfile.toSipAccountConfig(loginPassword)`.
- **volte-client / ptt-client**: 첫 진입 = `LoginScreen` → `ProvisioningClient` → 자기 kind 프로파일을 `ConfigStore` 에 저장 → 홈. 수동 설정은 §5-1 수동 설정 모드.
- 토큰: access_token 보관, 만료 시 재로그인(또는 refresh). SIP 비번 미수신 시 로그인 비번 재사용.
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
