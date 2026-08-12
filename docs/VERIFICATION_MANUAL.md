# CIMS 검증 수동 실행 가이드

> 원본 SSOT: `docs/VERIFICATION_PROCESS.md`
> 본 문서: 사용자가 직접 단계별로 따라할 수 있도록 정리한 실행용 체크리스트.
> 6단계 (S1~S6) 파이프라인 기준.

---

## 0. 사전 준비

### 0.1 환경 확인

```bash
cd /home/nex/work/cims

# (a) 작업 IP (외부 인터페이스 ens160)
ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1   # 예: 192.168.x.x

# (b) git 상태
git status
git rev-parse --short HEAD

# (c) preflight (자동 진단)
./cims.sh preflight                                          # ens160 IP, 충돌 포트
```

### 0.2 TB 상시 기동

TB-CSC(4419) / TB-Console(3000) 은 **검증 진행 중 절대 내리지 않음**.

```bash
./cims.sh status                              # TB RUNNING 확인
./cims.sh start tb                            # 미기동 시
```

### 0.3 IP 변경 시

```bash
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump                       # tarball 속 cims.sh / config 최신화 (S4 영향)
```

---

## 1. 빠른 실행

### 1.1 한 사이클 풀 실행 (콜드 스타트)

```bash
./cims.sh status
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)

# pipeline-full preset — S1~S6 전체
python3 -m tests.cims_verify run --preset pipeline-full
```

### 1.2 stage 단위 실행

```bash
./cims-verify stage1                       # 정적 검사
./cims-verify stage2                       # 빌드
./cims-verify stage3                       # 스모크 (1콜 VoIP/PTT)
./cims-verify stage4                       # 패키지화 (manifest.json)
./cims-verify stage5                       # 로컬 배포 (csc/console/csp/cmp)
./cims-verify stage6                       # 통합 검증 (4 시나리오)
```

각 stage 에 옵션 통과 가능: `./cims-verify stage5 --stop-after`.

### 1.3 메타 조회

```bash
./cims-verify list                         # 전체 항목
./cims-verify list --stage 5               # stage 5 만 (execution_order 순)
./cims-verify list-presets                 # 12 프리셋
./cims-verify describe S5-CSC-DEPLOY-INSTALL
```

### 1.4 부분 실행

```bash
# items (자식 ID 직접)
./cims-verify run --items S5-CSC-DEPLOY-PKG-UPLOAD,S5-CSC-DEPLOY-INSTALL

# preset
./cims-verify run --preset stage5-full
./cims-verify run --preset post-deploy     # S5 + S6 (배포 + 통합)

# 자식 필터 (그룹 안에서 일부만)
./cims-verify run --preset stage5-full --only-children S5-CSC-DEPLOY=S5-CSC-DEPLOY-INSTALL

# 디버그용 강제 FAIL 주입 (gate / 회귀 점검)
./cims-verify stage1 --inject-fail S1-CPP-FORMAT
```

### 1.5 Console UI

| URL | 메뉴 | 용도 |
|---|---|---|
| `http://<ens160>:3000/release/verify` | 패키징 > 검증 실행 | **6단계 LIVE** (1.5s 폴링, stage gate 노란 배너) |
| `http://<ens160>:3000/release/verify-history` | 패키징 > 검증 이력 | 회차 이력 + 통계 (KpiGrid + ScopeTable + Sparkline) + DetailModal PDF |
| `http://<ens160>:3000/release/package` | 패키징 > 패키징 | 빌드 / 패키지화 / tarball 다운로드 (카드 그리드 8장) |
| `http://<ens160>:3000/deploy/packages` | 시스템 > 패키지 | 배포본 패키지 등록·관리 (tarball 업로드) |
| `http://<ens160>:3000/deploy/servers` | 시스템 > 시스템/인프라 | **primary 진입** — 좌측 트리(시스템/서버) + 4탭. 서버별 모듈 lifecycle(install/start/stop·설정·metrics) 과 그룹 편집(절체 조건·멤버·VIP·그룹 마운트·공유 store) 을 한 화면에서 |

