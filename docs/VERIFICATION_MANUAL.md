# CIMS 검증 수동 실행 가이드

> 원본 SSOT: `docs/VERIFICATION_PROCESS.md`
> 본 문서: 사용자가 직접 단계별로 따라할 수 있도록 정리한 실행용 체크리스트.
> 각 단계의 명령 → 기대 결과 → 실패 시 대응 순서로 구성.

---

## 0. 사전 준비 (모든 Phase 공통)

### 0.1 환경 확인

```bash
cd /home/nex/work/cims

# (a) 작업 IP 확인 (외부 인터페이스 ens160)
ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1
# 기대: 192.168.x.x 형태 (예: 192.168.199.129)

# (b) git 상태
git status
git rev-parse --short HEAD
# 기대: working tree clean (또는 의도된 변경만)

# (c) preflight (자동 진단)
./cims.sh preflight
# 기대: ens160 IP 정상, 충돌 포트 없음
```

### 0.2 TB 3종 상시 기동 확인

TB-CSC(4419) / TB-Console(3000) / TB-agent(9902) 는 **Phase 진행 중 절대 내리지 않음**.

```bash
./cims.sh status
# 기대: tb-csc(4419) / tb-console(3000) / tb-agent(9902) 가 모두 RUNNING

# 만약 안 떠있으면:
./cims.sh start tb
```

### 0.3 IP 변경 시 필수 작업

`ens160` IP 가 바뀌었다면 Phase 진행 전 **반드시** 아래 순서로:

```bash
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump      # tarball 속 cims.sh / config 최신화 (Phase 2 필수)
```

---

## 1. Phase 1 — 배포 전 검증 (build/dist 직접 기동)

**목적**: 개발/보완 후 회귀 본진. `build/dist/<모듈>/` 에서 직접 기동하여 기능 확인.
**진입 조건**: 코드 수정 완료, git 커밋/스테이징 정리.
**합격 기준**: §0.9 4시나리오 + §0.10 추가 시나리오 PASS, ERROR/FATAL 0.

### 1.1 자동 실행 (권장)

```bash
./cims.sh verify phase1
```

내부 흐름: preflight → reset → build → configure → start → 시나리오 → 리포트.
완료 후 모듈 그대로 유지 (사용자 추가 시험 가능).

**기대 결과**:
- 종료 코드 0
- 리포트: `verify_reports/<YYYYMMDD_HHMMSS>_phase1.md` 생성
- 4+3 시나리오 모두 PASS

### 1.2 수동 단계별 (자동이 실패할 때)

```bash
# (1) 빌드
./cims.sh build
# 기대: warning/error 0, build/dist/ 갱신

# (2) (선택) Python/스크립트만 변경 시 sync
./cims.sh sync csc        # csc / agent / scripts / pkg-meta / console / phone 중 하나
# (전체 sync 가 필요하면)
./cims.sh sync all

# (3) 설정
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
# 기대: csp.json / cmp.json / csc.json / csc-tb.json / cwrtc.json / .env.local 재생성

# (4) 초기화 (가입자 보존, 그 외 wipe)
./cims.sh reset
# 기대: DB TRUNCATE + 로그/녹취/배포본 디렉토리 전부 정리, TB-agent 레코드는 보존

# (5) 실행
./cims.sh start
# 기대 순서: cmp → csp → cwrtc → csc → console → phone 모두 RUNNING

# (6) 상태 확인
./cims.sh status
# 기대: 8개 서비스 (cmp/csp/csc/cwrtc/console/phone/cspsim 등) + TB 3종

# (7) 시나리오 실행 — 브라우저 (선택)
# TB-Console: http://<ens160>:3000/testbed/verify
# 화면 상단 [Phase 1] 탭 → ▶ Phase 1 실행 클릭
# (검증 UI 는 TB-CSC 4419 백엔드에 의존하므로 TB-Console 3000 에서만 동작)
```

