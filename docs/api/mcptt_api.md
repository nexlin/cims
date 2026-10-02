# MCPTT API (3GPP TS 24.482/483/484)

CSC 의 MCPTT 서버(포트 4430)가 제공하는 3GPP 표준 MCPTT 서비스 엔드포인트.

**Base URL**: `https://<CSC>:4430`
**인증**: OAuth 2.0 (IdMS) → Access Token → 각 서비스 호출 시 `Authorization: Bearer`

---

## 1. IdMS (Identity Management Server)

MCPTT 단말 로그인 + 토큰 발급 (OAuth 2.0 Authorization Code + PKCE S256 필수).

| Method | Path | 용도 |
|---|---|---|
| GET  | `/.well-known/openid-configuration` | OIDC 디스커버리 (authorization/token/introspection endpoint 광고) |
| GET  | `/idms/authreq` | **두 말투 병행** — ① `user_name`+`user_password` 쿼리 동반: 자체 단말 간이형 → `200 JSON {code,state,Location}` ② 자격 없음(규격 OIDC Authentication Request): `client_id`·`redirect_uri`(필수)·`code_challenge`(필수)·`code_challenge_method=S256`·`scope`·`state`·`nonce`·`response_type=code` → `200 text/html` 로그인 폼 |
| POST | `/idms/authreq` | 규격 로그인 폼 제출 (`application/x-www-form-urlencoded`: 입력칸 `username`/`password` — 이름은 `IdMs.FormLoginField`/`FormPasswordField` 설정 + hidden 문맥) → 성공 **`302 Location: redirect_uri?code=…&state=…`** / 실패 `200` 폼 재표시+오류 |
| POST | `/idms/tokenreq` | `grant_type=authorization_code`(`code`·`code_verifier`·`client_id`·`redirect_uri`) 또는 `refresh_token`(+선택 `scope` 축소) → JSON(access/id/refresh token·`expires_in`·**`scope`=실제 허가분**). JSON·form-urlencoded 모두 수용. scope = 요청 ∩ 카탈로그(`openid`·`cims:provisioning`·TS 33.180 `3gpp:mc:*` 8종, 구 `3gpp:mcptt:ptt_server` 별칭) — [mcx_identity_scope.md](../design/features/mcx_identity_scope.md) |
| POST | `/idms/introspect` | 토큰 introspection (RFC 7662: `active sub iss client_id mcptt_id mcdata_id aud exp iat scope`) |

검증 공통: PKCE 누락/plain → 400, `redirect_uri` 허용목록 `IdMs.RedirectUriAllow`(비면 전부 허용,
정확 일치) 위반 → 400, 인증 실패(간이형) → 401 `access_denied`. 리소스 서버(GMS/CMS/KMS/`/mcdata/fd`)는
토큰 부재/무효 → 401 `WWW-Authenticate: Bearer`, scope 부족 → 403 `insufficient_scope` + `WWW-Authenticate: Bearer
error="insufficient_scope", scope="…"`(`IdMs.ScopeEnforcement=enforce`; `log` 는 로그만). 흐름 상세는 3GPP TS 24.482 §6.3.1 과
[mcptt_standard_conformance.md §3 IdMS](../design/features/mcptt_standard_conformance.md).

---

## 2. GMS (Group Management Server)

XCAP(RFC 4825) 리소스 기반 — TS 24.481 Ut. 인증 = `Authorization: Bearer <IdMS PKCE access token>`,
경로의 `{xui}` 는 토큰 `mcptt_id` 본인 트리만(타인 트리 403). 그룹 URI 는 시스템 관례대로 `tel:<id>`
(예 `tel:g001`, 클라이언트 생성 `tel:g-0a1b2c3d`); `sip:<id>@<PTT 도메인>` 도 같은 그룹으로 받는다.

