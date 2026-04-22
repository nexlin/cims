# csp.json.template 역할 분석 (v3 2026-04-22)

## 두 템플릿 비교

| 파일 | 소비자 | 역할 |
|---|---|---|
| `csp/config/csp.json.template` | `configure.sh` → `csp.json` | CSP 프로세스가 **기동 시 읽는 scalar base config** (`@CSP_IP@`, `@DB_HOST@` 등 placeholder 치환) |
| `csp/config/config_template.json` | Console UI + agent 배포 | UI **폼 렌더 schema** + 9 collection **jsonl 스키마** |

## 중복 필드 (양쪽에 존재)

`Setup.Sip.{LocalIp, UdpPort, UdpThreadCount, StackExecutePeriod, MinRegisterTimeout, UserTimeout, StaleCallTimeout, CallPickupId, SendOptionsPeriod}`,
`Setup.Roles.{CSCF, TAS, PTT_AS, IBCF}`, `Setup.RtpRelay.*`, `Setup.Log.*`, `Setup.Database.*`

## csp.json.template 에만 있는 필드 (UI 미노출 인프라)

| 필드 | 성격 |
|---|---|
| `Setup.Sip.{TcpPort, TcpThreadCount, TcpRecvTimeout, TlsPort, TlsAcceptTimeout, CertFile}` | TCP/TLS 리스너 (v3 에서 Local Node 로 이관 예정) |
| `Setup.RtpRelay.LocalCmpIp` | 로컬 CMP 응답 수신용 IP |
| `Setup.DataFolder.{User, SipServer, Group}` | JSON fallback 경로 (legacy) |
| `Setup.Cdr.Folder` | CDR 디렉토리 |
| `Setup.ServiceLogging.{Dir, Enable, Recording}` | 서비스 로그 경로 — `@SERVICE_LOG_DIR@` 치환 |
| `Setup.Monitor.{Port, ClientIpList}` | Monitor TCP 포트 |
| `Setup.Security.DenySipUserAgentList` | 기본 차단 UA 리스트 |

## 왜 csp.json.template 이 지금은 필요한가

1. **환경 의존 placeholder (`@VAR@`)** — `configure.sh` 가 `--local-ip`, `--db-password` 등 런타임 값을 주입. 빌드마다/배포마다 바뀜. `config_template.json` 의 static default 만으로는 이걸 처리 못함.
2. **CSP 기동 초기 진입점** — CSP 는 `csp.json` 이 있어야 기동. `config_template.json` 은 UI 용이고 CSP 는 이걸 직접 읽지 않음 (jsonl collection 로더만).
3. **UI 미노출 인프라 값** — TcpPort/CertFile/DataFolder 등은 운영자가 UI 로 변경하지 않는 값.

## 향후 통합 방향 (권고)

**A안 (권장): `config_template.json` 으로 단일화 + `csp.json.template` 제거**
- 중복된 sections 필드를 `config_template.json` 에 유지
- 현재 `csp.json.template` 전용 필드를 `hidden: true` 로 `config_template.json` 에 추가
- `configure.sh` 를 `config_template.json` 기반으로 재작성 — default 값을 읽어 csp.json 생성 + `@VAR@` 치환
- 장점: 단일 소스, UI 와 base config 불일치 제거
- 단점: `configure.sh` 재작성 범위 큼. `hidden` 필드 + `env_placeholder` 같은 메타 필드 필요

**B안: 현 구조 유지 (이번 세션 결론)**
- 두 파일 분리 유지
- 중복 default 값은 일치시키기만 (이미 sync 됨)
- 장점: 범위 작음, 즉시 반영. 단점: 여전히 두 파일 관리

## 이번 세션 결정

**B안** 적용 — 두 파일 모두 `csp/config/` 아래 유지. `config_template.json` 이동 완료.
통합은 **다음 세션에서 `configure.sh` 재설계와 함께** 진행 권장.

(CMP 도 동일한 구조: `cmp/config/cmp.json.template` + `cmp/config/config_template.json`)
