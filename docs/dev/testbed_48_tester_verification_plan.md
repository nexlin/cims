# .48 배포본 계측기 검증 계획

대상 = `121.161.164.48` 배포·시험 환경(관리평면 4419, CSP dep34 · CMP dep32 · CSC dep31 · CMDP · OAM-svc), 계측기 = 같은 OAM 뒤 `oam-cims-tester`(dep35) + 워커 `cims-tester-worker`(dep36, 대상과 동거), 토폴로지 = **#2 `tb48`**(소스 관리본 [ems/tester/oam/scenarios/topology.tb48.yaml](../../ems/tester/oam/scenarios/topology.tb48.yaml)). 시나리오 정본은 계측기 동봉본 66종(volte 30 · ptt 13 · trunk 23), 판정은 계측기 verdict(기대치 + `target_evidence`)다.

절차 문서: 배포·컬렉션 주입은 [oam_api_deploy_runbook.md](oam_api_deploy_runbook.md), 계측기 모델은 [test_instrument.md](../design/features/test_instrument.md), 시험이 드러낸 CSP 과제는 같은 문서 §12.

## 1. 목표와 산출물

| 목표 | 산출물 |
|---|---|
| 동봉 시나리오 전부를 **현 배포 빌드(csp 0.2.139)** 위에서 한 번씩 돌려 pass/fail/차단 을 확정 | run 색인(`/test/results`, 토폴로지 `tb48` 필터) + 회귀 표(§6) |
| tb48 이 못 도는 시나리오의 **토폴로지 빈칸**을 채워 66종 전부를 실행 가능한 상태로 | tb48 v3(풀 8개 추가·hosts.ssh) — 소스 관리본 갱신 + store #2 PUT |
| cims-verify S3/S6 의 계측기 다리를 .48 대상으로 **게이트 모드** 실행 | verify 보고서(항목마다 run id) |
| 실패를 **계측기 결함 / CSP·CMP 과제 / 환경** 셋으로 갈라 기록 | §12 표 갱신, 새 과제는 로드맵으로 |

## 2. 현 상태 (2026-09-21 run 색인 기준)

**A. tb48 에서 pass 한 것 (13)** — 빌드가 섞여 있어(0.2.137~0.2.139) 1단계에서 0.2.139 로 전부 재실측한다.

| 시나리오 | 마지막 pass 빌드 |
|---|---|
| VOLTE-CALL-BASIC · TRUNK-PBX-OUTBOUND · TRUNK-IBCF-FAILOVER · TRUNK-IBCF-FAILOVER-5XX | 0.2.139 |
| VOIP-REGISTER · TRUNK-PBX-TRANSCODE · TRUNK-PBX-INBOUND-NATIONAL · TRUNK-MGCF-REJECT-Q850 | 0.2.138 |
| VOLTE-CALL-NATIONAL-DIAL · VOLTE-CALL-INTL-DIAL · VOLTE-TLS-REGISTER · TRUNK-PBX-REGISTER-REG · MCDATA-FD-GROUP | 0.2.137 |

