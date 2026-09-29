# MC 종단간 보안 — KMS · MIKEY-SAKKE (TS 33.180)

> MCPTT·MCData 의 **종단간(E2E) 미디어·키 관리** 정본. 신원 기반 암호(ECCSI RFC 6507 / SAKKE
> RFC 6508)를 KMS 가 가입자별로 프로비저닝하고, 그 위에서 MIKEY-SAKKE(RFC 6509)로 그룹 키(GMK)·
> 개인 통화 키(PCK)·클라이언트-서버 키(CSK)를 나눈다. 이 문서는 **현재 구현(구조만 있는 KMS)**과
> **남은 개발 항목**을 정리한다 — 설계 정본, 미구현.
>
> 관계 문서: 구간(hop-by-hop) SRTP 는 [media_security.md](media_security.md)(e2ae SDES — VoLTE·비암호화
> 그룹), 토큰·scope 는 [mcx_identity_scope.md](mcx_identity_scope.md), floor SRTCP 엔진은
> [mcptt_standard_conformance.md](mcptt_standard_conformance.md) F6, KMS 정합 항목 S5.
> 제품 명칭: CSC = SPS, CMDP = MDP(발표·운영 명칭). 이 문서는 코드 명칭을 쓴다.
>
> ⚠ 규격 원문 대조 전 초안이다 — 조항 번호에 **(판본 확인)** 표시가 있는 곳은 착수(K0) 때
> TS 33.180 / TS 24.379 / TS 24.481 v18 판본으로 확정한다.

## 1. 규격 모델 요약

| 요소 | 내용 | 근거 |
|---|---|---|
| KMS | 신원 기반 키의 신뢰 뿌리. KMS 공개키(SAKKE `Z_T` = `PubEncKey`, ECCSI `KPAK` = `PubAuthKey`)를 `KmsCertificate` 로, 가입자별 비밀키(SAKKE RSK = `UserDecryptKey`, ECCSI SSK/PVT = `UserSigningKeySSK`/`UserPubTokenPVT`)를 `KmsKeySet` 으로 내린다. 키 기간(`UserKeyPeriod`·`UserKeyOffset`)마다 새 키 | TS 33.180 부속서 D(KmsInit·KmsKeyProv·KmsCert), RFC 6507/6508/6509 |
| 접근 | HTTPS + IdMS access token, scope `3gpp:mc:ptt_key_management_service` / `3gpp:mc:data_key_management_service` | TS 33.180 부속서 B.4.2.2·B.10 |
| UID | 키는 MC ID 가 아니라 **MC ID URI + 키 기간**에서 유도한 UID 에 묶인다 | TS 33.180 UID 생성 부속서(판본 확인) |
| GMK | 그룹 미디어 키. **GMS 가 생성**해 멤버별 MIKEY-SAKKE I_MESSAGE(수신자 UID 로 SAKKE 암호화 + GMS 신원으로 ECCSI 서명)로 그룹 관리 절차에 실어 보낸다. 미디어 SRTP 는 GMK 에서 유도한 키 + MKI(GUK-ID) | TS 33.180 그룹 키 배포 절(판본 확인), TS 24.481 |
| PCK | 개인 통화 키. **발신 단말이 생성**해 착신자 UID 로 I_MESSAGE 를 만들어 호 개시 시그널링에 싣는다 — 서버는 풀지 않고 전달만 | TS 33.180 개인 통화 절, TS 24.379(판본 확인) |
| CSK | 클라이언트-서버 키. **단말이 생성**해 MC 서버 신원으로 I_MESSAGE 를 만들어 서비스 인가 때 보낸다. floor control SRTCP·시그널링 XML 보호에 쓴다 | TS 33.180 §9.4(floor)·XML 보호 절, TS 24.379 §7.3 |
| SPK | 서버 간 시그널링 보호 키(타 MC 시스템 연동) | TS 33.180(판본 확인) |
| MKFC·MSCCK·MuSiK | MBMS(멀티캐스트) 전용 | 범위 밖 — CIMS 는 unicast 만 |
| DPPK | MCData 페이로드 보호 | TS 33.180 MCData 절 — §7 후속 |

