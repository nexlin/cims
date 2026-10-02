# MCX 신원·토큰·scope 모델 — IdMS 발급 규칙과 리소스 서버 인가

> CSC IdMS 가 발급하는 토큰의 신원 claim·scope 와, GMS/CMS/KMS/MCData FD 가 그 토큰을 검사하는 규칙의 정본.
> 규격 = 3GPP TS 33.180 Annex B(MCX Connect 프로파일), TS 23.280 §10.1.4.1(MC service ID), TS 24.482(IdMS 절차),
> RFC 6749/6750/7519/7662. 구현 = [csc/src/services/mcptt.py](../../../csc/src/services/mcptt.py)
> (`grant_scope`·`create_tokens`·`require_scope`·`resolve_idms_identity`), [mcdata_fd.py](../../../csc/src/services/mcdata_fd.py).
> IdMS 로그인 흐름(폼·PKCE·redirect)은 [mcptt_standard_conformance.md §3](mcptt_standard_conformance.md) 과
> [modules/csc.md](../modules/csc.md) 가 정본이고, 이 문서는 **발급되는 내용과 그 소비 규칙**만 다룬다.

## 1. 신원 모델 — 단일 MC service ID

| 규격 개념 | CIMS 값 | 근거 |
|---|---|---|
| MC ID (IdMS 로그인 신원) | `users.login_id` (예 `test003`) → 토큰 `sub` | TS 33.180 B.2.1.2 |
| MCPTT ID | `tel:+E.164` (PTT 가입 번호) → `mcptt_id` | TS 23.379 |
| MCData ID | **MCPTT ID 와 같은 값** → `mcdata_id` | TS 23.280 §10.1.4.1 "사업자가 단일 MC service ID 를 요구하면 값이 같다" |
| MCVideo ID | **MCPTT ID 와 같은 값** → `mcvideo_id` — MCVideo 이용 자격(`mcvideo_user_profile` 행)이 있는 사용자만 | TS 23.280 §10.1.4.1 · [mcvideo.md](mcvideo.md) §7 D1 |

CIMS 는 사업자 하나·서버 한 벌(CSC+CSP)·가입 테이블 하나(`ptt_subscriptions`)·그룹 문서 하나(`allow_sds`/`allow_fd`
공용)인 단일 서비스 도메인이라 서비스별 ID 를 나누지 않는다. 단일 ID 구성이 규격에 지우는 의무 하나는 "통신 중 어느
MC 서비스인지 표시"(같은 절)이며, SIP 평면은 Content-Type(`application/vnd.3gpp.mcdata-*`), 토큰 평면은 서비스별
claim(`mcptt_id`/`mcdata_id`)과 scope 로 충족한다.

**MC service ID 는 PTT 가입이 있는 사람만 가진다**(TS 24.482 §4.1 · TS 33.180 B.4.2.2 — 토큰의 scope·MC service ID 는 그 사용자가
인가된 MC 서비스로 정해진다). 로그인 계정의 `mcptt_id` 는 그 사람의 `ptt_subscriptions` 회선에서만 만든다(`_load_login_accounts`).
전화 전용(volte·voip) 계정은 IdMS 로그인은 되지만(프로비저닝 `/provisioning/*` 용) 토큰에 `mcptt_id`·`mcdata_id`·`mcvideo_id` 가
없고 MC scope 를 받지 못한다 — CMS·GMS 는 본인 문서 대조에서 403, KMS 는 403 `no_mc_service_identity`. 프로비저닝 계열은 토큰의
MC 신원이 없으면 로그인 계정의 첫 회선(`line_id` — ptt → volte → voip)으로 사람을 찾는다(`token_line_id`).

## 2. 토큰 claim

`create_tokens` 가 발급한다. 서명 = RS256(§2.1), 수명 = `IdMs.AccessTokenTtl`(기본 3600 s).

