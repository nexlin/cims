---
name: 2026-05-10 round 2 — verify pipeline S1~S6 종합 점검 + W1/W2 commit
description: S1~S6 단계별 [목표/환경/종속성] 3관점 점검 후 P0 두 건 (init wizard + S6 VoLTE IP 정합화) 구현 + commit (4105f59)
type: project
originSessionId: feb205b4-1076-433a-9ec6-adf38ea7a813
---
# 2026-05-10 round 2 세션

## 결론

**commit `4105f59`**: feat(verify): 초기설정 wizard + S6 VoLTE IP 정합화 (W1+W2). 7 files / +239 / -16. main 이 origin 보다 1 commit 앞 (push 는 사용자 결정).

## S1~S6 점검 결과 (단계별 3관점: 목표 / 환경 / 종속성)

| Stage | 항목 수 | 핵심 발견 |
|---|---|---|
| S1 | 5 | 큰 이슈 없음. cmp/cwrtc/cspsim 의 .clang-format 정책 미정 (보류) |
| S2 | 2 | PREFLIGHT 가 사실상 항상 PASS (정보성, gate 부재) / BUILD 가 dev binary unlink → 좀비 |
| S3 | 7 | dev process 가 S5 까지 살아남음 (의도 — 사용자 외부 단말 시험 가능) / SEED 무조건 PASS / **ens_ip 빈 값 → 127.0.0.1 fallback 이 REQ1+REQ2 동시 위배** |
| S4 | 2 | dev 안 건드림 ✅. PKG-BUILD `n>=5` 조건 약함 (소소) |
| S5 | 7+ | dev 안 건드림 ✅. _INSTANCES IP 분리 (csp/cmp=127.0.0.1, psp/pmp=127.0.0.3) — dev 가 ens_ip 일 때만 정합 |
| S6 | 13 | **VoLTE 시나리오 2종이 dev csp 향함 — PTT 와 비대칭, 배포본 검증 본질 위배 (확정 bug)** |

## 사용자 핵심 요구사항 (S3 검토 도중 명시)

> S3 까지가 가장 중요. S4~S6 는 배포 후 종합 회귀 재확인. 각 모듈 기능은 S3 에서 검증되어야 함. S3 종료 후 사용자가 단말로 외부에서 접속해 추가 시험 가능해야 함.

- **REQ1**: S3 dev 모듈은 ens160 IP 로 bind (외부 단말 접속).
- **REQ2**: S4~S6 가 S3 dev 환경에 영향 X (사용자 추가 시험과 공존).
- 동시 운용 정책: dev (ens160) + 배포본 (127.0.0.1 base + 127.0.0.3 variant) 가 동시 가동.

## 구현 — W1 (init wizard + .cims/server.local.json)

**목적**: ens160 hardcoded 가정 + 127.0.0.1 fallback 제거.

**변경**:
- `cims.sh` 신규 `cmd_init` — default route src IP 자동 감지, env (CIMS_LOCAL_IP/CIMS_DB_PASSWORD) override, `--non-interactive` 모드, `.cims/server.local.json` (0600) 저장
- `.gitignore` — `.cims/` 추가
- `configure.sh:29` — `LOCAL_IP="127.0.0.1"` default 제거. env > .cims > 빈 값(abort) 우선순위
- `verify/lib/context.py` — `_detect_ens160_ip` 폐기 → `_detect_local_ip(repo_root)` (env > .cims > default route src)
- `verify/lib/items/stage3/configure.py` — ens_ip 빈 값 시 즉시 FAIL + 안내

**효과**: dev 가 ens_ip 로, 배포본은 127.0.0.1/127.0.0.3 으로 자연 분리. 인터페이스 이름 (ens160/eth0/enp0s3) 무관.

## 구현 — W2 (S6 VoLTE IP 정합화)

**근거**: PTT 시나리오는 `target_ip("psp", ctx.sim_ip)` 로 배포본 향함. VoLTE 만 `ctx.sim_ip` 로 dev 향함 — 비대칭 + 배포본 검증 본질 위배. 저번 세션 (488259e) VoLTE FAIL 의 직접 원인.

**변경** (작은 diff):
- `verify/lib/items/stage6/volte_voice.py:21`: `ctx.sim_ip` → `target_ip("csp", ctx.sim_ip)` + import
- `verify/lib/items/stage6/volte_video.py:21`: 동일

## 회귀 검증

