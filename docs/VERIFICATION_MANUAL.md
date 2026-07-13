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
./cims.sh verify stage1                       # 정적 검사
./cims.sh verify stage2                       # 빌드
./cims.sh verify stage3                       # 스모크 (1콜 VoIP/PTT)
./cims.sh verify stage4                       # 패키지화 (manifest.json)
./cims.sh verify stage5                       # 로컬 배포 (csc/console/csp/cmp)
./cims.sh verify stage6                       # 통합 검증 (4 시나리오)
```

각 stage 에 옵션 통과 가능: `./cims.sh verify stage5 --stop-after`.

### 1.3 메타 조회

```bash
./cims.sh verify list                         # 전체 항목
./cims.sh verify list --stage 5               # stage 5 만 (execution_order 순)
./cims.sh verify list-presets                 # 12 프리셋
./cims.sh verify describe S5-CSC-DEPLOY-INSTALL
```

### 1.4 부분 실행

```bash
# items (자식 ID 직접)
./cims.sh verify run --items S5-CSC-DEPLOY-PKG-UPLOAD,S5-CSC-DEPLOY-INSTALL

# preset
./cims.sh verify run --preset stage5-full
./cims.sh verify run --preset post-deploy     # S5 + S6 (배포 + 통합)

# 자식 필터 (그룹 안에서 일부만)
./cims.sh verify run --preset stage5-full --only-children S5-CSC-DEPLOY=S5-CSC-DEPLOY-INSTALL

# 디버그용 강제 FAIL 주입 (gate / 회귀 점검)
./cims.sh verify stage1 --inject-fail S1-CPP-FORMAT
```

### 1.5 Console UI

| URL | 메뉴 | 용도 |
|---|---|---|
| `http://<ens160>:3000/release/verify` | 패키징 > 검증 실행 | **6단계 LIVE** (1.5s 폴링, stage gate 노란 배너) |
| `http://<ens160>:3000/release/verify-history` | 패키징 > 검증 이력 | 회차 이력 + 통계 (KpiGrid + ScopeTable + Sparkline) + DetailModal PDF |
| `http://<ens160>:3000/release/package` | 패키징 > 패키징 | 빌드 / 패키지화 / tarball 다운로드 (카드 그리드 8장) |
| `http://<ens160>:3000/deploy/services` | 배포 > 서버 + HA | 서비스(HA 그룹/standalone) 단위 서버 inline 관리 — primary 진입 |
| `http://<ens160>:3000/deploy/packages` | 배포 > 패키지 | 배포본 패키지 등록·관리 (tarball 업로드) |
| `http://<ens160>:3000/deploy/servers` | 배포 > 서버 Inspector | 서버별 모듈 lifecycle (advanced) — install/start/stop, 설정, metrics |

검증 실행은 **TB-Console (3000)** 에서 수행 — TB-CSC 4419 backend.

---

## 2. Stage 별 합격 체크

### 2.1 S1 — 정적 검사

```bash
./cims.sh verify stage1
# 5 항목: PY-SYNTAX / FRONTEND-LINT / FRONTEND-TYPECHECK / CPP-FORMAT / UNIT-VERIFY-LIB
# 기대: 5/5 PASS, 종료 코드 0
```

FAIL 시 → S2~S6 자동 BLOCKED. 코드 수정 후 재시도.

### 2.2 S2 — 빌드

```bash
./cims.sh verify stage2
# PREFLIGHT → BUILD (depends_on)
# 기대: warning/error 0, build/dist/ 갱신
```

### 2.3 S3 — 스모크

```bash
./cims.sh verify stage3
# RESET → CONFIGURE → START → SEED → HEALTH → {VOIP-SMOKE, PTT-SMOKE}
# 기대: 7/7 PASS, seg_*.rtp 녹취 +2 이상
```

### 2.4 S4 — 패키지화

```bash
./cims.sh verify stage4
# PKG-BUILD → PKG-MANIFEST
# 기대: build/dist/packages/<m>-<ver>.tar.gz 12개 (base 8 + 변종 4) +
#       packages/manifest.json (SHA-256, manifest.json 자체 _self_sha256 포함)
#   base 8 : csp / cmp / cwrtc / csc / console / phone / cspsim / agent
#   변종 4 : psp · isp (CSP staging) · pmp · imp (CMP staging)
```

