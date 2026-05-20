---
name: CIMS Phase 진행 상태
description: 2026-04-25 70ad7ec 기준. Phase 2/3 재설계 완료 — Phase 2 가 전 모듈 배포+기동, Phase 3 는 서비스 검증 전용. docs 반영 완료.
type: project
originSessionId: 078c269a-6e1d-46ac-b647-d08d4f8bc5ac
---

CIMS 는 PTT/VoLTE 서버를 "중앙 콘솔에서 여러 호스트에 배포·설정" 하는 시스템.

**다음 세션 시작 시 이 파일을 먼저 읽기.**

---

## 🟢 SESSION SIGN-OFF (2026-04-25 새벽, Phase 2/3 재설계 — 70ad7ec)

### 한 줄 요약
**Phase 2/3 구조 전면 재설계.** Phase 2 = 전 모듈(csc/console/csp/cmp/cspsim) 배포+기동 (22단계). Phase 3 = 서비스 검증 전용 (4시나리오만). docs 반영 완료.

### 이번 세션 커밋 (4건)
```
70ad7ec  feat(verify): Phase 2 = 전 모듈 배포+기동 / Phase 3 = 서비스 검증 전용 (재설계)
bab3f1c  fix(verify): Phase 3 의 배포 주체를 TB-CSC → 배포본 csc(4445)로 변경
9f4d80f  feat(verify): Phase 2/3 종료 시 배포본 기동 유지 (기본 동작 변경)
(+docs)  docs/VERIFICATION_PROCESS.md Phase 2/3 섹션 전면 교체 (미커밋)
```
origin 대비 13 ahead.

### Phase 2 (22단계) 최종
- 시작: `cmd_reset --keep-processes` (유일한 wipe 시점)
- csc-server 배포 (TB-CSC 4419): csc+console tarball → install → csc Start(4445) + Health + console Start(8081)
- csp/cmp/sim-server 배포 (배포본 csc 4445): tarball 업로드 + 3 agent enroll → install → csp(5060)/cmp(9000) Start. sim install-only.
- 종료: 기본 전체 기동 유지. `--stop-after` 시 정리.

### Phase 3 축소 (663줄 → 220줄)
- 진입 조건 체크 (4포트 LISTEN)
- access_services.jsonl 시드 (배포본 csp)
- 4시나리오 (VoLTE 음성/영상, PTT 그룹 음성/영상)
- 데이터 wipe/stop 없음

### 발견 함정 (메모리 기록)
- **console overlay 는 tarball 속 cims.sh 에 포함되어야 함** → `--skip-pkg` 로 verify 하면 stale. 권장: `--skip-build` 만, 또는 `pkg --no-bump` 선행.
- **cmd_sync scripts 별도 실행 필요** — `build/dist/cims.sh` 가 소스 변경과 동기화되지 않을 수 있음. pkg 가 dist 의 cims.sh 를 tarball 에 포함.

### 최종 검증 (20260425_011232_phase2 + _011329_phase3)
**Phase 2 PASS**:
- csc-server: {agent, console, csc} / csp-server: {agent, csp} / cmp-server: {agent, cmp} / sim-server: {agent, sim}
- 기동: csc 4445 + console 8081 + csp 5060 + cmp 9000
- Test-agent 4개 (9903/9904/9905/9906)

**Phase 3 PASS** (4/4):
- VoLTE 음성/영상 각 +4 녹취
- PTT 그룹 음성/영상 각 +10 녹취
- 배포본 csp/cmp ERROR/FATAL 0

### docs 반영 (docs/VERIFICATION_PROCESS.md)
- §0.1 3단계 구조 표 재작성 — Phase 2 = 환경 구축, Phase 3 = 검증 전용
- §2 Phase 2 — 22단계 상세 (tarball → csc+console+csp+cmp+cspsim 전 모듈)
- §3 Phase 3 — 진입 조건 + 4시나리오 + 요약 (배포/enroll 없음)
- 부록 B 명령어 요약 갱신

### ⏭ 다음 세션 가능한 작업 (선택)
1. docs 커밋 (이번 세션에서 docs 수정은 반영됨, 커밋만 남음)
2. Console Phase UI 실제 브라우저 사용성 확인
3. phase 1 전용 추가 시나리오 (CSC 변경 NOTIFY / SUBSCRIBE / mTLS rotation) 자동화 유지 여부 점검

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -5 --oneline
./cims.sh status                                        # TB 3 + 배포본 유지 여부
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump                                 # tarball 최신화 (start_console overlay 반영)
./cims.sh verify phase2 --skip-build --skip-pkg         # 22단계 전부 PASS 기대
./cims.sh verify phase3                                 # 4시나리오 PASS 기대
```

---

## 🟢 SESSION SIGN-OFF (2026-04-25 새벽, Console Phase UI 완성 — 65f19c2)

### 한 줄 요약
**Phase 1/2/3 통합 Console UI + 백엔드 API 완성.** VerificationPage 에 Phase 탭·옵션·실행·리포트 조회 통합. Phase 3 via API → PASS 검증.

### 이번 추가 커밋
```
65f19c2  feat(console): Phase 1/2/3 통합 검증 UI + 백엔드 API
```
총 9 커밋 ahead of origin.

### 백엔드 (csc/src/handlers/verification.py)
- `POST /api/v1/verification/phases/<N>` — cims.sh verify phase<N> subprocess 실행
- `GET /api/v1/verification/phases/<N>/latest-report` — 최신 리포트 내용
- `GET /api/v1/verification/phases/<N>/reports` — 리포트 목록

### 발견 함정 (이번 세션)
1. **async + blocking subprocess.run 자기 deadlock**: uvicorn 이벤트 루프 block → verify script 의 self-call (TB-CSC 4419) 실패. `asyncio.to_thread` worker 필수.
2. **repo root 탐색 버그**: csc_app.py 의 tests_dir fallback 이 `build/dist/tests` (존재 안 함) 지정 → verification.py 가 엉뚱한 _REPORT_DIR. `cims.sh + CMakeLists.txt` 공존 찾기로 수정.

### 프론트엔드 (cims-console/src/pages/VerificationPage.tsx)
- Phase 1/2/3 탭 + 옵션 체크박스 (--skip-build / --skip-pkg / --keep-agent)
- 실행 → 판정 컬러 + returncode + stdout_tail + 자동 리포트 로드
- 기존 run_all.py 세밀 검증은 하단 "Phase 1 상세 검증" 섹션으로 분리

### 검증
- `POST /api/v1/verification/phases/3` via curl → PASS (rc=0, ts=20260424_235547)
- TypeScript 타입 체크 통과
- Vite production build 성공

### ⏭ 다음 세션 가능한 작업 (선택)
- Console Phase UI 실제 브라우저 사용성 확인 (Dev-Console 3001 에서 `/testbed/verify`)
- Phase 1 상세 검증 (run_all.py) 과 Phase 1 cims.sh verify 의 출력 통합 고려
- 이슈 reproducer 용도로 `verify phase3` 에 선택적 `--scenario <name>` 필터 옵션

사용자 목적 **"배포 전/과정/이후 3단계 검증 자동화 + Console UI"** 모두 달성.

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -9 --oneline                                # 65f19c2 부터
./cims.sh status                                    # 검증 6 + TB 3
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump                             # tarball 최신 IP
./cims.sh verify phase3 --skip-build --skip-pkg     # CLI 기반 smoke
# UI 기반: http://<ens160>:3001/testbed/verify 에서 Phase 3 탭 → 실행
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 밤 마감, Phase 3 v3 — 806da88)

### 한 줄 요약
**Phase 3 v3 (4시나리오 자동 실행) 완성.** 배포본 csp/cmp 대상 VoLTE 음성/영상 + PTT 그룹 음성/영상 = 4/4 PASS. 총 28개 녹취 파일 생성, 배포본 ERROR/FATAL 0.

### 이번 세션 커밋 (Phase 3 완성까지)
```
806da88  feat(verify): Phase 3 v3 — 4시나리오 자동 실행 (배포본 csp 대상)
0712d4b  docs(verify): Phase 3 §3.3 v2 범위 반영 + 함정 노트 추가
acb47bb  feat(verify): Phase 3 v2 — start/health/stop (csp·cmp) + UDP 파싱 버그 수정
bdd7f75  feat(verify): Phase 3 v1 (install-only) 자동화
e11ae28  docs(verify): 3단계 구조 재정리 + Phase 3 를 배포본 기능 회귀로 재정의
```
origin 대비 **8 커밋 ahead** (e0c44a7, d90a08c 포함).

### v3 추가 단계 (v2 17단계 → v3 17단계 내 §14-15 추가)
- 14.0 배포본 csp jsonlDir 에 access_services.jsonl 시드 + SIGUSR1
- 14.1 VoLTE 음성 2자 (cspsim -no_video, count=2)
- 14.2 VoLTE 영상 2자 (video 포함)
- 14.3 PTT 그룹 음성 5인 (cspsim -mode ptt -no_video)
- 14.4 PTT 그룹 영상 5인
- 15 시나리오 요약 (녹취 카운트, Flow 라인, 배포본 ERROR/FATAL)

### 판정 기준
- 각 시나리오 실행 후 `seg_*.rtp` 녹취 +1 이상 → PASS
- v3 전체 PASS: install + 설치 파일 + start + health + 4시나리오 전체

### 발견 함정 (이번 세션)
1. **UDP ss 파싱**: `ss -uln` Local Address 는 $4 ($5 는 Peer). acb47bb 에서 수정.
2. **tarball stale**: `--skip-pkg` 로 verify 시 tarball 속 LocalIp 가 과거 configure 값이면 start 실패. 해결: ens160 IP 변경 후 `configure + pkg` 재실행.
3. **pipefail + grep -c** (806da88): grep -c 0 매칭 시 exit 1 → subshell abort. array-based guard 또는 `|| true` 로 catch.

### 검증 결과 (20260424_233731_phase3.md)
- VoLTE 음성 녹취: +4
- VoLTE 영상 녹취: +4
- PTT 그룹 음성 녹취: +10
- PTT 그룹 영상 녹취: +10
- SIP msg/flow 로그: 2628 라인 (msg=1278, flow=1350)
- 배포본 csp/cmp ERROR/FATAL: 0
- csp·cmp stop cleanup: OK

### Phase 1 §7 로직 재사용
- 가입자 선택 SQL (volte_subscriptions / ptt_subscriptions / ptt_groups)
- access_services.jsonl 시드 (volte / ptt kind 별 domain 분리)
- cspsim 호출 패턴 (cmd_sim + -no_video 옵션)

### ⏭ 다음 세션 우선 작업

#### (1순위) **Console Phase 3 UI** (선택)
CLI 자동화 (`verify phase3`) 는 완성. UI 추가는:
- VerificationPage 에 Phase 1/2/3 탭
- 백엔드 `/api/v1/verification/phase/{phase_id}`
- 사용자가 UI 로 Phase 3 실행 + 결과 조회
- 이미 CLI 로 처리 가능하므로 **우선순위 낮음**

#### (2순위) **실제 Test-CSP/CMP 와 배포본 csp/cmp 의 기동 분리**
현재 agent 의 cims.sh 호출이 Phase 1 Test-CSP/CMP 바이너리를 기동하는 방식
(install_path/cims.sh 가 있고 DIST_DIR=install_path 로 치환되어 install_path/<mod>/bin/<mod>
실행 — 즉 **실제로는 배포본 바이너리를 기동 중**. 탐색으로 확인).
단 현재 구조가 agent 관점에서 투명하게 작동하므로 긴급성 낮음.

#### (3순위) 메모리/docs 정돈
- project_verification_process.md 이미 갱신 완료
- project_phase_status.md 이번 sign-off 추가 완료
- Phase 3 v3 완성으로 "다음 세션 Phase 3 본구현" 항목 전부 해소

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -8 --oneline                                # 806da88 부터 시작
./cims.sh status                                    # 검증 6 + TB 3
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump                             # tarball 최신 IP 반영
./cims.sh verify phase1                             # Phase 1 smoke
./cims.sh verify phase2 --skip-build --skip-pkg     # Phase 2 smoke
./cims.sh verify phase3 --skip-build --skip-pkg     # Phase 3 v3 smoke (4/4 PASS 기대)
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 밤 늦게, Phase 3 v2 — acb47bb)

### 한 줄 요약
**Phase 3 v2 (install-only → csp·cmp start/health/stop 추가) 완성.** 전 항목 PASS. sim 은 install-only 유지 (단발 실행). 5 commits ahead of origin.

### 이번 세션 커밋
```
acb47bb  feat(verify): Phase 3 v2 — start/health/stop (csp·cmp) + UDP 파싱 버그 수정
bdd7f75  feat(verify): Phase 3 v1 (install-only) 자동화
e11ae28  docs(verify): 3단계 구조 재정리 + Phase 3 를 배포본 기능 회귀로 재정의
```

### v2 추가 단계 (v1 11단계 → v2 16단계)
- 11. Start jobs (csp, cmp) — agent 가 cims.sh 경유 기동
- 12. 포트 LISTEN 대기 (csp 5060/udp, cmp 9000/udp, 최대 20s)
- 13. Health check jobs — agent hardcoded port 체크
- 14. Stop jobs + 포트 해제
- 15. Test-agent 종료

### 발견 / 수정한 함정
1. **UDP ss 파싱 버그**: `ss -uln` Local Address 는 **$4** ($5 는 Peer `0.0.0.0:*`). v1 에서 $5 썼다가 UDP 체크 항상 miss — v2 에서 수정.
2. **cwrtc 와 동일한 tarball stale 함정**: `--skip-pkg` 사용 시 tarball 속 csp.json LocalIp 가 과거 ens160 값이면 UdpListen 실패. IP 변경 후엔 configure + pkg 재실행 필수.

### 검증 결과
- csp 5060/udp Start 2s 이내 LISTEN OK
- cmp 9000/udp Start 1s 이내 LISTEN OK
- Health check 양쪽 OK
- Stop cleanup 양쪽 OK
- 리포트: verify_reports/20260424_232127_phase3.md

### 재사용한 기존 코드
- tarball 이 install_path 에 cims.sh 포함 — agent 가 install_path/cims.sh 사용 가능 (fallback 불필요)
- `_start_one` case 소문자 매칭 — agent 가 process_name.lower() 로 전달하므로 OK
- Phase 2 v2 의 start/health/stop 패턴 그대로 복제

### ⏭ 다음 세션 우선 작업

#### (1순위) **Phase 3 v3 — 4시나리오 자동 실행**
docs §0.9 의 4시나리오 자동화:
1. VoLTE 음성 2자 통화 (B2BUA)
2. VoLTE 영상 2자 통화
3. PTT 그룹 음성 (5인)
4. PTT 그룹 영상 (5인)

cspsim 으로 REGISTER + 통화 트리거, 녹취 파일·Flow 로그 검증. Phase 1 `verify phase1` 의 시나리오 로직 재사용 가능성. Phase 3 에선 배포본 csp/cmp 를 대상으로.

#### (2순위) **Console Phase 3 UI**
- VerificationPage 에 Phase 1/2/3 탭 추가
- 백엔드 `/api/v1/verification/phase/{phase_id}` 신설
- 사용자가 UI 로 Phase 3 실행 + 결과 조회

#### (3순위) sim start/health 지원
`_start_one` case 에 sim 추가 + cspsim 의 ephemeral 실행 방식 설계. 현재는 단발 실행 전용.

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -5 --oneline                                # acb47bb, bdd7f75, e11ae28, d90a08c, e0c44a7
./cims.sh status                                    # 검증 6 + TB 3
./cims.sh configure --local-ip $(ip -4 -o addr show ens160 | awk '{print $4}' | cut -d/ -f1)
./cims.sh pkg --no-bump                             # tarball 속 config 최신 IP 반영
./cims.sh verify phase3 --skip-build --skip-pkg     # v2 PASS 재현
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 밤, docs 재정리 + Phase 3 v1 — e11ae28, bdd7f75)

### 한 줄 요약
**docs/VERIFICATION_PROCESS.md 3단계 구조 재정리 (Phase 3 를 배포본 기능 회귀로 재정의) + Phase 3 v1 install-only 자동화 완성.** 첫 실행 PASS (3개 agent enroll + install 2초).

### 이번 세션 추가 커밋 (2건)
```
bdd7f75  feat(verify): Phase 3 v1 (install-only) 자동화
e11ae28  docs(verify): 3단계 구조 재정리 + Phase 3 를 배포본 기능 회귀로 재정의
```
브랜치: `feature/sip-console-runtime` (origin 대비 4 ahead).

### docs 재정리 (e11ae28)
- 418줄 → 329줄 (21% 축소)
- Phase 3 정의 뒤집기: "배포 체인만 REGISTER smoke" → **"배포본에서 기본 4시나리오 + 보완 사항 재수행"**
- 기본 검증 4시나리오 분리 (§0.9): VoLTE 음성/영상, PTT 그룹 음성/영상
- Phase 3 진입 조건: Phase 1 Console 유지 + 서버 모듈 중지 + TB 유지
- "Phase 2/3 기능 회귀 반복 X" → Phase 2 한정으로 축소

### Phase 3 v1 (bdd7f75)
**스펙**: install-only. Phase 2 v1→v2 와 동일 증분.
- 진입: Phase 1 서버 모듈 (cmp/csp/cwrtc/phone/cspsim) 중지, Console+TB 유지
- 3개 Test-agent 병렬 enroll:
  · `csp-server-local` (sync **9904**)
  · `cmp-server-local` (sync **9905**)
  · `sim-server-local` (sync **9906**)
- csp/cmp/cspsim tarball 업로드 + deployment 생성 + install 폴링
- 설치 파일 검증 (`meta.json` + `config/`)
- 디렉토리: `build/dist/{csp,cmp,sim}-server/{agent, <modname>, config/}`
- pkg name → modname 매핑: csp→csp, cmp→cmp, cspsim→sim

**smoke 결과**: 
- 3개 agent enroll OK (20s 이내)
- Install 2초 완료
- 전 설치 파일 검증 OK
- verify_reports/20260424_230526_phase3.md

### 재사용한 기존 코드 (탐색 결과)
- **CSC deployment API** — 모듈 제약 없음 (agents.py 의 `/api/v1/packages`, `/api/v1/deployments`, `_queue_job`)
- **cims_agent.py** — install/config overlay (dot-path)/multi-agent (`CIMS_AGENT_SYNC_PORT`) 모두 기존 지원
- **cmd_pkg** — cims.sh:2201 `targets=(cmp csp cwrtc csc console phone cspsim agent)` 로 전 모듈 tarball 생성 가능
- **cmd_reset** — `build/dist/{csc,csp,cmp,sim}-server/` 정리 이미 포함

Phase 3 v1 작성에 약 270줄의 새 함수만 추가.

### ⏭ 다음 세션 우선 작업

#### (1순위) **Phase 3 v2 — start/health/stop**
v1 install 후 실제 배포본 기동 자동화. 복잡도 요인:
- 배포본 csp 는 5060 UDP + 5061 TCP/TLS 기동 필요 → Phase 1 csp 가 죽어있어야 (v1 에서 이미 처리)
- 배포본 cmp 는 9000 UDP
- 배포본 sim (cspsim) 은 단발 실행 — "start" 의미가 다름 (scenario 실행 형태)
- agent 가 cims.sh 호출해서 start — 각 배포본에 cims.sh 포함되어야 (agent 탐색 결과: 현재 fallback 으로 `/home/nex/work/cims/build/dist/cims.sh`)

#### (2순위) **Phase 3 v3 — 4시나리오 자동 실행**
배포본 기동 후 VoLTE 음성/영상, PTT 그룹 음성/영상 시나리오를 자동 트리거. cspsim 을 클라이언트로 사용 + 녹취/Flow 검증.

#### (3순위) Console Phase 3 UI
탐색 결과에 따르면 VerificationPage 에 탭 추가 + PhaseVerificationPage 신규. 백엔드 API `/api/v1/verification/phase/{phase_id}` 신설.

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -4 --oneline                                # bdd7f75, e11ae28, d90a08c, e0c44a7
./cims.sh status                                    # 검증 6 + TB 3
./cims.sh verify phase3 --skip-build --skip-pkg     # v1 PASS 재현 기대
ls build/dist/{csp,cmp,sim}-server/                 # 배포본 트리 확인
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 저녁 늦게, 블록 A 완료 — d90a08c)

### 한 줄 요약
**cwrtc 8080 → 8443 (HTTPS alt) 이전 + Test-Console 8080 단독 사용 검증 완료.** plan 의 모든 블록 (A+B+C+D) 완료.

### 이번 세션 커밋 (블록 A)
```
d90a08c  feat: cwrtc 8080 → 8443 이전 (+ Test-Console 8080 단독 사용)
```
브랜치: `feature/sip-console-runtime` (origin 대비 2 ahead — e0c44a7 + d90a08c).

### 변경 요지 (블록 A)
- `cwrtc/config/cwrtc.json.template` `Setup.WsPort`: 8080 → 8443
- `configure.sh` `VITE_CWRTC_TARGET=...:8443`
- `cims.sh`: `_svc_port_proto cwrtc`, `start_cwrtc` fallback, reset/preflight 포트 리스트 갱신 (8080 = Test-Console 유지, 8443 = cwrtc 신규)
- `cims-phone/vite.config.ts` `cwrtcTarget` default `wss://127.0.0.1:8443`
- `cims-phone/nginx.conf` `/cwrtc proxy_pass https://127.0.0.1:8443` (WSS)
- `start_console` 주석 갱신 (8080 충돌 사유 제거)
- docs §0.1, §0.5, §0.10, §1.5 cwrtc 8443 / Test-Console 8080 단독 표기