### 1.3 수동 확인 포인트 (브라우저)

검증 UI 는 모두 **TB-Console (`http://<ens160>:3000/`)** 에서:

| 확인 위치 | 무엇을 보는가 |
|---|---|
| `/testbed/verify` | Phase 1/2/3 통합 실행 + 리포트 조회 |
| `/testbed/modules` | 버전 / 설정 템플릿 / overlay 반영 |
| Dev-Console(3001)/Test-Console(8080) | Phase 1 가입자 화면 (Test-CSC 4421 backed) — 시나리오 결과/녹취 직접 확인용 |

### 1.4 4시나리오 (`verify phase1` 자동 / TB-Console `/testbed/verify` Phase 1 탭에서 실행되는 항목)

| # | 시나리오 | 합격 포인트 |
|---|---|---|
| 1 | VoLTE 음성 2자 (B2BUA) | REGISTER × 2, INVITE 통화 연결, `seg_*.rtp` 녹취, 양 leg 동일 sesid |
| 2 | VoLTE 영상 2자 | (1) + 영상 RTP m-line 협상, 양방향 비디오 흐름 |
| 3 | PTT 그룹 음성 5인 | multipart INVITE, Conference NOTIFY, floor port 협상, 플로어 요청/그랜트 |
| 4 | PTT 그룹 영상 5인 | (3) + 영상 stream + 그룹 멤버 영상 분배 |

### 1.5 Phase 1 전용 추가 시나리오

| 항목 | 확인 방법 |
|---|---|
| CSC 가입자/그룹 변경 → NOTIFY | admin API CRUD → CspUserMap/CGroupMap 캐시 갱신 → GMS/CMS NOTIFY |
| SUBSCRIBE/NOTIFY 흐름 | 통화이력 → 호 선택 → Flow 모달에서 IdMS / GMS / CMS nodes 순서 정상 |
| (mTLS) Cert rotation e2e | `cert_rotate_pending=1` → heartbeat → agent rotate → 재기동 |

### 1.6 실패 시

- **Blocker / Major** → 코드 보완 → §1.1 부터 재수행
- **Minor** → 리포트(verify_reports/...md) 에 기록 후 진행 판단
- **Phase 1 PASS 가 Phase 2 진입 조건**

---

## 2. Phase 2 — 배포 과정 + 환경 구축

**목적**: tarball → agent 배포 체인. csc/console/csp/cmp/cspsim 전 모듈 설치 + 기동.
**진입 조건**: Phase 1 PASS.
**합격 기준**: 전 모듈 install + 기동 (csc 4445 / console 8081 / csp 5060 / cmp 9000), Test-agent 4개 정상.

### 2.1 자동 실행 (권장)

```bash
./cims.sh verify phase2
# 옵션:
#   --skip-build    이미 빌드된 상태면 빌드 생략
#   --skip-pkg      tarball 이미 최신이면 생략 (주의: stale 가능)
#   --stop-after    검증 후 전체 정리 (기본은 기동 유지)
```

**기대 결과 (22단계)**:

| 그룹 | 단계 | 기대 |
|---|---|---|
| 시작 | (1) cmd_reset --keep-processes | Phase 1 모듈 유지, 로그/DB/배포본 wipe |
| 빌드 | (2) build / (3) configure / (4) pkg | tarball 갱신 |
| csc-server | (5)~(14) admin login → agent enroll → tarball 업로드 → install → start (4445) → health | 4445 LISTEN, tcp:4445=open |
| console | (15) console Start (8081 HTTPS) | 8081 LISTEN |
| csp/cmp/sim | (16)~(21) 배포본 csc 4445 경유 admin login → 3 tarball 업로드 → 3 agent enroll → install → csp/cmp Start | 5060/udp + 9000/udp LISTEN, sim 은 install-only |
| 종료 | (22) 기본 기동 유지 | 4포트 LISTEN 상태로 종료 |

### 2.2 수동 합격 확인

