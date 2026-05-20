---
name: session-2026-05-14-phase1-refactor
description: 2026-05-14 후반 — Console 인프라/모듈 분리 리팩토링 Phase 1.1/1.2 + 토큰 UX + 패키지 자동등록 + 다수 fix. commit 안 됨. 다음 세션 A/S 정상 설치까지 우선.
metadata: 
  node_type: memory
  type: project
  originSessionId: c16bfb26-d747-49eb-8c39-9a611d78c8d6
---

# 다음 세션 (2026-05-15) 즉시 진입 — A/S 정상 설치 우선

## 핵심 목표

**Control-Server (A/S) 시스템이 정상 동작하는 끝까지 완료** 후 그 다음 작업 진행. 검증 진행하면서 발견되는 문제 근본 해결이 목적.

## 1단계 — 사전 환경 확인 (휘발성 서비스 4 종 + 영구 데이터)

**개발 서버 리부팅 후 진입 시**: NetNS / TB-CSC / Vite dev / 4 agent 모두 휘발 — 다시 띄워야 함.
**영구 보존 (디스크)**: file_store (agents 1~4 record, ha_groups 1~2, packages 1~12, jobs, metrics), install 디렉토리, source code.

```bash
# (1) NetNS 4-node — 재부팅 시 사라짐
sudo ./verify/scripts/ha-netns-status.sh
# 기대: 4 ns / 3 bridge / ping 12/12 ✓ / multicast 3/3 ✓
# 사라졌으면: sudo ./verify/scripts/ha-netns-up.sh

# (2) TB-CSC 4419 LISTEN — 재부팅 시 사라짐
ss -tlnp | grep ':4419'
# 안 떠 있으면:
cd /home/nex/work/cims/build/dist/csc/src
CIMS_CSC_CONFIG=../config/csc-tb.json nohup python3 csc_app.py > /tmp/tb-csc.log 2>&1 & disown
sleep 2 && ss -tlnp | grep ':4419'

# (3) Vite dev (Console UI port 3000) — 재부팅 시 사라짐
ss -tlnp | grep ':3000'
# 안 떠 있으면:
cd /home/nex/work/cims/cims-console
npm run dev -- --mode tb --port 3000 --host > /tmp/vite-tb.log 2>&1 & disown

# (4) 4 agent process — 재부팅 시 사라짐
pgrep -af 'cims_agent.py.*-Server'
# 없으면 → 2단계로 재기동

# (5) file_store 데이터 보존 확인 (재부팅에도 그대로)
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/agents/   # 4 json
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/ha_groups/ # 2 json
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/packages/  # 12 json
```

## 2단계 — 4 agent 재기동 (한 줄)

```bash
sudo bash -c '
for ns in ctrl-a ctrl-b media-a media-b; do
  ip netns exec $ns sudo -u nex bash -c "
    cd /home/nex/work/cims/build/dist/netns-agents/$ns/install &&
    setsid nohup ./run.sh < /dev/null > ../agent.log 2>&1 &
    echo \"[$ns] PID=\$!\"
  "
done
'
```

비번 `<REDACTED_SUDO_PW>`. 4 PID 출력 확인. 잠시 후 (10s) heartbeat 확인:
```bash
tail -30 /tmp/tb-csc.log | grep heartbeat | tail -5
# 10.0.0.11 / 12 / 21 / 22 분포
```

## 3단계 — A/S 정상 설치까지 끝내기 (이번 세션 못 끝낸 부분)

### 3.1 ServiceIpPanel 용도 라벨 입력
브라우저 `/deploy/services` → Control-Server 카드 펼침 → 각 서버 row 의 📡 인터페이스 chip 클릭:
- Control-Server-01 의 svc NIC: 용도="서비스" (한글 가능 — IME-safe input)
- Control-Server-02 의 svc NIC: 용도="서비스" (같은 라벨)
- (선택) mgmt NIC 도 "관리" 라벨

### 3.2 VipPanel VIP 추가
Control-Server 카드의 📡 VIP chip → 펼침:
- `＋ VIP 추가` → 용도 dropdown 에 "서비스" (양 멤버 mask 일치 시) 선택 가능
- VIP IP `10.0.1.` `[100]` `/24` (host 부분만 입력 — prefix 자동)
- `[적용]` 클릭 → update_ha job 큐잉

### 3.3 검증 — agent 가 새 코드로 ha.json + keepalived.conf 생성