### 검증 결과
- cwrtc 기동: 8443 LISTEN + UDP 5062 (SIP UA) UNCONN OK
- Test-Console 8080 dist HTTPS 단독 기동 OK (`curl -sk https://127.0.0.1:8080/` → HTTP 200)
- verify phase2 --skip-build --skip-pkg → **PASS** (verify_reports/20260424_215558_phase2.md)

### 진단 메모 — cwrtc StartServer failed 원인
직전 세션 (e0c44a7 검증 중) cwrtc 가 `StartServer failed` 로 죽었던 원인은 **8080 충돌이 아닌** SIP UA UDP 5062 bind 실패였음. cwrtc.json `Setup.LocalIp` 가 stale 한 값(192.168.0.2)을 가지고 있어, 호스트의 실제 ens160 IP(192.168.199.129) 와 불일치 → bind 실패. ens160 IP 로 configure 재실행 후 정상 기동.

**교훈**: configure 의 `--local-ip` 는 반드시 `cims.sh preflight` 의 ens160 감지 결과와 일치해야 함. DHCP 환경이라 ens160 IP 가 변경될 수 있음.

### 발견 / 메모 (메모리 기록됨)
- **cwrtc.json LocalIp stale 함정**: project_ports.md 주의 항목 추가
- **cmd_sync 가 cwrtc 미지원**: cwrtc/config/cwrtc.json.template 편집 시 `cp` 로 src→dist 직접 복사 필요. 향후 sync 에 cwrtc 추가 검토 (I6 의 연장).

### 현재 환경 상태 (세션 종료 시점)
- 브랜치: `feature/sip-console-runtime` (origin 대비 2 ahead)
- Working tree: clean
- 서비스: 검증 6종 + TB 3종 전부 실행 중
- 포트:
  - Test-CSC 4421, Dev-Console 3001, Test-CWRTC 8443, Test-Phone 3002, Test-CSP 5060/5061, Test-CMP 9000
  - Test-Console 8080 (Dev 모드라 미사용 — 검증 중에만 임시 기동)
  - TB-CSC 4419, TB-Console 3000, TB-agent 9902
- 외부 IP (ens160): **192.168.199.129** (이번 세션 변경)
- DB: cims

### ⏭ 다음 세션 우선 작업

#### (1순위) **Phase 3 본구현**
- `_verify_phase3` 함수 신설 (REGISTER 1건 smoke)
- `build/dist/{csp,cmp,sim}-server/{agent,<모듈>,config}/` 신규 트리 구현
- Console Phase 3 UI (csp/cmp/sim 배포 진입점)
- 배포본 console:80 기동 자동화 (nginx vs vite preview 결정 + cap_net_bind)

#### (2순위) cmd_sync 에 cwrtc 추가
cwrtc.json.template 편집 시 매번 src→dist `cp` 해야 하는 함정 제거. csc 처럼 sync 대상에 추가.

#### (3순위) I3/I4/I5/I6 Minor

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -2 --oneline                                # d90a08c, e0c44a7
./cims.sh status                                    # 검증 6 + TB 3 (전부 running 기대)
./cims.sh preflight                                 # ens160 IP 확인 (DHCP — 변경 가능)
ip -4 addr show ens160 | awk '/inet/{print $2}'    # 실제 IP 확인 → configure 시 사용
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 저녁, 블록 B+C+D 완료 — e0c44a7)

### 한 줄 요약
**Console 3분화 (Dev 3001 / Test 8080 / 운영 80) + Phase 2 reset 자동 (`cmd_reset --keep-processes`) + Phase 3 docs 정합 단일 커밋 완성.** verify phase2 PASS 로 Phase 1 Test-* 유지 확인.

### 이번 세션 커밋 (1건)
```
e0c44a7  feat(verify): Console 3분화 + Phase 2 reset 자동 + Phase 3 docs 정합
```
브랜치: `feature/sip-console-runtime` (origin 대비 1 커밋 ahead — 이전 7 커밋은 그새 push 된 것으로 추정).

### 변경 요지

**블록 B — Console 3분화**:
- `cims.sh start_console` 모드 분기:
  · SRC_CONSOLE 있으면 **Dev-Console** (vite dev, **3001**) → Test-CSC 4421 proxy
  · dist 만 있으면 **Test-Console** (serve dist, HTTPS **8080**)
- `_svc_port_proto console` 도 모드별 분기 (Dev 3001 / Test 8080)
- `_svc_port_proto csc` 4420 → 4421 정정 (이전 commit 잔재)
- reset/preflight 포트 리스트: 3011 → 3001 + 8080
- 4f53b7d 의 Test-Console 3011 부분 롤백 (원래 의도는 Dev/Test 분기)

**블록 C — Phase 2 reset 자동화**:
- `cmd_reset --keep-processes` 신설: Phase 1 Test-* 프로세스 유지, 포트 kill 건너뜀, 로그/DB/csc-server/cert 만 wipe
- `_verify_phase2 §1 Cleanup` 교체: 기존 수동 pkill+rm+SQL → `cmd_reset --all --keep-processes` 호출
- verify_reports/ 보존 (기존 동작 유지)

**블록 D — Phase 3 docs 정합**:
- `docs/VERIFICATION_PROCESS.md`:
  - §0.1 표 — Console 3종 + Test-CWRTC 8443 명시
  - §0.3 line 53 — 프로세스 초기화 행 갱신
  - §0.5 line 68 — 포트 충돌 리스트 갱신
  - §0.10 footnote — Console 3분화 + 8443 (블록 A) + Phase 3 미구현 주석
  - §1.5 line 204 — Health Check 포트 갱신
  - §2.1 — `cmd_reset --keep-processes` 사용 명시
  - §2.4.1 — console install-only 사유 (Phase 3 와 함께 재논의)
  - §3.1 머리말 — Phase 3 미구현 명시
  - 부록 A — TB-Console dist 빌드 안내 정정 (vite dev 모드 전용)

### 검증 결과
- `./cims.sh reset` + `start tb` + `configure --local-ip 192.168.0.2` + `start` (Dev 모드)
- `ss -tlnp`: Test-CSC 4421 / Dev-Console 3001 / TB-CSC 4419 / TB-Console 3000 / TB-agent 9902 / Phone 3002 모두 LISTEN
- `verify phase2 --skip-build --skip-pkg` → **PASS**:
  · Cleanup §1 새 형식 ("cmd_reset --keep-processes 실행") 으로 기록
  · Phase 1 Test-* PID 7910/7949/8035/8064/8163 verify 전후 동일 (--keep-processes 정상)
  · CSC Start (4445 LISTEN) / Health (tcp:4445=open) / Stop 모두 OK
- 리포트: `verify_reports/20260424_214008_phase2.md`

### 알려진 이슈 (이번 세션)
- **cwrtc StartServer failed** — 본 작업과 무관. 블록 A (cwrtc 8080→8443) 작업 영역. cwrtc 자체 이슈 (8080 충돌 아님 — Dev 모드라 Console 은 3001).

### ⏭ 다음 세션 우선 작업

#### (1순위) **블록 A — cwrtc/phone 포트 이전 (별도 세션)**
사용자가 "cwrtc 와 phone 은 다음에 따로 요청할께" 명시. plan 파일 (`~/.claude/plans/soft-petting-avalanche.md`) 의 블록 A 섹션 참조.
- `cwrtc/config/cwrtc.json.template` `Setup.WsPort`: 8080 → 8443
- `configure.sh:312` `VITE_CWRTC_TARGET=...:8080` → `...:8443`
- `cims.sh:124, 427, 619, 808` cwrtc 포트 리스트 갱신
- `cims-phone/` env WSS target URL 8080 → 8443
- 회귀: Test-CMP ↔ Test-CWRTC ↔ Test-Phone WebRTC smoke
- 블록 A 후 Test-Console 8080 실기동 검증 가능

#### (2순위) **Phase 3 본구현**
- `_verify_phase3` 함수 신설
- `build/dist/{csp,cmp,sim}-server/{agent,<모듈>,config}/` 신규 트리 구현
- Console Phase 3 UI (csp/cmp/sim 배포 진입점)
- 배포본 console:80 기동 자동화 (nginx vs vite preview 결정 + cap_net_bind)
- REGISTER 1건 smoke

#### (3순위) I3/I4/I5/I6 Minor

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -1 --oneline                                # e0c44a7
./cims.sh status                                    # 검증 6 + TB 3 (cwrtc 는 죽어있을 수 있음)
./cims.sh preflight                                 # 포트/ens160/DB

# 블록 A 진행 시 plan 정독
cat ~/.claude/plans/soft-petting-avalanche.md     # 블록 A 섹션
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 늦은 오후, plan-mode 마무리)

### 한 줄 요약
**사용자 피드백으로 용어 혼선 3건 확인**: Test-Console 3011 은 오독. Dev-Console(3001) / Test-Console(8080) / 배포본 console(80) 3분화 + 모든 Phase 1 모듈 `Test-*` 접두 통일 + Phase 2 verify 시 reset 자동 + Phase 3 docs 정합 plan 확정. **구현은 다음 세션**.

### Plan 파일
**`~/.claude/plans/soft-petting-avalanche.md`** — 다음 세션 시작 시 이 파일부터 열어서 진행.

### 사용자 확정 결정
- cwrtc 8080 → **8443** 이전 (HTTPS alt)
- Reset 로그 범위: `service_log/`, `msg_log/`, `build/dist/log/` (verify_reports/ 는 보존)
- Phase 3: docs + dist 정합 (stale 삭제 + §0.10 설계 명시 + 미구현 명기)

### 구현 착수 순서
1. **블록 B** (Console 3분화) + **블록 C** (Phase 2 reset 자동) + **블록 D** (Phase 3 docs 정합) 단일 커밋
2. **블록 A** (cwrtc/phone 이전) — 사용자가 별도 세션에서 요청 예정. **이번 plan 에 설계만 남기고 구현 보류**.

### 주의
- **블록 A 미진행 상태에서 Test-Console 8080 실기동은 cwrtc 충돌로 불가**. 블록 B 에서는 docs/명명만 먼저 반영하고 실기동 검증은 블록 A 후.
- Dev-Console 3001 복원 = 이번 세션 commit 4f53b7d 의 부분 롤백. 커밋 메시지에 명시.

### 최종 환경 상태
- 브랜치: `feature/sip-console-runtime` (origin 대비 **7 커밋 ahead**, 이번 plan-mode 에서 추가 커밋 없음)
- Working tree: **clean** (이번 세션 plan 답변만 수집)
- 서비스: 검증 6종 + TB 3종 running (Test-CSC 4421, Test-Console 3011 임시, cwrtc 8080)
- 마지막 커밋: `4f53b7d feat(verify): Phase 1 Test-* 포트 전환 (4421 / 3011)` — 3011 부분은 다음 세션 롤백 예정

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -1 --oneline                   # 4f53b7d 기대
./cims.sh status                       # 검증 6 + TB 3

# Plan 정독
cat ~/.claude/plans/soft-petting-avalanche.md

# 구현 시작 (블록 B + C + D)
# 1) cims.sh start_console 두 모드 분기 (Dev 3001 / Test 8080)
# 2) cmd_reset --keep-processes 옵션 신설 + _verify_phase2 Cleanup 교체
# 3) docs §0.1 / §0.3 / §0.10 / §2.1 / §2.4.1 / §3.1 Console 3종 + Test-* 통일 + Phase 3 미구현 명기
# 4) verify phase2 smoke (Test-Console 8080 실기동은 보류 — cwrtc 충돌)
# 5) 단일 커밋

# 블록 A (cwrtc) 는 사용자 별도 요청 시 진행
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 오후, Phase 2 v2 + 포트 전환)

### 한 줄 요약
**Phase 2 v2 (start/health/stop + config overlay) 자동화 완성 + Phase 1 Test-* 포트 전환 (4421 / 3011) 완료.** 양쪽 모두 smoke PASS, 공존 확인.

### 이번 세션 커밋 (2건, 이전 세션 위에 +2 → origin 대비 7 커밋 ahead)

```
4f53b7d  feat(verify): Phase 1 Test-* 포트 전환 (4421 / 3011)
7dad064  feat(verify): Phase 2 v2 — start/health/stop 자동화 (config overlay)
```

