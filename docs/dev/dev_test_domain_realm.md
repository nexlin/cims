# 개발·시험 환경 SIP 도메인/Realm 규약

개발 서버(media01 .48)와 시험 환경(테스트베드 배포본·시뮬레이터·단말)이 공통으로 쓰는
서비스 도메인과 인증 realm 의 정본. 서버·단말·검증 도구가 같은 값을 써야 등록(Digest)이
성립하므로, 값은 여기 한 곳에서 정하고 아래 적용 지점에 그대로 넣는다.

| 서비스 | 접속서비스 이름(`access_services.name` = 가입 `service_ref`) | domain | auth_realm |
|---|---|---|---|
| VoLTE | `volte` | `volte.cims.example.kr` | `volte.cims.example.kr` |
| PTT (MCPTT/MCData) | `mcptt` | `ptt.cims.example.kr` | `ptt.cims.example.kr` |

- **domain** = SIP URI authority. 공개 ID(IMPU) `sip:<msisdn>@<domain>`, Digest username(IMPI)
  `<imsi>@<domain>`, PTT 그룹/PSI `sip:<group>@<ptt domain>`, `sip:gms_psi@<ptt domain>`.
- **auth_realm** = Digest challenge 의 realm. 비우면 domain 을 상속한다(CSP `EffectiveRealm`,
  CSC `_service_realm` 동일 규칙). 개발·시험 환경은 **domain 과 같은 값**으로 두어 한 축만 관리한다.
- DNS 는 필요 없다. 단말은 프로비저닝이 내려주는 host/port 로 접속하고 도메인은 URI/realm 에만 쓴다.
  TLS 접속 단말이 서버 인증서를 검증(`VerifyTls`)하면 인증서 SAN 에 접속 host(IP) 가 있어야 하며,
  도메인은 SAN 대상이 아니다.

## 1. 서버 적용 지점

| 지점 | 키/필드 | 값 | 비고 |
|---|---|---|---|
| CSP 접속서비스 컬렉션 `access_services` | `domain`, `auth_realm` | 표의 값 | CSP 인증·라우팅의 **정본**. dev 는 `build/dist/config/access_services.jsonl`, 배포본은 콘솔 `패키지 설정 > csp > Access Service` 또는 `PUT /deployments/{id}/collection/access_services` |
| CSC 단말 프로비저닝 | `Provisioning.Services.volte.domain`, `Provisioning.Services.ptt.domain` | 표의 값 | `/provisioning/me` 가 단말에 내려주는 `sip.domain`. dev 는 `configure --volte-domain/--ptt-domain`(csc.json 렌더), 배포본은 배포 설정 |
| CSC H(A1) 결박 | (파생) | — | `sipHa1 = MD5(imsi@domain:realm:passwd)`. realm 은 가입자 `service_ref` 로 `access_services` 를 조회해 얻고, 없으면 `Provisioning.Services.<kind>` 로 폴백. **도메인/realm 을 바꾸면 기존 H(A1) 이 전부 무효** — 가입자 passwd 를 다시 넣어 재생성해야 한다 |
| CSC IdMS 신원 | `IdMs.Domain`, `IdMs.Issuer`, `IdMs.KmsUri` | 비움(유도) | 비우면 PTT 도메인에서 유도: Domain=`ptt.cims.example.kr`, Issuer=`McpttServer.PublicUrl` 또는 `idms.ptt.cims.example.kr`, KMS=`kms.<Domain>`. 도메인 변경 시 발급된 토큰의 `iss` 가 바뀌어 단말 재로그인 |
| CSC MCPTT 식별자 | (파생) | — | `mcptt_id`/`mcdata_id` = `sip:<msisdn>@<ptt domain>`, 그룹 PSI = `sip:<mcptt_group_id>@<ptt domain>`. DB 에는 bare 값만 저장되고 도메인은 응답 시 붙는다 |
| CSP `Setup.Sip.AuthRealm` | — | 쓰지 않음 | 접속서비스 `auth_realm` 이 정본. 전역 키는 legacy |
| configure.sh (dev) | `--volte-domain`, `--ptt-domain` (`.cims/server.local.json` `configure.volte_domain/ptt_domain`) | 표의 값 | csc.json 렌더 + 시험 스크립트용 `tests/test_env.json`. 현재 코드 기본값은 `ims.mnc033.mcc450.3gppnetwork.org` — **dev 서버는 반드시 옵션으로 지정** |
| 배포 블루프린트 | `deployment/*/scenarios/*.yaml` `access_services[].domain/auth_realm`, 가입자 `domain` | 표의 값 | dev-single-host `smoke.yaml`, prod-multi-host `volte-ptt.yaml` |

