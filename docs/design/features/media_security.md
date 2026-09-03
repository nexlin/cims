# 미디어 보안 (SRTP) — 구간 암호화 설계

> UE↔CMP 미디어(RTP/RTCP) 구간 암호화 — SDES(RFC 4568) 키 교환 기반 SRTP(RFC 3711).
> 서버(CSP·CMP — PTT 그룹/사설 + VoLTE relay)·cspsim·단말(pjsip SRTP 빌드 + 앱
> mediaSecurity 정책, §7)·콘솔 스키마(`media_srtp` — 접속서비스 편집)·CSC 프로비저닝
> (§7.2)은 본 설계대로 구현되어 있다. **잔여**: S3 실측(§9 — 정지 창)·실기기/와이어
> 캡처 검증, 협력업체 SDK 재빌드(§7.3). E2E(KMS/MIKEY-SAKKE, TS 33.180)는 §10 로드맵.

## 1. 목표와 범위

**목표**: UE 와 CMP 사이의 미디어 평면(오디오/비디오 RTP, relay RTCP)을 암호화한다.
시그널링은 이미 TLS([sip_tls_signaling.md](sip_tls_signaling.md))·IMS AKA+IPsec
([sip_access_security.md](sip_access_security.md))로 보호되므로, 미디어가 마지막 평문 구간이다.

**모델 = 구간(hop-by-hop) 암호화, e2ae**. CMP 가 양쪽 leg 의 SRTP 를 종단한다.
3GPP TS 33.328(IMS media plane security)의 **e2ae(end-to-access-edge) SDES** 모드에 상응한다
— UE↔미디어 엣지 구간을 SDES 로 보호하고 코어 내부는 신뢰 도메인으로 본다.

CMP 종단(내부 평문)이 필수인 이유 — CMP 의 세 기능이 평문 RTP 를 전제한다:
- **녹취** — 트랙별 원본 보관·믹스/단독 재생 ([recording.md](recording.md))
- **그룹 분배 시 수신자별 SSRC/seq 재작성** — 화자 슬롯별 스트림 합성 ([cmp.md §3.5](../modules/cmp.md))
- **U10 동시 발언 디먹스** — 단말 SSRC 디먹스 전제 ([mcptt_ue_multitalker_media.md](mcptt_ue_multitalker_media.md))

**범위 밖** (§10): E2E 미디어 암호화(KMS·MIKEY-SAKKE — CMP 녹취/믹스와 양립 불가, 고객사 명시
요구 시 별도 트랙), MCData MSRP 평면(cmdp — MSRPS/TLS 별도 과제), floor control SRTCP 키 배포
(§7.4 — 현행 F6 구조 유지, KMS 대기).

## 2. 규격 근거

| 규격 | 사용 범위 |
|---|---|
| RFC 3711 (SRTP) | 패킷 보호 포맷·KDF·AES-CM·HMAC-SHA1 인증 |
| RFC 4568 (SDES) | SDP `a=crypto` 키 교환, offer/answer 절차 |
| 3GPP TS 33.328 | IMS 미디어 보안 e2ae 모델 — 등록 시 능력 협상, SDES 는 기밀 시그널링 채널 전제 (§4) |
| 3GPP TS 24.229 / RFC 3329 | 등록 시 미디어 보안 능력 선언 — `Security-Client: sdes-srtp` + `mediasec` 헤더 파라미터 (§4.1) |
| 3GPP TS 33.180 | MCPTT 보안 — floor SRTCP(F6, 구현됨)·E2E 키관리(로드맵) |

**crypto suite**: `AES_CM_128_HMAC_SHA1_80` 필수(기본 제안), `AES_CM_128_HMAC_SHA1_32` 수용만.
floor SRTCP(`PFloorCrypto`)와 동일한 2종 — 엔진·검증 벡터를 공유한다.

## 3. 아키텍처 — 키 흐름

```
[등록]  UE ──Security-Client: sdes-srtp (mediasec)──→ CSP    바인딩별 SRTP 능력 학습 (§4.1)

[호]    UE ──(A) SDP a=crypto, TLS 시그널링──→ CSP ──(B) UDP JSON media_crypto──→ CMP
                                                │
   UE 상향: UE 가 SDP 에 실은 키(UE-TX)  ─→  CMP 그 leg 의 RX 키
   UE 하향: CSP 가 leg 마다 생성한 키    ─→  서버 SDP a=crypto 로 UE 에 광고
                                             + CMP 그 leg 의 TX 키

CMP 내부:  ingress unprotect → 평문 (믹스·디먹스·녹취·DTMF 감지) → egress protect
```