검증 실행은 **TB-Console (3000)** 에서 수행 — TB-CSC 4419 backend.

---

## 2. Stage 별 합격 체크

### 2.1 S1 — 정적 검사

```bash
./cims-verify stage1
# 5 항목: PY-SYNTAX / FRONTEND-LINT / FRONTEND-TYPECHECK / CPP-FORMAT / UNIT-VERIFY-LIB
# 기대: 5/5 PASS, 종료 코드 0
```

FAIL 시 → S2~S6 자동 BLOCKED. 코드 수정 후 재시도.

### 2.2 S2 — 빌드

```bash
./cims-verify stage2
# PREFLIGHT → BUILD (depends_on)
# 기대: warning/error 0, build/dist/ 갱신
```

### 2.3 S3 — 스모크

```bash
./cims-verify stage3
# RESET → CONFIGURE → START → SEED → HEALTH → {VOIP-SMOKE, PTT-SMOKE}
# 기대: 7/7 PASS, seg_*.rtp 녹취 +2 이상
```

### 2.4 S4 — 패키지화

```bash
./cims-verify stage4
# PKG-BUILD → PKG-MANIFEST
# 기대: build/dist/packages/<m>-<ver>.tar.gz 12개 (base 8 + 변종 4) +
#       packages/manifest.json (SHA-256, manifest.json 자체 _self_sha256 포함)
#   base 8 : csp / cmp / cmdp / csc / oam / oam-svc / cspsim / agent
#   변종 4 : psp · isp (CSP staging) · pmp · imp (CMP staging)
```

> 변종 4종은 `cims.sh cmd_pkg` 의 staging 단계에서 base dist 디렉토리를 임시
> 복사한 뒤 바이너리 (`bin/csp` → `bin/psp`), config (`config/csp.json` →
> `config/psp.json`), launcher (`csp.sh` → `psp.sh`) 를 rename 후 tar — 즉
> tarball 안 디렉토리 구조가 실제로 분리된 형태다. 콘솔 `/release/package`
> 카드의 ⤓ 다운로드 버튼은 변종별로 노출.

### 2.5 S5 — 로컬 배포 (22 native step, mgmt + 5 service-server)

```bash
./cims-verify stage5                       # 기본: 전체 기동 유지
./cims-verify stage5 --stop-after          # 종료 시 stop+kill
```

**토폴로지** — mgmt 체인이 standalone **oam**(관리평면 4445, 콘솔 정적 동봉·단일
오리진)을 부트스트랩하고, 이후 모든 모듈(csc 서비스 모듈 + service-server)은
**배포본 OAM(4445) 경유**로 배포한다. `verify/lib/items/stage5/_native_steps.py`
의 `_INSTANCES` SoT:

| display_name | agent | dist 경로 | 변종 | 포트 |
|---|---|---|---|---|
| CIMS 관리 서버 | mgmt-server | `build/dist/mgmt-server/` | oam (console 동봉) + sim (install-only) | 4445 |
| Mgmt Service (CSC) | mgmt-svc-server | `build/dist/mgmt-svc-server/` | csc | 4446/tcp (MCPTT 4431) |
| VoLTE SIP Server | volte-sip-server | `build/dist/volte-sip-server/` | csp (CSP) | 5060/udp · 127.0.0.1 |
| VoLTE Media Server | volte-media-server | `build/dist/volte-media-server/` | cmp (CMP) | 9000/udp · 127.0.0.1 |
| PTT SIP Server | ptt-sip-server | `build/dist/ptt-sip-server/` | psp | 5060/udp · 127.0.0.3 |
| PTT Media Server | ptt-media-server | `build/dist/ptt-media-server/` | pmp | 9000/udp · 127.0.0.3 |

**기대 결과**:

| 그룹 | 자식 / step |
|---|---|
| RESET (10) | step 01: cleanup (가입자 보존, agent dir + pkill 패턴 + supervised.json 잔재 정리) |
| CSC-DEPLOY (20) | 21 AGENT-ENROLL (05+06+07: mgmt-server) / 22 PKG-UPLOAD (08: oam/cspsim) / 23 INSTALL (09+10, oam overlay = Server.Port 4445 + runtime/packages/log 격리) |
| CSC-VERIFY (30) | 31 FILES (11, current 통로 해석) / 32 OVERLAY (12: oam/config.json Server.Port=4445) |
| CSC-RUN (40) | 41 CSC-START (13: oam 4445 LISTEN) / 42 CSC-HEALTH (14) / 43 CONSOLE-START (15: GET / SPA 정적 서빙) |
| MODULES-DEPLOY (50) | 51 AUTH (16: 배포본 OAM) / 52 PKG-UPLOAD (17: csc/csp/cmp/psp/pmp) / 53 AGENT-ENROLL (18: sync 9904~9909) / 54 INSTALL (19+20 — DB·Xcap·MediaServer.Endpoints·MCPTT 포트 overlay 주입) |
| MODULES-RUN (60) | 61 START (21) — local_nodes 시드 + csc(4446) + csp+cmp/psp+pmp pair OnCmpStatusChanged Connected wait + immutability marker |
| FINALIZE (70) | step 22: 기동 유지 / --stop-after = 모듈(배포본 OAM 경유) → mgmt 순 stop + agent kill |

**합격 체크**:

```bash
# endpoint LISTEN (mgmt oam 4445 + csc 4446 + service 4)
ss -tln | grep -E ':(4445|4446)\b'
ss -uln | grep -E '127.0.0.[135]:(5060|9000)\b'

# 디렉토리 구조 — 버전형 설치 (modules/<모듈>/<ver>/ + current 심볼릭)
ls build/dist/mgmt-server/                           # agent/ oam/ sim/
ls build/dist/mgmt-svc-server/modules/csc/
ls build/dist/{volte-sip,volte-media,ptt-sip,ptt-media}-server/modules/

# Test-agent (mgmt 9903 + csc 9909 + service 9904~9908, CIMS_AGENT_NO_SUPERVISE=1)
pgrep -af cims_agent.py

# Immutability marker
cat build/dist/.deployed-manifest.json    # manifest_sha + ts
```

### 2.6 S6 — 통합 검증

```bash
./cims-verify stage6
# ENTRY-CHECK (6 host:port + immutability) → SEED → 시나리오 → SUMMARY
```

**시나리오 (cspsim 4종 + 깊이검증 5종)**:

| # | 시나리오 | 대상 | 합격 |
|---|---|---|---|
| 1 | VoLTE 음성 2자 (`-mode volte -count 2 -no_video`) | volte-sip | seg_*.rtp +1 |
| 2 | VoLTE 영상 2자 (`-mode volte -count 2`) | volte-sip | seg_*.rtp +1 |
| 3 | PTT 그룹 음성 5인 (`-mode ptt -scenario group_call -count 5 -no_video`) | ptt-sip (127.0.0.3) | seg_*.rtp +1 |
| 4 | PTT 그룹 영상 5인 (`-mode ptt -scenario group_call -count 5`) | ptt-sip | seg_*.rtp +1 |
| 5 | L7-NOTIFY (SUBSCRIBE/NOTIFY xcap-diff/resource-lists/conference-info XML well-formed) | csp | NOTIFY body XML namespace 매칭 |
| 6 | CMP-GROUP-SYNC (그룹콜 세션 중 admin PUT floor_policy → PTT_GROUP_MODIFY 전파) | pmp 9000/udp | 세션 수립(STATS group_details 등장) + 변경값 STATS 반영 (CMP 그룹은 on-demand 세션 자원 — 유휴 시 상시 roster 없음) |
| 7 | MCPTT-FLOOR-GRANT (cmp_*.flow.jsonl 의 FLOOR_GRANT/TAKEN/IDLE) | pmp | flow.jsonl 매칭 |
| 8 | DB-SYNC (admin → CSP 의 USER_CHANGED/GROUP_CHANGED notify 로그 매칭) | csp + psp | log glob 매칭 |
| 9 | CERT-ROTATE (mTLS 토글 시 cert 발급 + agent reload) — `--enable-mtls` 시만 | mgmt csc + agent | cert_issued_at 갱신 또는 agent_mtls.crt mtime 60s 이내 |