| Method | Path | 인가 | 응답 |
|---|---|---|---|
| GET  | `/org.openmobilealliance.groups/users/{xui}` | 본인 트리 | JSON 배열 — 멤버인 그룹 + **소유(`authorized_user_id`) 그룹**(비멤버라도). 항목 `{uri, display_name, etag, member_count, is_owner}` — `is_owner` = 편집·삭제 가능 |
| GET  | `…/users/{xui}/{group_uri}` | 멤버 또는 소유자 | `application/vnd.oma.poc.groups+xml` + `ETag`; `If-None-Match` → 304 |
| PUT  | `…/users/{xui}/{group_uri}` | **신규** = 프로파일 `allow_create_group`(OAM 부여, 프로비저닝 `ptt.allowCreateGroup`) **또는** 역할 관리 범위(`ptt_group_manage=scope|all`, mcptt_authorization.md §4.1) · **기존** = 소유자 또는 관리 범위 안 그룹(`org_code` 범위 — 소유권은 유지, [dispatch_center.md §3.4](../design/features/dispatch_center.md)) | 201(신규)/200(갱신) + 문서 + `ETag`. 403 `group_creation_not_allowed` / `not_group_owner`(소유자 없는 콘솔 그룹 포함), 409 `uri_taken`(타인 소유 id — 다른 id 로), 400 `invalid_group_id`·`reserved_prefix`·`invalid_group_document`·`unknown_member`·`required_exceeds_max_members`(필수 멤버 > 정원, TS 24.379 §6.3.5.5 NOTE 4), 412 `etag_mismatch`(`If-Match` 사용 시) |
| DELETE | `…/users/{xui}/{group_uri}` | 소유자 또는 관리 범위 안 그룹 | 200. 403 `not_group_owner`, 404 |

- **신규 그룹 식별자는 클라이언트가 정한다**(XCAP 관습): `g-` + 소문자 hex 8자리(`tel:g-0a1b2c3d`). `adhoc-`/`priv-` 는
  즉석 세션 예약 접두사라 거부. 콘솔이 만든 `g001` 류는 형식이 달라도 소유자면 PUT/DELETE 가능.
- 처리 = DB(`ptt_groups`·`ptt_group_members`, 소유자 = 토큰 가입자 `users.id`) → in-memory GROUPS 동기화 →
  CSP `GROUP_CHANGED` 통지(CSP 가 xcap-diff NOTIFY 로 단말에 전파). 관리 API(4421, 콘솔 토큰)의 그룹 CRUD 와
  같은 정본·같은 동기화를 쓴다.
- **MCVideo** — MCVideo 그룹이면 같은 문서에 MCVideo `<service>`(enabler = MCVideo ICSI)·`<mcvideo-*>` 속성·entry `<mcvideo-mcvideo-id>` 가
  실린다(TS 24.481 §7.2.2, [mcvideo.md](../design/features/mcvideo.md) §5.1). PUT 에 MCVideo `<service>` 가 있으면 MCVideo 를 켜고 속성을 반영하며
  (`mcvideo-protect-*` true·범위 밖은 400), 없으면 MCVideo 상태를 그대로 둔다(전환기 규칙). 그룹 영상은 이 MCVideo 몫이 전부다 — MCPTT 몫에는
  영상 요소가 없고, PUT 본문에 TS 24.481 스키마 밖 요소 `<mcpttgi:mcptt-video>` 가 있으면 무시한다.