**E2E 의 뜻**: 그룹·개인 미디어는 **단말끼리만** 풀 수 있다. 서버(CMP)는 암호문을 그대로 나른다.
서버가 읽어야 하는 floor control·시그널링은 E2E 가 아니라 **CSK(단말↔서버)** 로 보호한다.

## 2. 현재 구현

| 영역 | 상태 | 위치 |
|---|---|---|
| KMS 엔드포인트 | `/keymanagement/identity/v1/init`·`/keyprov` 두 개(메서드 무관, Bearer + scope `SCOPE_PTT_KMS`/`SCOPE_DATA_KMS`). **KmsCert(인증서 캐시) 없음** | `csc/src/services/mcptt.py` 라우트 표(`# KMS`), `handle_kms_init`·`handle_kms_keyprov` |
| KmsInit | `PubEncKey`·`PubAuthKey` **고정 hex**, `KmsId`·`Issuer` 임의값, xmldsig 네임스페이스만 있고 서명 없음, 네임스페이스 `http://org.csc.kms`(규격 네임스페이스 아님) | `get_kms_init_xml` |
| KmsKeyProv | RSK/SSK/PVT = HMAC-SHA256 파생 **placeholder**(참 ECCSI/SAKKE 점 아님) | `get_kms_keyprov_xml`·`_kms_derive` |
| 마스터 비밀 | `KMS_MASTER_SECRET = secrets.token_bytes(32)` — 프로세스마다 새로 만들고 저장 안 함 → 재기동하면 모든 가입자 키가 바뀐다 | `mcptt.py` 상단 |
| 설정 | `IdMs.KmsUri`(기본 `kms.<domain>`)·`IdMs.KmsClientReqUrl`(기본 포트 4421 — 실제 KMS 라우트는 MCPTT 서버 4430) | `csc/config/config_template.json` |
| ue-init-config | `<kms>` URL 광고, `integrity-/confidentiality-protection-enabled` = false(XML 보호 미구현) | `mcptt.py` ue-init-config |
| 그룹 `encryption` | DB `ptt_groups.encryption` → GMS `<on-network-encryption>`·관리 API·콘솔·SDK 가 싣지만 **동작을 켜는 곳이 없다**(CSP 는 세션 이력 JSON 에 옮겨 적기만) | `sql/cims_schema.sql`, `csp/DbManager.cpp`, `csp/GroupCallService.cpp` |
| 키 저장 테이블 | 없음(GMK·서버 신원 키·키 기간) | — |
| MIKEY | 파서·생성기 없음, `a=key-mgmt` 없음 | — |
| floor SRTCP | **CMP 엔진 완료** — `PFloorCrypto`(RFC 3711 SRTCP, MKI ≤16B, 재전송 창), 그룹 키 `PTT_GROUP_ADD/MODIFY.floor_crypto`·멤버 CSK `PTT_JOIN.floor_crypto`, 폐기 계수 `floor_crypto_drop`. **CSP 가 `floor_crypto` 를 보내지 않아 운용은 평문** | `cmp/PFloorCrypto.*`, `cmp/PMcpttGroup.*`, `tests/cmp_floor_crypto_test.cpp` |
| 미디어 SRTP | SDES e2ae — CMP 가 복호해 평문으로 분배·녹취 후 재암호화(E2E 아님), MKI 미사용 | [media_security.md](media_security.md), `cmp/PMediaCrypto` |
| 단말 | SRTP 는 SDES 만(pjproject `PJMEDIA_HAS_SRTP`), KMS scope 요청만 있고 KMS 클라이언트·MIKEY·GMK·MKI·floor SRTCP 없음 | `sdk/core/include/cimsue/csc.h`, `sdk/core/src/account_map.cpp` |
| MCData | IEI 0x7A(Security parameters and Payload) 는 크기만 셈, E2E 미적용(TLS + 서버 RBAC) | `csp/McDataCodec.cpp`, [mcdata_messaging.md](mcdata_messaging.md) |
| 암호 라이브러리 | OpenSSL(CMP·단말), libsrtp 2.5(서버), pjproject 동봉 libsrtp(단말). **pairing(SAKKE)·ECCSI 라이브러리 없음**. CSC 파이썬 vendor 에 `cryptography` 없음(AuC 는 자체 AES-128 `services/auc/aes128.py`) | `ext/`, `csc/vendor/` |
| 알람 | `A-PRC-015`(KMS 키 만료·획득 실패) 정의만 | [alarm_catalog.md](../alarm_catalog.md) |

