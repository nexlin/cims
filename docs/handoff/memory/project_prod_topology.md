---
name: 상용 배포 토폴로지 — P1 완료, P2 의제
description: VoLTE/PTT/IBCF 시그널링 + 미디어 6 인스턴스 (CSP/PSP/ISP + CMP/PMP/IMP) 분리. P1 (PSP/PMP) 2026-05-08 구현 완료 (uncommitted). P2 = ISP/IMP 활성 + IBCF 트렁크 호.
type: project
originSessionId: 28af9309-57bc-4b49-ba8f-6a19c4447828
---
# 상용 배포 토폴로지 — 검증 파이프라인 반영 작업

**P1 (PSP/PMP 분리)**: 2026-05-08 후속 세션 구현 완료, uncommitted. 상세 [project_session_2026_05_08_p1.md](project_session_2026_05_08_p1.md).

## 모듈 정의 (사용자 컨펌 — 2026-05-08)
- csp 바이너리는 다용도. Roles 토글로 인스턴스화:
  - **CSP** = CSCF + TAS (VoLTE 가입자 register + B2BUA)
  - **PSP** = CSCF + PTT-AS (PTT 가입자 register + 그룹콜)
  - **ISP** = IBCF (IP-PBX 트렁크 게이트웨이)
- cmp 바이너리도 동일. 짝 미디어:
  - **CMP** = VoLTE RTP relay
  - **PMP** = PTT RTP + Floor
  - **IMP** = IBCF 트렁크 RTP

검증 환경: 단일 PC + loopback IP alias. CSP+CMP=127.0.0.1, PSP+PMP=127.0.0.3, ISP+IMP=127.0.0.4 (P2).

## 사용자가 제시한 목표 토폴로지

| 서버 | 설치 모듈 | 역할 |
|---|---|---|
| **VoLTE SIP Server** | CSP + **ISP** | VoLTE SIP 시그널링 |
| **VoLTE Media Server** | CMP + **IMP** | VoLTE RTP relay/media |
| **PTT SIP Server** | **PSP** | PTT SIP 시그널링 (PTT-AS 분리) |
| **PTT Media Server** | **PMP** | PTT RTP/floor control |
| **CIMS 관리 서버** | CSC + console + phone + cwrtc + simulator | 관리 도구 + UE 시뮬 |

**현재 구조와 차이**:
- 지금은 단일 CSP 가 `Roles` 설정으로 CSCF/TAS/PTT_AS/IBCF 모두 in-process. 한 서버에 csp/cmp/csc/console/phone/cwrtc/cspsim 다 떠있음.
- 상용은 **5개 서버로 분리**. CSP 의 PTT_AS 가 떨어져나와 **PSP** 가 됨. CMP 의 PTT 부분이 **PMP** 로 분리.
- **ISP / IMP** 는 신규 — VoLTE 측에 CSP/CMP 와 별도로 무엇이 있어야 하는지 사용자 설명 추가 필요 (사용자한테 ISP=I... SIP Pro?, IMP=I... Media Provider? 약어 풀이를 받을 것).

## 검증 파이프라인 영향 범위 (예상)

### S4 (패키지)
- 현재 5종 tarball: csc, csp, cmp, sim, console (+ phone, cwrtc, agent 별도)
- 추가 필요: **isp, imp, psp, pmp** 모듈 tarball — 빌드 산출물 정의부터.

### S5 (로컬 배포)
- 현재 S5 는 1단(TB-CSC) → 2단(배포본 csc) → 3단(csp/cmp/sim 모듈) 체인.
- 상용 토폴로지는 **5개 서버 ↔ 모듈 매칭** 이 더 복잡.
  - VoLTE SIP Server: CSP + ISP 두 모듈 deploy
  - VoLTE Media Server: CMP + IMP
  - PTT SIP/Media: PSP / PMP 단독
  - 관리 서버: csc, console, phone, cwrtc, cspsim
- `_native_steps.py` 의 `_MODULES = ("csp", "cmp", "sim")` + `_AGENT_SYNC_PORT_MOD` 확장.
- agent 가 단일 서버에 N 개 모듈을 install 하는 케이스 (CSP+ISP 한 서버) 처리 필요.

### S6 (통합 검증)
- entry-check 가 LISTEN 포트 검사 — 5 서버 IP/포트 매트릭스로 변경.
- `manifest immutability gate` — 5 서버 각각의 deployed-manifest 가 같은 SHA-256 가리키는지.
- 시나리오: VoLTE 호 (UE → CSP → ISP → CMP → IMP), PTT 호 (UE → PSP → PMP) 흐름 모듈 확인.

### 새 검증 항목 후보
- 서버간 mTLS / mutual auth (CSP↔ISP, CMP↔IMP, PSP↔PMP).
- 서버별 health check (cmd_health 분리).
- 분산 deploy 에서 단일 모듈 장애 시 영향 범위 (degraded mode).

## 다음 세션 준비

### 사용자에게 물어볼 것
1. **ISP / IMP / PSP / PMP 약어 풀이** — 각 모듈이 내부적으로 무엇을 하는지 명세.
2. **모듈간 통신 인터페이스** — CSP↔ISP, CMP↔IMP 가 SIP/JSON/Diameter 어느 것?
3. **포트 매트릭스** — 각 서버에서 어느 포트가 LISTEN.
4. **단일 PC 시뮬 vs 다중 PC** — 검증 환경에서 5 서버를 PC 1대에 띄울 건지 (다중 IP/loopback) 또는 docker/VM 분리할지.
5. **롤아웃 단계** — 한 번에 5 서버 분리 vs 점진 (예: PSP/PMP 먼저, ISP/IMP 다음).

### 시작 전 점검
```bash
cd /home/nex/work/cims
git log -25 --oneline      # 최근 커밋 흐름
git status
python3 -m unittest tests.test_verify_lib  # 161 OK 베이스라인
ss -tlnp 2>/dev/null | grep -E ":(4419|4421|4445|5060|9000|9001|3000|3001)" | head
./cims.sh status           # 서비스 상태
```

### 현재 baseline (2026-05-08)
- pipeline-full --enable-mtls 36/36 PASS in 273s (이전 세션)
- 본 세션 (2026-05-08) 작업 — 커밋되지 않은 fix 다수:
  · S1~S6 execution_order 명시
  · configure.sh idempotent guard 버그 fix
  · live_store + /active + V2 auto-attach
  · S5 stale test-agent 정리 + cims_agent ETXTBSY 회피
  · ptt_smoke remove_group 제거 + cmd UPPERCASE
  · flow_logger CSC raw-data 원칙 (method 블랙리스트 제거)
- **다음 세션 시작 시 git status 로 미커밋 변경 확인 필요**.