⚡ **2026-05-14 후반 추가 — 사전 1회 작업**: NetNS 환경에서 cims-ha 의존성이 install/agent 에 없으면 keepalived.conf 가 생성 안 됨. 재부팅에도 install 디렉토리는 보존되므로 **1회만** 필요:
```bash
for ns in ctrl-a ctrl-b media-a media-b; do
  DEST=/home/nex/work/cims/build/dist/netns-agents/$ns/install/agent
  cp -r /home/nex/work/cims/build/dist/agent/bin "$DEST/"
  cp -r /home/nex/work/cims/build/dist/agent/lib "$DEST/"
  cp -r /home/nex/work/cims/build/dist/agent/systemd "$DEST/"
  cp -r /home/nex/work/cims/build/dist/agent/keepalived/keepalived.conf.tpl "$DEST/keepalived/"
done
# media-a/b 는 keepalived/ 디렉토리가 없을 수 있음. cp -r .../keepalived "$DEST/" 통째로
```

⚡ **host keepalived 충돌 방지** (apt 설치 시 자동 시작됨, NetNS keepalived 와 동일 PID 파일 경로 사용):
```bash
sudo apt-get install -y keepalived  # 미설치 시
sudo systemctl stop keepalived && sudo systemctl disable keepalived
```

```bash
# ha-groups apply 호출 (UI 의 VipPanel [적용] 등가)
curl -sk -X POST -H "Content-Type: application/json" -d '{}' https://127.0.0.1:4419/api/v1/ha-groups/1/apply

# job 결과 확인 — rc=0 이어야
sleep 8
for f in $(ls -t /home/nex/work/cims/build/dist/ext_mnt/runtime/jobs/*.json | head -4); do
  python3 -c "import sys,json; d=json.load(open(sys.argv[1])); print(f'  job {d[\"id\"]}: status={d.get(\"status\")} rc={d.get(\"result_code\")}')" "$f"
done

# ha.json + keepalived.conf 생성 확인 (out/ 안)
find /home/nex/work/cims/build/dist/netns-agents/ctrl-{a,b}/install/agent/keepalived -name "*.conf" -o -name "ha.json" 2>/dev/null

# ha.json 의 local_ip 가 svc 망 (10.0.1.x) 여야 정상 (mgmt 10.0.0.x 면 split brain)
python3 -c "import json; d=json.load(open('/home/nex/work/cims/build/dist/netns-agents/ctrl-a/install/agent/keepalived/ha.json')); print(f'  local_ip={d[\"local_ip\"]} peer_ip={d[\"peer_ip\"]} interface={d[\"interface\"]}')"
```

### 3.4 keepalived 수동 기동 (NetNS 특수 — Step 4)

⚠️ **PID 파일 분리 필수** — `--pid` + `--vrrp_pid` 둘 다 ns 별 분리 안 하면 "daemon is already running" 에러:
```bash
sudo bash -c '
for ns in ctrl-a ctrl-b; do
  KCONF=/home/nex/work/cims/build/dist/netns-agents/$ns/install/agent/keepalived/out/keepalived.conf
  if [[ -f $KCONF ]]; then
    ip netns exec $ns keepalived -P -D -f $KCONF \
      --pid /tmp/$ns-keepalived.pid \
      --vrrp_pid /tmp/$ns-vrrp.pid \
      -l > /dev/null 2>&1 &
    echo "[$ns] keepalived spawned"
  else
    echo "[$ns] keepalived.conf 없음 — ha-groups apply 먼저"
  fi
done
'
# VIP 인수 확인 (5초 대기 — advertise timeout 3.6s)
sleep 6
sudo ip netns exec ctrl-a ip addr show svc | grep '10.0.1.13' && echo "✓ ctrl-a 가 master 로 VIP 인수"
sudo ip netns exec ctrl-b ip addr show svc | grep -c '10.0.1.13'  # 0 이어야 (BACKUP)
# 로그는 host 의 /var/log/syslog 에 들어감: sudo grep -i keepalived /var/log/syslog | tail -20
```

### 3.5 fail-over 검증 (Step 5)

⚠️ **`pkill -KILL keepalived` 양쪽 다 죽임** (NS 격리 안 됨, host PID 공유). PID 파일 기반 단일 kill 사용:
```bash
sudo bash -c '
pid=$(cat /tmp/ctrl-a-keepalived.pid)
vpid=$(cat /tmp/ctrl-a-vrrp.pid 2>/dev/null)
kill -KILL $pid 2>/dev/null
[[ -n $vpid ]] && kill -KILL $vpid 2>/dev/null
'
sleep 5
# ctrl-b 가 인수했어야
sudo ip netns exec ctrl-b ip addr show svc | grep '10.0.1.13' && echo "✓ ctrl-b 가 VIP 인수"
# 주의: ctrl-a 의 VIP 도 남아있음 (KILL 이라 notify_stop 미호출). 실제 운영에서는 노드 다운으로 자동 해소
```

