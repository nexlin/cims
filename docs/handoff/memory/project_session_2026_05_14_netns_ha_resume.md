---
name: session-2026-05-14-netns-ha-resume
description: 2026-05-14 NetNS HA 검증 이어서 진행 가이드. 4-node ns 환경 셋업 완료 + agent 등록부터 차근차근 재개.
metadata: 
  node_type: memory
  type: project
  originSessionId: 8880af93-fd21-4f93-9203-f53de51d8446
---

# 내일 (2026-05-14) NetNS HA 검증 이어서 진행

이전 세션 (2026-05-13) 에 NetNS 환경 + frontend/backend 보완 완료. 내일은 agent 4개 등록부터 차근차근 다시.

## 사전 상태 확인 (세션 진입 즉시)

```bash
# 1. NetNS 환경이 살아있나
sudo ./verify/scripts/ha-netns-status.sh
# → 4 ns + 3 bridge + ping 매트릭스 ✓ 12/12 + multicast ✓ 3/3 이면 OK
# → 사라졌으면: sudo ./verify/scripts/ha-netns-up.sh 다시
```

**Why:** 호스트 재부팅 시 NetNS 사라지므로 첫 확인. ping/multicast 매트릭스가 검증의 기본 신호.

```bash
# 2. TB-CSC 살아있나
ss -tlnp 2>/dev/null | grep ':4419' | head -1
# → 없으면: cd build/dist/csc/src && CIMS_CSC_CONFIG=../config/csc-tb.json nohup python3 csc_app.py > /tmp/tb-csc.log 2>&1 & disown
```

```bash
# 3. file_store agent 영역 비어있나 (어제 정리 완료)
ls /home/nex/work/cims/build/dist/ext_mnt/runtime/agents/ 2>/dev/null
# → 비어있어야 OK. 남아있으면 어제 정리 실패 — 다시 rm.
```

```bash
# 4. Console 접속 가능한지
# Browser: http://192.168.199.129:3000/deploy/services 로그인
```

## 4-node ↔ agent 매핑 (이번에 사용할 이름)

| ns | mgmt | svc | int | Console 에서 만들 시스템 | agent 이름 (자동) |
|---|---|---|---|---|---|
| ctrl-a | 10.0.0.11 | 10.0.1.11 | 10.0.2.11 | `Control-Server` (A/S) | `Control-Server-01` |
| ctrl-b | 10.0.0.12 | 10.0.1.12 | 10.0.2.12 | `Control-Server` (A/S) | `Control-Server-02` |
| media-a | 10.0.0.21 | 10.0.1.21 | 10.0.2.21 | `Media-Server` (AA) | `Media-Server-01` |
| media-b | 10.0.0.22 | 10.0.1.22 | 10.0.2.22 | `Media-Server` (AA) | `Media-Server-02` (수동 추가) |

권장 VIP: CSC=10.0.0.100, CSP=10.0.1.100, PSP=.101, ISP=.102

## Step 1 — Console 에서 시스템 2개 추가

`/deploy/services` 페이지 하단 `＋ 시스템 추가`:

| # | 이름 | 유형 | 결과 |
|---|---|---|---|
| 1 | `Control-Server` | `A/S (자식 2)` | Control-Server-01/02 자동 + ha_group + 두 토큰 자동 발급 (frontend 자동 approve + 30분 TTL) |
| 2 | `Media-Server` | `AA (자식 N)` | Media-Server-01 자동 + ha_group + 1 토큰. **`＋ 서버 추가` 한 번 더** 클릭해서 Media-Server-02 추가 |

각 시스템의 server row 의 **🔧 설치명령** 버튼 → 클릭 시 토큰 신규 발급 + clipboard 자동 복사 + flash 메시지에 `· 30분 내 사용` 표시.

## Step 2 — ns 별 install (각 노드 동일 패턴)

**핵심 — ns 진입 + nex user 컨텍스트 + install command 그대로 (DEV-CSC 라 `--no-systemd` 자동 부여됨)**

ctrl-a 예시 (다른 ns 동일, 이름/IP만 다름):

```bash
# 호스트 nex user shell 에서 — ns 진입 + nex 권한 한 줄
sudo ip netns exec ctrl-a sudo -u nex -i

# (이제 ctrl-a ns 안의 nex shell. 비번 prompt 뜨면 <REDACTED_SUDO_PW>)
# → 프롬프트: nex@nex-ubuntu:~$

# 확인 — IP 가 10.0.0.11 이어야 함 (호스트 10.0.0.1 아님)
ip addr show mgmt | grep "inet 10.0.0"

# 설치 디렉토리
mkdir -p /home/nex/work/cims/build/dist/netns-agents/ctrl-a/install
cd       /home/nex/work/cims/build/dist/netns-agents/ctrl-a/install

# Console 에서 🔧 설치명령 클릭 → 자동 복사된 명령 그대로 붙여넣기
# (DEV-CSC 라 --no-systemd 자동 포함됨)
curl -k https://10.0.0.1:4419/install-agent.sh | bash -s -- \
  --csc-url https://10.0.0.1:4419 \
  --enrollment-token <복사된_토큰> \
  --name Control-Server-01 \
  --no-systemd

# 백그라운드 실행
nohup ./run.sh > ../agent.log 2>&1 &
echo "PID=$!"

# ns 셸 종료 (agent 는 백그라운드 유지)
exit
```