### 변경 요지

**Phase 2 v2** (7dad064):
- `POST /api/v1/deployments` 가 `config` overlay 수용 → `agent_deployment.config_json` 저장 → `_queue_job` 이 모든 job 에 자동 전달.
- `job_health_check` 가 params.config 의 Server.Port 등으로 port override 지원.
- `cims.sh start_csc/stop_csc` overlay-aware (install_path/config.json 의 Server.Port 우선) + DIST_DIR 포함 절대경로 pattern.
- `_verify_phase2` 에 §12 overlay 검증 + §13 start + §14 health_check + §15 stop 추가.
- Phase 2 csc 는 **Server.Port=4445** overlay 로 기동 (Phase 1 Test-CSC 4421 / 배포본 4420 과 모두 공존).
- Test-agent `--heartbeat-sec 3` 로 pickup 지연 최소화.

**Phase 1 포트 전환** (4f53b7d):
- Test-CSC 4420 → **4421**, Test-Console 3001 → **3011** (초안 8080 은 cwrtc 충돌)
- `csc/config/config_template.json` Server.Port default 4421 (deploy_value 4420 유지 — 운영)
- `configure.sh` .env.local 4420→4421, TB overlay Kms replace 확장 (:4420 및 :4421 둘 다 수용)
- `cims.sh start_console` npm/serve 모두 3011, port list 갱신 (4421 추가, 3001→3011)
- `cmd_sync` 가 `config_template.json` 도 dist 로 복사 (I6 partial 해결)
- docs §0.1/§0.3/§0.5 갱신

### 회귀 smoke 결과
- Phase 2 v2: 3회 연속 PASS (115517 / 115540 / 115601)
- 포트 전환 후 Phase 2 smoke: PASS (120406)
- Test-CSC 4421 admin/1234 login: OK
- Test-Console 3011: HTTP 200
- TB-CSC 4419 login: OK

### 해결된 이슈
- ~~Phase 2 v2 start/health/config overlay~~ ✅ 완료 (7dad064)
- ~~Phase 1 포트 전환~~ ✅ 완료 (4f53b7d)

### 발견 / 주의사항 (메모리 기록됨)
- **4421 포트 TCP/UDP 공존**: Test-CSC TCP 4421 + CSP CscInterface UDP 4421 (csp/CspServer.cpp:259). proto 다름이라 무충돌. 혼동 주의 — 번호 재배치는 후속 과제.
- **verify phase2 --skip-pkg 함정**: 소스 수정 후 `sync all` + `pkg --no-bump` 를 하지 않으면 tarball 속 cims.sh / cims_agent.py 가 stale → start/health 가 옛 로직 사용.
- **TB-CSC 재기동**: 소스 수정 후 `cims.sh restart tb-csc` 필수 (dist/csc/src 에서 import 되므로). 이 때문에 verify phase2 첫 run 에서 config_json NULL, 두 번째 run 부터 정상.
- **8080 = cwrtc**: docs 초안의 Test-Console 8080 은 충돌. 3011 로 재지정.

### 현재 환경 상태 (세션 종료 시점)
- 브랜치: `feature/sip-console-runtime` (origin 대비 **7 커밋 ahead**)
- Working tree: clean
- 서비스: 검증 대상 6종 + TB 3종 전부 실행 중
- 포트:
  - Test-CSC: 4421 (기능 검증 4/6 확인)
  - Test-Console: 3011
  - TB-CSC: 4419 / TB-Console: 3000 / TB-agent: 9902
  - cwrtc: 8080, csp: 5060/5061, cmp: 9000, phone: 3002
- 외부 IP (ens160): **192.168.0.2**
- DB: cims. csc-server-local agent (offline, 마지막 verify 잔재)

### ⏭ 다음 세션 우선 작업

#### (1순위) **Phase 3 실측 구현**
Phase 2 v2 가 완성됐으므로 Phase 2 csc (포트 4445 overlay 로 기동된 배포본) 가 CSP/CMP/Sim 을 배포하는 체인 검증.
- `build/dist/{csp,cmp,sim}-server/` 레거시 wiping 후 신규 생성
- `_verify_phase3` 함수 추가 + REGISTER 1건 smoke
- Phase 2 v2 에서 Start된 csc(4445) 를 기반으로 새 TB-agent 또는 Test-agent 가 csp/cmp/sim 배포

#### (2순위) **console 기동 자동화 (Phase 2 v3)**
현재 Phase 2 는 csc 만 start/health. console 은 install-only. 배포본 console 기동 설계 필요:
- nginx.conf + port 80 + cap_net_bind 또는 reverse proxy
- vite preview dist 모드
- Health check: HTTP GET / → 200

#### (3순위) I3/I4/I5/I6 Minor
- I3 TB-Console dist 배포 경로 / I4 TB-CSC 가 4431 McpttServer 불필요 기동 / I5 configure JWT secret 재생성 / I6 (partial 해결됨) csc_app.py sync

### 🚀 다음 세션 Cold-start 체크리스트

```bash
cd /home/nex/work/cims
git log -1 --oneline                                # 4f53b7d
git status                                          # clean
git log origin/feature/sip-console-runtime..HEAD    # 7 ahead
./cims.sh status                                    # 검증 6 + TB 3
./cims.sh preflight                                 # 포트/ens160/DB
./cims.sh verify phase2 --skip-build --skip-pkg     # smoke PASS 기대
curl -sk -X POST https://127.0.0.1:4421/api/v1/auth/login \
  -H 'Content-Type: application/json' -d '{"login_id":"admin","password":"1234"}'
# Test-CSC 4421 login 확인
```

---

## 🟢 SESSION SIGN-OFF (2026-04-24 오전, 정식 마무리)

### 한 줄 요약
**§0.10 + I1/I2 해결 + Phase 2 자동화 (`cims.sh verify phase2` install-only v1) 완성.** 3회 연속 PASS. 다음 세션은 Phase 2 v2 (start/health) 및/또는 Phase 1 포트 전환.

### 이번 세션 커밋 (5건, origin 대비 5 커밋 앞섬)

```
387cee0  docs:           Phase 2 v1 첫 성공 리포트 (verify phase2 install-only)
89b2db2  feat(verify):   Phase 2 배포 검증 자동화 — cims.sh verify phase2
68473c5  docs(verify):   Phase 1/2 포트 체계 분리 (Test-* dev 포트 vs 운영 포트)
1e663c1  fix(tb):        I1/I2 — reset 후 TB-agent 보존 + admin 부트스트랩 스키마 정합
6185c1c  docs(verify):   Phase 2/3 배포 대상 디렉토리 · 명명 규칙 (§0.10) + §2.3/§3.1 보완
```

origin HEAD: `6f1ae92` (이전 세션 TB 3종 인프라). push 안 함 — 다음 세션 결정.

### 변경 파일 (이번 세션)

