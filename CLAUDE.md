# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 이 문서는 **프로젝트 개요·빌드 환경·문서 맵·작업 원칙**만 담는다. 아키텍처/모듈/기능의
> 상세 설계는 [docs/](docs/) 의 해당 문서가 정본이며, 여기서는 그 경로만 가리킨다.

## 프로젝트 개요

CIMS 는 3-tier PTT/VoIP 서버다. **CSP** 가 SIP 시그널링을, **CMP** 가 RTP 미디어를 담당하고,
**cspsim** 이 단말을 시뮬레이션한다. 운영·관리 평면(OAM)과 콘솔, 가입자/MCPTT 서버(CSC)가 이를
구성·배포·감시한다.

```
cspsim  ←─ SIP (UDP 5060 / TCP 25061 / TLS 5061) ──→  CSP
                                                         │
                                               UDP JSON (port 9001)
                                                         │
                                                        CMP  ←─ RTP ─→ clients
```

| 구성요소 | 역할 | 정본 설계 문서 |
|---|---|---|
| **CSP** (`csp/`) | IMS 역할 기반 모듈형 SIP 서버 (CSCF/TAS/PTT-AS/IBCF) | [docs/design/modules/csp.md](docs/design/modules/csp.md) |
| **CMP** (`cmp/`) | RTP relay + MCPTT floor control (CSP 의 UDP JSON 제어). 피어 leg 한정 G.711↔AMR-WB 트랜스코딩은 설계만(cmp.md §11, IP-PBX 트렁크용) | [docs/design/modules/cmp.md](docs/design/modules/cmp.md) |
| **CMDP** (`cmdp/`) | MCData media plane — 대용량 SDS MSRP 종단 + FD 스토어 (CSP 의 UDP JSON 제어) | [docs/design/features/mcdata_messaging.md](docs/design/features/mcdata_messaging.md) §4.7 |
| **cspsim** (`cspsim/`) | SIP/RTP 부하·기능 시험용 단말 시뮬레이터 | — |
| **계측기** (`ems/tester/`, `tester/`) | `oam-cims-tester` — OAM 게이트웨이 뒤 서비스 모듈(`oam-svc` 와 동격, loopback 4490) + `cims-tester-worker`(C++ 부하·피어 워커) + 콘솔 팩(`시험` 그룹). N UE 부하 + IBCF/IP-PBX/MGCF 피어 시뮬레이터. 독립(자기 `oam`)/동거 두 운영 형태, 워커는 시험 대상 밖 호스트. **A·B 단계 구현** = 계약 모델(pydantic→`schema/*.json`)·컨트롤러 API·콘솔 팩·게이트웨이 SSE 통과·nav 서비스 게이팅 + `libcsim`(cspsim 승격)·워커 ue 풀/단계 실행기/1초 집계 스트림·컨트롤러 run 오케스트레이션(프로파일·요약·판정·보고서)·CLI `run/report/creds-from-db`. **C 단계** = `libcsim` 피어 엔진 `CsimPeer`(고정 수신점·신원 범위·다수 동시 호, ibcf/pbx/mgcf 코덱 프로파일)·워커 `peer` 풀·대상 CSP 컬렉션 시드/복원(토폴로지 피어 풀에서 파생, OAM 컬렉션 API, 피어링 접속점 시드·ACL scope=local_node)·IBCF in/out·ACL·failover 시나리오. **D 단계** = pbx/mgcf 프로파일 — 183 early media·100rel/PRACK, hold/resume(re-INVITE), blind REFER, RFC 4733 DTMF(libcsim RTP 송수신), Reason Q.850 송신·관측, pbx 트렁크 REGISTER, 번호 prefix 시드 규칙(pbx/mgcf 는 번호 그대로 다이얼), 단계 `progress/hold/resume/dtmf/refer`·`cause`, 비율 지표 4종. 실측 7 pass, fail 3 은 CSP 과제(§12 Reason 미투과·503→603·트렁크 계정). **E 단계** = 콘솔 팩 5 라우트(토폴로지 편집·연결 검사·워커 상태 / 시나리오·프로파일 YAML 편집기 / 실행 라이브 SSE 차트·중단·율 조정 / 결과 보고서·인쇄 / 비교 회귀 판정) + 컨트롤러 편집·검사·시계열·비교·api-docs API. **토폴로지 v2** = 호스트›워커·대상 노드›풀 3단(주소 기본값은 호스트, 노드 `addr`(VIP)·수신점 `ip`·피어 `bind.ip` 로 덮어씀; SIP 노드 수신점 N개 `sip.listeners{edge,ip,port,protocol,local_node}` = CSP local_nodes 1:1, 풀은 `listener` 로 항목 선택 — 피어는 access 항목도 됨(CSP 신뢰 = Route `inbound_auth`); 대상 = 역할별 노드 집합 sip/tas/media/subscriber/oam/db — CIMS·타 IMS·IP-PBX 공통, 동거 허용, 풀 하나 = 워커 하나 + `group`) — 컨트롤러 모델·v1 승계·워커 계약 파생 + 콘솔 **DnD 캔버스 편집기**(팔레트·호스트 영역·카드·모델 파생 선·속성·JSON/검증/연결 검사 드로어). **시나리오 시퀀스 캔버스 편집기**(레인=역할×행=단계, 세션 열, `media_hold.during` 마커, vocab 팔레트, YAML 양방향(파서는 서버), 적합성 = `compile-check` 드라이런, 절차표). **실행·결과·비교 정식화** = 워커·대상 띠 + 단계 사다리·종료 조건 게이지(Little 여유)·지표별 소형 차트(기대치 점선)·절차 진행·SIP 드로어·색인 필터/날짜 그룹·재실행·시작 창 계획 미리보기(`runs/plan`) / run 레일 + 판정 요약(왜 FAIL·§12 연결)·알람 마커·히스토그램(`hist`)·여유 막대·대상 증거 / run 카드·Δ 막대 매트릭스·t+0 정렬 겹침·기대치 diff·추세·md/csv. 컨트롤러 API 추가 = `vocab`·`compile-check`·`runs/plan`·`hold`·`hist`·`sip/{call_id}`·`target-alerts`·색인 필터·`compare?format=`. **미디어 평면** = `invite.media.rtp`(auto/none 시그널링 전용/explicit)·단계 `media_send{who,sample,loop,after_ms}`/`media_stop`(SDP 교환 뒤에만, during 가능)·토폴로지 샘플 라이브러리 `media.samples{id:{amr-wb|pcmu|pcma: 파일|synthetic}}`(워커 `Media.SampleDir`, 동봉 `ringback_kr`·`tone_1k`·`sample_voice.amrwb`(AudioFile 기본)·`sample_video.h264`(VideoFile 기본))·워커 `Media.MaxRtpStreams`/health `media`·libcsim `CRtpThread` 송출 제어(단일 송신 루프, AMR-WB 합성 = NO_DATA 프레임, 상대 hold 정지)·지표 `early_rtp_pct`(200 전 RTP 도달). 실측 4 pass — `TRUNK-MGCF-EARLY-MEDIA` 가 CSP 의 18x SDP 미디어 미앵커링을 드러냈고 CSP 0.2.134 가 반영(18x+SDP 에서 `RELAY_MODIFY`·SDP relay 재작성 — volte_flows C1a). **워커 SIP 덤프** = psip 네트워크 로그 콜백으로 Call-ID 별 캡처 → 실패한(또는 `Sip.Capture=all` 이면 모든) 인스턴스의 호를 스트림 `sip` 레코드로 → `runs/<id>/sip/<call_id>.log`(콘솔 SIP 드로어 사다리·결과 화면 덤프 목록). **대상 관측** = 대상 OAM agent heartbeat 로 호스트 CPU·메모리 수집(`stop_on.target_cpu_pct`·`target-series`·결과 차트) + `target_evidence` 2차 판정(녹취·알람·이벤트 건수, 원천 없는 항목은 판정 불가로 제외), 대상 OAM 토큰 = 환경변수 → 요청자 로그인 토큰 폴백(동거 형태), `Tester.DataDir` 기본 = 버전 무관 `runtime/data`. **워커 분산·발견** = creds `source.offset` 로 워커마다 다른 신원(겹치면 컴파일 오류)·워커 2대 분산 실측(78 시도 = 39+39, SER 100 %)·`GET /workers/discovered`(자기 base OAM 배포 목록 → 편집기 '발견된 워커', `Tester.BaseOamUrl`). UE 풀 **`source.db`** = 대상 DB 에서 H(A1) 보유 가입자 직접 읽기(db 노드 + 환경변수 자격). **워커 PTT 단계** = UE 풀 `service: ptt`(MCPTT 단말 — 기동 절차 REGISTER→GMS/CMS 구독→affiliation→conference 구독, 자동응답, floor RTCP APP) + **그룹 세션**(body 에 `group_call` → 인스턴스 = MCPTT 그룹 하나, 단일 역할에 멤버 하나씩·`multi: true` 역할에 나머지, Little 자원 = 쓸 수 있는 그룹 수) + 단계 `group_call/floor_request(payload 기대 결과)/floor_release`·`bye who`, 지표 `affiliate_ms·group_fanout_ms·floor_grant/taken/queue/idle_ms·floor_grant_pct`(floor 수신 시각 µs), PTT 미디어 = floor 를 가진 동안만 송출, 신원 그룹 = creds `group`/`source.ptt_group`(`creds-from-db --ptt-group`), 동봉 `PTT-GROUP-CALL-BASIC`·`PTT-FLOOR-HANDOVER`, `S6-SCN-PTT-VOICE` 이전. 실측 pass(g001 5명, fan-out 250 ms·grant 7 ms). **전달·합류·구독 단계** = `refer`(blind / 상담 통화 중이면 attended — 두 번째 `invite` = 상담 호)·`pickup`(피처코드 `${pickup_code}`, to = 지정)·`subscribe`(이벤트 패키지·감시 대상)·`replaces`/`join`(RFC 3891/3911 — 앞선 dialog `subscribe` 로 배운 다이얼로그, 컴파일 검사)·`publish`(TS 24.379 affiliation 명령), libcsim 훅 `OnSubscribeResponse`/`OnDialogNotify`·구독 해제·상담 호 정리, 지표 `join_tap_pct`·`affiliate_ms`, 동봉 시나리오 11종, S3 XFER/PICKUP/DIALOG/MONITOR 항목의 검사 단위 계측기 경로(계획 `identities_by_role` 에 DB 픽스처). 남은 것 = 호스트 SSH 관측(프로세스별 CPU·로그 오류)·**지표 보강** = MOS(G.107 E-model, 코덱 = 수신 wire PT, 기대치 `min`)·RFC 6076 SEER/ISA(`seer_ok`/`isa_fail`/`invite_tx`, ISA 는 상한)·RTCP SR/RR 수신 통계·영상 `video_pct`(`media.video: h264` → m=video, 워커 동봉 `sample_video.h264`) + S6 VOLTE-VIDEO/PTT-VIDEO/IBCF-TRUNK 다리. **호스트 SSH 관측** = `hosts.*.ssh` 로 `/proc` 를 읽어 호스트 CPU/메모리 + `procs` 프로세스별 CPU(틱 차분)/RSS(`target_proc`, `target_rss_delta_mb` 소크 누수)·`nodes.*.logs` ERROR 증분 = `log_errors` 증거, 연결 검사가 키 인증까지 확인. **대표번호·청취 이전** = `invite/pickup.to` 번호 리터럴(`${pilot}`)·포크 관측(`fork_alert_pct`, 승자 외 CANCEL 정상, `pcpid_ok`) + 그룹 밖 역할 `member: false`·`group_call payload: listen`(recvonly 청취 합류, `listen_pct`, floor `denied`) → S3-SCN-FA F1/F3/F5/F6·S3-SCN-PTT-LISTEN L1~L4 계측기 경로(계획 `group_session.first_group`·비멤버 후보). **피어 오류 주입** = `answer: reject`(즉시 fault.code + Reason Q.850)·`delay`(fault.delay_ms 응답 지연)·`TRUNK-IBCF-FAILOVER-5XX`. **`real-ue` 풀** = 신원마다 `cimsue-cli … drive`(libcimsue/pjsua2 실스택) 프로세스(워커 `RealUe.*`, 패키지에 cimsue-cli 동봉), stdin 명령/stdout JSON 이벤트를 가상 단말과 같은 Event 로 풀어 단계 실행기 무변경, 단계 게이트 `REAL_UE_STEPS`(등록·1:1 호·hold/resume·DTMF·픽업·그룹콜·floor), 미디어는 실스택 것(rtp auto 만), `real_*` 지표(SRD·손실·지터·MOS) + 전체 지표 동시, 동봉 `VOLTE-CALL-REAL-UE`, 실측 VoLTE·PTT 그룹콜(멤버 전원 실단말) pass. **피어 후속·D 잔여** = 재전송 유실(`fault.drop_invite/drop_pct` — psip `RecvFilter`, `retrans_rx_pct`)·TLS 상호인증(`tls_client_auth/tls_verify/tls_client_cert` — psip 연결 단위 클라이언트 인증서·검증 저장소·접속점 전용 ctx, 워커 `Tls.*`)·THIG 토큰화 Via(`thig_pct`)·UE 측 183(`progress who: [UE]`)·in-band DTMF(`dtmf: inband` G.711 톤·Goertzel)·G.722(PT 9), 동봉 시나리오 6종·단위시험 `csim_peer_fault_test`·`csim_tls_mutual_test`. **F 잔여** = 소크 누수 판정(`target_evidence` `rss_growth_mb`/`fd_growth` — SSH 관측 프로세스 RSS·fd 처음↔끝 차, `VOLTE-CALL-SOAK-LEAK`)·알람 타임라인 겹침(대상 자원 차트·라이브 패널)·MCData SDS(libcsim `McDataSds` MESSAGE 경로, 단계 `sds_send`/`sds_recv`·그룹 SDS·`sds_disposition_pct`, `MCDATA-SDS-1TO1/GROUP`). **NAT 풀** = UE 풀 `nat{netns,local_ip}` — 워커가 setns 한 스레드에서 스택을 띄움(`scripts/nat-netns.sh`, `cims-priv setcap-sys-admin`, `VOLTE-CALL-NAT`). **cspsim 검사 이전** = `subscribe`(to 대표번호 리터럴·`payload: conference` 그룹 AoR) + `check` 단계(`conference_roster_visible|hidden`·`conference_warning_138`·`dialog_consistent`, `check_pct`) → S3-SCN-FA F7·S3-SCN-PTT-LISTEN L1b/L2b/L3b/L5 계측기 경로(`VOLTE-FA-DIALOG-FORK`·`PTT-GROUP-LISTEN-ROSTER/CONF-DENIED`). **미디어 전담 워커** = UE 풀 `media_worker` → CRtpThread 원격 모드(`RtpRemote.h`)·워커 에이전트 `/media/*`(`MediaAgent`)·SDP `MediaIp()`·health `media.agent_streams`(`tester_media_agent_test`). **MCData SDS media plane** = `sds_send plane: media`(libcsim `McDataMsrp` — INVITE m=message → cmdp a=path 로 MSRP SEND, 완료 = 200/REPORT) + UE 풀 `msrp`(Contact mcdata.sds → 서버발 MSRP 배포 수신, 아니면 FILEURL 폴백) + `sds_media_pct`, `MCDATA-SDS-GROUP-MEDIA`, `csim_msrp_test`. 남은 것 = MCData FD 단계·부하 강화 재현·배포 뒤 대상 상대 실측(CSP §12 반영분 포함) | [docs/design/features/test_instrument.md](docs/design/features/test_instrument.md) |
| **CSC** (`csc/`) | 가입자 관리 + MCPTT(IdMS/GMS/CMS/XCAP) 서버 | [docs/design/modules/csc.md](docs/design/modules/csc.md) |
| **OAM/Console** (`ems/`) | 운영·관리 평면 게이트웨이 + 웹 콘솔 (core/service 분리) + 자동 배포 엔진 내장 | [docs/design/console_platform.md](docs/design/console_platform.md), [docs/design/oam_csc_split.md](docs/design/oam_csc_split.md) |
| **Agent** (`agent/`) | 노드 에이전트 (배포/HA/업그레이드 supervised) | [docs/design/modules/agent.md](docs/design/modules/agent.md) |
| **UE SDK** (`sdk/`, `android/`) | 단말 SDK — C++ 코어 `libcimsue`(pjsua2 위) + Android/Windows 플랫폼 SDK. 관제조작반·PTT·VoLTE 앱의 공통 토대 (설계 정본, Android 부터 구현) | [docs/design/features/ue_sdk.md](docs/design/features/ue_sdk.md) |
| **관제 앱** (`windows/dispatch-desktop`, `android/dispatch-tablet`) | 관제조작반 — 데스크톱(WPF) + 태블릿(Compose). 같은 화면 의미론을 두 플랫폼에 | [dispatch_desktop_ui.md](docs/design/features/dispatch_desktop_ui.md), [android_dispatch_tablet.md](docs/design/features/android_dispatch_tablet.md) |