## 2. 단말·시뮬레이터 적용 지점

| 지점 | 값의 출처 | 비고 |
|---|---|---|
| Android UE(volte/ptt/cims), Windows 관제조작반, libcimsue SDK 앱 | **프로비저닝** `/provisioning/me` → `services[].sip.domain` | 앱에 도메인을 하드코딩하지 않는다. `SipAccountConfig.domain` 기본값은 빈 문자열, Windows `DispatchSession.VolteDomain/PttDomain` 도 프로비저닝 값 |
| `cimsue-cli` (헤드리스 UE, S3 검증) | `--domain <domain>` | VoLTE 시나리오 `volte.cims.example.kr`, PTT 시나리오 `ptt.cims.example.kr` |
| `cspsim` | `-domain <domain>` | 기본값 `csp` 라 **항상 명시**. Digest username 을 `imsi@<domain>` 으로 조립하므로 서버 접속서비스 domain 과 다르면 403 |
| 검증 파이프라인 | `verify/lib/common/subscribers.py` `VOLTE_DOMAIN`/`MCPTT_DOMAIN`, S3·S6 `seed.py`, `verify/lib/common/access_services.py` 시드 | 코드 상수. 규약 변경 시 함께 갱신 |
| 단위/통합 시험 | `tests/test_csc_subscription_realm.py`, `tests/csc_idms_authreq_unit.py` 등 | 리터럴 도메인 사용 — 규약 변경 시 함께 갱신 |

## 3. 적용 순서 (도메인/realm 을 바꿀 때)

1. **CSP 접속서비스** `volte`/`mcptt` 의 `domain`·`auth_realm` 을 새 값으로 저장(dev: jsonl + `SIGUSR1`, 배포본: 컬렉션 API `signal=true` 또는 restart).
2. **CSC 프로비저닝** `Provisioning.Services.*.domain` 을 같은 값으로(dev: configure 재실행, 배포본: 배포 설정 → update_config → restart). `IdMs.*` 는 비워 유도에 맡긴다.
3. **가입자 H(A1) 재생성** — 모든 Digest 가입자에 passwd 를 다시 설정(콘솔 가입자 편집 또는 `PUT /api/v1/users/{id}` `passwd`). realm 이 바뀌었으므로 이전 `ha1` 은 인증에 실패한다.
4. 단말 **재로그인** — 새 도메인·IdMS issuer 를 받는다(프로비저닝 ETag 변경). 리프레시 토큰은 무효.
5. 시뮬레이터·검증 상수(`cspsim -domain`, `cimsue-cli --domain`, `verify/lib/common/subscribers.py`, 시험 코드 리터럴)와 블루프린트 YAML 을 갱신한다.

## 4. 현재 상태 (.48)

dev 실행본과 테스트베드 배포본(배포 #31 csc, #34 csp)은 아직 `ims.mnc033.mcc450.3gppnetwork.org` /
`ptt.mnc033.mcc450.3gppnetwork.org` 로 동작한다(`access_services.jsonl`·csc 프로비저닝·검증 상수 전부).
이 문서의 값으로 전환하는 작업은 §3 순서대로 서버 → 가입자 H(A1) → 단말 → 검증 도구 순으로 진행한다.