## 4단계 — A/S 완료 후 진행할 작업 (우선순위)

| 순서 | 작업 | 비고 |
|---|---|---|
| 1 | **Media-Server (AA) 동일 패턴** | media-a/b 에 svc 라벨 → VIP → keepalived | 또는 AA 의 VIP 유무 결정 |
| 2 | Phase 1.3 — 시스템 카드 인프라/모듈 분리 (탭/섹션 UI) | 작업 양 중 |
| 3 | Phase 1.4 — 모듈 config 의 NIC/VIP reference dropdown | ModuleConfigModal 의 ip_slot 필드 → NIC/VIP 선택 dropdown |
| 4 | 패키지 등록 + 서비스 매핑 + module install (deployment) | Step 3-A2 ~ 모듈 기동 |
| 5 | 변경 사항 commit (대량) | 아래 §변경 사항 commit 안 됨 |

## 이번 세션 변경 사항 (commit 안 됨 — 다음 세션 정리 + commit)

### Backend `csc/src/handlers/agents.py` (source + build/dist sync)
- `_delete_agent` 에 ha_group.members cascade 추가 (dangling 방지)
- `_regenerate_token` 신설 — id 보존, 만료 전 차단 409. 라우팅 `POST /api/v1/agents/<id>/regenerate-token`
- `_get_install_command` 신설 — 기존 토큰 재복사용. 라우팅 `GET /api/v1/agents/<id>/install-command`
- `_agent_to_json` 에 `enrollment_token_expires_at` 노출
- **virtual scan 제거** (`_scan_dist_virtual_packages` / `_dist_root_for_packages` 삭제) — `/deploy/packages` 가 file_store 정식 등록만 표시
- `_register_packages_from_dist` 신설 — DevMode 전용. `build/dist/packages/*.tar.gz` 일괄 file_store 등록. 라우팅 `POST /api/v1/packages/register-from-dist`

### Agent `agent/cims_agent.py` (source + dist + 4 ns install dir 모두 sync)
- `_resolve_install_path` 에 **cwd fallback** — params.install_path 가 쓰기 불가 시 (`/opt/cims` 권한 없음 시) cwd 사용. dev/netns 환경 대응

### Frontend `cims-console/src/api/deployment.ts`
- `Agent.enrollment_token_expires_at` 필드 추가
- `regenerateToken(id)` / `getInstallCommand(id)` / `registerPackagesFromDist()` API

### Frontend `cims-console/src/pages/HaServicesPage.tsx`
- 통합 `handleInstallCmdClick` — 토큰 유효 시 복사, 만료 시 재발행
- 1분 단위 `setMinuteTick` re-render (만료 카운트다운)
- `isTokenValid` / `minutesLeft` helpers
- **Standalone 시스템 단일 row UI** — 그룹 카드 X, ServerRows 하나만. 시스템 이름 = agent 이름
- `MODE_LABEL.standalone: 'Standalone' → 'SA'` / `MODE_TOOLTIP` 추가 / `ModeBadge` 에 title + cursor:help
- Service header row: ❯ chevron + 90° rotate 토글 + flex 정렬 (각 row 의 번호 위치 일치)
- **ServiceIpPanel** — 패키지 의존 제거. NIC 자체 정보 (이름/IP/mask) 만으로 표시. 사용자 자유 입력 라벨
- **VipPanel** — 패키지 의존 제거. slot dropdown 옵션 = ServiceIpRow 의 사용자 정의 라벨만. 멤버별 IP 일치 검증 + 진행 상황 표시 + subnet prefix 자동
- **VIP IP input 분리** — `10.0.0.` (회색) + host input + `/24` (회색). 비표준 mask 면 full input fallback
- VipPanel 멤버 컬럼 readonly 표시 (`iface (ip/mask)`) — dropdown 제거
- **`ImeSafeInput`** 컴포넌트 — 한글 입력 시 IME composition 깨짐 방지 (compositionend/blur/Enter 시점에만 commit)
- `splitPrefixHost(ip, mask)` top-level helper (/8 /16 /24 지원)

### Frontend `cims-console/src/pages/ServicesPage.tsx`
- release job 완료 watcher 가 success 시 `registerPackagesFromDist()` 자동 호출 (DevMode 만 동작 — Prod 는 메뉴 차단)

