---
name: 2026-05-08 P1 세션 — PSP/PMP 분리 + 디버깅 11종 (commit 390cda1)
description: P1 토폴로지 구현 + LIVE 디버깅 11개 fix. 18회차 32/1/1 PASS. commit 390cda1 push 완료.
type: project
originSessionId: 7e91a791-869b-409a-9e4e-7cc15b69d99f
---
# 2026-05-08 P1 세션 — committed 390cda1 (push 완료)

**시작 base**: c700744. **commit 390cda1**: P1 인프라 + 11개 fix, 20 파일 변경 (19 modified + 1 new), 750 insertions / 184 deletions.

## 개요
사용자가 명시한 상용 배포 토폴로지 (CSP/ISP/PSP + CMP/IMP/PMP 6 인스턴스) 의 P1 (PTT 우선 = PSP+PMP 분리) 을 검증 파이프라인 (S4/S5/S6) 에 반영. csp/cmp 바이너리 그대로 + Roles 토글로 인스턴스화 — C++ 리팩토링 0.

## 사용자 결정 (이 세션에서 컨펌)
1. **모듈 정의**: csp 바이너리는 다용도 (CSP=CSCF+TAS, PSP=CSCF+PTT_AS, ISP=IBCF). cmp 도 동일 (CMP/PMP/IMP). 설정만 다름.
2. **패키지**: 모듈별 별도 tarball (csp/isp/psp/cmp/imp/pmp 6종 + csc/console/cspsim 3종 = 9종).
3. **P1 범위**: 패키지=9종, 배포/검증=csp/psp/cmp/pmp 4 인스턴스 (ISP/IMP 는 P2).
4. **네트워크**: loopback IP alias 분리 (CSP+CMP=127.0.0.1, PSP+PMP=127.0.0.3).
5. **CSC notify**: CspNotify (VoLTE) + PspNotify (PTT) 분리 endpoint. GROUP_CHANGED → PSP only, USER_CHANGED → 양쪽 broadcast.
6. **legacy 제거**: pipeline-full 자체가 P1 토폴로지 (4 인스턴스). 기존 36/36 baseline 폐기, 새 baseline 재정의.

## 변경 묶음 (commit 단위 후보)

### 1. _INSTANCES descriptor 도입 (P1 SoT)
**파일**: `verify/lib/items/stage5/_native_steps.py:89-`
- `_MODULES` tuple → `_INSTANCES` list of dict (id/tarball/dir/process/sync_port/local_ip/listen/peer_id/config_overlay).
- 5 entry: csp/psp/cmp/pmp/sim. ISP/IMP 는 P2 entry 추가.
- 호환 dict (`_TARBALL_PREFIX` 등) 자동 생성 — 기존 사용처 점진 교체.
- Helper: `_instance(id)` lookup.

### 2. loopback IP alias helper
**파일**: `verify/lib/common/loopback.py` (신규), `_native_steps.py:step_01_cleanup`
- `has_alias(ip)` / `ensure_alias(ip)` / `required_aliases(instances)`.
- step_01 (cleanup) 직후 자동 호출 — 모든 LocalIp ≠ 127.0.0.1 검증/추가.
- sudo -n 권한 없으면 SKIP/WARN, cleanup PASS 유지 (step_21 LISTEN 단계에서 명확 fail).

### 3. cims.sh 6 variant dispatch
**파일**: `cims.sh`
- `_start_csp_variant <name>` / `_start_cmp_variant <name>` 일반화.
- `start_csp` / `start_psp` / `start_isp` / `start_cmp` / `start_pmp` / `start_imp` 모두 wrapper.
- `_apply_overlay_to_module_config` — install_path/config.json (dotted-key dict) 을 csp.json/cmp.json 에 머지. 시작 직전 자동 호출.
- `_svc_port_proto` + `cmd_status` + `_start_one` 모두 6종 인지.
- `kill_stray` 패턴은 `$DIST_DIR/csp/bin/csp` 절대경로 — 다른 인스턴스 영향 차단.

### 4. cmd_pkg 9종 tarball
**파일**: `cims.sh:cmd_pkg`
- default targets = `(cmp pmp imp csp psp isp cwrtc csc console phone cspsim agent)` 12종 (핵심 9 + 부가 3).
- `_src_root_for(psp)` = `$SCRIPT_DIR/csp` (소스 동일).
- `_src_sub_for(psp)` = `csp` (tarball 안 모듈 dir 이름 = csp/cmp 그대로).
- meta.json 의 name 만 분리 (psp/isp/imp/pmp).
- description suffix — `_ROLE_SUFFIX` dict 로 PSP/ISP/PMP/IMP 식별 표시.
- 변종 (psp/isp/pmp/imp) 은 base (csp/cmp) 의 version 을 read-only — patch+1 누적 회피.

