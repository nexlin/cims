# CIMS 검증 수동 실행 가이드

> 원본 SSOT: `docs/VERIFICATION_PROCESS.md`
> 본 문서: 사용자가 직접 단계별로 따라할 수 있도록 정리한 실행용 체크리스트.
> 6단계 (S1~S6) 파이프라인 기준 — 옛 Phase 1/2/3 가이드는 git history 참조.

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

### 0.2 TB 3종 상시 기동

TB-CSC(4419) / TB-Console(3000) / TB-agent(9902) 는 **검증 진행 중 절대 내리지 않음**.

```bash
./cims.sh status                              # 3종 RUNNING 확인
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

| URL | 용도 |
|---|---|
| `http://<ens160>:3000/testbed/verify-v2` | **6단계 LIVE** (1.5s 폴링, stage gate 노란 배너) |
| `http://<ens160>:3000/testbed/verify-history` | 회차 이력 + 통계 (KpiGrid + ScopeTable + Sparkline) + DetailModal PDF |
| `http://<ens160>:3000/testbed/modules` | 버전 / 설정 템플릿 / overlay 반영 |

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
# 기대: build/dist/packages/{csc,console,csp,cmp,cspsim}-*.tar.gz 5개 +
#       packages/manifest.json (SHA-256)
```

### 2.5 S5 — 로컬 배포 (22 native step)

```bash
./cims.sh verify stage5                       # 기본: 4 ports 기동 유지
./cims.sh verify stage5 --stop-after          # 종료 시 stop+kill
```

**기대 결과**:

| 그룹 | 자식 / step |
|---|---|
| RESET (10) | step 01: cleanup (가입자 보존) |
| CSC-DEPLOY (20) | 21 AGENT-ENROLL (05+06+07) / 22 PKG-UPLOAD (08) / 23 INSTALL (09+10) |
| CSC-VERIFY (30) | 31 FILES (11) / 32 OVERLAY (12) |
| CSC-RUN (40) | 41 CSC-START (13) / 42 CSC-HEALTH (14) / 43 CONSOLE-START (15) |
| MODULES-DEPLOY (50) | 51 AUTH (16) / 52 PKG-UPLOAD (17) / 53 AGENT-ENROLL (18) / 54 INSTALL (19+20) |
| MODULES-RUN (60) | 61 START (21) — csp 5060/udp + cmp 9000/udp + immutability marker |
| FINALIZE (70) | step 22: 기동 유지 / --stop-after 정리 |

**합격 체크**:

```bash
# 4 ports LISTEN (TCP 4445/8081 + UDP 5060/9000)
ss -tln | grep -E ':(4445|8081)\b'
ss -uln | grep -E ':(5060|9000)\b'

# 디렉토리 구조
ls build/dist/csc-server/    # agent/ csc/ console/ config/
ls build/dist/{csp,cmp,sim}-server/

# Test-agent 4개 (sync 9903/9904/9905/9906)
pgrep -af cims_agent.py

# Immutability marker
cat build/dist/.deployed-manifest.json    # manifest_sha + ts
```

### 2.6 S6 — 통합 검증

```bash
./cims.sh verify stage6
# ENTRY-CHECK (4 ports + immutability) → SEED → 4 시나리오 → SUMMARY
```

**4 시나리오**:

| # | 시나리오 | cspsim 옵션 | 합격 |
|---|---|---|---|
| 1 | VoLTE 음성 2자 | `-mode volte -count 2 -no_video` | seg_*.rtp +1 |
| 2 | VoLTE 영상 2자 | `-mode volte -count 2` | seg_*.rtp +1 |
| 3 | PTT 그룹 음성 5인 | `-mode ptt -scenario group_call -count 5 -no_video` | seg_*.rtp +1 |
| 4 | PTT 그룹 영상 5인 | `-mode ptt -scenario group_call -count 5` | seg_*.rtp +1 |

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

`http://<ens160>:3000/testbed/verify-history` — 회차 list + 통계 KPI + Sparkline + DetailModal (📄 PDF 인쇄).

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

### 4.3 4 ports 미기동 (S6-ENTRY-CHECK FAIL)

```bash
ss -tln | grep -E ':(4445|8081)\b'
ss -uln | grep -E ':(5060|9000)\b'
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
| TB-Console (검증) | `http://<ens160>:3000/testbed/verify-v2` |
| 회차 이력 | `http://<ens160>:3000/testbed/verify-history` |