### Backend `csc/src/handlers/ha_groups.py` (2026-05-14 후반 — A/S split brain 근본 fix)
- `_iface_ip(agent_row, iface_name)` helper 신설 — agent.interfaces[] 에서 iface 매칭 IP 추출
- `_render_ha_for_agent` 의 `local_ip`/`peer_ip` 가 **interface 매칭 IP** 사용 (옛: `agent.ip_address` 항상 = mgmt 망 → interface=svc 인데 unicast_src_ip 가 mgmt → VRRP 광고 송신 svc 망 안 통함 → split brain)
- `_enqueue_update_ha_for_members` 가 agent dict 에 `interfaces` 필드 포함 (위 fix 가 동작하려면)
- LIVE 검증 완료: ha-groups apply → ha.json local_ip=10.0.1.11/peer=10.0.1.12 (svc 망) → keepalived 양쪽 BACKUP 진입 → ctrl-a 만 MASTER → VIP 10.0.1.13 인수 ✓
- fail-over LIVE 검증: ctrl-a PID 단일 kill → 5s 후 ctrl-b 가 VIP 인수 ✓ (단 ctrl-a 의 keepalived `kill -KILL` 시 dangling VIP — 운영 시 노드 다운으로 자동 해소)

## 5단계 — 정리 (commit) 다음 세션 끝에

위 변경사항을 의미별로 분리 commit:
- backend agents.py (토큰/cascade/regenerate/register-from-dist/virtual-scan 제거)
- agent cims_agent.py (cwd fallback)
- backend ha_groups.py (split brain fix — interface 매칭 IP)
- frontend HaServicesPage (Standalone/모드 라벨/ServiceIpPanel/VipPanel/ImeSafeInput)
- frontend ServicesPage + deployment.ts (자동등록 통합)

## 자주 막힐 포인트 (이번 세션 발견)

| 증상 | 원인 / 해결 |
|---|---|
| `nohup: './run.sh' 명령 실행 실패: 그런 파일이나 디렉터리` | install 명령 (curl…\|bash) 실행 안 하고 setsid 부터 시도. **install 명령 먼저 실행** |
| update_ha job rc=2, "Permission denied: '/opt/cims'" | nex user 가 /opt/cims 권한 없음. **agent cims_agent.py 의 cwd fallback 이미 적용**. 다음 세션 진입 시 agent 재기동만 하면 OK |
| 한글 입력 시 누락 | `ImeSafeInput` 컴포넌트 사용 — 이미 ServiceIpPanel 의 용도 input 에 적용 |
| VipPanel 화면 안 나옴 | 옛: 패키지의 vip-scope slot 없으면 panel 차단. 이번 세션 해제. 다음 세션엔 그냥 보임 |
| 시스템 추가 시 record id=3 → id=7 변화 | 옛 버그 — 🔧 설치명령 버튼이 delete+create. 이번 세션에서 fix (regenerate-token endpoint) |
| build/dist/csc/src 가 source 와 stale | source 수정 후 dist 에 cp + __pycache__ 정리 필수 |
| keepalived **split brain** (양쪽 다 VIP 인수) | 옛: ha.json local_ip=10.0.0.11 (mgmt) + interface=svc → VRRP 광고 못 도달. ha_groups.py 의 `_render_ha_for_agent` fix 적용 (2026-05-14). 다음 세션엔 자동 정상 |
| keepalived `daemon is already running` | NS 별 PID 파일 분리 필요. `--pid /tmp/$ns-keepalived.pid --vrrp_pid /tmp/$ns-vrrp.pid` 둘 다 명시 |
| `pkill -KILL keepalived` 양쪽 다 죽음 | pkill 은 PID namespace 인식 안 함 — host 의 모든 keepalived 죽임. fail-over 시엔 **PID 파일 cat → kill** 사용 |
| cims-ha 의존성 (bin/, lib/, systemd/, keepalived/tpl) 미설치 | NetNS install 환경에서 agent 패키지가 keepalived/ 만 가져옴. `build/dist/agent/{bin,lib,systemd,keepalived}` 를 install/agent/ 로 cp 필요 (재부팅 시 install 디렉토리는 보존되므로 1회만) |
| host keepalived service 가 NetNS keepalived 와 충돌 | `sudo systemctl stop keepalived && sudo systemctl disable keepalived` (1회만) |
| keepalived.conf 경로 — out/ 안에 생성 | `install/agent/keepalived/out/keepalived.conf` (옛 메모리의 `keepalived/keepalived.conf` 오기) |

## 관련 메모리

- [[user_credentials]] — sudo 비번 `<REDACTED_SUDO_PW>`
- [[project_session_2026_05_14_netns_ha_resume]] — 이번 세션 진입 시 가이드 (옛 SoT)
- [[project_db_external]] — DB 외부 위임 결정 (불변)