- **(A) UE↔CSP**: SDES 그대로 — 각 측이 **자기 송신 키**를 `a=crypto` 로 선언한다.
  SDES 키는 SDP 본문에 평문으로 실리므로 **기밀 시그널링 채널이 전제**다 (§4 정책 결합).
- **(B) CSP→CMP**: 기존 UDP JSON 제어([cmp_media_api.md](../../api/cmp_media_api.md))의
  leg 생성/수정 명령에 `media_crypto` 필드를 추가한다. `floor_crypto` 와 동일한 인라인 키
  전달 관례 (base64, [cmp.md §3.2](../modules/cmp.md)).
- **키 생성 주체 = CSP**. leg 를 만드는 주체가 그 leg 의 서버측 키(30B random = key 16B +
  salt 14B)를 생성한다. 재협상(re-INVITE)마다 새로 생성한다.

## 4. 정책 모델 — 접속서비스 단위

`access_services.jsonl` (`ServiceInfo`, `csp/CspServiceMap.h`) 에 필드 1개를 추가한다.

| 필드 | 값 | 의미 |
|---|---|---|
| `media_srtp` | `off` (기본) | 현행 — 평문 RTP. `a=crypto` 무시 |
| | `optional` | 능력 기반 혼용(전환기) — CSP 발신 offer 는 **mediasec 능력을 선언한 바인딩(§4.1)에 `RTP/SAVP`**, 미선언 바인딩에 평문 AVP. 수신 offer 는 내용대로(SAVP=SRTP, AVP=평문) |
| | `required` | CSP 발신 offer = `RTP/SAVP` 단일(능력 미선언 단말 포함). crypto 없는 수신 offer 는 **488** |

- **TLS 결합**: `media_srtp=required` 는 그 서비스 가입자의 채널 정책이 TLS(또는 ipsec-3gpp)
  강제일 때만 유효하다 — SDES 키가 SDP 에 실리기 때문(TS 33.328). 로드 시 채널 게이트
  (`sip_transport`, [sip_access_security.md §3](sip_access_security.md)) 미강제 서비스에
  required 가 걸리면 ERROR 로그(정책은 유지 — 집행은 채널 게이트 몫).
- **수용 관대화(규격 밖, 의도된 예외)**: pjmedia 의 optional 모드처럼 `RTP/AVP` 에
  `a=crypto` 를 병기하는 best-effort offer 도 SRTP 로 수락한다(실무 interop 관례 —
  RFC 4568 은 SAVP 를 요구). CSP 가 내는 offer 는 어느 모드에서도 이 형태를 쓰지 않는다.
- 그룹콜은 **leg 단위 판단** — 같은 그룹에 SRTP 멤버와 평문 멤버가 공존할 수 있다
  (optional 전환기). CMP 는 leg 마다 독립 컨텍스트이므로 혼용에 추가 비용이 없다.

### 4.1 능력 학습 — 등록 시 mediasec (TS 33.328 / TS 24.229)

CSP 발신 offer 의 형태를 per-call 폴백 없이 결정하기 위해, **단말의 SRTP 지원 여부를
등록 시점에 학습**한다 — TS 33.328 e2ae 의 정식 절차다.

- UE 는 REGISTER 의 `Security-Client` 헤더에 `sdes-srtp` 메커니즘 + `mediasec` 헤더
  파라미터를 실어 미디어 보안 능력을 선언하고, CSP 는 `Security-Server` 로 응답한다.
  기존 sec-agree(RFC 3329, [sip_access_security.md §5](sip_access_security.md) — 구현 완료)
  의 확장이며, 채널 보안 메커니즘(tls/ipsec-3gpp) 협상과 같은 헤더를 공유하되 `mediasec`
  파라미터로 구분된다(TS 24.229).
- 학습 결과는 **등록 바인딩 집합**([registration_binding_set.md](registration_binding_set.md))
  의 바인딩 항목에 능력 플래그로 저장한다 — offer 형태 판단은 대상 바인딩 조회로 끝난다.
