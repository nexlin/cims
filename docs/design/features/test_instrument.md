# 계측기 `oam-cims-tester` — UE 부하·피어 시뮬레이터·시험 콘솔

> CIMS 서버(CSC·CSP·CMP·CMDP)의 **기능 시험과 성능 시험을 한 도구로** 수행하는 관리평면 서비스 모듈 + 워커.
> 네 가지를 한다 — ① N 개 UE 로 CIMS 접속 서비스(VoLTE·VoIP·PTT·MCData) 수행, ② CIMS 와 피어링하는
> **IBCF 시뮬레이터**(하위에 단말 다수가 있는 타 IMS 처럼 동작), ③ **IP-PBX · MGCF/MGW 시뮬레이터**,
> ④ 이 셋을 설정·실행하고 결과를 보는 **시험 콘솔**.
>
> 계측기는 **OAM 게이트웨이 뒤의 서비스 모듈**이다 — `oam-svc`(서비스 관측)와 동격으로 `oam`(base) 위에 얹는다
> ([oam_base_service_split.md](oam_base_service_split.md) D3·D5). 그래서 두 가지로 운영한다:
> **독립 운영**(계측기 호스트에 자기 `oam` + `oam-cims-tester` — 시험 대상 밖, 상용 시험 표준) 과
> **동거 운영**(대상 관리평면의 `oam` 에 `oam-svc` 와 나란히 — 개발서버·소규모). 어느 쪽이든 부하 **발생기(워커)는
> 대상과 다른 호스트**에 둔다([../csp_control_plane_load_hardening.md](../csp_control_plane_load_hardening.md) §테스트 인프라 교훈).
> 상용 배포 게이트(S1~S6, `cims-verify`)를 **대체하지 않고** 그 시나리오 실행부와 성능 시험 축을 담당한다(§9).
>
> 관련: [oam_base_service_split.md](oam_base_service_split.md)(게이트웨이·서비스 모듈 계약 — 이 모듈이 따르는 규약),
> [sip_service_model.md](sip_service_model.md)(CSP 피어링 모델 — 계측기 피어 축의 상대),
> [sip_statistics.md](sip_statistics.md)(세는 단위 3계층 — 계측기 지표가 같은 어휘를 쓴다),
> [../../VERIFICATION_PROCESS.md](../../VERIFICATION_PROCESS.md)(게이트 모델),
> [ue_sdk.md](ue_sdk.md) §4.7(`cimsue-cli` — 실스택 UE 축).

---

## 1. 범위와 결론

| 결정 | 내용 | 근거 |
|---|---|---|
| 컨트롤러 = OAM 서비스 모듈 `oam-cims-tester` | `oam-svc` 와 같은 규약의 독립 Python 모듈 — 자기 `pkg.json`·`config_template.json`·엔트리포인트 `oam_cims_tester_app.py`, loopback bind, 게이트웨이 세그먼트 `/api/v1/tester` 를 self-register, 인증은 base 공유 `CimsAuth.JwtSecret` 로 독립 검증. 공통 코드는 `oam/src`(httpsrv·util·file_store)에서 import(oam-svc 와 동일) | 로그인·계정·RBAC·배포·감독·인증서·설정 UX 를 base 에서 그대로 받는다. 별도 인증·별도 배포 체계를 만들지 않는다 |
| 콘솔 = 콘솔 팩 | 별도 SPA 가 아니라 `ems/service/console` 과 같은 **서비스 팩**(`ems/tester/console`, alias `@tester`). 셸·디자인 시스템·레이아웃 엔진은 core 것 | 콘솔 플랫폼 원칙(코어는 서비스를 모르고, 서비스는 매니페스트로 등록) 그대로 |
| 워커 = 별도 C++ 모듈 `cims-tester-worker` | cspsim 의 `SimSession·RtpThread` 를 라이브러리 `libcsim` 으로 승격해 UE·피어 엔드포인트 엔진으로 쓴다. agent 가 배포·감독하는 일반 모듈(cmp 와 같은 지위). cspsim 바이너리는 `libcsim` 위 CLI 로 전환기 유지 | 프로세스당 수백 UA·AKA/IPsec/SRTP/floor 를 이미 구현. 워커 N 대 배포·CPU/RSS 관측을 agent heartbeat 로 공짜로 얻는다 |
| 실스택 UE 축 병행 | 소수 UE 의 코덱·지터·SRTP 정합은 `cimsue-cli`(pjsua2) 로. 대량은 `libcsim` | 대상과 같은 스택(psip)만 쓰면 공통 결함이 안 보인다 |
| 피어 3종 = 한 엔진 + 프로파일 | IBCF·IP-PBX·MGCF/MGW 는 **peer 엔드포인트 엔진 하나**에 프로파일(헤더 규약·코덱·응답 타이밍·기능)만 다르게 | 셋 다 "등록 없는 고정 주소 + 다수 신원 + 자동 응답 정책" 이라는 같은 골격 |
| 지표 표준 | 시그널링 = **RFC 6076**(RRD·SRD·SDD·SER·SEER·SCR·ISA), 부하 절차 = **ETSI TS 186 008**(SApS·DOC·IHS), 미디어 = RFC 3550 손실/지터(+ITU-T G.107 E-model MOS 추정), floor = TS 24.380 request→granted 지연 | 자체 정의 대신 규격 지표로 보고서를 낸다 |
| 결과 저장 | run 디렉터리(`run.json` + `metrics.sqlite` + 이벤트 JSONL) — 모듈 소유 데이터 디렉터리. file_store 에는 run **색인**만(`modules/oam-cims-tester/runtime/runs`) | 1초 버킷·히스토그램은 jsonl 레코드 스토어의 대상이 아니다. 색인만 있으면 콘솔 목록·검증 콘솔 교차 참조가 된다 |
| 운영 형태 2가지 | **독립**: 계측기 호스트에 `agent`+`oam`(base)+`oam-cims-tester`, 워커는 같은 호스트 또는 N 대. **동거**: 대상 관리평면 노드의 `oam` 뒤에 `oam-svc` 와 함께 | 독립 = 대상 OAM 이 죽어도 계측기 콘솔·판정이 산다(상용 시험). 동거 = 설치 한 번으로 시험(개발서버). 모듈은 어느 쪽인지 모른다 — 대상 관측은 항상 대상 OAM **API** 로 한다(I5 스토어 단일 소유) |

