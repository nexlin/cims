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

## 알려진 이슈 (Phase 1~4 에서 모두 해소)

| # | 이슈 | 해소 |
|---|---|---|
| 1 | csp 가 mgmt IP 로 bind (VIP 경유 SIP 불가) | `local_nodes.jsonl` 의 is_primary row 로 VIP bind 명시 (render.py) |
| 2 | 모듈 default config IP 가 외부 환경 박힘 | render.py 가 env.yaml 기준 csp.json/cmp.json 자동 생성 |
| 3 | DB 미연결 → service_binding 부재 (403) | `_loadUserFromFile` 에 service_ref/imsi 읽기 + scenario seed (commit `814fe53`) |
| 4 | cmp PTT pool leak | `timeoutLoop` cleanup 보강 (commit `9c032d1`) |
| 5 | ha.json port/proto 누락 | `_render_ha_for_agent` 자동 채우기 (commit `0910c13`) |

## 시나리오 (`scenarios/`)

| 시나리오 | 상태 | 비고 |
|---|---|---|
| [`volte-ptt`](scenarios/volte-ptt.yaml) | ✅ 작성 완료 | VoLTE + PTT 동시. VIP 10.0.1.13 으로 SIP listen + CMP relay |
| [`volte-only`](scenarios/volte-only.yaml) | ✅ 작성 완료 | VoLTE 만 (PTT_AS off). CSCF + TAS 만으로 VoIP B2BUA |
| `full` | placeholder | VoLTE + PTT + IBCF (외부 trunk seed 필요) |

## LIVE 검증 cheat sheet

```bash
# 1) sudo timestamp 갱신 (verify.py 가 netns 진입에 sudo 사용)
sudo -v

# 2) lifecycle.sh 가 최근에 변경됐다면 dist + install 동기화
cp /home/nex/work/cims/agent/lib/lifecycle.sh /home/nex/work/cims/build/dist/agent/lib/
for ns in ctrl-a ctrl-b media-a media-b sim-a; do
  cp /home/nex/work/cims/agent/lib/lifecycle.sh \
     /home/nex/work/cims/build/dist/netns-agents/$ns/install/agent/lib/lifecycle.sh
done

# 3) render + apply (bundle → install dir)
cd /home/nex/work/cims/deployment
./bin/apply.py --env tb-netns-4-node --scenario volte-ptt --dry-run    # plan 확인
./bin/apply.py --env tb-netns-4-node --scenario volte-ptt              # 실제 복사

# 4) 모듈 재시작 (csp/cmp 가 새 설정 로드)
for dep in 27 28 19 20; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d '{"job_type":"restart"}' \
    https://127.0.0.1:4419/api/v1/deployments/$dep/job
done

# 5) verify — listen → smoke → failover
./bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase listen
./bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase smoke
./bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase failover

# 또는 한번에
./bin/verify.py --env tb-netns-4-node --scenario volte-ptt --phase all
```

LIVE 검증 기준 (`smoke` 의 expect):
- `register-ptt` : `Registered : 1/1`
- `register-volte`: `Registered : 1/1` (`-auth_id 450033100000001@ims...` 풀폼 명시 필수 — VoLTE 모드는 auto 유도 안 함)
- `call-ptt-1on1`: `Call OK/End : 2`

`failover` 의 expect:
- ctrl-a keepalived SIGTERM → 6s 후 ctrl-b 가 VIP 10.0.1.13 인수 → followup `register-ptt` 동일하게 PASS

## 관련 문서

- [../README.md](../README.md) — deployment/ 디렉토리 전체 안내
- [../_schema/env.schema.md](../_schema/env.schema.md) — env.yaml 필드 명세
- [../_schema/scenario.schema.md](../_schema/scenario.schema.md) — scenario.yaml 필드 명세