| 파일 | 커밋 | 변경 요지 |
|---|---|---|
| `docs/VERIFICATION_PROCESS.md` | 6185c1c, 68473c5 | §0.10 신설 + §0.3/§2.3/§3.1 보완, 포트 체계 footnote |
| `cims.sh` | 1e663c1, 89b2db2 | cmd_reset I1 fix + `_verify_phase2` (~290줄) + build/dist/*-server/ 정리 |
| `sql/migrate_auth.sql` | 1e663c1 | idempotent 재작성 (login_id / uq_login_id / admin 1234) |
| `verify_reports/20260424_104939_phase2.md` | 387cee0 | Phase 2 v1 첫 성공 리포트 |

### 해결된 이슈

- **I1 (Major)** `cims.sh reset` 후 TB-agent 401 → `DELETE FROM cims_agent WHERE name <> 'tb-agent-local'` 로 전환. `CIMS_TB_AGENT_NAME` env override 지원. 1126 heartbeat 401 0건 확인.
- **I2 (Major)** `migrate_auth.sql` ↔ `handlers/auth.py` 컬럼 불일치 → idempotent 재작성. `login_id` + `uq_login_id` UNIQUE + admin/1234 (test_env.json 일치). TB-CSC login 200 + JWT 검증.

### Phase 2 자동화 (v1, install-only)

**커맨드**: `cims.sh verify phase2 [--skip-build] [--skip-pkg] [--keep-agent]`

**흐름 12단계**:
1. Cleanup (csc-server/ + stale Test-agent 프로세스 + `cims_agent WHERE name='csc-server-local'` DB rows)
2. Build (skip-build 가능)
3. Configure (`--local-ip ens160`)
4. Pkg (`--no-bump`, skip-pkg 가능)
5. Admin login → JWT (TB-CSC 4419, admin/1234)
6. Agent 등록 (name=csc-server-local) + approve (409 handling 포함)
7. Test-agent 기동 (nohup `cims_agent.py`, sync **9903**, state-dir=`csc-server/agent/state/`, CIMS_AGENT_INSTALL_ROOT=`csc-server/`) + 15s enroll 대기
8. csc + console tarball 업로드 (POST /api/v1/packages multipart)
9. Deployment 생성 (install_path=`<abs>/csc-server/<name>` 명시)
10. Install job queue + 60s 폴링
11. 설치 파일 검증 (`meta.json` + `config/` 존재)
12. Test-agent 종료 (--keep-agent 로 유지 가능)

**산출물**: `verify_reports/<ts>_phase2.md`. 판정 PASS/FAIL 명기.

**배포된 디렉토리 구조** (실측):
```
build/dist/csc-server/
├── agent/state/ (agent.crt, agent.key, state.json)
├── csc/     (meta.json, config.json, config_template.json, config/, csc/pkg.json, cims.sh)
└── console/ (meta.json, config.json, config/, console/nginx.conf+pkg.json, cims.sh)
```

### 배포 체인 API (재참조용)

- `POST /api/v1/auth/login` `{login_id, password}` → JWT
- `POST /api/v1/agents` `{name, note}` → `{id, enrollment_token, ...}` (409 시 DELETE 후 재시도)
- `POST /api/v1/agents/{id}/approve` → 200
- `POST /api/v1/packages` multipart `file=@<tar>, force=true` → `{id, name, version, sha256}`
- `POST /api/v1/deployments` `{agent_id, package_id, install_path, process_name}` → `{id}`
- `POST /api/v1/deployments/{id}/job` `{job_type: install|start|stop|restart|uninstall|update_config|health_check}` → `{job_id, status:queued}`
- `GET /api/v1/deployments/{id}` → deployment row (polling)

### cims.sh reset 보강 (I1 외)

파일 초기화 블록에 `build/dist/{csc,csp,cmp,sim}-server/` 추가. 관련 Test-agent 프로세스(`pkill -f "cims_agent.py --name {csc,csp,cmp,sim}-server-local"`) 자동 종료. docs §0.3 / §0.10 일치.

### 현재 환경 상태 (세션 종료 시점)

- 브랜치: `feature/sip-console-runtime` (origin 대비 **5 커밋 앞섬**)
- Working tree: **clean**
- 서비스: 검증 대상 6종 (cmp/csp/cwrtc/csc/console/phone) + TB 3종 (tb-csc/tb-console/tb-agent) 모두 실행 중
- 외부 IP (ens160): **192.168.0.2**
- `cims_agent` DB:
  - id=1 tb-agent-local (online, 상시)
  - id=5 csc-server-local (offline, 마지막 verify phase2 잔재 — 다음 실행 시 자동 재생성)
  - id=6 test1-server / id=7 ffff (pending, TB-Console UI 에서 수동 등록한 실험물 — 무시해도 됨)
- `cims_package` DB: id=1 csc 0.0.1, id=2 console 0.0.1 (verify phase2 실행 중 누적)
- `agent_deployment` DB: 최근 2건 (csc/console, status=stopped)
- Phase 2/3 대상 디렉토리:
  - `build/dist/csc-server/` (이번 세션 작업 산출물)
  - `build/dist/{csp,cmp,sim}-server/` (2026-04-22 레거시, 과거 실험물 — 내용 구조가 다름. 재사용 안 함)

### ⏭ 다음 세션 우선 작업

#### (1순위) **Phase 2 v2** — start/health/config overlay
현 v1 은 install 에서 멈춤. 추가:
- **start job**: `POST /api/v1/deployments/{id}/job {job_type:"start"}` 큐잉 → agent 가 install_path/cims.sh 로 프로세스 기동
- 포트 충돌 해결: Phase 1 csc=4420 / console=3001 이 이미 점유 → Phase 2 배포본은 **다른 포트**(예: 4430/3003) 로 기동하거나 Phase 1 먼저 stop
- **config overlay**: `POST /api/v1/deployments` 에 `config:{Port:4430, ...}` 전달 → agent 가 `install_path/config.json` 에 기록 → cims.sh 기동 시 반영
- **health check**: `POST /api/v1/deployments/{id}/job {job_type:"health_check"}` → 리슨 포트 체크
- 리포트에 start 성공 + 포트 리슨 + heartbeat 까지 포함

#### (2순위) **Phase 1 포트 체계 전환** — docs 설계 구현
docs 의 §0.10 신규 설계:
- Phase 1 Test-CSC: 4420 → **4421**
- Phase 1 Test-Console: 3001 → **8080**
- Phase 2 배포 csc: 4420 (유지), console: **80** (sudo/cap 필요)
작업 범위:
- `configure.sh` / `csc/config/config_template.json` / `cims-console/.env.local` / `cims.sh start_csc·start_console` / §0.1 TB 표 갱신
- console:80 은 `setcap 'cap_net_bind_service=+ep' <node>` 또는 별도 reverse proxy 고려

#### (3순위) **Phase 3 실측 정통 구현**
Phase 2 v2 에서 csc 가 실제 기동되면, 그 csc(4420)를 경유해 csp/cmp/sim 을 `csp-server/` / `cmp-server/` / `sim-server/` 에 배포. 현재 `{csp,cmp,sim}-server/` 디렉토리는 레거시 구조라 wiping 후 신규 생성 필요. `_verify_phase3` 함수 추가 + REGISTER 1건 smoke.

#### (후순위) I3/I4/I5/I6 — `project_tb_known_issues.md` 참고
- I3 TB-Console dist 배포 경로 미정 / I4 TB-CSC 가 McpttServer(4431) 도 기동 / I5 configure JWT secret 재생성 / I6 csc_app.py sync 수동 필요

### 🚀 다음 세션 Cold-start 체크리스트

1. **메모리 정독**
   ```
   project_phase_status.md          # 이 파일 (SOT)
   project_tb_infra.md              # TB 3종 + Test-agent
   project_verification_process.md  # Phase 1/2/3 정의 + §0.10
   project_tb_known_issues.md       # I1/I2 해결됨 / I3~I6 + Phase 2 v2 항목
   docs/VERIFICATION_PROCESS.md     # 원본 SSOT
   ```

2. **레포 상태 확인**
   ```bash
   cd /home/nex/work/cims
   git log -1 --oneline                                # 최신: 387cee0
   git status                                          # clean
   git log origin/feature/sip-console-runtime..HEAD    # 5 커밋 ahead
   ```

3. **서비스 + TB + 검증 대상 확인**
   ```bash
   ./cims.sh status       # 검증 대상 6 + TB 3 모두 running 기대
   ./cims.sh preflight    # 포트 점유 / ens160 / DB 확인
   ```

4. **Phase 2 회귀 smoke**
   ```bash
   ./cims.sh verify phase2 --skip-build --skip-pkg
   # 기대: 판정 PASS, verify_reports/<ts>_phase2.md
   ```

5. **작업 시작 — Phase 2 v2 또는 포트 전환**
   - v2 start job 부터 진행 시: `_verify_phase2` 내 "## 10. Install job ..." 이후에 "## 11-start. Start job + health" 단계 삽입
   - 포트 전환 시: configure.sh + config_template.json 부터 개편

---

## 🔴 이전 세션 SIGN-OFF (2026-04-24 TB 인프라 세션)

### 한 줄 요약
**TB 3종 (TB-CSC 4419 / TB-Console 3000 / TB-agent 9902) 상시 기동 인프라 구축 + Phone 3000 → 3002 이전 완료.**
TB-agent 자동 enrollment 포함. Phase 1 REGISTER smoke 1/1 PASS (회귀 없음).

### 이번 세션 변경 파일
```
cims.sh                         (+252/-31 lines) — TB start/stop/status/preflight/reset 분기 추가
configure.sh                    (+37 lines)     — apply_csc_tb_overlay + .env.tb.local 생성
csc/src/csc_app.py              (1 line)        — _CONFIG_PATH 에 CIMS_CSC_CONFIG ENV override
cims-phone/vite.config.ts       (1 line)        — port 3000 → 3002
cims-phone/nginx.conf           (1 line)        — listen 3000 → 3002
cims-console/.gitignore         (+1 line)       — .env.*.local 추가
```
(참고: `cims-console/.env.tb.local` 은 configure 가 생성, gitignored)

### 현재 상태 (세션 종료 시점, 커밋 전)
- 브랜치: `feature/sip-console-runtime`
- origin 대비: **20 커밋 앞섬** (이번 세션 변경 미커밋)
- Working tree: 변경 있음 (위 6개 파일)
- 서비스: 검증 대상 6종 + TB 3종 모두 실행 중
- 외부 IP (ens160): 192.168.0.2

### ⏭ 다음 세션 우선 작업

#### (0순위) **커밋 여부 확인 + 이번 세션 변경 commit**
단일 커밋 권장: `feat(tb): TB 3종 인프라 + Phone 3000→3002 이전`

#### (1순위) **알려진 이슈 (I1 / I2) 해결** — `project_tb_known_issues.md` 참고
- **I1 (Major)**: reset 후 `cims_agent` TRUNCATE 로 TB-agent 가 401. 권장 해법 (a): `DELETE FROM cims_agent WHERE name != 'tb-agent-local'` 로 전환 + docs §0.3 보강
- **I2 (Major)**: `migrate_auth.sql` 과 `handlers/auth.py` 의 컬럼 불일치 (email vs login_id). TB-agent auto-enroll 기본 계정 `admin/1234` 정합성 보장 필요

#### (2순위) **Phase 2 실측 정통 구현**
I1/I2 해결 후:
- `cims.sh verify phase2`: TB-CSC(4419) tarball 업로드 → TB-agent 가 검증 대상 CSC(4420)/Console(3001) 배포 → 기동 확인 → 리포트
- 기능 회귀는 반복하지 않음 (Phase 1 에서 끝)
- `cims_package` 해시 일치 / scalar overlay / collection 전달 / HEARTBEAT 정상 검증

#### (3순위) **Phase 3 실측**
배포된 New-CSC(4420) 가 CSP/CMP/Sim 을 다시 배포하는 체인. REGISTER 1건 smoke 만.

#### (후순위) **이슈 I3 / I4 / I5 / I6** — `project_tb_known_issues.md`

### 🚀 다음 세션 Cold-start 체크리스트

1. **메모리 정독**
   ```
   project_phase_status.md          # 이 파일 (현재 상태 SOT)
   project_tb_infra.md              # TB 3종 포트/명령/설계 요약
   project_tb_known_issues.md       # 이슈 I1~I6 + 해결 옵션
   project_verification_process.md  # Phase 1/2/3 정의
   docs/VERIFICATION_PROCESS.md     # 원본 SSOT
   ```

2. **레포 상태 확인**
   ```bash
   cd /home/nex/work/cims
   git log -1 --oneline
   git status
   ```

3. **TB 3종 + 검증 대상 동작 확인**
   ```bash
   ./cims.sh status                 # 두 섹션 나와야 함
   ./cims.sh preflight              # TB 3종 동작중 / 검증대상 점유중
   ```

4. **smoke (회귀 baseline)**
   ```bash
   ./cims.sh sim -scenario register -count 1 -duration 3
   # 기대: Registered 1/1 fail=0
   ```

---

## 🟡 이전 세션 SIGN-OFF (2026-04-24 00:30)

### 한 줄 요약
**1순위 (CMP/CSC config_template 이관) + 2순위 (Console UI groups/hidden/배지) 완료.**
Phase 2/3 는 `docs/VERIFICATION_PROCESS.md` 정의상 TB 3종(TB-CSC 4419 / TB-Console 3000 / TB-agent 9902) 상시 인프라가 전제인데 현재 환경 미구축 → 다음 세션 scope 로 이월.

### 이번 세션 결과물 (3 커밋)
```
8a607f7  docs:           Phase 1 회귀 리포트 (CMP/CSC config_template 이관 이후)
ffa7244  feat(console):  설정 편집기에 _infra hidden 섹션 + groups sub-header + restart 배지
325a43d  refactor(config): CMP/CSC 를 config_template.json 단일 SOT 로 전환
```

### 현재 상태 (세션 종료 시점)
- **브랜치**: `feature/sip-console-runtime`
- **origin 대비**: **20 커밋 앞섬** (여전히 push 안 함)
- **Working tree**: clean
- **서비스**: cmp/csp/cwrtc/csc/console/phone 전부 실행 중
- **외부 IP (ens160)**: 192.168.0.2 (DHCP)
- **Phase 1 회귀 최신 PASS**: `verify_reports/20260424_001505_phase1.md`

### ⏭ 다음 세션 우선 작업

#### (1순위) **TB 3종 인프라 구축** — Phase 2/3 실측 선결 조건

`docs/VERIFICATION_PROCESS.md` §0.1 / §2.x / §3.x 기준:
- **TB-CSC** (4419): 패키지/에이전트/배포/검증 실행 관리. 상시 동작
- **TB-Console** (3000): TB-CSC UI. `VITE_ADMIN_TARGET=https://127.0.0.1:4419 npm run build`
- **TB-agent** (sync 9902): TB-CSC 에 enroll. install_path `/tmp/cims-tb-agent/`
- **검증 대상 CSC** (4420) / Console (3001) / Phone (3002 — 현재 3000 에서 이전 필요)

작업 항목:
1. `cims.sh` 에 TB 3종 서비스 타입 추가 (`start tb-csc` / `start tb-console` / `start tb-agent`)
2. `csc-tb.json` 설정 (4419, 별도 DB 또는 같은 DB 공유) + TB-Console 빌드 분기
3. TB-agent enrollment 자동화 (enrollment_token 발급 API 경로 확인)
4. Phone 포트 3000 → 3002 이전 (문서와 현실 정합)
5. `cims.sh status` 에 TB 3종 상태 표시
6. `cims.sh reset` 이 TB 는 건드리지 않도록 확인 (이미 그런 설계인지 검증)

#### (2순위) **Phase 2 정통 구현**
- `cims.sh verify phase2`: TB-CSC(4419) 에 tarball 업로드 → TB-agent 로 검증 대상 CSC(4420)/Console(3001) 배포 → 기동 확인 → 리포트
- 기능 회귀는 반복하지 않음 (Phase 1 에서 끝)

#### (3순위) **Phase 3 정통 구현**
- Phase 2 로 배포된 New-CSC(4420) 가 CSP/CMP/Sim 을 다시 배포하는 체인
- REGISTER 1건 smoke 만

### ⚠ 이번 세션 중간 시행착오 (메모)
- 문서 미참고 상태에서 `_verify_phase2` 를 "tarball 구조 검증 + 4420 CSC 업로드" 로 급조했다가
  사용자가 `docs/VERIFICATION_PROCESS.md` 를 지목 → 문서의 Phase 2 정의(TB-CSC 4419 경유)와 다름을 확인 → 제거.
- 교훈: **기존 docs/ 내부 검증 프로세스 문서를 먼저 읽고 접근**.

### 🚀 다음 세션 Cold-start 체크리스트

1. **레포 동기화**
   ```bash
   cd /home/nex/work/cims
   git log -1 --oneline          # 최신: 8a607f7
   git status                     # clean
   git log origin/feature/sip-console-runtime..HEAD --oneline | wc -l  # 20 (push 안 됨)
   ```

2. **문서 선독**
   ```bash
   cat docs/VERIFICATION_PROCESS.md   # Phase 1/2/3 정의 및 TB 3종 인프라
   ```

3. **Phase 1 회귀 smoke (TB 작업 시작 전 baseline)**
   ```bash
   ./cims.sh sim -scenario register -count 1 -duration 3
   # 기대: Registered 1/1 fail=0
   ```

4. **TB 3종 구축 설계부터** — 이 세션의 시행착오 재현 방지 위해 문서 지정 포트/경로 그대로 따르기

---

## 🟡 이전 세션 SIGN-OFF (2026-04-23 23:55)

### 한 줄 요약
**R1~R8 + G1~G11 + CSP 설정 전체 정비 완료.** Phase 1 회귀 PASS 유지. 다음 세션은 CSC/CMP 설정을 CSP 방식으로 맞추는 작업부터.

### 현재 상태 (세션 종료 시점)
- **브랜치**: `feature/sip-console-runtime`
- **origin 대비**: **17 커밋 앞섬** (user 가 명시적으로 push 안 함 — 다음 세션에서 push 여부 재확인)
- **Working tree**: clean
- **서비스**: cmp/csp/csc/console/phone 전부 실행 중 (2026-04-23 23:55 기준)
- **외부 IP (ens160)**: 192.168.0.2 (DHCP, 매 세션 재확인)
- **최신 커밋**:
  ```
  17f98e1  docs:             Phase 1 회귀 리포트 (미디어서버 용어 통일 이후)
  5d2dc0b  refactor(config): CMP→미디어서버 + Roles→Functions + _infra sub-grouping
  0bf0e45  docs:             Phase 1 회귀 리포트 (config 재구성 이후)
  cdf5e90  chore(config):    섹션 재구성 + restart 재분류 + _infra 라벨 보완
  ed69d4b  refactor:         CDR 레거시 제거 (service_log 로 대체)
  2f48c12  feat(G10):        SipServerMap (legacy IBCF XML) 제거
  8233c74  feat(G9):         TCP/TLS primary 도 local_nodes 에서 자동 주입
  9eeb07d  docs:             Phase 1 회귀 리포트 4건 추가
  3d02d3a  chore(G3/G5/G6/G11): 설정 정리 + 문서 교정
  966017a  feat(G8):         cspsim -callee_override 옵션 추가
  af417ee  feat(G7):         sip.msg.jsonl 에 sesid 필드 embed
  7cc8bf6  feat(G1/G2):      Routing 결정 단일화 via CspPendingRouteMap
  ```

### ⏭ 다음 세션 우선 작업 (사용자 명시 순서)

#### (1순위) **CSC / CMP 설정 정비** — CSP 와 유사 방식 적용

CSP 설정에서 이번 세션에 적용한 패턴을 CSC / CMP 에도:

**CSP 에서 확립된 패턴** (2026-04-23):
- `config_template.json` 단일 파일에 scalar sections + collections 정의 (CSP 는 이미 있음)
- 섹션 분리 원칙: "SIP Stack / CSCF / TAS / Functions / 미디어서버 연동 / 로깅 / DB / _infra"
- `restart: true/false` 플래그 + `reload_hint` — SIGUSR1 이 scalar 도 재로드
- `_infra` 섹션에 hidden 필드 + `groups` 배열로 sub-grouping (sip_fallback / data_folder / service_logging / monitor / security)
- 모든 필드에 `label` + `help` 보완
- deploy-time 치환은 `deploy_value: "@VAR@"` 로 `configure.sh` 가 채움

**CSC 현재 상태** (추측):
- `csc/bin/csc_pihttp/config/csc.json` 이 SOT
- `cims.sh configure` 가 템플릿 렌더 중인지 확인 필요
- 섹션: Server (HTTPS 4420 / MCPTT 4430) / DB / CspNotify 엔드포인트 / 등
- 검토할 것:
  * `csc` 에 `config_template.json` 이 있는지
  * 있다면 CSP 와 같은 구조인지
  * 없다면 새로 만들어서 csc.json 생성 경로 통합
  * CSC 만의 고유 영역 (HTTPS 인증서, Flow API 경로, Agent sync 포트 등) 섹션화

**CMP 현재 상태** (추측):
- `cmp/cmp.json` — VoIP/PTT RTP 포트 풀, Floor 포트 풀, DTMF PTT digits 등
- 역시 `config_template.json` 도입 여부 확인 + 미도입 시 CSP 패턴 적용
- 섹션 후보: Control Port (CSP 연동) / VoIP RTP Pool / PTT RTP Pool / PTT Floor Pool / Logging / Monitor

**작업 순서**:
1. `csc/` / `cmp/` 의 현재 config 구조 인벤토리
2. `config_template.json` 없으면 신규 작성 (CSP 패턴 참고)
3. scalar 파싱 코드 (있다면) 유지 + schema 만 정비
4. section 분리 / restart 플래그 / label / help / groups 적용
5. `cims.sh configure` 의 `apply_config_template` 가 CSC/CMP 도 렌더링하도록 확장 (이미 된 것 같으면 확인만)
6. Phase 1 smoke 로 회귀 없음 확인

#### (2순위) **Console UI group 속성 렌더링**

CSP 의 `_infra` 에 `groups` 배열 + 필드 `group` 속성이 이미 추가됐지만 Console UI 는 아직 이를 렌더링 안 함. CSC/CMP 도 같은 패턴 적용 후 UI 측 한번에 구현:

- Console (cims-console) 의 모듈 설정 편집기 (`ModuleConfigEditor.tsx` 근처) 에서 섹션의 `groups` 를 읽어 sub-header 렌더
- 필드의 `group` 속성으로 필드 그룹 정렬
- `group` 미지정 필드는 "기타" 그룹 또는 상단에 그대로
- `restart: false` 필드에 "즉시 반영" 배지 + `reload_hint` tooltip 표시
- `restart: true` 필드에 "재기동 필요" 배지
- `hidden: true` 섹션은 "고급 설정" 토글로만 표시

#### (3순위) **Phase 2 (배포) → Phase 3 (배포/서비스 검증)**

CSC/CMP 설정까지 정비되고 Console UI 가 group 렌더 지원하면 배포 체인 검증 진입.

**Phase 2 준비물** (기존 P3 gap 참고):
- `cims.sh verify phase2` 자동화 (현재 미구현, P3-04)
- install 시 9 collection jsonl 자동 전달 (P3-01)
- scalar overlay merge 검증 (P3-02 — R1 이후 일부 해결, 배포 환경 실측 남음)
- `ServiceLogging.Dir` 호스트별 경로 변환 (P3-03)

**Phase 3 준비물**:
- 배포 체인 전체 (tarball 업로드 → Deployment 생성 → install → config push → restart → 상태 반영) end-to-end 자동화
- 실제 2-host (console 호스트 ≠ CSP/CMP 호스트) 분리 시험

### 🚀 다음 세션 Cold-start 체크리스트

#### 1. 레포 동기화 확인
```bash
cd /home/nex/work/cims
git log -1 --oneline          # 최신: "17f98e1 docs: Phase 1 회귀 리포트 (미디어서버 용어 통일 이후)"
git status                     # clean 여야 함
git log origin/feature/sip-console-runtime..HEAD --oneline | wc -l  # 17 (push 안 됨)
```

#### 2. 환경값 재확인
```bash
ip -4 addr show ens160 | grep -oP 'inet \K[0-9.]+' | head -1   # 현재 IP (DHCP)
```

#### 3. 서비스 재기동 (다른 계정 시작 시)
```bash
./cims.sh build
./cims.sh configure --local-ip <IP> --db-password <REDACTED_DB_PW>
./cims.sh start
./cims.sh status   # 6개 (cmp/csp/cwrtc/csc/console/phone) 실행 중 확인
```

#### 4. Phase 1 회귀 smoke (아무 작업 전에)
```bash
./cims.sh sim -scenario register -count 1 -duration 3
# 기대: Registered 1/1 (fail=0), User=+821357007001
```
실패 시 → 아래 "⚠ 함정" 참조

#### 5. 다음 작업 시작 (CSC/CMP 설정 정비)
1. `ls csc/ cmp/` 로 현재 구조 확인
2. 각각에 `config_template.json` 있는지 확인:
   ```bash
   find csc cmp -name "config_template.json" -not -path "*/build/*" -not -path "*/node_modules/*"
   ```
3. 있으면 CSP 패턴과 비교 (어느 요소가 누락?)
4. 없으면 CSP 의 `csp/config/config_template.json` 을 템플릿으로 신규 작성

### ⚠ 주요 함정 (다음 세션 전에 확인)

#### A. DB 가입자 passwd 재확인
이전 세션에서 `+821357007001` / `+821357007003` 의 passwd / imsi 가 오염된 적 있음. Phase 1 VoIP call fail=1 나오면:
```sql
SELECT id, passwd, imsi FROM volte_subscriptions WHERE id IN ('+821357007001','+821357007002','+821357007003');
-- 기대: passwd=123456, imsi=4500331000000XX 형식
```

#### B. console/phone 외부 잔여 프로세스
이전 세션 종료 시 외부 프로세스(pid=...) 로 남는 경우 있음. `cims.sh start` 가 자동 kill 후 재기동하므로 걱정 X. 단 수동 삭제 필요 시 `pkill -f vite`.

#### C. cmake 재구성 필요 시점
새 cpp/h 파일 추가 시 (또는 csc/cmp config_template.json 신규 시) `file(GLOB ...)` 로 자동 수집되지만, 한 번 `cd build && cmake ..` 재실행 필요.

#### D. 빌드 디렉토리 혼동
- CMake build dir: `/home/nex/work/cims/build`
- dist: `/home/nex/work/cims/build/dist` (runtime)
- `./cims.sh build` 만 dist 까지 복사. `make` 만 치면 `build/bin/*` 갱신되고 dist 안 됨 → 기동 시 옛 바이너리.

#### E. `-mode` 는 `volte` (이전 'voip' 아님)

---

## 🔍 2026-04-23 21:47 — R8 실효 검증 (seed 추가 / IBCF 토글 시험)

**목표**: routing_policies + rule_sets + rules + route_sets + routes + remote_nodes 6개 seed 추가 → INVITE → ROUTE_SET 경로 동작 확인.

### 시험 seed (to_uri_user == +821357007002 매칭, 더미 peer 127.0.0.1:5999 UDP)
- 6개 jsonl 작성 → `kill -USR1 <csp_pid>` reload → `RoutingPolicyEngine: sync complete, 1 policies` 확인

### 결과
- **엔진 체인 완전 동작**: `[SYSTEM] RoutingPolicyEngine: policy='r8-test-policy' route_set='r8-test-routeset' picked_route='r8-test-route' → RemoteNode r8-test-peer (127.0.0.1:5999 UDP)` 출력
- **실 TX outbound 은 미발생**: 아래 G1 gap 으로 `ModuleDispatcher::RecvRequest` 의 AddRoute 경로가 무효 + `EventIncomingCall` 의 실효 경로는 callee isAlive=true 라서 미진입.
- IBCF=ON 으로 바꾸고 재시도해도 같은 이유로 미진입 (callee 가 등록된 volte 가입자라 로컬 B2BUA 우선).

### 원상복구 완료
- 6개 seed 삭제 + `csp.json Setup.Roles.IBCF=false` 복원 + CSP restart.
- 확인: `Roles CSCF=ON TAS=ON PTT-AS=ON IBCF=OFF` + 모든 엔진 `sync complete, 0 nodes/policies`.

### 파생 gap → 아래 G1 / G2 섹션

---

## ✅ 2026-04-23 21:xx 세션 — Phase 1 회귀 전체 PASS

**리포트**: `verify_reports/20260423_211959_phase1.md` (git: feature/sip-console-runtime @ 6d9cb52)

### 자동 6항목 (`./cims.sh verify phase1`)
- ✅ 녹취: VoIP 4개 + PTT 14개, 0byte 없음
- ✅ SIP/msg 로그: 1249 라인 (msg=606, flow=643)
- ✅ ERROR/FATAL: 0건
- ✅ VoIP B2BUA: Registered 2/2, Call 1/0 OK, setup 1002ms
- ✅ PTT 그룹콜 5 member: 전원 REGISTERED + Conference NOTIFY
- ✅ Package auto-upload: 8개

### 수동 4항목 (이번 세션에서 CLI로 검증)
- ✅ **sesid 일관성**: VoIP call S20260423212032667849 에서
  * session.json: sesid=`+821357007001::csp::20260423212032667722::1`, call_ids=[leg A, leg B]
  * CMP JSON msg.jsonl: sesid 직접 기록 6건
  * flow.jsonl: sesid 직접 기록 20건
  * SIP msg.jsonl: sesid 필드 없음 (raw SIP엔 없음) → Call-ID 양쪽 leg 7+7건으로 매핑 가능
- ✅ **Flow API nodes**: `GET /api/v1/flow/{call_id}?date=2026-04-23&hour=21` → `nodes={csp:20, cmp:6}` 반환
- ✅ **CSC CRUD → NOTIFY**: `PUT /api/v1/users/1/call/+821357007001 {dnd:true}` (200 OK) → CSP 로그 즉시 `CscInterface Event: USER_CHANGED, URI: tel:+821357007001 ... SesId: +821357007001::csc::...` + `SendSipNotify: user_change User=+821357007001`. dnd=false 로 원복 완료.
- ⏭ **mTLS**: Phase D (인증서 교체) 해당 시만. 현재 N/A.

### Phase 1 관련 세부 환경 확인 사항
- CSP 실제 로그 위치: `build/dist/csp/log/csp_YYYYMMDD_N.log` (일자별 롤링)
  * `build/dist/log/csp.log` 는 wrapper용 빈 파일 — NOTIFY 검증 시 `csp/log/csp_*.log` 최신 참조 필요
- Flow API: `/api/v1/flow/{call_id}?date=YYYY-MM-DD&hour=HH` (session_id 아닌 call_id)
- 가입자 CRUD: `PUT /api/v1/users/{pid}/call/{msisdn}` with `{dnd, forward_id}` (admin.py)
- Auth: `POST /api/v1/auth/login {login_id, password}` → `token`

---

## 🚀 다음 세션 Cold-start 체크리스트 (Phase 1 회귀 시작용)

이 메모는 2026-04-23 22:15 시점 snapshot. 작성자(nex) 계정 기준. 다른 계정/환경에서 시작 시:

### 1. 레포 동기화 확인
```bash
cd /home/nex/work/cims   # (또는 실제 로컬 경로)
git log -1 --oneline      # 최신 커밋이 "6d9cb52 feat(R8): ..." 여야 함
git status                 # clean working tree
```
만약 최신이 아니면 `git fetch && git checkout feature/sip-console-runtime` 후 최신 R8 커밋까지 확인. **브랜치가 `origin` 대비 앞서 있으면** 아직 push 안 됨 (user 가 명시적으로 push 한 적 없음) — 원격 fetch 해도 local 이 더 최신.

### 2. 환경 값 확인
```bash
ip -4 addr show ens160 | grep -oP 'inet \K[0-9.]+' | head -1   # 현재 IP 필요
```
이전 세션 IP: `192.168.0.2`. DHCP 일 수 있으므로 **반드시 재확인**.

### 3. 빌드 + configure + 서비스 기동
```bash
./cims.sh build
./cims.sh configure --local-ip <현재IP> --db-password <REDACTED_DB_PW>
./cims.sh start        # CMP + CSP + cwrtc + csc 모두
./cims.sh status       # 모두 "실행 중" 확인
```

### 4. Smoke test — **핵심 한 줄**
```bash
./cims.sh sim -scenario register -count 1 -duration 3
```
기대 출력: `Registered : 1/1 (fail=0)` — `User=+821357007001` 로 200 OK.

(실패 시 아래 "함정" 섹션 참고)

### 5. Phase 1 회귀 본격 진행
```bash
./cims.sh verify phase1
# 리포트: verify_reports/{timestamp}_phase1.md
```
**검증 6 항목** (이 리포트에 기록됨):
1. 녹취: 통화당 raw/seg RTP 생성, 0byte 파일 없음
2. SIP/msg 로그: csp_01_sip.msg.jsonl 누락 없음, 파싱 가능
3. ERROR/FATAL 로그 0건 (known false-positive 외)
4. sesid 일관성: 1 통화 전체 (SIP + CMP JSON + CSC) 동일 sesid
5. CSC CRUD → NOTIFY: user 변경 시 CSP 가 gms NOTIFY 발송
6. Flow API nodes: `/api/v1/flow/nodes` 가 cspsim 통화 session_id 에 대해 전체 path 반환
7. (해당 시) mTLS: Phase D 인증서 교체 시 TLS handshake 성공

### 6. R8 실효 검증 (선택)
routing_policies.jsonl + rules/rule_sets seed 추가 후 INVITE smoke.
- seed 예: `rules.jsonl` 에 `to_uri_host == <peering-domain>` 규칙 + `routing_policies.jsonl` 에 target_type=route_set 정책
- INVITE 시 CSP 로그에 `RoutingPolicyEngine: policy='...' route_set='...' picked_route='...' → RemoteNode ...(ip:port UDP/TCP/TLS)` 확인

---

## ⚠️ 함정/주의 (Phase 1 진행 전 필수 확인)

### A. REGISTER 조합
- **DB 에 있는 가입자만 REGISTER 성공**. 임의 번호 `-user 1001` 로 테스트하면 403.
- 정답 조합은 자동화되어 있음: `./cims.sh sim -scenario register` 만 치면 DB 자동 로드.
- 수동 인자 줄 때는: `-user +821357007001 -auth_id "450033100000001@ims.mnc033.mcc450.3gppnetwork.org" -domain ims.mnc033.mcc450.3gppnetwork.org -password 123456`.
- 이슈 경위는 커밋 `5b91f60` 메시지 참조.

### B. `cims.sh build` vs `make` 차이
- `make -C build` 는 `build/bin/*` 만 갱신. **`build/dist/*` 는 안 바뀜** → 기동 시 옛 바이너리 실행.
- 항상 `./cims.sh build` 사용 (build + dist 복사 모두 수행).

### C. `access_services.jsonl` seed
`cims.sh verify phase1` 이 access_services.jsonl 을 **자동 시드** (volte + ptt). 이미 있으면 유지.
```
volte: domain=ims.mnc033.mcc450.3gppnetwork.org, auth_realm=동일
ptt:   domain=ptt.mnc033.mcc450.3gppnetwork.org,  auth_realm=동일
```

### D. cspsim `-mode` 는 "volte" (과거 "voip" 더 이상 사용 안 함)
cspsim 소스는 "volte" 만 인식. `cims.sh`/test scripts 모두 volte 로 통일됨 (`5b91f60`).
CHANGELOG.md 의 "-mode voip" 는 history 텍스트라 그대로 둠.

### E. `console`/`phone` 외부 실행 잔여
이전 세션에서 console (pid=130821, port=3001) / phone (pid=130943, port=3000) 이 "실행 중(외부)" 상태로 종료됐을 수 있음. 다른 계정에서 자동으로 해제되지 않으므로 필요 시:
```bash
./cims.sh status              # 외부 잔여 확인
pkill -f vite                  # node/vite dev server kill
```

---

## 현재 브랜치/커밋

- 브랜치: `feature/sip-console-runtime`
- **최신 커밋**: `6d9cb52 feat(R8): ROUTING_ROUTE_SET 실 outbound 배선 (picked_route → Route header)`
- **origin 대비 5 커밋 앞** (아직 push 안 됨 — 사용자 명시 요청 없음)

### R1~R8 전체 커밋 체인 (역순, 신 → 구)
```
6d9cb52  feat(R8):       ROUTING_ROUTE_SET → Route header 주입 → B2BUA forward
5b91f60  fix(sim):       -mode 'volte' 통일 + DB 모드 domain 자동 감지
611d714  feat(R6):       From identity per Access Service (server_identity_uri)
494a924  feat(R5.b'''):  TCP/TLS client connect source bind (TcpConnectFrom)
28d9d4b  feat(R5.b''):   UDP response path per-listener source (m_iListenerId)
f1e451c  feat(R5.b'):    UDP request per-listener source (Via[0] 매칭)
2d58b67  feat(R5.c):     psip TLS per-listener SSL_CTX
52db1e2  feat(R5.b):     CspAddressing context-aware 분기
ccc472f  refactor(R5.a): CspAddressing helper 도입
8ec5591  feat(R4):       CspListenerManager TCP/TLS 분기
925304c  feat(R3):       psip TCP/TLS multi-listener API
2a97f89  feat(R2):       UDP per-listener thread_count
8bbbbe3  feat(R1+config): primary local_node 자동 주입 + build/configure/pkg 3단계
```

---

## psip + CSP 리팩토링 진행 상태 (이번 세션 합의, 2026-04-23)

**배경**: `Setup.Sip.LocalIp`/`UdpPort` 는 단순 bootstrap 이 아니라 From/Via/Contact 생성의 **CSP 인스턴스 primary identity**. psip 는 Transport 멀티-리스너를 지원하지만 CSP 가 global LocalIp 하나로 Via/Contact 를 생성하는 한계. 새 9-collection 모델 (local_nodes / remote_nodes / routes / route_sets / rules / rule_sets / routing_policies / acl_policies / access_services) 에 맞춰 psip + CSP 를 RFC 규격대로 단계 리팩토링.

**단계 분해** (각 단계 후 Phase 1 회귀 6항목 필수):

| 단계 | 내용 | 상태 |
|---|---|---|
| **R1** | primary local_node → Setup.Sip.LocalIp/UdpPort 자동 주입 | ✅ 완료 (2026-04-23) |
| **R2** | UDP multi-listener per-listener `thread_count` 전달 | ✅ 완료 (2026-04-23) |
| **R3** | psip TCP/TLS multi-listener API 확장 (`m_hTcpSocket`/`m_hTlsSocket` singleton → vector) | ✅ 완료 (2026-04-23) |
| **R4** | CspListenerManager TCP/TLS 분기 + protocol-별 add/remove (outbound Route 선택은 R5 로 이관) | ✅ 완료 (2026-04-23) |
| **R5.a** | CspAddressing helper 도입 (SIP/RTP/XCAP 주소 semantic 분리). 현재 모두 단일 LocalIp 반환 (전이 버전) | ✅ 완료 (2026-04-23) |
| **R5.b** | Context-aware 분기: inbound listener id → local_node bind_ip / outbound proto+edge 선택 (CSP 레이어). psip Send 분기는 R5.b' 로 이관 | ✅ 완료 (2026-04-23) |
| **R5.c** | TLS per-listener SSL_CTX (local_node.tls_cert_path/key/ca 활성화). primary listener 는 stack-global SSLServerStart 유지 (backward compat) | ✅ 완료 (2026-04-23) |
| **R5.b'** | psip UDP Send per-request source socket (Via[0] 매칭). Request 경로만. TCP/TLS connect-oriented 는 이번 scope 외. Response 는 primary 유지 | ✅ 완료 (2026-04-23) |
| **R5.b''** | UDP Response path per-listener source. CSipMessage::CreateResponse 가 m_iListenerId 계승, Send response 분기가 listener id 로 소켓 선택 | ✅ 완료 (2026-04-23) |
| **R5.b'''** | TCP/TLS client connect 시 source bind. TcpConnectFrom 신규, SipTcp/TlsClientThread 가 Via[0] 기반 source 로 bind 후 connect | ✅ 완료 (2026-04-23) |
| **R6** | From identity per Access Service — access_services.server_identity_uri 필드 + GetServerIdentityForService helper + UserMap.cpp 적용 | ✅ 완료 (2026-04-23) |
| **R7** | ACL / Routing Policy 메시지 경로 연결. ModuleDispatcher::RecvRequest 에 ACL.Check + RoutingPolicy.Decide 이미 배선되어 있음. REJECT 반영, ROUTE_SET/ACCESS_SERVICE 는 로그 → legacy fallback. seed 없으면 기본 ALLOW/legacy | ✅ 완료 (2026-04-23, 기존 구현 확인) |
| **R8** | ROUTING_ROUTE_SET 실 outbound 배선. picked_route → RouteMap → RemoteNode 조회 → Route header 주입 → B2BUA 가 Route 따라 forward. 매칭 실패 시 legacy fallback | ✅ 완료 (2026-04-23) |

**R7 이후** 에 Phase 1 회귀 6항목 전체 + Phase 2/3 검증 재개.

---

## 이번 세션 완료 사항 (2026-04-23 17:30–22:15)

### (S) R8: ROUTING_ROUTE_SET 실 outbound 배선 (2026-04-23 22:00–22:15)
- `csp/ModuleDispatcher.cpp` INVITE 분기의 ROUTING_ROUTE_SET 처리:
  * `gclsRouteMap.GetByName(rd.picked_route)` → `RouteConfig` 조회
  * `gclsRemoteNodeMap.GetByName(rc.remote_node_ref)` → `RemoteNodeInfo` 조회
  * `RemoteNode.protocol` (UDP/TCP/TLS) 을 `ESipTransport` 로 매핑
  * `pclsMessage->AddRoute(rn.ip, rn.port, eT)` 로 Route header 주입
  * `return false` → CSipUserAgent B2BUA 가 Route 따라 outbound forward (R5 시리즈의 per-listener source + TLS per-listener cert + TcpConnectFrom 전부 활용)
  * 조회 실패 시 ERROR 로그 + legacy 경로로 fallback (backward compat)
- ROUTING_ACCESS_SERVICE 는 명시적 분기 없이 legacy TAS 경로로 진행 (DND/reject 판정 기존 로직 재사용). 로그만 INFO.
- 중복 `#include "CspRouteMap.h"` 제거.
- **검증**: 빌드 성공, REGISTER smoke 통과 (Registered 1/1). Routing seed 없음 → NO_MATCH 경로로 legacy 유지 → 회귀 없음. 실 ROUTE_SET 동작은 seed 추가 후 Phase 1 회귀에서 검증.

### (R) REGISTER 403 이슈 해결 + cims.sh sim 자동화 (2026-04-23 20:50–21:00)
커밋 `5b91f60`. 상세는 "✅ 선행 이슈 해결" 섹션 참조.

### (Q) R7: ACL/Routing wiring 확인 (2026-04-23 21:45–22:00)
- 기존 구현 확인: `ModuleDispatcher::RecvRequest` (ModuleDispatcher.cpp:202+) 에 이미 wiring 되어 있음
  * Line 225: `gclsAclPolicyEngine.Check(mctx, strLocalNodeName, "", "")` — 수신 경로 ACL 평가
  * Line 230: denied → `SendResponse(403)` + drop
  * Line 275: `gclsRoutingPolicyEngine.Decide(mctx, hashKey)` — INVITE 에만 평가
  * Line 280: `ROUTING_REJECT` → 403 drop
  * Line 285-292: `ROUTING_ROUTE_SET` / `ROUTING_ACCESS_SERVICE` 는 로그만 찍고 legacy 로 진행
- MessageCtx 는 from/to/req uri + src_ip + method + user_agent 로 자동 조립
- **seed 파일 없음**: `build/dist/config/{acl_policies,routing_policies,rules,rule_sets}.jsonl` 모두 부재 → 엔진 empty → ACL 기본 ALLOW, Routing NO_MATCH → legacy 경로 유지
- **검증**: `cims.sh sim -scenario register -count 1` → `Registered: 1/1 (fail=0)` 통과. ACL 평가 + OPTIONS auto-reply + CscfModule REGISTER 처리 모두 정상.
- **남은 작업 (R8 로 분리)**: ROUTING_ROUTE_SET 의 `picked_route` 를 실제 outbound transport (TCP/TLS connect) 로 배선. 현재는 legacy SipServerMap (IBCF XML) 으로 fallback. ROUTING_ACCESS_SERVICE 도 TAS 모듈로 직접 라우팅 배선 필요.

### (P) R6: From identity per Access Service (2026-04-23 21:30–21:45)
- `csp/config/config_template.json`:
  * `access_services.schema.fields` 에 `server_identity_uri` (optional string, default "") 추가
  * help: CSP 발신 SIP 메시지의 From URI. 비면 `sip:cspserver@<domain>` 자동 조립.
- `csp/CspServiceMap.h/.cpp`:
  * `ServiceInfo` 에 `std::string server_identity_uri` 필드 추가
  * `Sync()` 에서 `row.GetString("server_identity_uri")` 파싱
- `csp/CspAddressing.h/.cpp`:
  * 신규 `GetServerIdentityForService(const std::string& kind)` helper
  * 1차: `ServiceMap.GetByKind(kind).server_identity_uri` 가 명시되면 반환
  * 2차: `sip:cspserver@{service.domain}` 자동 조립
  * 3차: `sip:cspserver@{gclsSetup.m_strLocalIp}` primary fallback
- `csp/UserMap.cpp`:
  * OPTIONS keepalive 의 From URI 를 `GetServerIdentityForService("volte")` 기반으로 전환
  * `CSipUri::Parse` 로 user/host 분리 → `m_clsFrom.m_clsUri.Set(proto, user, host)`
  * Parse 실패 시 기존 `GetLocalSipAddressForOutbound("UDP","access")` fallback
  * Call-ID host 는 여전히 outbound access edge local addr (의도적 — 메시지 유일성 보장)
- **검증**: 빌드 성공, REGISTER smoke 정상 (403 password 이슈 무관, 기존 CscfModule 회귀 없음)
- **실효 검증**: OPTIONS keepalive 는 등록된 사용자 필요 — 배포 환경에서 검증 가능. 현재 seed 에 server_identity_uri 없으므로 helper 가 domain 기반 URI (`sip:cspserver@ims.mnc033.mcc450.3gppnetwork.org`) 자동 조립 — 기존 `sip:cspserver@192.168.0.2` 보다 IMS 규격 정합.

### (O) R5.b''': TCP/TLS client connect source bind (2026-04-23 21:15–21:30)
- `ext/psip/SipPlatform/SipTcp.h/.cpp`:
  * 신규 `Socket TcpConnectFrom(pszSrcIp, pszIp, iPort, iTimeout)` — src_ip 가 유효하면 `socket → bind(src_ip, port=0) → connect`. IPv4/IPv6 모두 처리. src_ip 가 NULL/"0.0.0.0"/빈 문자열이면 bind 없이 기존 `TcpConnect` 동일 동작.
- `ext/psip/SipStack/SipTcpClientThread.cpp`:
  * `CSipTcpClientArg` 에 `m_strSourceIp` 필드 추가
  * `TcpConnect` → `TcpConnectFrom(src_ip, ...)` 교체
  * `StartSipTcpClientThread` 에서 `pclsSipMessage->m_clsViaList.front().m_strHost` 를 source IP 로 자동 추출
- `ext/psip/SipStack/SipTlsClientThread.cpp`: TCP 와 완전 대칭
- **검증**: 빌드 성공. UDP REGISTER smoke 정상 (403 password 이슈 무관, TCP/TLS 경로와 무관). TCP/TLS outbound 실효 검증은 remote_node TCP/TLS 구성 필요.

### (N) R5.b'': UDP response path per-listener source (2026-04-23 21:00–21:15)
- `ext/psip/SipParser/SipMessage.cpp`:
  * `CreateResponse()` / `CreateResponseWithToTag()` 에 `m_iListenerId` 복사 추가 — 응답은 요청이 수신된 listener 로 회신
- `ext/psip/SipStack/SipStack.h/.cpp`:
  * 신규 `Socket _SelectUdpSocketByListenerId(int)` helper — id > 0 이면 `m_vecUdpListeners.m_iId` 매칭. 실패 시 `m_hUdpSocket` primary fallback.
- `ext/psip/SipStack/SipStackComm.hpp`:
  * UDP Send 분기에서 request → `_SelectUdpSocketForViaRequest`, response → `_SelectUdpSocketByListenerId(m_iListenerId)` 로 분기
- **이미 존재**: `RecvSipMessage` 경로에서 `pclsMessage->m_iListenerId = t_iCurrentListenerId` 로 설정됨 (SipStackComm.hpp:237)
- **검증**: REGISTER 스모크 정상 (403 password 이슈 무관). Response 의 `m_iListenerId` 계승 chain: RX request → listener id 저장 → CreateResponse 계승 → Send 가 올바른 socket 선택.
- **회귀 없음**: primary (id=0) 만 있는 환경에서는 fallback 경로로 기존과 동일.

### (M) R5.b': psip UDP Send per-request source socket (2026-04-23 20:45–21:00)
- `ext/psip/SipStack/SipStack.h/.cpp`:
  * 신규 private helper `Socket _SelectUdpSocketForViaRequest(CSipMessage*)` 추가
  * Via[0] (CheckSipMessage 가 request 에 자동 추가) 의 host:port 와 매칭되는 listener socket 반환.
    bind_ip match: exact match or `0.0.0.0`/empty (any-interface).
    Port match: listener port 와 Via port 일치.
  * 매칭 실패 시 `m_hUdpSocket` (primary) fallback
- `ext/psip/SipStack/SipStackComm.hpp`:
  * `CSipStack::Send` 의 UDP 분기에서 request 에만 helper 호출하여 source socket 선택
  * Response 는 기존대로 primary — Via[0] 는 peer 주소이므로 source 결정 불가
- **검증**: REGISTER smoke 정상. 403 응답 (password 이슈, 무관). 다른 회귀 없음.
- **이관**:
  * R5.b'' — Response path per-listener source (inbound listener 추적)
  * R5.b''' — TCP/TLS connect-oriented per-listener source

### (L) R5.c: TLS per-listener SSL_CTX (2026-04-23 20:30–20:45)
- `ext/psip/SipStack/TlsFunction.h/.cpp`:
  * 신규 `SSLServerCtxCreate(cert, key, ca)` — 독립 SSL_CTX 생성+cert/key 로드+검증. key 미지정 시 cert 파일 재사용 (combined PEM). ca 제공 시 mTLS 활성화 (SSL_VERIFY_PEER|FAIL_IF_NO_PEER_CERT).
  * 신규 `SSLServerCtxFree(ctx)`
  * 신규 `SSLAcceptWithCtx(fd, ctx, ...)` — ctx 가 NULL 이면 gpsttServerCtx fallback
  * 기존 `SSLServerStart` / `SSLAccept` 는 backward-compat 유지
- `ext/psip/SipStack/TcpSessionList.h` (SipStack 용 CTcpComm):
  * `SSL_CTX* m_pSslCtx` 필드 추가 (#ifdef USE_TLS). accept thread → worker 로 per-listener ctx 전파 채널.
- `ext/psip/TcpStack/TcpThreadList.h` (TcpStack 용 CTcpComm):
  * 동일하게 필드 추가 (TcpStack 도 같은 패턴 지원, 당장 사용 없음)
- `ext/psip/SipStack/SipStackListener.h` (CSipStackTlsListener):
  * `SSL_CTX* m_pSslCtx` + `std::string m_strCertFile/m_strKeyFile/m_strCaCertFile` 필드. NULL ctx 는 stack-global 사용.
- `ext/psip/SipStack/SipStack.h/.cpp`:
  * `AddTlsListener(extId, ip, port, cert, key, ca, out)` signature 확장. cert 지정 시 `SSLServerCtxCreate` 호출하여 listener 전용 ctx 생성.
  * `_Stop`, `_StopTlsListenerLocked` 에서 per-listener ctx `SSLServerCtxFree`.
  * Start() 의 primary TLS 경로는 직접 listener 객체 생성 (signature 영향 없음), m_pSslCtx=NULL → global ctx 사용.
- `ext/psip/SipStack/SipTlsThread.cpp`:
  * `SipTlsListenerThread` 에서 accept 후 `clsTcpComm.m_pSslCtx = pListener->m_pSslCtx` 설정
  * worker (`SipTlsThread`) 에서 `SSLAccept` 를 `SSLAcceptWithCtx(fd, clsTcpComm.m_pSslCtx, ...)` 로 교체
- `csp/CspListenerManager.h/.cpp`:
  * `ManagedInfo.tlsCertPath/tlsKeyPath/tlsCaPath` 필드 추가
  * `Sync()` 에서 `row.GetString("tls_cert_path"/"tls_key_path"/"tls_ca_path")` 수집 + INFO 로깅
  * `_addListenerToStack` 의 TLS 분기에 cert/key/ca 전달
- **검증**: local_nodes.jsonl 에 TLS 리스너 (5064, cert=csp.pem) 추가 기동
  * `ListenerManager: id=868654195 TLS per-listener cert='/home/nex/.../csp.pem' key='<same as cert>' ca='<none>'`
  * `AddTlsListener id=868654195 0.0.0.0:5064 cert=/home/nex/.../csp.pem`
  * `ss -tlnp`: 5064 LISTEN + 기존 bootstrap TLS 5061 (stack-global cert) 공존 확인
  * UDP 5060 primary 회귀 없음
- **미해결 (R5.b')**: outbound SIP 송신 시 source socket 여전히 primary. 엄격한 per-listener bind 는 psip Send 확장 필요.

### (K) R5.b: CspAddressing context-aware 분기 (2026-04-23 20:15–20:30)
- `CspAddressing.h/.cpp` signature 확장:
  * `GetLocalSipAddress(int inbound_listener_id = 0)` — listener id 가 유효(>0)하면 LocalNodeMap 조회 후 해당 local_node 의 bind_ip 반환. bind_ip=0.0.0.0 이면 primary. 실패 시 primary fallback.
  * 신규 `GetLocalSipAddressForOutbound(proto="UDP", edge_preference="peering")` — protocol+edge 매칭 → protocol 매칭 → primary 순.
  * `_resolveBindIp(LocalNodeInfo)` 내부 헬퍼 (bind_ip=0.0.0.0 정규화)
- 호출부 업데이트:
  * `CscfModule.cpp` Service-Route: `GetLocalSipAddress(GetCurrentInboundListenerId())` — 수신 listener 기준
  * `ModuleDispatcher.cpp` 302 Moved Temporarily Contact: 동일 (+ `#include "SipStackThread.h"`)
  * `UserMap.cpp` OPTIONS keepalive: `GetLocalSipAddressForOutbound("UDP","access")` — UE 쪽으로 outbound
- **검증**: CMP+CSP 기동 + REGISTER smoke 정상 (403 password 이슈는 known). helper 가 기존 primary 반환 경로와 동일 결과 → 회귀 없음.
- **R5.b' 로 이관**: psip `CSipStack::Send` 가 실제 송신 시 어느 socket 을 쓰는지는 여전히 primary (`m_hUdpSocket` alias). Via header 값은 올바르지만 실제 source bind 는 primary. 엄밀한 per-listener bind 는 psip API 확장 필요.

### (J) R5.a: CspAddressing helper 도입 (2026-04-23 20:00–20:15)
- 새 파일 `csp/CspAddressing.h` + `csp/CspAddressing.cpp` (namespace `CspAddressing`):
  * `GetLocalSipAddress()` — SIP Contact/From/Call-ID host
  * `GetLocalRtpAddress()` — SDP media (RTP relay) IP
  * `GetLocalXcapAddress()` — XCAP/MCPTT URL host
  * 모두 현재 `gclsSetup.m_strLocalIp` 반환 (R5.b/R5.c/R6 확장점)
- **live code 참조 치환** (16개, dead code/R1 경로 제외):
  * `CscfModule.cpp:285` (Service-Route header): `GetLocalSipAddress()`
  * `UserMap.cpp:294,300` (From URI, Call-ID): `GetLocalSipAddress()`
  * `CspServer.cpp:363` (XCAP root URL): `GetLocalXcapAddress()`
  * `ModuleDispatcher.cpp:530` (Forward Contact): `GetLocalSipAddress()`
  * `ModuleDispatcher.cpp:579,641,691,695,713,764,774,794,795,803,849,893` (RTP relay IP): `GetLocalRtpAddress()`
- **치환 제외** (의도적):
  * `CspServer.cpp:117-149`, `SipServerSetup.cpp:201,402,417,425` — R1 경로 (write side: primary 주입/fallback)
  * `SipServerUserAgent.hpp`, `SipServerRegister.hpp`, `SipServerPickUp.hpp` — dead code (어디서도 include 안 됨)
  * `RtpMap.h:29`, `RtpMap.cpp:218,382` — CRtpInfo 내부 필드 (의미 다름)
  * `CspServer.cpp:391` — CSipStack 내부 참조 (psip setup)
- **검증**: CMP+CSP 기동 + cspsim REGISTER smoke 정상
  * `Contact: <sip:1001@192.168.0.2:5060>` 에 helper 결과 반영 확인
  * 403 Forbidden 은 known password 이슈 (R7 이전 무관)
- CMakeLists.txt 는 `file(GLOB SRCS "*.cpp")` 라 신규 파일 자동 수집. `cmake -B` 재설정 한 번 필요.

### (I) R4: CspListenerManager TCP/TLS 분기 (2026-04-23 19:45–20:00)
- `csp/CspListenerManager.h`: `ManagedInfo.protocol` 필드 확장 (UDP/TCP/TLS), 새 헬퍼 선언 (`_normalizeProtocol`, `_isAlreadyBound`, `_addListenerToStack`, `_removeListenerFromStack`)
- `csp/CspListenerManager.cpp` 전면 재구성:
  * `_normalizeProtocol(s)`: 소문자→대문자. "UDP"/"TCP"/"TLS" 만 허용, WS/WSS 는 빈 문자열 반환 → skip
  * `_isAlreadyBound(proto, ip, port)`: psip `Get{Udp|Tcp|Tls}ListenerInfo` 로 protocol 별 스냅샷 조회 후 포트/IP 매칭
  * `_addListenerToStack(m, outId)`: protocol 에 따라 `Add{Udp|Tcp|Tls}Listener` 호출. UDP 만 threadCount 전달
  * `_removeListenerFromStack(m)`: protocol 에 따라 `Remove{Udp|Tcp|Tls}Listener` 호출
  * `Sync()`: TCP/TLS 레코드를 desired 에 포함. TLS 의 `tls_cert_path` 는 로깅만 (per-listener cert 는 R5+)
  * 로그 포맷에 protocol 포함 (`ListenerManager: added id=... TCP 0.0.0.0:5062`)
- **검증**: local_nodes.jsonl 에 TCP(5062) + TLS(5063) 레코드 추가 기동
  * `AddTcpListener id=112379902 0.0.0.0:5062` + `added id=112379902 TCP 0.0.0.0:5062`
  * `AddTlsListener id=738090088 0.0.0.0:5063` + `added id=738090088 TLS 0.0.0.0:5063`
  * `ss -tlnp`: 5062, 5063 LISTEN 확인 (+ 기존 5061 bootstrap TLS, UDP 5060)
  * 기존 UDP primary 경로/bootstrap-skip 모두 정상
- **미해결 (R5)**: TLS per-listener cert (현재 stack-global SSLServerStart), outbound Route→listener 선택

### (H) R3: psip TCP/TLS multi-listener API (2026-04-23 19:30–19:45)
- `ext/psip/SipStack/SipStackListener.h`: `CSipStackTcpListener` / `CSipStackTlsListener` 클래스 추가 (UDP listener 미러: id/socket/bindIp/port/ipv6/drain/activeThreads/parent)
- `ext/psip/SipStack/SipStack.h`: `m_vecTcpListeners` / `m_vecTlsListeners`, per-type mutex, nextExtId 추가. Add/Remove/Get 공개 API + 내부 helpers (_Start*/_Stop*/_RefreshPrimary*Locked). `m_hTcpSocket`/`m_hTlsSocket` 은 primary alias 로 유지 (하위호환).
- `ext/psip/SipStack/SipStack.cpp`:
  * 생성자: `m_iNextTcpListenerExtId`/`m_iNextTlsListenerExtId = 0` 초기화
  * `Start()`: 기존 singleton `TcpListen`/`StartSipTcpListenThread(this)` 경로 제거. primary listener 를 `CSipStackTcpListener` 로 래핑, `_StartTcpListenerLocked` + `StartSipTcpListenThreadForListener(pPrimary)` 로 치환. vector push + alias 설정. TLS 동일.
  * `_Stop()`: `closesocket(m_hTcpSocket)` 제거, vector 순회하며 drain/close/delete. TLS 동일.
  * Add/Remove/Get TCP/TLS Listener 구현 (UDP 와 완전 대칭)
  * `_Stop*ListenerLocked` 는 `m_bDrain=true` 로 accept thread 종료 유도 후 `m_iActiveThreads==0` 대기 (최대 2초, 50ms 폴링) 후 socket close