### 1.1 왜 CSP 변종이 아닌가

CSP 골격(리스너·jsonl 설정·로깅·통계·패키징·HA)을 물려받는 안(가칭 TSP 변종 + `IModule` 계측 모듈)을
검토했다. 얻는 것은 리스너/TLS/설정 로더 재사용이고, 잃는 것은 아래다.

- **계측기의 본체가 CSP 에 없다** — UAC(발신·REGISTER 클라이언트)·RTP 발생/측정·부하 모델·1초 해상도
  지표·다중 워커 조율. CSP 통계 파이프라인은 1분/5분 버킷의 운영 통계라 성능 시험 지표(히스토그램·p95·SApS 단계)와 다르다.
- **대상과 코드베이스를 공유하면** 계측 코드가 상용 경로에 섞이고(현 IBCF 처럼 `IsEnabled` 가드 누락 한 곳이면 오염),
  CSP 릴리스마다 계측기가 재빌드·재검증 대상이 된다.
- 피어 시뮬레이터가 "하위 단말처럼 응답" 하려면 로컬 미디어가 필요한데 CSP 는 미디어를 CMP 에 위임한다.

재사용은 **라이브러리·관습 단위**로 한다 — `ext/psip`, `ext/libsrtp`, cspsim 의 `SimSession/RtpThread`, CSP 의
`config_template.json`·`pkg.json`·`cims-svc` 규약, 메시지 로그 `*.msg.*.jsonl` 형식.

### 1.2 왜 별도 콘솔·별도 인증이 아닌가

첫 안은 "대상 OAM 이 시험 중 재시작해도 콘솔이 살아야 한다" 는 이유로 별도 SPA·로컬 계정을 뒀다. 그 요구는
**독립 운영 형태**(계측기 자기 `oam`)가 충족하고, 동거 형태에서 base 재기동 시 잃는 것은 **화면**뿐이다 — run 은
`oam-cims-tester` 프로세스와 워커가 이어가고(base 는 게이트웨이일 뿐, 프로세스 수명이 다르다) run 상태는 자기 디렉터리에
있다. 반면 별도 SPA 는 계정·RBAC·감사·TLS 인증서·배포·업그레이드·감독을 전부 두 번 만든다. 관리평면이 이미 "게이트웨이
뒤 N 개 서비스 모듈" 로 일반화돼 있으므로(D3) 그 자리에 들어가는 것이 체계에 맞다.

### 1.3 소스 배치

```
ems/tester/oam/           oam-cims-tester — pkg.json · config/config_template.json · src/oam_cims_tester_app.py
                          · src/handlers(run·scenario·topology·report·stream) · scenarios/ · schema/ · bin/cims-tester(CLI)
ems/tester/console/       콘솔 팩 — manifest.tsx · pages/ · widgets/  (core console 이 @tester 로 참조)
tester/worker/            cims-tester-worker — C++17, pkg.json, libcsim 위 (루트 CMake 대상)
cspsim/                   libcsim(SimSession·RtpThread 추출) + cspsim CLI(전환기)
```

`ems/service/{oam,console}` ↔ `ems/tester/{oam,console}` 이 대칭이다. 엔트리포인트 파일명은 agent 의 python 데몬
매칭 규칙(패키지 id 하이픈 → 언더스코어 `<stem>_app.py`)을 따른다.

---

## 2. 구성

```
브라우저 ──HTTPS:4419──▶ oam (base 게이트웨이 · 콘솔 정적 · 계정/RBAC · 배포/agent)
                          │ /api/v1/tester/* ──loopback──▶ oam-cims-tester (Python, 127.0.0.1:4490)
                          │ /api/v1/stats 등 ─────────────▶ oam-svc (동거 형태일 때만 존재)
                          │                                   · 시나리오/프로파일 저장소 (YAML)
                          │                                   · run 오케스트레이터 (워커 배분·단계 부하·중단)
                          │                                   · 결과 저장 (run.json + metrics.sqlite + 이벤트 JSONL)
                          │                                   · 대상 관측 어댑터 (대상 OAM API·agent heartbeat·ssh /proc)
                          │                                   · 보고서 (Markdown/PDF, RFC 6076 표·차트)
                          │                                   · SSE 라이브 스트림 (게이트웨이 통과)
                          │                                        │ HTTP/JSON 제어      ▲ TCP JSONL(1초 집계·이벤트)
   agent(계측기 호스트) ── 배포·감독 ──▶ ┌──────────────────────────┴──┐  ┌───────────────┴─────────────┐
                                        │ cims-tester-worker #1 (C++) │  │ cims-tester-worker #N        │
                                        │  ue 풀 (libcsim)            │  │  peer:ibcf / pbx / mgcf      │
                                        │  real-ue (cimsue-cli)       │  │  ue 풀                       │
                                        └──────────────┬──────────────┘  └───────────────┬─────────────┘
                                                       │ SIP(UDP/TCP/TLS) · RTP/SRTP · HTTPS(CSC)   │ SIP 피어링 · RTP
                                                       ▼                                            ▼
                                            ┌──── 시험 대상 CIMS ────┐                 CSP remote_nodes/routes 로 등록된 피어 주소
                                            │ CSC · CSP · CMP · CMDP │◀───────────────────────────────┘
                                            │ OAM(관측 API 만)        │
                                            └────────────────────────┘
```

