---
name: project-session-2026-05-17-walkthrough
description: 2026-05-17 hands-on walkthrough 회기 — 단계 1~4 LIVE 검증 + 11 patch + CSC config server 아키텍처 결정
metadata: 
  node_type: memory
  type: project
  originSessionId: c6ff1f28-72b3-4642-8b21-90ea35b33861
---

# 2026-05-17 — hands-on walkthrough + 아키텍처 결정

**목표**: 사용자가 console + 터미널로 단계 1~5 수동 진행, LIVE 회기능 검증 + UX gap 발견 + 즉시 patch.

**진행 결과**:
- 단계 1, 2, 3 완료
- 단계 4 부분 완료 (시스템 설정만)
- 단계 5 (호 시험) **별도 회기로** — CSC config server 아키텍처 작업 후

## 적용된 patch 11건 (LIVE 검증 완료)

| # | task | 내용 | commit 대기 |
|---|---|---|---|
| 1 | #6 | install-agent.sh DevMode 자동 기동 (--no-systemd 분기 + nohup run.sh) | ✓ |
| 2 | TS | AgentCreateResult.enrollment_token_expires_at 타입 좁힘 (prod tsc 통과) | ✓ |
| 3 | #7 | 권한 격리 — agent nex EUID + agent/bin/cims-priv wrapper + sudoers NOPASSWD 화이트리스트 | ✓ |
| 4 | #7 | cims_agent.py 의 ip addr add → sudo -n cims-priv ip-add (with `_resolve_cims_priv()` dev dist canonical 우선) | ✓ |
| 5 | #7 | cims-priv 의 "File exists" + "Address already assigned" 둘 다 SKIP 처리 (kernel 메시지 다양) | ✓ |
| 6 | #8 | [적용] 결과 UX — agents.py 에 GET /agents/{aid}/jobs/{jid} endpoint + AgentJob TS 타입 + HaServicesPage applyServiceIp 에 polling + 결과 기반 flash (OK/SKIP/DENY/FAIL counts) | ✓ |
| 7 | UX | ServiceIp/Vip 입력 중 status='unknown' (isChanged / dirty 플래그) — 적용 전 'down' 즉시 표시 회피 | ✓ |
| 8 | #9 | install-agent.sh 정공법 — CSC `/agent-bundle.tar.gz` endpoint + install-agent.sh 가 tarball download + extract → install_path/agent/{bin,lib,keepalived,systemd}/ 다 깔림 | ✓ |
| 9 | #7+ | cims-ha 도 sudoers + `--ha-dir` 옵션 + cims_agent.py 가 `_resolve_cims_ha()` 로 dev dist canonical 우선 호출 | ✓ |
| 10 | #13 | config_template 에 `scope?: 'system' \| 'service'` 메타 추가 (TS 타입 + JSON). csp template 17 entries × 모든 위치 (source/dist/INSTALL_DIR + file_store packages) 채움 | ✓ |
| 11 | #13 | ModuleConfigEditor 의 `source.type='group'` 지원 (fetch 첫 멤버, save 양쪽 PUT). ModuleConfigModal 의 scope=service collection 🔒 + section 자동 숨김 | ✓ |

## 발견 — 아키텍처 결정 트리거

walkthrough 진행 중 사용자 우려 점차 명확해짐:

1. **HA 그룹 멤버 설정 정합** — preset 만 그룹 단위 가능하면 부족, collection 도 동시 적용 필요
2. **시스템 vs 서비스 분리** — 멤버 specific (local_nodes) vs 그룹 공통 (access_services 등) 명확 분리 의도
3. **재기동 정책** — 서비스 설정은 SIGUSR1 reload, 시스템 설정은 재기동 — 현 UX 가 둘 다 재기동 요구 → 부적합
4. **csp 설정 정리 부족** — csp.json + collections 두 source 정합 어려움 → 호 시험 불가

→ **결정**: walkthrough 단계 5 (호 시험) skip + **CSC config server 아키텍처** 별도 회기.

## 미완 보류 (commit 대기)

