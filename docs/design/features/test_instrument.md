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

**libcsim 이 cspsim 위에 더한 것**(워커용, `cspsim/CsimObserver.h`·`SimSession`): ① 관측자 훅 `ICsimObserver`
(최초 REGISTER 응답+RRD · 착신 도착 · 발신 확립+SRD · 다이얼로그 종료 · 로컬 BYE 최종 응답+SDD — psip 스택 스레드에서 불리므로
워커는 큐에만 넣는다) ② 착신 응답 모드 `SetAnswerMode(E_ANSWER_DEFERRED)` — 180 만 내고 오퍼를 보관, 워커 스케줄러가 `after_ms` 뒤
`AnswerCall()`/`RejectCall(code)` 를 부른다(cspsim 의 auto 모드 = 180→1초 sleep→200 은 그대로) ③ RTP 수신 품질 —
시퀀스 공백 손실 누계·RFC 3550 A.8 지터(`CRtpThread::m_ullRecvLost`·`m_llRecvJitterUs`, 호마다 `ResetRecvStats`)
④ `Stop(iFlushMs)` — 수천 세션을 내릴 때 세션마다 300 ms 를 기다리지 않게 ⑤ **미디어 평면 제어**(`CRtpThread` — §4 미디어 평면):
RTP 모드 `SetMediaMode(AUTO|NONE|EXPLICIT)`, 송출 제어 `MediaSend(코덱별 파일, loop)`·`MediaSendDefault()`·`MediaStop()`, 상대 hold(a=sendonly/
inactive, RFC 3264 §8.4) 동안 송출 정지 `SetHoldPaused`, 송신 누계 `m_ullSentTotal`. 송신 스레드는 원천을 틱마다 다시 고르는 단일 루프 —
원천 = AMR-WB raw(61 B 프레임)·G.711 raw(160 B/20 ms) 파일 또는 합성(G.711 = 상수 PCM, **AMR-WB = RFC 4867 NO_DATA 프레임을 협상 PT 로**
— 세션 코덱과 다른 PT 를 내지 않는다). 파일 프레임은 프로세스 공유 캐시(호마다 다시 읽지 않는다). 정지 중에는 타임스탬프만 흐르고
시퀀스는 멈춰 수신 측 손실 계산에 공백이 없다. 워커가 지원하는 단계(B) = `register`(prelude)·
`invite`·`answer`·`reject`·`bye`·`media_hold`·`wait`·`expect`·`deregister`(epilogue). 나머지 단계는 run 시작 시 400 `unsupported_step`. `invite` 의 `expect.code` 가 300 이상이면 그 최종 응답이
성공 조건이다(ACL 403·라우팅 reject) — 워커는 발신자의 최종 응답을 기다려 코드가 다르면 실패로 센다.

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
`rules`/`routing_policies`(도메인·번호 prefix)·`acl_policies`. 계측기는 **토폴로지의 피어 풀 정의에서 이 레코드를 파생**해
run 전에 대상 OAM 의 컬렉션 API(`PUT /api/v1/deployments/{id}/collection/{name}`, SIGUSR1 reload)로 넣고, run 이 끝나면
저장해 둔 원본으로 되돌린다(`services/tester_target.py` — `S6-SEED` 의 `seed_ibcf_routing()` 을 일반화). 레코드 스키마는 CSP 의 것
그대로이고 계측기 번역 계층은 없다.

**피어 엔진 구현 반영**(C 단계 — `cspsim/CsimPeer.{h,cpp}`, 워커 `kind=peer` 풀):
- **엔진 = 스택 하나 + 다수 동시 호.** `CsimPeer` 는 psip `CSipUserAgent` 하나를 `bind` 수신점에 열고(REGISTER 없음), 호마다
  `CRtpThread` 를 따로 만들어 Call-ID 로 귀속한다. 관측자 `ICsimPeerObserver`(착신·확립·종료·BYE 응답)는 스택 스레드에서 불리며
  워커는 큐에만 넣는다. 응답(180/200/거절)은 워커 스케줄러가 정한다(`Ring/Answer/Reject/Bye`). `answer: silent` 면 착신에 아무 응답도
  내지 않는다(죽은 피어 — failover 시험).
- **신원.** 풀의 `identities.e164_range|did_range`(+`count`)를 컨트롤러가 펼쳐 워커에 `Identity(user, domain=풀 domain)` 로 보낸다.
  워커 Endpoint 하나 = 신원 하나(스택 없음, `callId` 로 엔진 호를 가리킴). 착신 INVITE 의 To user 가 범위 밖이면 404, 같은 신원의
  두 번째 호는 486. `ibcf` 프로파일은 착신에 `P-Asserted-Identity` 가 없으면 `pai_missing` 로 센다.
- **발신.** From = 자기 신원, Request-URI/To = `user@상대 도메인`(psip 기본 `user@접속IP` 를 `CreateCall` 뒤 고쳐 보낸다), 다음 홉 =
  풀이 가리킨 `peering` 노드의 수신점(`listener` — 비면 첫 `edge=peering` 항목, 없으면 bind 와 같은 protocol 의 첫 항목. access 수신점이라도
  된다 — CSP 는 접속점 edge 가 아니라 시드된 Route 로 피어를 신뢰한다). `ibcf` 는 `P-Charging-Vector`(icid-value·orig-ioi, TS 24.229 §7.2A.5)를 싣는다.
  UE 가 피어 신원을 부를 땐 `user@피어도메인` 을 다이얼한다 — `SimSession::StartCall` 도 `user@domain` 목적지를 정공법으로 낸다
  (Request-URI host = 도메인 → CSP `req_uri_host` 규칙).
- **코덱.** 프로파일 기본(ibcf/mgcf = AMR-WB,AMR,PCMU,PCMA · pbx = PCMA,PCMU) 또는 `codecs`. 착신 answer 는 오퍼와의 첫 공통 코덱
  (오퍼 PT echo, RFC 3264), 없거나 SAVP 오퍼면 488 — IP-PBX G.711 ↔ AMR-WB 불일치(cmp.md §11)가 여기서 드러난다.
- **워커 고정.** 피어 풀은 수신점이 하나이므로 워커 **하나**(`worker`)에 놓이고 수신점은 그 워커 호스트 주소 : `bind.port` 다. 피어를 쓰는 시나리오는
  그 워커에서만 돈다(인스턴스의 역할들이 한 워커에 있어야 하므로 — §4 워커 교집합 규칙).
- **시드 파생 규칙**(`tester_target.derive_records`, 태그 `cims-tester` 로 재실행 시 잔재 제거): 시나리오 **역할이 선언한** 피어 풀만
  (`seed.enabled`) — 같은 `route_set` 의 형제라도 선언하지 않으면 시드하지 않는다(워커가 열지 않은 피어를 CSP 가 고르면 호가 죽는다).
  풀 P 마다 `tester-rn-P`(remote_node — 주소는 `bind.ip` → 워커 호스트)·`tester-r-P`(route, `local_node_ref` = 그 풀이 가리킨 수신점의 LocalNode,
  **`inbound_auth: none`** — CSP 는 이 Route 로 식별한 요청을 Digest 없이 받는다)·`tester-rule-P-domain`(`req_uri_host eq`),
  `seed.route_set`(기본 = 풀 이름)마다 `tester-rs-<set>`(members = priority/weight, `health_check_mode: none`)·`tester-rs-<set>-match`(OR)·
  `tester-rp-<set>`(priority 50 → route_set, fail_action reject). `seed.acl: allow|deny` 면 `src_ip eq <피어 수신점 ip>` 규칙 + ACL 정책
  **scope=route(그 피어의 Route)** — CSP 가 (수신 접속점, 소스 주소) 로 인바운드 Route 를 식별하므로 같은 접속점의 UE 나 다른 피어에는 걸리지 않는다.
  접속점 = 수신점 항목의 `local_node` 이름이 대상에 있으면 그 레코드, 없으면 같은 protocol·port 의 기존 레코드(access 수신점을 가리킨 피어는 대개
  여기서 끝난다), 그것도 없으면 그 이름(비면 `cims-tester-<수신점 id>`)·그 edge 로 시드한다(CSP 가 SIGUSR1 로 포트를 연다, 복원 시 닫힌다).
  피어 풀마다 접속점이 다를 수 있다(풀별 `local_node_ref`).
- **대상 OAM.** `oam` 노드(url = `https://<host.ip>:<port>`) + `token_env`(환경변수의 로그인 토큰) + `csp_deployment_id`(비면 배포 목록에서 패키지 `csp`). 시드/복원
  결과는 run 노트(`csp seed(dep …): remote_nodes+1 …` / `csp seed restored`).
- **CSP 정합**: 피어 신뢰는 접속점 edge 가 아니라 **인바운드 Route 식별**에서 나온다 — CSP 가 (수신 LocalNode, 소스 IP[:UDP 포트]) 로 Route 를 찾고
  `inbound_auth=none` 이면 Digest 없이 받는다(TS 24.229 §5.10 IBCF, TS 29.165 II-NNI). `edge=peering` 접속점은 Route 없는 소스에 403,
  access 접속점은 낯선 소스를 가입자 인증 흐름으로 들인다([sip_service_model.md §4](sip_service_model.md)). psip UAC 는 등록 정보 없는
  401/407 을 재전송하지 않고 최종 실패로 넘긴다(전엔 INVITE↔401 무한 루프).

**pbx·mgcf 구현 반영**(D 단계 — 같은 `CsimPeer`/워커/컨트롤러 위에 얹은 능력, UE 측은 `SimSession` 에 대응 능력):
- **183 early media · 100rel/PRACK**(RFC 3262) — 단계 `progress`(who=피어) → `CsimPeer::Progress` = 183 + SDP answer(첫 공통 코덱, 링백 RTP
  송신 시작). 착신 INVITE 가 100rel 을 지원(Supported/Require)하고 풀 `prack` 이면 RSeq 를 싣고, 상대 PRACK 을 `prack_rx` 로 센다. 200 은 같은 answer 를 반복.
  UE 풀 `prack: true` 면 발신 INVITE 에 `Supported/Require: 100rel`, RSeq 있는 1xx 에 PRACK(`prack_tx`), 183 SDP 면 early media 수신 시작(`early_media`).
  피어 UAC(mgcf/ibcf 기본 `prack` 켬, pbx 끔)도 같다. 워커는 183 뒤 발신자의 1xx 도달(RING 이벤트)까지 기다린다 — CSP 가 183/SDP 를 전달하지 않으면 시한 실패.
- **hold/resume**(RFC 3264 §8.4) — 단계 `hold`/`resume`(who|from) → re-INVITE `a=sendonly` / `a=sendrecv`(psip `HoldCall/ResumeCall`), 최종 응답을
  `reinvite_ok/fail` 로 센다. 수신 re-INVITE 는 psip 이 200 으로 answer 하고 방향만 관측(`reinvite_rx`·`remote_hold`). UE·피어 양쪽 지원.
- **blind REFER**(RFC 3515) — 단계 `refer from: <전달자> to: <대상 역할>` → `Refer-To = 대상 신원`(psip `TransferCallBlind`), 최종 응답 `refer_codes.<n>`
  (기대 기본 202). CSP(B2BUA)가 REFER 를 종단해 대상에 INVITE 를 내고 전달자 leg 를 BYE 로 접는다 — 시나리오는 이어서 `answer who: [대상]`.