| 구성요소 | 패키지 id | 언어 | 역할 |
|---|---|---|---|
| 컨트롤러 | `oam-cims-tester` | Python 3.14 (oam/src httpsrv) | 시나리오 검증·워커 배분·run 수명주기·결과 저장·대상 관측·보고서·콘솔 API·CLI. `ha_capability: standalone` |
| 워커 | `cims-tester-worker` | C++17 (`libcsim` = psip + libsrtp + cspsim 이식) | 엔드포인트 풀 실행. 종류 `ue` / `peer` / `real-ue`. 컨트롤러 명령으로 등록·발신·응답·부하율을 바꾸고 1초 집계와 이벤트를 스트림으로 올린다. agent 가 배포·감독 |
| 콘솔 팩 | (console 번들 안) | React · Tailwind · shadcn | 토폴로지/시나리오/프로파일 편집, 실행, 라이브 KPI, 결과 비교, 보고서 |
| CLI | `cims-tester` | Python | `cims-tester run <scenario> [--load <profile>] --json`, `runs`, `report <id>` — cims-verify 항목이 호출하는 진입점. 컨트롤러 API 의 얇은 클라이언트 |

### 2.1 운영 형태

| 형태 | 노드 구성 | 쓰는 곳 | 비고 |
|---|---|---|---|
| **독립** | 계측기 호스트: `agent` + `oam`(base) + `oam-cims-tester` (+ 워커). 워커 추가 호스트 N 대는 `agent` + `cims-tester-worker` | 상용 배포 전 성능·기능 시험, 사이트 인수 시험 | 대상 관리평면과 **완전 분리** — 계정·스토어·인증서 모두 자기 것. 대상은 시나리오 `target` 로 지정(여러 대상 등록 가능). 설치는 초도 설치 절차([../../user-manual/initial_install.md](../../user-manual/initial_install.md))와 같다 — 부트스트랩 후 콘솔에서 `oam-cims-tester`·워커 배포 |
| **동거** | 대상 관리평면 노드의 `oam` 뒤에 `oam-svc` 와 나란히. 워커는 다른 호스트 | 개발서버·소규모 시험베드 | base 재기동 시 콘솔만 잠시 끊긴다(run 은 계속). 대상 관측은 여전히 대상 OAM API — 같은 노드라도 스토어를 직접 읽지 않는다(I5) |

두 형태의 차이는 **블루프린트**([auto_deployment.md](auto_deployment.md))에만 있다. 모듈 코드·설정 스키마는 같다.

---

## 3. 엔드포인트 모델 — 워커가 실행하는 것

엔드포인트(endpoint) = SIP 신원 하나를 가진 행위자. 워커는 **풀(pool)** 단위로 만든다.

| 종류 | 신원 | 등록 | 수신 주소 | 미디어 | 용도 |
|---|---|---|---|---|---|
| `ue` | 가입자(DB `*_subscriptions` 또는 creds JSONL — cspsim `-db`/`-creds` 승계) | REGISTER (Digest/AKA, sec-agree, IPsec, TLS) | UE 마다 포트 | RTP/SRTP 송수신·측정 | 접속 서비스 시험·부하 |
| `peer` | 도메인 하나 + 가상 신원 범위(DID/내선/E.164) | 없음(고정 IP) 또는 트렁크 REGISTER(PBX 프로파일) | **풀당 하나**(CSP `remote_nodes` 가 가리키는 ip:port:protocol) | 신원별 RTP | IBCF·PBX·MGCF 시뮬레이션 |
| `real-ue` | 가입자 | REGISTER | 프로세스당 1 | pjsua2 실코덱 | 정합 표본 검사(소수) |

### 3.1 `ue` 풀

cspsim `SimSession` 의 능력을 그대로 승계한다 — REGISTER/AKA/sec-agree/IPsec, UDP/TCP/TLS, SRTP SDES
off/optional/required, AMR-WB/H.264, MCPTT(그룹콜·floor·affiliation·GMS/CMS SUBSCRIBE·emergency·preempt·adhoc·listen),
CSC/IdMS·XCAP, VoLTE 보조(REFER blind/attended·INVITE-Replaces·Join·pickup·hunt·dialog 이벤트).
**추가할 것**: MCData SDS(MESSAGE·MSRP, 현재 SDK 에만 있음), TLS 서버 인증서 검증(현재 미검증), RTCP 수신 통계,
RFC 4028 세션 타이머 응답, 신원마다 개별 행동 스크립트(§4).

### 3.2 `peer` 풀과 프로파일

한 엔진에 프로파일 셋을 얹는다. 공통 = 고정 수신점, INVITE 수신 시 **To 신원이 범위 안이면 하위 단말이
받는 것처럼** 응답 정책 실행(즉시/지연 200, 180 후 200, 486/480/404/503, 무응답), 발신 시 From 신원을
범위에서 뽑아 CSP 로 INVITE, OPTIONS 응답, 오류 주입(응답 지연·특정 코드·미디어 무응답·재전송 유실).

| 프로파일 | 규격 기준 | 특징 동작 |
|---|---|---|
| `ibcf` | TS 29.165(II-NNI Ici/Izi), TS 24.229 | 타 IMS 코어처럼 — `P-Asserted-Identity`·`P-Charging-Vector`·`Route/Record-Route` 다중 hop, THIG 흔적(토큰화 Via), 100rel/PRACK, `Privacy`, 도메인 기반 착신, TLS 상호인증 옵션, 다중 peer(장애조치·round-robin 상대) |
| `pbx` | RFC 3261/3262/3311/3515/4733, **SIP Forum SIPconnect 2.0**(IP-PBX↔사업자 SIP 트렁크 프로파일) | 트렁크 REGISTER(계정 1개로 DID 범위 대표) 또는 고정 IP 피어링, 내선 범위·DID 매핑, 183 early media, hold/resume(`sendonly`/`inactive` re-INVITE), REFER 발신(PBX 측 전달), DTMF RFC 4733, 세션 타이머 refresher. **코덱 = G.711 A/μ 기본**(SIPconnect 필수 코덱 — 광대역은 선택이고 기업 PBX 는 AMR-WB 를 거의 안 가짐), 시나리오로 G.722·AMR-WB 추가 오퍼 가능 → CIMS 트랜스코딩 경로(§12)와 순수 relay 경로를 둘 다 시험 |
| `mgcf` | TS 29.163(Mg — CSCF↔MGCF 는 **평문 SIP**, TS 24.229 프로파일), Q.850 | E.164 만, 183+ringback early media, `Reason: Q.850;cause=` 종료, 응답 지연(PSTN 셋업 모사), in-band DTMF 옵션. **코덱 = AMR-WB 기본 + AMR + G.711**(IM-MGW 가 IMS 쪽에 AMR-WB 를 오퍼하고 PSTN 쪽 G.711 로 자기 트랜스코딩 — TS 29.163 §9 IM-MGW 기능, GSMA IR.92 코덱 세트) → CIMS 는 relay 만 하면 된다. **SIP-I(ISUP 캡슐화, ITU-T Q.1912.5)는 범위 밖** — Mg 에는 없고 CS 상호접속 트렁크의 프로파일이라 VoLTE IMS 연동에 필요하지 않다 |

