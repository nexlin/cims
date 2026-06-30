# 안드로이드 UE 로그인·자동 프로비저닝 설계

> **목적**: 단말에서 서버/계정을 수동 입력하지 않도록, **로그인 1회 → 서버가 접속·계정 정보를 내려주고
> 단말이 자동 구성**한다. VoLTE(CSP)와 PTT(PSP)가 **다른 서버**일 수 있으므로 응답은 **서비스별 프로파일**
> 목록으로 구성한다. 신원·설정 플레인은 **CSC(IdMS)** 한 곳이며, 시그널링은 서비스별 서버로 분기한다.
>
> 본 기능은 **클라이언트 + 서버** 양쪽으로 구현돼 있다. 클라이언트는 contract 에 맞춰 동작하고
> (실패 시 수동설정 fallback), 서버는 CSC 가 `GET /provisioning/me` 를 제공한다(아래 §4). 서비스별
> 시그널링 도메인/주소는 `access_services` 확장 대신 CSC 설정 `Provisioning.Services.<kind>` 로 내려준다.

---

## 1. 흐름

```
앱 첫 실행 → [로그인 화면]  (CSC 주소 + 아이디 + 비번; CSC 주소는 앱 기본값/빌드설정 가능)
  → IdMS OAuth2 PKCE 인증(TS 33.180) → access_token
  → GET /provisioning/me (Bearer)  → 서비스별 프로파일 수신
  → 앱이 자기 service kind 프로파일로 SipAccountConfig 자동 구성·저장
  → [홈/통화 화면]   (수동 설정은 "고급" fallback 으로만 노출)
```

- 로그인은 **CSC(IdMS) 한 곳**. CSP/PSP 시그널링 서버 주소는 프로비저닝 응답으로 받는다.
- **volte-client** 는 `kind=="volte"`, **ptt-client** 는 `kind=="ptt"` 프로파일을 사용. 두 앱은 별도 APK 라 각자 로그인(같은 자격증명).

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
  "services": [
    {
      "kind": "volte",
      "sip":     { "host": "<CSP host>", "port": 5060, "transport": "UDP",
                   "domain": "ims.mnc033.mcc450.3gppnetwork.org" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000001",
                   "authId": "", "sipPassword": null }
    },
    {
      "kind": "ptt",
      "sip":     { "host": "<PSP host>", "port": 5060, "transport": "UDP",
                   "domain": "ptt.mnc033.mcc450.3gppnetwork.org" },
      "account": { "msisdn": "+821300000001", "imsi": "450330000000002",
                   "authId": "", "sipPassword": null, "mcpttId": "tel:+821300000001" }
    }
  ]
}
```

필드 규칙:
- `sip.host/port/transport/domain`: 단말이 접속할 **서비스별 시그널링 서버**. VoLTE=CSP, PTT=PSP (다를 수 있음).
- `account.imsi`: Digest username = `imsi@sip.domain`(서버 CscfModule 강제). 서비스별로 다를 수 있음.
- `account.msisdn`: 공개 ID(AOR user part). `authId`: 전체 IMPI 직접지정(보통 빈값 → imsi@domain 합성).
- `account.sipPassword`: **옵션**. `null`/생략 시 단말은 **로그인 비번을 SIP Digest 비번으로 재사용**(권장 — 비번이 망에 안 돌아다님). 서비스별 SIP 비번이 다른 정책이면 명시.
- `account.mcpttId`: PTT 프로파일에만. GMS/CMS/affiliation/floor 에서 사용.

오류: 토큰 무효 401. 사용자에 해당 서비스 없으면 `services` 에서 제외(빈 배열 가능).

## 4. 서버측 구현 (CSC)

1. **엔드포인트 `GET /provisioning/me`** (`csc/src/services/mcptt.py` `handle_provisioning_me`, CSC mcptt 서버 4430):
   - 인증: Bearer access_token → `mcptt_id`(또는 sub) → msisdn 추출.
   - 조회: 로그인 msisdn 으로 person(`user_id`) 확인 → 그 person 의 `volte_subscriptions`+`ptt_subscriptions`
     전 서비스를 반환(로그인 1회로 보유 서비스 모두). 계정: id(msisdn)/imsi/auth_id.
   - 사용자: `users.name` → displayName.
2. **시그널링 도메인/주소** ← CSC 설정 `Provisioning.Services.<kind>` `{host,port,transport,domain}`
   (configure.sh 가 `VOLTE_DOMAIN`/`PTT_DOMAIN` 으로 주입). `host` 빈값이면 요청 Host(=UE 가 접속한 CSC IP)를
   사용(올인원 기본). 다중 노드면 volte=CSP, ptt=PSP 대표/VIP 주소로 채운다.
   (표준 `access_services` 는 CSP 컬렉션이라 CSC 가 직접 못 읽으므로, 시그널링 매핑은 CSC 설정으로 둔다.)
3. 비번: 응답 `sipPassword=null` → 단말이 로그인 비번을 SIP Digest 비번으로 재사용(망에 SIP 비번 미전송).
   서비스별 SIP 비번이 다르면 응답에 명시.

> 참고: 이는 TS 24.484 CMS 설정 플레인의 **확장**으로 볼 수 있다(표준 user-profile/service-config 는 SIP 코어 접속 주소를 담지 않으므로 본 프로젝트 전용 프로비저닝 문서로 정의). 서버 정합 갭은 [mcptt_standard_conformance.md](mcptt_standard_conformance.md) 와 함께 관리.

## 5. 클라이언트 구현 (core + 각 앱)

- **core `provision/`** (공유): `Pkce`(PKCE S256), `ProvisioningClient`(IdMS 로그인 + `/provisioning/me` 조회, OkHttp), `ProvisioningModels`(ProvisioningProfile/ServiceProfile/SipServer/AccountInfo/TokenSet), `ServiceProfile.toSipAccountConfig(loginPassword)`.
- **volte-client / ptt-client**: 첫 진입 = `LoginScreen` → `ProvisioningClient` → 자기 kind 프로파일을 `ConfigStore` 에 저장 → 홈. 수동 `ConfigScreen` 은 "고급" fallback.
- 토큰: access_token 보관, 만료 시 재로그인(또는 refresh). SIP 비번 미수신 시 로그인 비번 재사용.
- 서버 엔드포인트 준비 전: 로그인/프로비저닝 실패 시 **수동설정으로 graceful fallback**.

## 6. 미해결/후속

- CSC 주소 기본값(빌드 설정) vs 입력 — 현재 입력(기본값 채움). 사내 배포 시 기본값 고정 가능.
- 다중 서비스 동시(한 단말이 VoLTE+PTT 둘 다) — 현재는 앱별 단일 서비스. 통합 앱 시 확장.
- refresh_token 회전·로그아웃·EncryptedSharedPreferences(토큰/비번 보관).