- 단말이 SDP 에 싣는 `a=3ge2ae:requested`(TS 33.328)는 무해 수용한다 — CMP 종단 구조상
  모든 SRTP 가 e2ae 이므로 별도 분기가 없다.
- psip/pjsip/cspsim 모두 sec-agree 구현이 이미 있으므로 mediasec 토큰 추가는 그 연장이다
  (§7.2, §8).

## 5. SDP 협상 (CSP + psip)

### 5.1 psip 확장

- **`CSipCallRtp`** (`ext/psip/SipUserAgent/SipUserAgentCallBack.h`) — 응용↔스택 미디어 계약에
  SRTP 필드를 추가한다: local crypto(스택이 SDP 로 방출)·remote crypto(스택이 수신 SDP 에서
  파싱) 각 `{suite, key||salt(base64), mki, tag}`.
- **SDP 방출** (`CSipDialog::AddSdp`, `ext/psip/SipUserAgent/SipDialog.cpp`): local crypto 가
  설정된 경우 `m=audio ... RTP/SAVP ...` + `a=crypto:<tag> <suite> inline:<key||salt>` 를
  코덱 테이블 경로와 media-list 경로 양쪽에서 방출한다. answer 는 offer 의 tag/suite 를 echo
  (RFC 4568 §5.1.2).
- **SDP 파싱**: 수신 SDP 의 `a=crypto` 를 기존 `CSdpAttributeCrypto`
  (`ext/psip/SdpParser/SdpAttributeCrypto.h` — 파서 기구현)로 해석해 `CSipCallRtp` 에 올린다.
  protocol 이 `RTP/SAVP` 인데 유효한 crypto 가 없으면 협상 실패로 응용에 알린다(응용이 488).

### 5.2 CSP 협상 규칙

- offer/answer 형태 = **접속서비스 정책(`GetForUser`) × 대상 바인딩의 mediasec 능력(§4.1)**
  (§4 표). per-call 폴백(488 후 재-offer)은 두지 않는다 — 능력을 등록에서 이미 안다.
- **suite 선택**: 수신 offer 의 crypto 목록에서 지원 suite(§2) 중 첫 항목을 채택. 지원 suite
  가 없으면 정책에 따라 488(required) 또는 평문 폴백(optional — SAVP offer 면 488).
- **재협상(re-INVITE)**: 새 a=crypto 수신 시 서버측 키도 재생성하고 CMP 에 키 갱신을 내린다
  (RELAY_MODIFY / PTT_MODIFY 확장). 직전과 동일 선언(refresh/재전송)이면 키 유지 —
  latch 유지 규칙([ue_nat_traversal.md §5](ue_nat_traversal.md))과 동형.
- **VoLTE relay(media-list passthrough) leg**: SDP 를 투과하면 양 단말이 E2E SRTP 를 협상해
  버려 CMP 녹취가 깨진다. relay 는 **crypto 라인을 leg 별로 재작성**한다(구간 종단 유지) —
  모든 전달 지점(INVITE fan-out·18x early media·answer·re-INVITE·PRACK)에서 수신 crypto 를
  벗기고(`MediaSdes::StripCrypto`) 그 leg 의 협상 상태(CallMap `RelaySdesLeg` — leg×미디어별
  UE 키/서버 키)로 다시 싣는다. 판단 규칙은 PTT 와 동일(§4 표): A(발신) leg 는 offer 내용
  ×정책, B(착신) leg 는 정책×착신 바인딩 mediasec 능력으로 offer 형태(SAVP/AVP)를 결정하고,
  SAVP offer 에 crypto 없는/불일치 answer 는 호 종료(평문 폴백 금지). 키는 `RELAY_ADD`
  (peer0)·`RELAY_MODIFY`(peer1/재협상) 의 `media_crypto[_video]` 로 CMP 에 내린다. 서버가
  전달한 re-INVITE 의 재-answer 재키잉은 `EventReInviteResponse` 가 감지해 키 변경 시에만
  MODIFY 를 재발행한다(불변 = 무동작 — CMP 세션 유지). 호 전환(REFER — attended/blind)·
  당겨받기도 SRTP 를 유지한다 — 원 통화의 relay 세션을 유지한 채 교체되는 leg 만
  `RELAY_MODIFY` 로 재고정하며, attended 는 합류 단말의 기존 leg 키를 이관(재키잉 없음),
  blind/pickup 신규 leg 는 정책×능력으로 새 서버 키를 생성한다(떠나는 단말에 알려진 키
  재사용 금지). SAVP offer 에 crypto 없는 answer 는 전환만 중단하고 원 통화를 유지한다 —
  [volte_supplementary_services.md §6](volte_supplementary_services.md).