전체 아키텍처 개요는 [docs/design/01_overview.md](docs/design/01_overview.md), 배포 아키텍처는
[docs/design/02_deployment.md](docs/design/02_deployment.md) 를 본다.

## 개발/빌드 환경

**Prerequisites**: `cmake`, `build-essential`, `libssl-dev`, `libmariadb-dev`, `git`, `clang-format`

```bash
sudo apt-get install -y cmake build-essential libssl-dev libmariadb-dev clang-format
```

`libmariadb-dev` 는 MariaDB **클라이언트 라이브러리·헤더** (DB 서버 아님) — CSP 빌드 필수
(`csp/CMakeLists.txt` 가 없으면 configure 중단). MariaDB 서버는 별도 장비에 둘 수 있다
([docs/DEV_SERVER_SETUP.md](docs/DEV_SERVER_SETUP.md) §1.2, §6).
배포 대상 노드에는 이 패키지를 따로 깔지 않는다 — CSP 패키지가 `csp/vendor/*.deb` 로 런타임
라이브러리를 동봉하고, agent 가 모듈 설치 시점에 설치한다 (`csp/vendor/README.md`).
`clang-format` 은 검증 stage 1 (`S1-CPP-FORMAT`) 의 정적 검사용. 미설치 시 SKIP.