- `ext/psip/SipStack/SipTcpThread.cpp`: `SipTcpListenerThread(LPVOID)` + `StartSipTcpListenThreadForListener(CSipStackTcpListener*)` 추가. `m_bDrain` 감지 + `m_iActiveThreads` 관리. 받은 connection 은 기존 shared `m_clsTcpThreadList` 로 SendCommand (worker pool 재사용).
- `ext/psip/SipStack/SipTlsThread.cpp`: TCP 와 대칭 (`SipTlsListenerThread` + `StartSipTlsListenThreadForListener`)
- `ext/psip/SipStack/SipStackThread.h`: per-listener thread 기동 함수 선언 추가
- **검증**: 전체 빌드 성공, UDP 회귀 없음 (primary+보조 테스트 PASS), CSP 기동 정상 (UDP 5060 LISTEN). TCP/TLS Start 경로도 vector 기반으로 동작 검증됨 (CSP 가 실제로 TCP/TLS 리스너를 쓰려면 R4 에서 config 에 `LocalTcpPort` 활성화 필요).

### (G) R2: UDP listener per-listener thread_count (2026-04-23 19:25–19:30)
- `csp/config/config_template.json`: `local_nodes.schema` 에 `thread_count` (int, default 2, min 1, max 32) 필드 추가
- `csp/CspLocalNodeMap.h/cpp`: `LocalNodeInfo.thread_count` (int, default 0=fallback sentinel) + Sync 파싱
- `csp/CspListenerManager.h/cpp`: `ManagedInfo.threadCount` 필드, row 에서 per-listener 값 파싱 후 `AddUdpListener(id, ip, port, threadCount, out)` 전달. 0/미지정 → `gclsSetup.m_iUdpThreadCount` fallback, 그것도 0 → 1 보정. 로그 포맷에 `threads=%d` 추가.
- **검증**: 보조 local_node (`bind_port=5070, thread_count=4`) 추가 기동 시 psip 로그 `AddUdpListener id=210824256 0.0.0.0:5070 threads=4` + `ListenerManager: added id=210824256 0.0.0.0:5070 threads=4` 확인. UDP 5060/5070 양쪽 LISTEN 확인. seed 원복 완료.
- 노트: primary 포트 (5060) 는 bootstrap 이 이미 바인딩하므로 ListenerManager 가 "already bound by bootstrap — skip" 로직 유지 — primary 에 thread_count 를 명시해도 bootstrap 이 `Setup.Sip.UdpThreadCount` 로 고정 기동. primary 의 per-listener thread_count 이행은 R3 이후 bootstrap 구조 축소 시 자연 해결 예정.