| 토큰 | claim | 값 | 규격 |
|---|---|---|---|
| ID token | `iss` `sub` `aud` `exp` `iat` | issuer(§7) / login_id / 요청 `client_id` / 만료 / 발급 | OIDC Core, B.2.1.2 |
| ID token | `mcptt_id` `mcdata_id` (+ `mcvideo_id`) | MC service ID (같은 값). `mcvideo_id` 는 MCVideo 자격이 있을 때만(B.2.1.3 "REQUIRED for MCVideo") | B.2.1.3 |
| ID token | `nonce` | 요청에 있을 때 반영 | OIDC Core §3.1.2.1 |
| access token | `exp` `scope` `client_id` | 만료 / **공백 구분 문자열** / 요청 `client_id` | B.2.2.2 (RFC 7662) |
| access token | `mcptt_id` `mcdata_id` (+ `mcvideo_id`) | MC service ID — `mcvideo_id` 는 ID token 과 같은 규칙 | B.2.2.3 |
| access token | `iss` `sub` `aud`(`mcptt_client`) `iat` | RFC 7519 추가 claim — 검증은 서명+`aud` | — |

토큰 응답(RFC 6749 §5.1)은 `access_token`·`id_token`·`refresh_token`·`token_type`·`expires_in`(=TTL)·**`scope`(실제 허가분)**
를 항상 싣는다. refresh 토큰은 file_store 에 원 grant scope 를 보존한다(§4 축소 규칙).

### 2.1 서명 (TS 33.180 B.2.2.1 · RFC 7515 · OIDC Core §15.1)

access token 은 JSON web digital signature 프로파일을 싣고(B.2.2.1), OP 는 ID token 을 RS256 으로 서명한다(OIDC Core §15.1). ID token·
access token 둘 다 **RS256** 으로 서명한다 — 단말(ID token 검증, B.11.1 · OIDC Core §3.1.3.7)과 분리 배치된 리소스 서버가 공개 키로
검증한다.

