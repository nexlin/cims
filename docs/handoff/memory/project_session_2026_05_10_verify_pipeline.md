---
name: 2026-05-10 세션 — verify pipeline 환경 변동성 (미해결)
description: cims.sh start 의 readlink set -e abort fix + pipeline-full 1회차. S6 환경 변동성 분석은 다음 세션에서 재검토.
type: project
originSessionId: b0494a4a-248b-4aaf-a999-89d6c822fe61
---
# 2026-05-10 세션 — verify pipeline 1차 진단

**상태**: cims.sh fix commit + push (`488259e`). pipeline-full 1회차 21/1/12 (PASS/FAIL/SKIP). S6 환경 변동성 분석은 **신뢰도 낮아 다음 세션에서 처음부터 재검토 필요**.

## 확정 사항

### 1. cims.sh `_kill_own_install_listener` set -e abort fix (commit `488259e`)

**증상**: 사용자가 "S2 실패" 라고 본 것. 실제로는 **S3-START** 가 67ms 안에 fail. `cims.sh start` 헤더만 찍고 종료.

**원인**: 옛 verify 회차의 좀비 cmp/pmp 프로세스가 9000/udp 점유 + `/proc/<pid>/exe` 가 `(deleted)` 상태. `_kill_own_install_listener` 의 `local pid_exe; pid_exe=$(readlink -f /proc/<pid>/exe 2>/dev/null)` 가 `set -euo pipefail` 하에서 readlink 실패 시 abort.

**fix**: `readlink -f` → `readlink` (raw target, deleted 도 그대로 반환) + `|| true`. ` (deleted)` suffix strip 로직은 그대로.

**검증**: `./cims.sh start` 정상, `S3-START` 9.3s PASS, pipeline-full S1~S5 모두 PASS.

### 2. pipeline-full 1회차 결과 (357초)

```
S1 (5/5 PASS) / S2 (2/2 PASS) / S3 (6/6 PASS) / S4 (2/2 PASS) / S5 (6/6 PASS)
S6 첫 시도: ENTRY-CHECK FAIL (배포본 csp 127.0.0.1:5060 미기동)
S6 재실행 (배포본 csp 직접 띄운 후): 9/13 PASS, VOLTE-VOICE/VIDEO만 FAIL
```

**관찰**:
- S5-MODULES-RUN-START detail 에 "volte-sip-server: 127.0.0.1:5060/udp LISTEN (2s) ... CONNECTED" → S5 PASS 시점엔 살아있었음
- S6 진입 전 어느 시점에 배포본 csp 가 silent 종료. 배포본 psp/cmp/pmp 는 살아있음
- VoLTE 시나리오 cspsim 이 `192.168.199.129:5060` (dev csp) 로 INVITE → 5초 timeout 후 status=500
- PTT 시나리오는 `target_ip("psp")` 로 배포본 psp 사용 → PASS

## 분석 미완 — 다음 세션에서 재검토

내가 보고한 분석에 검증 부족 부분 있음. 사용자가 "이상하다" 지적. 특히:

- **DB 공유 → register/contact 충돌** 가설 → 잘못됨 인정. CspUserMap 은 process-local 메모리 (CLAUDE.md). 이미 정정.
- **dev cmp 가 죽었다** → log timestamp 만으로 추정. 실제 process 사망 시점 / 누가 죽였는지 / build-time wipe 여부 등 더 검증 필요.
- **VoLTE FAIL 의 진짜 원인** → dev cmp 사망에 의존한다는 가설은 추가 검증 필요. cspsim 의 server_ip 결정 로직 (`ctx.sim_ip` vs `target_ip("csp")`) 도 의도된 design 인지 bug 인지 명확하지 않음.
- **S5 가 dev process 를 정리해야 하는가** 도 정책 결정 사항 — `pipeline-full` 에서 RESET 분리한 게 의도 (`0b8f767`), dev process stop 도 포함 안 함.

## 다음 세션 시작 명령

```bash
cd /home/nex/work/cims
git log -3 --oneline                       # 488259e (cims.sh fix)
ls verify_runs/2026/05/ | tail -5          # 회차 파일
ls verify_reports/ | tail -5                # 최근 리포트
./cims.sh status                            # 현재 process 상태
ss -ulnp | grep -E ":(5060|9000)\b"         # 현재 LISTEN
```

## 다음 세션 의제 (사용자 결정)

> "검증단계를 차근차근 다시 검토" — 단계별로 무엇을 의도했는지, 현재 동작이 의도와 일치하는지 함께 짚기. 내 결론을 먼저 던지지 말고 사용자와 같이 코드/회차 데이터 보면서 진행.

핵심 질문 (사용자에게 확인하며):
1. pipeline-full 에서 S3 의 dev csp/cmp 와 S5 의 배포본 csp/cmp 가 동시 운용되는 게 의도된 design 인가?
2. VoLTE 시나리오의 `ctx.sim_ip` 사용은 의도인가 bug 인가? (PTT 만 `target_ip("psp")` 사용)
3. 배포본 csp 가 S5 PASS 후 silent 종료하는 빈도와 패턴 — 매번? 산발적?
4. 환경 변동성의 진짜 패턴이 뭔지 회차 데이터부터 다시 시작.

## 미해결

1. 배포본 csp silent death root cause
2. VoLTE 시나리오 target_ip 정책
3. 환경 변동성 안정화 (메모리 백로그 #2)