## 3. 설계 원칙

1. **규격 순정** — KMS 메시지는 TS 33.180 부속서 D 스키마·네임스페이스 그대로, 키 교환은 MIKEY-SAKKE
   그대로. 자체 키 교환(SDES 로 GMK 전달 등)은 두지 않는다.
2. **암호 구현은 하나** — ECCSI·SAKKE·MIKEY-SAKKE·KDF 를 C++ 공용 라이브러리(가칭 `libmcsec`) 하나로
   두고 CSP·단말 SDK·cspsim/계측기 워커가 같이 쓴다. KMS(CSC, 파이썬)의 키 생성은 스칼라 곱뿐이라
   같은 시험 벡터를 통과하는 파이썬 구현으로 둔다(RFC 6507/6508 부속서 벡터를 양쪽 단위시험이 공유).
3. **서버는 E2E 미디어를 풀지 않는다** — 암호화 그룹의 미디어는 CMP 가 복호·재작성 없이 전달한다.
   서버가 읽어야 하는 것(floor·XML)은 CSK 로 보호한다.
4. **구간 SRTP 와 공존** — E2E 는 MC 서비스(PTT·MCData)의 `encryption` 그룹·개인 통화에만 걸린다.
   VoLTE·비암호화 그룹은 현행 e2ae SDES([media_security.md](media_security.md)) 그대로.

## 4. 흐름

```
① 키 프로비저닝 (로그인 뒤, 키 기간마다)
  UE ──HTTPS+token──▶ CSC KMS  KmsInit    → KmsCertificate(Z_T, KPAK, 키 기간)
  UE ──HTTPS+token──▶ CSC KMS  KmsKeyProv → KmsKeySet(RSK, SSK, PVT)  @ UID(MC ID, 기간)
  (서버 신원 — MCPTT 서버·GMS — 도 같은 KMS 에서 자기 키를 받는다)

② 그룹 키 (GMS)
  GMS: GMK·GMK-ID 생성 → 멤버마다 I_MESSAGE = SAKKE(GMK → 멤버 UID) + ECCSI 서명(GMS 신원)
  UE  ◀── 그룹 문서 조회(TS 24.481) ── I_MESSAGE → 서명 검증 → RSK 로 GMK 복원

③ CSK (서비스 인가 — TS 24.379 §7.3)
  UE: CSK 생성 → I_MESSAGE(→ MCPTT 서버 UID) → REGISTER/PUBLISH 로 CSP
  CSP: 서버 RSK 로 CSK 복원 → SRTCP 키 유도 → CMP PTT_JOIN.floor_crypto (멤버별)

④ 그룹 통화 미디어
  UE(화자) ── SRTP(GMK 유도 키, MKI=GUK-ID) ──▶ CMP ── 그대로 ──▶ 멤버들 (각자 GMK 로 복호)
  floor RTCP 는 UE↔CMP 구간마다 CSK SRTCP

⑤ 개인 통화
  발신 UE: PCK 생성 → I_MESSAGE(→ 착신 UID) → INVITE 에 실음 → CSP 는 손대지 않고 전달
  양 단말 SRTP(PCK 유도 키), CMP 는 그대로 전달
```

## 5. 개발 항목 (WP)