| 항목 | 규칙 |
|---|---|
| 서명 키 | RSA 2048. runtime store `idms_keys/signing.pem`(PKCS#8, 0600) — 없으면 처음 기동 때 만든다. HA 쌍은 같은 runtime store 라 같은 키. `IdMs.SigningKeyFile` 로 다른 경로 지정 (`services/idms_keys.py`) |
| `kid` | JWS 헤더에 싣는다 — RFC 7638 JWK thumbprint |
| 공개 키 | `GET /idms/jwks`(RFC 7517, 인증 없음) — discovery `jwks_uri`. `kty RSA`·`use sig`·`alg RS256`·`kid`·`n`·`e` |
| 키 교체 | `signing.pem` 을 `previous-<이름>.pem` 으로 바꾸고 재기동 → 새 키로 서명, `previous-*.pem` 은 검증·JWKS 에만 쓰인다. 토큰 수명(`IdMs.AccessTokenTtl`)이 지나면 previous 를 지운다 |
| 검증 (`validate_access_token`) | 헤더 `alg` 로 키 종류를 골라 **그 방식 하나로만** 검증 — `RS256` = `kid` 의 공개 키(모르는 `kid` 거절), `HS256` = 공유 비밀(아래 전환기), 그 밖(`none` 포함) 거절. 공개 키를 HMAC 비밀로 쓴 토큰(알고리즘 바꿔치기)은 걸러진다 |
| 전환기 | `IdMs.AcceptHs256`(기본 true) — RS256 로 올리기 전에 나간 HS256 토큰을 만료까지 받는다. 끄면 401 `invalid_token` → 단말은 refresh 로 RS256 토큰을 받는다(refresh token 은 서명과 무관한 불투명 값) |
| `IdMs.SigningAlg` | `RS256`(기본) / `HS256`(공유 비밀 `IdMs.JwtSecret` MAC — 옛 방식). 서명 키를 읽지 못하면 오류 로그를 남기고 HS256 으로 선다(로그인은 계속 된다) |

## 3. scope 카탈로그

TS 33.180 B.4.2.2 의 MC 서비스 scope 중 CIMS 가 제공하는 서비스 분과 자체 scope 하나.

| scope | 여는 것 | 검사하는 리소스 서버 |
|---|---|---|
| `openid` | ID token 발급 (OIDC) | — |
| `cims:provisioning` | 자체 부트스트랩 `/provisioning/me`·`/directory`·`/history` | provisioning 핸들러 |
| `3gpp:mc:ptt_service` | MCPTT 서비스 사용자 자격 | (P5 이후 CSP REGISTER — §10) |
| `3gpp:mc:data_service` | MCData 서비스 — FD 업로드/다운로드 | `/mcdata/fd` |
| `3gpp:mc:ptt_group_management_service` / `3gpp:mc:data_group_management_service` | GMS XCAP 그룹 문서 | `/org.openmobilealliance.groups` (둘 중 하나) |
| `3gpp:mc:ptt_config_management_service` | CMS user-profile · service-config | `/org.3gpp.mcptt.*` |
| `3gpp:mc:data_config_management_service` | (MCData 문서 서빙 시 — §10) | — |
| `3gpp:mc:ptt_key_management_service` / `3gpp:mc:data_key_management_service` | KMS | `/keymanagement/*` (둘 중 하나) |

| `3gpp:mc:video_service` | MCVideo 서비스 사용자 자격 | (CSP REGISTER — §10) |
| `3gpp:mc:video_group_management_service` | GMS XCAP 그룹 문서(MCVideo 몫 포함 — 한 문서) | `/org.openmobilealliance.groups` (ptt·video·data 중 하나) |
| `3gpp:mc:video_config_management_service` | CMS MCVideo user profile · service config | `/org.3gpp.mcvideo.*` |
| `3gpp:mc:video_key_management_service` | KMS | `/keymanagement/*` (ptt·video·data 중 하나) |

discovery `scopes_supported` 는 카탈로그 전체와 §5 별칭을 광고한다.

## 4. 발급 규칙

- **허가 = 요청 ∩ 카탈로그** (`grant_scope`). 모르는 값은 조용히 제외하고(RFC 6749 §3.3 허용 동작) 응답 `scope` 로
  실제 허가분을 알린다. 요청 자체는 거절하지 않는다(`invalid_scope` 없음).
- `openid` 부재를 거절하지 않는다 — 규격 SDK 중 `openid` 를 빼고 요청하는 구현이 있어(실측) ID token 은 항상 발급한다.
- **사용자 단위 인가**: MC 서비스 scope(12종·구 별칭)는 **PTT 가입이 있는 계정에만** 준다(`grant_scope(…, mc_user)` — 전화 전용 계정은
  `openid`·`cims:provisioning` 만. refresh 도 지금 계정으로 다시 판정한다). PTT 가입자는 MCPTT·MCData 8종 전부를 받을 수 있다(MCData 만 막는 가입자 플래그 없음 — §10). MCVideo 4종은
  MCVideo 이용 자격(`mcvideo_user_profile` 행)이 있을 때만 준다(`grant_scope(requested, mcptt_id)` — 없으면 제외하고 응답 `scope` 로 알린다).
- **refresh 축소**: refresh 요청에 `scope` 가 있으면 `expand(요청) ∩ expand(원 grant)` 로 좁혀 access 를 발급하고,
  회전된 refresh 는 원 grant(broad)를 그대로 보존한다. 교집합이 비면 원 grant 로 발급한다. CIMS 앱의 AccountManager 가
  용도별(provisioning / MC 서비스) 토큰을 이 경로로 따로 받는다.
- authreq 두 말투(자격 쿼리 간이형 / 규격 폼) 모두 같은 규칙을 거친다(`_issue_auth_code` 는 요청 scope 를 저장, 허가 계산은
  tokenreq 에서).
- **refresh 때 계정 재확인**(B.5.3 RECOMMENDED): refresh token 은 발급 때의 자격 지문(`cred` — 로그인 ID·비밀번호의 해시)을 들고,
  재발급 때 지금 계정과 대조한다. 계정이 지워졌거나 비밀번호가 바뀌었으면 그 refresh token 을 회수하고 `invalid_grant`.
  refresh 요청의 `client_id` 는 필수가 아니다(표 B.5.2-1) — 실려 오면 발급 때의 값과 같아야 한다.
- **토큰 응답은 캐시 금지** — 성공·오류 모두 `Cache-Control: no-store`·`Pragma: no-cache`(B.4.2.5·B.5.3, RFC 6749 §5.1).

### 4.1 클라이언트 등록·필수 파라미터 (TS 33.180 B.3 · B.4.2.2 · B.4.2.4)

클라이언트는 IdMS 에 등록돼 있어야 하고 `client_id`·`redirect_uri` 는 등록 값과 같아야 한다. 등록 저장소 = 설정 `IdMs.Clients`
(`[{ClientId, RedirectUris}]`), 판정 = `IdMs.ClientEnforcement`(SIGUSR1 리로드):

| 모드 | 동작 |
|---|---|
| `log` (기본) | 판정만 계산해 통과. `[IdMS][client] would-reject stage=authreq\|tokenreq client_id=… problems=…` 한 줄 — 어떤 `client_id`·`redirect_uri` 가 쓰이는지 모으는 창 |
| `enforce` (최종 상태) | 400 — 인증 요청 `invalid_request`, 토큰 요청 `invalid_grant`, `error_description` 에 첫 사유. `IdMs.Clients` 가 비면 모든 요청이 거절된다 |
| `off` | 검사 없음 |

검사하는 것 — **인증 요청**(표 B.4.2.2-1): `response_type`·`client_id`·`scope`(`openid` 포함)·`redirect_uri`·`state`·`acr_values`
(`3gpp:acr:password` 포함) 필수 + `client_id` 등록 + `redirect_uri` 가 그 클라이언트의 등록 값. **토큰 요청**(표 B.4.2.4-1): `client_id`·
`redirect_uri` 필수 + 등록 대조. 모드와 무관하게 늘 집행하는 것 = PKCE S256(`code_challenge` 필수)·`response_type` 이 있으면 `code`·
토큰 요청의 `redirect_uri` 가 실려 오면 인증 요청의 값과 일치·전역 `IdMs.RedirectUriAllow`(채웠을 때).
**롤아웃**: `log` 로 내보내 우리 앱·협력업체·외부 SDK 의 값을 모은다 → `IdMs.Clients` 등록(자체 단말의 간이형 요청은 `state`·
`acr_values`·`response_type` 을 싣게 고친 뒤) → `enforce`.

## 5. 전환기 별칭 `3gpp:mcptt:ptt_server`

구 단일 scope(TS 33.179 표기)를 요청하는 클라이언트(구 Android/SDK 빌드·협력업체 단말·외부 SDK)가 이행 뒤에도 그대로
동작하도록 다음 넷을 지킨다.

1. **확장 범위 = MCPTT·MCData scope 8종 전체.** 종전에 그 하나가 GMS·CMS·KMS·FD 를 모두 열어 주던 의미와 같다. 일부만
   대응하면 구 클라이언트가 퇴행한다. MCVideo 넷은 별칭에 들지 않는다 — 별칭이 열던 범위에 없던 서비스이고, 구 클라이언트는 MCVideo 를 쓰지 않는다.
2. **구 문자열 병기.** 토큰·응답 `scope` 에 확장분과 함께 `3gpp:mcptt:ptt_server` 를 남긴다(요청 scope 를 문자열 대조하는
   클라이언트 호환).
3. **적용 지점 셋.** 발급(`grant_scope`)·refresh 축소(양쪽 확장 후 교집합)·리소스 서버 검사(`token_scopes` 가 검사 시점에도
   확장 — 이행 전 발급된 배열형 토큰과 구 refresh 저장분 호환). 재로그인을 요구하지 않는다.
4. **discovery 병기.** `scopes_supported` 에 신 이름과 함께 광고한다.

제거 조건: 신 이름을 요청하는 우리 Android/SDK 빌드가 전 단말에 배포되고, 협력업체가 신 이름으로 옮기거나 불필요를 확인한
뒤. 제거 = `SCOPE_ALIASES` 항목 삭제 + discovery 갱신(별도 결정, 자동 아님). 제거 뒤 구 문자열만 요청한 단말은 `openid`·
`cims:provisioning` 만 받아 §6 의 403 을 만난다.

## 6. 리소스 서버 scope 검사

`require_scope(args, token, endpoint, *accepted)` — `accepted` 중 하나가 토큰에 있으면 통과(TS 33.180 B.10).
`unauthorized()` — Authorization 에 Bearer 토큰이 **없으면 403**(TS 24.482 A.2.3 1)), 토큰이 있었는데 검증에 실패했으면(만료·서명 불일치)
**401** + `WWW-Authenticate: Bearer realm="<domain>", error="invalid_token"`(A.2.3 2)a) → RFC 6750 §3.1 — 단말이 토큰을 갱신하는 신호).