> 변종 4종은 `cims.sh cmd_pkg` 의 staging 단계에서 base dist 디렉토리를 임시
> 복사한 뒤 바이너리 (`bin/csp` → `bin/psp`), config (`config/csp.json` →
> `config/psp.json`), launcher (`csp.sh` → `psp.sh`) 를 rename 후 tar — 즉
> tarball 안 디렉토리 구조가 실제로 분리된 형태다. 콘솔 `/release/package`
> 카드의 ⤓ 다운로드 버튼은 변종별로 노출.

### 2.5 S5 — 로컬 배포 (22 native step, 5 server)

```bash
./cims.sh verify stage5                       # 기본: 4 ports 기동 유지
./cims.sh verify stage5 --stop-after          # 종료 시 stop+kill
```

**5 server 토폴로지 (P1)** — `verify/lib/items/stage5/_native_steps.py`
의 `_INSTANCES` SoT:

| display_name | agent | dist 경로 | 변종 | 포트 |
|---|---|---|---|---|
| CIMS 관리 서버 | mgmt-server | `build/dist/mgmt-server/` | csc + console + sim (sim install-only) | 4445 / 8081 |
| VoLTE SIP Server | volte-sip-server | `build/dist/volte-sip-server/` | csp (CSP) | 5060/udp · 127.0.0.1 |
| VoLTE Media Server | volte-media-server | `build/dist/volte-media-server/` | cmp (CMP) | 9000/udp · 127.0.0.1 |
| PTT SIP Server | ptt-sip-server | `build/dist/ptt-sip-server/` | psp | 5060/udp · 127.0.0.2 |
| PTT Media Server | ptt-media-server | `build/dist/ptt-media-server/` | pmp | 9000/udp · 127.0.0.3 |

**기대 결과**:

| 그룹 | 자식 / step |
|---|---|
| RESET (10) | step 01: cleanup (가입자 보존, 5 agent dir + pkill 패턴) |
| CSC-DEPLOY (20) | 21 AGENT-ENROLL (05+06+07: mgmt-server) / 22 PKG-UPLOAD (08: csc/console/cspsim) / 23 INSTALL (09+10) |
| CSC-VERIFY (30) | 31 FILES (11) / 32 OVERLAY (12) |
| CSC-RUN (40) | 41 CSC-START (13) / 42 CSC-HEALTH (14) / 43 CONSOLE-START (15) |
| MODULES-DEPLOY (50) | 51 AUTH (16) / 52 PKG-UPLOAD (17: csp/cmp/psp/pmp) / 53 AGENT-ENROLL (18) / 54 INSTALL (19+20) |
| MODULES-RUN (60) | 61 START (21) — csp+cmp pair + psp+pmp pair OnCmpStatusChanged Connected wait + immutability marker |
| FINALIZE (70) | step 22: 기동 유지 / --stop-after = stop list 7 + kill 5 agents |

**합격 체크**:

```bash
# 6 endpoint LISTEN (mgmt 2 + service 4)
ss -tln | grep -E ':(4445|8081)\b'
ss -uln | grep -E '127.0.0.[123]:(5060|9000)\b'

# 디렉토리 구조 — 5 server
ls build/dist/mgmt-server/                           # agent/ csc/ console/ sim/ config/
ls build/dist/{volte-sip,volte-media,ptt-sip,ptt-media}-server/

# Test-agent 5개 (mgmt 9903 + 4 service 9904~9906/+1)
pgrep -af cims_agent.py

# Immutability marker
cat build/dist/.deployed-manifest.json    # manifest_sha + ts
```

### 2.6 S6 — 통합 검증

```bash
./cims.sh verify stage6
# ENTRY-CHECK (6 host:port + immutability) → SEED → 시나리오 → SUMMARY
```

**시나리오 (cspsim 4종 + 깊이검증 5종)**:

| # | 시나리오 | 대상 | 합격 |
|---|---|---|---|
| 1 | VoLTE 음성 2자 (`-mode volte -count 2 -no_video`) | volte-sip | seg_*.rtp +1 |
| 2 | VoLTE 영상 2자 (`-mode volte -count 2`) | volte-sip | seg_*.rtp +1 |
| 3 | PTT 그룹 음성 5인 (`-mode ptt -scenario group_call -count 5 -no_video`) | ptt-sip (127.0.0.2) | seg_*.rtp +1 |
| 4 | PTT 그룹 영상 5인 (`-mode ptt -scenario group_call -count 5`) | ptt-sip | seg_*.rtp +1 |
| 5 | L7-NOTIFY (SUBSCRIBE/NOTIFY xcap-diff/resource-lists/conference-info XML well-formed) | csp | NOTIFY body XML namespace 매칭 |
| 6 | CMP-GROUP-SYNC (admin POST → CMP STATS_REQUEST 응답의 group_details 매칭) | pmp 9000/udp | 5s polling group_id 매칭 |
| 7 | MCPTT-FLOOR-GRANT (cmp_*.flow.jsonl 의 FLOOR_GRANT/TAKEN/IDLE) | pmp | flow.jsonl 매칭 |
| 8 | DB-SYNC (admin → CSP 의 USER_CHANGED/GROUP_CHANGED notify 로그 매칭) | csp + psp | log glob 매칭 |
| 9 | CERT-ROTATE (mTLS 토글 시 cert 발급 + agent reload) — `--enable-mtls` 시만 | mgmt csc + agent | cert_issued_at 갱신 또는 agent_mtls.crt mtime 60s 이내 |

> 각 PTT 시나리오는 `_helpers.target_ip("psp")` 로 PSP 의 LocalIp (127.0.0.2)
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
./cims.sh verify stage<N>     # FAIL 발생한 stage
./cims.sh verify run --preset pipeline-full   # 또는 전체 재실행
```

### 4.2 Immutability gate 불일치 (S6-ENTRY-CHECK FAIL)

S5 배포 후 S4 를 다시 돌리면 manifest sha 변경 → S6 진입 실패.

```bash
# 복구
./cims.sh verify stage4       # manifest 재생성
./cims.sh verify stage5       # 재배포 + marker 갱신
./cims.sh verify stage6       # 재진입
```

### 4.3 6 endpoint 미기동 (S6-ENTRY-CHECK FAIL)

```bash
# mgmt 2 + service 4 = 6 endpoint
ss -tln | grep -E ':(4445|8081)\b'
ss -uln | grep -E '127.0.0.[123]:(5060|9000)\b'
# 미충족 → S5 선행
./cims.sh verify stage5
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

```bash
# VoIP 호 (2 세션)
./bin/cspsim -server_ip 127.0.0.1 -count 2 -user 1001 -domain csp -password 1234 -mode volte -scenario call -call_duration 5

# PTT 그룹콜 (4 세션)
./bin/cspsim -server_ip 127.0.0.1 -count 4 -user 1001 -domain csp -password 1234 -mode ptt -group 1000 -scenario group_call -call_duration 10
```

인터랙티브 명령: `s`(stats) · `c`(call) · `g`(group call) · `t`/`r`(PTT push/release) · `sub`(subscribe) · `q`(quit).

> S6 통합 검증이 cspsim 에 넘기는 시나리오별 인자(`-mode`/`-scenario`/`-count`/`-no_video` 등)는
> 위 「2.6 S6 — 통합 검증」 표를 참조한다.

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
./cims.sh verify run --preset post-deploy --target prod
```

### unit test

```bash
python3 -m unittest tests.test_verify_lib    # 161 OK
```

---

## 부록. --inject-fail (gate 회귀 점검)

함수 호출 없이 ItemStatus.FAIL 을 반환하는 디버그 옵션. 환경 파괴 없이 stage gate / immutability gate 동작 검증용. CLI + backend `{"inject_fail":[...]}` 동등.

```bash
# stage gate: S1 한 항목 FAIL → S2~S6 모두 BLOCKED
./cims.sh verify run --preset pipeline-full --inject-fail S1-CPP-FORMAT

# immutability gate: marker 누락 시 S6-ENTRY-CHECK FAIL
rm -f build/dist/.deployed-manifest.json
./cims.sh verify run --preset post-deploy --inject-fail S5-MODULES-RUN-START

# 복구
./cims.sh verify stage4    # manifest 재생성
./cims.sh verify stage5    # 재배포 + marker 갱신
./cims.sh verify stage6
```

---

## 부록. 회차 이력 retention

회차 = `verify_runs/YYYY/MM/<ms_ts>.json` 1 파일. 휘발성 데이터, 기본 retention 7일.

```bash
./cims.sh verify purge-runs                  # 7일 초과 삭제, 최근 10개 보존
./cims.sh verify purge-runs --days 30
./cims.sh verify purge-runs --all            # 즉시 모두 삭제
./cims.sh verify delete-run 1778125658339    # 단건
./cims.sh verify purge-runs --json           # CI 연동

# cron 자동 정리 예시
0 3 * * * cd /home/nex/work/cims && ./cims.sh verify purge-runs --json >> /tmp/cims-verify-purge.log 2>&1
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