| WP | 대상 | 내용 |
|---|---|---|
| **K0 규격 확정·결정** | 문서 | TS 33.180·TS 24.379·TS 24.481 v18 대조로 이 문서의 (판본 확인) 조항·XML 요소(GMK 운반 요소, CSK·PCK I_MESSAGE 가 실리는 곳, SDP 프로파일), KMS 네임스페이스·요청 방식·KmsCert 경로를 확정. §6 결정 사항 확정 |
| **K1 암호 코어** | 신규 `libmcsec`(C++, OpenSSL BN/EC 위) | ECCSI 서명·검증(P-256), SAKKE 암·복호(파라미터 집합 1 — 1024-bit 초특이 곡선 Tate 페어링), MIKEY-SAKKE I_MESSAGE 부호화·해석(RFC 6509 — HDR·T·RAND·IDRi/IDRr·SAKKE·SIGN), TS 33.180 키 유도(GUK-ID, GMK/PCK/CSK → SRTP 마스터 키·솔트, MKI). 단위시험 = RFC 6507·6508 부속서 벡터(`S1-UNIT-MCSEC`) |
| **K2 KMS 실구현** | CSC `services/kms/`(신설) | ① 마스터 비밀(SAKKE `z_T`, ECCSI `KSAK`) 영속 + 암호화 보관 — AuC 키 보관 방식(`services/auc/keystore.py`, KEK) 재사용 ② 가입자 RSK·SSK/PVT 참 생성(키 기간·UID) ③ 부속서 D 스키마·네임스페이스로 KmsInit·KmsKeyProv·KmsCert, KMS 서명(xmldsig) ④ 서버 신원(MCPTT 서버·GMS·(K5 결정 시) 녹취 신원) 프로비저닝 ⑤ `KmsClientReqUrl` 기본값을 실제 서빙 포트로 ⑥ 알람 `A-PRC-015` 연결 ⑦ 옛 문서 정리(`docs/api/mcptt_api.md` §4 의 GET `/certificate`, `USAGE.md`, `csc/docs/idms_auth.md` 의 JSON 본문) ⑧ 단위시험 = RFC 벡터 + 재기동 후 키 동일 |
| **K3 GMK (GMS)** | CSC GMS·DB | `encryption` 그룹마다 GMK·GMK-ID 생성·보관(암호화), 멤버별 I_MESSAGE 를 그룹 문서 조회 응답에 실음(조회자 UID 로), 갱신 정책(키 기간 만료·멤버 제거 — §6 D3), xcap-diff 로 갱신 통지. DB: `ptt_group_keys(group_id, gmk_id, gmk_enc, valid_from, valid_to)` + 마이그레이션·[db_schema.md](../db_schema.md) |
| **K4 CSK·floor** | CSP·CMP | TS 24.379 §7.3 서비스 인가(REGISTER/PUBLISH access token 검증 — [mcx_identity_scope.md](mcx_identity_scope.md) 의 향후 항목)와 같은 자리에서 CSK I_MESSAGE 해석 → 서버 RSK 로 복원(`libmcsec`) → SRTCP 키 유도 → `PTT_JOIN.floor_crypto`(CMP 는 완료). F6 의 남은 반쪽을 닫는다. 서버 신원 키는 CSP 기동 때 KMS 에서 받고 키 기간마다 갱신 |
| **K5 E2E 미디어** | CSP·CMP·녹취 | ① CSP: 암호화 그룹 SDP(프로파일·키 식별 — K0), PCK I_MESSAGE 투명 전달, `encryption` 그룹에 E2E 능력 없는 참가자 합류 거절(응답 코드 K0) ② CMP: **암호문 전달 모드** — 암호화 그룹 세션은 복호·SSRC/seq 재작성 없이 분배(수신자별 재작성([cmp.md](../modules/cmp.md) §3.5)을 끔, floor 게이팅은 송신원 기준으로 유지), 영상 포함 ③ 녹취: §6 D1 결정대로(암호문 보관 + 녹취 신원 복호 / 녹취 제외) — [recording.md](recording.md) 갱신 ④ 계약: `PTT_GROUP_ADD.e2e`(가칭)·[cmp_media_api.md](../../api/cmp_media_api.md) |
| **K6 단말** | `libcimsue`·Android·Windows | KMS 클라이언트(KmsInit/KeyProv/Cert, 키 기간 갱신), 키 보관(Android Keystore 래핑·Windows DPAPI), GMK(그룹 문서)·PCK(개인 통화)·CSK(서비스 인가) 처리, SRTP 외부 키·**MKI** — pjproject `transport_srtp` 패치(현재 SDES/DTLS 만, MKI 없음), floor 모듈 SRTCP(CSK), 보안 상태 표시(잠금 표시·키 만료·KMS 오류), `cimsue-cli` 옵션 |
| **K7 검증** | cspsim·계측기·verify | `libcsim` 에 `libmcsec` 연결(E2E 단말 시뮬레이션), S3 `S3-SCN-PTT-E2E`(암호화 그룹콜 — 멤버 청취 OK·CMP 평문 없음·비멤버 복호 불가)·`S3-SCN-PTT-E2E-PRIVATE`(PCK)·`S3-SCN-FLOOR-CSK`(floor SRTCP), 계측기 시나리오 `PTT-GROUP-CALL-E2E`, 부하 시 CMP 전달 성능 |
| **K8 후속** | — | 시그널링 XML 보호(CSK — ue-init-config 보호 플래그), 서버 간 SPK, MCData E2E(DPPK·페이로드 보호 — 서버 저장·관리 열람과 충돌, [mcdata_messaging.md](mcdata_messaging.md)), cmdp MSRPS |