- **RFC 4733 DTMF** — `telephone-event` 를 오퍼(PT 101, 클록 = 첫 코덱 클록)하고 answer 는 오퍼 것을 echo(RFC 3264 §6.1). 협상 PT 는 `CRtpThread::m_iDtmfPt`
  로 — 송신 스레드가 20 ms 틱에 오디오 대신 이벤트 패킷(같은 SSRC/시퀀스, 이벤트 동안 타임스탬프 고정, 마커 첫 패킷, duration 누적, 종료 E 비트 3회)을
  내고, 수신은 E 비트 기준 이벤트 수·숫자열(지터 계산에서 제외). 단계 `dtmf from: … payload: "1234#"` → 숫자열 송신 후 다 나갈 때까지 대기,
  수신 수는 `bye` 에서 표본(`dtmf_tx`=단계 숫자 수·`dtmf_sent`=실제 송신 이벤트·`dtmf_rx`). 루프백 단위시험 `build/bin/csim_rtp_dtmf_test`.
- **Q.850 Reason**(RFC 3326) — `bye`/`reject` 의 `cause: <1..127>` → psip `StopCall(callId, code, "Q.850;cause=N")`(BYE·최종 응답·CANCEL 에 Reason). 수신은
  스택 콜백(UA 보다 먼저)에서 BYE/CANCEL/실패 최종 응답의 Reason 을 읽어 `OnCallEnd(…, q850)` 로 전달 → `q850_rx`·`q850.<cause>`. 비율 `q850_rx_pct` 가
  B2BUA 투과 여부를 말한다.
- **pbx 트렁크 REGISTER**(SIPconnect 2.0 §8 등록 모드) — 풀 `register: { user, ha1_env | password_env, realm?, expires }`. 컨트롤러가 환경변수를 풀어
  `PoolCreate.trunk_register` 로 내리고(없으면 컴파일 오류), 워커는 prelude 에서 **풀당 한 번** `CsimPeer::Register`(대상 access 접속점 — Digest 챌린지가 있는
  쪽, bind 와 같은 transport, psip 등록 스레드가 401 을 처리). 결과로 풀 신원 전부를 `registered` 로 표시(계정 하나가 DID 범위 대표). 시나리오
  `register who: [pbx]` 는 트렁크 계정이 있는 피어 풀만 허용(컴파일·워커 양쪽 검사). 등록 실패면 그 역할의 free 단말이 없어 run 이 곧 닫힌다.
- **다이얼·시드** — UE 가 피어 신원을 부르는 꼴은 프로파일이 정한다: ibcf = `user@피어도메인`(Request-URI host → `req_uri_host eq` 규칙), **pbx/mgcf =
  번호 그대로**(DID/E.164 — 실 단말이 다이얼하는 꼴). 시드 매칭 집합은 도메인 규칙 OR **번호 접두 규칙**(`req_uri_user prefix` = 신원 범위의 공통 접두,
  `tester-rule-<풀>-prefix`) — CSP 가 자기 도메인 Request-URI 의 번호 접두로 트렁크 RouteSet 을 고르는 것을 실측 확인(BGCF 식 번호 라우팅).
- **오퍼 코덱** — `invite` 의 `media.audio`(pcmu/pcma/amr-wb …)를 UE 오퍼 코덱으로 쓴다(`SimSession::SetOfferCodec`). 협상 코덱이 AMR-WB 가 아니면 파일
  미디어 대신 합성 PCMU(G.711 PT 로 스탬프) — pbx 상대 G.711 relay 경로와 AMR-WB 단독 오퍼(488, cmp.md §11 트랜스코딩 전) 를 시나리오가 고른다.
- **실측**(개발서버 CSP 0.2.126, 워커 동거): `TRUNK-PBX-OUTBOUND`(UE PCMU → PBX DID, 번호 prefix 라우팅) pass · `TRUNK-PBX-INBOUND` pass ·
  `TRUNK-PBX-HOLD-RESUME`(re-INVITE 2/2 200) pass · `TRUNK-PBX-TRANSFER`(REFER 202 → 대상 착신·전달자 BYE) pass · `TRUNK-PBX-DTMF` dtmf_rx 100 %
  (CMP 가 telephone-event PT 를 투과) · `TRUNK-MGCF-OUTBOUND` early_media 100 %·prack 100 %(CSP 가 183/SDP·PRACK 전달) · `TRUNK-MGCF-INBOUND` pass.
  **fail 로 남은 것 = CIMS 과제(§12)**: Reason Q.850 미투과(`q850_rx` 0 — DTMF/MGCF-OUTBOUND 의 `q850_rx_pct`), `TRUNK-MGCF-REJECT-Q850` 의 503 이 발신자에
  603 으로 도달, `TRUNK-PBX-REGISTER` 403(CSP 트렁크 계정 수신 미구현).

### 3.3 `real-ue`

`cimsue-cli --json` 을 워커가 스폰해 JSON 한 줄 결과(`rx_pkts`·`granted`·exit code 표)를 지표로 받는다.
같은 시나리오에서 `ue` 풀 100 + `real-ue` 2 처럼 섞어, 대량 부하 아래 실스택 단말의 품질을 표본 측정한다.

---

## 4. 시나리오 모델 — YAML 정본 (`ems/tester/oam/scenarios/`)

세 층으로 나눈다. **토폴로지**(어디에 무엇이) · **시나리오**(누가 무엇을 하고 무엇을 기대) · **부하 프로파일**(얼마나 빨리, 얼마나 오래).
기능 시험 = 시나리오 + 단발 실행, 성능 시험 = 같은 시나리오 + 부하 프로파일. 시나리오를 두 번 쓰지 않는다.
토폴로지는 콘솔에서 편집·저장(`modules/oam-cims-tester/runtime/topologies`), 시나리오·프로파일은 패키지 동봉 YAML + 운영자 추가분.

**토폴로지 모델 — 호스트 › 워커·대상 노드 › 풀.** 배치는 세 단이고 **주소의 기본값은 호스트**에서 온다 — 워커 URL·노드 API/OAM/DB 포트·피어
수신점 ip 는 호스트에서 파생되고, 대상 노드 `addr`(A/S 이중화의 VIP·다중 IP 호스트의 서비스 주소)·SIP 수신점 항목 `ip`·피어 `bind.ip` 로
덮어쓴다(사슬 `listener.ip → node.addr → host.ip`). SIP 노드의 수신점은 **N 개**(`sip.listeners{}` — CSP `local_nodes` 와 1:1, `edge`
access/peering). 시험 대상은 역할별 **노드 집합**이라 한 서버에 모인 CIMS 도, 노드마다 서버가 다른 타 IMS 도, IP-PBX 도 같은 레코드
구조다. 콘솔은 이 레코드를 드래그앤드롭 캔버스로 편집한다(§7).

```yaml
# topology.yaml — 대상과 자원
name: media01
hosts:                                   # 서버 — 주소·SSH 자격의 유일한 자리. 계측기/대상/동거 구분은 필드가 아니라 그 위에 무엇이 있느냐
  h45: { name: media01, ip: 10.0.0.45, ssh: { user: cims, key_env: TESTER_SSH_KEY } }
  h61: { name: tester-a, ip: 10.0.0.61 }
  h62: { name: tester-b, ip: 10.0.0.62 }
workers:                                 # 호스트 위의 cims-tester-worker 프로세스 — url = http://<host.ip>:<port> 파생. agent 인벤토리 자동 발견분에 보탠다
  - { name: w1, host: h61, port: 7100, cpus: 8 }
  - { name: w2, host: h62, port: 7100, cpus: 8 }
target:
  name: media01
  kind: cims                             # cims | ims | pbx — 라벨·기본값·컬렉션 시드 가능 여부(cims 만)
  nodes:                                 # 역할(role)별 노드 — 설정 블록이 역할마다 다르다. procs = 호스트 SSH 관측이 볼 프로세스 이름
    csp:                                 # addr: 10.0.0.50 — VIP·다중 IP 호스트의 서비스 주소(비면 호스트 ip)
      role: sip
      fn: CSP
      host: h45
      procs: [csp]
      sip:
        domains: [volte.cims.example.kr, ptt.cims.example.kr]
        listeners:                       # 수신점 N 개 = CSP local_nodes 와 1:1. ip 비면 노드 addr → 호스트 ip
          udp:     { edge: access, port: 5060, protocol: udp }
          tcp:     { edge: access, port: 25061, protocol: tcp }
          tls:     { edge: access, port: 5061, protocol: tls }
          peering: { edge: peering, port: 5070, protocol: udp, local_node: cims-tester-peering }   # 피어 풀의 기본 다음 홉
    cmp: { role: media, fn: CMP, host: h45, procs: [cmp], media: { rtp_range: [20000, 29999], control: 9001 } }
    csc: { role: subscriber, fn: CSC, host: h45, procs: [csc], api: { port: 4430, tls: true } }
    oam: { role: oam, host: h45, oam: { port: 4419, token_env: TESTER_OAM_TOKEN, csp_deployment_id: 34, observe: [oam_stats, oam_alarms, agent_heartbeat] } }
    db:  { role: db, fn: MariaDB, host: h45, db: { port: 3306, name: cims, user_env: TESTER_DB_USER, password_env: TESTER_DB_PASS } }
pools:                                   # 풀 하나 = 워커 하나. 어디에 닿는지는 노드 id 참조. 워커 여럿에 나누려면 워커마다 풀 + 같은 group
  volte_ue_a: { kind: ue, worker: w1, group: volte_ue, access: csp, listener: tls, source: { db: db, table: volte_subscriptions, offset: 0, count: 1000 }, srtp: optional }   # listener 를 주면 transport 는 그 protocol
  volte_ue_b: { kind: ue, worker: w2, group: volte_ue, access: csp, source: { db: db, table: volte_subscriptions, offset: 1000, count: 1000 }, transport: tls, srtp: optional }
  ptt_ue:   { kind: ue, worker: w1, access: csp, source: { creds: creds/ptt.jsonl }, transport: udp }
  peer_kt:  { kind: peer, worker: w2, peering: csp, profile: ibcf, bind: { port: 5080, protocol: udp }, domain: ims.kt.test,
              identities: { e164_range: ["+82212340000", "+82212349999"], count: 200 }, seed: { route_set: rs-kt, priority: 100 } }
  peer_kt_dead: { kind: peer, worker: w2, peering: csp, profile: ibcf, bind: { port: 5081, protocol: udp }, domain: ims.kt.test, answer: silent,
              identities: { e164_range: ["+82212340000", "+82212340009"] }, seed: { route_set: rs-kt, priority: 50 } }   # failover 상대
  peer_blocked: { kind: peer, worker: w2, peering: csp, profile: ibcf, bind: { port: 5082, protocol: udp }, domain: ims.blocked.test,
              identities: { e164_range: ["+82299990000", "+82299990009"] }, seed: { acl: deny } }                         # ACL 403 시험
  pbx_hq:   { kind: peer, worker: w1, peering: csp, profile: pbx, bind: { port: 5090, protocol: udp }, domain: pbx.hq.test, register: { user: pbx-hq, ha1_env: PBX_HA1 },
              identities: { did_range: ["0212345000", "0212345099"], ext_len: 4 } }
layout:                                  # UI 배치 상태 — 캔버스 영역(호스트) 좌표·크기, 카드(워커·노드) 좌표. 컴파일러는 읽지 않는다
  regions: { h45: { x: 420, y: 40, w: 990, h: 470 }, h61: { x: 30, y: 40, w: 360, h: 330 }, h62: { x: 30, y: 400, w: 360, h: 380 } }
  items: { csp: { x: 14, y: 12 }, cmp: { x: 290, y: 12 }, w1: { x: 14, y: 12 }, w2: { x: 14, y: 12 } }
```

