# prod-multi-host — 상용 reference 환경

상용 배포 reference. 실제 사이트별 IP/도메인/VIP/암호는 운영 시 override.

## 토폴로지

```
Control 망 (svc, mgmt, int)
  ctrl-1 (10.1.0.11)  A/S MASTER  csp + csc + ha
  ctrl-2 (10.1.0.12)  A/S BACKUP
        └ VIP 10.1.0.10 (svc, vrid=51)

Media 망 (media)
  media-1 (10.1.1.21)  AA  cmp
  media-2 (10.1.1.22)  AA

외부 DB (HA pair)
  10.1.255.20:3306

agent 관리망 (mgmt)
  10.1.255.0/24 ─ CSC 10.1.255.10:4419
```

## 사용

1. `env.yaml` 의 IP/암호/CSC URL 을 사이트 값으로 patch.
2. `scenarios/volte-ptt.yaml` 의 도메인/auth_realm 을 사이트 값으로 patch.
3. render → 각 host 에 install (수동 또는 배포 파이프라인).

```bash
cd /home/nex/work/cims/deployment
./bin/render.py --env prod-multi-host --scenario volte-ptt --check-only
./bin/render.py --env prod-multi-host --scenario volte-ptt --out /tmp/prod-bundle
```

`apply.py` 는 현재 NetNS 환경 (build/dist/netns-agents) 매핑만 가정. 상용은 각 host 의 install dir 에 별도 배포 채널 (Ansible / rsync / agent install API) 사용 권장.

## 알려진 한계 (reference 한정)

- VIP `__REPLACE__` 같은 placeholder 가 그대로 deploy 되면 keepalived auth 실패. patch 필수.
- DB 암호도 동일.
- `verify.smoke` 는 비어있음 — 상용은 cspsim 대신 실제 단말로 검증.

## 관련 문서

- [../README.md](../README.md) — deployment/ 전체 안내
- [../tb-netns-4-node/README.md](../tb-netns-4-node/README.md) — NetNS testbed (개발/검증)
- [../_schema/env.schema.md](../_schema/env.schema.md) / [scenario.schema.md](../_schema/scenario.schema.md)