CSP 쪽 상대 설정은 기존 모델 그대로다 — `remote_nodes`(피어 주소)·`routes`·`route_sets`(failover/round_robin/weighted)·
`rules`/`routing_policies`(도메인·번호 prefix)·`acl_policies`. 계측기는 시나리오의 `target.csp_collections` 로
이 컬렉션 시드를 **대상 CSC 컬렉션 API** 에 넣고(`S6-SEED` 의 `seed_ibcf_routing()` 을 일반화), 끝나면 되돌린다.

### 3.3 `real-ue`

`cimsue-cli --json` 을 워커가 스폰해 JSON 한 줄 결과(`rx_pkts`·`granted`·exit code 표)를 지표로 받는다.
같은 시나리오에서 `ue` 풀 100 + `real-ue` 2 처럼 섞어, 대량 부하 아래 실스택 단말의 품질을 표본 측정한다.

---

## 4. 시나리오 모델 — YAML 정본 (`ems/tester/oam/scenarios/`)

세 층으로 나눈다. **토폴로지**(어디에 무엇이) · **시나리오**(누가 무엇을 하고 무엇을 기대) · **부하 프로파일**(얼마나 빨리, 얼마나 오래).
기능 시험 = 시나리오 + 단발 실행, 성능 시험 = 같은 시나리오 + 부하 프로파일. 시나리오를 두 번 쓰지 않는다.
토폴로지는 콘솔에서 편집·저장(`modules/oam-cims-tester/runtime/topologies`), 시나리오·프로파일은 패키지 동봉 YAML + 운영자 추가분.

```yaml
# topology.yaml — 대상과 자원
target:
  name: media01
  csp: { ip: 10.0.0.45, udp: 5060, tcp: 25061, tls: 5061, domain_volte: volte.cims.example.kr, domain_ptt: ptt.cims.example.kr }
  csc: { host: 10.0.0.45, port: 4430, tls: true }
  oam: { url: https://10.0.0.45:4419, token_env: TESTER_OAM_TOKEN }   # 대상 관측 전용 (동거 형태여도 API 경유)
  observe: [oam_stats, oam_alarms, agent_heartbeat, ssh_proc]         # 대상 측 KPI 원천
workers:                                                              # 배포된 cims-tester-worker — 자기 agent 인벤토리에서 자동 발견, 수동 추가 가능
  - { name: w1, url: http://10.0.0.61:7100, cpus: 8 }
  - { name: w2, url: http://10.0.0.62:7100, cpus: 8 }
pools:
  volte_ue:  { kind: ue, source: { db: target, table: volte_subscriptions, offset: 0, count: 2000 }, transport: tls, srtp: optional }
  ptt_ue:    { kind: ue, source: { creds: creds/ptt.jsonl }, transport: udp }
  peer_kt:   { kind: peer, profile: ibcf, bind: { ip: 10.0.0.62, port: 5080, protocol: tls }, domain: ims.kt.test,
               identities: { e164_range: ["+82212340000", "+82212349999"] } }
  pbx_hq:    { kind: peer, profile: pbx, bind: { ip: 10.0.0.62, port: 5090, protocol: udp }, register: { user: pbx-hq, ha1_env: PBX_HA1 },
               identities: { did_range: ["0212345000", "0212345099"], ext_len: 4 } }
```

```yaml
# scenarios/volte/call_basic.yaml — 시나리오 (기능·성능 공용)
id: VOLTE-CALL-BASIC
roles:
  caller: { pool: volte_ue }
  callee: { pool: volte_ue, disjoint_from: caller }
flow:                               # 단계 = 매핑 하나 (step 키 + 인자). 정본 스키마 = schema/scenario.schema.json
  - { step: register,   who: [caller, callee], expect: { code: 200, rrd_ms: { p95: 500 } } }
  - { step: invite,     from: caller, to: callee, media: { audio: amr-wb, video: h264 } }
  - { step: answer,     who: [callee], after_ms: 1500, expect: { srd_ms: { p95: 1500 } } }
  - { step: media_hold, seconds: "${ht}", expect: { rtp_loss_pct: { max: 0.5 }, jitter_ms: { p95: 30 } } }
  - { step: bye,        from: caller, expect: { sdd_ms: { p95: 300 } } }
target_evidence:                    # 대상 측 증거 (선택 — 기능 시험 게이트에서 사용)
  - { kind: recording_created, min: 1 }
  - { kind: log_errors, max: 0 }
```

```yaml
# profiles/step_load.yaml — 부하 프로파일 (ETSI TS 186 008 step 방식)
model: step                          # constant | step | ramp | soak | burst
unit: saps                           # 시나리오 시도/초 (= cps 를 시나리오 단위로 일반화)
start: 5      step: 5      hold_s: 300      max: 100
ht: 20                               # ${ht} 바인딩
ihs_threshold_pct: 0.1               # 부적절 처리 시나리오 비율 — 초과 시 DOC 확정·정지
stop_on: { target_cpu_pct: 85, csp_5xx_pct: 1.0 }
```

- 단계(step) 어휘는 cspsim 시나리오 enum 을 **데이터로 옮긴 것**이다: `register/deregister/invite/answer/reject/bye/refer/replaces/join/pickup/
  subscribe/publish/group_call/floor_request/floor_release/sds_send/sds_recv/media_hold/wait/expect`. 새 단계 = 워커 재빌드, 새 시나리오 = YAML 만.