- **노드 역할과 설정 블록.** `sip`(CSP · P/I/S-CSCF · SBC · IBCF · PBX) 는 수신점 목록 `sip.listeners{id: {edge, ip?, port, protocol, local_node?}}`
  를 가진다 — 항목 하나 = CSP LocalNode 하나. `edge=access` = UE 가 등록·발신하는 곳, `edge=peering` = 피어의 다음 홉(CSP 는 Route 있는 피어만
  받는다). `ip` 가 비면 노드 `addr` → 호스트 ip. `local_node` 는 cims 대상 `local_nodes` 이름(시드가 찾거나 만든다, 비면 `cims-tester-<id>`).
  `sip.domains` 의 첫 항목이 기본 홈 도메인, `ptt` 가 든 항목이 PTT 풀 도메인. 수신점이 없으면 관측 전용 노드(S-CSCF 처럼).
  이전 꼴 `sip.access{udp,tcp,tls,domains}`·`sip.peering{port,protocol,local_node}` 는 읽을 때 항목 `udp`/`tcp`/`tls`/`peering` 으로 승계한다
  (`tester_models.normalize_sip_block` — 모델 입력·store 읽기·저장 모두). `tas`(코어 내부, 참고 포트) · `media`(CMP · MRF · TrGW — `rtp_range` 가 있어야 미디어 leg 지표를 그 노드에 귀속) ·
  `subscriber`(CSC · HSS — `api` 가 있으면 DB 원천·연결 검사 대상, HSS 처럼 없으면 관측만) · `oam`(CIMS 전용 — 통계·알람 관측 + 컬렉션 시드,
  url = `https://<host.ip>:<port>`) · `db`(MariaDB — H(A1) 보유 가입자 원천, `creds-from-db` 와 같은 원천). 어느 노드든 `procs` 를 적으면 호스트의 `ssh`
  로 그 프로세스 CPU/메모리를 본다(`stop_on.target_cpu_pct` 원천). `fn`·`label` 은 표시용.
- **풀 → 수신점 참조.** `ue/real-ue.access` = access 수신점이 있는 노드 + `listener`(비면 `transport` 와 같은 protocol 의 첫 access 항목, 주면
  `transport` 는 그 protocol — 둘 다 주고 다르면 거절) · `peer.peering` = 수신점이 있는 SIP 노드 + `listener`(비면 첫 `edge=peering`, 없으면
  `bind.protocol` 과 같은 첫 항목 — access 항목도 된다) · `source.db` = `db` 노드 또는 `api` 있는 `subscriber` 노드. 워커에 내려가는
  `PoolCreate.target_csp`(§6.1) 는 컨트롤러가 이 참조에서 **파생**한다(ip = 그 수신점 주소, udp/tcp/tls = 같은 주소의 access 항목 포트,
  `peering` = 피어가 가리킨 수신점) — 워커 계약은 그대로다.
- **풀 → 워커 배치.** 풀 하나 = 워커 하나(`worker`). 피어 풀의 수신점은 `bind.ip`(비면 그 워커 호스트 주소) : `bind.port`/`bind.protocol`. 부하를 워커 여럿에 나누려면 **워커마다 풀을
  두고 같은 `group`(논리 풀 이름)을 준다** — 신원은 풀 정의가 나눈다(DB `offset/count` 또는 creds 파일). 시나리오의 `roles.X.pool` 은 풀 **이름 또는
  `group`** 과 맞으면 되고, 컴파일러는 워커마다 역할을 그 워커의 로컬 풀 하나로 해석한다(이름 또는 group 일치 — 워커당 같은 group 은 하나, group 은
  다른 풀 이름과 겹치지 않는다). **모든 역할이 해석되는 워커만 run 에 참여**하고, 그런 워커가 없으면 컴파일 오류. 워커 한 대만 쓰면 group 없이 이름만으로
  끝난다. 신원 범위를 워커에 나눠 보내는 컨트롤러 배분(§6.1)은 이 규칙으로 대체된다 — 워커에 내려가는 신원 = 그 워커 풀의 신원 전부.
- **호스트 성격은 파생.** 워커만 = 계측기, 대상 노드만 = 대상, 둘 다 = 동거(허용 — CPU 지표는 워커 몫을 뺀 값으로 본다), 없음 = 빈 호스트. 필드로
  두지 않는다.
- **`kind`.** `cims` 는 `oam` 노드가 있을 때 컬렉션 시드(§3.2)·`target_build` 를 쓴다. `ims`/`pbx` 는 시드 없음 — 피어 수신점으로의 라우팅은 대상 쪽에서
  미리 잡아 두고, 피어 풀 `seed.*` 는 경고.
- **컨트롤러 계약 = 이 모델.** `tester_models.Topology`(pydantic, `schema/topology.schema.json`) 가 참조 무결성을 검증한다 — 워커·노드의
  `host` ∈ hosts, 풀의 `worker` ∈ workers, `ue.access` 는 access 수신점 있는 노드(+`listener`/transport 일치), `peer.peering` 은 수신점 있는 SIP
  노드(+`listener` 존재), `source.db` 는 db 노드 또는 api 있는 subscriber 노드, `group` 은 다른 풀 이름과 겹치지 않고 워커당 하나, 수신점
  (ip:port/protocol — 노드 사이에도)·피어 수신점 유일, 역할과 다른 설정 블록 거절. 파생 조회는 모델 메서드(`worker_url`·`node_ip`·`listener_ip`·
  `pool_bind_ip`·`pool_listener(pool)`·`target_csp_for(pool)`·`peer_listeners`·`oam_ref`·`host_kind`) — 워커 계약 `PoolCreate.target_csp`
  (ip·access 포트·도메인·peering)와 `peer.bind.ip` 는 여기서 나온다. **이전 꼴 레코드**(`target.csp/csc/oam` · `workers[].url` ·
  `pools.*.bind.ip`)는 스토어가 읽을 때 기계적으로 승계한다(`tester_store.topology_v1_to_v2` — csp/csc/oam → 호스트+노드, url → 호스트+포트, bind.ip →
  그 주소의 워커, UE 풀은 첫 워커; 레코드에 `migrated_from: v1` 표시, 쓰기 가능하면 그 자리에서 저장). 콘솔 캔버스는 목업(`/test/topology-canvas`)에서
  정식 편집기로 바뀐다(§7).

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

- 단계(step) 어휘는 cspsim 시나리오 enum 을 **데이터로 옮긴 것**이다: `register/deregister/invite/progress/answer/reject/bye/hold/resume/dtmf/refer/
  replaces/join/pickup/subscribe/publish/group_call/floor_request/floor_release/sds_send/sds_recv/media_hold/wait/expect`. 새 단계 = 워커 재빌드, 새 시나리오 = YAML 만.
  피어 축 단계(§3.2 D): `progress`(who — 183 early media) · `hold`/`resume`(who|from — re-INVITE) · `dtmf`(from + `payload` 숫자열) · `refer`(from + to) ·
  `bye`/`reject` 의 `cause`(Reason Q.850). 미디어 평면 단계: `media_send`(who + `sample`·`loop`·`after_ms`) · `media_stop`(who).
  워커 지원 = `register/invite/progress/answer/reject/bye/hold/resume/dtmf/refer/media_hold/media_send/media_stop/wait/expect/deregister`.
- **실행 의미(워커)** — 흐름을 셋으로 나눈다. **prelude** = 앞쪽의 `register`(+`wait`) 단계: 역할 슬라이스의 단말 **전부**를 run 시작 때 한 번
  등록한다(`Timers.RegisterIntervalMs` 간격, 이미 등록된 단말은 재사용). **body** = 나머지: **시나리오 인스턴스** 하나가 실행하는 단위 — 인스턴스는
  `rate_saps` 로 발생하고(토큰 버킷), 역할마다 free 단말을 하나씩 잡아 단계를 차례로 실행한 뒤 돌려준다. free 단말이 모자라면 그 슬롯은 `skipped`
  로 센다(Little 의 법칙: 동시 인스턴스 ≈ SApS × SDT — 역할당 단말 수가 그보다 커야 한다). **epilogue** = 끝의 `deregister`: run 종료 시 단말 정지.
  body 안의 `register/deregister` 는 거절한다. `invite` 는 비동기(다음 단계로 바로 진행), `answer/reject` 는 착신 도착을 기다렸다가 `after_ms` 뒤
  응답하고 발신자 확립(또는 최종 응답)까지 기다린다, `media_hold` 는 확립을 기다린 뒤 `seconds` 유지하고 끝에 RTP 품질 표본을 뜬다,
  `bye` 는 BYE 최종 응답(SDD)까지 기다린다. 어느 대기든 시한(`Timers.InviteTimeoutMs`·`ByeTimeoutMs`)을 넘기면 인스턴스 실패 + event.
- **역할의 풀** — `roles.X.pool` 은 토폴로지 풀 **이름 또는 `group`**(논리 풀 이름, §4). 워커마다 그 워커의 로컬 풀 하나로 해석된다
  (`tester_compile.resolve_roles`). 워커가 지원하지 않는 단계(`WORKER_STEPS` 밖)와 행위자 kind 게이트 위반(`STEP_VOCAB.kind` — progress/refer 는
  피어, PTT 단계는 UE)은 컴파일 오류다.
- **`media_hold.during`** — 유지 구간 안 시각 지정 동작 `[{at_s, step: dtmf|hold|resume|refer, from|who, to, payload, expect}]`. 컴파일러가
  `media_hold at_s → 동작 → media_hold 나머지` 로 풀어 워커에 보낸다(워커 계약 무변경). 기대치는 마지막 조각에 붙고, 풀린 조각은 `src` 로 원
  단계 인덱스를 가리킨다(계획 미리보기·절차표는 원 단계 기준). during 에 둘 수 있는 것(`DURING_STEPS`) = 통화 중 동작 + 송출 제어
  (`media_send`/`media_stop` — `who`·`sample`·`loop`).
