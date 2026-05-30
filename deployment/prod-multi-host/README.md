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

`apply.py` 는 `<base>/csp`, `<base>/cmp` install dir 에 config 를 복사. 상용은 각 host (ctrl01/ctrl02/media01/media02) 로의 바이너리/패키지 전달을 agent install API (Console → deployment install job) 가 담당.

## 알려진 한계 (reference 한정)

- VIP `__REPLACE__` 같은 placeholder 가 그대로 deploy 되면 keepalived auth 실패. patch 필수.
- DB 암호도 동일.
- `verify.smoke` 는 비어있음 — 상용은 cspsim 대신 실제 단말로 검증.

## 관련 문서

- [../README.md](../README.md) — deployment/ 전체 안내
- [../dev-single-host/README.md](../dev-single-host/README.md) — 단일 host 로컬 smoke
- [../_schema/env.schema.md](../_schema/env.schema.md) / [scenario.schema.md](../_schema/scenario.schema.md)
