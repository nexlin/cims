---
name: session-2026-05-15-deployment-scaffold
description: 2026-05-15 — deployment/ 디렉토리 신설 (Phase 1~3 완료) + 이슈
metadata: 
  node_type: memory
  type: project
  originSessionId: 1dc43bf8-57c0-434a-a796-ac65292b07a5
---

# 2026-05-15 — deployment/ 신설 + 이슈 #1 fix

## TL;DR

- **deployment/** 신규 디렉토리 — 검증/상용 공통 SoT (배포 환경별)
- **Phase 1~3 완료** — env/scenario schema + tb-netns-4-node/env.yaml + scenarios/volte-ptt.yaml
- **이슈 #1 (csp VIP bind) 해소** — `config/local_nodes.jsonl` 에 VIP primary row 작성 + `ip_nonlocal_bind=1` → ctrl-a/b 모두 10.0.1.13 로 SIP UDP 5060 / TCP 25061 / TLS 5061 LISTEN ✓
- 남은: 이슈 #2 (default config IP — generator 자동화), 이슈 #3 (service_binding 부재 — 호 시 403)

## deployment/ 디렉토리

```
deployment/
├── README.md                          전체 안내 + 컨벤션
├── _schema/
│   ├── env.schema.md                  env.yaml 필드 명세
│   ├── scenario.schema.md             scenario.yaml 필드 명세 + auto/override 패턴
│   └── csp-layers.md                  ⭐ CSP 10 entity layer 모델 + R1 우선순위 + 이슈 #1 fix 흐름
├── tb-netns-4-node/
│   ├── env.yaml                       3 bridge + 4 node + 2 ha_group
│   ├── README.md
│   └── scenarios/
│       └── volte-ptt.yaml             278 lines (auto:true 패턴 + verify entry)
├── dev-single-host/                   placeholder
├── prod-multi-host/                   placeholder
└── bin/                               (Phase 4 generator 자리)
```

### 설계 핵심
- **환경 디렉토리 = SoT 단위**. 한 환경의 인프라(env.yaml) + 시나리오들(scenarios/*) 이 한 곳
- **`auto: true` 패턴** — env 로부터 local_nodes/remote_nodes/cmp_config 자동 유도. `overrides:` 로 환경별 변종 가능
- **검증/상용 공통** — 같은 디렉토리 구조. prod 도 추가 시 동일 패턴

### Phase 진행 상태
| Phase | 내용 | 상태 |
|---|---|---|
| 1 | 환경 카탈로그 + schema | ✅ |
| 2 | CSP layer 모델 (csp-layers.md) | ✅ |
| 3 | tb-netns-4-node × volte-ptt 시나리오 | ✅ |
| 4 | `bin/render.py` generator | ✅ commit `94ddd3d` |
| 5 | 검증 자동화 (`bin/verify.py` + `bin/apply.py`) | ✅ commit `a625bef` |

### Phase 4 — `deployment/bin/render.py` (commit `94ddd3d`)

단일 파일 + PyYAML + stdlib. CLI: `./render.py --env <env> --scenario <scn> [--out <dir>] [--check-only]`

출력 (`<out>/<node>/`):
- CSP 노드: `csp.json` + `config/*.jsonl` (9종) + `user/<sip_id>.json`
- CMP 노드: `cmp.json`
- `manifest.json` (counts/노드 목록)

자동 유도 규칙:
- `local_nodes.auto`: A/S→VIP UDP/TCP/TLS 3 row, AA→멤버별 row, standalone→첫 멤버 svc IP
- `remote_nodes.auto_cmp`: cmp ha_group 멤버별 endpoint
- `routes/route_sets/routing_policies/rules/rule_sets/acl_policies`: yaml 그대로 + 참조 무결성 검증

회기 회귀 (tb-netns-4-node × volte-ptt LIVE 와 diff): 모든 파일 의미적 일치. 차이 2건은 의도:
- access_services.inbound_policy: yaml `default-inbound` (정답) vs LIVE 임시값 `open`
- cmp.json.SystemId: render `cmp_media-a` (노드별 unique) vs LIVE `cmp_01` (두 노드 동일)

부수:
- env.yaml 에 `service_logging.dir/enable/enable_for_cmp/recording` 추가 (host 절대경로 SoT)
- scenario.yaml 에 `setup.monitor/security` override 노출 (선택)
- schema doc + README 갱신

## CSP layer 모델 (csp-layers.md 핵심)

CSP 의 설정은 **10 entity** — `csp/CspConfigCache.cpp` 의 `kEntityName` 배열:
- Layer 0: `csp.json` (`Setup.*` = _infra)
- Layer 1: `local_nodes.jsonl` ⭐ R1 (is_primary=true 가 SOT)
- Layer 2: `remote_nodes.jsonl`
- Layer 3: `access_services.jsonl`
- Layer 4-6: `routes / route_sets / routing_policies.jsonl`
- Layer 7-9: `rules / rule_sets / acl_policies.jsonl`

**R1 우선순위** (CspServer.cpp:121-160):
```
local_nodes 의 enabled=true & is_primary=true & protocol=UDP row
  ├─ bind_ip 가 "0.0.0.0"/""/":""→ GetLocalIp() 자동 치환 (host default route 의 outgoing NIC)
  └─ → gclsSetup.m_strLocalIp / m_iUdpPort 덮어씀
없으면 → _infra Setup.Sip.LocalIp / UdpPort 그대로
```

⚡ **어제 이슈 #1 의 진짜 원인**: `local_nodes.jsonl` 비어있어서 _infra fallback → SIP stack 의 0.0.0.0 → mgmt NIC 매핑.

**fix**: VIP 명시 primary row + ip_nonlocal_bind=1.

## 이슈 #1 fix 적용 — 실제 수행 명령 (재현 가능)

⚠ **csp 가 읽는 jsonlDir**: `install/modules/csp/0.0.1/CSP/config/` (CSP 디렉토리의 `config/` 서브 — `CSP/csp/config/` 아님 — 이게 어제 fix 1차 실패 원인)

```bash
# (1) local_nodes.jsonl 작성 (ctrl-a / ctrl-b 동일)
for ns in ctrl-a ctrl-b; do
  DST=/home/nex/work/cims/build/dist/netns-agents/$ns/install/modules/csp/0.0.1/CSP/config/local_nodes.jsonl
  cat > "$DST" <<'EOF'
{"id":"csp-main-udp","name":"csp-main-udp","edge":"access","bind_ip":"10.0.1.13","bind_port":5060,"protocol":"UDP","thread_count":2,"enabled":true,"is_primary":true,"tags":[],"note":"VIP UDP"}
{"id":"csp-main-tcp","name":"csp-main-tcp","edge":"access","bind_ip":"10.0.1.13","bind_port":25061,"protocol":"TCP","thread_count":2,"enabled":true,"is_primary":true,"tags":[],"note":"VIP TCP"}
{"id":"csp-main-tls","name":"csp-main-tls","edge":"access","bind_ip":"10.0.1.13","bind_port":5061,"protocol":"TLS","thread_count":2,"enabled":true,"is_primary":true,"tls_cert_path":"cert/csp.pem","tags":[],"note":"VIP TLS"}
EOF
done

# (2) ip_nonlocal_bind=1 — BACKUP 노드도 VIP bind 가능해야 fail-over 즉시 처리
sudo sysctl -w net.ipv4.ip_nonlocal_bind=1
sudo ip netns exec ctrl-a sysctl -w net.ipv4.ip_nonlocal_bind=1
sudo ip netns exec ctrl-b sysctl -w net.ipv4.ip_nonlocal_bind=1

# (3) csp restart
for dep in 27 28; do
  curl -sk -X POST -H "Content-Type: application/json" \
    -d '{"job_type":"restart"}' \
    https://127.0.0.1:4419/api/v1/deployments/$dep/job
done

# (4) 검증
sudo ip netns exec ctrl-a ss -tulnp | grep csp
# 기대: 10.0.1.13:5060 UDP / 25061 TCP / 5061 TLS 모두 ✓
```

LIVE 검증 결과:
```
ctrl-a: 10.0.1.13:5060 UDP / 10.0.1.13:25061 TCP / 10.0.1.13:5061 TLS ✓
ctrl-b: 동일 ✓ (BACKUP 도 nonlocal_bind 로 bind)
keepalived: ctrl-a MASTER (VIP 보유)
csp 로그: "primary local_node 'csp-main-udp' → LocalIp=10.0.1.13 UdpPort=5060" ✓
```

REGISTER 시도 (svc 망 media-a 안에서 cspsim):
```bash
sudo ip netns exec media-a sudo -u nex bash -c '
cd /home/nex/work/cims/build/dist/cspsim
./bin/cspsim -server_ip 10.0.1.13 -local_ip 10.0.1.21 -count 1 \
  -user +82571900001 -domain ptt.mnc033.mcc450.3gppnetwork.org \
  -password 123456 -mode ptt -scenario register
'
# 결과: SIP 도달 + Digest 인증 ✓ + "Auth reject: has no service binding" 403 (이슈 #3)
```

## 알려진 sub-issue (이슈 #1 fix 의 부수)

- csp UDP 가 TCP primary port (25061) 도 같이 LISTEN — psip SIP stack 의 사이드 효과. 동작 영향 없음.
- TLS port 5061 이 UDP 로도 LISTEN — 동일 사이드 효과.

## 이슈 #3 fix — service_binding seed (commit `814fe53`)

**근본 원인**: `csp/CspUser.cpp:_loadUserFromFile` 가 user JSON 에서 `service_ref` / `imsi` 를 안 읽음 (DB 경로만 보강됐고 file fallback 누락). `CCscfModule::CheckAuthorization` 에서 `m_strServiceRef` 빈 문자열 → 403 "has no service binding".

**fix**:
1. `csp/CspUser.cpp` 의 `_loadUserFromFile` 에 service_ref / imsi 읽기 추가 (1 hunk, 6 line)
2. csp 빌드 → `cp /home/nex/work/cims/build/bin/csp` 를 4 ns install dir 에 배포 (stop → unlink → cp → start)
3. `access_services.jsonl` 작성 (ctrl-a/b 의 `CSP/config/`) — `volte-basic`, `ptt-basic` 두 row
4. user JSON 에 imsi + service_ref seed — PTT 10 + VoLTE 10 × 2 ns (`csp/user/`)

**LIVE 검증** (`media-a` 안에서 cspsim → VIP 10.0.1.13):
- PTT REGISTER: `Registered: 1/1` ✓
- VoLTE REGISTER (`-auth_id 450033100000001@ims.mnc033.mcc450.3gppnetwork.org` full form 명시 필요): `Registered: 1/1` ✓
- **PTT 1콜 (count=2, scenario=call)**: Call OK 2/0 (fail=0), Setup 101ms, RTP relay via CMP 10.0.1.21:50076 ✓

cspsim 사용 시 주의:
- PTT mode: `-auth_id` 자동 유도 (+82E.164 → IMSI 변환)
- VoLTE mode: `-auth_id <imsi>@<domain>` full form 명시 (자동 유도 미지원)

## fail-over LIVE 검증 — 완전 성공

ctrl-a SIGTERM (keepalived TERM + csp stop) → 6s 후 ctrl-b 가 VIP 10.0.1.13 인수. cspsim 동일 server_ip 로 재호 → 정상 동작 (`Registered 2/2`, Call OK 2/0, Setup 101ms). ctrl-a 복구 시 priority 100 > 90 으로 preempt 자동 재인수.

```bash
# fail-over 트리거 (메모리 가이드)
sudo bash -c '
ka_pid=$(cat /tmp/ctrl-a-keepalived.pid)
vpid=$(cat /tmp/ctrl-a-vrrp.pid 2>/dev/null)
kill -TERM $ka_pid; [[ -n $vpid ]] && kill -TERM $vpid
'
curl -sk -X POST -H "Content-Type: application/json" -d '{"job_type":"stop"}' \
  https://127.0.0.1:4419/api/v1/deployments/27/job
sleep 6

# 검증
sudo ip netns exec ctrl-b ip addr show svc | grep '10.0.1.13' && echo "✓ ctrl-b 인수"
```

## sub-issue 진단 + fix — CMP `ADD_GROUP no available resource` ✅ fix `9c032d1`

⚠️ **자원 leak 원인** (당초 진단보다 정확): `processAddGroup` 자체는 이미 idempotent (existing 분기 처리 OK). 진짜 leak 은 **`timeoutLoop` 의 stale 그룹 cleanup 경로** — `_groups.erase` 만 호출하고 PTT session 을 free pool 로 반환하지 않음. csp 가 REMOVE_PTT_GROUP 없이 재기동/주기 동기화 시 cmp 의 timeoutLoop 가 SessionTimeout(기본 600s, 멤버 없으면 stale) 도달 시 group erase. 이게 매 cycle −N 누적.

타임라인 (cmp.log, fix 전):
- 20:40 init → remaining 10 → 두 그룹 alloc → remaining 8
- 23:08 csp restart → 같은 group_id 인데 `(new)` 분기 → remaining 6
- … 매번 −2 누적 …
- 05:14 → remaining 0 → 이후 모든 ADD_GROUP FAILED

**fix** (`cmp/PCmpServer.cpp:timeoutLoop` 4 line):
```cpp
PRtpMulticast* ptt = it->second->getPttSession();
if (ptt) { ptt->reset(); freePttResource(ptt); }
delete it->second;
_groups.erase(it);
// + _groupSubId.erase(gid);
```

**LIVE 검증** (SessionTimeout=30 으로 단축, media-a):
- 11:08:12 alloc rtp=52016/52018 → remaining 8
- 11:08:19 Group timeout + **freePttResource × 2** → remaining 10 복귀 ✓
- 11:08:50 csp 재동기화 alloc → 8 (oscillating 8↔10 패턴 안정)
- 이전: 단조 감소 (10→8→6→4→0)

영향 범위: PTT 그룹 호 (영구 그룹 등록 시) 에만 영향. **1대1 PTT 호 RTP relay 는 VoIP 풀 (50000~) 사용 → 영향 없음**.

부수: `removeGroup` 는 `_sesidMap`/`_serviceMap`/`_groupSubId` 모두 정리하는데 timeoutLoop 경로는 `_groupSubId` 도 누락 → 같이 정리하도록 보강.

## 다음 진입 후보

| 옵션 | 내용 |
|---|---|
| ~~(A) Phase 4 generator — `bin/render.py`~~ | ✅ commit `94ddd3d` — LIVE diff 완전 일치 (의도된 차이 2건만) |
| ~~(A2) Phase 5 검증 자동화 `bin/verify.py`~~ | ✅ commit `a625bef` — listen/smoke/failover 자동 실행, sudo 가용성 점검 + 안내 |
| ~~(A3) bundle → install dir 배포 자동화 `bin/apply.py`~~ | ✅ commit `a625bef` — 30 파일 매핑 자동, --dry-run / --no-render 옵션 |
| ~~(B) dev-single-host env~~ | ✅ commit `a625bef` — loopback 단일 host smoke 시나리오. render.py 의 svc-net 하드코딩 제거 (service_ip/service_net fallback) |
| ~~(F) VoLTE seed 정합~~ | ✅ commit `a625bef` — render 가 IMSI 별칭 user JSON 자동 생성 (cspsim 의 -user E.164/IMSI 어느 form 도 매칭) |
| ~~(C) cmp restart 분석~~ | ✅ commit `a625bef` — 분석만. 가설: `_start_cmp_variant.is_running` 가 stale pid 파일 보면 "이미 실행 중" return 0 |
| ~~(C-fix) cmp restart fix~~ | ✅ commit `c7c6581` — `is_running` exe 검증 + cmd_restart 에 kill_stray + sleep 1→3. lifecycle.sh 동기화 cheat sheet README |
| ~~(prod) prod-multi-host env~~ | ✅ commit `c7c6581` — 2-host A/S + AA reference. 사이트별 IP/암호 placeholder |
| ~~(live-guide) LIVE 검증 가이드~~ | ✅ commit `c7c6581` — tb-netns-4-node/README 의 cheat sheet 5단계 (sudo-v → 동기화 → apply → restart → verify) |
| ~~(verify-host) verify.py single-host~~ | ✅ commit `c8d9fd9` — netns 없는 환경은 bash -c 직접 실행. sudo 필요 조건도 동적 |
| ~~(apply-restart) apply.py --restart~~ | ✅ commit `c8d9fd9` — CSC API 로 자동 restart job POST (--restart 27,28,19,20) |
| ~~(volte-only) volte-only 시나리오~~ | ✅ commit `c8d9fd9` — PTT/IBCF off. tb-netns-4-node 의 두 번째 시나리오 |
| ~~(full) full 시나리오 (IBCF)~~ | ✅ commit `312c69a` — IBCF trunk + 진짜 schema 정합 |

## 진짜 csp schema 정합 (commit `312c69a` — 중요 발견)

이전 yaml 의 routes/route_sets/routing_policies/rules/rule_sets/acl_policies 가 csp 의 실제 .cpp 헤더 schema 와 불일치 → csp 가 silently skip + ERROR log. 어제 LIVE 가 동작한 건 옛 entries 없이도 동작했기 때문.

진짜 schema (csp/Csp*.cpp 헤더):
- **routes** : `(local_node_ref, remote_node_ref)` pair SOT. 외부 trunk (IBCF) 용. VoLTE/PTT 내부 호는 routes 없이 동작
- **route_sets** : `members[].route_ref/priority/weight` + distribution_policy (failover/weighted/hash)
- **routing_policies** : `match_rule_set_ref` + `target_ref` (route_set name)
- **rules** : `name/field/op/value` 트리플
  - field: src_ip/dst_ip/from_uri_host/from_uri_user/to_uri_host/to_uri_user/req_uri_host/req_uri_user/user_agent/method/p_asserted_identity/via_host
  - op: eq/ne/prefix/suffix/contains/regex/in_cidr/in_list/exists/not_exists
- **rule_sets** : `members[].rule_ref/negate` + combinator AND/OR
- **acl_policies** : `match_rule_set_ref` + scope + action (allow/deny)
- **remote_nodes** : ip/port/protocol/remote_domain/srv_lookup/dns_fallback/tls_verify (host→ip, transport→protocol 이 옛 yaml fix)

render.py 의 builder 모두 진짜 schema 로 변환 + ref dangling 검증. 5 시나리오 모두 render --check-only 통과.

### 추가 정합 (commit `ac4cf95`)

- **access_services** : `listener_ids` (잘못된 키) → `allowed_local_node_refs` (CspServiceMap.cpp:41). yaml 도 마이그레이션. kind 화이트리스트 (volte|ptt) 검증.
- **cmp endpoint 활용 한계** : CmpClient 는 `Setup.MediaServer.Host` 하나만 primary 사용. `AddEndpoint` 미호출 (1.E-2 stub). remote_nodes 의 cmp row 는 미래용 SOT.
- **dev-single-host smoke 활성화** : single-host 분기 (`c8d9fd9`) 활용한 verify.smoke (register-volte / call-volte-1on1) 추가.

## LIVE 회기 끝까지 (commit `8a1aa2e` — 세션 클로즈)

전체 워크플로 LIVE 적용:
1. lifecycle.sh 동기화 (build/dist + 5 install dir)
2. csp 바이너리 atomic install (`install -m 755`) — text file busy 회피
3. `./bin/apply.py --backup --restart auto` — 38 파일 적용 + dep 27/28/19/20 자동 restart
4. csp.log 확인 — AddEndpoint LIVE 활성 확인:
   ```
   CmpClient: primary endpoint registered (key=10.0.1.21:9000)
   CmpClient::AddEndpoint registered (key=10.0.1.22:9000, total=2)
   ```
5. multiple is_primary ERROR 발견 → render.py fix → 재 apply+restart → 사라짐

**multi-cmp endpoint 분배 LIVE 활성 (1.E-2 stub 종료)** 완전 검증. 양 csp (ctrl-a/b) 가 primary + 1 additional endpoint 로 consistent hash ring 분배.

남은 LIVE 작업 (사용자 sudo 필요): verify.py 의 smoke/failover phase (cspsim 호출).

## 세션 종료 (2026-05-15 — push 완료)

origin/main 에 37 commit push (`7fee5e4..daac1c8`). 오늘 19 commit + 이전 세션 18 commit.

deployment/ 완성 도구 세트 (`bin/`):
- `sync-agent.sh` — lifecycle.sh + 바이너리 동기화
- `render.py` — env+scenario → bundle (+ `--diff`)
- `apply.py` — install dir 적용 (env.kind 분기, `--backup` / `--restore` / `--restart auto` status-aware / `--skip-restart-if-no-change` / `--verify`)
- `verify.py` — listen/smoke/failover (+ `--json` CI)
- `check-all.sh` (or `make verify-scenarios`) — 5 시나리오 회기
- `health.sh` — sudo 없는 빠른 LIVE 진단

LIVE 검증된 fix:
- `814fe53` csp file fallback service_ref/imsi
- `9c032d1` cmp GROUP_TIMEOUT leak (LIVE freePttResource 동작 확인)
- `0910c13` ha.json port/proto 자동
- `8a1aa2e` local_nodes is_primary single
- `4b77ed9` CmpClient AddEndpoint LIVE 활성 (1.E-2 stub 종료)

3 환경 × 5 시나리오 모두 PASS:
- tb-netns-4-node: volte-ptt / volte-only / full(IBCF)
- dev-single-host: smoke
- prod-multi-host: volte-ptt
| ~~(B) PTT 자원 leak 본격 fix~~ | ✅ commit `9c032d1` |
| (C) **cmp restart job 분석** | restart job 이 실제 PID 변경 못 시키는 케이스 (race condition?) |
| (D) **dev-single-host env** | 로컬 1-node 환경 추가 등록 (verify 자동화 위한 다양화) |
| ~~(E) ha-group port/proto 자동 채우기~~ | ✅ commit `0910c13` — `_render_ha_for_agent` 가 멤버 agent 의 daemon deployment 들 보고 자동 기재. process_name 매칭 (package_name 이 None 일 수 있음). LIVE 검증 cims-health rc=0 / VIP 자동 부여 ✓ |
| (F) **VoLTE seed 정합** | sim-a 에서 VoLTE REGISTER 실패 — user JSON seed (450033100000001) 와 cspsim auth_id 매칭 점검 |

## 3-tier 일괄배포 자동화 (이번 세션 commit `c26c1e9`)

새 디렉토리: `deployment/bin/`
- `deploy-modules.sh` — TB-CSC API 기반 모듈 deployment 일괄 생성 + install + start. 멱등.
- `verify-modules.sh` — sim-a 에서 ping/REGISTER/1대1 호 검증

토폴로지:
- ctrl-a/b: csp + isp + psp + csc (active_standby)
- media-a/b: cmp + imp + pmp (all_active)
- sim-a: cspsim (standalone, install 만)

총 15 deployment (14 데몬 running + cspsim install 만). LIVE 검증 1대1 호 Setup 101ms.

### 진입 prerequisite (스크립트 외 수동 작업)
1. `verify/scripts/ha-netns-up.sh` — ns 5개 구축 (ctrl-a/b, media-a/b, sim-a)
2. 각 ns 에 agent enroll: `verify/scripts/ha-netns-install-agent.sh <ns> <token> <name>` + console approve
3. ha_group 정의 (Control AS, Media AA)
4. **ctrl-*/keepalived ha.json 의 services.<group> 에 `port` + `proto` 추가** (현재 console UI 자동 안 채움)
5. keepalived SIGHUP

이 4번이 이번 세션에서 새로 발견된 sub-issue. 옵션 (E) 가 자동화 진입 후보.

## 관련 메모리
- [[project_session_2026_05_14_deploy_verify]] — 어제 자율 검증 (이슈 3건 발견)
- [[project_session_2026_05_14_phase1_refactor]] — 어제 commit 7개
- [[project_db_external]] — DB 외부 위임 결정