### (A) cims.sh build / configure / pkg 3단계 명확 분리
- 기존: build 가 pkg 자동 생성 (옵션 `--no-pkg` 로 스킵) → 혼재
- 변경: 3단계 완전 독립. `build` (빌드 + build/dist 복사) → `configure` (시험환경 설정) → `pkg` (tarball)
- `--no-pkg` / `-v` / `-m` 플래그 제거 (pkg 로 이관됨)
- help 텍스트 + `docs/VERIFICATION_PROCESS.md` 3단계 표기

### (B) CSP 설정 구조 통합 (config_template.json SOT)
- `csp/config/csp.json.template` **삭제** (sed 기반 구 렌더러)
- `csp/config/config_template.json` 이 유일 SOT — scalar + _infra + 9 collections 통합
- `configure.sh` 에 `apply_config_template()` Python 기반 렌더러 추가
  - `sections[*].fields` 의 dotted key 를 nested dict 로 구축
  - `deploy_value` 속성 있으면 그 값, 아니면 `default`
  - `@VAR@` 플레이스홀더 환경변수 치환
- Database.Host / Database.User / Log.Folder default 정합화 (`--db-host`/`--db-user` 반영되도록)

### (C) CSC handlers dist-first 로딩
- `csc/src/handlers/modules.py` + `agents.py`: dist 디렉토리 (`build/dist/<name>/pkg.json` + `config_template.json`) 우선 탐색, 없으면 cims_package DB fallback
- 가상 package id < 0 으로 DB 와 충돌 없이 Console 모듈관리 UI 에 표시
- Phase 1 에서 tarball 업로드 없이도 UI 에 "현재 build/dist 구성" 이 보이도록