```bash
# 4개 핵심 포트 LISTEN 확인
ss -tln | grep -E ':(4445|8081)\b'      # tcp 4445, 8081
ss -uln | grep -E ':(5060|9000)\b'      # udp 5060, 9000
# 기대: 4개 모두 LISTEN

# 디렉토리 구조 확인
ls build/dist/csc-server/    # agent/ csc/ console/ config/
ls build/dist/csp-server/    # agent/ csp/ config/
ls build/dist/cmp-server/    # agent/ cmp/ config/
ls build/dist/sim-server/    # agent/ sim/ config/

# Agent 4개 확인
pgrep -af cims_agent.py
# 기대: 9903 (csc-server) / 9904 (csp-server) / 9905 (cmp-server) / 9906 (sim-server)

# 리포트
ls -lt verify_reports/*_phase2.md | head -1
```

### 2.3 합격 조건 체크리스트

- [ ] Agent enroll OK × 4 (csc-server-local, csp-server-local, cmp-server-local, sim-server-local)
- [ ] Tarball 업로드 OK × 5 (csc / console / csp / cmp / cspsim)
- [ ] Install 완료 × 5
- [ ] Config overlay 반영 (csc `Server.Port=4445`, console `Port=8081`)
- [ ] Start + Health OK × 4 (csc / console / csp / cmp)
- [ ] sim install-only
- [ ] 종료 후 4포트 LISTEN 유지

### 2.4 함정 / 실패 대응

| 증상 | 원인 | 해결 |
|---|---|---|
| start_console 실패 (8081 안 뜸) | tarball 속 cims.sh 가 stale (overlay 로직 누락) | `./cims.sh sync all && ./cims.sh pkg --no-bump` 후 재실행 |
| csp start 시 LocalIp 불일치 | csp.json LocalIp 가 과거 IP | `configure --local-ip <ens160>` + `pkg --no-bump` 재실행 |
| TB-CSC 4419 변화 미반영 | csc 소스 수정 후 TB-CSC 재기동 안 함 | `./cims.sh sync csc && ./cims.sh restart tb-csc` |
| 배포 체인 이슈 | Phase 2 자체 보완 사항 | Phase 2 만 재수행 (Phase 1 재수행 불필요) |
| 기능 이슈 | Phase 1 검증 미흡 신호 | Phase 1 재수행 |

### 2.5 정리 (선택)

검증 완료 후 환경 정리하고 싶다면:

```bash
./cims.sh verify phase2 --stop-after
# 또는 부분 stop
./cims.sh stop csc csp cmp console
```

---

## 3. Phase 3 — 서비스 검증 전용

**목적**: Phase 2 배포본에서 4시나리오 실행 (배포 X, agent enroll X, wipe X).
**진입 조건**: Phase 2 완료 (4포트 LISTEN).
**합격 기준**: 4시나리오 4/4 PASS, 배포본 ERROR/FATAL 0.

### 3.1 진입 조건 체크 (수동)

```bash
ss -tln | grep -E ':(4445|8081)\b'      # tcp 4445 (csc), 8081 (console)
ss -uln | grep -E ':(5060|9000)\b'      # udp 5060 (csp), 9000 (cmp)
# 기대: 4개 모두 LISTEN
# 미충족 시 → Phase 2 선행 실행
```

### 3.2 자동 실행 (권장)

```bash
./cims.sh verify phase3
```

옵션 없음 (Phase 2 결과물에 의존).

**내부 흐름**:
1. 진입 조건 체크 (위 4포트)
2. DB 에서 가입자/그룹 선택 (volte_subscriptions / ptt_subscriptions / ptt_groups)
3. 배포본 csp jsonlDir (`build/dist/csp-server/csp/config/`) 에 `access_services.jsonl` 시드
4. 배포본 csp 에 `SIGUSR1` (ConfigCache reload)
5. 4시나리오 순차 실행 (cspsim → 배포본 csp 5060)
6. 결과 요약 + 리포팅

