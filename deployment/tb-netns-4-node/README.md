# tb-netns-4-node — Test Bed (NetNS 4 node)

단일 host 위에 `ip netns` 4개로 4 node 환경을 구성한 검증 testbed. Control-Server A/S + Media-Server AA 구조.

## 토폴로지

```
host
├── br-cims-mgmt  10.0.0.0/24   (관리망 — agent ↔ CSC, VRRP multicast)
├── br-cims-svc   10.0.1.0/24   (서비스망 — SIP / RTP, VIP 10.0.1.13)
└── br-cims-int   10.0.2.0/24   (내부망 — CMP control / sync)

NetNS:
  ctrl-a    [ha_group 1, master]    mgmt=10.0.0.11  svc=10.0.1.11  int=10.0.2.11
  ctrl-b    [ha_group 1, backup]    mgmt=10.0.0.12  svc=10.0.1.12  int=10.0.2.12
  media-a   [ha_group 2, AA]        mgmt=10.0.0.21  svc=10.0.1.21  int=10.0.2.21
  media-b   [ha_group 2, AA]        mgmt=10.0.0.22  svc=10.0.1.22  int=10.0.2.22

HA Groups:
  1  Control-Server   active_standby   ctrl-a (M) / ctrl-b (B)   VIP 10.0.1.13 / vrid 51
  2  Media-Server     all_active       media-a / media-b         VIP 없음
```

## 셋업 / 운용

```bash
# 셋업 (재부팅마다)
sudo ./verify/scripts/ha-netns-up.sh

# 상태 확인
sudo ./verify/scripts/ha-netns-status.sh
# 기대: 4 ns / 3 bridge / ping 12/12 ✓ / multicast 3/3 ✓

# 4 agent 기동
sudo bash -c '
for ns in ctrl-a ctrl-b media-a media-b; do
  ip netns exec $ns sudo -u nex bash -c "
    cd /home/nex/work/cims/build/dist/netns-agents/$ns/install &&
    setsid nohup ./run.sh < /dev/null > ../agent.log 2>&1 &
  "
done
'

# keepalived (ctrl-a/b)
sudo bash -c '
for ns in ctrl-a ctrl-b; do
  KCONF=/home/nex/work/cims/build/dist/netns-agents/$ns/install/agent/keepalived/out/keepalived.conf
  ip netns exec $ns keepalived -P -D -f $KCONF \
    --pid /tmp/$ns-keepalived.pid --vrrp_pid /tmp/$ns-vrrp.pid -l > /dev/null 2>&1 &
done
'
```

## 한계 / 알려진 이슈

| # | 이슈 | 영향 | 회피 |
|---|---|---|---|
| 1 | csp 가 `Setup.Sip.LocalIp=0.0.0.0` 무시하고 mgmt IP 로 bind | VIP 경유 SIP 트래픽 수신 불가 → fail-over LIVE 의미 제한 | scenario.yaml 의 `local_nodes` 명시로 우회 시도 중 (Phase 2~3) |
| 2 | 모듈 default config IP 가 외부 환경 (`192.168.199.129`) 박힘 | start 시 bind 실패 | generator 가 env.yaml 기준 자동 patch (Phase 4) |
| 3 | DB 미연결 → service_binding 부재 | 호 시도 시 403 | `subscribers.source=file` 로 시나리오에서 seed |

## 시나리오 (`scenarios/`)

| 시나리오 | 상태 | 비고 |
|---|---|---|
| [`volte-ptt`](scenarios/volte-ptt.yaml) | ✅ 작성 완료 (Phase 3) | VoLTE + PTT 동시. VIP 10.0.1.13 으로 SIP listen + CMP relay |
| `volte-only` | placeholder | VoLTE 만 |
| `full` | placeholder | VoLTE + PTT + IBCF |

## 검증 흐름 (수동 — 시나리오 없이)

자율 검증 결과는 [project_session_2026_05_14_deploy_verify.md](../../../.claude/projects/-home-nex-work-cims/memory/project_session_2026_05_14_deploy_verify.md) 의 §"명령 cheat sheet" 참조.

## 관련 문서

- [../README.md](../README.md) — deployment/ 디렉토리 전체 안내
- [../_schema/env.schema.md](../_schema/env.schema.md) — env.yaml 필드 명세
- [../_schema/scenario.schema.md](../_schema/scenario.schema.md) — scenario.yaml 필드 명세