## 6. CMP 설계

### 6.1 암호 엔진 — libsrtp2

**엔진 = libsrtp 2.5.x** (BSD-3). ROC 추정·재전송 윈도우·SSRC 별 스트림 수명 관리를
검증된 구현에 위임한다. floor SRTCP 의 `PFloorCrypto`(자체 구현)는 **현행 유지** — C++
클래스 vs C `srtp_` 심볼이라 공존에 문제 없다.

- **조달**: upstream 소스를 **`ext/libsrtp/` 로 독립 vendoring** 하고 opencore-amr 과 동일한
  로컬 소스 패턴(`DOWNLOAD_COMMAND ""`)으로 빌드한다 — 레포 내 소스(air-gapped 자족),
  OpenSSL 백엔드 활성(EVP — AES-NI). `ext/pjproject/third_party/srtp/`(2.5.0 동봉분)는
  **링크 금지** — 그 트리는 pre-commit 훅이 pjproject fork 에서 동기화하는 영역이라 단말
  빌드 동기화가 서버 빌드를 흔든다.
- **컨텍스트 모델**: leg 당 얇은 래퍼 `PMediaCrypto` 가 `srtp_t` 2개(inbound/outbound,
  각각 `ssrc_any_inbound`/`ssrc_any_outbound` 템플릿)를 소유한다. 하향 슬롯 SSRC
  (0x10000000+, 0x40000000+…)는 템플릿이 SSRC 별 스트림을 자동 생성한다.
- **스레드 규약 (중요)**: `srtp_t` 는 thread-safe 가 아니다. CMP 의 현 구조가 직렬화를
  보장한다 — relay 는 핸들러의 전 fd 가 같은 리액터에 배정되고(`epollAddHandler` 핸들러
  단위 widx), 그룹 경로는 분배까지 그룹 `_mutex` 아래다. **leg 컨텍스트는 이 직렬화 범위
  안에서만 접근한다** — 향후 락 분리/리액터 재배정 시 이 전제를 깨면 안 된다(코드 주석
  명문화).
- **재협상 키 교체 = 세션 재생성**(dealloc→alloc, ROC·재전송 창 리셋). `srtp_update()` 는
  ROC 를 유지하는 반면 pjmedia 는 키 변경 시 세션을 재생성(ROC 0)하므로, update 를 쓰면
  재협상 직후 전 패킷 인증 실패한다. **금지.**
- MKI 미사용(offer 에 싣지 않음). `srtp_init()` 은 프로세스 1회. protect 는 인증 태그
  (10B, `_32` suite 는 4B)를 부가하므로 송신 버퍼에 headroom 을 확보해 호출한다
  (relay 2048B·그룹 4096B 버퍼로 충분).

### 6.2 leg 컨텍스트와 데이터 경로

- **`PRtpRelay`** (VoIP/사설콜): `Leg` 구조체에 `PMediaCrypto` 2세트(오디오/비디오) 부착.
  proc() 의 소스 검증 통과 직후 unprotect → (녹취·relay) → 반대 leg protect 후 송신.
  **RTCP 도 동일 키로 SRTCP** 보호(RFC 3711 — relay 경로는 RTCP 를 중계하므로 필수).
- **`PPttMemberPort` / `PMcpttGroup`** (그룹): `Peer` 에 `PMediaCrypto` 부착. 상향
  `onMemberRtpPacket` unprotect → 평문 분배 로직(현행 무변경) → 하향 `sendAudioToAll` 의
  수신자별 SSRC/seq 재작성 **후** 그 수신자 leg 키로 protect. 그룹 경로는 미디어 RTCP 를
  쓰지 않으므로(floor 가 별도) SRTCP 대상 아님.
- **SSRC 와 ROC**: RFC 3711 의 세션 키는 SSRC 무관(키스트림만 SSRC·인덱스 의존)이므로 leg
  키 하나로 하향 다중 SSRC(화자 슬롯 0x10000000+ssrc / 0x40000000+…)를 보호할 수 있다.
  SSRC 별 스트림·ROC·재전송 창은 libsrtp 템플릿 세션이 내부 관리한다(§6.1). 하향은
  수신자별 SSRC/seq 재작성(현행) **후** protect 하므로 스트림마다 seq 가 연속이다.