- PUT 본문 = **GET 이 돌려주는 문서와 같은 포맷**(아래). 없는 요소는 갱신 시 기존값 유지, 생성 시 기본값
  (prearranged, priority 5, SDS 허용, FD 불허, 긴급통화 불허, 긴급경보 허용, hang-timer 30초, maximum-duration 3600초). `<list>` 가 있으면
  멤버 전체 교체(없으면 유지) — entry uri 는 PTT 가입 번호(`tel:+E.164`, `sip:` 형 가능), 미가입 번호는 400.
  **그룹 종류 = `<mcpttgi:on-network-invite-members>`**(TS 24.481 §7.2.2 a — `true`=prearranged, `false`=chat). 이 요소가
  없을 때만 `<mcpttgi:session-type>`(규격 밖 전환기 요소 — 구 단말)을 읽고, `broadcast` 는 400(일제 통화는 호 속성 —
  [mcptt_broadcast_group_call.md](../design/features/mcptt_broadcast_group_call.md)). 그룹 호 타이머 =
  `<mcpttgi:on-network-hang-timer>`(T4 Inactivity, 0~3600초) · `<mcpttgi:on-network-maximum-duration>`(TNG3, 0~86400초) —
  xs:duration(`PT30S`), 범위 밖·형식 오류는 400. GET 은 0 을 싣지 않는다 — T4 0 = 요소 생략, chat 그룹 = TNG3 요소 생략(TNG3 를
  돌리지 않는다), 편성 그룹 TNG3 0(무제한) = `PT2147483647S`. PUT 은 그 값 이상과 `PT0S` 를 0 으로 읽는다 — 받은 문서를 그대로
  PUT 해도 값이 바뀌지 않는다([mcptt_timers.md](../design/features/mcptt_timers.md) §3). 확인 통화 설정(TS 24.481 §7.2.2 s)t)u), TS 24.379 §6.3.3.3) =
  `<mcpttgi:on-network-minimum-number-to-start>`(0~65535, 기본 0) · `<mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>`
  (TNG1, xs:duration 1~300초, 기본 5초) · `<mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>`
  (`proceed`·`abandon`, 정의 밖 값 = abandon, 기본 abandon) — GET 은 셋을 늘 싣는다. 필수 멤버 = entry 의 `<mcpttgi:on-network-required/>`
  (§7.2.4.2 — **필수 멤버에만** 싣고, PUT 의 `<list>` 교체도 이 표시를 그대로 읽는다. 정원보다 많으면 400).
  정원 `<mcpttgi:on-network-max-participant-count>` — GET 은 0(무제한)이면 싣지 않는다. 우선순위 `<mcpttgi:on-network-group-priority>`·
  entry `<mcpttgi:user-priority>` = priorityType 0~255(§7.2.4.2, 클수록 높다) — 범위 밖·정수 아님은 400. `<mcpttgi:on-network-disabled/>`
  (§7.2.2 g)) = on-network 를 끈 그룹 — GET 은 꺼진 그룹에만 싣고, PUT 은 요소가 있으면 끈다(없으면 그대로).
  GET 은 서버가 정하는 값을 더 싣는다 — `<mcpttgi:preferred-voice-encodings>`(서비스 코덱 AMR-WB, TS 24.379 §6.2.1 2)b) 단말 offer 가 따른다) ·
  `<mcpttgi:protect-media>`·`<mcpttgi:protect-floor-control-signalling>` false(없으면 GMK 필수로 읽힌다, §7.2.8 — E2E 미구현) · 규칙
  actions `<mcpttgi:on-network-allow-getting-member-list>true`(멤버의 명단 열람, §7.2.12.1). PUT 은 이 셋을 읽지 않는다.
  `<mcpttgi:authorized-user>` 는 서버가 정한다(본문의 값 무시). floor 정책(`floor_policy`/`max_talkers`)은 관리 API 전용.
  entry 의 `<mcpttgi:participant-type>` 를 생략하면 **`participant` 로 저장**된다 — 그룹 소유(chair 권한)는 member role 이
  아니라 `authorized_user_id`(= 생성자)로 판정하므로, 생성자를 chair 로 표기하려면 자기 entry 에 `chair` 를 명시한다(앱 기본 동작).
  `<cp:actions>` 의 `<mcpttgi:on-network-allow-conference-state>`(TS 24.481 §7.2.4.2, 기본 true) = 멤버의 conference 이벤트
  (RFC 4575) 구독 허용 — CSP 가 초기 SUBSCRIBE 에서 판정(TS 24.379 §10.1.3.4.1, 불허 403 `Warning: 138`).

```xml
<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="tel:g-0a1b2c3d">
    <display-name xml:lang="en-us">관제채널</display-name>
    <list>
      <entry uri="tel:+82510001001">
        <rl:display-name>관제1석</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>chair</mcpttgi:participant-type>
        <mcpttgi:user-priority>1</mcpttgi:user-priority>
      </entry>
      <entry uri="tel:+82500000001">
        <rl:display-name>테스트001</rl:display-name>
        <mcpttgi:participant-type>participant</mcpttgi:participant-type>
        <mcpttgi:user-priority>5</mcpttgi:user-priority>
      </entry>
    </list>
    <mcpttgi:mcdata-allow-short-data-service>true</mcpttgi:mcdata-allow-short-data-service>
    <mcpttgi:mcdata-allow-file-distribution>false</mcpttgi:mcdata-allow-file-distribution>
    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>10</mcpttgi:on-network-max-participant-count>
    <mcpttgi:preferred-voice-encodings><mcpttgi:encoding name="AMR-WB"/></mcpttgi:preferred-voice-encodings>
    <mcpttgi:on-network-require-affiliation>true</mcpttgi:on-network-require-affiliation>
    <mcpttgi:on-network-hang-timer>PT30S</mcpttgi:on-network-hang-timer>
    <mcpttgi:on-network-maximum-duration>PT3600S</mcpttgi:on-network-maximum-duration>
    <mcpttgi:on-network-minimum-number-to-start>0</mcpttgi:on-network-minimum-number-to-start>
    <mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>PT5S</mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>
    <mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>abandon</mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>
    <mcpttgi:protect-media>false</mcpttgi:protect-media>
    <mcpttgi:protect-floor-control-signalling>false</mcpttgi:protect-floor-control-signalling>
    <mcpttgi:on-network-group-priority>5</mcpttgi:on-network-group-priority>
    <mcpttgi:on-network-encryption>false</mcpttgi:on-network-encryption>
    <cp:ruleset><cp:rule id="a7c"><cp:actions>
      <mcpttgi:on-network-allow-getting-member-list>true</mcpttgi:on-network-allow-getting-member-list>
      <mcpttgi:allow-MCPTT-emergency-call>false</mcpttgi:allow-MCPTT-emergency-call>
      <mcpttgi:allow-MCPTT-emergency-alert>true</mcpttgi:allow-MCPTT-emergency-alert>
      <mcpttgi:on-network-allow-conference-state>true</mcpttgi:on-network-allow-conference-state>
    </cp:actions></cp:rule></cp:ruleset>
  </list-service>
</group>
```