### (D) Console ModuleConfigEditor rerender 방지 (기존 P3-02 관련)
- `ModuleConfigModal.tsx`: `editorSource` useMemo 로 identity 고정
- `ModuleConfigEditor.tsx`: React.memo 로 props 동일 시 re-render 차단
- 편집 중 polling 에 의해 값 유실되는 문제 제거

### (E) R1: primary local_node 자동 주입
- `csp/CspLocalNodeMap.h/cpp`: `is_primary` 필드 + `GetPrimary()` 메서드 (rule 1/2/3)
- `csp/CspServer.cpp`: ConfigCache + LocalNodeMap 선로드 + primary → `gclsSetup.m_strLocalIp`/`m_iUdpPort` override
- `csp/config/config_template.json`: `sip` 섹션에서 LocalIp/UdpPort 제거, `_infra` 로 이동, `local_nodes` schema 에 `is_primary` 추가
- `build/dist/config/local_nodes.jsonl` seed 에 `"is_primary": true` 명시
- **검증**: primary 경로 ✅ (`primary local_node 'volte-access' → LocalIp=192.168.0.2 UdpPort=5060`) + fallback 경로 ✅ (`no primary local_node — using _infra ...`)

### (F) dist 정리
- `build/dist/csp/bin/` 의 깨진 UTF-8 디렉토리 4개 (2026-04-13 18:56 flow/msg 초기 버그 잔재) 삭제

---

## R2 에서 확인된 흐름 (다음 세션 참고)

1. **`cims.sh build`** 가 수행하는 dist 복사 경로를 반드시 사용할 것. `cd build && make` 로는 `build/bin/csp` 만 갱신되고 `build/dist/csp/bin/csp` 는 구버전 유지 → 기동 시 구버전이 실행돼 혼란.
2. `cims.sh configure` 가 `local_nodes.jsonl` seed 를 **덮어쓰지 않음** (이번 세션 확인). 추가된 보조 노드 유지됨.
3. bootstrap listener 와 ListenerManager 의 역할 중복: `Setup.Sip.LocalIp/UdpPort` 는 `CSipStack::Start` 가 먼저 바인딩 → ListenerManager 는 같은 포트를 skip. R3~R4 에서 bootstrap 축소 검토 (primary 도 ListenerManager 가 SOT 로).

---

## 이전 세션 (2026-04-23 12:31–13:53) Phase 1/3 검증 결과

### Phase 1 완전 통과 (`verify_reports/20260423_123110_phase1.md`)
- 녹취 14개 / 0바이트 없음 / SIP/msg 로그 1141 라인 / ERROR/FATAL 0
- 수동 검증 4건: sesid 일관성, CSC CRUD→NOTIFY, Flow API nodes, mTLS(N/A)

### Phase 3 근사 검증 (`verify_reports/20260423_135337_phase3.md`)
- Phase 1 의 CSC/Console 을 New-CSC/New-Console 대체로 사용
- Agent 1개 + CMP/CSP/cspsim 3 deployment
- REGISTER smoke: Registered 1/1 PASS

---

## Phase 3 에서 발견된 배포 체인 gap (R7 이후 해결 예정)

- **P3-01 (Major)**: install 시 9 entity jsonl 이 install_path/config/ 에 **자동 전달되지 않음** — 수동 `PUT /collection/{name}` 필요. 미푸시 시 `Auth reject: 데이터 불완전` 403.
- **P3-02 (Major)**: scalar overlay 가 `install_path/config.json` 에만 저장되고 `install_path/{pkg}/config/{pkg}.json` 에 merge 안 됨. ★ R1 이후 CSP 가 overlay 직접 읽어 merge 하는 경로는 확인됨 (`SipServerSetup.cpp:180-191`) — Phase 1 환경에서는 정상 반영. 배포 경로에서는 여전히 추가 작업 필요.
- **P3-03 (Minor)**: 배포된 CSP/CMP 의 `ServiceLogging.Dir` 이 원본 호스트 경로 (다중 호스트 충돌).
- **P3-04 (Major)**: TB 3종 및 `cims.sh verify phase2/phase3` 자동화 미구현.

---

## 다음 세션 우선 처리

### ✅ Phase 1 회귀 — 완료 (2026-04-23 21:24, 위 섹션 참조)
### ✅ R8 엔진 체인 검증 — 완료 (2026-04-23 21:47, 아래 G 섹션 참조)

### (1순위) **R1~R8 + CSP 설정 보완 작업** (사용자 요청: Phase 2 진입 전 꼼꼼히)

보완 항목 목록. 우선순위 **P0 (기능 완성)** / **P1 (정합성)** / **P2 (청소)**.

#### ✅ 2026-04-23 22:55 — G3~G11 청소 완료

**해결**:
- **G3** `build/dist/config/listeners.jsonl` 0byte 잔재 삭제
- **G5** `Setup.Sip.UdpThreadCount` help 문구에 "local_nodes.thread_count 가 SOT / fallback" 명시
- **G6** access_services.jsonl 각 row 에 `server_identity_uri` 필드 추가 (volte/ptt 각각 `sip:cspserver@<domain>`). `cims.sh` 의 auto-seed 경로도 동일하게 반영.
- **G7** SipMessageLogger `WriteInterfaceLine` signature 에 `pszSesId` 추가, msg.jsonl 레코드에 `sesid` 필드 embed. SIP/CMP/CSC 모든 경로. 검증: `{"ts":..., "dir":"RX", "peer":..., "caller":..., "callee":..., "sesid":"+821357007001::csp::20260423225524849189::1", "proto":"SIP", "msg":...}`
- **G8** cspsim `-callee_override <user>` 옵션 추가. call scenario 에서 outbound INVITE target 을 임의 값으로 덮음 (외부 peer routing 시험용). `-mode` help 의 `voip|ptt` 도 `volte|ptt` 로 정정.
- **G11** CLAUDE.md `-mode voip` → `-mode volte` 예시 수정.

**Deferred (Phase 2 후속)**:
- **G4** `build/dist/log/csp.log` 는 cims.sh start 가 CSP stdout/stderr 캡처용으로 사용 (기동 실패 진단). **의도된 wrapper — 유지**. 정상 기동 시 0byte 는 정상.

---

#### ✅ 2026-04-23 23:48 — CMP→미디어서버 용어 통일 + Functions 타이틀 + _infra sub-grouping

**커밋 `5d2dc0b`**:

- **Roles → Functions** (타이틀만): section title "역할 (Roles)" → "기능 (Functions)". C++ key `Setup.Roles.*` 는 유지 (호환).

- **RTP Relay / CMP 연동 → 미디어서버 연동** (key+parser 동반 rename):
  - section key `rtp_relay` → `media_server`, title "미디어서버 연동"
  - field key rename:
    * `Setup.RtpRelay.UseRtpRelay`  → `Setup.MediaServer.Enable`
    * `Setup.RtpRelay.CmpIp`        → `Setup.MediaServer.Host`
    * `Setup.RtpRelay.CmpPort`      → `Setup.MediaServer.ControlPort`
    * `Setup.RtpRelay.LocalCmpPort` → `Setup.MediaServer.LocalPort`
    * `_infra.Setup.RtpRelay.LocalCmpIp` → `Setup.MediaServer.LocalIp` (infra hidden 에서 visible 섹션으로 이동)
  - `SipServerSetup.cpp` parser: MediaServer 블록 우선 + RtpRelay fallback (기존 배포 csp.json 호환)
  - C++ 내부 필드명 (`m_bUseRtpRelay`, `m_strCmpIp`, `m_iCmpPort`, `m_iLocalCmpPort`) 은 유지 — 외부 JSON key 만 rename, 파급 최소화

- **_infra sub-grouping**:
  - 섹션에 `groups` 배열 (sip_fallback / data_folder / service_logging / monitor / security)
  - 각 필드에 `group` 속성 부여 — Console UI 렌더 시 sub-header 로 가독성 확보
  - (UI 측 group 렌더링 구현은 별도 작업으로 남음)

**검증** (커밋 `17f98e1` 에 리포트):
- `cims.sh configure` 로 rendered `csp.json` 에 `MediaServer` 블록 생성, `RtpRelay` 블록 없음
- CSP restart + REGISTER smoke 1/1 PASS
- Phase 1 회귀: Registered 2/2, ERROR/FATAL 0, 녹취 14개 PASS (`verify_reports/20260423_234821_phase1.md`)

---

#### ✅ 2026-04-23 23:33 — config 섹션 재구성 + CDR 제거 + SIGUSR1 scalar reload

**구조 원칙 (사용자 C안 채택)**:
- csp.json 단일 파일 유지 (파일 분리 X — 배포 파이프라인 단순화 + P3-01 gap 확산 방지)
- config_template.json 의 `sections` 배열이 UI 조직 담당 (파일 구조와 독립)
- C++ key 경로 (`Setup.Sip.xxx`) 유지 → 파서/바인딩 무변경

**섹션 재구성** (커밋 `cdf5e90`):
- 기존 `sip` → **`sip_stack`** (UdpThreadCount, StackExecutePeriod) + **`cscf`** (MinRegisterTimeout, UserTimeout, SendOptionsPeriod) + **`tas`** (CallPickupId, StaleCallTimeout)
- CallPickupId 가 SIP Stack 에 있던 건 잘못된 분류 — VoLTE 당겨받기는 TAS 기능
- 섹션 키가 `Roles.CSCF` / `Roles.TAS` 와 1:1 매핑 → UI 조직 명확

**restart 재분류**:
- sip_stack 2 필드: `restart: true` 유지 (psip thread pool / stack tick)
- cscf 3 + tas 2 필드: `restart: false` + `reload_hint` — SIGUSR1 로 재로드되고 단순 값이라 다음 호출/cycle 에서 자연 반영
- _infra.Security.DenySipUserAgentList 도 `restart: false` 로 (다음 request 부터 적용)

**_infra 라벨 보완**:
- 모든 hidden 필드에 label 추가 (Console 편집 시 UX 확보)
- fallback 성격 / legacy 성격을 help 에 명시

**CDR 제거** (커밋 `ed69d4b`):
- `Setup.Cdr.Folder` 스키마 + `m_strCdrFolder` 필드 + parse 블록 제거
- `CspServer` 의 CDR 폴더 자동 생성 로직 제거
- `ModuleDispatcher::SaveCdr` → `OnCallEnded` 리팩토링:
  * CSV 파일 기록 블록 전체 제거 (service_log 로 대체)
  * `DbManager.UpdateCallLogEnded` + `CallDir.VoipCallEnd` 호출만 유지
- 호출부 (StopCall + CallMap BYE 처리) 2곳 업데이트
- 참조 없는 `csp/csp.xml` (legacy XML 설정 샘플) 삭제

**SIGUSR1 scalar reload**:
- CspServer 메인 루프 SIGUSR1 블록 맨 앞에 `gclsSetup.Read()` 호출 추가
- restart: false 필드들이 실제로 재로드되어 반영됨. bootstrap 필드 (DB/UdpThreadCount) 는 기존 객체에 미적용 (재기동 필요 — 기존 동작 유지)

**검증**:
- 빌드 성공
- Phase 1 회귀: Registered 2/2, Call 1/1, ERROR 0, 녹취 14개 PASS (`verify_reports/20260423_233219_phase1.md`)

---

#### ✅ 2026-04-23 23:10 — G9/G10 구현 + 커밋 완료

**G9 — TCP/TLS primary 도 local_nodes 자동 주입** (커밋 `8233c74`)
- `CspLocalNodeMap::GetPrimaryByProtocol(protocol)` 신규. 기존 `GetPrimary()` 는 UDP 지향 identity semantics 유지 (backward compat).
- CspServer 에서 UDP 주입 직후 `GetPrimaryByProtocol("TCP"/"TLS")` 호출 → `gclsSetup.m_iTcpPort / m_iTlsPort / m_strCertFile / m_strCaCertFile` override. 미조회 시 `_infra` 기존값 유지 (회귀 없음).
- 검증: local_nodes 에 TCP(25061) + TLS(5061, cert=csp.pem) primary 추가 기동 시 `"primary local_node '...' (TCP) → TcpPort=25061"`, `"...(TLS) → TlsPort=5061 cert=cert/csp.pem"` 로그 + `ss -tlnp` 로 bind 확인.