- **재전송/위조**: 상향 unprotect 실패(인증 태그 불일치·재전송 창 밖)는 드롭 + 신규 카운터
  `srtp_drop` (STATS detail — `floor_crypto_drop` 과 병렬, [cmp_media_api.md §6](../../api/cmp_media_api.md)).
- **latch 상호작용**: NAT 목적지 latch 는 주소 학습일 뿐 신원 판정이 아니다(수신 소켓=신원,
  [cmp.md §3.3](../modules/cmp.md)) — SRTP 인증 태그가 추가 방어층이 된다. latch 전 첫 유효
  RTP 판정에 "unprotect 성공"을 포함한다(제3자 주입으로 latch 오염 불가).

### 6.3 제어 API 확장 (UDP JSON)

`RELAY_ADD`/`RELAY_MODIFY`(leg 별 — `peer_index` 필수)·`PTT_JOIN`(멤버별, 주소 동반 ②
호출) payload 의 선택 필드 (정본 = [cmp_media_api.md §6.4](../../api/cmp_media_api.md)).
`PTT_GROUP_ADD` 로스터는 주소처럼 키도 싣지 않는다 — 키는 SDP 교환 후 JOIN ② 로 온다.

```json
"media_crypto": {
  "alg":  "AES_CM_128_HMAC_SHA1_80",
  "rx":   { "key": "b64(16B)", "salt": "b64(14B)" },   // UE→CMP 상향 (UE 의 a=crypto)
  "tx":   { "key": "b64(16B)", "salt": "b64(14B)" }    // CMP→UE 하향 (CSP 생성)
}
```

- audio 는 `media_crypto`(RTP + relay 경로 RTCP), video 는 `media_crypto_video` — SDES 는
  m= 라인마다 키가 다르다.
- 생략 시 그 leg 는 평문(신규), 기존 SRTP leg 의 재요청 생략은 **기존 키 유지** —
  optional 혼용 그룹의 표현이 자연스럽다.
- 재협상 키 교체 = 같은 명령의 재전송(`RELAY_MODIFY` / JOIN ② 재호출). 동일 구성 재선언은
  세션 유지, 키 변경은 세션 재생성(ROC·재전송 창 리셋 — §6.1).
- 키 오류(길이·base64·suite)는 `floor_crypto` 와 동일하게 **명령 거부**(fail-fast — 조용한
  평문 폴백 금지).

### 6.4 녹취 탭 이동

현행 녹취는 수신 원본 패킷을 저장한다 — SRTP 도입 시 **unprotect 이후 평문 RTP 를 저장**으로
탭 지점을 옮긴다(`PSyncRtpRecorder` 인터페이스 무변경, 호출 위치만 이동). 재생·믹스
파이프라인([recording.md](recording.md))은 무변경. 보관 파일이 평문인 것은 현행과 동일
(저장소 보안은 NAS/디스크 계층 과제 — 범위 밖).

## 7. 단말 (pjsip / Android)

### 7.1 pjsip 플래그

엔진 config_site 정본 `sdk/engine/config_site/common.h`([ue_sdk.md](ue_sdk.md) §3) 는 `PJMEDIA_HAS_SRTP 1` (TLS 축과 같은 OpenSSL
android-arm64 정적 링크 — 추가 의존 없음). pjmedia 의 SDES 협상(`a=crypto` 생성/파싱,
RTP/SAVP)은 내장 — 앱은 use-srtp 수준만 지정한다. 빌드만으로는 무영향 — 런타임은 앱
계정 정책(§7.2)이 켤 때까지 off.

`PJSUA2_MAX_SDP_BUF_LEN 4000` 도 필수 — pjsua2 `SdpSession.wholeSdp` 인쇄 버퍼(기본
1024B)는 SRTP 오퍼(전 crypto suite `a=crypto`, RTP m= 라인마다)가 넘친다. 넘치면
`wholeSdp=""` 로 내려와 앱 floor 주입([CimsCall.onCallSdpCreated])이 조각 SDP 를 만들고
`pjmedia_sdp_validate` assert 로 네이티브 abort 한다. 앱도 방어한다 — `wholeSdp` 가 비면
주입을 포기하고 pjsua 원본 오퍼(floor 없음)로 강등(크래시 대신 기능 축소).