- bash -n / python3 ast.parse syntax OK
- `python3 -m unittest tests.test_verify_lib` — 161 PASS
- `cims.sh init --non-interactive` + env override → `.cims/server.local.json` 정상 생성 (0600/0700)
- `configure.sh` 의 LOCAL_IP 빈 값 abort (rc=1 + 안내)
- `_detect_local_ip` 우선순위 (env > .cims > default route src) 동작
- `target_ip("csp")=127.0.0.1`, `target_ip("psp")=127.0.0.3` 정합
- `stage3/configure` 의 ens_ip="" → FAIL 흐름 OK

## 미해결 / 백로그

### P1/P2 (다음 라운드 후보)
- S3-SEED 의 무조건 PASS → 조건 분기 (seeded_n / reload 검증)
- S2-PREFLIGHT gate 화 (의도/누락 결정 후)
- S3-HEALTH ERROR/FATAL grep 정밀화 (실제 로그 샘플 확인 후)
- S3-CONFIGURE depends_on 표기 정리 (S3-RESET 미실행 preset)
- S4-PKG-BUILD `n >= 5` → `n == len(_TARGETS)` 강화

### 정책 결정 보류
- S2-BUILD 가 dev process 동거 시 자동 stop / warn 정책
- cmp/cwrtc/cspsim 의 .clang-format 정책

### B1 — 별도 작업 (S1~S6 외)
**상용 환경 검증 도구 (배포된 cspsim 활용)**: 사용자 의도 — S1~S6 와 분리, 상용 서버 정식 배포 후 운영 환경 검증. 배포된 sim (`<install_path>/sim/cspsim/bin/cspsim`) 호출. 운영 console UI 의 별도 메뉴.

## 다음 세션 진입

1. ~~사용자 측 `./cims.sh init`~~ — 2026-05-10 round 3 에서 `--non-interactive` 로 자동 생성 완료 (local_ip=192.168.199.129 자동 감지, db_password=<REDACTED_DB_PW> 기본값). 사용자가 본인 환경 IP/PWD 로 바꾸려면 다시 `./cims.sh init` 실행 (대화형) 또는 `.cims/server.local.json` 직접 편집.
2. ~~push origin main~~ — `4105f59` 이미 origin/main 동기화 완료 (round 2 종료 시점에 push 됨, 메모리 노트가 stale 했음).
3. (선택) pipeline-full 1회차 → W1+W2 LIVE 종합 회귀 확인. 단, "환경 변동성" 백로그상 LIVE 회차마다 fail 다름 — 결과 해석 주의.
4. P1/P2 보완 라운드 진입 또는 B1 (상용 환경 검증 도구) 별도 라운드.

## 2026-05-10 round 3 (옵션 1+2 진행 — pipeline LIVE + P1/P2 보완)

W1+W2 구조 검증:
- W1 init wizard `--non-interactive` 정상 (`.cims/server.local.json` 0600, dir 0700)
- W1 read 경로 정상 (`configure.sh:36-43, 47, 58`)
- W2 의도 정상 (`target_ip("csp", ctx.sim_ip)` → 127.0.0.1)
- 161 unit tests OK

옵션 1 — pipeline-full LIVE 회귀 baseline:
- W1+W2 직후 1회: 34 / PASS 33 / FAIL 0 / SKIP 1 (S6-SCN-CERT-ROTATE) / 361.3s
- 옵션 2 적용 후 1회: 34 / PASS 33 / FAIL 0 / SKIP 1 / 333.2s
- "환경 변동성" 백로그 (LIVE 회차마다 fail 다름) — 본 round 2회 모두 clean PASS

옵션 2 commit `a72fba7` — feat(verify): S2~S4 gate/조건 정밀화. 5 files / +319 / -37.

| 항목 | 변경 |
|---|---|
| S3-SEED | seeded_n>0 + reload OK 조건 PASS (이전: 무조건 PASS) |
| S2-PREFLIGHT | cims.sh preflight 위임 → verify lib 자체 검사 (4 BLOCK: local_ip/DB/TB 3종/검증포트). 검증포트는 cims 외부만 BLOCK (cwd-기반 cims pid 판정 — cims-phone vite 등 monorepo 하위 모두 catch). cims.sh preflight CLI 는 정보성 그대로 유지 |
| S3-HEALTH | psip C++ `[E]/[F]` 마커 + uvicorn `ERROR:/CRITICAL:` 행시작 매칭. 전체 파일 → tail 2000줄 |
| S3-CONFIGURE | depends_on=[S3-RESET] 제거 (cross-preset 표기 misleading) |
| S4-PKG-BUILD | `n>=5` → 12 expected 컴포넌트 정확 매칭 (cmp/pmp/imp/csp/psp/isp/cwrtc/csc/console/phone/cspsim/agent) |