| 모드 (`IdMs.ScopeEnforcement`, SIGUSR1 리로드) | 동작 | 용도 |
|---|---|---|
| `enforce` (템플릿 기본값 = 최종 상태) | 부족 → **403** `{"error":"insufficient_scope","required":[…]}` + `WWW-Authenticate: Bearer realm=…, error="insufficient_scope", scope="<필요 scope>"` (RFC 6750 §3.1) | 운영 |
| `log` | 판정만 계산해 통과. 로그 한 줄 `[IdMS][scope] would-deny endpoint=… method=… mcptt_id=… client_id=… granted=… required=…` | 라이브 관찰 창 |
| `off` | 검사 없음 | 비상 롤백 |

provisioning 3종은 종전 규칙(빈 scope 는 레거시 허용, `cims:provisioning` 부재는 403)을 유지하고 응답 헤더만 같은 형식이다.

**롤아웃 절차**: 개발 서버 `enforce` + S1/S3 그린 → 라이브는 콘솔 값 `log` 로 배포 → 클라이언트 종류마다(우리 Android 앱·
Windows 관제 앱·협력업체 단말 APK·외부 SDK) 로그인/refresh 와 GMS·CMS·KMS·FD 사용이 한 번 이상 지나고 `would-deny` 0건
확인 → 콘솔에서 `enforce` 로 전환(update_config, 재기동 없음) → 문제 시 `log` 로 즉시 복귀. 이행 전 발급된 access token 은
TTL(1 h) 안에 소멸한다.