> 각 PTT 시나리오는 `_helpers.target_ip("psp")` 로 PSP 의 LocalIp (127.0.0.3)
> 를 자동 선택. ENTRY-CHECK 는 `csc_console + 4 service-server` 6 host:port
> 매트릭스로 LISTEN 검증.

```bash
# 합격 후 녹취 갯수
find ext_mnt/service_log -name 'seg_*.rtp' -newer verify_reports -mmin -10 | wc -l
# 기대: 28 이상 (4 + 4 + 10 + 10)
```

---

## 3. 회차 이력

### 3.1 list / detail

```bash
# 최근 50회
curl -sk https://127.0.0.1:4419/api/v1/verification/runs?limit=50 | jq

# 단일 회차 + 항목 결과
curl -sk https://127.0.0.1:4419/api/v1/verification/runs/123 | jq

# 통계 (overall + by_scope + timeline)
curl -sk "https://127.0.0.1:4419/api/v1/verification/runs/stats?days=30" | jq
```

### 3.2 Console

`http://<ens160>:3000/release/verify-history` — 회차 list + 통계 KPI + Sparkline + DetailModal (📄 PDF 인쇄).

### 3.3 environment 메타

```bash
curl -sk https://127.0.0.1:4419/api/v1/verification/env | jq
# 기대: host, git_branch, git_sha, pkg_manifest_hash
```

V2Page PDF 인쇄 시 자동 주입.

---

## 4. 함정 / 실패 대응

### 4.1 Stage gate 자동 차단

stage N FAIL → stage>N 의 leaf 가 BLOCKED 로 표시. V2Page 노란 배너 + history page 의 BLOCKED row.

```bash
# 대응: FAIL 항목 detail 확인 → 해당 stage 만 재실행 → 잔여 자동 진행
./cims-verify stage<N>     # FAIL 발생한 stage
./cims-verify run --preset pipeline-full   # 또는 전체 재실행
```

### 4.2 Immutability gate 불일치 (S6-ENTRY-CHECK FAIL)

S5 배포 후 S4 를 다시 돌리면 manifest sha 변경 → S6 진입 실패.

```bash
# 복구
./cims-verify stage4       # manifest 재생성
./cims-verify stage5       # 재배포 + marker 갱신
./cims-verify stage6       # 재진입
```

### 4.3 6 endpoint 미기동 (S6-ENTRY-CHECK FAIL)

```bash
# mgmt 2 + service 4 = 6 endpoint
ss -tln | grep -E ':(4445|8081)\b'
ss -uln | grep -E '127.0.0.[123]:(5060|9000)\b'
# 미충족 → S5 선행
./cims-verify stage5
```

### 4.4 흔한 함정

```bash
# tarball 속 cims.sh 가 stale
tar tzf build/dist/packages/csc-*.tar.gz | grep cims.sh
# 해결: ./cims.sh sync all && ./cims.sh pkg --no-bump

# csp.json LocalIp stale (IP 변경 후)
grep LocalIp build/dist/csp/config/csp.json
# 해결: ./cims.sh configure --local-ip <ens160> && ./cims.sh pkg --no-bump

# TB-CSC verification.py 수정 후
./cims.sh sync csc && ./cims.sh restart tb-csc
```

### 4.5 디버그 진입점

| 무엇을 볼 때 | 어디 |
|---|---|
| S2/S3 모듈 로그 | `build/dist/log/<module>.log` |
| S5 배포본 로그 | `build/dist/{csc,csp,cmp}-server/<module>/log/` |
| Service log (호 단위) | `ext_mnt/service_log/<type>/YYYY/MM/DD/HH/.../*.d/` |
| SIP msg log | `ext_mnt/msg_log/csp/sip/YYYY/MM/DD/HH/sip.jsonl` |
| Verify reports | `verify_reports/<ts>_stage<N>.md` |
| TB-Console (검증) | `http://<ens160>:3000/release/verify` |
| 회차 이력 | `http://<ens160>:3000/release/verify-history` |
| 빌드 / 패키징 | `http://<ens160>:3000/release/package` |