GET 응답은 여기에 `<mcpttgi:authorized-user>tel:+82510001001</mcpttgi:authorized-user>`(소유자)·`<cims:user-title>`
(직함, CIMS 확장)·MCData 크기 요소·`<oxe:supported-services>` 가 더 실린다. 클라이언트가 변경 구독 시
SIP `SUBSCRIBE Event: xcap-diff` 이용.

---

## 3. CMS (Configuration Management Server)

MCPTT 설정 문서 (TS 24.484). ue-init-config 만 **익명 GET**(로그인 전 부트스트랩 — XUI 는 UE
인스턴스 ID, 무검증), 나머지는 Bearer 토큰 + 본인 문서만(403). 전부 ETag/If-None-Match 지원.

| Method | Path | 인증 |
|---|---|---|
| GET  | `/org.3gpp.mcptt.ue-init-config/users/{instance}/{doc}` | 없음 (익명) |
| GET  | `/org.3gpp.mcptt.user-profile/users/{user}/user-profile` | Bearer + 본인 + scope `ptt_config_management_service`. TS 24.484 §8.3.2 문서 — `<OnNetwork><MCPTTGroupInfo>` = 소속 그룹 목록(규격 단말의 그룹 소스, 없어도 빈 요소), `<PrivateCallList>` = 동료 연락처, 긴급 대상·`cp:ruleset` 인가, `<MaxSimultaneousCallsN6>` = 관제(역할 배정) 10 / 그 밖 5(콘솔 MCPTT 정책). 모든 `<entry>` 에 `index`. ETag 내용 파생 |
| GET  | `/org.3gpp.mcptt.service-config/users/{user}/service-config` | Bearer + 본인. 전역 문서 — `<signalling-protection>` false/false(없으면 단말이 mcptt-info 를 암호화한다, TS 24.484 §8.4.2.6) · floor 타이머 · Resource-Priority |
| GET  | `/org.3gpp.mcvideo.user-profile/users/{user}/mcvideo-user-profile-<n>.xml` | Bearer + 본인 + scope `video_config_management_service`. TS 24.484 §9.3 MCVideo user profile — MCVideo 이용 자격(`mcvideo_user_profile` 행)이 없으면 404. `<MCVideoGroupInfo>` = 멤버인 MCVideo 그룹, `<MaxSimultaneousVideoStreams>` = 수신 상한([mcvideo.md](../design/features/mcvideo.md) §5.1) |
| GET  | `/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml` | Bearer + scope `video_config_management_service`. **전역 문서**(TS 24.484 §9.4.2.9) — `<signalling-protection>` false · Resource-Priority · `<tc-timers-counters-R14>`(CSC 설정 `McVideoServiceConfig.*`) |

user-profile 의 인가 `<cp:ruleset><cp:rule id="mcptt-user-authorisation"><cp:actions>` 값은 `ptt_user_profile`(admin API
`…/users/{pid}/ptt/{msisdn}/profile`, [admin_api.md §6.8](admin_api.md))이다 — 규격 요소를 TS 24.484 §8.3.2.1 11) 목록 순으로 싣고
뒤에 `<anyExt>`(11)xxxviii), 자식은 그 목록 순):