- **미디어 평면** — 호마다 RTP 를 어떻게 다룰지는 `invite.media.rtp` 가 정한다(그 인스턴스의 모든 단말 — 전달 대상 포함 — 에 같은 모드).

  | `media.rtp` | 뜻 | 쓰임 |
  |---|---|---|
  | `auto`(기본) | SDP 가 오가면(183+SDP 또는 200) 곧바로 기본 원천으로 송출·수신 | 일반 호 |
  | `none` | **시그널링 전용** — SDP 는 오퍼/answer 하되 RTP 스레드를 띄우지 않는다. RTP 표본 없음 | 제어평면만 높은 율로(워커 RTP 부하 없이 SApS 상한) |
  | `explicit` | 수신만 시작하고 송출은 `media_send` 가 부를 때 | early media(단말은 early dialog 에서 송출하지 않는다 — TS 24.628)·단방향 경로·무음 구간 |

  `media_send {who, sample?, loop?, after_ms?}` = 그 역할의 송출 시작(원천 교체 포함) — `sample` 생략 = 풀 기본 원천(워커 `Media.AudioFile` 또는 합성),
  `loop: false` = 샘플 끝에서 정지. `media_stop {who}` = 송출 정지(수신은 계속). 둘 다 **그 호에서 SDP 가 오간 뒤**(`progress` 또는 `answer` 뒤,
  `bye` 앞)에만 둘 수 있고 `rtp: none` 호에는 못 둔다 — 시나리오 검증 오류. `auto` 호에서도 쓸 수 있다(원천 교체·일시 정지). 비동기 단계
  (`after_ms` 뒤 실행하고 곧바로 다음 단계). SDP 교환 전(RTP 미기동)에 닿으면 인스턴스 실패 + event.
  **샘플 라이브러리** = 토폴로지 `media.samples{id: {amr-wb|pcmu|pcma: 파일 | synthetic}}` — 파일은 워커 샘플 디렉터리(`Media.SampleDir`) 안의
  상대 경로(절대 경로·`..` 거절), raw 형식(amr-wb = 61 B 프레임 23.85 kbps · pcmu/pcma = 160 B/20 ms). 송신기는 **합의 코덱**의 항목을 쓰고,
  항목이 없거나 `synthetic` 이면 그 코덱의 합성을 낸다. 컨트롤러는 시나리오가 참조한 샘플만 발췌해 워커에 보낸다(`RunStart.samples`);
  라이브러리에 없는 id 는 컴파일 오류. 워커 패키지는 G.711 샘플 `ringback_kr`(440+480 Hz, 1 s on / 2 s off)·`tone_1k` 를 동봉한다
  (`tester/worker/samples/gen_tones.py` 산출물 — 인코더는 레포 `cspsim/G711.cpp` 와 전 구간 대조). AMR-WB 샘플은 운영자가 넣는다.
  `workers[].media{samples[], max_rtp_streams}` = 그 워커가 보유한다고 선언한 샘플 id(비면 전부)·RTP 동시 상한 선언.
  동봉 예 = `VOLTE-CALL-SIGNALING`(none) · `VOLTE-CALL-ONEWAY-MEDIA`(explicit + during `media_stop`) · `TRUNK-MGCF-EARLY-MEDIA`(183 뒤 링백 샘플).
- **역할의 신원 창** — `count` 생략 = 풀 전체. 단, 같은 풀에서 서로 `disjoint_from` 인 역할들이 `count` 없이 있으면 풀을 **균등 분할**한다
  (caller/callee 가 한 풀을 나눠 쓰는 흔한 꼴). 명시 `count` 는 먼저 빼고 나머지를 나눈다. 창이 비면 컴파일 오류.
- **단발(기능) 실행** = 프로파일 없이 `POST /runs {instances: N}` — 워커가 N 개(워커 간 배분)를 발생시키고 다 끝나면 스스로 run 을 닫는다
  (`RunStart.max_instances`). 성능 실행 = 프로파일 결합. 요청 모델은 `run_request` 스키마.
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
| 미디어 | 손실 % · 지터 ms · 단방향 무음 · MOS 추정 | RTP seq/timestamp(RFC 3550), RTCP 수신 시 상대 보고, G.107 E-model R→MOS(선택). 카운터 `rtp_tx`·`rtp_rx`·`rtp_lost`·`rtp_silent_legs`(표본 시점에 수신 0 인 단말)·`media_send`·`media_stop`·`skipped_rtp_cap`, 게이지 `rtp_streams`. 표본은 `media_hold` 를 지난 인스턴스의 `bye` 진입 때(`rtp: none` 호는 뜨지 않는다). 워커 디버그 로그에 단말별 `rtp sample <역할>: tx rx lost jitter` |
| 피어 트렁크 | `early_media_pct` · `early_rtp_pct` · `prack_pct` · `dtmf_rx_pct` · `q850_rx_pct` · re-INVITE/REFER 코드 | 발신기 관측 비율(`RATIO_METRICS` — 분자/분모 카운터 정의 단일): 183+SDP 도달/183 송신, **200 전에 RTP(≥ 5 패킷)를 받은 발신자/183 송신**, PRACK 수신/신뢰 183, DTMF 수신 이벤트/송신 숫자, Reason 수신/송신. B2BUA 투과 여부를 말한다. `early_media_pct` 는 시그널링(183 의 SDP 가 발신자에 닿았는가), `early_rtp_pct` 는 미디어 평면(링백 RTP 가 실제로 닿았는가 — 미디어 앵커가 18x SDP 를 반영해야 100 %)이다 |
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
- **제어(컨트롤러 → 워커)** HTTP/JSON: `POST /pools`(풀 생성·신원 적재 — `worker_pool_create`, 같은 이름은 교체), `DELETE /pools/{name}`,
  `POST /runs`(컴파일된 단계 + 역할 배분 — `worker_run_start`; `max_instances` 있으면 단발), `POST /runs/{id}/rate`(SApS 변경),
  `POST /runs/{id}/stop {drain_s}`, `GET /runs/{id}`(상태·누계 스냅샷), `GET /health`(용량·CPU·활성 엔드포인트·시각 — `worker_health`).
  워커는 시나리오 YAML 을 모르고 **컴파일된 단계 목록**만 받는다. 워커당 run 은 하나(409 `run_active`).
  미디어 평면(§4): 단계의 `media.rtp`·`sample`·`loop` 와 `RunStart.samples{id: {코덱: 파일|synthetic}}` — 워커는 run 시작 때 파일을 확인한다
  (400 `sample_missing`·`sample_unknown`·`sample_codec_unsupported`). `GET /health` 의 `media{rtp_streams, max_rtp_streams, sample_dir, files[]}` 로
  컨트롤러 계획 미리보기가 샘플 파일 보유·RTP 상한을 검산한다. 워커 설정 `Media.SampleDir`(상대 경로는 모듈 디렉터리 기준, 기본 `samples`)·
  `Media.MaxRtpStreams`(0 = 제한 없음 — 넘는 인스턴스 슬롯은 `skipped` + `skipped_rtp_cap`).
  `kind=peer` 풀은 `PoolCreate.peer`(토폴로지 PeerPool 그대로)로 엔진을 만들고 생성 즉시 bind 한다(실패 400 `peer_bind_failed`);
  `target_csp.peering` 이 발신 다음 홉이다(컨트롤러가 풀이 가리킨 수신점에서 채운다 — edge 무관).
  신원 `Identity.auth_id` 는 IMPI 사용자부 — `@` 가 없으면 워커가 `domain` 을 붙인다(cspsim `-creds` authId 규약; CSP 는 `authId@domain` 을 기대한다).
  컨트롤러는 각 워커에 **그 워커 풀의 신원**을 보낸다(`role_slices` 는 워커 로컬 인덱스 — `disjoint_from` 창은 풀 안에서 나눈다). 워커 사이의 신원 분할은
  컨트롤러가 하지 않고 풀 정의(워커마다 풀 + 같은 `group`, §4)가 한다.
- **관측(워커 → 컨트롤러)** 지속 TCP JSONL(컨트롤러 `Tester.WorkerStreamPort` 7110) 한 줄 = 한 레코드: `hello` → `agg`(1초 집계 — `counters`(attempt/session/leg·응답 코드·RTP 카운터) · `gauges`(동시 세션·등록 수·CPU) · `timers`(`rrd_ms`·`srd_ms`·`sdd_ms`·`jitter_ms`·`floor_grant_ms` 히스토그램)) · `event`(실패 개별 건 — Call-ID·역할·단계·코드) · `log`. UDP 는 부하 중 유실되어 지표를 왜곡하므로 쓰지 않는다.
- 워커 용량 선언: 기동 시 측정한 `max_endpoints`·`max_saps`(cspsim 실측 기준 코어당 UA 약 200, RTP 포함). 컨트롤러는 풀을 워커에
  나눠 배분하고 부족하면 시작 전에 거절한다.
- 시계: 워커·컨트롤러 NTP 정렬을 `GET /health` 에서 확인, 오차 > 50 ms 면 경고(run 노트).
- 스트림 목적지(`RunStart.stream`) = 컨트롤러가 **그 워커로 갈 때 쓰는 로컬 IP**:`Tester.WorkerStreamPort`(워커 관점 도달 주소).
  NAT 등으로 다르면 `Tester.WorkerStreamAdvertiseIp` 로 고정한다. 워커는 끊기면 지수 backoff 재접속, 그동안 레코드는 큐(상한 2만)에 둔다.
- 컨트롤러 저장: 1초 버킷은 `runs/<id>/metrics.sqlite`(`agg(t, worker, counters, gauges, timers)`), 실패 건은 `events.jsonl`, 요약·판정·기대치 결과·
  단계 로그는 `run.json`. 백분위는 워커 히스토그램(로그 상한 버킷 1·2·5·…·60000)에서 **상한값**으로 근사한다 — 기대치 판정은 보수적이다.
  verdict: pass = 기대치 전부 만족 ∧ 실패 인스턴스 0 ∧ 시도 ≥ 1 · fail · aborted(운영자 중단) · error(컴파일/워커 오류).
- 관리 store 리스: 컨트롤러는 기동 시 자기 서브트리 `modules/oam-cims-tester/runtime` 에 소유권 리스(flock)를 잡는다 — base `oam` 이 잡는 루트와
  별개(I5 단일 소유, oam_ha §4.4 단일 writer). 못 잡으면 read-only 로 떠서 토폴로지 저장·run 색인이 `not_lease_owner` 로 거절된다.
- 워커 발견: 컨트롤러는 자기 base 의 배포 목록(`GET /api/v1/deployments`, 패키지 `cims-tester-worker`)에서 워커 주소를 자동 수집한다.
  토폴로지의 `workers` 항목(호스트 참조 + 포트)은 이를 덮어쓰거나 보탠다(agent 없는 호스트).

### 6.2 컨트롤러 ↔ base OAM (서비스 모듈 규약)

[oam_base_service_split.md](oam_base_service_split.md) 의 규약을 그대로 따른다. 계측기 고유 사항만 적는다.

