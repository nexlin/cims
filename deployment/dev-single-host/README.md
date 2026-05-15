# dev-single-host — 단일 host 로컬 환경

개발자 워크스테이션에서 csc/csp/cmp 를 loopback (`127.0.0.1`) 위에 동거시키는 최소 환경. NetNS 없이 그냥 build/dist 에서 직접 실행 가능.

## 토폴로지

```
host (loopback 127.0.0.1)
  ├ csp  127.0.0.1:5060/25061/5061
  ├ cmp  127.0.0.1:9000
  └ cspsim (로컬 호출)
```

ha_group=1 `Standalone` mode — VIP 없음.

## 사용

```bash
cd /home/nex/work/cims/deployment

# render (bundle 생성만 — apply 없이도 가능)
./bin/render.py --env dev-single-host --scenario smoke --out /tmp/dev-smoke
ls /tmp/dev-smoke/self/

# 직접 적용 (옵션) — 로컬 build/dist 의 csp/cmp 디렉토리에 복사
cp /tmp/dev-smoke/self/csp.json /home/nex/work/cims/build/dist/csp/config/csp.json
cp /tmp/dev-smoke/self/config/*.jsonl /home/nex/work/cims/build/dist/csp/config/
cp /tmp/dev-smoke/self/user/*.json    /home/nex/work/cims/build/dist/csp/user/
cp /tmp/dev-smoke/self/cmp.json /home/nex/work/cims/build/dist/cmp/config/cmp.json
```

(apply.py 는 NetNS 의 `netns-agents/<node>/install/` 경로를 가정. dev 는 한 디렉토리만 갱신하면 됨.)

## 시나리오

| 시나리오 | 비고 |
|---|---|
| [`smoke`](scenarios/smoke.yaml) | VoLTE REGISTER + 1대1 호. CSCF + TAS 만 활성 |