- `expect` 는 RFC 6076 지표 이름을 그대로 쓴다(`rrd_ms`·`srd_ms`·`sdd_ms`·`code`·`ser_pct`·`scr_pct`). 판정은 **발생기 측 관측이 1차**,
  `target_evidence`(녹취·이벤트·로그 오류 0)는 2차다 — cims-verify 가 대상 산출물만 세던 것과 반대다.
- 기존 시험 문서 형식(`ptt-test-scenario/*.csv` 의 절차·예상 결과·확인 방법)은 보고서 출력 형식으로 유지한다.

---

## 5. 지표 — 세는 단위와 정의

[sip_statistics.md](sip_statistics.md) §1 의 **attempt / session / leg** 3계층을 그대로 쓴다. 계측기는 자기가 만든 시도를 알기 때문에
분모가 정확하고, 대상 통계와 **같은 어휘로 대조**할 수 있다(예: 계측기 SER 99.8 % vs CSP 성공률 99.7 % → 차이는 손실 응답).

| 축 | 지표 | 정의 |
|---|---|---|
| 등록 | RRD | REGISTER 첫 송신 → 최종 200 (401 왕복 포함). p50/p95/p99, 실패 코드 분해 |
| 세션 | SRD · SDD · SDT | INVITE → 200(또는 최종 응답), BYE → 200, 세션 지속 |
| 세션 비율 | SER · SEER · SCR · ISA | 성립률(200/시도)·유효 성립률(사용자 거절 제외)·완료율(정상 BYE/성립)·부적절 시도 |
| 부하 | SApS · 동시 세션 · DOC · IHS | 단계별 시도율, 순간 동시, 설계 목표 용량(IHS 임계 넘기 직전 단계), 부적절 처리 비율 |
| 미디어 | 손실 % · 지터 ms · 단방향 무음 · MOS 추정 | RTP seq/timestamp(RFC 3550), RTCP 수신 시 상대 보고, G.107 E-model R→MOS(선택) |
| PTT | floor request→granted · taken 도달 · queue 대기 · 그룹 fan-out 완료 시간 | TS 24.380 메시지 시각 |
| MCData | SDS 전달 지연 · disposition 회신율 | TS 24.282 |
| 대상 측 | CSP/CMP/CSC CPU·RSS · 5xx 수 · 알람 발생 · 포트/세션 누수 · 로그 ERROR/FATAL | 대상 OAM API·heartbeat·ssh 샘플러 — 발생기 시계와 같은 1초 버킷에 정렬 |

저장: run 마다 `<DataDir>/runs/<id>/run.json`(정의·verdict·요약), `metrics.sqlite`(1초 버킷 × 지표, 히스토그램은 로그 버킷),
`events.jsonl`(실패 개별 건 — Call-ID·코드·시각·워커), `sip/`(선택 — 실패 호의 메시지 덤프, CSP `*.msg.*.jsonl` 과 같은 형식).
`DataDir` 은 모듈 설정(`Tester.DataDir`, §8) — oam-svc 의 `ServiceLogging.Dir` 과 같은 지위. file_store 에는 run 색인 레코드만 둔다.

---

## 6. 계약

### 6.1 컨트롤러 ↔ 워커

- **계약 정본** = `ems/tester/oam/src/services/tester_models.py`(pydantic) → `schema/*.json`(`bin/gen-schemas` 생성, 워커 C++ 가 읽는다). 모든 모델은 `extra='forbid'` — 오타 난 키는 거절한다.
- **제어(컨트롤러 → 워커)** HTTP/JSON: `POST /pools`(풀 생성·신원 적재 — `worker_pool_create`), `POST /runs`(컴파일된 단계 + 역할 배분 — `worker_run_start`), `POST /runs/{id}/rate`(SApS 변경),
  `POST /runs/{id}/stop`, `GET /health`(용량·CPU·활성 엔드포인트·시각 — `worker_health`). 워커는 시나리오 YAML 을 모르고 **컴파일된 단계 목록**만 받는다.
- **관측(워커 → 컨트롤러)** 지속 TCP JSONL(컨트롤러 `Tester.WorkerStreamPort` 7110) 한 줄 = 한 레코드: `hello` → `agg`(1초 집계 — `counters`(attempt/session/leg·응답 코드·RTP 카운터) · `gauges`(동시 세션·등록 수·CPU) · `timers`(`rrd_ms`·`srd_ms`·`sdd_ms`·`jitter_ms`·`floor_grant_ms` 히스토그램)) · `event`(실패 개별 건 — Call-ID·역할·단계·코드) · `log`. UDP 는 부하 중 유실되어 지표를 왜곡하므로 쓰지 않는다.
- 워커 용량 선언: 기동 시 측정한 `max_endpoints`·`max_saps`(cspsim 실측 기준 코어당 UA 약 200, RTP 포함). 컨트롤러는 풀을 워커에
  나눠 배분하고 부족하면 시작 전에 거절한다.
- 시계: 워커·컨트롤러 NTP 정렬을 `GET /health` 에서 확인, 오차 > 50 ms 면 경고.
- 워커 발견: 컨트롤러는 자기 base 의 배포 목록(`GET /api/v1/deployments`, 패키지 `cims-tester-worker`)에서 워커 주소를 자동 수집한다.
  토폴로지의 `workers` 수동 항목은 이를 덮어쓰거나 보탠다(agent 없는 호스트).

### 6.2 컨트롤러 ↔ base OAM (서비스 모듈 규약)

[oam_base_service_split.md](oam_base_service_split.md) 의 규약을 그대로 따른다. 계측기 고유 사항만 적는다.