| 항목 | 내용 |
|---|---|
| 게이트웨이 세그먼트 | `/api/v1/tester` **하나**(D2 — 서비스 = 최상위 세그먼트 하나). `pkg.json` `gateway.routes=["/api/v1/tester"]`, `gateway.default_port=4490`. 하위: `/health`·`/schema/{name}`·`/validate`·`/scenarios`·`/profiles`·`/topologies`·`/runs`(GET 색인+라이브 — 필터 `?scenario=&build=&verdict=a,b&since=ISO\|7d&profile=&label=&topology=&load=1` / POST `run_request` → 202 id, 동시에 하나)·`/runs/{id}`·`/runs/{id}/stop`·`/runs/{id}/rate`·`/runs/{id}/hold {hold}`(단계 고정 — 프로파일 시계 정지, 율 유지)·`/runs/{id}/stream`(SSE — 그 run 의 agg/events/runs 프레임)·`/runs/{id}/report`(run.json + Markdown)·`/runs/{id}/events`·`/runs/{id}/series`(metrics.sqlite → 초 단위 열 형태 시계열 — 결과 차트)·`/runs/{id}/hist?timer=`(전 구간 버킷 분포 + p50/p95/p99 — 행 펼침 히스토그램)·`/runs/{id}/sip/{call_id}`(그 Call-ID 의 실패 이벤트 + 덤프 `runs/<id>/sip/<call_id>.log` 가 있을 때 — 워커 SIP 덤프 이전은 후속)·`/runs/{id}/target-alerts`(대상 OAM `/alerts` 를 run 창으로 잘라 — oam 노드 필요)·`DELETE /runs/{id}`(진행 중 409)·`/runs/compare?ids=a,b[,c][&format=md\|csv]`(첫 id 기준 지표별 delta·회귀 — 비율 지표 0.5 pt / 그 외 5 % 허용, `target_build` 병기; format 은 텍스트)·`/workers[?topology=]`(토폴로지 워커 + health, 병렬 probe)·`/events`(SSE — run/워커 상태 변화). **계획 미리보기** = `POST /runs/plan` 과 `POST /scenarios/compile-check`(같은 함수 `services/tester_plan.build_plan` — compile_run 드라이런; 입력 `{scenario_id\|doc\|yaml, topology_id\|topology, profile?, bindings?, instances?, rate_saps?, probe?}`, 출력 `{ok, errors, warnings, notes, roles(역할→요청 풀·kind·워커별 창·total), workers(배분·율·인스턴스·용량·health), steps(컴파일·src), phases, procedure(절차표), seed(예정 컬렉션), env(필요 환경변수), little(SDT·율별 필요 신원·부족 시작 율·권고 max), estimate(예상 소요·피크·동시)}` + 알려진 CSP 과제 §12 경고). `GET /scenarios/vocab` = 단계 어휘 표(`STEP_VOCAB` — group·actor·kind 게이트·제안 지표·워커 지원)·지표 라벨·비율 정의·Q.850·코덱·enum 들 — 편집기 팔레트/폼의 정본. 시나리오·프로파일은 `GET /{id}`(doc + YAML 원문) · `PUT /{id} {yaml}`(운영자본 저장 — `Tester.DataDir/scenarios/`, 검증 통과분만, 문서 id = 경로 id) · `DELETE /{id}`(운영자본만, 동봉본 409 `bundled_read_only`) · `POST /validate {kind, yaml|doc}`(저장 없는 검증 — 편집기가 타이핑 중 호출). 토폴로지 `POST /topologies/{id}/check` = 연결 검사(항목 `<노드>:udp\|tcp\|tls\|peering\|api\|db\|oam` · `<호스트>:ssh` · `worker_<이름>`, 각 항목에 `target{kind,id}` — 캔버스가 카드에 붙인다; `services/tester_check.py`). 모듈은 `/api/v1/api-docs` 로 자기 API 를 기술(`TESTER_API_DOCS`)하고 base 가 업스트림에서 수집한다([api_docs.md](api_docs.md)) — 콘솔 라우트 `apis` 가 그 id 를 참조 |
| 인증·RBAC | base 가 배포 시 `CimsAuth.JwtSecret` 주입(`meta.gateway.routes` 보유 모듈 자동), 모듈이 토큰 독립 검증. 권한 = 조회 `monitor`, run 실행·중단·토폴로지 편집 `operator`, 시나리오/프로파일 삭제 `manager` |
| 설정 | `config_template.json` 선언 키만(§14.7 write 마스크). `Server.Ip/Port`(loopback 4490)·`Tester.DataDir`·`Tester.RunRetainDays`·`Tester.WorkerControlPort`(7100)·`Tester.WorkerStreamIp/Port`(7110 — 워커 관측 수신, 관리망 bind)·`Tester.WorkerStreamAdvertiseIp`(선택). 대상(SUT)·워커·풀은 설정이 아니라 **토폴로지 레코드**(런타임 store, 콘솔 편집)다. `CimsAuth.JwtSecret`·`Mgmt.Cidr`·`CimsRuntimeDir` 은 **선언하지 않는다**(base 주입 파생값) |
| 프로파일 구동 | 오케스트레이터 스레드가 프로파일을 시간축으로 만든다 — `constant/soak` = rate 로 duration · `step` = start 부터 hold_s 마다 창 IHS(실패+건너뜀 / 시도)를 보고 임계 이내면 +step(max 까지), 초과면 중단하고 직전 단계가 **DOC** · `ramp` = 5 초마다 선형 증가 뒤 hold · `burst` = burst_interval 마다 1 초 burst_size. `stop_on.csp_5xx_pct`·`ser_pct_min` 은 최근 60 초 창(시도 ≥ 10)으로 판정해 fail 중단, `target_cpu_pct` 는 대상 관측(C 단계) 전까지 미적용(노트). 중단은 워커 `stop {drain_s = 5 + ht}` |
| CLI | `cims-tester run <scenario> --topology <name\|id> [--load <profile>] [--ht N] [--bind k=v] [--instances N] [--rate R] [--no-wait] [--json]` — 완주까지 기다려 RFC 6076 표를 찍고 verdict 로 종료 코드(pass=0). `plan <scenario> --topology …`(같은 인자 — 계획 미리보기, ok 면 0)·`report <id>`·`hist <id> --timer`·`stop <id>`·`rate <id> <saps>`·`workers`. `creds-from-db --csp-json <csp.json> --domain <sip domain> --out <jsonl>` = DB 의 ha1 보유 가입자로 creds JSONL 생성(토폴로지 `source.creds`) |
| 스토어 | `modules/oam-cims-tester/runtime/{topologies,runs}` 단일 소유(I5). run 본체는 `Tester.DataDir`. run 색인 `target_build` = 대상 OAM 이 있으면 CSP 배포 패키지 버전(`csp <ver> (dep N)`, `tester_target.csp_build`) — 비교 화면의 회귀 축 |
| 버전 계약 | `pkg.json` `gateway.requires_base_oam` — SSE 통과(아래)를 가진 base 최소 버전. base 는 self-register 시 라우트 레코드에 기록하고 자기 버전이 낮으면 경고 로그(등록은 한다 — 거부하면 콘솔에서 원인이 보이지 않는다) |
| **base 확장 ① — SSE 통과** | 게이트웨이 프록시(`handlers/gateway.py`)는 요청 `Accept: text/event-stream` 이면 총 타임아웃 없이(연결 5 s) 업스트림을 부르고, 응답 `Content-Type: text/event-stream` 이면 **청크 passthrough**(전체 버퍼링 없음, 클라이언트 절단·업스트림 종료 어느 쪽이든 응답 해제)한다. 판정은 라우트 속성이 아니라 응답 타입 — 어느 서비스 모듈이든 SSE 를 낼 수 있다. 그 외 응답은 종전대로 5 s(다운로드 120 s) 버퍼링 |

---

## 7. 콘솔 팩 (`ems/tester/console`) — 화면