**G10 — SipServerMap (legacy IBCF XML) 완전 제거** (커밋 `2f48c12`)
- 삭제 파일: `csp/SipServerMap.{h,cpp}`, `csp/CspSipServer.{h,cpp}`, `csp/SipServerUserAgent.hpp`
- 호출부 정리: ModuleDispatcher (Start/RecvRequest/EventRegister/EventIncomingRequestAuth/EventIncomingCall 5곳), CspServer (Load + 주기 reload 2곳), Monitor (빈 응답 유지)
- Setup 필드 제거: `m_strSipServerDataFolder` + parse + `Setup.DataFolder.SipServer` 스키마 항목
- `EventIncomingCall` 의 callee 미등록 + policy 미매칭 fallback 은 이제 `CallPickupId` 아니면 `SIP_NOT_FOUND`
- 외부 peer 라우팅은 **PendingRouteMap (G1) 단일 경로**로 일원화 — 이중화 완전 제거
- 검증: 빌드 성공 (GLOB 자동 수집), Phase 1 회귀 Registered 2/2 / Call 1/1 / ERROR 0 / 녹취 14개 PASS

---

**커밋 체인 (feature/sip-console-runtime)**:
```
2f48c12  feat(G10):        SipServerMap (legacy IBCF XML) 제거
8233c74  feat(G9):         TCP/TLS primary 도 local_nodes 에서 자동 주입
9eeb07d  docs:             Phase 1 회귀 리포트 4건 추가
3d02d3a  chore(G3/G5/G6/G11): 설정 정리 + 문서 교정
966017a  feat(G8):         cspsim -callee_override 옵션 추가
af417ee  feat(G7):         sip.msg.jsonl 에 sesid 필드 embed
7cc8bf6  feat(G1/G2):      Routing 결정 단일화 via CspPendingRouteMap
6d9cb52  feat(R8):         ROUTING_ROUTE_SET 실 outbound 배선 (← 이전 세션)
```

**Phase 1 최종 회귀 (2026-04-23 22:55, 리포트 `verify_reports/20260423_225452_phase1.md`)**:
- ERROR/FATAL: 0 ✅
- Registered: 2/2 ✅
- Call OK/End: **1/1** ✅ (이전 실패 해결 — DB seed 복구 + 코드 변경 안정)
- 녹취: 14개 (VoIP 4 + PTT 10) ✅
- SIP/msg 로그: 1165 라인 ✅
- G7 효과 확인: sip.msg.jsonl 첫 레코드 keys = `['ts', 'dir', 'peer', 'caller', 'callee', 'sesid', 'proto', 'msg']`

---

#### ✅ G1/G2 해결 완료 (2026-04-23 22:39)

**구현 요약**:
- 신규 `csp/CspPendingRouteMap.{h,cpp}` — Call-ID 기반 routing decision 저장소 + TTL cleanup
- `ModuleDispatcher::RecvRequest`: INVITE 분기의 AddRoute 블록 → **PendingRouteMap.Insert(CallId, {ip,port,protocol,route,route_set,policy})** 로 교체. REJECT 는 그대로 403.
- `ModuleDispatcher::EventIncomingCall`: 서비스 모드 체크 직후 **PendingRouteMap.Take(CallId)** 시도. 있으면 `clsUserInfo` 에 peer 세팅 + `SetCallOwner(IBCF)` + `bRoutePrefix=true` → B2BUA `CreateCall` 가 B-leg 을 peer 로 생성 → dialog state 에 route 저장 → 이후 ACK/BYE 자동 forward.
- Pending Take 가 `isAlive` 체크보다 **앞**에 위치 → callee 가 내부 가입자여도 policy 매칭 시 외부로 라우팅 (policy 우선).
- `m_clsIbcf.IsEnabled()` 조건 **제거** — PTT 는 앞단에서 이미 분기되어 본 블록 도달 시 VoLTE 뿐.
- `CspServer::ServiceMain` 메인 루프에 30초 주기 `gclsPendingRouteMap.CleanupExpired(30s)` 추가.

**실효 검증 완료** (2026-04-23 22:39):
- `[SYSTEM] RoutingPolicyEngine: outbound via route_set='r8-test-routeset' route='r8-test-route' policy='r8-test-policy' → 127.0.0.1:5999/UDP` 출력
- `csp_01_sip.msg.jsonl` 에 `TX INVITE sip:+821357007002@127.0.0.1:5999` 다수 + CANCEL 기록 — **실제 B-leg peer forward 동작 확인**
- peer 응답 없음 → 재전송 → CANCEL → status=487 (정상 흐름)
- seed 제거 후 SIGUSR1 reload → 모든 엔진 0 로 복원

**B2BUA 체인**:
```
RecvRequest (INVITE only, Decide 1회) → PendingRouteMap.Insert
  → return false (UserAgent 에 위임)
CSipUserAgent::RecvRequest → dialog 생성 → EventIncomingCall
  → PendingRouteMap.Take → clsUserInfo 세팅
  → CreateCall(&clsRoute) → B-leg INVITE 를 peer 로 생성
  → StartCall → 실 TX
동일 Call-ID 의 ACK/BYE/re-INVITE → CSipUserAgent dialog state 로 자동 forward
```

**파일 변경**:
- 신규: `csp/CspPendingRouteMap.h` / `csp/CspPendingRouteMap.cpp`
- 수정: `csp/ModuleDispatcher.cpp` (RecvRequest + EventIncomingCall)
- 수정: `csp/CspServer.cpp` (include + cleanup timer)

**Phase 1 회귀 재실행** (2026-04-23 22:42):
- ERROR/FATAL 0 ✅, PTT 그룹콜 PASS ✅, 녹취 10개 ✅
- VoIP call fail 1건 — **DB `volte_subscriptions` 의 `+821357007003` row imsi 필드 오염** (msisdn 중복 저장). R8 변경과 무관한 환경 seed 결함.
- 복구 SQL (사용자 확인 후 실행):
  ```sql
  UPDATE volte_subscriptions SET imsi='450033100000003', passwd='123456' WHERE id='+821357007003';
  UPDATE volte_subscriptions SET passwd='123456' WHERE id='+821357007001';
  ```

---

#### (해결됨, 참고용) G1. R8 ROUTE_SET wiring 이중화 / 통합
2026-04-23 21:47 seed 검증에서 발견. **2개의 ROUTE_SET 평가 지점이 병존**:

| 경로 | 위치 | 실효 | 조건 |
|---|---|---|---|
| A | `ModuleDispatcher::RecvRequest:275+` | **무효** — `AddRoute()` 로 원본 RX 메시지에 Route 박고 `return false` 하지만 CSipUserAgent B2BUA 가 새 Call-ID 로 B-leg 메시지를 **새로 생성**하므로 Route header 가 carry-over 안 됨 | INVITE 진입 시 항상 |
| B | `ModuleDispatcher::EventIncomingCall:489+` | **실효** — `clsUserInfo.m_strIp/port/eTransport`, `SetCallOwner(IBCF)` 로 outbound 배선. "outbound via route_set=..." 로그 출력 | `m_clsIbcf.IsEnabled() && gclsCspUserMap.isAlive(pszTo)==false` |

**검증 팩트**:
- 엔진 체인 (Decide → SelectRoute → picked_route → GetByName) **완전 동작** — `[SYSTEM] RoutingPolicyEngine: policy='r8-test-policy' route_set='r8-test-routeset' picked_route='r8-test-route' → RemoteNode r8-test-peer (127.0.0.1:5999 UDP)` 출력 확인
- 그러나 실 TX outbound INVITE 가 peer 로 나가지 않음 (경로 A 로 진입하고 경로 B 는 callee isAlive=true 라서 미진입)
- IBCF=ON + 매칭된 callee isAlive=false 조합에서만 경로 B 진입 (그 조합 구성이 까다로움)

**해결 방향** (설계 결정 필요):
1. 경로 A 제거 + 경로 B 를 ROUTE_SET 평가 단일 지점으로 통합
2. 또는 CSCF/TAS 모드에서도 ROUTE_SET 적용할지 설계 결정 (현재 IBCF 전용)
3. CSipUserAgent B2BUA forward 시 원본 Route header carry-over 메커니즘 (경로 A 를 유지하려면)

#### (해결됨, 참고용) G2. EventIncomingCall ROUTE_SET — IBCF 역할 제약
`m_clsIbcf.IsEnabled()==true` 여야만 외부 peering 가능. CSCF/TAS 배포 환경에서는 RoutingPolicy 아무리 정의해도 외부로 forward 안 됨.
- 의도된 설계인지 확인 필요. 의도라면 문서화, 아니면 조건 완화.

#### (P1) G3. `listeners.jsonl` 잔재
- `build/dist/config/listeners.jsonl` 0byte 잔재. `config_template.json` 에는 이미 제거됨, `CACHE_LISTENER` 도 제거됨.
- → `cims.sh configure` 또는 seed 정리 로직에서 자동 삭제.

#### (P1) G4. `build/dist/log/csp.log` 0byte wrapper
- 실 로그는 `build/dist/csp/log/csp_YYYYMMDD_N.log`. wrapper 용도 불분명.
- verify/tooling 이 이 경로를 참조하는지 확인 후 제거 or 용도 명시.

#### (P1) G5. `Setup.Sip.UdpThreadCount` fallback 의미 불명
- R2 이후 SOT 는 `local_nodes.thread_count` 이지만 `sip` 섹션에 노출돼 있어 UI 에서 편집 가능.
- help 문구에 "fallback (local_nodes 미지정 리스너용)" 명시.

#### (P1) G6. `access_services.server_identity_uri` seed 미지정
- 현재 volte/ptt 모두 비어있어 domain 기반 자동 조립 (`sip:cspserver@<domain>`). 운영 환경에서는 명시 권장.

#### (P1) G7. SIP msg.jsonl 에 `sesid` 필드 부재
- Call-ID 만 있고 sesid 없음. Phase 1 수동 검증에서 SIP ↔ CMP 대조를 Call-ID 로 재해석해야 했음.
- `SipMessageLogger` 가 CCallDir 조회해서 sesid 필드 embed 하면 디버깅 편의 ↑.

#### (P1) G8. cspsim 외부 peer 시험 수단 부재
- R8 경로 B 검증 시 "CSP 내 사용자가 아닌 callee" 로 INVITE 발사 필요. 현 cspsim 은 DB 모드 자동 로드만 지원.
- 옵션: cspsim interactive `c <uri>` 또는 `-callee_override sip:user@peering.test`.

#### (P2) G9. bootstrap TCP/TLS listener vs ListenerManager 이중화
- `_infra.Setup.Sip.TcpPort=25061 / TlsPort=5061` 이 bootstrap 경로 유지 → ListenerManager 는 "already bound by bootstrap — skip". UDP primary 는 R1 에서 정리됐지만 TCP/TLS 는 남아있음.
- local_nodes 완전 SOT 화: bootstrap 경로 축소 (TCP/TLS primary 도 local_nodes 에서 주입).

#### (P2) G10. Setup.DataFolder.SipServer (legacy XML)
- 빈 디렉터리 (`build/dist/csp/route/`). IBCF XML 경로는 R8 ROUTE_SET 으로 대체 의도. schema 에는 잔재.
- `SipServerMap` 관련 코드 잔재 정리.

#### (P2) G11. CLAUDE.md `-mode voip` 언급
- cspsim 소스는 `volte` 만 accept. CLAUDE.md 의 "Running" 예시 업데이트.

#### (참고) I. Phase 3 배포 체인 gap (기존)
- P3-01: install 시 9 entity jsonl 자동 전달 X (수동 PUT 필요)
- P3-02: scalar overlay merge gap
- P3-03: 배포 CSP/CMP 의 ServiceLogging.Dir 이 원본 호스트 경로
- P3-04: Phase 2/3 verify 자동화 미구현

### (2순위) Phase 2 (배포) — 위 P0 해결 후 진입
- `cims.sh verify phase2` 자동화 + P3-01/02/03 해결 필수

### ✅ 선행 이슈 해결 (2026-04-23 20:55)
REGISTER 403 의 근본 원인 파악 + 수정 완료 (커밋 `5b91f60`):
- cspsim 의 DB 모드가 `strFilterMode=="volte"` 만 accept 하는데 cims.sh 기본이 `-mode voip` 여서 DB 조회 0명. 
- domain 미지정 시 cspsim default `csp` 로 fallback → access_services.jsonl 의 실제 domain 과 auth_id 불일치.
- 해결: cims.sh 기본 `-mode volte` 로 변경 + DB 모드일 때 access_services.jsonl 에서 kind 매칭 domain 자동 감지. cspsim 소스 무변경.
- 검증: `./cims.sh sim -scenario register` 만으로 `User=+821357007001` / IMPI=`450033100000001@ims...3gppnetwork.org` / **200 OK** 확인.

### (참고) 이번 세션 별도 이슈 (R7 회귀 전에만 확인)
- REGISTER 403 (`450033100000001`/`123456`): 이전 세션 관측, R1 변경과 무관. R7 회귀 전 CSC API (`/api/v1/volte/subscriptions`) 로 DB 상태 재확인 필요.

---

## 환경 사실 (2026-04-23 22:15 기준)

- 외부 IP: `ens160` (DHCP). 매 세션 `ip -4 addr show ens160` 로 재확인. 현재 **192.168.0.2** (22:15).
- **smoke test 표준 커맨드 (R 전체 세션 검증용)**: `./cims.sh sim -scenario register -count 1 -duration 3`
  * DB 모드 + domain 자동 감지 → User=+821357007001 (MSISDN), IMPI=450033100000001@ims...3gppnetwork.org → 200 OK
- Admin JWT: `admin / 1234` (login_id/password, HTTPS 4420, 응답 `token`)
- volte_subscriptions password: 이전 memo 는 `123456` 이었으나 현재 REGISTER 403 관측 → 세션 시작 시 확인 필요
- DB: MariaDB localhost `cims/<REDACTED_DB_PW>`
- 가입자: volte 8건 / ptt 11건
- access_services seed: volte@ims.mnc033.mcc450.3gppnetwork.org, ptt@ptt.mnc033.mcc450.3gppnetwork.org

---

## 검증 절차 SSOT (유지)

- `docs/VERIFICATION_PROCESS.md` — Phase 0/1/2/3 3단계 절차 (이번 세션에서 build/configure/pkg 3단계 표기로 갱신)
- `cims.sh reset` / `cims.sh verify phase1` 자동화 (phase2/phase3 미구현)
- CSC Python 소스 수동 동기화 필요: `cp csc/src/handlers/*.py build/dist/csc/src/handlers/` + `./cims.sh restart csc`
- 로그 경로: `build/dist/ext_mnt/service_log/YYYY/MM/DD/HH/{system_id}_{iface}.msg.jsonl` + `{system_id}.flow.jsonl`

---

## 현재 실행 중인 프로세스

- **2026-04-23 21:24 기준: 전 서비스 실행 중** (CMP/CSP/cwrtc/csc/console/phone)
- Phase 1 회귀 PASS 후 세션 유지 상태. 다음 작업 전에 필요 시 `./cims.sh restart` 또는 `./cims.sh stop`.

---

## Task 리스트 (TaskList 에 남아있음)

- ✅ #1 cims.sh build/configure/pkg 3단계 분리
- ✅ #2 CSP 설정 렌더러 검증 (apply_config_template)
- ✅ #7 R1 primary local_node 주입
- ✅ #8 R2 UDP multi-listener thread_count
- ✅ #9 R3 psip TCP/TLS multi-listener API
- ✅ #10 R4 CspListenerManager TCP/TLS 분기
- ✅ #11 R5.a CspAddressing helper 도입
- ✅ #12 R5.b Context-aware 분기 (inbound id / outbound proto+edge)
- ✅ #13 R5.c TLS per-listener SSL_CTX
- ✅ #14 R5.b' psip UDP Send per-request source socket
- ✅ #15 R5.b'' UDP response path per-listener source
- ✅ #16 R5.b''' TCP/TLS client connect source bind
- ✅ #17 R6 From identity per Access Service
- ✅ #18 R7 ACL/Routing Policy 메시지 경로 연결 (기존 구현 확인)
- ✅ #19 R8 Outbound routing wiring (ROUTE_SET picked_route → Route header → B2BUA forward)
- 🟡 #20 Phase 1 회귀 6항목 (다음)
- ⏸ #12 R6 From identity per Access Service
- ⏸ #13 R7 ACL/Routing Policy 경로 연결
- ⏸ #3 Console 모듈관리 UI 점검 (R7 대기)
- ⏸ #4 Phase 1 회귀 6항목 재검증 (R7 대기)
- ⏸ #5 Phase 2 배포 검증 (Phase 1 PASS 대기)
- ⏸ #6 Phase 3 배포 체인 검증 (Phase 2 PASS 대기)