```xml
<allow-emergency-group-call>true</allow-emergency-group-call>          <!-- allow_emergency_call ∧ 긴급 대상 결정 가능 -->
<allow-emergency-private-call>true</allow-emergency-private-call>      <!-- allow_emergency_private_call ∧ 수신자 결정 가능 -->
<allow-cancel-group-emergency>false</allow-cancel-group-emergency>      <!-- allow_cancel_group_emergency (서버 판정 = 개시자 ∨ 이 값) -->
<allow-cancel-imminent-peril>true</allow-cancel-imminent-peril>        <!-- allow_cancel_imminent_peril -->
<allow-activate-emergency-alert>true</allow-activate-emergency-alert>  <!-- allow_emergency_alert ∧ 긴급 대상 결정 가능 -->
<allow-cancel-emergency-alert>true</allow-cancel-emergency-alert>      <!-- allow_cancel_emergency_alert -->
<anyExt>
  <allow-to-receive-non-acknowledged-users-information>false</allow-to-receive-non-acknowledged-users-information>  <!-- L) allow_non_ack_users_info -->
  <allow-adhoc-group-call>true</allow-adhoc-group-call>                                                              <!-- R) allow_adhoc_call -->
</anyExt>
<cims:allow-adhoc-group-call>true</cims:allow-adhoc-group-call>   <!-- 전환기 별칭 -->
<cims:allow-create-group>false</cims:allow-create-group>          <!-- CIMS 확장 — GMS 그룹 생성 자격 -->
<cims:allow-ambient-listening>false</cims:allow-ambient-listening>  <!-- CIMS 확장 — PTT 그룹 호 청취 자격 allow_ambient_listening -->
```

`<cims:allow-ambient-listening>` 은 비멤버 관제사의 PTT 그룹 호 recvonly 합류 자격(역할 배정의 결과, [dispatch_center.md](../design/features/dispatch_center.md) §5.6)이다 —
규격 ambient listening(원격·로컬 개시 1:1 호 — anyExt `<allow-request-remote-/locally-initiated-ambient-listening>`, TS 24.484 §8.3.2.1 11)xxxviii)C)·D))과
다른 것이라 CIMS 이름공간에 싣는다.

`allow-to-receive-non-acknowledged-users-information`(표 8.3.2.7-49, 부재 = false) 가 true 면 이 사용자가 개시한 그룹 호에서 확인 통화
설정이 필수 멤버 없이 진행될 때 controlling MCPTT function 이 응답하지 않은 멤버 목록을 SIP INFO 로 보낸다(TS 24.379 §6.3.3.3).
해제 인가 셋(`allow-cancel-group-emergency`·`allow-cancel-imminent-peril`·`allow-cancel-emergency-alert`)은 개시 인가와 달리 긴급 대상 결정
가능 여부와 AND 하지 않는다 — 이미 선 긴급 상태·경보를 푸는 자격이다(TS 24.379 §6.3.3.1.13.3·.4·.6).

service-config 의 `<on-network>` 는 선택 요소 `<emergency-call><group-time-limit>` 를 첫 자식으로 싣는다 — 진행 중 긴급 그룹 호 시한으로,
MCPTT 서버(CSP)가 TNG2 로 쓴다(TS 24.379 §6.3.3.1.16). 값 = CSC 설정 `ServiceConfig.EmergencyCall.GroupTimeLimit`(ms), 0(기본)이면 요소째 뺀다.
개별 호·애드혹 그룹 호의 세션 타이머(TS 24.484 §8.4.2.1·§8.4.2.3 — 그룹 문서가 없는 호라 이 문서가 출처다):

```xml
<on-network>
  <emergency-call>…</emergency-call>                                    <!-- 선택 -->
  <private-call>                                                         <!-- ServiceConfig.PrivateCall.* -->
    <hang-time>PT30S</hang-time>                                         <!-- 개별 호 T4 (TS 24.380 표 11.1.3-1) -->
    <max-duration-with-floor-control>PT3600S</max-duration-with-floor-control>        <!-- TS 24.379 §6.3.8.2 2) -->
    <max-duration-without-floor-control>PT3600S</max-duration-without-floor-control>  <!-- full-duplex 개별 호 -->
  </private-call>
  <transmit-time>…</transmit-time> <fc-timers-counters>…</fc-timers-counters> <!-- RP 셋 -->
  <anyExt>
    <adhoc-group-call>                                                   <!-- ServiceConfig.AdhocGroupCall.* -->
      <allow-adhoc-group-call-support>true</allow-adhoc-group-call-support>  <!-- 필수 — 없으면 «애드혹 미지원» -->
      <max-no-participants>64</max-no-participants>                      <!-- 필수 -->
      <hang-time>PT30S</hang-time>                                       <!-- 애드혹 T4 -->
      <broadcast-hang-time>PT30S</broadcast-hang-time>                   <!-- 일제 애드혹 T4 -->
      <max-duration-of-call>PT3600S</max-duration-of-call>               <!-- 애드혹 TNG3 (§17.4.2.2 13)) -->
    </adhoc-group-call>
  </anyExt>
</on-network>
```