### 3.3 4시나리오 판정 기준

| # | 시나리오 | cspsim 옵션 | 합격 |
|---|---|---|---|
| 3.1 | VoLTE 음성 2자 | `-mode volte -count 2 -no_video` | `seg_*.rtp` +1 이상 |
| 3.2 | VoLTE 영상 2자 | `-mode volte -count 2` | `seg_*.rtp` +1 이상 |
| 3.3 | PTT 그룹 음성 5인 | `-mode ptt -scenario group_call -count 5 -no_video` | `seg_*.rtp` +1 이상 |
| 3.4 | PTT 그룹 영상 5인 | `-mode ptt -scenario group_call -count 5` | `seg_*.rtp` +1 이상 |

### 3.4 합격 확인 (수동)

```bash
# 녹취 파일 갯수 확인
find ext_mnt/service_log -name 'seg_*.rtp' -newer verify_reports -mmin -10 | wc -l
# 기대: 28 이상 (4 + 4 + 10 + 10)

# 배포본 csp/cmp ERROR/FATAL
grep -E 'ERROR|FATAL' build/dist/csp-server/csp/log/*.log | wc -l
grep -E 'ERROR|FATAL' build/dist/cmp-server/cmp/log/*.log | wc -l
# 기대: 0 (권장)

# 리포트
ls -lt verify_reports/*_phase3.md | head -1
cat verify_reports/$(ls -t verify_reports/ | grep phase3 | head -1)
```

### 3.5 Console UI 진입점

| Console | URL | 용도 |
|---|---|---|
| **TB-Console** | `http://<ens160>:3000/testbed/verify` | **Phase 1/2/3 실행 + 리포트 조회 (권장)** — TB-CSC 4419 backed |
| 배포본 console | `https://<ens160>:8081/testbed/modules` | 배포된 csc/csp/cmp/sim 설정 확인·편집 (배포본 csc 4445 backed) |
| 배포본 console | `https://<ens160>:8081/testbed/verify` | 같은 화면이 뜨지만 backend 가 배포본 csc — 같은 호스트에서만 동작 |

검증 실행은 **TB-Console (3000)** 에서 수행하는 것이 표준. 배포본 console (8081) 은 운영 환경 모듈관리에 사용.

### 3.6 실패 시 분류

| 증상 | 다음 단계 |
|---|---|
| 진입 조건 4포트 미충족 | Phase 2 선행 실행 |
| 시나리오 실패 + Phase 1 도 같은 증상 재현 | 코드 이슈 → Phase 1 재수행 |
| 시나리오 실패 + Phase 1 정상 | 배포 경로/설정/환경 의존 버그 → Phase 2 배포 설계 재검토 |
| 개발/보완 사항 실패 | Console (8081) 에서 직접 재현 + 이슈 리포트 |

---

## 4. 빠른 참조 (Cheatsheet)

### 4.1 한 사이클 풀 실행 (콜드 스타트)

```bash
cd /home/nex/work/cims

# 0. 환경 확인
./cims.sh status
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)

# 1. Phase 1
./cims.sh verify phase1

# 2. Phase 2 (tarball 갱신 후)
./cims.sh pkg --no-bump
./cims.sh verify phase2 --skip-build --skip-pkg     # 22단계, 기동 유지로 종료

# 3. Phase 3
./cims.sh verify phase3                             # 4시나리오
```

### 4.2 리포트 확인

```bash
ls -lt verify_reports/ | head -10

# 가장 최근 리포트
cat verify_reports/$(ls -t verify_reports/ | head -1)
```

### 4.3 디버그 진입점