**Build** (out-of-source, 레포 루트에서):
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

첫 빌드는 외부 의존성(oneTBB, opencore-amr, vo-amrwbenc, googletest, psip/pasf)을 내려받아
컴파일한다 (CMake `ExternalProject_Add`). 바이너리는 `build/bin/` 에 생성된다.

**배포 패키지**:
```bash
cd build && make dist     # build/dist/ (컴포넌트별 tarball + manifest)
```

**개발 원스톱**: `./cims.sh up [--skip-build]` — build → configure -y → 전체 재시작.

**단일 프론트/엔진 경계**: 개발 서버에서는 `cims.sh` 가 단일 진입점(프론트) — 빌드/설정/패키징에
더해 기동·상태·로그(`start|stop|restart|status [--full]|log`)와 검증(`verify`)도 서브커맨드로
위임 실행한다. 정본(엔진)은 `agent/bin/cims-svc`(운영 lifecycle — 배포본·agent·OAM 이 직접 호출)
와 `./cims-verify`(검증, 소스 트리 전용). 엔진은 dist 계약만 취급 — 소스 트리 전용 동작(vite dev
콘솔 등)은 프론트에만 둔다.

> cwrtc(WebRTC 게이트웨이)·cims-phone(웹 단말)은 **재설계 예정** — 빌드/패키징/기동 대상에서
> 제외 (소스만 보존).

**실행**: `./cims.sh start` (기동 순서·pid/log 관리는 엔진이 처리). 개별 바이너리 직접 실행 시
CMP 를 CSP 보다 먼저 기동:
```bash
./bin/cmp ../cmp/cmp.json
./bin/csp ../csp/csp.json -n          # -n=foreground (백그라운드는 csp.sh)
```

**시뮬레이터(cspsim)·검증 파이프라인(S1~S6)** — 단말 시뮬레이터 사용법과 상용 배포 전 검증
게이트는 검증 문서가 정본이다. 절차·게이트 정의는
[docs/VERIFICATION_PROCESS.md](docs/VERIFICATION_PROCESS.md), 실행/시뮬레이터 사용법은
[docs/VERIFICATION_MANUAL.md](docs/VERIFICATION_MANUAL.md) 를 본다. 진입점은 `cims-verify`
또는 콘솔 `/testbed/verify-v2`.

> **설정 파일 위치**: `csp/csp.json`, `cmp/cmp.json`, `csc/bin/csc_pihttp/config/csc.json`.
> 가입자/그룹 데이터는 DB(MariaDB) primary, `csp/User/`·`csp/Group/` JSON fallback.
> 각 설정 키의 의미는 해당 모듈 설계 문서를 참조한다.

## 참조 문서 (docs)

상세 설계·동작은 아래 문서가 정본이다. 기능 작업 전 관련 문서를 먼저 읽는다.

**최상위**
- [docs/README.md](docs/README.md) — docs 인덱스
- [docs/DEV_SERVER_SETUP.md](docs/DEV_SERVER_SETUP.md) — 개발 서버 셋업
- [docs/VERIFICATION_PROCESS.md](docs/VERIFICATION_PROCESS.md) / [VERIFICATION_MANUAL.md](docs/VERIFICATION_MANUAL.md) — 검증 절차·매뉴얼