| 항목 | 내용 |
|---|---|
| 게이트웨이 세그먼트 | `/api/v1/tester` **하나**(D2 — 서비스 = 최상위 세그먼트 하나). `pkg.json` `gateway.routes=["/api/v1/tester"]`, `gateway.default_port=4490`. 하위: `/health`·`/schema/{name}`·`/validate`·`/scenarios`·`/profiles`·`/topologies`·`/runs`·`/runs/{id}/stream`(SSE)·`/runs/{id}/report`·`/workers`·`/events`(SSE — run/워커 상태 변화) |
| 인증·RBAC | base 가 배포 시 `CimsAuth.JwtSecret` 주입(`meta.gateway.routes` 보유 모듈 자동), 모듈이 토큰 독립 검증. 권한 = 조회 `monitor`, run 실행·중단·토폴로지 편집 `operator`, 시나리오/프로파일 삭제 `manager` |
| 설정 | `config_template.json` 선언 키만(§14.7 write 마스크). `Server.Ip/Port`(loopback 4490)·`Tester.DataDir`·`Tester.RunRetainDays`·`Tester.WorkerControlPort`(7100)·`Tester.WorkerStreamIp/Port`(7110 — 워커 관측 수신, 관리망 bind). 대상(SUT)·워커·풀은 설정이 아니라 **토폴로지 레코드**(런타임 store, 콘솔 편집)다. `CimsAuth.JwtSecret`·`Mgmt.Cidr`·`CimsRuntimeDir` 은 **선언하지 않는다**(base 주입 파생값) |
| 스토어 | `modules/oam-cims-tester/runtime/{topologies,runs}` 단일 소유(I5). run 본체는 `Tester.DataDir` |
| 버전 계약 | `pkg.json` `gateway.requires_base_oam` — SSE 통과(아래)를 가진 base 최소 버전. base 는 self-register 시 라우트 레코드에 기록하고 자기 버전이 낮으면 경고 로그(등록은 한다 — 거부하면 콘솔에서 원인이 보이지 않는다) |
| **base 확장 ① — SSE 통과** | 게이트웨이 프록시(`handlers/gateway.py`)는 요청 `Accept: text/event-stream` 이면 총 타임아웃 없이(연결 5 s) 업스트림을 부르고, 응답 `Content-Type: text/event-stream` 이면 **청크 passthrough**(전체 버퍼링 없음, 클라이언트 절단·업스트림 종료 어느 쪽이든 응답 해제)한다. 판정은 라우트 속성이 아니라 응답 타입 — 어느 서비스 모듈이든 SSE 를 낼 수 있다. 그 외 응답은 종전대로 5 s(다운로드 120 s) 버퍼링 |

---

## 7. 콘솔 팩 (`ems/tester/console`) — 화면

| 화면 | 내용 |
|---|---|
| 토폴로지 | 대상(CSP/CSC/OAM 주소·도메인), 워커 상태·용량(agent 배포 목록 + `GET /health`), 풀 정의(가입자 원천·피어 프로파일). 연결 검사(OPTIONS·CSC health·대상 OAM 토큰) |
| 시나리오 | YAML 편집(스키마 검증·단계 팔레트), 시나리오 목록·태그(volte/ptt/mcdata/trunk/pbx/mgcf), 단발 실행 |
| 부하 프로파일 | constant/step/ramp/soak/burst 편집, 정지 조건 |
| 실행(라이브) | SApS·동시 세션·SER·SRD p95·RTP 손실·대상 CPU 를 한 시간축에(SSE 1초), 단계 진행 띠, 실패 이벤트 표, 즉시 중단·율 조정 |
| 결과 | run 상세 — RFC 6076 표, 히스토그램, 단계별 DOC/IHS, 실패 코드 분해, 대상 알람 타임라인 겹침, 실패 호 SIP 덤프 열기 |
| 비교 | run 두 개 이상 겹쳐 회귀 판정(빌드 sha·패키지 manifest 해시 기준) |
| 보고서 | Markdown/PDF — 검증 콘솔 `VerificationPrintReport` 와 같은 인쇄 규약, `ptt-test-scenario` CSV 형식의 절차·예상·결과 표 포함 |

**메뉴 자리** — 관리 영역(`admin`)에 그룹 `test`(**시험**)를 새로 둔다. ITU-T M.3400 Maintenance 기능군의 *Testing* 에
해당하며, 릴리스 그룹(SW Mgmt — 검증/패키징)과 다르다: 검증은 배포 게이트, 시험은 부하·피어 시험 도구다.
`nav-types.ts` 의 `RouteSection` 에 `id:'test', area:'admin'` 하나 추가 — 코어 수정은 이 한 줄이고 화면은 전부 팩 소유.

**콘솔 번들 구성 — base 확장 ②.** 번들은 **하나**다(D1 "full 번들 1개" 그대로) — 코어 + CIMS 팩(`@svc`) + 계측기 팩(`@tester`)을
전부 담아 `oam`(base) 패키지에 동봉한다(`scripts/sync.sh` 한 벌 빌드 → `dist/console/dist` → `scripts/package.sh` oam 스테이지).
서비스 모듈 패키지(`oam-svc`·`oam-cims-tester`)는 콘솔을 동봉하지 않고, base 의 정적 해석(`console_static.resolve_console_static_dir`)은
`Console.StaticDir` → oam 동봉본 → 개발 트리 형제 `console/dist` 순서만 본다. 팩이 늘어도 번들 조합은 생기지 않는다.

- **표시는 설치된 서비스로 게이팅** — 위젯 카탈로그가 `requires_service` 로 하는 규칙을 **nav 섹션·라우트까지** 넓혔다.
  `RouteSection.requiresService`·`RouteDef.requiresService`(팩 id 가 아니라 **패키지 id** — `oam-svc`·`oam-cims-tester`)를 셸(`MenuContext.gateSectionsByService`)이
  `GET /api/v1/console/catalog` 의 `installed_services`(in-process 서비스 ∪ 게이트웨이 라우트 테이블 ∩ enabled, 서버 권위)로 게이팅한다.
  카탈로그를 못 받으면 게이팅 섹션은 숨긴다(미설치를 설치로 보이는 쪽이 더 나쁘다). 미설치 서비스의 메뉴는 없고, 설치되면 재로그인 없이 다음 조회에서 나타난다.
  CIMS 팩 섹션(`service`·`perf`·`config`)과 코어 섹션 안의 `/deploy/roles`(csc roles)는 `oam-svc`, 계측기 섹션은 `oam-cims-tester` 로 게이팅한다.
  부트스트랩 직후(base 만)는 코어 섹션(대시보드·시스템·릴리스·문서)만 보인다.