| 화면(라우트) | 내용 |
|---|---|
| 토폴로지 `/test/topologies` | **왼쪽 레코드 레일**(`components/ListRail` — 결과 화면 run 레일과 같은 꼴: 검색(이름·대상·주소)·kind 칩·행 = 이름/kind/접속점/워커 수/갱신, [새 토폴로지…] 프리셋 선택, 접기 기억) + 툴바(#id·이름·대상 배지·검증 배지·자동 배치·되돌리기·연결 검사·저장/생성/삭제, v1 승계 배지) + **캔버스 편집기**(`components/topology/TopologyCanvas`, 모델 `lib/topology-model.ts`). ① **팔레트**(왼쪽, 카테고리 접기: 서버(호스트·SSH 관측) · 계측기 워커 · 풀(UE·실단말·IBCF·PBX·MGCF) · 시험 대상 노드 6 역할 · 프리셋 3종 CIMS 한 호스트 5 노드 / 일반 IMS 분리 배치 / IP-PBX — 워커가 있는 호스트·워커는 유지) ② **캔버스** — 호스트 = 옅은 색 영역(`--chart-N` 6색, 이름 앞 색점, 성격 배지 계측기/대상/동거 파생), 자유 배치·모서리 크기 조절·**내용물에 맞춰 자동 확장**(렌더 뒤 DOM 측정); 워커·대상 노드 = 영역 안 자유 배치 카드(영역 사이로 끌면 호스트 변경, 밖에 두면 빨간 점선 고아); 풀 = 워커 카드 안(워커 사이로 끌면 이동, `group` 칩); **빈 곳에 놓으면 상자 자동 생성**(풀 → 호스트+워커, 워커·노드 → 호스트) ③ **선은 모델에서 파생**(SVG 오버레이) — UE 풀 → 그 수신점 행(transport 색·srtp 라벨) · 피어 풀 → 그 수신점 행(route_set·priority 또는 ACL 라벨, 타 IMS 는 `→ 노드`) · 트렁크 REGISTER 점선(같은 transport access 행) · RTP 점선 → 미디어 노드 · DB 원천 점선; SIP 노드 카드는 **수신점 행을 N 개**(`피어링|SIP <proto>` · `[ip:]port · id`, ip 는 노드 주소와 다를 때만) 그리고, 풀 카드의 오른쪽 **앵커 점을 끌어 수신점 행**에 놓으면 그 수신점이 접속점(UE — access 행만)·다음 홉(피어 — 어느 행이든, access 행이면 토스트로 알림)·신원 원천(DB/API 행)이 된다(끄는 동안 점선이 따라오고 놓을 수 있는 행은 초록) · 풀 카드를 SIP 노드 위에 놓으면 `listener` 지정을 풀고 기본 규칙으로(수신점 없는 노드는 거절 토스트) ④ **속성 패널**(오른쪽, 종류별 폼 — 호스트 id/이름/주소/SSH · 워커 이름/호스트/포트/cpus/health(RTP 사용/상한·샘플 디렉터리·실제 파일 목록)/보유 샘플 id 선언·`max_rtp_streams` · **토폴로지(선택 없음) — 대상 이름/kind + 미디어 샘플 라이브러리**(id·코덱별 파일|synthetic, 추가·삭제·id 바꾸기 — §4 미디어 평면) · 노드 id/라벨/fn/호스트/**주소 addr(VIP)**/**수신점 표(id·edge·ip·port·proto·local_node, + access/+ peering 추가·삭제·id 바꾸기는 풀 참조 동시 갱신)**/도메인/RTP/API/OAM/DB/감시 프로세스 · 풀 이름/워커/group/접속점 노드·수신점·transport(수신점을 고르면 잠김)·srtp/원천 DB|creds/등록 옵션 또는 프로파일/응답/도메인/bind(ip·port·proto)/다음 홉 노드·수신점/신원/코덱/시드/트렁크 REGISTER; 이름 바꾸기는 참조 동시 갱신) ⑤ **하단 드로어** = 검증(오류·경고·참고, 항목 클릭 → 카드 선택, 오류 있으면 저장 잠금 — 컨트롤러 `Topology` 모델과 같은 규칙) · 레코드 JSON(읽기/적용) · 연결 검사(항목 `<노드>:<수신점>`·`<호스트>:ssh`·`worker_<이름>`, 결과가 카드의 같은 행에 OK/실패/참고·ms 로 붙고 실패 선은 붉게, 행 클릭 → 카드) ⑥ 자동 배치·되돌리기(저장 시점) + **편집 이력** `Ctrl+Z`/`Ctrl+Y`(`lib/use-history.ts` — 끌기 한 묶음 = 한 걸음, 영역 자동 확장은 이력 밖)·`Del`·`Ctrl+D` 풀/노드 복제(이름 `_2`·피어 bind 포트 비킴)·`Esc` · **줌** = `Ctrl+휠`·버튼·화면 맞춤(스테이지 `transform` 하나, 포인터 좌표는 zoom 으로 나눔) · 팔레트 접기 · 팔레트 **클릭 = 선택한 호스트/워커 위에 추가**(끌기 없이) · 선 라벨은 같은 수신점·같은 라벨이면 한 번만, 다른 라벨은 세로로 비킴(선택한 풀의 선은 항상 라벨) · 선택한 풀은 앵커를 키우고 "점을 끌어 수신점 행에 연결" 힌트 · 드로어 높이 손잡이(`lib/use-drawer-height.ts`) · 검증 목록 심각도 정렬 · 연결 검사 결과는 편집 뒤 흐리게(저장 전 결과 표시) · 테마 대응. 툴바 = 대상 이름·kind 배지, [새 토폴로지…] 는 프리셋 선택, 비활성 버튼은 사유 병기(저장 뒤 검사 / 오류 n 해결 뒤 / 변경 없음), 변경 중 레코드 전환은 확인·탭 닫기는 경고. 카드 위치·영역 크기는 레코드 `layout` 에 저장(§4). 이전 꼴 레코드는 컨트롤러가 읽을 때 승계한다 |
| 시나리오 `/test/scenarios?id=` | 탭 둘 — **시나리오** = **왼쪽 레일**(`ListRail` — 검색(id·제목)·태그 칩·출처 그룹(운영자본/동봉)·행 = id/오류 수/단계 수/제목/태그, 검증 오류 파일도 열림, 접기 기억) + 툴바(템플릿에서 새로·**문서 검증 배지**(토폴로지 없이 = 저장 게이트)·`Ctrl+Z`/`Ctrl+Y` 편집 이력·되돌리기·저장(운영자본 — 동봉본이면 override)·삭제·[단발 실행]·[부하 실행…](미리보기 프로파일이 기본값) — 변경이 있으면 **[저장하고 … 실행]** 이 저장 → 실행 창을 이어 연다(새 문서는 id 가 생겨 다시 마운트돼도 이어짐)·비활성 사유 병기. 변경 중 다른 시나리오 선택은 확인·탭 닫기는 경고) + **시퀀스 캔버스 편집기**(`components/scenario/ScenarioCanvas`, 모델 `lib/scenario-model.ts`, 어휘 = `GET /scenarios/vocab`). ① **팔레트**(역할 UE/피어 · 단계를 vocab group 별로 접기, 워커 미지원 단계는 회색·드롭 불가, 클릭 = 선택 단계 뒤 삽입) ② **시퀀스 캔버스** — **레인 = 역할**(헤더: 이름·count·`disjoint_from` ⟷·kind 배지 ue/ibcf/pbx/mgcf·기준 토폴로지 해석 줄 pool → 풀[+풀]·group 표시, 드래그 재배열), **행 = 단계**(번호·body 누적 시각 `t+`·종류·요약·레인 위 점/화살표(from→to, 라벨 = 코덱/코드/after_ms/Q.850)·`media_hold`/`wait` 는 전폭 띠·기대치 칩·× 삭제), **구간 띠 자동**(prelude/body/epilogue — body 안 register 는 오류), 행 사이 드롭 삽입·행 드래그 재배열, **세션 열**(레인 오른쪽 레일 — 다이얼로그마다 INVITE~확립 점선 + 확립~bye RTP 막대, refer 는 닫으면서 새 세션) · **during** = in-dialog 단계(dtmf/hold/resume/refer)를 통화 유지 바 위에 놓으면 `at_s` 마커(드래그로 시각 변경, 클릭 → 속성) — **during ↔ 행 상호 변환**: 기존 in-dialog 행을 유지 바 위에 끌어 놓으면 during 으로(행 제거·필드 승계), 마커를 바에서 세로 24px 넘게 벗어나 행 사이(gap)에 놓으면 독립 행으로; 속성 패널 [during 으로](가장 가까운 media_hold, 위쪽이면 at_s = 끝)·[행으로 빼기](유지 단계 바로 뒤) 도 같은 일(`scenario-model` `stepToDuring/duringToStep/nearestHold`) ③ **바인딩 바** = 기준 토폴로지·미리보기 프로파일(ht 자동 반영)·`${var}` 값 ④ **속성 패널** — 시나리오(id/title/tags/머리 주석/`target_evidence`/구간 요약) · 역할(이름 → 참조 동시 개명 / pool datalist = 토폴로지 풀 ∪ group / `disjoint_from` 같은 풀만 / count + 해석 박스·적합성 결과의 워커 창) · 단계(종류 변경, 행위자 역할 칩 — kind 게이트로 비활성(progress/refer/cause = 피어, register = UE 또는 트렁크 계정 피어), after_ms, seconds 고정값↔`${ht}` 토글, 코덱, payload, Q.850 목록, during 목록, **기대치 편집기** = 단계에 맞는 지표 ★ 우선 + 전체, 임계 p50/p95/p99/max/min) · during(at_s·동작·행위자·payload·to) ⑤ **하단 드로어** = YAML(양방향 — 캔버스 편집은 머리 주석 보존·한 줄 = 단계 재직렬화, YAML 편집은 `YamlEditor` 가 타이핑 중 컨트롤러 `POST /validate` 로 검증하고 [적용] 이 돌려받은 doc 으로 캔버스 재구성, 파서는 서버 하나) · 검증(스키마·행위자·구간·kind 게이트·in-dialog·during·바인딩, 심각도 정렬, **문서 오류 n / 토폴로지 적합 오류 m** 을 따로 세고 토폴로지 문맥 항목엔 배지 — 문서가 유효하면 저장은 되고 그 토폴로지로는 실행 못 함, 항목 클릭 → 행/레인) · **토폴로지 적합성**(`POST /scenarios/compile-check` 드라이런 → `PlanPreview` — 역할→풀→워커 창·용량·시드·환경변수·Little 검산·예상 소요) · 절차표(보고서와 같은 절차·예상 결과·확인 방법, during 은 n.k 행). 드로어 높이는 손잡이로. 키 = `↑↓` 행 선택 이동·`Alt+↑↓` 순서·`Del`·`Ctrl+D`·`Esc`; 끌기 중 행 사이 놓을 자리는 16px 로 넓어짐; 행위자 칩은 kind 게이트에 걸리면 누를 수 없음(취소선+사유); 기대치 칩·절차표의 부등호는 임계별(`min` ≥, 그 외 ≤, 단일값 =). **부하 프로파일** = YAML 편집기(`POST /validate` 600 ms 디바운스)·저장·삭제 + **율 곡선**(`ProfileCurve` — 검증된 doc 의 constant/step/ramp/soak/burst 시간축 SVG, 피크·부하 구간·예상 소요·총 시도·ht 동시 세션). **미디어 평면 편집**(§4): `invite` 속성에 `rtp` 선택(auto/none/explicit), 팔레트 미디어 묶음에 `media_send`/`media_stop`(행위자 = who 여럿, `after_ms`), `media_send` 속성 = `sample` 선택(기준 토폴로지 `media.samples` 의 id, 비면 기본 원천 — 라이브러리에 없는 id 는 표시)·반복 체크, 둘 다 통화 유지 바 위에 놓으면 during 마커(행 ↔ during 상호 변환 포함). 편집 시점 검증 = 서버 규칙과 같다 — SDP 가 오간 뒤(progress/answer 뒤)에만·`rtp: none` 호 금지는 문서 오류, 샘플이 기준 토폴로지에 없음은 토폴로지 적합 오류, `explicit` 인데 `media_send` 없음은 경고. 행 라벨 `송출 <샘플>`·`송출 정지`·`… · rtp explicit`, 적합성 결과에 `미디어 평면` 행(모드·참조 샘플·코덱) |
| 실행 `/test/runs` | ① **워커·대상 띠**(`WorkerFleet` — 토폴로지 워커마다 SIP 단말·RTP 스트림·CPU 막대, 최대 SApS·cpus·진행 run·시계 오차 > 50 ms 배지, 미응답은 점선; 대상 카드 = kind·SIP 노드 접속점·빌드(run 의 `target_build`), 진행 중이면 5 초·평상시 30 초 갱신) ② **라이브 패널**(`RunLivePanel`) — 머리(판정·상태·id·시나리오·라벨·토폴로지/프로파일/대상 빌드·라이브 점) + **진행 막대**(경과(고정 시간 제외) / 예상 소요 = 프로파일 규칙(step: 단계 수 × hold_s) / 종료 예정) + 왼쪽(`LiveSidebar`) **단계 사다리**(step 율마다 완료 ✓·IHS %, 현 단계 진행 막대, DOC 배지, 신원 부족 예상 단계는 흐리게 + 필요 신원) + **종료 조건 게이지**(IHS 현 단계 창 / 임계 · 대상 CPU(F) · CSP 5xx 60 초 창 · **Little 신원 여유** 율×SDT×역할 수) + **KPI 타일에 기대치 모서리**(시나리오 expect 유도, ok/bad 테두리) + **지표별 소형 차트 6장**(`LiveCharts`/`MiniChart` — 시도율·실패/초 · 동시 세션 · SER · SRD p95 · RTP 손실 · 워커/대상 CPU, 지표마다 자기 축 + 붉은 점선 기대치(stop_on 포함) + 단계 띠 + 십자선 호버 전 차트 동기) + **절차 진행 표**(flow 단계별 진입/완료/진행/실패 — 워커 누계 카운터 근사 + 누계 기대치 칩 `lib/metrics.judgeMetric`) + **실패 이벤트**(응답 코드/지표 칩 필터, 행 → **SIP 사다리 드로어** `SipDrawer` = `GET /runs/{id}/sip/{call_id}` 이벤트 + 덤프(있을 때)) + 워커 로그 접기 + 조작 = 율 적용 · **단계 고정**(`POST /runs/{id}/hold` — 프로파일 시계 정지, 율 유지) · 즉시 중단 ③ **run 색인**(`RunIndex`) — 필터 바(라벨/id/시나리오 검색·시나리오·대상 빌드·판정 칩(건수)·부하 run 만·기간 → `GET /runs?…` 300 ms 디바운스), **날짜 그룹**, 열 = 판정 먼저·시각·시나리오/라벨(중단 사유 배지)·프로파일·대상 빌드·시도·SER·SRD p95·손실·소요, 행 호버 액션 = **재실행**(같은 조건으로 시작 창)·결과, 체크 → 하단 고정 비교 바(첫 선택 = 기준) ④ **run 시작 창**(`RunStartDialog` + `PlanPreview`) — 입력이 바뀌면 500 ms 뒤 `POST /runs/plan`: 역할→풀→워커·신원 창 표, 예상 소요, 피크 율·동시 세션(SDT), 워커 SIP/RTP 용량·율·인스턴스, 대상 시드 예정 컬렉션, 바인딩 출처, 환경변수, 피어 고정, **Little 검산 경고·권고 max**, 워커 최대 SApS 초과·알려진 CSP 과제(§12) 경고; 컴파일 오류·진행 중 run 이 있으면 시작 잠금 |
| 결과 `/test/results?id=` | ① **왼쪽 run 레일**(날짜 그룹·판정 점·검색·전체/FAIL/PASS/부하 칩, 툴바 ↑↓ 이전/다음) ② **판정 요약 머리**(`RunReport` — 왼쪽 색 띠 = 판정, 큰 PASS/FAIL + 기대치 n/m, **왜 그 판정인가** = 실패 기대치마다 `#단계 종류 지표 기대/관측`·실패 인스턴스·중단 사유·**알려진 CSP 과제 §12 연결**(q850_rx_pct·트렁크 REGISTER·5xx→603·failover)·PASS 면 DOC, 메타 칩(run·토폴로지·프로파일·대상 빌드·시작/소요(고정 포함)·워커·역할→풀·바인딩), 액션 = **이전 빌드와 비교**(같은 시나리오·프로파일의 직전 다른 빌드 run 을 기준으로)·같은 조건 재실행·시나리오 열기) ③ 고정 **구획 내비** ④ **시간축** = 소형 차트 6장(실행과 같은 구성) + **대상 알람·이벤트 마커**(`GET /runs/{id}/target-alerts` — run 창으로 자른 대상 OAM `/alerts`, 심각도 색 세로 마커 + 칩; oam 노드 없으면 note) ⑤ **지연 분포 표 → 행 펼침 히스토그램**(`GET /runs/{id}/hist?timer=` 로그 상한 버킷 막대 + p50/p95/p99 마커 + 기대 초과 버킷 붉게) ⑥ **절차·예상·확인 표에 여유 막대**(관측/임계 비율, 80 % 넘으면 주의색, FAIL 행 강조) ⑦ 실패 이벤트 = 코드 칩 집계 + 행 → SIP 사다리 드로어 ⑧ 단계 로그 = IHS 막대(임계 위치 표시)·DOC·임계 초과 → 중단 배지 ⑨ **대상 증거 카드**(target_evidence 항목별 대기 — 대상 OAM 조회는 F, `seed_restored` 는 run 메모로 판정) ⑩ Markdown 미리보기·복사. 진행 중 run 이면 라이브 패널. 인쇄는 레일·내비·액션을 숨기고 구획 단위로 쪽 나눔 · 삭제(manager) |
| 비교 `/test/compare?ids=&mode=` | 두 모드. **run 비교**: ① **run 카드 열**(색 견본 = 차트 색, 첫 카드 = 기준, 드래그/‹ › 순서 변경·[기준으로]·제거, [+ run 추가] picker 는 **같은 시나리오·프로파일 후보를 먼저**(다른 것은 [전부 보기] 로 흐리게)) ② **지표 매트릭스**(행 = 지표·방향, 열 = run(대상 빌드로 표기), 셀 = 값 + Δ(pt 또는 절대) + **중심 0 의 Δ 막대**, 회귀 붉게·개선 초록, 허용 오차 열 ±0.5 pt/±5 % — 판정은 `GET /runs/compare`) ③ **시간축 겹침**(행 클릭 → 그 지표의 run 별 `/series` 를 **run 시작 t+0 기준으로 정렬**해 한 차트에, 기준 run 굵게, 기대치 점선, 기준 run 의 단계 띠 — 프로파일이 다르면 경고) + 응답 코드 분해 run 별 ④ **기대치 판정 diff**(행 = 단계·지표·기대, 열 = run 별 PASS/FAIL, 변화 = 기준 PASS→마지막 FAIL **회귀** / FAIL→PASS **복구**, 하단 중단 사유). **추세**: 시나리오 × 프로파일 × 토폴로지 선택 → 모든 run 을 시간순 점으로(x = run 시각, 색 = 대상 빌드, ● PASS / ◆ FAIL·중단, 기대치 점선, **기준 = 첫 PASS run ± 허용 오차 회색 띠**, 벗어나면 '회귀' 라벨) 4 장(SRD p95·SER·손실·DOC) + run 목록(체크 → run 비교로). 내보내기 Markdown/CSV(`compare?format=`, 클립보드). 시나리오가 다르면 경고 배지 |
| 보고서 | 결과 화면의 [인쇄] = `window.print()` — 검증 콘솔과 같은 인쇄 규약(셸·툴바·레일·이벤트 표 숨김, `.tester-report` 만 A4, 구획 단위 쪽 나눔), 표지에 발행 일시. Markdown 은 CLI `report` 와 같은 본문 |

컴포넌트는 팩 안 `components/`(MiniChart · LiveCharts · WorkerFleet · LiveSidebar · RunLivePanel · RunIndex · RunStartDialog · PlanPreview · SipDrawer · RunReport · YamlEditor(`onValid(ok, doc)`) · ProfileCurve · ListRail(레코드 레일 + RailRow/RailGroup) · topology/TopologyCanvas · scenario/ScenarioCanvas), 모델·표시 헬퍼 `lib/`(fmt.ts = RFC 6076 라벨·판정 톤·수치 형식 · metrics.ts = 기대치 임계·라이브 누계 판정·절차 진행·SDT/예상 소요(컨트롤러 규칙과 동일) · topology-model.ts · scenario-model.ts · use-history.ts = 문서 이력·Ctrl+Z/Y·이탈 경고 · use-drawer-height.ts = 드로어 높이). 차트는 라이브러리 없이 SVG(`--chart-N` 토큰), 지표별 소형 차트는 자기 축. 배지는 크기 오버라이드 없이 계약(12px SemiBold) 그대로(console_design_system §7-31).
**남은 것** = 대상 관측(F — 알람 마커는 API 로 붙지만 대상 CPU·target_evidence 판정은 대상 OAM 조회 후속) · 워커 SIP 덤프 이전(`runs/<id>/sip/<call_id>.log` 규약만) · cims-verify S3/S6 시나리오 항목의 `cims-tester` 호출 이전 · 워커 자동 발견(agent 배포 목록) · 미디어 평면 후속(AMR-WB 샘플 동봉·비디오 송출 제어·미디어 전담 워커 분리).

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
- 빌드: 루트 CMake `option(CIMS_TESTER ON)` 으로 `cims-tester-worker`(+`libcsim`)를 `build/bin/` 에. `libcsim` = `cspsim/` 의 `add_library(csim STATIC …)`
  (SimSession·RtpThread·SipClient·G711) — cspsim CLI 와 워커가 같은 정적 라이브러리를 링크한다. 링크 순서: `libsrtp2` 가 psip(동봉 opensrtp) 보다 앞
  (심볼 충돌). `make dist` 가 `dist/cims-tester-worker/{bin,config/{config_template,cims-tester-worker}.json,pkg.json}` 을 채운다.
- 워커 설정(`tester/worker/config/config_template.json`): `Worker.Name`(비면 hostname) · `Server.Ip/Port`(제어 7100) · `Sip.LocalIp`(비면 자동 탐지)·
  `Sip.PortBase`(0=OS 자동, >0 = base+2i) · `Media.AudioFile/VideoFile`(비면 합성 PCMU/비디오 없음) · `Limits.EndpointsPerCore/SapsPerCore`(용량 선언) ·
  `Timers.*`. 배포 overlay `config.json`(평면 키)은 lifecycle 가 모듈 설정에 머지하고 워커도 자기 옆의 것을 읽는다. libcsim 의 printf 진단은 부하 중
  초당 수천 줄이라 워커는 stdout 을 `/dev/null` 로 돌리고(`--verbose` 로 유지) 자기 로그는 stderr 로 낸다.
- 검증 게이트: `S1-UNIT-TESTER`(계약·핸들러·오케스트레이터(가짜 워커)·피어 시드 파생·게이트웨이 SSE 단위시험 + 네이티브 `build/bin/csim_rtp_dtmf_test`
  RFC 4733 루프백) · `S1-CONFIG-PORTABILITY` 대상에 두 모듈 설정 ·
  `S2-PREFLIGHT` 네이티브 바이너리 목록 · `S4-PKG-BUILD` 기대 tarball 에 `oam-cims-tester`·`cims-tester-worker`.
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
| **B. UE 축 + 컨트롤러 최소** | `libcsim` 추출(SimSession/RtpThread → 라이브러리, cspsim 은 그 위 CLI), `cims-tester-worker` ue 풀·단계 실행기·1초 집계 스트림, `oam-cims-tester` run/저장/CLI + pkg·config_template·self-register, 프로파일 constant·step(+ramp·soak·burst) | **구현 반영** — 개발서버 CSP(UDP 15060) 상대로 `cims-tester run VOLTE-CALL-BASIC --topology … --instances 3` 완주(SER 100 %, RRD p95 5 ms, SRD ≈ after_ms+20 ms, RTP 손실 0, 보고서·기대치 판정), 워커 단독 4쌍 1 SApS 지속. 남은 것 = 부하 강화 문서 시험(4 cps/HT20, 10 cps/HT5) 재현 실측·워커 2대 분산 실측·`db` 신원 원천(대상 CSC 위임)·워커 자동 발견(base 배포 목록)·대상 관측(`stop_on.target_cpu_pct`) | L |
| **C. 피어 축 — ibcf** | peer 엔진(고정 수신점·신원 범위·응답 정책·무응답) + `ibcf` 프로파일, 대상 CSP 컬렉션 시드/복원, 트렁크 in/out·route_set failover·ACL 시나리오 | **구현 반영** — `CsimPeer` 엔진·워커 peer 풀·컨트롤러 시드/복원(§3.2). 개발서버 CSP 상대 실측: `TRUNK-IBCF-OUTBOUND`(가입자→피어, SRD p95 820 ms, RTP 손실 0)·`TRUNK-IBCF-ACL-DENY`(피어링 접속점 ACL → 403) **pass**, `TRUNK-IBCF-INBOUND`(피어→피어링 접속점→가입자, SRD p95 1185 ms, RTP 손실 0) **pass**(CSP 피어링 접속점 인증 생략 반영본), `TRUNK-IBCF-FAILOVER` 는 CSP 헬스체크 부재로 우선(무응답) 피어에서 Timer B — §12 확인. 남은 것 = 오류 주입(응답 지연·특정 코드·재전송 유실)·TLS 상호인증·THIG 흔적 | M |
| **D. 피어 축 — pbx · mgcf** | 트렁크 REGISTER, DID/내선, 183 early media·PRACK, hold/resume, REFER 발신, RFC 4733 DTMF, Q.850 Reason, G.711 | **구현 반영**(§3.2 pbx·mgcf) — 시나리오 `trunk/pbx_{outbound,inbound,dtmf,hold_resume,transfer,register}.yaml`·`trunk/mgcf_{outbound,inbound,reject_q850}.yaml`. 개발서버 실측 7 pass / 3 fail — fail 은 전부 CIMS 측(§12: Reason 미투과·503→603·트렁크 계정). 남은 것 = UE 측 183(실 단말 착신 모사 아님)·in-band DTMF·G.722·TLS 상호인증 | M |
| **E. 콘솔 팩** | §7 화면 전부, SSE 라이브, 비교·보고서. cims-verify S3/S6 시나리오 항목의 `cims-tester` 호출 이전 | **구현 반영**(§7 표) — 컨트롤러 = 토폴로지 v2 모델·v1 승계·계획 미리보기(`runs/plan`=`compile-check`)·`vocab`·`hold`·`hist`·`sip/{call_id}`·`target-alerts`·색인 필터·`compare?format=`·`during` 평탄화·연결 검사 `노드:수신점`; 콘솔 팩 = 토폴로지 캔버스 편집기·시나리오 시퀀스 캔버스 편집기·실행(워커 띠·단계 사다리·종료 조건·소형 차트·절차 진행·SIP 드로어·색인 필터·계획 미리보기)·결과(run 레일·판정 요약·히스토그램·여유 막대·알람 마커·대상 증거)·비교(run 카드·Δ 매트릭스·t+0 겹침·기대치 diff·추세). 개발서버 실측: 연결 검사 CSP OPTIONS 200 OK / TLS 1.3 / 워커 health, `VOLTE-CALL-BASIC` 단발 3 인스턴스 완주(pass, SER 100 %), 단위시험 73 건(S1-UNIT-TESTER PASS), 콘솔 tsc·vite build 통과. 남은 것 = §7 '남은 것' + 콘솔 화면 실기 확인(oam 패키지 재빌드·배포) | L |
| **미디어 평면** | `invite.media.rtp`(auto/none/explicit)·`media_send`/`media_stop`·샘플 라이브러리의 워커 이행 — libcsim `CRtpThread` 송출 제어·원천 교체(단일 송신 루프, AMR-WB/G.711 파일·합성)·상대 hold 정지, 워커 단계·샘플 검증·health `media`·RTP 상한, 컨트롤러 모델·컴파일(샘플 발췌)·계획 미리보기 검산·`early_rtp_pct`, 콘솔 편집 | **구현 반영**(§4 미디어 평면) — 단위시험: libcsim `csim_rtp_media_test`(none·explicit·샘플 1회 재생 = 프레임 수만큼·정지/재개에 시퀀스 공백 없음·hold 정지·AMR-WB NO_DATA) + 컨트롤러 모델·컴파일·계획(S1-UNIT-TESTER). 개발서버 CSP/CMP 실측: `VOLTE-CALL-SIGNALING` pass(RTP 카운터 0) · `VOLTE-CALL-ONEWAY-MEDIA` pass(발신자만 4 s — tx 197 = rx 197, 손실 0, 무수신 단말 1, CMP relay 경유) · `VOLTE-CALL-BASIC` 회귀 pass(CMP 녹취 PT = 협상 PT 96) · `TRUNK-MGCF-EARLY-MEDIA` **fail = CSP 과제**(§12: `early_rtp_pct` 0 % — 18x SDP 미앵커링). 남은 것 = AMR-WB 샘플 동봉·비디오 송출 제어·미디어 전담 워커 분리 | M |
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
| 토폴로지 모델 | **확정** | 호스트 › 워커·대상 노드 › 풀 3단(§4) — 주소는 호스트에만, 대상 = 역할별 노드 집합(sip/tas/media/subscriber/oam/db, CIMS·타 IMS·IP-PBX 공통), 호스트 성격(계측기/대상/동거)은 파생·동거 허용, 풀 하나 = 워커 하나(워커 여럿은 워커마다 풀 + 같은 `group`, 시나리오는 이름 또는 group 으로 참조), 콘솔은 DnD 캔버스로 편집(§7 — 빈 곳 드롭 = 상자 자동 생성, 선은 모델 파생). 컨트롤러 이행은 E 남은 것 |
| 목표 규모 | 제안 | 등록 UE 5,000 · VoLTE 100 SApS × HT 20 s(동시 2,000) · PTT 그룹 200 × 20명 · 피어 트렁크 50 SApS. 워커 호스트 수는 B 단계 실측 후 확정 |
| 워커 호스트 | 제안 | 시험 대상과 분리된 최소 2대(8 코어) — 발생기 동거로 v1~v6 오진한 이력. 워커 용량 선언 기본 = 코어당 200 단말·10 SApS(`Limits.*`) |
| MGCF 범위 | **확정** | 평문 SIP(TS 29.163 Mg, TS 24.229) + Q.850 Reason + early media. **SIP-I 불필요** — CSCF↔MGCF(Mg)·IMS↔IMS NNI(GSMA IR.95)는 SIP/SDP 이고, SIP-I 는 CS 망 상호접속 트렁크(ITU-T Q.1912.5) 프로파일이다. 그런 트렁크를 받으려면 CIMS 자신이 MGCF(ISUP 해석) 역할을 해야 하는데 그것은 계측기가 아니라 CIMS 로드맵 문제다 |
| 트랜스코딩 | **확정** | MGCF 는 IM-MGW 가 AMR-WB 를 오퍼하므로 문제 없다. **IP-PBX 는 G.711 이 필수 코덱**(SIPconnect 2.0)이라 CIMS 쪽 트랜스코딩이 필요하다 → **CMP 과제로 채택**, 설계 정본 [../modules/cmp.md](../modules/cmp.md) §11(피어 leg 한정 G.711↔AMR-WB, TrGW 역할). 계측기 pbx 프로파일은 G.711 기본으로 그 경로를 시험한다 |
| 대상 관측 원천 | 제안 | 대상 OAM API + agent heartbeat 기본, ssh 샘플러는 옵션. OAM 이 없는 최소 배치도 시험 가능해야 |

---

## 12. 시험이 드러낼 CIMS 측 과제 (계측기 범위 밖, 병행 결정)

피어 축 시험은 현 CSP 피어링 구현의 미구현 항목([sip_service_model.md](sip_service_model.md) §9)과 바로 충돌한다.
계측기는 이를 **실패로 기록**하고, 채택 여부는 CIMS 로드맵에서 정한다.

| 항목 | 현 상태 | 시험에서 보이는 모습 |
|---|---|---|
| RouteSet 헬스체크(OPTIONS 프로브) | 미구현, `alive` 항상 true | 피어 1대 정지 시 failover 안 됨 — **실측 확인**(`TRUNK-IBCF-FAILOVER`): failover 집합의 우선 피어가 무응답이면 B-leg 가 Timer B(32 s)까지 기다리고 다음 피어로 넘어가지 않는다(A-leg 도 그동안 최종 응답 없음) |
| 트렁크 REGISTER(`register_to_remote`, 수신 측 트렁크 계정) | 미구현 | **실측**(`TRUNK-PBX-REGISTER`): PBX 계정의 Digest REGISTER 가 가입자 조회에서 403 — 등록형 트렁크 시나리오는 attempts 0 으로 닫힌다. 고정 IP 피어링(ACL 신뢰)만 동작 |
| THIG·번호 정규화·`Privacy` | 없음 | ibcf 프로파일의 신원·프라이버시 검사 실패. (`P-Asserted-Identity` 는 psip 이 발신 leg 도메인으로 실어 B-leg 에 있다 — 실측 `pai_missing` 0) |
| 피어 인바운드 인증 | **반영** — 신뢰는 접속점 edge 가 아니라 **인바운드 Route 식별**(수신 LocalNode + 소스 IP[:UDP 포트] → Route, `routes.inbound_auth=none`) 에서 나온다. `edge=peering` 접속점은 Route 없는 소스에 403, access 접속점의 피어도 Route 가 있으면 Digest 없이 통과. 라우팅 정책이 트렁크를 고른 호라고 인증을 건너뛰던 우회(PendingRouteMap — 미등록 발신자의 무인증 트렁크 발신 구멍)는 제거. ACL `scope=route/route_set` 인바운드 동작 | `TRUNK-IBCF-INBOUND` 가 401 로 실패 + psip UAC 가 401 에 INVITE 를 무한 재송(그 전 변경에서 수정) → 시드 ACL 은 `scope=route` 로 그 피어에만 |
| PRACK/100rel·183 시그널링 트렁크 전달 | **정상 확인** — `TRUNK-MGCF-OUTBOUND` 실측 early_media 100 %·prack 100 %(CSP 가 183/SDP 를 A-leg 로, A-leg PRACK 을 B-leg 로 전달, PRACK SDP 재작성 §5.2) | 회귀 시험 항목으로 유지. 시그널링 도달만 말한다 — 미디어 경로는 아래 행 |
| **18x SDP 의 미디어 앵커링**(early media RTP) | **CSP 코드 반영 — 반영본 실측 대기**([volte_flows.md](volte_flows.md) C1a: 18x+SDP 에서 answer leg 를 relay 에 반영(`RELAY_MODIFY`)하고 상대 leg SDP 를 relay 주소로 재작성). 계측기가 드러낸 모습(CSP 0.2.132, `TRUNK-MGCF-EARLY-MEDIA` — CSP SIP 로그·CMP 로그·CMP 녹취 대조): ① 발신자가 받는 183 의 `m=` 포트가 피어 자신의 RTP 포트(200 의 SDP 만 CMP 주소로 재작성 — `m_iPort` 만 바꾸고 미디어 목록은 그대로였다) ② CMP 에 B-leg 주소를 넘기는 `RELAY_MODIFY` 가 200 에만 | 0.2.132 에서는 피어가 CMP B측 포트로 보낸 링백 RTP 가 주소 미등록이라 200 까지 폐기되고(`early_rtp_pct` 0 %), 발신자의 early RTP 는 CMP 를 우회해 피어에 직접 갔다(같은 호스트 시험망이라 직접 경로가 열려 있었을 뿐). 반영본에서 `early_rtp_pct` 100 % 가 회귀 시험 기준이다 — 확인 전까지 계획 미리보기는 이 항목을 알려진 과제로 경고한다 |
| re-INVITE hold/resume·REFER 트렁크 전달 | **정상 확인** — `TRUNK-PBX-HOLD-RESUME`(re-INVITE 200 2/2)·`TRUNK-PBX-TRANSFER`(피어 leg REFER 202 → 대상 INVITE → 전달자 BYE) | 회귀 시험 항목 |
| RFC 4733 telephone-event 투과(CMP) | **정상 확인** — `TRUNK-PBX-DTMF` dtmf_rx 100 %(CMP `PRtpRelay` 가 협상 PT/TE PT 만 통과) | 회귀 시험 항목 |
| 번호 prefix 라우팅(자기 도메인 Request-URI → 트렁크 RouteSet) | **정상 확인** — `req_uri_user prefix` 규칙으로 UE 가 DID/E.164 를 그대로 다이얼 | pbx/mgcf 시드 규칙 |
| G.711 ↔ AMR-WB 트랜스코딩 | **채택** — [../modules/cmp.md](../modules/cmp.md) §11 설계, 구현 전 | 구현 전까지 pbx 프로파일 G.711 호는 488 또는 미디어 무음. 구현 뒤 = pbx 시나리오가 회귀 시험 |
| RFC 4028 세션 타이머 | 설계만([leg_liveness.md](leg_liveness.md)) | 피어 leg 유실 회수 시나리오 |
| `Reason: Q.850` 종료 사유 전달 | 없음 — **실측 확인**: 피어 BYE `Reason: Q.850;cause=16` 이 상대 leg BYE 에 없고(`q850_rx` 0, `TRUNK-PBX-DTMF`·`TRUNK-MGCF-OUTBOUND`), 거절 503 + `Reason` 도 발신자에 Reason 없이 도달 | 통계 실패 사유 분해 불일치. B2BUA 는 BYE/최종 응답의 Reason 을 상대 leg 에 복사해야 한다(RFC 3326 §2, TS 24.229 §5.4.3.2) |
| 트렁크 최종 응답 코드 매핑 | B-leg 503 → A-leg **603 Decline** (`TRUNK-MGCF-REJECT-Q850` 실측 `codes.603`) | 발신자가 망 장애를 사용자 거절로 본다. RFC 3261 §16.7 은 503 을 500 으로 바꿀 수 있다고만 했다 — 4xx/5xx 는 그대로(또는 503→500) 전달해야 한다 |
| `Setup.Roles.IBCF=false` 인데 피어 라우팅이 동작 | 가드 누락 | 역할 격리 시나리오 실패 |