---

## 5. 합격 흐름

```
[S1] PASS ─▶ [S2] PASS ─▶ [S3] PASS ─▶ [S4] PASS ─▶ [S5] PASS ─▶ [S6] PASS ─▶ ✅ 상용 배포
  │            │            │            │            │            │
  └ FAIL = stage gate ─▶ stage>N 자동 BLOCKED. FAIL 분석 후 해당 stage 재실행.
```

---

## 부록. 백그라운드 / 자동화

### A.1 비동기 실행 (job_id 폴링)

```bash
# 시작
JOB=$(curl -sk -X POST https://127.0.0.1:4419/api/v1/verification/stages/5 \
  -H "Authorization: Bearer $TOK" -H "Content-Type: application/json" \
  -d '{"async": true}' | jq -r .job_id)

# 폴링 (1.5s 간격 권장)
curl -sk https://127.0.0.1:4419/api/v1/verification/jobs/$JOB | jq
# items_progress.completed/total + stage_gate 필드
```

### A.2 CI / CD 연동

```bash
# JSON 출력 (verdict + totals + report_path)
python3 -m tests.cims_verify run --preset pipeline-full --json
echo $?    # 0=PASS, 1=FAIL
```

### A.3 unit test (인프라 회귀)

```bash
python3 -m unittest tests.test_verify_lib -v    # 109 OK
```

---

## 부록. --inject-fail 시연 시나리오 (gate 회귀 점검)

`--inject-fail ITEM_ID` 는 함수 호출 없이 ItemStatus.FAIL 을 반환하는 디버그
옵션. 실제 환경 파괴 없이 stage gate / immutability gate 동작을 확인하는 데
사용. backend `/stages/<N>` + `/run` 도 body `{"inject_fail": [...]}` 로 동등
지원.

### B.1 Stage gate 자동 차단 시연

S1 의 한 항목을 강제 FAIL 시켜 S2~S6 가 모두 BLOCKED 처리되는지 확인:

```bash
./cims.sh verify run --preset pipeline-full --inject-fail S1-CPP-FORMAT
# 기대:
# - S1-CPP-FORMAT 만 FAIL, S1 의 나머지는 정상 실행 (PASS)
# - S2~S6 의 모든 leaf 가 BLOCKED (함수 호출 없이 즉시)
# - stdout: [VERIFY] stage-blocked: stage=2 reason=stage1-FAIL count=2 등
# - V2Page: 노란 배너 + Stepper S2~S6 회색 + 🚫 amber 배지
# - 종료 코드 1
```

stage 단독에서도 동일:

```bash
./cims.sh verify stage1 --inject-fail S1-PY-SYNTAX
# 단일 stage 라 gate 효과는 없지만 FAIL 1건 + verdict=FAIL 확인
```

### B.2 Immutability gate 시연 (S6-ENTRY-CHECK 차단)

배포 후 manifest 변경을 가정한 회귀 테스트. `S5-MODULES-RUN-START` 를 강제
FAIL 시켜 `.deployed-manifest.json` marker 가 작성되지 않게 한 다음 S6 진입
시도:

```bash
# 정상 흐름이라면 S5 종료 시 marker 작성됨 → S6 immutability 매칭
./cims.sh verify run --preset post-deploy

# 회귀: S5-MODULES-RUN-START 강제 FAIL → marker 미작성
rm -f build/dist/.deployed-manifest.json
./cims.sh verify run --preset post-deploy \
  --inject-fail S5-MODULES-RUN-START
# 기대:
# - S5-MODULES-RUN FAIL → S6 의 모든 leaf BLOCKED (stage gate)
# - 또는 marker 만 누락된 경우: S6-ENTRY-CHECK FAIL (immutability check)

# 정상 복구
./cims.sh verify stage4    # manifest 재생성
./cims.sh verify stage5    # 재배포 + marker 갱신
./cims.sh verify stage6    # 재진입
```

### B.3 backend API 로 inject

비동기 job 도 동등:

```bash
TOK=$(curl -sk -X POST https://127.0.0.1:4419/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"login_id":"admin","password":"1234"}' | jq -r .token)

curl -sk -X POST https://127.0.0.1:4419/api/v1/verification/stages/1 \
  -H "Authorization: Bearer $TOK" -H "Content-Type: application/json" \
  -d '{"async": true, "inject_fail": ["S1-CPP-FORMAT"]}'
# → job_id 즉시 반환. /jobs/<id> 폴링 시 stage_gate 필드 + items_progress
#    의 BLOCKED count 확인.
```

### B.4 회차 기록 검증

`--inject-fail` 회차도 verify_runs/ 에 자동 기록 (기록 자체는 정상 동작):

```bash
ls verify_runs/$(date +%Y/%m)/ | tail -3
# 새 회차 파일 확인. trigger="cli" (CLI) 또는 "user"/"ci" (API).
curl -sk https://127.0.0.1:4419/api/v1/verification/runs?limit=3 | jq
# verdict=FAIL + totals.fail >= 1 확인.
```

---

## 부록. 회차 이력 / 정리

### C.1 verify_runs/ 레이아웃

각 회차 = JSON 파일 1개:

```
verify_runs/
├── 2026/05/
│   ├── 1778125235732.json    # id = ms timestamp
│   ├── 1778125658339.json
│   └── ...
└── 2026/06/
    └── ...
```

### C.2 자동 기록 트리거

- **CLI**: `python3 -m tests.cims_verify run ...` (default 기록, `--no-record`
  로 비활성).
- **Backend 비동기**: `POST /stages/<N>` 또는 `/run` body `{"async":true}`.
  job 종료 시 `_watch_job` 가 `_record_run` 호출.

### C.3 회차 정리 (retention)

```bash
# CLI: 90일 초과 회차 삭제, 최근 10개는 무조건 보존
./cims.sh verify purge-runs --days 90 --keep-min 10

# JSON 출력 (CI 연동)
./cims.sh verify purge-runs --days 30 --json

# 모두 삭제 (위험 — --force 필요)
./cims.sh verify purge-runs --days 0 --force --keep-min 0
```

또는 수동 find:

```bash
find verify_runs -name "*.json" -mtime +90 -delete
find verify_runs -type d -empty -delete
```

---

## 부록. webhook 발행 (외부 통지)

job 종료 시 verdict + 회차 메타를 외부 URL 로 자동 POST. Slack incoming
webhook / 사내 Hook 서비스 / CI gate 알림 등 연동.

### D.1 환경 설정

```bash
# 필수: POST 대상 URL
export CIMS_VERIFY_WEBHOOK_URL="https://hooks.slack.com/services/..."

# 옵션: verdict 필터 (FAIL 만 알림)
export CIMS_VERIFY_WEBHOOK_FILTER="FAIL"
# 또는 여러개: "FAIL,UNKNOWN"

# 옵션: HTTP timeout 초 (기본 5)
export CIMS_VERIFY_WEBHOOK_TIMEOUT="3"
```

설정 후 backend 재기동:

```bash
./cims.sh restart tb-csc
```

### D.2 발송 시점

CLI / backend 비동기 job 모두 `_record_run` 직후 동일하게 발송:

| 트리거 | 흐름 |
|---|---|
| `python3 -m tests.cims_verify run ...` | CLI 종료 시 fire-and-forget |
| `POST /api/v1/verification/stages/<N> {async:true}` | `_watch_job` 종료 시 fire-and-forget |

실패 (network/HTTP error) 는 stderr 한 줄 + 검증 verdict 에 영향 X.

### D.3 Payload schema

```json
{
  "run_id": 1778125658339,
  "verdict": "FAIL",
  "scope": "stage5",
  "totals": {"total": 7, "pass": 6, "fail": 1, "skip": 0, "blocked": 0},
  "elapsed_ms": 23456,
  "started_at": "2026-05-07T12:00:00.000",
  "finished_at": "2026-05-07T12:00:23.456",
  "git_branch": "main",
  "git_sha": "abc1234",
  "host": "nex-ubuntu",
  "trigger": "cli",
  "report_path": "/home/.../verify_reports/...",
  "pkg_manifest_hash": "deadbeef..."
}
```

### D.4 Slack 연동 예시

Slack incoming webhook 은 위 payload 를 그대로 받아 `text` 필드 가공이
필요. 중간에 transform 서버를 두거나, 본 webhook 을 받아 Slack format 으로
변환하는 작은 receiver 작성 (예: AWS Lambda).