---

## 5. 합격 흐름

```
[S1] PASS ─▶ [S2] PASS ─▶ [S3] PASS ─▶ [S4] PASS ─▶ [S5] PASS ─▶ [S6] PASS ─▶ ✅ 상용 배포
  │            │            │            │            │            │
  └ FAIL = stage gate ─▶ stage>N 자동 BLOCKED. FAIL 분석 후 해당 stage 재실행.
```

---

## 부록. 시뮬레이터 (cspsim) 수동 실행

`cspsim` 은 SIP/RTP 단말 시뮬레이터로, S3 스모크·S6 통합 검증의 호 발생기다. 빌드 산출물은
`build/bin/cspsim`. 단독 수동 시험에도 사용한다.

### 서비스 표준 코덱 — 시험 전 필독

**CIMS 의 VoLTE/PTT 음성 코덱은 AMR-WB (PT=99, `AMR-WB/16000/1`), 영상은 H.264 (PT=96,
`H264/90000`) 다.** 녹취 변환 파이프라인(OAM ffmpeg)도 AMR-WB/H.264 를 전제한다
([design/features/recording.md](design/features/recording.md)).

cspsim 은 **미디어 파일(`-media_file`/`-media_dir`)을 지정하지 않으면 SDP 를 PCMU (PT=0) 로
만든다** — 이 fallback 은 표준 시험이 아니며, 녹취 변환·코덱 경로가 검증되지 않는다.
**기본 호시험은 반드시 AMR-WB 미디어 파일과 함께 실행한다.** 샘플 미디어는
`tests/media/*_audio.amrwb`(음성) / `tests/media/*_video.h264`(영상).

### 기본 호시험 (표준 명령)

권장 진입점은 `./cims.sh sim` — DB 가입자 로드(`-db`), domain 자동 감지(access_services),
**AMR-WB 미디어 자동 주입**(`build/dist/cspsim/media/` 존재 시)을 알아서 처리한다.

```bash
# VoLTE 음성 2자 (AMR-WB) — 기본 호시험
./cims.sh sim -mode volte -scenario call -count 2 -call_duration 8 -no_video

# VoLTE 영상 2자 (AMR-WB + H.264)
./cims.sh sim -mode volte -scenario call -count 2 -call_duration 8

# PTT 그룹콜 (그룹은 DB 첫 그룹 자동 감지, -group 으로 지정 가능)
./cims.sh sim -mode ptt -scenario group_call -count 4 -call_duration 10
```

> `cims.sh sim` 의 미디어 자동 주입은 `build/dist/cspsim/media/` (·`media/audio_only/`) 가
> 있을 때만 동작한다. 없으면 **경고 없이 PCMU 로 진행**되므로, 최초 1회
> `mkdir -p build/dist/cspsim/media/audio_only &&
> cp tests/media/*_audio.amrwb tests/media/*_video.h264 build/dist/cspsim/media/ &&
> cp tests/media/*_audio.amrwb build/dist/cspsim/media/audio_only/` 로 시드한다.

`build/bin/cspsim` 직접 실행 시엔 미디어를 **명시적으로** 지정한다 (repo 루트 기준):

```bash
# VoLTE 음성 2자 (AMR-WB) — server_ip 는 local_nodes.jsonl 의 primary bind_ip (127.0.0.1 아님)
./build/bin/cspsim -server_ip <primary_bind_ip> -count 2 -mode volte -scenario call \
  -call_duration 8 -no_video -db build/dist/csp/config/csp.json -media_dir tests/media

# VoLTE 영상 2자 (AMR-WB + H.264)
./build/bin/cspsim -server_ip <primary_bind_ip> -count 2 -mode volte -scenario call \
  -call_duration 8 -db build/dist/csp/config/csp.json -media_dir tests/media

# PTT 그룹콜 4자
./build/bin/cspsim -server_ip <primary_bind_ip> -count 4 -mode ptt -scenario group_call \
  -call_duration 10 -db build/dist/csp/config/csp.json -media_dir tests/media
```