### 5. S5 step 17~21 + finalize 일반화
**파일**: `verify/lib/items/stage5/_native_steps.py`
- step_19 (line 1497): `_instance(m)` lookup + `payload["config"] = inst["config_overlay"]` (deployment 시점 overlay 주입).
- step_21 (line 1632): `_LISTEN_PORTS.items()` → `_INSTANCES iteration (listen != None)`. host=inst.local_ip 별 _wait_listen.
- step_21 cmp wait: csp↔cmp + psp↔pmp 두 pair 모두 `OnCmpStatusChanged: Connected` 검사.
- step_22 finalize: stop list 도 `_INSTANCES (listen != None)` 에서 유도.

### 6. shell.port_listening + _wait_listen host kwarg
**파일**: `verify/lib/shell.py`, `_native_steps.py:_wait_listen`
- `port_listening(port, proto, host="")` — host 매칭 (0.0.0.0/* 도 허용).
- `_wait_listen(port, proto, timeout, host="")` 시그니처 추가.

### 7. S6 entry-check N 인스턴스 일반화
**파일**: `verify/lib/items/stage6/entry_check.py`
- `_required_ports` 가 csc/console + `_INSTANCES (listen != None)` 모두 반환. (port, proto, host, label) 4-tuple.
- shell.port_listening(host=host) 로 IP 별 정확 매칭.

### 8. PTT 시나리오 endpoint = PSP/PMP
**파일**: `verify/lib/items/stage6/{ptt_voice,ptt_video,scn_subscribe,scn_cmp_group_sync}.py`, `_helpers.py`
- `target_ip(role, default)` helper — `_INSTANCES` 에서 role.local_ip 반환.
- ptt_voice/ptt_video/scn_subscribe: `-ip target_ip("psp", ctx.sim_ip)`.
- scn_cmp_group_sync: `cmp_ip = target_ip("pmp", "127.0.0.1")` (PMP 미디어 검증).
- VoLTE 시나리오 (volte_voice/volte_video) 무변경.

### 9. CSC CspNotify + PspNotify 분리
**파일**: `csc/config/config_template.json`, `csc/src/services/mcptt.py`
- config_template 에 PspNotify.{Ip,Port} 섹션 추가 (deploy_value @PSP_IP@).
- `_notify_targets(event_type)` — GROUP_CHANGED → PSP, 그 외 → CSP+PSP broadcast (dedup).
- PSP 미설정 (PSP_NOTIFY_IP="") 시 CSP 단독 — legacy 호환.
- `notify_csp` 가 단일 socket 으로 N targets 에 sendto + flow log peer 표시.

### 10. configure.sh 신규 IP 옵션
**파일**: `configure.sh`
- `--psp-ip / --isp-ip / --pmp-ip / --imp-ip` 추가.
- default: PSP/ISP=CSP_IP, PMP/IMP=CMP_IP (legacy 호환).
- env 로 export → config_template @PSP_IP@ 등 치환.

### 11. test_verify_lib 5 entry 적응
**파일**: `tests/test_verify_lib.py`
- step_17/18/19 mock: csp/psp/cmp/pmp/sim 5 모듈 expectations.
- step_21 테스트: CIMS_VERIFY_CMP_WAIT_S=0 으로 시그널링↔미디어 wait 비활성 (단위 테스트는 csp_*.log 없음).
- `port_listening = lambda port, proto="tcp", host="":` 로 시그니처 갱신 (5곳).
- entry_check 테스트: 4-tuple `(port, proto, host, label)` 으로 갱신.

## 검증 결과
- ✓ unit test 161/161 PASS
- ✓ bash -n cims.sh, configure.sh syntax OK
- ✓ ./cims.sh pkg --no-bump → 12 tarball (psp/isp/pmp/imp 포함)
- ✓ ./configure.sh --psp-ip 127.0.0.3 → csc.json 에 CspNotify(127.0.0.1) + PspNotify(127.0.0.3)
- ✓ mcptt._notify_targets 동작 — broadcast/dedup/PSP-only(GROUP_CHANGED)

## LIVE 검증 결과 (10회차 — PSP register 디버깅 후)

**30 PASS / 1 FAIL / 3 SKIP / 299.7초 (5분)**

### PSP register 403 + SUBSCRIBE 디버깅 — 6개 원인 모두 fix
1. **S6-SEED 가 PSP install 시드 누락** — _INSTANCES csp variant 모두 시드
2. **csp_variants filter 가 `dir=="csp"` (PSP dir="psp" 라 누락)** — `tarball in (csp,psp,isp)` 로 변경
3. **cfg_dir 위치 잘못** (csp 의 CspConfigCache jsonlDir = install_path/config, install_path/csp/config 가 아님)
4. **PSP CmpClient 9001 bind fail** (dev csp 충돌) — `Setup.MediaServer.LocalPort=9012` overlay (CSP=9011)
5. **_kill_own_install_listener 가 (deleted) suffix 매칭 실패** — suffix strip 비교
6. **scn_subscribe.py 의 cspsim tail_lines=100 작아 마커 잘림** — cims.sh sim 의 검증 결과 ls 가 100+ 줄 → cspsim 본체의 "Subscriptions complete" 마커가 윈도우 밖. tail_lines=500 으로 확장.

### 4회차 (디버깅 전) → 10회차 (디버깅 후) 비교
| 항목 | 4회차 | 10회차 |
|---|---|---|
| 총 결과 | 28/3/3 | 30/1/3 |
| S6-SCN-PTT-VOICE | FAIL | **PASS** |
| S6-SCN-PTT-VIDEO | FAIL | **PASS** |
| S6-MCPTT-FLOOR-GRANT | SKIP | **PASS** |
| S6-CMP-GROUP-SYNC | SKIP | **PASS** |
| S6-SCN-DB-SYNC | SKIP | **PASS** |
| 잔존 FAIL | — | S6-SCN-SUBSCRIBE (PSP NOTIFY 흐름 — 별도 fix) |
| 시간 | 513초 | 299초 (S5 RUN-START 150s → 6s — PSP 정상) |

### PASS — P1 인프라 검증 완료 ✓
- S1~S4 전체 (13개) PASS
- **S5 전체 PASS** — 5 인스턴스 (csp/psp/cmp/pmp/sim) deployment install + 4 인스턴스 동시 LISTEN + 시그널링↔미디어 두 pair connection wait (153초)
- **S6-ENTRY-CHECK PASS** — 6 host:port 매트릭스 (csc/console/CSP 127.0.0.1:5060/PSP 127.0.0.3:5060/CMP 127.0.0.1:9000/PMP 127.0.0.3:9000) 모두 LISTEN
- **VoLTE voice/video PASS** — CSP/CMP 회귀 0

### FAIL — PTT 시나리오 fine-tuning 영역
- S6-SCN-PTT-VOICE / PTT-VIDEO / CMP-GROUP-SYNC
- 원인: PSP 가 cspsim REGISTER 에 `403 Forbidden, User not registered` 반환
- PSP 의 csp.json 비교 결과 LocalIp 외 dev csp 와 동일 (Roles overlay 정상)
- 추정: mTLS cert 가 PSP 에 발급 안 됐거나, PSP 의 가입자 인증 흐름이 dev csp 와 미세 차이
- 해결은 다음 세션 — PSP 의 register 응답 흐름 디버깅

### LIVE 도중 발견된 추가 fix (커밋 대상)
1. **`kill_stray` port-based kill 이 host 무시** → `_kill_own_install_listener` 추가
   (자기 install 의 `/proc/<pid>/exe` 만 죽임 — PSP 시작 시 CSP 죽이는 회귀 차단)
2. CMP RTP 풀 누적 (verify-cmp-XXX 그룹) — 검증 시작 전 CMP 재시작 필요시 (환경 이슈)

## LIVE 검증 절차 (다음 세션 재현)
```bash
cd /home/nex/work/cims
sudo ip addr add 127.0.0.3/8 dev lo                 # 1회만 (sudo 비번 필요)
./configure.sh --psp-ip 127.0.0.3 --pmp-ip 127.0.0.3
./cims.sh sync scripts csc                          # dist 의 cims.sh + csc src 갱신
./cims.sh restart tb-csc cmp                         # mcptt + RTP 풀 fresh
./cims.sh verify run --preset pipeline-full --enable-mtls
```

**현재 baseline (재현 가능)**: 28 PASS / 3 FAIL (PTT) / 3 SKIP / 8.6분.

## 회귀 위험 (LIVE 시 점검)
1. dist 안 cims.sh 가 새 _start_psp_variant 갖고 있는지 (`./cims.sh sync scripts` 재실행).
2. PSP 의 install_path/csp/config/csp.json 의 LocalIp 가 127.0.0.3 (overlay 적용 확인).
3. PMP 의 cmp.json 의 CspIp = 127.0.0.3 (PSP IP 가리키기 — overlay).
4. step_21 의 csp↔cmp + psp↔pmp 양쪽 connected 모두 잡혀야 PASS.
5. TB-CSC 가 새 mcptt.py 로딩됐는지 (restart tb-csc 필요).

## P2 (다음 세션 의제)
- ISP/IMP 인스턴스 활성 (현재 패키지만 생성).
- ISP IBCF SIP routing — CSP→ISP→외부 PBX 연동.
- IMP RTP relay — ISP 짝.
- entry-check 8 인스턴스 (ISP/IMP 127.0.0.4 추가).
- ISP-specific 시나리오 (외부 트렁크 호 시뮬).