## 7. issuer · 도메인 · discovery

`resolve_idms_identity` — 설정 명시값 > 유도값. 템플릿 기본값이 비어 있어 배포 overlay 에 실리지 않으므로, 운영자가
콘솔에 적을 때만 그 값을 쓴다(OAM 배포 경로는 `deploy_value` 를 치환하지 않는다 — [csc.md §8.1](../modules/csc.md)).

| 값 | 우선순위 |
|---|---|
| `IDMS_DOMAIN` | `IdMs.Domain` > `Provisioning.Services.ptt.domain` > 코드 기본값 |
| `IDMS_ISSUER` (`iss`, discovery `issuer`) | `IdMs.Issuer` > `McpttServer.PublicUrl`(URL 형 — TS 33.180 B.2.1.2 "IdM 서버의 URL") > `idms.<IDMS_DOMAIN>` |
| `KMS_URI` | `IdMs.KmsUri` > `kms.<IDMS_DOMAIN>` |

`iss` 는 토큰과 discovery 에 같은 문자열로 실린다. `PublicUrl` 미설정 배포는 FQDN 형(`idms.ptt.cims.example.kr`)
이다. URL 형으로 고정하려면 `McpttServer.PublicUrl` 을 단말이 실제로 도달하는 하나의 주소로 명시한다(내부/공인 두 경로가
있으면 그중 하나를 택해야 한다 — `iss` 는 하나).