- `-db build/dist/csp/config/csp.json`: DB 가입자(imsi 기반 IMPI) 자동 로드. 수동 계정 지정
  (`-user/-password/-domain`) 시엔 가입자의 imsi/service_ref 와 일치해야 REGISTER 가 통과한다.
- 시험 성공 판정: stdout `Registered N/N`·`Call OK`, SIP 원문 로그(`*_sip.msg.*.jsonl`)의
  SDP 가 `m=audio ... RTP/AVP 99` (AMR-WB) 인지 확인. `RTP/AVP 0`(PCMU) 이면 미디어 미주입.

인터랙티브 명령: `s`(stats) · `c`(call) · `g`(group call) · `t`/`r`(PTT push/release) · `sub`(subscribe) · `q`(quit).

### NAT 호시험 (단말 NAT traversal — [design/features/ue_nat_traversal.md](design/features/ue_nat_traversal.md))

NAT 미디어(leg 별 전용 포트 + 목적지 latch)의 검증은 3단계다:

1. **CMP 직접 모사 (루트 불필요)** — 제어 API 로 포트변환 NAT 를 모사해 latch·하향
   실주소 도달·제3자 주입(타 SSRC) 차단·`STATS detail.nat` 를 확인한다.
   1:1 relay 스모크는 `python3 cmp/verify_rtp_bridge.py <CMP_IP>` (leg 포트 브리지).
2. **SIP E2E 판정·전달 (루트 불필요)** — 대상 access service 의 `media_nat_mode=force`
   + SIGUSR1 후 기본 호시험 실행. CSP 로그의 `caller/callee/member leg NAT` 와 CMP 의
   `setRemote ... nat=1` / `addMember ... nat=1` 로 판정→전달을 확인한다
   (no-NAT 환경이라 latch 는 미발동 — 선언=관측 정확 일치 경로로 미디어 무영향).
3. **풀 NAT E2E (sudo 필요)** — `scripts/nat_netns_sim.sh` 로 cspsim 을 사설
   네임스페이스(10.99.0.2) 뒤 SNAT NAT 로 보낸다. UE 가 SDP 에 사설 주소를 선언하고
   관측 소스가 달라지므로 `media_nat_mode=auto` 판정과 CMP latch 가 실호에서 발동한다.

```bash
# 풀 NAT E2E (③) — access service 는 media_nat_mode=auto 상태에서
sudo bash scripts/nat_netns_sim.sh setup
sudo ip netns exec uenat sudo -u cims bash -c \
  'cd /home/cims/work/cims && ./cims.sh sim -mode ptt -scenario group_call -group g001 -count 4 -call_duration 10'
sudo bash scripts/nat_netns_sim.sh teardown
```

성공 판정: CSP `leg NAT (svc=... sdp=10.99.0.2 sig=10.99.0.1)` 로그, CMP
`dest latched (NAT)` 로그, `STATS detail.nat` 의 `learned_ip/learned_port`, 발언자별
녹취 세그먼트 정상. 미협상 소스는 `rtp_src_drop` 카운터로 관측된다.

> cspsim `-local_ip <사설IP>` 단독으로는 NAT 모사가 안 된다 — SIP 스택이 그 주소로
> bind 를 시도해 기동 실패한다. 반드시 netns 방식(③)을 쓴다.

> S6 통합 검증이 cspsim 에 넘기는 시나리오별 인자(`-mode`/`-scenario`/`-count`/`-no_video` 등)는
> 위 「2.6 S6 — 통합 검증」 표를 참조한다.

### floor 정책 시험 (dual/multi-talker·private·ambient·floor SRTCP)