**B. media01-dev(토폴로지 #1) 에서만 pass — tb48 미실측 (9)**: MCDATA-SDS-1TO1 · MCDATA-SDS-GROUP · MCDATA-SDS-GROUP-MEDIA · PTT-GROUP-CALL-BASIC · VOLTE-CALL-UE-EARLY-MEDIA · VOLTE-FA-PARALLEL · VOLTE-FA-OVERFLOW · VOLTE-FA-SEQUENTIAL · VOLTE-FA-DIALOG-FORK. 그리고 PTT-GROUP-CALL-VIDEO 는 fail(g001 이 `video_enabled` 아님 — 규격상 port 0 거절, 환경 원인).

**C. tb48 현 풀로 돌 수 있는데 미실측 (34)**

| 축 | 시나리오 | 전제 |
|---|---|---|
| VoLTE 기본 | VOLTE-REGISTER · VOLTE-CALL-SIGNALING · VOLTE-CALL-ONEWAY-MEDIA · VOLTE-CALL-VIDEO · VOLTE-SUBSCRIBE-BAD-EVENT | 없음 |
| 보조 서비스(S3 축) | VOLTE-PICKUP-GROUP · -DIRECTED · -DIRECTED-DENIED · -GROUP-NOCALL · VOLTE-XFER-BLIND · -ATTENDED · -DENIED · VOLTE-BLF-PICKUP · VOLTE-BLF-DENIED · VOLTE-MONITOR-JOIN · VOLTE-FA-PICKUP | 대상 DB 픽스처(`pickup_group`·`phone_groups`·역할·`service_ref`) 를 계획의 역할 신원에 입혀야 한다 — §4 3단계의 verify 다리가 자동으로 하고, 단독 실행 때는 수동 시드 |
| PTT | PTT-FLOOR-HANDOVER · PTT-AFFILIATION-CHURN · PTT-GROUP-NONMEMBER-DENIED · PTT-GROUP-LISTEN · -LISTEN-DENIED · -LISTEN-ROSTER · -LISTEN-CONF-DENIED | LISTEN 4종·NONMEMBER 는 `member: false` 역할 = **g001 밖 PTT 신원**이 creds 에 있어야 한다(현 `ptt.jsonl` 은 g001 8명만) + 청취 역할(`ptt_listen`) 배정 |
| MCData | MCDATA-FD-1TO1 | 없음(FD-GROUP 과 같은 전제) |
| 피어 | TRUNK-IBCF-OUTBOUND · -INBOUND · -ACL-DENY · TRUNK-MGCF-OUTBOUND · -INBOUND · -EARLY-MEDIA · TRUNK-PBX-INBOUND · -DTMF · -HOLD-RESUME · -TRANSFER | `Setup.Roles.IBCF=true` overlay(적용됨)·`PBX_HA1` env(agent#13 상속, 적용됨) |

TRUNK-PBX-REGISTER(동봉본)는 고정 IP 풀 `pbx_hq` 를 참조해 tb48 에선 compile fail 이 정상 — 등록형은 TRUNK-PBX-REGISTER-REG 로 대체하고 표에는 "대체"로 적는다.

**D. 토폴로지 확장이 필요한 것 (9)**

| 시나리오 | 부족한 것 | 채우는 법(sample 토폴로지의 같은 이름 풀을 옮긴다) |
|---|---|---|
| TRUNK-IBCF-RETRANS-LOSS | `peer_kt_lossy` | ibcf 피어, `fault.drop_invite: 1`, 새 포트 |
| TRUNK-IBCF-THIG | `peer_kt_thig` | ibcf 피어 `thig: true` |
| TRUNK-IBCF-TLS-MUTUAL | `peer_kt_tls` + CSP **TLS 피어링 접속점** | 풀 `bind.protocol: tls`·`tls_client_auth/verify/cert: true`, `listener: tls`(access TLS 에 붙임 — 피어는 access 항목도 된다) · 워커 `Tls.CaFile` = 사이트 CA · CSP 노드 인증서를 워커가 신뢰 |
| TRUNK-MGCF-DTMF-INBAND | `mgcf_inband` + `volte_ue_inband` | mgcf 피어 `dtmf: inband`, UE 풀 `dtmf: inband` 에 **다른 `source.offset`**(신원 겹치면 컴파일 오류) |
| TRUNK-PBX-G722 | `pbx_wb` | **제외(§5 결정 1)** — 사이트 PBX 코덱 G.711A, CSP·CMP 는 G.722 relay 만 |
| VOLTE-CALL-NAT | `volte_ue_nat` | **진행 안 함(§5 결정 4)** |
| VOLTE-CALL-REAL-UE | `real_ue` | `kind: real-ue`(워커 패키지에 cimsue-cli 동봉), 별도 신원 offset, 워커 `RealUe.*` |
| VOLTE-CALL-SOAK-LEAK | `hosts.h48.ssh` + `nodes.csp.procs/logs` | 운영자 `ssh-copy-id -p 10022`, 컨트롤러 env `TESTER_SSH_KEY`(agent#13 env 상속 → agent 재기동) — `rss_growth_mb/fd_growth` 증거가 여기서 나온다 |
| PTT-GROUP-CALL-VIDEO | g004(`video_enabled`) 멤버 | g004 멤버 추가(tb48 항목 5) + creds `--ptt-group g004` |

## 3. 판정 규칙

- **pass** = 계측기 verdict pass. fail 은 실패 사유를 반드시 세 갈래 중 하나로 적는다: ⓐ 계측기 결함(시나리오·컴파일·워커) ⓑ CSP/CMP/CSC 과제(§12 등재) ⓒ 환경(픽스처·env·인증서·번호 블록).
- 같은 빌드에서 두 번 연속 같은 결과여야 확정. 간헐 실패는 `compare` 로 두 run 을 나란히 두고 SIP 드로어(`sip/<call_id>`)로 원인을 적는다.
- 판정 기준선(baseline) = 1단계가 끝난 시점의 run 집합. 이후 배포마다 같은 순서로 돌려 `compare` 회귀 판정을 본다.

## 4. 단계

### 0단계 — 준비 (반나절)

1. **회귀 스크립트** `scripts/tester-regress.sh`(신규): 시나리오 목록 파일을 읽어 `cims-tester --url --token run <id> --topology tb48 --json` 을 순차 실행(run 당 timeout 240 s, 풀 교체 시간 포함), 결과를 `id | verdict | run_id | 요약` 표(md)로. 워커 풀 교체가 겹치지 않게 **순차만**. 실패는 계속 진행(fail-fast 아님).
2. **creds 보강**(`cims-tester creds-from-db`, DataDir `scenarios/creds/`): ptt.jsonl 에 g001 밖 신원 추가(청취자·비멤버 후보 — `--ptt-group` 없이 뽑아 `group` 비움 또는 다른 그룹), inband/real_ue 용 volte 신원 구간(offset 겹침 금지), g004 멤버(항목 5 뒤).
3. **tb48 v3 토폴로지**: D 표의 풀 8개 + `hosts.h48.ssh`(키 배치 뒤) 를 소스 관리본에 넣고 `PUT /tester/topologies/2` → `compile-check` 66종 전부 ok 확인(REGISTER 동봉본 1건은 예상 fail). `docs/dev/testbed_48_tester_topology.yaml` 은 소스 동봉본과 같은 내용이므로 **동봉본만 남기고 삭제**(중복 정본 방지).
4. **대상 측 설정**(dep34 overlay·collection, runbook 절차): TLS 피어 `remote_nodes.tls_verify` 는 시드가 넣는다 — CSP 가 워커 피어 인증서를 검증하려면 사이트 CA 가 `tls_ca_path` 에 있어야 하므로 워커 피어 인증서는 **같은 사이트 CA 로 발급**(`service-cert.sh`). CMP RTP 대역은 §5 결정 2(20000~29999) 대로 overlay.
5. **비밀 등록**: dep35 배포 설정 `Tester.Secrets` 에 `PBX_HA1`(CSP routes `tb48-r-pbx_reg.auth_ha1` 과 같은 값)·`TESTER_DB_USER/PASS`·`TESTER_SSH_KEY`(PEM 본문) 를 `PUT /deployments/35/config` 로 등록 → restart(또는 SIGUSR1). 환경변수는 폴백일 뿐 agent 재기동에 의존하지 않는다.

### 1단계 — 기능 회귀, 현 풀 (하루)

순서는 실패 파급이 작은 쪽부터. 각 묶음 끝에 회귀 표를 갱신한다.

| 묶음 | 시나리오 | 비고 |
|---|---|---|
| 1-1 등록·기본 호 | A 재실측(VOLTE-CALL-BASIC·NATIONAL·INTL·TLS-REGISTER·VOIP-REGISTER) + VOLTE-REGISTER·CALL-SIGNALING·ONEWAY-MEDIA·VIDEO·UE-EARLY-MEDIA·SUBSCRIBE-BAD-EVENT | VIDEO 는 UE 간 m=video 협상(`video_pct`) |
| 1-1a 노드별 착/발신 | `NODE-CALL-{udp,tcp,tls}-{udp,tcp,tls}` 9종(접속점 access-udp/tcp/tls 3×3 매트릭스) · `NODE-CALL-VOIP-VOIP/VOIP-UDP/UDP-VOIP` · `NODE-PEERIN-<peer>-UDP`/`NODE-PEEROUT-UDP-<peer>`(pbx_hq·pbx_reg·mgcf_pstn·peer_kt 착/발신) · `NODE-PEERIN-PBXHQ-{TCP,TLS}`/`NODE-PEEROUT-{TCP,TLS}-PBXHQ` — 동봉 `scenarios/node/` 24종 | tcp 축 = 토폴로지 풀 `volte_ue_tcp`(listener tcp, volte.jsonl offset 30~33 — `volte_ue` 는 count 30). pbx_reg 2종은 dep35 배포 설정 `Tester.Secrets` 의 `PBX_HA1` 필요. 시나리오 끝에 `deregister` 를 두지 말 것 — 워커 `endRun` 이 풀 단말 전부를 `Stop(5)` 로 동기 해제하는 동안 제어 HTTP 가 막혀(`m_mtx`) 컨트롤러 조회 시간초과 → verdict error(워커 과제) |
| 1-2 피어 | A 재실측(PBX·MGCF·IBCF 8종) + TRUNK-IBCF-OUTBOUND·INBOUND·ACL-DENY·MGCF-OUTBOUND·INBOUND·EARLY-MEDIA·PBX-INBOUND·DTMF·HOLD-RESUME·TRANSFER | `q850_rx_pct`·`codes.503`·`early_rtp_pct`·`prack` 이 §12 재실측 지표 |
| 1-3 PTT·MCData | PTT-GROUP-CALL-BASIC·FLOOR-HANDOVER·AFFILIATION-CHURN · MCDATA-SDS-1TO1·GROUP·GROUP-MEDIA·FD-GROUP·FD-1TO1 | GROUP-MEDIA 는 dep34 `Setup.McDataMedia.Enable` 확인 |
| 1-4 대표번호 | VOLTE-FA-PARALLEL·OVERFLOW·SEQUENTIAL·DIALOG-FORK·PICKUP | 전화 그룹 픽스처 필요 → 3단계 verify 다리로 돌리는 편이 정확(픽스처 자동·복원). 단독은 수동 시드 |
| 1-5 청취·비멤버 | PTT-GROUP-LISTEN·LISTEN-DENIED·LISTEN-ROSTER·LISTEN-CONF-DENIED·NONMEMBER-DENIED | 0-2 의 g001 밖 신원 + `ptt_listen` 역할 배정(CSC roles API) |

### 2단계 — 확장 시나리오 (하루)

D 표 9종. 실패 예상 지점을 미리 적어 둔다.

- TLS-MUTUAL: CSP 발신 TLS 가 노드 인증서를 클라이언트 인증서로 내는지(§12 반영분 첫 실측). 워커 `Tls.CaFile` 이 사이트 CA 여야 CSP 인증서를 받는다.
- THIG: `thig_pct` — CSP 가 응답 Via 를 요청대로 되돌리면 100 %. THIG 자체는 미구현 과제라 관측 지표만 본다.
- RETRANS-LOSS: `retrans_rx_pct` — CSP UAC 재전송(Timer A) 관측.
- NAT: 워커 setcap·netns 는 호스트 권한 작업 — 운영자 실행.
- REAL-UE: 실스택 `real_*` 지표(MOS 포함). dev 에서 pass 했으니 환경 문제 위주.
- SOAK-LEAK: 30 분 돌려 `rss_growth_mb/fd_growth` — 4단계 소크와 합쳐도 된다.
- PTT VIDEO: g004 멤버 뒤. 규격상 `video_enabled` 그룹만 100 %.

### 3단계 — cims-verify 계측기 게이트 모드 (반나절)

```
CIMS_TESTER_URL=https://127.0.0.1:4419 CIMS_TESTER_TOPOLOGY=tb48 \
CIMS_TESTER_LOGIN=admin CIMS_TESTER_PASSWORD=… ./cims-verify --stage 3 --items S3-SCN-XFER,S3-SCN-PICKUP,S3-SCN-DIALOG,S3-SCN-MONITOR,S3-SCN-FA,S3-SCN-PTT-LISTEN
CIMS_TESTER_URL=… ./cims-verify --stage 6 --items S6-SCN-VOLTE-VOICE,S6-SCN-VOLTE-VIDEO,S6-SCN-PTT-VOICE,S6-SCN-PTT-VIDEO,S6-SCN-IBCF-TRUNK
```

전화 그룹·픽업 그룹·역할·service_ref 전제는 시나리오 `fixtures:` 가 선언하고 컨트롤러가 대상 CSC 관리 API(4419 게이트웨이 경유)로 적용·복원한다(§5.2 구현 반영) — verify 다리는 시나리오를 부르기만 하므로 dev 스택 기준 통지 포트 문제는 없다. cspsim 잔여 검사(D5·M5·M8·F5 타이밍)는 dev 5060 이 없어 SKIP 이 정상. 전제 = 새 tester 패키지 배포(fixtures 를 아는 컨트롤러).

### 4단계 — 부하·소크: **하지 않음(결정)**

.48 은 단일 서버 구성(대상 5 모듈 + 워커 동거)이라 부하 시험은 이 환경에서 하지 않는다. 소크 누수 판정(VOLTE-CALL-SOAK-LEAK)은 2단계에서 저율 30 분 1회만 돌려 SSH 관측 증거(`rss_growth_mb/fd_growth`)의 배관을 확인하는 데 그친다. 부하·소크는 워커 호스트가 분리된 환경이 생길 때 별도 계획으로.

### 5단계 — 마무리 (반나절)

- 회귀 표 최종본 + 기준선 run 집합을 이 문서 §6 에 적고, `compare?format=md` 로 빌드 간 추세를 남긴다.
- 실패 ⓑ 는 test_instrument.md §12 표에 행 추가(현 상태·시험에서 보이는 모습), 계측기 결함 ⓐ 는 수정 후 재실측, 환경 ⓒ 는 runbook 에 절차로.
- 토폴로지 v3·creds 규약·회귀 스크립트를 커밋. 메모리는 "다음 배포 때 같은 순서로 회귀" 로 갱신.

## 5. 결정 사항 (2026-09-21 확정분)

| # | 항목 | 결정 | 계획에 미치는 것 |
|---|---|---|---|
| 1 | G.722 PBX(`pbx_wb`) | **제외**. 현 CSP·CMP 는 G.722 를 relay 만 하고(AMR-WB↔G.722 변환 없음) 사이트 IP-PBX 코덱은 G.711A 로 둔다 | TRUNK-PBX-G722 는 "해당 없음(사이트 코덱 정책)"으로 기록. 번호 블록 추가 불필요. G.711A 경로는 `pbx_hq`(PCMA/PCMU + 트랜스코딩)가 이미 덮는다 |
| 2 | CMP RTP 포트 | **CMP = 20000~29999**(커널 임시 포트 32768~60999 아래라 워커·타 프로세스와 애초에 안 겹친다 — sysctl 불필요), 그 위는 계측기 몫 | dep32 overlay: `RtpStartPort 20000`·`RtpPoolSize 1000`(호당 8 → 20000~27999) · `PttRtpStartPort 28000`·`PttMemberPoolSize 100`(28000~28199) · `PttVideoStartPort 28500` · `PttFloorStartPort 29000`·`PttRtpPoolSize 20`, CMP 재기동(fd 상한 524288·20000 대 미사용 확인됨). 토폴로지 `cmp.media.rtp_range: [20000, 29999]`. 평상시엔 CMP 가 먼저 떠 포트를 쥐므로 임시 포트 충돌은 없고, 위험은 반대 순서(부하 중 CMP 재기동 때 대역 안 포트를 남이 쥐고 있으면 풀 유닛 초기화 실패)뿐 — 임시 포트 범위 아래로 두면 그 위험도 사라진다 |
| 3 | SSH 관측 | **허용** | 컨트롤러와 대상이 같은 호스트(`cims@127.0.0.1:10022`)라 전용 키를 만들어 `authorized_keys` 에 등록, `TESTER_SSH_KEY` 를 agent#13 env 에 넣고 agent → dep35 재기동. 토폴로지 `hosts.h48.ssh{user cims, key_env TESTER_SSH_KEY, port 10022}` + `nodes.csp.procs/logs` |
| 4 | NAT 시험 | **진행 안 함** | VOLTE-CALL-NAT 는 "미실행(환경 미구성)"으로 기록. `volte_ue_nat` 풀은 넣지 않는다 |
| 5 | PTT 그룹 | **적용 완료(2026-09-21, CSC 관리 API)** — g001 제외(실단말), g002 그대로(6명), g003 += +82500000013~017(7명), g004(video) += +82500000018~022(6명). 실단말 신원은 .45 CSP 에 붙어 있어 .48 fan-out 과 무관 | creds `--ptt-group g002|g003|g004` 그룹별 파일, 토폴로지 `ptt_ue_g00N` 풀. 청취·비멤버 후보 = 그룹 밖 +82500000023~040 |
| 6 | 시험 픽스처(전화 그룹·픽업 그룹·역할) | **상용 기준 정상 경로 = ⓒ**(§5.2) — 시험 도구는 운영자와 같은 프로비저닝 경로(CSC 관리 API)만 쓰고, DB 직접 쓰기·CSP 내부 통지는 시험 코드에서 걷어낸다 | **구현 완료(2026-09-21, 미배포)** — §5.2. 배포 뒤 1-4·1-5·3단계 |

### 5.0 대상 CSP 피어 라우팅 = 사이트 설정 (2026-09-21 적용)

tb48 의 피어 풀 7개(pbx_hq·pbx_reg·mgcf_pstn·peer_kt/dead/503·peer_blocked)에 맞는 CSP 컬렉션 레코드를 **영구 배치**했다. run 마다 시드·원복하던 것을
사이트 설정으로 바꾼 것이라, 계측기는 이제 그 설정을 검증한다(풀 `seed.enabled: false`, 소스 관리본 `topology.tb48.yaml` + store #2).

| 컬렉션 | 건수 | 내용 |
|---|---|---|
| local_nodes | +1 | `tb48-peering` UDP 121.161.164.48:15070 (edge peering) |
| remote_nodes | 7 | `tb48-rn-<풀>` — 워커 피어 수신점(5080~5095 UDP), pbx 는 `transcode_codecs [PCMA, PCMU]` |
| routes | 7 | `tb48-r-<풀>` inbound_auth none, `pbx_reg` 는 digest + 트렁크 계정 pbx-hq |
| route_sets | 5 | `tb48-rs-<풀>` · `tb48-rs-rs-kt`(peer_kt 100 / dead 50 / 503 40, `options_ping`) |
| rules · rule_sets | 11 · 6 | 도메인 eq + 번호 대역 in_range(pbx 1010~1014 / 1015~1019, mgcf 2200 01010~019), peer_blocked 소스 IP(acl) |
| routing_policies | 5 | `tb48-rp-<풀>` priority 50 |
| acl_policies | 1 | `tb48-acl-peer_blocked` deny scope=route |

만드는 법 = 계측기 파생 규칙 그대로(`tester_target.derive_records` + `pick_local_node`) → 이름 `tester-`→`tb48-`, 태그 `cims-tester`→`tb48`(계측기가 자기 시드로
오인하지 않게) → `scripts/oam-deploy.py collection 34 <컬렉션> --file …`(마지막만 `--signal`). 재실측: TRUNK-PBX-OUTBOUND·INBOUND 각 1 호 pass, run 노트에
시드 없음, CSP 로그 `tb48-rs-pbx_hq`/`InboundRoute … local_node=tb48-peering`. **부작용**: rs-kt 의 `options_ping` 이 워커 피어가 안 떠 있는 평상시에도 2 s 마다
OPTIONS 를 보내 세 피어를 dead 로 판정(A-COM-003)하고, `pbx_reg` 는 트렁크 REGISTER 바인딩이 없을 때 Route dead 다 — 사이트 설정의 정상 동작이며 시험 중에만 해소된다.
피어 시나리오를 안 돌리는 기간엔 이 두 RouteSet 의 health_check_mode 를 none 으로 두거나 알람을 감안한다.

### 5.1 PTT 그룹 구성 (적용 뒤, DB .45)

| 그룹 | video | 멤버 | 계측기 사용 |
|---|---|---|---|
| g001 | 0 | 9 (실단말) | 제외 |
| g002 | 0 | 6 (+82500000001·002·007, +82510001001~003) | 그대로 사용 — 실단말 신원은 .45 CSP 에 등록돼 있어 .48 에서는 계측기가 같은 H(A1) 로 등록해 쓴다 |
| g003 | 0 | 7 (+82500000013~017 + 001·002) | 사용 |
| g004 | 1 | 6 (+82500000018~022 + +82510001001) | 사용 — PTT-GROUP-CALL-VIDEO |

적용은 `POST /api/v1/ptt/groups/{id}/members`(OAM 게이트웨이 4419 경유, CSC 가 GMS 문서 재생성·CSP 통지). 미배정 계측기 신원 +82500000023~040 은 청취·비멤버(`member: false`) 후보와 부하용.

### 5.2 시험 픽스처 — 상용을 기준으로 한 정상 경로

**문제의 본질**: 픽업 그룹·대표번호(전화 그룹)·역할은 운영 데이터다. 상용에서는 운영자가 콘솔·관제 앱 → CSC 관리 API 로 만들고, CSC 가 단일 쓰기 주체로서 DB 에 쓰고 CSP 에 통지한다([oam_csc_split.md](../design/oam_csc_split.md), [sip_access_security.md](../design/features/sip_access_security.md) P1). 지금 cims-verify 의 S3 항목은 이 경로를 우회해 DB 에 직접 쓰고 CSP 에 UDP 통지(`csp_notify.py` 상수 `127.0.0.1:4421`)를 보낸다 — 개발 스택 한 대에서만 맞는 지름길이며, 배포본(.48)·상용 어디서도 성립하지 않는다.

**정상 구조(ⓒ)** — 시험 도구는 운영자와 같은 경로만 쓴다:

1. **계측기 시나리오가 자기 전제를 선언**한다 — YAML `fixtures:` 블록(3층 모델의 시나리오 층). 예: `phone_group{pilot, members[role], mode parallel|sequential, overflow}`, `pickup_group{group_id, members[role]}`, `roles{role: [preset|capability…]}`. 역할 신원은 계획(`identities_by_role`)이 정하므로 픽스처는 역할 이름으로 쓴다.
2. **컨트롤러가 run 직전에 대상 subscriber 노드(CSC 관리 API — `/api/v1/phone-groups`·`/members`·`/api/v1/roles`·`/api/v1/ptt/groups/*`)에 적용**하고, 적용 결과를 읽어 확인한 뒤 run 을 시작한다. 토큰은 대상 OAM 로그인 토큰(동거 형태는 요청자 토큰 폴백 — 지금 `target_csc`·`oam` 노드가 쓰는 규약 그대로). CSP 통지는 CSC 몫.
3. **run 뒤 복원**(만든 그룹 삭제·멤버 원복·역할 배정 원복)을 컨트롤러가 한다. 실패해도 run.json 에 남긴다.
4. **시험 가입자는 전용 번호 대역·전용 org** 로 둔다(지금 +82500000013~040 처럼). 픽스처는 이 대역 안에서만 움직이므로 상용 가입자를 건드릴 수 없고, CSC 감사(E-AUD-*)에 계측기 계정으로 남는다.
5. cims-verify S3 항목은 계측기 시나리오를 부르기만 한다(지금 다리 그대로). `_dispatch_common` 의 DB 직접 쓰기·`csp_notify` 는 계측기 경로 항목에서 제거하고, 남는 cspsim 잔여 검사(D5·M5·M8·F5·F7)만 개발 스택 한정으로 표시한다.

**왜 ⓐ(통지 주소 환경변수)·ⓑ(verify 만 CSC API 로) 가 아닌가**: ⓐ 는 우회를 배포본까지 넓힐 뿐이고, ⓑ 는 경로는 맞지만 픽스처가 verify 코드에 묻혀 콘솔·CLI 로 시나리오를 단독 실행할 때는 여전히 픽스처가 없다. ⓒ 는 시나리오 하나가 어디서 돌든 같은 전제를 갖는다.

**구현 반영(2026-09-21)**: `tester_models`(픽스처 4종·검증)·`tester_fixtures`(적용·확인·복원)·`tester_compile`(역할 신원·`${pilot}`·게이트)·`tester_run`(풀 생성 앞 적용, 끝에 복원, run.json `fixtures`)·계획 미리보기·콘솔 YAML/미리보기·동봉 S3 축 17종 `fixtures:`·verify 6 항목(계측기 경로는 시나리오만 호출)·단위시험 `tests/test_tester_fixtures.py`(S1-UNIT-TESTER). 정본 test_instrument.md §4. 배포(tester 패키지)·.48 실측은 1-4·1-5·3단계에서.

**부하 단계는 하지 않으므로** 가입자 대량 시드(결정 항목 7)는 없다. creds 는 현 H(A1) 보유 가입자(volte 43·voip 3·ptt 43) 안에서 만든다.

## 6. 회귀 표 (실행하며 채운다)

| 묶음 | 시나리오 | verdict | run id | 빌드 | 사유 분류(ⓐ/ⓑ/ⓒ) |
|---|---|---|---|---|---|
| | | | | | |