- 모든 patch 적용됐지만 git commit 안 됨 — walkthrough 회기 끝에 일괄 commit 권장
- 또는 아키텍처 회기 전에 안정화 commit (시스템 설정만 동작하는 상태 baseline)

## 다음 회기 진입 — task #16 (CSC config server 아키텍처)

상세 plan: [[project_csc_config_server_track]] (작성 예정)

## 관련 task (이번 회기 끝 기준)

| # | subject | 상태 |
|---|---|---|
| 6 | install-agent.sh DevMode 자동기동 | ✅ 완료 |
| 7 | 권한 격리 cims-priv | ✅ 완료 |
| 8 | [적용] 결과 polling | ✅ 완료 |
| 9 | install-agent.sh tarball bundle | ✅ 완료 |
| 13 | config_template scope 메타 | ✅ 완료 (Phase 1+2 + 부분 Phase 3) |
| 10 | NetNS-aware keepalived (dev fail-over) | 대기 |
| 11 | HaServicesPage install/restart + 그룹 설정 통합 | 대기 |
| 12 | install 전 "설정" 버튼 UX gap | 대기 |
| 14 | agent update_config 후 SIGUSR1 (live reload) | 대기 |
| 15 | GroupServiceConfigModal 활성화 | 대기 (별도 트랙) |
| **16** | **CSC config server 아키텍처** | **대기 — 다음 회기 진입점** |

## 명령 cheat sheet (walkthrough 중 발견)

```bash
# clean slate (file_store agents + ha_groups + deployments wipe, .seq 보존, NetNS 네트워크 유지)
RT=/home/nex/work/cims/build/dist/ext_mnt/runtime
for d in agents ha_groups deployments instances; do
  find $RT/$d -maxdepth 1 -name "*.json" ! -name ".seq" -delete 2>/dev/null
done

# agent 재기동 (nex EUID, NetNS 진입)
sudo pkill -f "/cims_agent.py.*--name Control-Server-"
sleep 1
sudo ip netns exec ctrl-a sudo -u nex bash -c '
  cd /home/nex/work/cims/build/dist/netns-agents/ctrl-a && \
  nohup ./run.sh > agent.log 2>&1 < /dev/null &'
sudo ip netns exec ctrl-b sudo -u nex bash -c '
  cd /home/nex/work/cims/build/dist/netns-agents/ctrl-b && \
  nohup ./run.sh > agent.log 2>&1 < /dev/null &'

# 토큰 강제 재발급 (still_valid 우회)
RT=/home/nex/work/cims/build/dist/ext_mnt/runtime
for id in 11 12; do
  python3 -c "
import json
p='$RT/agents/$id.json'
d=json.load(open(p))
d['enrollment_token_expires_at']='2020-01-01T00:00:00'
json.dump(d,open(p,'w'),indent=2,ensure_ascii=False)"
done
curl -sk -X POST https://192.168.199.129:4419/api/v1/agents/11/regenerate-token | python3 -m json.tool

# job 결과 조회 (이번 patch 로 가능해짐)
curl -sk https://192.168.199.129:4419/api/v1/agents/11/jobs/<jid>

# CSC 재기동
pkill -f csc_app.py; sleep 1
(cd /home/nex/work/cims/build/dist/csc/src && \
  CIMS_CSC_CONFIG=/home/nex/work/cims/build/dist/csc/config/csc-tb.json \
  nohup python3 csc_app.py > /tmp/csc.log 2>&1 < /dev/null & disown)
```

## 환경 상태 (회기 끝)

- ctrl-a/b agent 살아있음 (nex EUID, PID 1695914/1695923, status=online)
- HA group "Control-Server" (id=4) + ServiceIp 일부 적용 (svc 10.0.1.14 LIVE 부여)
- VIP 미적용 (keepalived NetNS-aware 한계)
- csp deployment 36/37 installed, status=stopped (기동 안 함 — config 부족)
- /etc/sudoers.d/cims-agent 설치됨 (cims-priv + cims-ha NOPASSWD)
- 모든 patch dist + INSTALL_DIR + file_store packages 동기화 완료