시간 값 0 인 요소는 싣지 않는다(그 타이머 미가동), `<private-call>` 은 자식이 없으면 요소째 뺀다.

ue-init-config 의 주소류(IdMS/CMS/GMS/KMS/XCAP 루트)의 base 는 CSC 설정 `McpttServer.PublicUrl`
이 정본이다(비면 요청 Host 유도 — 올인원 전용). CSP 가 xcap-diff NOTIFY 로 광고하는 `xcap-root`
도 같은 값이며, CSP 는 이를 내부 API 로 취득한다:

| Method | Path | 인증 | 응답 |
|---|---|---|---|
| GET | `/internal/mcptt/endpoint` (admin 4421) | `Bearer {InternalApi.Token}` | `{"xcap_root","mcptt_port","public_url_configured"}` |
| GET | `/internal/mcvideo/service-config` (admin 4421) | `Bearer {InternalApi.Token}` · `If-None-Match` | 단말이 받는 MCVideo service-config 문서와 같은 XML — MCVideo 서버(CSP)가 전송 제어 타이머를 CMP 로 전달한다(변경 통지 = `SERVICE_CONFIG_CHANGED` uri `mcvideo`) |

`/api/v1` 밖이라 OAM 게이트웨이가 프록시하지 않는다(CSP 직접 호출 전용, `/internal/aka/av` 와 동일).

나머지 주소류(domain·PLMN·GMS-URI)는 토폴로지에서 유도되고,
규격 파라미터값(Timers·con-ref·http-proxy·보호 플래그·group-creation-XUI·name)과 확장 요소
(`MCPTT/MCVideo/MCData-Service-Details` — MCVideo 는 기본 끔) 는 csc 설정 `UeInitConfig.*` 로 사용자지정한다 — 값이 바뀌면 ETag 도
바뀐다([mcptt_standard_conformance.md §R4-1](../design/features/mcptt_standard_conformance.md)). 단말 타이머 `<Timers>` 기본값 =
T100 1 · T101 1 · T103 4 · T104 4 · T132 2 초(TS 24.380 표 11.1.1-1).

**변경 통지(xcap-diff)** — cms 축(`sip:cms_psi@<domain>`)을 구독한 단말은 문서가 바뀌면 xcap-diff NOTIFY(RFC 5875)를 받는다.
user-profile·service-config 는 `<document sel="org.3gpp.mcptt.user-profile/users/tel:<id>/user-profile">`·
`<document sel="org.3gpp.mcptt.service-config/users/tel:<id>/service-config">`, UE initial configuration 은 그 단말의 문서 선택자
`<document new-etag="…" sel="org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID>"/>` 하나(TS 24.484 §7.2.1.1·§7.2.2.12 —
MCS UE ID = 단말 instance ID, 등록 Contact `+sip.instance`). 단말은 그 선택자의 문서를 If-None-Match 로 다시 받는다.

---

## 4. KMS (Key Management Server)

미디어 암호화 키 배포(TS 33.180 부속서 D). 현재 응답 키 material 은 구조만 맞춘 placeholder 다 —
실구현·남은 항목은 [mcx_e2e_security.md](../design/features/mcx_e2e_security.md).

| Method | Path | 응답 |
|---|---|---|
| POST | `/keymanagement/identity/v1/init` | `KmsInit`(KmsCertificate) |
| POST | `/keymanagement/identity/v1/keyprov` | `KmsKeyProv`(KmsKeySet) |

핸들러는 메서드를 가리지 않는다(단말은 POST). Bearer access token + scope `3gpp:mc:ptt_key_management_service` 또는 `3gpp:mc:data_key_management_service`.
KmsCert(인증서 캐시) 엔드포인트는 없다.

---

## 5. 관련 파일

- 소스: `csc/src/handlers/idms.py`, `mcptt.py` 계열
- 저장: `csc_idms` DB (auth_code, refresh_token)
- SIP 단말 인증은 `api/admin_api.md` 의 CSCF 참조

상세 3GPP 스펙 추종 확인은 공식 스펙 문서 참조. 이 문서는 CIMS 구현된 엔드포인트 일람만 제공합니다.