순서: K0 → K1 → K2 → (K3 ∥ K4) → K5 → K6 → K7. K4 만 끝나도 floor 보호(F6)가 운용에 들어간다.

## 6. 결정 사항 (K0 에서 확정)

| # | 쟁점 | 권고 |
|---|---|---|
| D1 | 암호화 그룹 녹취 — 서버가 평문을 못 본다 | **암호문 보관 + 녹취 신원 복호**: CMP 는 SRTP 를 그대로 저장, 녹취 기능이 KMS 신원을 갖고 GMK 수신자로 등록돼 재생 때만 복호. 규격의 로깅 기능 키 취득 조항 확인 후 확정. 녹취 제외안은 대안 |
| D2 | 암호 라이브러리 — 외부(라이선스 검토) vs 자체 | **자체 구현**(OpenSSL BN/EC 위, 파라미터 집합 1·P-256 한 벌만) + RFC 부속서 벡터. 외부 라이브러리는 라이선스·유지보수 확인 시에만 |
| D3 | GMK 갱신 시점 | 키 기간 만료 + 멤버 제거 시 즉시 갱신(제거된 멤버가 이후 통화를 못 풀게) |
| D4 | KMS 위치 | CSC 안(제품 명칭 SPS — 발표 자료의 SPS 구성 요소와 일치). 별도 모듈 분리는 하지 않는다 |
| D5 | 키 기간 | `UserKeyPeriod` 기본 30일(현 placeholder 2419200 s = 28일 → K0 에서 결정), 만료 전 사전 갱신 |
| D6 | 혼합 멤버 | `encryption` 그룹은 E2E 능력 없는 단말의 합류를 거절(무음 참여 방지) |

## 7. 검증 기준

- `S1-UNIT-MCSEC`: ECCSI·SAKKE RFC 벡터, I_MESSAGE 왕복, 키 유도 벡터
- KMS: 재기동 후 같은 기간의 가입자 키 동일, 다른 가입자·다른 기간 상이, 부속서 D 스키마 검증
- CMP: 암호화 그룹 세션에서 평문 분배·녹취 경로를 타지 않음(STATS), floor `floor_crypto_drop` 0
- 단말: 두 벤더 단말 상호 운용(규격 순정 — 자체 확장 없음)