| 무엇을 볼 때 | 어디 |
|---|---|
| Phase 1 모듈 로그 | `build/dist/log/<module>.log` |
| 배포본 모듈 로그 | `build/dist/{csc,csp,cmp}-server/<module>/log/` |
| Service log (호 단위) | `ext_mnt/service_log/<type>/YYYY/MM/DD/HH/.../*.d/` |
| SIP msg log | `ext_mnt/msg_log/csp/sip/YYYY/MM/DD/HH/sip.jsonl` |
| TB-Console (검증 실행 + 로그인 가능) | `http://<ens160>:3000/testbed/verify` |
| Dev-Console (Phase 1 가입자, 로그인 가능) | `http://<ens160>:3001/` |
| Test-Console (정적 dist 검증 전용, **로그인 불가**) | `https://<ens160>:8080/` — `/api` proxy 없음, GET / 만 OK |
| 배포본 console (모듈관리, **로그인 불가**) | `https://<ens160>:8081/` — Test-Console 과 동일 한계 |
| Flow viewer | 통화이력 페이지 (CallLogs / VolteHistory / PttHistory) 에서 호 클릭 → Flow 모달 |

> **Console 로그인 가능 여부**: vite dev (3000/3001) 만 `/api` proxy 가 동작. `npx serve dist` 기반(8080/8081) 은 정적 SPA 만 서빙하므로 로그인/CRUD 불가. dist UI 검증은 GET / 200 응답으로 충분, 실제 조작은 TB/Dev-Console 사용.

### 4.4 흔한 함정 빠른 체크

```bash
# tarball 안의 cims.sh 가 stale 한지
tar tzf build/dist/pkg/cims-*.tar.gz | grep cims.sh

# csp.json LocalIp 가 ens160 IP 와 일치하는지
grep LocalIp build/dist/csp/config/csp.json

# cwrtc.json LocalIp
grep LocalIp build/dist/cwrtc/config/cwrtc.json

# JWT 시크릿 변경 시 로그인 세션 무효 → 재기동 필요
grep -l jwt_secret build/dist/csc/config/*.json

# TB-Console 모듈관리에서 csc start 후 4421 LISTEN 확인
ss -tlnp | grep -E ':(4419|4421)\b'
# 기대: 4419 (TB-CSC) + 4421 (Test-CSC) 둘 다 — 같은 PID 면 잘못된 상태 (csc-tb.json bind)

# 8080 에서 로그인 시도 시 항상 404 — 정적 서빙 한계 (버그 아님)
curl -k -X POST https://<ens160>:8080/api/v1/auth/login -d '{}'  # → 404 정상
```

### 4.5 모듈관리(TB-Console) 에서 시작이 실패할 때

증상: `POST /api/v1/services/csc/start` 가 returncode 0 인데 csc 가 실제로 죽어있음 (bind 실패).

원인: TB-CSC 환경변수(`CIMS_CSC_CONFIG=csc-tb.json` 등)가 subprocess 로 누수되어 자식 csc_app.py 가 TB 포트 4419/4431 bind 시도 → 충돌.

수정 후 동작 (`csc/src/handlers/service_control.py` 의 `_sanitized_env`):
- `CIMS_CSC_CONFIG`, `CIMS_AGENT_SYNC_PORT` 등 TB 전용 env 를 subprocess env 에서 제거
- 자식 csc_app.py 는 base csc.json (4421/4430) 으로 정상 기동

소스 수정 시:
```bash
./cims.sh sync csc
./cims.sh restart tb-csc      # 변경된 service_control.py 반영
```

---

## 5. 합격/실패 판정 흐름

```
[Phase 1 verify]
   PASS ─▶ [Phase 2 verify]
              PASS ─▶ [Phase 3 verify]
                          PASS ─▶ ✅ 배포 가능
                          FAIL ─▶ §3.6 분류
              FAIL ─▶ §2.4 함정 / Phase 1 회귀 여부
   FAIL ─▶ 코드 보완 → Phase 1 재수행
```

각 Phase 의 합격 기준은 §0.7 (공통) + Phase 별 합격 조건.
의문 있을 때 원본: `docs/VERIFICATION_PROCESS.md`.