discovery(`/.well-known/openid-configuration`): `issuer`, 엔드포인트 3종 + `jwks_uri`(`public_base_url`), `scopes_supported`(§3 + §5),
`claims_supported`(`sub iss iat exp aud nonce scope client_id mcptt_id mcdata_id`), PKCE `S256`,
`id_token_signing_alg_values_supported` = `RS256`(§2.1).
introspection(`/idms/introspect`, RFC 7662): `active sub iss client_id mcptt_id mcdata_id aud exp iat scope`(문자열).

## 8. 클라이언트 요청 문자열

| 클라이언트 | 로그인 요청 scope | refresh 축소 |
|---|---|---|
| Android `android/core` (`CscEndpoint.scope`) | `openid cims:provisioning` + MC 8종 | `CimsAccounts.scopeFor`: `TOKEN_PROVISIONING`→`cims:provisioning`, `TOKEN_MCPTT`→MC 8종(`SCOPE_MC_SERVICES`) |
| Android `ptt-client` (`CscConfig.scope`) | `openid` + MC 8종 | — |
| SDK `libcimsue` (`csc.h CscEndpoint.scope`) | `openid cims:provisioning` + MC 8종 | — |
| 외부 규격 SDK | 자체 설정(`3gpp:mc:*` 또는 구 별칭) | — |

## 9. 검증

- **S1-UNIT-CSC** `tests/test_csc_idms_scope.py`: 요청∩카탈로그·미지 제외·별칭 확장/병기·배열형 구 토큰 수용·claim(scope
  문자열/`client_id`/`mcdata_id`)·refresh 축소(별칭 위)·검사 3모드·RFC 6750 헤더·issuer 유도·discovery·인프로세스 발급 흐름 ·
  전화 전용 계정(MC scope·신원 없음, KMS 403, refresh 로도 못 얻음)·클라이언트 등록 3모드·필수 파라미터·토큰 요청 `redirect_uri` ·
  no-store · refresh 계정 재확인(비밀번호 변경·삭제 → 회수) · Bearer 없음 403 · 서명(RS256 헤더 `kid`·JWKS 의 n·e 로 세운 키로
  ID/access token 검증·키 영속·키 교체 뒤 previous 로 검증·HS256 전환기 수용/거절·다른 키·모르는 `kid`·알고리즘 바꿔치기·`alg none` 거절).
- **S3** `tests/csc_bootstrap_conformance.py --enforcement enforce|log|off`: Step 3 토큰 계약, Step 3c 카탈로그(video 제외)·
  scope 부족 403(`WWW-Authenticate` 에 필요 scope)·충분 200·refresh 축소/broad 보존·discovery 정합(issuer = 토큰 `iss`).

## 10. 향후 과제

- **CSP REGISTER 토큰 검증** (TS 24.379 §7.3): `<mcptt-access-token>` 에서 MCPTT ID 를 식별해 IMPU 에 결박, `3gpp:mc:ptt_service`
  검사. CSP 의 CSC HTTP 클라이언트로 `/idms/introspect` 호출. 미탑재 단말 정책과 함께 3모드 스위치로 도입.
- **MCData XCAP 문서** (TS 24.484 §10.2~10.4: UE config·user profile·service config) — 규격 MCData 클라이언트가 요구할 때.
  `3gpp:mc:data_config_management_service` 검사 대상.
- **별칭 제거** (§5 조건 충족 후).
- 사용자 단위 MCData 자격 플래그(현재는 PTT 가입자 전체 허가).
- `iss` URL 고정 = `McpttServer.PublicUrl` 운영 결정.