S2-PREFLIGHT 정책 결정 (사용자):
- BLOCK: local_ip 미감지 / DB 연결 실패 / TB 3종 미동작 / 검증포트 외부 점유
- WARN 유지: Phase 2 잔존 (4445/8081), git uncommitted

S2-PREFLIGHT 구현 중 발견 버그 (커밋에 fix 포함):
- regex `\b\S*:PORT\b` 의 leading `\b` 가 `*:PORT` 와 안 맞음 → 선두 `\b` 제거
- `python3.12` 같은 minor 버전 suffix exe basename 에 매칭 실패 → `base.startswith("python")` 으로 변경
- cwd 기반 판정 추가로 cims-phone 같은 누락 케이스 catch

## round 3 종료 시점 상태
- main: `a72fba7` — `4105f59` 보다 1 commit 앞. push 는 사용자 결정.
- `.cims/server.local.json` 존재 (192.168.199.129 / <REDACTED_DB_PW>).
- dev (ens_ip:5060) + 배포본 4종 (csp/cmp/psp/pmp + mgmt-server csc) 동시 가동.

## 2026-05-10 round 4 — P2 ISP/IMP Layer 1+2

**commit `8194160`**: feat(verify): P2 — ISP/IMP 인스턴스 활성 (Layer 1+2) + cmd_reset 좀비 정리 보강. 4 files / +130 / -17.

### 변경
1. **`_INSTANCES` 확장** (`verify/lib/items/stage5/_native_steps.py`): isp/imp entry 추가
   - isp: agent=ibcf-sip-server, 127.0.0.5:5060, role IBCF only
   - imp: agent=ibcf-media-server, 127.0.0.5:9000, peer=isp
   - sync_port 9909/9910, MediaServer.LocalPort 9013

2. **`cmd_reset` 보강** (`cims.sh:998+`):
   - _agents 목록 + ibcf-sip-server / ibcf-media-server (이전 5종 하드코딩)
   - `_enum_install_pids()` helper — /proc/PID/exe + cwd prefix 매칭, '(deleted)' strip
   - $DIST_DIR/$_a + .prev 양쪽 wipe

3. **S2-PREFLIGHT loopback alias 정보성** (`verify/lib/items/stage2/preflight.py`):
   - required_aliases vs has_alias 비교, 누락 시 `sudo ip addr add ...` 안내
   - BLOCK 안 함 (실 실패는 S5 LISTEN 검증에서 surface)

4. **테스트 fixture 6 인스턴스 갱신** (`tests/test_verify_lib.py`):
   - step 17 (post_multipart, expected pkg2_id_isp/imp)
   - step 18 (_AID_BY_AGENT + spawned len 6)
   - step 19 (_PMAP + ISP IBCF role 단독 assertion + install_path/ibcf-sip-server)
   - step 20/21 dep2_id_isp/imp 추가

### LIVE 디버그 발견
- pipeline-full 첫 회차: csp 단독 미기동 (rc=1) — 21/1/12
- 두번째 회차: csp/psp/pmp 모두 미기동 — 누적 cascading
- 원인: install 시 current → .prev rename, OLD process 의 exe 가 .prev path → kill_stray (NEW path) 와 _kill_own_install_listener (NEW path) 둘 다 매칭 실패. NEW binary bind 시점에 OLD 가 port 점유.
- prep-reset → pipeline-full 사이클로 정착. clean state 한 번 거치면 PASS.
- 6 인스턴스 baseline pipeline-full: 345.7s (4 인스턴스 333s 와 동등)

### Layer 3+ 보류 (사용자 결정)
- routing_policies.jsonl / routes.jsonl seed (CSP 정책 seed 백로그)
- S6-SCN-IBCF-TRUNK 시나리오 (외부 SIP peer 또는 self-loopback)
- 운영 console UI 의 IBCF 라우팅 관리 화면

## round 4 종료 시점 상태
- main: `8194160` — **origin 동기화 완료** (`a72fba7`, `8194160` 둘 다 push, round 4 종료 시점).
- `.cims/server.local.json` 존재.
- 127.0.0.5 loopback alias 등록 (사용자 1회 sudo).
- 배포본 6종 정상 LISTEN: csp(127.0.0.1)/psp(127.0.0.3)/isp(127.0.0.5) + cmp/pmp/imp 동일 IP.