private call·ambient 청취·floor 암호화는 **CSP 가 아직 해당 정책 필드를 발행하지 않아** 실호
시나리오로는 구동되지 않는다. CMP 제어평면과 floor RTCP 를 직접 구동하는 프로브로 검증한다
(정본 [api/cmp_media_api.md](api/cmp_media_api.md) §7.7~§7.8).

> **그룹 동시 발언(dual/multi)은 실호 경로가 열려 있다** — CSP 가 `floor_policy`/`max_talkers`
> 를 발행한다. DB 에서 대상 그룹의 정책을 바꾸면(`UPDATE ptt_groups SET floor_policy='multi',
> max_talkers=2 WHERE mcptt_group_id='<그룹>'`) 다음 `SyncGroupsState` 주기에 `PTT_GROUP_MODIFY`
> 로 CMP 에 반영된다. 다만 **cspsim 은 단일 화자 전제**라 동시 발언 실호는 단말(Android UE)로
> 검증한다 — 화자 2 + 청취자 1, 총 3대.

```bash
python3 scripts/mcptt_floor_policy_probe.py --cmp <CMP_IP> [--port 9000] [--base-port 51500]
```

검증 항목: single 회귀(GRANT/TAKEN/큐/IDLE) · 규격 필드(서버 SSRC 헤더, GRANT 의 SSRC/Duration,
TAKEN 의 Permission·MSN·SSRC, IDLE 의 MSN·Indicator, 화자 본인 TAKEN 제외) · ack 요구
RELEASE(subtype 0x14) 처리와 Floor Ack 회신 · dual(동급 큐, 긴급 override 동시 GRANT,
대기자가 override 자리 미충원) · multi(정원 내 동시 GRANT, TAKEN 의 화자 리스트,
화자 1명 해제 시 잔여 참가자에게 Floor Release Multi Talker) · 타이머(T1 발언 종료 회수·
T2 초과 시 Revoke cause#2·T3 회수 유예·T8 재전송, 그룹별 `floor_timers` 로 짧게 설정) ·
선점(유예 중 기존 화자 floor 유지, 요청자 큐 선두 승급) · 재요청 시 GRANT 재송신과 큐 위치
유지 · 멤버 프로파일(MCPTT ID·큐잉 미협상 Deny#1·mc_granted 초기 발언권·1인 세션 Deny#3) ·
Unicast Media Flow Control(0x0B)·Queued Floor Requests 취소(0x0E) · 멤버별 floor 키(CSK) ·
private(개시자 초기 발언권, 큐 없는 DENY) · `floor_control=off`
(floor_port 미광고 + 양방향 중계) · ambient(`recv_only`/`floor_suppress`) · floor SRTCP
왕복과 평문 거부(`floor_crypto_drop`) · MODIFY 정책 변경(정원 축소 시 초과 화자 회수) ·
계약 위반 필드의 `BAD_REQUEST`.

주의: `--base-port` 부터 약 110 포트를 bind 하므로 CMP 자신의 RTP/floor 풀 대역과 겹치지
않는 값을 준다. 프로브 실행 중에는 CMP 가 이벤트 대상(CSP endpoint)을 프로브로 학습하며,
실제 CSP 는 다음 HEARTBEAT(3s)에 다시 학습된다. 프로브가 만든 그룹은 종료 시 스스로 해제한다.

단일 화자 실호 회귀는 기본 PTT 그룹콜(위 「기본 호시험」)과 S6 `S6-MCPTT-FLOOR-GRANT` 로 커버된다.

---

## 부록. 자동화 / CI

### 비동기 job (backend 폴링)

```bash
# 시작 — async:true 면 job_id 즉시 반환
JOB=$(curl -sk -X POST https://127.0.0.1:4419/api/v1/verification/stages/5 \
  -H "Authorization: Bearer $TOK" -H "Content-Type: application/json" \
  -d '{"async": true}' | jq -r .job_id)

# 폴링 (1.5s 간격 권장) — items_progress + stage_gate 필드
curl -sk https://127.0.0.1:4419/api/v1/verification/jobs/$JOB | jq
```

`POST /stages/<N>` 와 `/run` 모두 옵션 동등: `{"target":"prod"}`, `{"inject_fail":[...]}`.

### CI/CD JSON 출력

```bash
python3 -m tests.cims_verify run --preset pipeline-full --json
echo $?    # 0=PASS, 1=FAIL
```

### 운영 환경 verify (`--target prod`)

기본 `--target verify` (csc=4445, console=8081) 외에 운영 배포본 (csc=4421, console=80) 도 동일 native step 으로 검증 가능. step_09 overlay / step_12~15 LISTEN / S6-ENTRY-CHECK / SCN-DB-SYNC admin login 모두 `target` 따라 분기. csp/cmp 포트는 환경 동일 (5060/9000/udp).

```bash
./cims-verify run --preset post-deploy --target prod
```

### unit test

```bash
python3 -m unittest tests.test_verify_lib    # 161 OK

# CMP floor 코덱/암호 단위테스트 (외부 의존 없음 — 빌드 트리 불필요)
g++ -std=c++17 -Icmp -Iext/pasf/include tests/cmp_floor_codec_test.cpp cmp/PFloorCodec.cpp -o /tmp/floorcodec && /tmp/floorcodec
g++ -std=c++17 -Icmp tests/cmp_floor_crypto_test.cpp cmp/PFloorCrypto.cpp -lcrypto -o /tmp/floorcrypto && /tmp/floorcrypto
```

---

## 부록. --inject-fail (gate 회귀 점검)

함수 호출 없이 ItemStatus.FAIL 을 반환하는 디버그 옵션. 환경 파괴 없이 stage gate / immutability gate 동작 검증용. CLI + backend `{"inject_fail":[...]}` 동등.

```bash
# stage gate: S1 한 항목 FAIL → S2~S6 모두 BLOCKED
./cims-verify run --preset pipeline-full --inject-fail S1-CPP-FORMAT

# immutability gate: marker 누락 시 S6-ENTRY-CHECK FAIL
rm -f build/dist/.deployed-manifest.json
./cims-verify run --preset post-deploy --inject-fail S5-MODULES-RUN-START

# 복구
./cims-verify stage4    # manifest 재생성
./cims-verify stage5    # 재배포 + marker 갱신
./cims-verify stage6
```

---

## 부록. 회차 이력 retention

회차 = `verify_runs/YYYY/MM/<ms_ts>.json` 1 파일. 휘발성 데이터, 기본 retention 7일.

```bash
./cims-verify purge-runs                  # 7일 초과 삭제, 최근 10개 보존
./cims-verify purge-runs --days 30
./cims-verify purge-runs --all            # 즉시 모두 삭제
./cims-verify delete-run 1778125658339    # 단건
./cims-verify purge-runs --json           # CI 연동

# cron 자동 정리 예시
0 3 * * * cd /home/nex/work/cims && ./cims-verify purge-runs --json >> /tmp/cims-verify-purge.log 2>&1
```

---

## 부록. webhook 발행

job 종료 시 verdict + 메타를 외부 URL 로 fire-and-forget POST. Slack/CI 통지 연동.

```bash
export CIMS_VERIFY_WEBHOOK_URL="https://hooks.slack.com/services/..."
export CIMS_VERIFY_WEBHOOK_FILTER="FAIL"          # 옵션: FAIL,UNKNOWN
export CIMS_VERIFY_WEBHOOK_TIMEOUT="3"            # 옵션: 기본 5s
./cims.sh restart tb-csc                          # backend 재기동
```

Payload (CLI/backend 동일):

```json
{
  "run_id": 1778125658339, "verdict": "FAIL", "scope": "stage5",
  "totals": {"total": 7, "pass": 6, "fail": 1, "skip": 0, "blocked": 0},
  "elapsed_ms": 23456,
  "started_at": "...", "finished_at": "...",
  "git_branch": "...", "git_sha": "...", "host": "...",
  "trigger": "cli", "report_path": "...", "pkg_manifest_hash": "..."
}
```

Slack incoming webhook 은 `text` 필드 가공이 필요 → 중간 transform 서버 (예: AWS Lambda receiver) 권장.