- **콘솔 버전 스큐** — 팩과 그 API 가 다른 패키지에 있으므로(번들은 oam, API 는 oam-svc/oam-cims-tester) 스큐가 생길 수 있다. D1 이 원래 감수한 것이고,
  서비스 모듈이 `requires_base_oam` 으로 최소 base(=번들) 버전을 고정하는 것으로 막는다. 반대 방향(오래된 서비스 + 새 번들)은 API 의 하위 호환(D6 Sunset 절차)이 담당한다.

> 채택하지 않은 안 — 해석 우선순위를 패키지 목록으로 일반화하고 서비스 모듈 패키지마다 full 번들을 동봉하는 방식은 팩 조합마다 번들을 만들어야 한다.

스택·규칙은 서비스 팩과 같다 — [ems/service/console/CLAUDE.md](../../../ems/service/console/CLAUDE.md)·[../console_design_system.md](../console_design_system.md).
자체 `package.json`·`node_modules` 없이 core 것을 공유하고(`ensure-svc-modules.mjs` 를 팩 목록으로 일반화), `@tester` alias 로 참조된다.

---

## 8. 배포·패키징

- `make dist` 대상에 `oam-cims-tester`(`ems/tester/oam`) · `cims-tester-worker`(`tester/worker`) 를 추가한다(`scripts/package.sh` targets). 상용 사이트
  **블루프린트에는 넣지 않는다** — 배포 여부는 블루프린트가 정한다. dist 아티팩트 수가 바뀌므로 S4/S6 manifest 불변성 게이트의 기대 목록을 같은 변경에서 갱신한다.
- 설치·기동·감독은 다른 모듈과 같다 — 콘솔 배포(agent job)로 설치, `agent/lib/lifecycle.sh` 에 `start_oam_cims_tester`(oam-svc 와 동일 골격 — `kill_stray` 는
  절대경로 `oam_cims_tester_app.py` 매칭)·`start_cims_tester_worker`(cmp 골격) 추가, `supervised.json` 등록, `--preflight` 지원, TLS 인증서는 `ensure_node_cert`.
- 기동 순서: base → `oam-cims-tester` → 워커(워커는 컨트롤러 없이도 떠서 `GET /health` 만 응답).
- 빌드: 루트 CMake `option(CIMS_TESTER ON)` 으로 `cims-tester-worker`(+`libcsim`)를 `build/bin/` 에.
- 파이썬 인터프리터 선택은 [os_portability.md](os_portability.md) 규칙(`--python` > 동봉 > `python3.14` > `python3`)을 따른다.
- 워커 호스트 = 시험 대상과 **다른** 호스트(N 대). 독립 형태의 컨트롤러 노드는 워커 중 한 대에 동거해도 된다(소규모).

---

## 9. cims-verify · cspsim · oam-svc 검증과의 관계

| 구분 | 역할 | 계측기 도입 후 |
|---|---|---|
| `cims-verify` S1~S6 | 상용 배포 게이트(정적·빌드·스모크·패키징·배포·통합) | 유지. S3/S6 의 **시나리오 항목**이 `cims.sh sim` 대신 `cims-tester run <scenario> --json` 을 호출하도록 단계적 이전(§10 E). 게이트 판정·불변성·보고서는 그대로 |
| `oam-svc` `verification`(`/release/verify`) | 게이트 실행·이력 콘솔 | 유지. 동거 형태에서는 같은 base 뒤에 있으므로 검증 결과에 계측기 run 링크(`/test/runs/<id>`)를 남긴다(교차 참조). 두 모듈 사이 코드 의존은 없다 |
| `cspsim` | 경량 UE·mock peer CLI | `libcsim` 위의 얇은 CLI 로 유지(기존 플래그 호환). 이전이 끝난 항목부터 의존 제거 |
| `cimsue-cli` | 실스택 UE | 계측기 `real-ue` 로 편입, 수동 부록에서 자동 표본 검사로 |
| `scripts/longrun_8h.sh` 등 소크 스크립트 | 야간 부하 | `soak` 프로파일로 대체 |

---

## 10. 이행 순서

각 단계는 그 단계만으로 쓸 수 있는 산출물을 낸다. 규모는 상대치(S/M/L).

| 단계 | 내용 | 산출물 · 완료 기준 | 규모 |
|---|---|---|---|
| **A. 계약 + base 확장** | 시나리오/프로파일 YAML 스키마, 워커 제어·관측 JSONL 스키마, 지표 정의표(§5), 목표 규모(§11) 확정. base 확장 셋 — ① 게이트웨이 SSE 통과(§6.2) ② 콘솔 번들 하나 + nav 섹션 서비스 게이팅(§7) ③ nav 그룹 `test` | 본 문서 갱신 + `ems/tester/oam/schema/*.json`(스키마 단위시험). base 확장은 기존 콘솔·oam-svc 동작 무변경으로 S3 게이트 PASS | M |
| **B. UE 축 + 컨트롤러 최소** | `libcsim` 추출(SimSession/RtpThread → 라이브러리, cspsim 은 그 위 CLI), `cims-tester-worker` ue 풀·단계 실행기·1초 집계 스트림, `oam-cims-tester` run/저장/CLI + pkg·config_template·self-register, 프로파일 constant·step | 부하 강화 문서의 시험(4 cps/HT20, 10 cps/HT5)을 `cims-tester run` 한 줄로 재현, RFC 6076 표 출력. 콘솔 배포로 설치된 워커 2대 분산 동작 | L |
| **C. 피어 축 — ibcf** | peer 엔진(고정 수신점·신원 범위·응답 정책·오류 주입) + `ibcf` 프로파일, 대상 CSP 컬렉션 시드/복원, 트렁크 in/out·route_set failover·ACL 시나리오 | `S6-SCN-IBCF-TRUNK` 동등 시나리오 PASS + 피어 다중화 failover 시험. CSP 미구현이 드러난 항목은 §12 표로 등재 | M |
| **D. 피어 축 — pbx · mgcf** | 트렁크 REGISTER, DID/내선, 183 early media·PRACK, hold/resume, REFER 발신, RFC 4733 DTMF, Q.850 Reason, G.711 | PBX 내선 ↔ CIMS 가입자 양방향 호, MGCF 경유 E.164 발착신 시나리오. 코덱 불일치(G.711↔AMR-WB) 결과를 §12 로 | M |
| **E. 콘솔 팩** | §7 화면 전부, SSE 라이브, 비교·보고서. cims-verify S3/S6 시나리오 항목의 `cims-tester` 호출 이전 | 콘솔에서 시나리오 편집→실행→보고서까지 완주. S3/S6 관련 항목 이전 후 게이트 PASS 유지 | L |
| **F. 확장** | MCData SDS/MSRP ue 단계, `real-ue` 편입, NAT(netns) 풀, MOS 추정, soak 프로파일 + 누수 판정, 대상 알람 타임라인 겹침 | 야간 소크 스크립트 대체 | M |

