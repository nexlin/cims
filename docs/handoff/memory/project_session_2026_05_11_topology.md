---
name: 2026-05-11 ISP/IMP 토폴로지 통합
description: ISP/IMP 를 별도 agent 가 아닌 volte-sip-server / volte-media-server 의 sub-dir 공존으로 변경. 멀티-패키지 install + scoped overlay 인프라 구축.
type: project
originSessionId: b25db11b-005b-4c51-b4f6-4ca2e1ded329
---
## 세션 결과

LIVE pipeline-full **35/34 PASS / 0 FAIL / 1 SKIP** in 264s. VoLTE/PTT/IBCF 모든 시나리오 PASS.

commit: `68ad390` (refactor topology). origin/main 동기화 완료.

## 사용자 의도

> ISP, IMP 는 VoLTE SIP Server, VoLTE Media Server 에 각각 설치하는걸로 요청했었는데 IBCF Sip Server 를 따로 만들었네

직전 P2 라운드의 토폴로지가 사용자 의도와 어긋남. 한 server agent 에 csp/isp 또는 cmp/imp 가 sub-dir 공존이 의도였음.

## 최종 토폴로지

```
volte-sip-server/      ← 1 cims_agent + sync_port=9904
  agent/
  csp/  ← CSCF + TAS,    127.0.0.1:5060
  isp/  ← IBCF only,     127.0.0.5:5060
  config/    ← server-level jsonl 시드 위치 (CSP/ISP 모두 여기 읽음)
  log/, run/

volte-media-server/    ← 1 cims_agent + sync_port=9905
  agent/
  cmp/  ← VoLTE media,   127.0.0.1:9000
  imp/  ← IBCF media,    127.0.0.5:9000
  config/
  log/, run/

ptt-sip-server/psp/    (단일 변종)
ptt-media-server/pmp/  (단일 변종)
```

## 핵심 인프라 변경

### A. cims_agent.py — 멀티-패키지 install 지원

**문제 1**: 기존 install 은 install_path 전체를 wipe + tar 풀어 → sibling 변종 (csp install 이 isp/ 디렉토리 wipe) 영향.

**해결**: `_detect_tar_pkg_subdir(tar_path)` 추가. tarball 의 top-level 디렉토리 1개를 찾아 그 sub-dir 만 wipe/backup. cims.sh pkg 산출 tarball 은 항상 단일 root (`<pkg_name>/`).

**문제 2**: `install_path/config.json` (deployment overlay) 가 sibling 간 공유 → last-write-wins 로 첫 변종 overlay 가 덮어써짐 (예: csp 가 isp 의 IBCF role 받음).

**해결**: cfg_target_dir 도 변종별로 분리 (`install_path/<pkg>/config.json`). consumers (cims.sh, csc_app.py) 도 scoped 우선 + legacy fallback.

**문제 3**: CSP/CMP ELF 의 `SipServerSetup.cpp:239` jsonlDir fallback 이 startup 시점에 `install_path/config/` 디렉토리를 찾는데, install 시점에 이 디렉토리가 없으면 `jsonlDir=(none)` 으로 init. 이후 SIGUSR1 reload 도 in-memory jsonlDir 이 빈 채라 재탐색 안 함 → ServiceMap 영구 0 → cspsim REGISTER 403.

**해결**: install 시 `install_path/config/` 와 `install_path/<pkg>/config/` 양쪽 미리 생성. S6-SEED 가 나중에 write 만 수행.

### B. cims.sh — overlay 로딩 fallback

`_start_csp_variant` / `_start_cmp_variant` / `start_csc` / `start_console` 모두 install_path/<pkg>/config.json (scoped) 우선, install_path/config.json (legacy) fallback. 단일-변종 install (dev 모드 등) 와 후방 호환.

### C. csc_app.py — overlay 로딩 fallback

`load_config()` 의 `_apply_overlay` 호출 위치도 동일한 fallback 적용.

### D. _native_steps.step_18 — agent_name 기준 dedup

같은 agent_name 의 변종은 한 cims_agent 프로세스 + sync_port 공유. 첫 인스턴스만 register + Test-agent spawn. 후속 형제 인스턴스는 동일 aid/ta_pid 복사.

`seen_agents: dict` 으로 추적. csp 처리 시 register/spawn → seen_agents 에 캐시 → isp 처리 시 캐시 hit → 같은 aid/pid 사용.

step_22 finalize 의 Test-agent kill 도 `pid_set: set` 으로 dedup.

### E. seed.py — ISP 빈 access_services 쓰기 제거

기존 코드: ISP variant 시드 시 빈 access_services.jsonl 로 덮어쓰기 (이전 회차 잔여 제거 목적).

문제: csp 와 isp 가 같은 install_path/config/ 공유 (server-level seed dir). ISP 의 빈 쓰기가 csp 의 시드를 wipe.

해결: ISP 는 access_services 시드 skip. IBCF role 자체가 CSCF=false 라 access_services 무관. routing_policies 의 rule (`req_uri_host contains "trunk.peer.test"`) 은 VoLTE 호 흐름과 매칭 안 되므로 sharing 무해.

## 디버깅 여정 (회차별)

1. **회차 1 (FAIL: REGISTER 403)** — install_path/config.json 공유로 인한 last-write-wins. CSP 가 ISP overlay 받아 127.0.0.5 로 bind.
2. **회차 2 (FAIL: CSC-VERIFY)** — config.json 위치 변경했으나 step_11/12 가 옛 위치 확인.
3. **회차 3 (FAIL: CSC port 4421)** — csc_app.py 의 overlay 로딩이 legacy 위치만 확인.
4. **회차 4 (FAIL: REGISTER 403, ServiceMap=0)** — `volte-sip-server/config/` 디렉토리가 install 시점에 없어 CSP startup 의 jsonlDir fallback 실패.
5. **회차 5 (PASS!)** — install 시 양쪽 config/ 디렉토리 미리 생성.

가장 비직관적이었던 부분: **fallback 위치는 존재하지만 startup 시점엔 디렉토리 자체가 없어 jsonlDir 이 빈 채로 init → 이후 SIGUSR1 reload 도 무력화**. 동일 binary 가 동일 args 로 시작되는데도 startup 시점의 디렉토리 존재 여부가 영구적인 ServiceMap 상태를 결정.

## 다음 진입 권장

1. B1 — 상용 환경 검증 도구
2. WebRTC / cwrtc 검증
3. S6-SCN-CERT-ROTATE (mTLS, 환경 의존)