### 7.2 앱 정책 연결

계정 미디어 설정 `srtpUse` 는 프로비저닝 프로파일이 정한다 — CSC `/provisioning/me` 의
서비스별 프로파일 `sip.mediaSecurity: off|optional|required`
([android_ue_provisioning.md §3](android_ue_provisioning.md), CSC 설정
`Provisioning.Services.<kind>.media_srtp` — 서버 접속서비스 정책(§4)과 같은 값을 내려
단말·서버가 한 SoT 를 본다. CSP `access_services.media_srtp` 와의 동기는 운영자 몫 —
CSC 는 CSP 를 조회하지 않는다).

앱 매핑(`SipController.buildAccountConfig`): **TLS 접속일 때만** required→`MANDATORY`,
optional→`OPTIONAL`(AVP+crypto best-effort offer), 그 외/비-TLS→`DISABLED`. 비-TLS 에서
끄는 이유 — SDES 키가 SDP 에 실리므로 기밀 채널 전제(TS 33.328)이고, pjsua 는
`srtpUse≠DISABLED` 인데 시그널링이 비보안이면 호 자체를 거부한다(`ESESSIONINSECURE`,
secure-signaling 게이트). 비-TLS 바인딩은 mediasec 능력도 선언하지 않으므로 서버도 그
leg 에 평문을 offer 한다 — 양쪽 판단이 일치한다.

능력 선언(§4.1): 정책이 켜진 TLS 계정은 REGISTER `Security-Client` 에
`tls, sdes-srtp;mediasec` 를 싣는다 — 기존 sec-agree 제안 헤더의 확장(Security-Verify
echo 는 pjsip sip_reg.c 패치가 처리, 신규 축 아님).

### 7.3 협력업체 SDK

pjsip 재빌드(SRTP 플래그 + mediasec 선언 패치)가 필요하므로 협력업체 APK/SDK 에 변경
공지(파트너 안내문 별도). 구 APK 는 mediasec 을 선언하지 않으므로 `optional` 에서
자동으로 평문 leg 가 된다 — 별도 호환 처리 없이 공존한다.

### 7.4 floor control 과의 관계

floor SRTCP(F6, [mcptt_standard_conformance.md](mcptt_standard_conformance.md) §F6)는 **별개
축** — 키 배포가 TS 33.180 CSK(KMS) 체계라 SDES 로 대체하지 않는다(`m=application UDP MCPTT`
는 RTP 프로파일이 아니어서 `a=crypto` 적용 대상도 아니다). CMP 구현은 완료, 키 배포는 KMS
연동 대기(현행 유지). 미디어 SRTP 가 켜져도 floor 는 평문일 수 있다 — 두 축의 활성화는 독립.

## 8. 시뮬레이터 (cspsim)

### 8.1 SDP

`SimSession.cpp` 의 `BuildAudioMedia`/`BuildVideoMedia` 가 SRTP 모드에 따라 m-line 을
`RTP/SAVP` + `a=crypto` 로 낸다. `a=crypto` 는 m-line 단위(RFC 4568 §5)이므로 오디오·비디오가
**각자 키를 선언**하고(`m_strSrtpLocalKey`/`m_strSrtpVideoLocalKey`), answer 도 m-line 별로 확정한다.
상대 crypto 는 오디오는 psip `CSipCallRtp` 필드, 비디오는 `m_clsMediaList` 에서 직접 읽는다
(`ReadMediaCrypto` — psip 가 audio 만 필드로 올리므로). 판정 규칙은 오디오와 동일:
SAVP 오퍼인데 수락 불가 → 488, required 오퍼에 crypto 없는 활성 video answer → 호 종료,
optional → 평문 비디오, answer 가 video 를 거절/생략 → 비디오 미송신.
비디오 송신 목적지는 상대 SDP 의 활성 `m=video` 포트(RFC 3264)로 학습하고, PTT 의
`X-Video-Port` 헤더는 SDP 에 video 가 없을 때의 폴백이다.

### 8.2 RTP 경로