B 가 끝나면 성능 시험이, C·D 가 끝나면 피어 연동 기능 시험이 가능하다. E 는 B 와 병행 착수할 수 있다(API 계약이 A 에서 고정되므로).

---

## 11. 확정 항목

| 항목 | 상태 | 내용 |
|---|---|---|
| 모듈 이름 | **확정** | 컨트롤러 `oam-cims-tester`(`oam-svc` 는 그대로), 워커 `cims-tester-worker`, CLI `cims-tester`, 게이트웨이 세그먼트 `/api/v1/tester`, 콘솔 팩 alias `@tester`, nav 그룹 `test` |
| 운영 형태 | **확정** | 독립(계측기 자기 `oam`) / 동거(대상 `oam` 뒤) 두 가지, 블루프린트 차이만. 콘솔 인증 = base 콘솔 계정(로컬 계정 없음) |
| 포트 | **확정** | 컨트롤러 loopback 4490(oam-svc 4480·csc 4421 과 겹치지 않음), 워커 제어 7100 |
| 콘솔 번들 | **확정** | 번들 하나(`oam` 동봉) + nav 섹션·라우트 서비스 게이팅(§7). 서비스 모듈 패키지는 콘솔 미동봉 |
| 목표 규모 | 제안 | 등록 UE 5,000 · VoLTE 100 SApS × HT 20 s(동시 2,000) · PTT 그룹 200 × 20명 · 피어 트렁크 50 SApS. 워커 호스트 수는 B 단계 실측 후 확정 |
| 워커 호스트 | 제안 | 시험 대상과 분리된 최소 2대(8 코어) — 발생기 동거로 v1~v6 오진한 이력 |
| MGCF 범위 | **확정** | 평문 SIP(TS 29.163 Mg, TS 24.229) + Q.850 Reason + early media. **SIP-I 불필요** — CSCF↔MGCF(Mg)·IMS↔IMS NNI(GSMA IR.95)는 SIP/SDP 이고, SIP-I 는 CS 망 상호접속 트렁크(ITU-T Q.1912.5) 프로파일이다. 그런 트렁크를 받으려면 CIMS 자신이 MGCF(ISUP 해석) 역할을 해야 하는데 그것은 계측기가 아니라 CIMS 로드맵 문제다 |
| 트랜스코딩 | **확정** | MGCF 는 IM-MGW 가 AMR-WB 를 오퍼하므로 문제 없다. **IP-PBX 는 G.711 이 필수 코덱**(SIPconnect 2.0)이라 CIMS 쪽 트랜스코딩이 필요하다 → **CMP 과제로 채택**, 설계 정본 [../modules/cmp.md](../modules/cmp.md) §11(피어 leg 한정 G.711↔AMR-WB, TrGW 역할). 계측기 pbx 프로파일은 G.711 기본으로 그 경로를 시험한다 |
| 대상 관측 원천 | 제안 | 대상 OAM API + agent heartbeat 기본, ssh 샘플러는 옵션. OAM 이 없는 최소 배치도 시험 가능해야 |

---

## 12. 시험이 드러낼 CIMS 측 과제 (계측기 범위 밖, 병행 결정)

피어 축 시험은 현 CSP 피어링 구현의 미구현 항목([sip_service_model.md](sip_service_model.md) §9)과 바로 충돌한다.
계측기는 이를 **실패로 기록**하고, 채택 여부는 CIMS 로드맵에서 정한다.

| 항목 | 현 상태 | 시험에서 보이는 모습 |
|---|---|---|
| RouteSet 헬스체크(OPTIONS 프로브) | 미구현, `alive` 항상 true | 피어 1대 정지 시 failover 안 됨 |
| 트렁크 REGISTER(`register_to_remote`, 수신 측 트렁크 계정) | 미구현 | PBX 등록형 트렁크 시나리오 불가 |
| 헬스체크·THIG·`P-Asserted-Identity`·번호 정규화 | 없음 | ibcf 프로파일의 신원·프라이버시 검사 실패 |
| PRACK/100rel·183 early media 트렁크 전달 | 미확인 | mgcf 프로파일 링백 시나리오 |
| G.711 ↔ AMR-WB 트랜스코딩 | **채택** — [../modules/cmp.md](../modules/cmp.md) §11 설계, 구현 전 | 구현 전까지 pbx 프로파일 G.711 호는 488 또는 미디어 무음. 구현 뒤 = pbx 시나리오가 회귀 시험 |
| RFC 4028 세션 타이머 | 설계만([leg_liveness.md](leg_liveness.md)) | 피어 leg 유실 회수 시나리오 |
| `Reason: Q.850` 종료 사유 전달 | 없음 | 통계 실패 사유 분해 불일치 |
| `Setup.Roles.IBCF=false` 인데 피어 라우팅이 동작 | 가드 누락 | 역할 격리 시나리오 실패 |