**검증 — source IP 가 10.0.0.11 으로 표시되는지** (호스트 10.0.0.1 가 아니라):
```bash
tail -3 /tmp/tb-csc.log | grep heartbeat
# 정상: "INFO:     10.0.0.11:... POST /api/agent/heartbeat 200 OK"
# 비정상: "INFO:     10.0.0.1:..."  → ns 진입 안 됨, 호스트에서 실행됨
```

ctrl-b / media-a / media-b 도 동일. 각각 IP, 디렉토리, 토큰, agent 이름만 교체.

## Step 3 — Console UI 후속 작업

4 agent 모두 online 확인 후:

1. **각 agent 의 ServiceIpPanel** — 각 NIC ↔ slot 매핑 입력 후 [적용]
   - ctrl: mgmt→CSC, svc→{CSP/PSP/ISP}
   - media: int→CMP/PMP/IMP control, svc→RTP
2. **Control-Server (A/S) 의 VipPanel** — 권장 VIP 입력 + [적용]
   - CSC=10.0.0.100/mgmt, CSP=10.0.1.100/svc, PSP=10.0.1.101/svc, ISP=10.0.1.102/svc
3. **패키지 install** — Control 에 (csc/console/csp/isp/psp/cwrtc/cspsim/phone), Media 에 (cmp/imp/pmp)

## Step 4 — keepalived 수동 기동 (NetNS 특수)

ha.json + keepalived.conf 가 자동 분배되어도 호스트 systemd 가 ns 안 service 못 띄움. 직접 실행:

```bash
for ns in ctrl-a ctrl-b; do
  sudo ip netns exec $ns bash -c "
    KCONF=/home/nex/work/cims/build/dist/netns-agents/$ns/install/agent/keepalived/keepalived.conf
    [[ -f \$KCONF ]] || { echo '[$ns] keepalived.conf 없음'; exit 1; }
    nohup keepalived -P -D -f \$KCONF --pid /tmp/$ns-keepalived.pid > /tmp/$ns-keepalived.log 2>&1 &
    echo \"[$ns] keepalived PID=\$!\"
  "
done

# VIP 인수 확인
sleep 5
sudo ip netns exec ctrl-a ip addr show mgmt | grep '10.0.0.100' && echo "✓ CSC VIP 인수"
```

## Step 5 — VIP fail-over 검증

```bash
# active kill
sudo ip netns exec ctrl-a pkill -KILL keepalived
sleep 5
# standby 가 인수했나
sudo ip netns exec ctrl-b ip addr show mgmt | grep '10.0.0.100' && echo "✓ ctrl-b 인수"
```

## 어제 적용한 개선사항 (모두 반영됨)

- ✅ HaServicesPage 자동 approve — createAgent 후 즉시 status='approved'. heartbeat 30s 안에 자동 online.
- ✅ `🔧 설치명령` 단일 버튼 (옛 📋 복사 + ↻ 토큰 통합) — 클릭 → 토큰 재발급 + 자동 복사 + 만료 안내
- ✅ enrollment_token TTL — `csc-tb.json` `Agent.EnrollmentTokenTtlSec=1800` (30분). 만료 시 enroll 401 응답
- ✅ DEV-CSC 자동 `--no-systemd` — `csc-tb.json` `Server.DevMode=true` 면 install_command 끝에 자동 부여
- ✅ agent name space 처리 — `shlex.quote` 적용, `--name 'Control Server 02'` 도 안전
- ✅ clipboard fallback — HTTP 환경에서도 execCommand 로 복사 동작

## 자주 막힐 포인트 (어제 학습)

| 증상 | 원인 / 해결 |
|---|---|
| `bash: 줄 1: <REDACTED_SUDO_PW>: 명령어를 찾을 수 없음` | `echo '...' \| sudo -S bash` 패턴 — sudo timestamp 캐싱 시 비번이 새 bash stdin 으로 흘러감. **interactive sudo 사용** |
| `ERROR: root 로 실행하지 마세요` | ns 진입 시 root → install-agent.sh 거부. **`sudo ip netns exec <ns> sudo -u nex -i`** |
| heartbeat source IP=10.0.0.1 | ns 진입 안 됨 — 호스트에서 실행. **ns 안 `ip addr show mgmt` 로 10.0.0.X 확인 후 install** |
| status pending → online 전이 안 됨 | (어제 fix 됨) HaServicesPage 가 createAgent 후 approveAgent 호출. 옛 record 면 file_store .json 의 status 직접 수정 |
| Clipboard 권한 없음 | (어제 fix 됨) HTTP 환경에서도 execCommand fallback 동작 |
| install_command 의 URL 이 placeholder | (어제 fix 됨) csc-tb.json Server.PublicUrl=https://10.0.0.1:4419 설정 |

## sudo 비번

[[user_credentials]] — `<REDACTED_SUDO_PW>`. 메모리에만 보관, git 외 영역.

## 관련 파일

- `verify/scripts/ha-netns-up.sh` / `ha-netns-down.sh` / `ha-netns-status.sh` — 환경 셋업
- `verify/scripts/ha-netns-install-agent.sh` — install helper (옵션, 수동 진행 시 미사용)
- `docs/design/ha_design.md §11.8` — NetNS 검증 환경 SoT
- `build/dist/csc/config/csc-tb.json` — TB-CSC config (PublicUrl + DevMode + EnrollmentTokenTtl)