`RtpThread.cpp` 가 m-line 별 libsrtp 세션(`SrtpSession` audio/video, `ext/libsrtp` 동일 타깃,
§6.1)을 들고 오디오 송신·수신, 비디오 송신(단일 NAL·FU-A 두 경로) 지점에서 protect/unprotect
한다. 키는 세션당 생성(발신)·SDP 파싱(수신). REGISTER 능력 선언(§4.1)은 psip sec-agree 확장을
공유한다.

### 8.3 인자

`-srtp off|optional|required` (기본 off). 검증 시나리오가 평문 대조군과 SRTP 군을 같은
바이너리로 구동할 수 있어야 한다.

## 9. 검증 계획

- **S1 단위** — `tests/cmp_media_crypto_test.cpp` (`PMediaCrypto` + ext/libsrtp 단독 링크):
  protect/unprotect 왕복(RTP/RTCP·양방향) + seq 랩어라운드(ROC) 관통 + 인증 실패/재전송
  드롭 + 하향 슬롯 SSRC 다중화 + 재협상 세션 재생성(update 아님) + 동일 구성 재선언 세션
  유지(재전송 창 보존으로 입증) + 키/salt/suite fail-fast. RFC 3711 §B.3 벡터 시험은
  floor(`PFloorCrypto`) 기존 유지. `media_crypto` JSON fail-fast 는 S3 게이트로 본다.
- **S3 `S3-SCN-SRTP`** (verify/lib/items/stage3/scn_srtp.py — 정책 플립+SIGUSR1, 자기복원):
  - R1 required 그룹콜 — cspsim SRTP 성립(`RTP/SAVP`+`a=crypto` 왕복) + CMP `media_crypto`
    수신 로그 + 신규 녹취(unprotect 후 평문 저장 — 탭 이동 검증)
  - R2 required 에서 평문 offer → 488 (CSP 협상 게이트 로그)
  - R3 optional — `RTP/AVP`+`a=crypto`(best-effort) 수용 관대화 경로
  - R4 off 대조군 — 정책·단말 off 원복 후 평문 그룹콜 그린(기존 동작 유지)
  - R5 VoLTE relay — volte 서비스 required 플립 + cspsim 2자 통화(`-srtp required`, 영상 동반):
    양 단말 오디오·비디오 SRTP 성립 + CMP relay leg crypto audio/video 각 2건(leg·m-line 별 독립
    키 — 투과가 아닌 종단, §5.2). 녹취에 비디오 트랙(`seg_*_va/vb.rtp`)이 생기면 보호된 영상
    RTP 가 양 leg 를 관통해 CMP 가 복호했다는 증거
- **S1 단위(relay SDP 조작)** — `tests/csp_media_sdes_relay_test.cpp`: `ReadOfferCrypto`
  (SAVP 유효/지원불가 suite/-1·AVP best-effort/평문·비활성 미디어)·`StripCrypto`·
  `ApplyCrypto`(키 재작성·여타 속성 보존·평문 정규화) 13케이스.
- **정지 창/실기기 잔여**: 와이어 캡처 실측(암호화 페이로드 실증), optional 혼용 그룹
  (SRTP 멤버+평문 멤버 상호 수신), 능력 기반 offer(mediasec 선언/미선언 바인딩에
  SAVP/AVP — §4.1), W999/MF52 SRTP 그룹콜 + 귀검증, 협력업체 단말은 SDK 재빌드 후.

## 10. 롤아웃과 로드맵

**배포 = 3축 동반**: csp·cmp(+cspsim)·APK. 정책 기본값 `off` 이므로 배포 자체는 무영향 —
서비스 단위로 `optional`(혼용 검증) → `required`(완전 전환) 단계 상향한다.

**로드맵 (범위 밖, 요구 발생 시)**
- **E2E 미디어 암호화** — TS 33.180 KMS(ECCSI/SAKKE)·MIKEY-SAKKE, GMK/PCK. CMP 녹취·믹스·U10
  디먹스와 양립 불가 → 녹취 정책 재설계가 선행 조건. CSC KMS 는 구조만 존재(S5 placeholder).
- **floor SRTCP 키 배포** — KMS CSK 연동(F6 의 잔여 반쪽).
- **MCData MSRP 보안** — cmdp MSRPS(TLS) 종단.
- **AEAD suite**(AES-GCM, RFC 7714) — 단말 fleet 이 지원 확정되면 suite 추가.