**design/** (아키텍처·플랫폼)
- [01_overview.md](docs/design/01_overview.md) — 전체 아키텍처 개요
- [02_deployment.md](docs/design/02_deployment.md) — 배포 아키텍처/절차
- [ha_design.md](docs/design/ha_design.md) — HA 설계
- [db_schema.md](docs/design/db_schema.md) — DB 스키마 (file_store SoT 포함)
- [console_platform.md](docs/design/console_platform.md) — 콘솔 플랫폼
- [console_design_system.md](docs/design/console_design_system.md) — 콘솔 디자인 시스템 — **시각 계약 정본** (Tailwind + shadcn/ui + Radix. Mantine 안 씀). 적용은 **세 층** — ①방식(하드코딩 CSS·인라인 style 대신 shadcn 컴포넌트)·②규칙(절대 규칙·컴포넌트 계약·토큰)은 **전 31 라우트 + 공통 셸**, ③화면별 구체 값은 도안이 있는 `/deploy/servers`·공통 셸만. **기능(메뉴 소속·라우팅·동작)은 건드리지 않는다.** 시안에 없으면 지우지도 고치지도 않는다(양방향) — **단 도면이 전부 있는 `시스템/인프라` 는 예외로 도안에 없는 것을 지운다(§7-39)**. 원본 자료 = `cims-design-handoff/`(읽기 전용) + Figma MCP 직접 읽기 §6.1, 토큰 §5 · 컴포넌트 계약 §4 · 충돌 판정 §7 · 지켜야 할 검사 §8 · 남은 항목 §9)
- [oam_csc_split.md](docs/design/oam_csc_split.md) — OAM/CSC 분리 경계·인증·토폴로지
- [csc_config_server.md](docs/design/csc_config_server.md) — CSC config server
- [csp_control_plane_load_hardening.md](docs/design/csp_control_plane_load_hardening.md) — CSP 제어평면 부하 대책
- [alarm_standardization.md](docs/design/alarm_standardization.md) — 알람 표준화
- [alarm_self_reporting.md](docs/design/alarm_self_reporting.md) — 모듈 알람/이벤트 자기보고(FM push) 경로
- [alarm_pipeline.md](docs/design/alarm_pipeline.md) — 알람/이벤트 파이프라인 — 발생→전달→수집/보관→가시화 전 구간 절차·연동 계약 정본
- [alarm_catalog.md](docs/design/alarm_catalog.md) — 알람/이벤트 카탈로그 설명서 — 정의 행(기능 관점 **요구**·정의 코드 채번 정본) + 감지 행(모듈 자기감지 **구현 추적**)의 단일 목록 (목록 정본 = [alarm_catalog.csv](docs/design/alarm_catalog.csv))
- [vibcf_pod_alarms.md](docs/design/vibcf_pod_alarms.md) — 사내 vIBCF/TrGW POD 알람/Fault 카탈로그 변환 참고자료 (CIMS 대조 = alarm_standardization §7.2)
- [identifier_model.md](docs/design/identifier_model.md) — 식별자 모델 — 동작은 불변 id 로, 표시는 name 으로. 이름은 어떤 키(파일명·설정 식별자·알람 상관 키)에도 쓰지 않는다 + 재키잉 절차
- [runtime_store_design.md](docs/design/runtime_store_design.md) / [runtime_store_v2_module_namespacing.md](docs/design/runtime_store_v2_module_namespacing.md) — 런타임 스토어

**design/modules/** (컴포넌트별 상세)
- [csp.md](docs/design/modules/csp.md) · [cmp.md](docs/design/modules/cmp.md) · [csc.md](docs/design/modules/csc.md) · [agent.md](docs/design/modules/agent.md)

**design/features/** (기능별 상세)
- [ptt_flows.md](docs/design/features/ptt_flows.md) — PTT(MCPTT) 케이스·메시지 flow
- [volte_flows.md](docs/design/features/volte_flows.md) — VoLTE 호 flow
- [volte_supplementary_services.md](docs/design/features/volte_supplementary_services.md) — 유선 VoIP 보조 서비스 (데스크폰·소프트폰·관제 앱 — USIM 없는 Digest+TLS 가입자 규약(가입 id=E.164, 내선=표시 라벨), 유선 접속서비스 `kind=voip` 에 피처코드·전달 기본값, 당겨받기 축 `pickup_group`=전화 그룹 id(org 폴백 없음)·서비스별 `pickup_feature_code`(전역 `CallPickupId` 제거됨)·지정/그룹 픽업, 호 전달 REFER(blind/attended) — 미디어 재고정은 전부 CMP `RELAY_MODIFY`·SRTP 유지, 표준형=수신 INVITE-Replaces(RFC 3891)·dialog 이벤트 패키지(RFC 4235 BLF)·489. 검증=S3-SCN-XFER/PICKUP/DIALOG. 보조 서비스 로직은 CTasModule 소유(IModule 훅), relay SDES leg 헬퍼는 MediaSdes 공용. `voip` 귀속·폴백 제거 구현 반영 — 기존 회선 `voip` 이관(H(A1) 재결박) §10.3a)
- [mcptt_authorization.md](docs/design/features/mcptt_authorization.md) — **권한 모델 정본** — 역할 = 능력(capability)+범위 하나로 콘솔 관리 권한(내장 프리셋 admin/manager/operator/monitor, 전역)과 관제 권한(감독/관리/전체 프리셋, 한정 범위)을 통합. principal 둘(콘솔 계정 `console:<login>` / 가입자 `user:<users.id>`, 토큰 realm 은 안 섞음), CSC 단일 판정 `can(principal, capability, target)`, 불변 규칙 = `authz_manage` 위임 불가·`allow_ambient_listening` 은 역할 배정의 결과(직접 편집 없음)·수행/감사 열람 분리. PTT 그룹 소유 `authorized_user_id`·GMS XCAP 경로·`group.json`. 후속 = 콘솔 계정 → IdMS 신원 통합(내장 admin 은 OAM 로컬). 역할 모델 구현 반영(CSC `services/authz.py`·`/api/v1/roles`·CSP `CspRole`, 라이브 배포는 정지창)
- [dispatch_center.md](docs/design/features/dispatch_center.md) — 관제 센터 — 세 축: **접속환경**(유선 VoIP `kind=voip`) / **전화 그룹**(`phone_groups` = 픽업 그룹+대표번호 — 유선 전화의 일반 기능, `pickup_group` 값을 그룹 id 로 파생, 가입자당 그룹 1개, 대표번호 병렬 호출 = TS 24.239 Flexible Alerting(포크 집합·최초 200 승자·RELAY_MODIFY 고정)·sequential·링잉 대표번호 픽업(`PickUpFork`)·대표번호 발신 표시(§4.7 PPI)) / **역할**(감청·청취·이력·관리 권한 — mcptt_authorization 정본). **업무망 합법감청**(TS 33.107/33.108·ETSI 정합 — CC 분리 인도·귀속 보존, 서버 믹싱 미규정) = RFC 4235 dialog 이벤트 인가(규칙 1 같은 전화 그룹 BLF / 규칙 2 역할 `monitor_call`) + RFC 3911 Join `a=recvonly` → CMP 청취 leg `RELAY_TAP_*`(양 peer 를 SSRC 2개 분리 인도·RFC 5576 라벨링·믹싱은 단말·상향 폐기·은닉), PTT 그룹콜 청취 = 관제사의 `a=recvonly` INVITE 합류 → `PTT_JOIN recv_only=1` + TS 24.484 `allow_ambient_listening` 자격(역할 배정 시 CSC 동기) + 역할 `ptt_listen` 범위 2단 인가, 로스터 노출 `listen_visibility`, 비멤버 sendrecv 403, 감사 `E-AUD-016`. **PTT 세션 가시성**(§5.6a — PTT 회선 `Event: dialog`, remote=세션 URI+`<mcptt>` 확장, 즉석 세션 관측 인가 `CanObserveEphemeral`). 관리 범위 = 역할 `directory_write`(none|own|all, §3.4 — 관제 앱 `/provisioning/directory/*`, 콘솔 관리 API 와 같은 쓰기 코드·같은 판정) + 녹취 열람 §5.7b. 검증 `S3-SCN-FA`/`S3-SCN-MONITOR`/`S3-SCN-PTT-LISTEN`. **절차(포크·tap·청취·감사)·전화 그룹/역할 분해(§8.1 마이그레이션 `dispatch_groups`→`phone_groups`+`roles`, CSP `CspPhoneGroup`+`CspRole`·CSC·콘솔·검증 시드)·org 폴백 폐기 구현 반영(라이브 배포는 정지창) — 유선 VoIP 접속환경 `kind=voip` 도 구현 반영(§8.3). 자리(관제석)/사람 분리는 보류(§10). 남은 단말 파트 U10·U6**)
- [mcptt_emergency_modes.md](docs/design/features/mcptt_emergency_modes.md) — 긴급/임박/알림/ad-hoc 모드
- [mcptt_standard_conformance.md](docs/design/features/mcptt_standard_conformance.md) — MCPTT 서버(CSC/CSP/CMP) 3GPP TS 규격 정합 보완 사항(단말 interop 전제) + §0-R 미반영 로드맵
- [mcptt_csp_cmp_roadmap_contract.md](docs/design/features/mcptt_csp_cmp_roadmap_contract.md) — 로드맵 기능(private call·dual/multi-talker·pre-established 등) CSP↔CMP 연동 메시지 규격, Call Control/Media Plane 2파트 분담 계약
- [mcdata_messaging.md](docs/design/features/mcdata_messaging.md) — MCData 그룹 메시징(SDS) — TS 24.282 그룹 SDS·TS 24.481 그룹별 게이트·disposition
- [mcx_identity_scope.md](docs/design/features/mcx_identity_scope.md) — MCX 신원·토큰·scope 모델 — 단일 MC service ID(`mcdata_id`=`mcptt_id`, TS 23.280 §10.1.4.1), IdMS 토큰 claim(TS 33.180 Annex B: `client_id`·`scope` 문자열·`mcptt_id`/`mcdata_id`), scope 카탈로그 `3gpp:mc:*` 8종 요청∩카탈로그 발급·구 `3gpp:mcptt:ptt_server` 전환기 별칭(전체 확장·병기), 리소스 서버(GMS/CMS/KMS/MCData FD) scope 검사 `IdMs.ScopeEnforcement` enforce|log|off + RFC 6750 응답, issuer 유도(`IdMs.Issuer` > `McpttServer.PublicUrl` > `idms.<PTT 도메인>`). 향후 = CSP REGISTER 토큰 검증(TS 24.379 §7.3)·MCData XCAP 문서(TS 24.484 §10)
- [ue_nat_traversal.md](docs/design/features/ue_nat_traversal.md) — 단말 NAT traversal (시그널링·미디어 leg 포트·목적지 latch·정책)
- [registration_binding_set.md](docs/design/features/registration_binding_set.md) — 등록 바인딩 집합 (AoR 당 도달 경로 여러 개, flow 생존 판정으로 선택 — transport 혼합 운용의 토대. 정리 정책 3계기)
- [sip_tls_signaling.md](docs/design/features/sip_tls_signaling.md) — SIP 시그널링 transport (UDP/TCP/TLS **단말 선택** 지원. transport 별 도달 모델 = 목적지 주소 vs 연결 열쇠, 접속점 개설 실패 격리·A-PRC-012, 인증서 = **2단 PKI** §8 — 개발사 오프라인 루트 → 사이트 CA → 서버 leaf. 단말 앵커는 루트 한 장(APK 동봉), 서버 IP 변경·leaf 갱신·사이트 CA 교체는 현장 안에서 끝나고 APK 무관, 현장↔개발사 무네트워크 전제. 기간 = 사이트 CA = 루트 만료일(갱신 이벤트 없음) · leaf 2년(자동 갱신 대상) · 루트는 **현 세대(10년, 2036) 유지 — 임시 사이트 배포 단계, APK 무변경** → **1.0 안정화 이후 루트 재발급**(기간은 그때 결정, 오프라인 생성 `CIMS Root CA G1`, 병기 후 제거 = APK 1회, 사이트 CA 는 같은 키 재서명·leaf 무변경). **만료 방어 §8.6** = 사이트 CA 는 그룹 CA 키의 루트 교차 인증서(관리평면 앵커 불변) → lifecycle 엔진이 잔여 60일 자동 재발급(일일 스윕·CSP 편입·강등 금지) + A-PRC-009 자동 갱신 실패·콘솔 배너·관제 앱 경고·verify 게이트, 임계 60/30/7 단일 정의, 유예 없음. **구현 반영** = 엔진 E1~E7(`cert.sh`·`cims-svc cert`·agent 일일 스윕·갱신 상태→heartbeat `cert_renew`→OAM `cert_renew_failed`)·CSP 같은 경로 내용 교체 지문 재적재(psip `ReloadTlsListenerCert`)·`service-cert.sh site-ca {issue,csr,sign --cross}`+루트 기준 `verify`·join `ca-cross.crt` 복사·SDK `tlsPeerExpiry`+Windows 관제 앱 배너(전 화면)·요약 띠 경고·콘솔 셸 배너 `CertExpiryBanner`·Android 설정 "서버 인증서" 행(`TlsPeerObserver`, pjsua2/OkHttp 관측)·S3-HEALTH 잔여 게이트+`S3-SCN-TLS-CERT-RENEW`. 사내 .45 는 교차 인증서 체인 전환 완료. 잔여 §9 = 루트 키 오프라인 이관·FQDN(선택)·루트 재발급(1.0 이후))
- [sip_access_security.md](docs/design/features/sip_access_security.md) — SIP 접속 보안 (TS 33.203 정합 로드맵 P0~P4. P0=채널 정책 게이트(`sip_transport=TLS` 집행, psip listener id 전 transport 전파), P1=인증 자료 경계(H(A1) SoT·CSC 단일 쓰기 주체·Digest 정비·`/provisioning/me` sipHa1). P2=Sec-Agree(RFC 3329 협상·강등 방지 494/421, 협상 결과의 게이트 합류, Service-Route `;transport=`). P3=IMS AKA over TLS(Annex X — CSC AuC: Milenage·K/OPc 암호화 보관·`POST /internal/aka/av` 단일 SQN 발급자, CSP `AKAv1-MD5` 챌린지/검증/AUTS 재동기, `auth_scheme` 프로비저닝 고착, psip/cspsim 소프트-USIM). P4=IMS AKA+IPsec(본문 §6~7 — Annex M 미포함, NAT 와 access service `sec_mechanisms`/등록 시 감지 두 겹 상호배제, psip `XfrmSa` netlink 로 ESP SA 4개, `cims-priv setcap-net-admin` 으로 `CAP_NET_ADMIN`, psip/cspsim 단말 한정). P0~P4·§4.7 ⑤·⑥ 구현 반영(Android 는 sec-agree·AKA 자격 연결 후속) — 값 소거·컬럼 DROP 스크립트 적용은 운영 절차(코드는 `passwd` 컬럼을 읽지 않음). 보안 알람 A-SEC-003(채널 정책 403)·A-SEC-004(sec-agree 494/421) 급증)
- [leg_liveness.md](docs/design/features/leg_liveness.md) — 비정상 종료 leg 감지 (SIP 세션 타이머 RFC 4028 — BYE 없이 사라진 leg 의 시한 회수) + **서버 발신 in-dialog 요청 목적지 재해석** §6.3 (BYE·re-INVITE·NOTIFY·REFER·INFO 를 생성 직전 `EventGetLegDest` 의 살아있는 등록 바인딩으로 — NAT 뒤 단말의 승격 TCP 가 닫힌 뒤 상대 종료 BYE 유실 방지. psip `SipUserAgentLegDest.hpp`, 단위시험 S1-UNIT-PSIP). 구현 반영
- [media_security.md](docs/design/features/media_security.md) — 미디어 보안 (UE↔CMP 구간 SRTP — RFC 3711 + SDES(RFC 4568), TS 33.328 e2ae. 접속서비스 `media_srtp` off/optional/required + TLS 결합, 능력 학습=등록 시 `Security-Client: sdes-srtp`+`mediasec`(TS 24.229, sec-agree 확장 — per-call 폴백 없음), CSP SDP 협상·키 생성, CMP leg 별 종단(녹취·믹스·U10 디먹스가 평문 전제)·`media_crypto` 제어 확장(cmp_media_api §6.4) — 엔진=libsrtp2 `ext/libsrtp` 독립 vendoring(재협상=세션 재생성, `srtp_update` 금지), cspsim `-srtp`, 검증=S1 단위+`S3-SCN-SRTP`. 단말=pjsip SRTP 빌드+앱 `mediaSecurity`(프로비저닝 `sip.mediaSecurity` — CSC `Provisioning.Services.<kind>.media_srtp`, TLS 접속만 srtpUse 반영)+`Security-Client: sdes-srtp;mediasec` 병기, 콘솔=접속서비스 `media_srtp` 스키마 편집, VoLTE relay=leg 별 crypto 재작성·종단(모든 SDP 전달 지점 strip+재광고, 전환은 strip 만). 잔여=정지창 S3/실기기·와이어 실측, 협력업체 SDK 재빌드. E2E KMS/MIKEY-SAKKE·MSRP 보안은 로드맵)
- [recording.md](docs/design/features/recording.md) — 녹취 구조 (슬롯 트랙·믹스/단독 재생·PTT 세션 이력 UI)
- [flow_logging.md](docs/design/features/flow_logging.md) — SIP/Flow 로깅 (sesid 규칙·5분 버킷)
- [monitoring.md](docs/design/features/monitoring.md) — 모니터링
- [sip_statistics.md](docs/design/features/sip_statistics.md) — SIP 호·메시지 통계 (세는 단위 3계층 attempt/session/leg — 그룹통화·감청·PBX 로 leg 이 늘어도 비율이 흔들리지 않게. 성공률·소통률·완료율·참여율, 1분 기저 롤업 피라미드(분~월 정수배 유도), 미결 호 추적, 보존 계층. 구현: 서비스 판정 단일화(`services/access_services`)·VoLTE 3지표·1분 기저 집계(`services/stats_rollup`, oam-svc 주기 실행). 잔여: 조회 API 통일(§7)·PTT 시도 기록 Y6)
- [sip_service_model.md](docs/design/features/sip_service_model.md) / [sip_runtime_config.md](docs/design/features/sip_runtime_config.md) — SIP 서비스 모델·런타임 설정 (접속서비스 `kind` = 접속환경 클래스 `volte` 이동 / `voip` 유선 / `ptt` — `voip` 구현 반영: CSP kind 검증·같은 전화 경로·로그/통계 서비스축 합산 `LogServiceOf`, CSC `service_ref`↔서비스 name 매칭 프로비저닝, 관제 앱 전화 회선 voip 우선. **CSC 접속서비스 읽기 경로 단일화** `services/access_services` — 정의는 관리 store 미러(CSP 정본) · csc.json `Provisioning.Services` 는 단말 도달 정보+미러 부재 폴백. 가입 테이블 = kind(`volte_subscriptions`/`voip_subscriptions`/`ptt_subscriptions`, exact-kind 게이트))
- [ha_service_model.md](docs/design/features/ha_service_model.md) — HA 서비스 운영 모델 (책임 분리·선언적 verdict·절체 판정 — 설계 정본, 단계적 이행)
- [oam_base_service_split.md](docs/design/features/oam_base_service_split.md) / [oam_self_upgrade.md](docs/design/features/oam_self_upgrade.md) — OAM base/service 분리·self-upgrade
- [oam_ha.md](docs/design/features/oam_ha.md) — 관리평면(OAM) 이중화 A/S (서비스 그룹 동거·모듈별 health, 공유 스토리지(NAS) + 소유권 리스 펜싱, 그룹 공통 신원, 자기 계획절체 — 설계 정본)
- [auto_deployment.md](docs/design/features/auto_deployment.md) — 자동 배포 (인벤토리+블루프린트 YAML → SSH agent 설치·시스템 구성·모듈 설치. **OAM 내장** — `services/provision/`, 콘솔 `관리>릴리스>자동 배포`, CLI `scripts/prov`)
- [api_docs.md](docs/design/features/api_docs.md) — 위젯별 사용 API 노출 (모듈이 코드 옆에 자기 API 선언 + 위젯이 쓰는 id 선언 → 개발자 모드 `[API]` 배지)
- [csc_standalone_module.md](docs/design/features/csc_standalone_module.md) — CSC 독립 모듈화
- [build_and_packaging.md](docs/design/features/build_and_packaging.md) / [package_and_template.md](docs/design/features/package_and_template.md) — 빌드·패키징·템플릿
- [os_portability.md](docs/design/features/os_portability.md) — OS 이식성 — 실행 조건은 배포판 이름이 아니라 **능력 셋**(x86_64 · glibc 2.38+ · CPython 3.14). 원칙 = 분기가 아니라 축(모듈 소스에 OS 분기 없음, 갈라지는 곳은 설치·기동 경계). 인터프리터 선택 단일 규칙(`--python` > 동봉 > `python3.14` > `python3`, **버전을 물어서** 판정 — agent 의 sys.executable → `CIMS_PYTHON` → `PYBIN` 으로 전 파이썬 모듈에 전파, 구현 3곳 복제 명시) · 패키지 계열 축 debian/rhel(TB 05 단계·철거·모듈 vendor·agent HA) · 반입 rpm 수집 `tb-fetch-rpms.sh`(대상과 같은 배포판 장비에서) · OS 의존 지점 목록(상류 대조용) · 확인 조합 Ubuntu 26.04 / Rocky Linux 10.2. 남은 것 = 철거 rhel 갈래 · HA/NFS rpm · SELinux · 인터프리터 동봉
- [android_ue_client.md](docs/design/features/android_ue_client.md) / [android_ue_m1_pjsip_integration.md](docs/design/features/android_ue_m1_pjsip_integration.md) — Android UE 클라이언트
- [mcptt_ue_multitalker_media.md](docs/design/features/mcptt_ue_multitalker_media.md) — 단말 동시 발언 미디어 평면(U10) 선택지·구현 설계 + floor 코덱 공유/정의 단일화 검토 (pjproject·안드로이드 빌드 환경 필요)
- [android_ue_provisioning.md](docs/design/features/android_ue_provisioning.md) — UE 로그인·자동 프로비저닝(서비스별 프로파일, CSC `/provisioning/me`)
- [android_dispatch_tablet.md](docs/design/features/android_dispatch_tablet.md) — 관제조작반 **Android 태블릿** 앱(`android/dispatch-tablet`) — [dispatch_desktop_ui.md](docs/design/features/dispatch_desktop_ui.md) 의 화면 의미론을 태블릿 밀도(§12)로 이식한 정본. 층 = 앱 → `:cimsue`(Kotlin 파사드 + SWIG) → `libcimsue`(C++ 코어), 앱은 `com.cims.ue.sdk.*` 만 쓴다(§2.3). 엔진 단일화 = `org.pjsip.**`·`libpjsua2.so` 제공처가 `:cimsue-engine` 하나(§2.2, 커밋 산출물 폐기). **수명** = Foreground Service 가 세션을 들고 Activity 는 화면만(§6.1), 화면 VM 은 `ViewModelStore` 가 아닌 `MainViewModel` 소유라 `ScreenViewModel.close()` 로 수명을 명시(§6.1a). **착신은 배너 + 알림·벨소리 두 표면**(§6.2a — 카드만으로는 다른 탭·화면에서 못 본다). **주소록** = `service=volte` 가 전화 가족(volte∪voip) 합산 축, PTT 는 따로(§6.2b). **요청 대상은 `tel:` 을 벗겨 넘긴다**(§6.2c — 코어 `normalizeTarget` 이 `tel:` 을 그대로 두어 라우팅 불가). 화면 = 하단 내비 넷 + [관제] 안 탭 둘(§6.3), [이력]·[PTT 그룹]·[관리]·감청 시트·요약 띠(§6.10~§6.13). 검증 `S1-UE-*` 7종(§9), 무전/통화 분리 출력 실측(§8)
- [ue_sdk.md](docs/design/features/ue_sdk.md) — 단말 SDK `libcimsue` — 플랫폼 중립 C++ 코어(sip·media·floor·mcdata·csc·domain, pjsua2 위, 공개 헤더 = 바인딩 정본) + Android SDK(SWIG Java+Kotlin 파사드+Android 접점, AAR) + Windows SDK(C++ DLL + C API `cimsue_c.h` + .NET 파사드 `CimsUe.dll`, 앱은 WPF/C#). 엔진 = `ext/pjproject` 단일 소스 정본(config_site 플랫폼별, Linux/NDK/MSVC 같은 트리), `cimsue-cli` 헤드리스 UE 로 S3 검증(등록·1:1·TLS/SRTP·그룹콜 floor·SDS·CSC PKCE 프로비저닝·관제 dialog/Join/픽업/전달 실측). floor 정의 정본 = [mcptt_floor_defs.yaml](docs/design/features/mcptt_floor_defs.yaml) + `scripts/gen_floor_defs.py`. 관제조작반 앱(Windows+Android 태블릿) 요구↔API 매핑 §7. 이행 순서 A~F(A~D 코어·관제 API 구현 완료, Windows = F1 엔진·코어 빌드 확정 → F2 C API·.NET 파사드(단위시험 50건)·WPF 관제 앱 **구현 완료** → 실기 시험 → F3 영상 — §6 결정표·`sdk/windows` 슈퍼빌드)
- [test_instrument.md](docs/design/features/test_instrument.md) — 계측기 `oam-cims-tester` 계획 — `oam`(base) 게이트웨이 뒤 서비스 모듈(`oam-svc` 규약 그대로: `ems/tester/oam`, 세그먼트 `/api/v1/tester`, loopback 4490, JwtSecret 주입, `standalone`) + 콘솔 팩 `ems/tester/console`(`@tester`, nav 그룹 `test`=M.3400 Testing) + `cims-tester-worker`(`tester/worker`, C++ — cspsim `SimSession/RtpThread` 를 `libcsim` 으로 승격, `ue` 풀 + `peer` 엔진 프로파일 ibcf/pbx/mgcf + `real-ue`=cimsue-cli, agent 배포·감독). 운영 형태 = 독립(계측기 호스트 자기 `oam`) / 동거(대상 `oam` 뒤 `oam-svc` 와 나란히) — 블루프린트 차이만. 시나리오 YAML 3층(토폴로지·시나리오·부하 프로파일), 지표 = RFC 6076 + ETSI TS 186 008 + sip_statistics 3계층. **base 확장 셋**(A 단계) = 게이트웨이 SSE 청크 통과 · 콘솔 번들 하나(`oam` 동봉, 서비스 모듈 패키지는 콘솔 미동봉)+nav 섹션·라우트 `requiresService` 게이팅 · nav 그룹 `test`. 이행 A~F(A·B·C 구현), §11 확정/제안 항목, §12 시험이 드러낸 CSP 과제 — RouteSet OPTIONS 헬스체크·Reason Q.850 투과·최종 응답 코드 투과(503→603 폐기)·PTT 그룹 video answer(RFC 3264 port 0)·NNI TLS 클라이언트 인증서/`tls_verify` 는 **CSP 반영(배포 뒤 재실측)**, 18x SDP 미디어 앵커링·피어링 접속점 인증 생략은 반영·정상 확인. 남은 CSP 과제 = 트렁크 REGISTER 계정·G.711↔AMR-WB 트랜스코딩
- [dispatch_desktop_ui.md](docs/design/features/dispatch_desktop_ui.md) — 관제조작반 앱 UI(Windows WPF `windows/dispatch-desktop`, Android 태블릿 밀도 §12) — **PTT 채널 중심**, 화면 한 장(1920×1080 무스크롤)을 **3×2 패널 격자**로: 상단 ① 내 채널(멤버 그룹+내가 건 사설콜·애드혹, 항상 전부 표시, 빠른 발신 줄+팝오버, **발언 바** = 40px PTT + 카드 체크로 **다중 채널 동시 발언** — 대상별 승인/대기/거부) · ② 범위 채널(청취·관리 범위 그룹 + 타인 간 사설콜·애드혹, 필터·검색은 여기만, 청취 토글·편집 드로어) · ③ 일반통화(한 열 — 빠른 발신·[▦▾] 다이얼패드/주소록/최근 팝오버·[문자] SMS/LMS 팝오버(상시 패널 없음)·그룹원 띠·대표번호 대기열·오늘 데스크·내 통화, DTMF 는 카드 팝오버), 하단 ④ PTT 메시지(포커스 채널 따라가기) · ⑤ PTT 이벤트(진행 중 행 없음, 포커스 필터) · ⑥ 일반통화 내역. 채널 카드 2줄/포커스 3줄, **포커스(보는 채널) ≠ 발언 대상(말하는 채널)**, 사람 메뉴·`Ctrl+K` 통합 검색. **3×2 배치 구현 완료**(패널 6개 도킹 · `TalkBarViewModel` 발언 바 · `ScopedChannelsViewModel` · 팝오버 호스팅(사설콜/애드혹·▦·문자·DTMF·전달) · 채널 편집 드로어 · 사람 메뉴/Ctrl+K · 오늘 데스크 — `--ui-preview-canvas` 로 렌더 점검) — 남은 것 = SDK 팬아웃 뒤 다중 발언 활성(`MultiTalkSupported`), 발언 세트, ② 타인 세션(서버 과제), 실기 e2e 재시험 · **PTT 그룹 생성·편집·삭제** = 관제사(가입자)가 GMS XCAP PUT/DELETE(TS 24.481, 자격 `ptt.allowCreateGroup`·본인 소유) — 앱 [PTT 그룹] 화면 인라인 편집 폼 `GroupEditView`(별창 없음)·`RefreshGroupsAsync`·xcap-diff 자동 재조회, SDK `CscClient.putGroup/deleteGroup`+`GroupDoc`. 활성 세션 발견은 프로비저닝 `dispatch.members[]/pttTargets[]`(계약 [docs/design/features/android_ue_provisioning.md](docs/design/features/android_ue_provisioning.md) §3). **상단 바 최상위 메뉴 4개** §3.4 = [관제 F1](도킹 캔버스) · [이력 F2](§4.6 끝난 세션 날짜 창 조회 `/provisioning/history?until=` — 콘솔 `/service/history/volte|ptt` 와 같은 구성: 시간대 밴드·통화 표(상태/시작/응답/종료/종료사유)·PTT 세션 카드 + 선택 세션 패널(참여자·발언 타임라인 막대 클릭 재생·floor/입퇴장 이벤트 타임라인, 상세 `/provisioning/history/ptt/{recordingId}`) — PTT 창 조회·상세는 CSC 가 OAM 세션 인덱스·세션 이벤트를 범위 게이트 뒤에서 프록시 + 녹취 재생 `/provisioning/recordings/*` = CSC 범위 게이트 + oam-svc 프록시, MP4/AAC) · [PTT 그룹 F3](§4.7 범위 안 전부, GMS XCAP) · [관리 F4](§4.5 조직/구성원/VoLTE·VoIP·PTT 번호 — 회선 카드 셋·transport ANY 명시값, 관제 그룹 `directory_admin` 범위 own|all, `/provisioning/directory/*` — 범위 없으면 비활성+툴팁) — 별창이 아니라 같은 창의 레이어 전환(도킹 호스트 상시 마운트), 관제 밖 화면 위 **관제 요약 띠** §3.5(발언 대상·발언 상태·PTT·대기열·문자 미읽음·내 통화·감청 창·[관제로]), 세션을 만드는 조작은 관제로 자동 복귀, 화면 [↗ 별창으로](`ScreenWindow`), 화면 VM 은 앱 수명 동안 하나(폼 유지)

**api/**
- [admin_api.md](docs/api/admin_api.md) · [collection_api.md](docs/api/collection_api.md) · [agent_api.md](docs/api/agent_api.md) · [mcptt_api.md](docs/api/mcptt_api.md) · [cmp_media_api.md](docs/api/cmp_media_api.md) — CMP 미디어 서비스 제어 API (UDP JSON envelope v2)

**user-manual/**
- [initial_install.md](docs/user-manual/initial_install.md) — 초도 설치 절차 (DB 부트스트랩 → `install.sh` → 콘솔 배포 → CSP `local_nodes` 시드 → 철거/재설치)
- [deployment_workflow.md](docs/user-manual/deployment_workflow.md) · [ue_interface.md](docs/user-manual/ue_interface.md) · [volte_ue.md](docs/user-manual/volte_ue.md) · [ptt_ue.md](docs/user-manual/ptt_ue.md)

## 개발/기능보완/문서 현행화 원칙

**설계 우선순위 (기능 보완/추가 시)** — 아래 순서로 판단한다.
1. **표준규격 준수가 최우선.** 관련 3GPP TS/RFC 규격에 부합하는지를 가장 먼저 본다. 규격과
   어긋나는 편의 구현은 채택하지 않는다.
2. **규격에 부합하는 선에서 체계성·일관성을 중시한다.** 자원 모델·명령 체계·명명·계약을
   통일된 구조로 설계한다 (일회성 예외를 늘리지 않는다).
3. **기존 구현에 얹는 최소 보완(band-aid) 구조 제안은 최소화한다.** "기존 코드에 최소 변경"을
   이유로 규격·체계성을 희생하는 방향은 우선 제안하지 않는다 — 올바른 구조를 먼저 제시하고,
   최소 보완안은 명시적으로 요청받거나 정당한 근거(호환 전환기 등)가 있을 때만 대안으로 든다.

**코드**
- 주변 코드의 관습(명명·주석 밀도·구조)을 따른다. 독자적 스타일을 새로 도입하지 않는다.
- C/C++ 는 out-of-source `build/` 에서 빌드하고, 배포는 `make dist` 산출물(`build/dist/`)을 통한다.
  레포 소스 직접 실행과 배포본은 구분한다.
- 기능 작업 전 **관련 design/features 문서를 먼저 읽는다.** 문서와 코드가 어긋나면 코드를 정본으로
  보되, 그 차이를 문서 갱신으로 해소한다.

**콘솔 UI (프론트)** — 규칙은 콘솔 폴더 작업 시 자동 로드되는 [ems/core/console/CLAUDE.md](ems/core/console/CLAUDE.md) 가
정본이다(시각 계약 [docs/design/console_design_system.md](docs/design/console_design_system.md) 를 먼저 읽는다).

**기능 보완 = 코드 + 문서 동시 갱신**
- 동작·인터페이스·설정 키를 바꾸면 해당 docs 문서를 같은 변경에서 갱신한다.
- 새 기능은 design/features 에 정본 문서를 두고, 본 CLAUDE.md 의 참조 인덱스와 개요 표에 한 줄로 등록한다.
- 검증이 필요한 변경은 S1~S6 게이트로 확인한다 (최소 관련 stage).

**문서 현행화 = 최종 상태만 기술**
- 문서는 **현재 동작(최종 상태)** 만 서술한다. 변경 히스토리·진행 로그는 남기지 않는다.
- 제거 대상: `작성일`/`최종 수정`/`버전 N (날짜)` 헤더, 하단 "변경 이력" 표, `Phase/P1~PN ✅완료`
  진행 로그, `구(old) X → 새 Y` before/after 주석, 날짜·배포버전 결합 검증 이력, 사후 회고 노트.
- `구 X → 새 Y` 가 최종 동작 Y 를 서술하는 유일한 곳이면, 이력 부분만 떼고 **Y 를 현재 사실로
  재작성**해 정보 손실을 막는다.
- 보존 대상: 메시지 flow 다이어그램, 표, 설정/스키마, 규격 참조(TS/RFC), 교차링크, 미구현/향후 과제.
- 변경 이력은 git 으로 추적한다. 문서 안에 중복 기록하지 않는다.
