# 관리평면(OAM) 이중화 — Active/Standby 설계

> 관리평면(base `oam` + `oam-svc`)을 Active/Standby 로 이중화하는 정본 설계다.
> HA 판정·절체 모델은 [ha_service_model.md](ha_service_model.md) 가 정본이며 이 문서는
> **그 모델에 관리평면을 편입하기 위해 필요한 것**만 기술한다. keepalived 인프라 상세는
> [../ha_design.md](../ha_design.md) §11, base/service 경계는
> [oam_base_service_split.md](oam_base_service_split.md) 를 본다.
>
> 관리평면은 HA 모델이 `safety.class=shared_writer` 로 분류하는 대상이다
> (ha_service_model.md §14) — 공유 파일 write·스케줄러·VIP 무관 background 작업을 모두
> 가진다. 따라서 **VIP 이관만으로는 정합이 성립하지 않고, 소유권 펜싱이 설계의 핵심이다.**

## 1. 왜 서비스 모듈과 다른가

CSP/CMP/CSC 는 상태를 외부(DB·UE 재등록)에 두므로 VIP 이관 = 서비스 이관이다. 관리평면은
**자기 상태를 자기 노드 디스크에 들고 있다.**

| 관리평면이 소유한 상태 | 위치 | 절체 시 없으면 |
|---|---|---|
| agent 목록 + `agent_token`·enrollment | `control/agents` | 전 agent 인증 실패(401) — 관리 전면 불능 |
| 배포 목록 + config overlay | `control/deployments` | 설정 SoT 상실, 재배포 불가 |
| 패키지 메타 + 타르볼 | `control/packages`, `pkg_files` | 설치·업그레이드·`/install-agent.sh` 불가 |
| HA 그룹 정의 | `control/ha_groups` | **자기 절체 근거 상실** (ha.json 재렌더 불가) |
| 게이트웨이 라우트 | `control/gateway_routes` | csc/oam-svc 프록시 전면 503 |
| job 큐 + 결과 | `control/jobs` | in-flight 작업 유실 |
| 계획 절체 operation | `ha_operations` | 진행 중 절체가 미완 상태로 고착 |
| 콘솔 계정·레이아웃·메뉴 | `console/*` | 계정·화면 구성 소실 |
| auth_codes / refresh_tokens | 런타임 루트 | 세션 소실 |
| metric 시계열 | `control/metrics` | 그래프·KPI 이력 단절 |

또한 신원(시크릿·인증서)이 **설치 시점에 노드별로 생성**되므로 노드 A 가 발급한 토큰이
노드 B 에서 무효다. 이 두 가지(상태·신원)를 해결하지 않은 이중화는 "절체는 되지만 아무것도
못 하는 콘솔"을 만든다.

## 2. 결정 요약

| # | 결정 |
|---|---|
| D1 | VIP 는 노드 쌍당 1개다. 관리평면은 **서비스 그룹과 같은 그룹·같은 VIP** 에 동거하고, 그룹의 단일 대표 헬스 제약을 **모듈별 health 로 정면 수정**한다 |
| D2 | 상태 SoT = **공유 스토리지(NAS) + 소유권 리스(epoch fence)**. 양 노드 상시 마운트, 리스 보유 노드만 write (§4.1) |
| D3 | 신원(시크릿·키)은 **공유하지 않고 복제**. 노드 로컬 `0600`, 그룹 공통 자산으로 배포 |
| D4 | 토큰 서명 비대칭 전환은 **이번 범위 밖 — 상위 결정 대기** (§5.1). 신규 컴파일 의존성(`cryptography`) 도입 판단이 선행 |
| D5 | `oam`/`oam-svc` 를 service descriptor·module_specs 에 **1급 등록**, 둘 다 cold |
| D6 | `OAM_ROLE=base` 를 배포 스펙으로 승격 (systemd drop-in 의존 제거) |
| D7 | 계획 절체는 **신 Active 가 이어받는다** (source 가 자기 자신을 정지하므로) |
| D8 | 모든 agent 와 브라우저는 **그룹 VIP(.140) 하나만** 본다. `Server.Ip` 는 `0.0.0.0` 고정 |
| D9 | 2번 노드 설치는 부트스트랩 **join 모드** — 콘솔 배포 경로로는 성립하지 않는다 |

## 3. 토폴로지 (D1)

**VIP 는 노드 쌍당 1개**다. 따라서 관리평면은 별도 VIP·별도 그룹을 갖지 않고 그 쌍의
기존 서비스 그룹에 동거한다 — 하나의 VIP 를 두 vrrp_instance 가 나눠 갖는 구성은 성립하지
않기 때문이다(같은 IP 를 두 인스턴스가 소유 다툼 → ARP 충돌).

```text
        ┌────────── nodeA (121.161.164.134) ──────────┐  ┌──── nodeB (.137) ────┐
Control │ csc      (cold, relevant)  ┐                 │  │ csc      (cold)      │
 group  │ oam      (cold, relevant)  ├ 개별 감시        │  │ oam      (cold)      │
VIP.140 │ oam-svc  (cold, relevant)  ┘ 하나라도 실패=절체│  │ oam-svc  (cold)      │
        │ cims-agent  --oam-url https://.140:4419      │  │ cims-agent  동일      │
        └────────────────────┬─────────────────────────┘  └─────────┬────────────┘
                             └──── 공유 store (NAS, 리스로 단일 writer) ────┘
브라우저 ──> https://.140:4419   (VIP 보유 노드가 응답)
```

동거가 만드는 이점: base OAM 과 csc·oam-svc 가 **항상 같은 노드에서 Active** 이므로
게이트웨이 upstream 이 언제나 `127.0.0.1` 이다 — `Server.GatewayHost` 를 VIP 로 지정할
필요가 없고, 절체해도 프록시 경로가 그대로 성립한다. VIP 도 서비스망 VIP 그대로이므로
`agent.md` §11 의 "VIP 는 서비스망에만" 원칙을 개정할 필요가 없다.

### 3.1 모듈별 health — 그룹 단일 대표 제약의 정면 수정

한 그룹에 데몬이 여럿이면 **모듈마다 자기 포트로 개별 감시**되어야 한다. 그룹당 대표 하나만
포트를 갖게 하면 나머지 모듈의 readiness 가 "프로세스 존재" 로 대체돼 **소켓만 살아있고
핸들러가 죽은 좀비를 영구히 놓친다** — 그룹을 쪼개서 우회할 문제가 아니라 모델의 결함이므로
정면으로 고쳐져 있다.

- ha.json 의 service entry 에 **모듈별 health 맵**이 실린다(기존 service 레벨
  `port`/`proto`/`health_module`/`health_config_key`/`health_collection` 은 하위호환
  유지, 새 맵이 우선):

  ```jsonc
  "services": { "Control": {
      "cold_modules": ["csc", "oam", "oam-svc"],
      "relevant_modules": ["csc", "oam", "oam-svc"],
      "module_health": {
        "csc":     { "port": 4421, "proto": "tcp", "config_key": "Server.Port" },
        "oam":     { "port": 4419, "proto": "tcp", "config_key": "Server.Port",
                     "http_path": "/health" },
        "oam-svc": { "port": 4480, "proto": "tcp", "config_key": "Server.Port",
                     "http_path": "/health" }
      } } }
  ```

- agent `_health_targets` 가 이 맵을 읽어 **모듈마다 자기 포트로** liveness/readiness/
  preflight 를 수행한다. verdict 집계는 이미 모듈 단위(`relevant` 순회 +
  `_module_health(m, check)`)라 Evaluator 는 무변경이고, `cims-health` 는 verdict reader 라
  root 측 변경도 없다.
- `http_path` 를 선언한 모듈은 포트 확인에 **로컬 HTTP 프로브**가 더해진다(2xx/3xx·401/403 =
  정상). 관리평면 모듈은 bind 를 유지한 채 핸들러만 죽는 좀비가 실제 위험이라 필수다.
- 해석 우선순위: 운영자 `module_specs.<mod>.health` > descriptor `modules[].health` >
  descriptor 상수 `port/proto`. 포트의 실제 해석은 agent 가 검사 시점에 노드 로컬 파일로
  수행한다(배포기록↔실파일 드리프트에도 실제 bind 포트를 본다).

### 3.2 절체 파급 — 셋 다 relevant

VIP 가 하나이므로 **절체 단위는 그룹**이고, `csc`·`oam`·`oam-svc` **세 모듈 모두**
`failover_relevant: true` 다(`_MODULE_SPEC_DEFAULT` 의 기본값과 동일 — 예외를 두지 않는다).

```text
세 모듈은 각각 자기 포트로 개별 감시된다 (§3.1)
  → 어느 하나라도 로컬 복구(restart_limit)를 소진하거나 좀비 판정되면
  → FAILOVER_LATCHED → vrrp_eligible=false → VIP 반납
  → 그 노드의 hot·cold 모듈 전부 정지 (래치 규칙, ha_service_model.md §7.1)
  → 상대 노드 승격 → 공유 store 확인(§4.2) → 세 모듈 기동
```

즉 **콘솔(OAM)이 못 살아나면 csc 도 함께 넘어간다.** 이것이 VIP 1개 구성의 정의이며,
"관리평면만 남기고 서비스는 유지" 같은 부분 절체는 존재하지 않는다.

파급되는 성질(수용 사항):

- 한 모듈의 crash-loop 이 그룹 전체 절체를 유발한다 → `restart_limit`(기본 3회/300초)이
  세 모듈 공통 임계다. 모듈별로 다른 임계가 필요해지면 `module_specs` 확장 대상이다.
- 유지보수(EXCLUDE_NODE)·계획 절체도 그룹 단위다 — 관리평면만 따로 점검·절체할 수 없다.
- 절체 시 콘솔 단절과 서비스 순단이 항상 동시에 발생한다.
- `oam`/`oam-svc` 는 `safety.class=shared_writer` 라 래치 해제가 수동
  (`latch_clear_mode: manual`)이다 → 절체당한 노드는 운영자가 원인을 고치고 start/restart
  로 명시 재합류시킨다. 자동 재합류를 켜면 안 된다(§4.4 리스와 충돌).

## 4. 상태 SoT — 공유 스토리지 + 소유권 리스 (D2)

### 4.0 무엇이 옮겨가고 무엇이 남는가

옮기는 것은 **OAM 의 control-plane store 뿐**이다. agent 자기 상태는 노드 로컬로 남는다 —
HA 판정이 노드 로컬이어야 한다는 원칙(ha_service_model.md §5·§15)에 걸리기 때문이다.

| 노드 로컬 유지 (변경 없음) | 공유 store 로 |
|---|---|
| `run/ha/{verdict,role,health,promotion,recovery}` | `control/*` (agents·deployments·jobs·metrics·packages·ha_groups·gateway_routes) |
| `state/ha/{maintenance,planned_release}`, `run/ha/desired.json` | `console/*` (계정·레이아웃·메뉴) |
| `run/keepalived/ha.json`, `run/managed_ips.json`, `run/supervised.json` | `ha_operations`, `auth_codes`, `refresh_tokens`, `pkg_files`, `verify_runs` |
| `modules/<mod>/service.json`, 모듈 `config.json` | |
| **시크릿·TLS·CA** (`runtime/_secrets/`, §5) | |

현재 `file_store.runtime_root` 는 파생 경로가 공유 마운트로 유도되면 `RuntimeError` 를 던지고
(`oam.json` 에도 "관리 데이터 SoT 는 mgmt host 로컬 유지" 주석이 고정돼 있다), 그 근거는
**"OAM 다중 노드에서 동시 write 시 손상"** 이다. 즉 금지의 대상은 공유 스토리지 자체가 아니라
**펜싱 없는 다중 writer** 다. 이 설계는 그 전제를 제거한다(§4.1·§4.4) — 따라서 가드는 폐지하지 않고
**"리스 없이 공유 금지"** 로 재정의하고, 명시 `CimsRuntimeDir` 이 검사를 우회하는 현재 구멍도
같이 막는다(검사 기준을 경로 → 리스 보유로 이동).

### 4.1 store 위치 = 공유 스토리지 (NAS)

관리 store 를 **양 노드가 상시 마운트하는 공유 경로**에 둔다. 블록 복제(DRBD)를 쓰지 않는
이유는 실무적이다: DRBD 는 노드마다 **여유 블록 장치(파티션/LV)** 를 요구하고, 커널 모듈
의존(커널 업그레이드마다 재빌드)이 붙는다. 이 환경에는 여유 블록 장치가 없고, 반면 NAS 는
**이미 마운트되어 서비스 로그를 받고 있다**(`ServiceLogging.Dir`). 마운트 관리(fstab 영속,
`_netdev,nofail` 강제)도 이미 제품 기능으로 있다.

```text
        nodeA                              nodeB
   /NAS/cims/oam_store  ←── 같은 경로 상시 마운트 ──→  /NAS/cims/oam_store
        │  (fstab, _netdev,nofail)                      │
        └──── 리스(flock+epoch) 보유 노드만 write ──────┘
                     = VIP 보유 노드
```

- **마운트는 양 노드 상시**다. 절체 시 mount/umount 를 하지 않는다 — 마운트는 로그 등
  다른 용도와 공유되는 자원이고, 흔들어서 얻는 것이 없다. 대신 **write 권한을 리스로
  통제**한다.
- 따라서 **파일시스템 계층의 펜싱이 없다**. 단일 writer 를 만드는 것은 리스 하나뿐이므로
  (§4.4) 리스가 load-bearing 이고, **잠금이 실제로 동작하는지 자기검증**한다.
- `ServiceLogging.Dir`(서비스 로그·alert 이력)은 지금처럼 같은 NAS 유지 — append-only 관측
  데이터라 두 노드가 동시에 써도 무해하다. 관리 store 와 성격이 다르다.

**수용하는 위험**: NAS 가 SPOF 다. NAS 가 죽으면 관리평면(+ 같은 그룹의 csc)이 멈춘다.
통화 서비스(CSP/CMP)는 별 그룹이고 NAS 와 무관하다. 이미 서비스 로그가 NAS 의존이므로
**새로운 종류의 SPOF 는 아니다**. 블록 복제로 이 SPOF 를 없애는 선택지는 여유 블록 장치가
확보되면 다시 검토할 수 있다.

### 4.2 절체 순서 — store 는 인수 대상이 아니라 전제 조건

마운트를 옮기지 않으므로 절체 순서에 볼륨 단계가 없다. 대신 **모듈을 기동하기 전에 store 가
쓸 수 있는 상태인지 확인**한다.

```text
승격 (BACKUP/FAULT → MASTER)          강등 (MASTER → BACKUP/FAULT/래치)
 1. keepalived 가 VIP 부착              1. 모듈 정지 (csc·oam·oam-svc)
 2. 공유 store 확인 (마운트 + write)    2. 리스 소유권 해제(기록만, 강제 아님)
 3. 리스 획득(epoch+1)                     → OAM 은 read-only 로 내려간다
 4. 모듈 기동 (승격 grace 시작)            (umount 단계 없음)
```

- **소유 주체는 Recovery Supervisor(reconcile)** 다. cims-notify 는 role 파일만 쓰고
  아무것도 실행하지 않는다는 원칙(ha_service_model.md §2)을 유지한다.
- store 스펙은 **그룹 스코프**(`ha_groups` record 의 `shared_store`)이고 ha.json 의
  `services.<svc>.shared_store` 로 내려간다: `{mount_point}` 하나다. 절대경로가 아니면
  조용히 빠지는 대신 **미사용**으로 정규화되고, 스펙이 없으면 store 단계 자체가 없다
  (단일 노드·기존 그룹 무영향).
- 마운트 생성·영속은 **서버별 마운트 관리**(`cims-priv mount-add`, fstab + `_netdev,nofail`)가
  담당한다. agent 의 HA reconcile 은 마운트를 만들지도 지우지도 않는다 — 권한 작업이 줄어
  `cims-priv` 에 볼륨 전용 서브커맨드가 필요 없다.
- **store 확인 실패는 승격 실패다.** 모듈 기동을 **시도하지 않고** 보류한다 → readiness 가
  안 올라오므로 `vrrp_eligible` 이 떨어져 상대 노드가 인수한다. 조용히 진행하면 §4.3 의
  mount guard 가 걸려 모듈이 기동조차 못 하는 상태로 VIP 를 붙들게 된다.

**preflight 에 store 상태를 넣는다.** cold 모듈의 승격 자격은 현재 "설치·설정 존재"만 본다
(`_run_health_check` preflight). 여기에 **공유 store 검사**를 추가한다.

| 역할 | 요구 조건 | 실패 시 |
|---|---|---|
| BACKUP/UNKNOWN/FAULT (승격 자격) | store 가 **마운트돼 있고 write 가능** (실제 write·fsync 1회) | `vrrp_eligible=false` — 관리 데이터에 접근 못 하는 노드로 승격하면 **빈/읽기전용 콘솔을 이어받는다** |
| MASTER + 승격 grace | 위와 동일 | 승격 중단 |
| MASTER 정상 운전 | **마운트되어 있을 것** (write 가능까지는 요구하지 않음) | 절체 사유 — 마운트가 빠졌으면 모듈이 엉뚱한 위치(마운트 포인트 하부 로컬 디스크)에 쓰는 중 |

> 승격 자격에 **실제 write** 를 요구하는 이유: NFS 는 서버 장애 시 마운트는 남아 있는데
> I/O 만 막히는 상태(stale handle)가 되므로 마운트 존재 확인만으로는 부족하다.
> 반대로 MASTER 정상 운전에는 write 를 요구하지 않는다 — 일시적 NFS 지연으로 절체하면
> **같은 NAS 를 보는 피어**로 넘어가 나아지는 것이 없다.

이 검사 때문에 **양 노드 모두 승격 불가(VIP 공백)** 가 될 수 있으며, 그것이 데이터에 접근
못 하는 채로 서비스하는 것보다 안전하다는 기존 판단(ha_service_model.md §19)을 따른다.

**승격 grace 는 그룹 설정(`failover_options.health.grace_sec`, 기본 30초)을 따른다**(§10).
store 확인 + OAM 콜드스타트(python·마이그레이션·cert·bind, `CIMS_OAM_HEALTH_TIMEOUT` 기본
20s)를 합치면 30초도 빡빡할 수 있으므로 이 그룹은 **45~60초** 로 설정한다 — 짧으면 승격
직후 grace 만료 → eligible=false → 방금 얻은 VIP 반납 = flap 이다.

### 4.3 배치 + mount guard

`CimsRuntimeDir` 를 그 공유 마운트 하위 경로로 지정한다. 관리평면 데이터는 전부 옮기며 예외를
두지 않는다 — 일부만 옮기면 절체 후 화면이 반쪽이 된다.

```text
<shared>/control/{agents,deployments,jobs,metrics,packages,ha_groups,
                 gateway_routes,csp_sync_txn}/
<shared>/console/{console_accounts,console_layouts,console_menu,console_user_layouts}/
<shared>/{ha_operations,auth_codes,refresh_tokens,pkg_files,verify_runs}/
<shared>/.owner.json                     ← 소유권 리스
```

- `ha_operations`·`auth_codes`·`refresh_tokens`·`verify_runs` 는 현재 `_OAM_CATEGORY` 에
  없어 런타임 루트 평면에 떨어진다. 레이아웃 도입과 함께 카테고리를 정리한다.
- 시크릿(`_secrets/`)·TLS cert 는 **공유 store 에 두지 않는다** (§5) — 노드 로컬 `0600`.

**mount guard (필수).** 마운트가 안 된 상태에서 OAM 이 기동하면 마운트 포인트 **아래의
로컬 디스크**에 두 번째 빈 store 를 만든다. 절체할 때마다 서로 다른 store 를 보게 되는
가장 나쁜 divergence 이고, 조용히 진행되기 때문에 사고가 난 뒤에야 발견된다. 다만 **무조건
기동 거부는 답이 아니다** — 설정을 고칠 콘솔이 사라져 관리평면이 되돌릴 통로 없이 영구
정지한다(실측). 그래서 상태별로 나눈다:

| 마운트 | 대상 경로 store | 로컬 store | 동작 |
|---|---|---|---|
| 있음 | — | — | 통과 (`CimsRuntimeDir` 이 마운트 하위인지만 확인) |
| 없음 | **있음** | — | **기동 거부** — 공유 store 를 보던 노드가 스토리지를 잃은 상태. 로컬로 갈아타면 데이터가 분기된다 |
| 없음 | 없음 | **있음** | **로컬 store 로 기동** + 경고. 아직 이관 전이거나 설정이 잘못 들어간 경우다. 콘솔이 살아 있어 운영자가 고치거나 이관을 실행할 수 있다. 이 노드는 공유 store 를 못 쓰므로 agent preflight 가 승격 자격에서 제외한다 |
| 없음 | 없음 | 없음 | **기동 거부** — 신규 설치인데 마운트가 없다. 여기서 뜨면 마운트 지점 하부에 store 가 생겨 나중에 마운트가 붙을 때 그 데이터가 가려진다 |

**마운트 지점은 자유 입력이 아니다.** agent 가 heartbeat 로 **실제 마운트 목록**
(`mount_targets` — cims-managed 여부와 무관, 운영자가 미리 붙여둔 NAS 포함)을 보고하고,
콘솔은 **멤버 노드 공통 마운트만 select 로** 제시한다. 서버도 저장·이관 시 그 목록으로
검증해 아니면 400 `not_a_mount_point`(실제 마운트 목록 동봉)로 거부한다. 자유 입력을 받으면
NAS **안의 하위 폴더**를 마운트 지점으로 지정하는 실수가 나고, guard 는 정확히 일치하는
경로만 통과시키므로 OAM 이 기동을 거부한다 — 콘솔이 사라져 되돌릴 통로까지 없어진다
(실측 사고: `/NAS/cims_johnyim/oam_store` 지정, 실제 마운트는 `/NAS` 하나).

설정 키는 oam 배포설정 `CimsRuntimeMount`(마운트 지점)이며, 공유 store 구성에서는 반드시
채운다. `--preflight`(self-upgrade 검증 경로)도 같은 검사를 지나므로 마운트 이상은 업그레이드
health-gate 에서도 즉시 드러난다.

**store 경로를 자동으로 옮기지 않는다.** `lifecycle.sh start_oam` 은 예전에 `CimsRuntimeDir`
접근이 안 되면 조용히 `dist_dir/ext_mnt/runtime` 으로 바꿔 기동했다. 그 결과 관리 데이터의
SoT 가 **버전 디렉터리 안**으로 들어가 업그레이드가 그것을 교체하며 서버·그룹·배포 기록이
사라졌다(실측 사고). 공유 마운트 구성에서는 "마운트가 잠깐 없다"는 이유로 store 가 로컬로
이동해 절체 시 빈 콘솔이 된다. 지금은 **경고만** 하고 경로 판정은 위 guard 가 담당한다.

**store 경로 폴백은 버전 무관 노드 로컬.** `CimsRuntimeDir` 가 비었을 때 store 가 어디에
떨어지는지가 mount guard 다음의 두 번째 divergence 원인이다. 폴백은 `paths.local_runtime_dir()`
= **패키지 루트의 `runtime/`** 한 곳이며, 로그 디렉터리 sibling 이나 프로세스 cwd 를 쓰지
않는다. cwd 는 배포본에서 `.../releases/<version>/` 이라 store 가 **버전 디렉터리 안**에 생기고,
업그레이드가 그 디렉터리를 교체하면 관리 데이터가 통째로 사라진다(실측 사고).

기동 시 확정된 런타임 루트에 `control/` 이 없고 legacy 위치(`<pkg>/ext_mnt/runtime`,
`cwd/runtime`)에 있으면 **1회 복사**해 옮기고 경고를 남긴다 — 잘못된 위치에 이미 쌓인 데이터를
버리지 않기 위한 이행 장치이며, 멱등이다.

### 4.4 리스 = VIP 소유권의 파일 표현

시간 기반 TTL 을 쓰지 않는다. ha_service_model.md §15 가 **크로스노드 시각 비교를
금지**하며, 실제로 과거 절체 조사에서 노드 간 30초 시계 오차가 포렌식을 무효화한 사례가
있다. 리스는 시각이 아니라 **커널 배타 잠금(flock) + 단조 epoch** 으로 정의한다.

공유 스토리지는 양 노드가 동시에 마운트하므로 **파일시스템 계층의 펜싱이 없다**. 방어는
2층이고, 단일 writer 를 만드는 것은 **리스 하나뿐**이다(= load-bearing):

| 층 | 수단 | 막는 것 |
|---|---|---|
| 1 | mount guard (§4.3) | 마운트 없이 떠서 로컬 디스크에 두 번째 store 생성 |
| 2 | **리스 (`services/lease.py`)** | 두 노드의 OAM 동시 write / 같은 노드의 두 OAM 프로세스 |

```jsonc
// <runtime>/.owner.json  (원자 교체 — tmp+fsync+rename)
{ "node_id": "nodeA", "epoch": 42,
  "boot_id": "7b4c…",                 // 재부팅 전 값 재사용 차단
  "pid": 12345,
  "acquired_at": "2026-08-04T10:11:12", // 사람이 읽는 용도 — 판정에 쓰지 않는다
  "prev_node_id": "nodeB" }
```

**획득 (모두 로컬 판정, store 접근 전)**

```text
acquire(root) :=
      <root>/.owner.lock 에 배타 flock(LOCK_EX|LOCK_NB) 획득  ← 실패 = 다른 writer 존재
  AND .owner.json 의 epoch 를 +1 해 원자 기록 (프로세스 수명 동안 잠금 보유)
```

**잠금 자기검증 (공유 스토리지에서 필수).** 공유 파일시스템에서 advisory lock 이 no-op 인
구성(NFSv3 에 lockd 없음, `nolock` 마운트, 일부 CIFS)이면 flock 은 **항상 성공**하고 펜싱이
조용히 사라진다 — 파일시스템 층이 없으므로 곧 데이터 손상이다. 그래서 획득 직후 **두 번째
fd 로 같은 파일에 배타 잠금을 시도**해 반드시 실패하는지 확인한다(flock 은 open file
description 단위이므로 같은 프로세스에서도 충돌해야 한다). 실패하지 않으면 소유권을 인정하지
않고 `reason=locking_not_enforced` 로 read-only 에 머문다.

epoch 는 그 위에서 "누가 나중에 잡았는가" 를 시각 없이 판정한다 — 잠금이 끊겼다 재수립되는
경우(NFS 서버 재시작 등)에도 구 소유자가 자기 소유라고 착각한 채 write 하는 것을 막는다.

**재획득.** 리스는 기동 시 1회만 잡는 것으로는 부족하다. 절체 직후 신 Active 가 (구 Active 가
아직 놓지 않아) 획득에 실패하면, 구 Active 가 물러난 뒤에도 **영원히 read-only** 로 남는다
(실측: VIP 는 넘어갔는데 콘솔이 `locked_by_other_writer`). 그래서 소유권이 없는 동안 스위퍼
루프가 **5초마다 재획득을 시도**해 절체를 완결시킨다.

**write 게이트**

- `file_store` 의 모든 write 진입점(`save`/`delete`/`next_id`/`jsonl_append`)이
  `lease.assert_writable()` 을 통과해야 한다. 실패 시 `LeaseLostError` → HTTP **409
  `not_lease_owner`**(컨트롤러가 매핑 — 500 이면 "OAM 고장" 으로 오해된다).
- 조회(`load`/`load_all`/`by_id`)는 제약이 없다 — 소유권이 없으면 **read-only 강등**이고
  프로세스는 살아 있다(운영자가 원인을 볼 수 있어야 한다).
- epoch 확인은 write 직전 재확인(1초 캐시)이다. 자기 것과 다른 `node_id`/`epoch` 가 보이면
  즉시 read-only 로 강등한다 — **먼저 있던 쪽이 물러난다**.
- 상태는 `GET /api/v1/gateway/health` 의 `lease`/`read_only` 로 노출된다(콘솔 배너 근거).
- 한계: "migrate-on-read" 성격의 기회적 write(예: 구 그룹 record 정규화)는 read-only 에서
  실패한다. 알려진 경로는 예외를 무시하도록 감싸져 있다.

**dual-MASTER 시 (VRRP 단절)**: 양쪽이 VIP 를 보유해 `own_lease()` 전반부를 통과할 수
있다. 이때 epoch 경쟁이 최후 방어다 — 나중에 획득한 쪽이 epoch 를 올리고, 먼저 있던 쪽은
다음 write 에서 epoch 불일치를 발견해 read-only 로 내려간다. **데이터 손상은 막지만
가용성은 보장하지 않는다**(둘 다 read-only 로 수렴할 수 있다). 이는 ha_service_model.md
§14 가 요구하는 `shared_writer` 보호의 구현이며, 그보다 강한 정합이 필요하면 같은 문서가
가리키는 fencing(STONITH)·witness 도입이 맞다.

### 4.5 sweeper 게이트

OAM 백그라운드 루프(`oam_app.py`)는 8개 sweeper 를 1초 tick 으로 돌리며 전부 store 를
쓴다: stale-agent, cert-rotate, alert, sync-txn, drift(+auto_resync), auto-sync,
ha-operations, metric-purge. **리스 미보유 시 전부 skip 한다** — API 만 막고 sweeper 를
두면 background writer 가 그대로 남아 이중 write 가 된다. 루프는 매 tick `lease.verify()`
를 호출하므로 write 가 없어도 epoch 펜싱이 감지되고, 강등 시 60초 주기로 경고를 남긴다.

### 4.6 절체 후 연속성

| 항목 | 근거 |
|---|---|
| agent 재인증 불필요 | `agent_token` 이 공유 `control/agents` 에 있다 |
| in-flight job 이어받기 | `control/jobs` 공유 — 신 Active 가 그대로 디스패치 |
| VIP 관측 즉시 가능 | `vip_observation` 은 `agents.last_heartbeat`+`interfaces` 를 읽고 둘 다 공유 (stale 창 90s) |
| 게이트웨이 라우트 유효 | 공유 `gateway_routes`, upstream=127.0.0.1 (§6 동거 배치) |
| 계획 절체 resume | 공유 `ha_operations` (§7) |

## 5. 신원 — 공유하지 않고 복제 (D3)

개인키를 네트워크 공유에서 읽는 구성은 채택하지 않는다: NFS 기본 인증(AUTH_SYS)은
호스트/uid 주장을 신뢰하고, 평문 전송·백업 확산·감사 불가 문제가 있으며, **관리평면이
살아남아야 하는 상황에서 NAS 장애가 기동 불능 사유가 된다.** 어플라이언스 2노드 HA 의
표준(corosync authkey·keepalived auth_pass)과 동일하게 **각 노드 로컬 `0600`
+ 합류 시 1회 배포**로 한다.

| 자산 | 위치 | 배포 |
|---|---|---|
| `CimsAuth.JwtSecret` | 노드 로컬 `runtime/_secrets/jwt_secret`(0600) | **그룹 공통** — 배포 설정 overlay 로 양 노드에 주입 |
| `BuiltinAccounts`(admin) | 배포 설정 overlay | 그룹 공통 (base `oam` 주입 대상) |
| TLS 서버 인증서 | `runtime/cert/{server.key,server.crt}` (key 0600, 버전무관) | **lifecycle 엔진이 모듈 기동 전 보증**(§5.2). 노드별 keypair, SAN 에 VIP·노드 IP·hostname |
| 그룹 CA | 노드 로컬 `runtime/_secrets/ca/{ca.crt,ca.key}`(key 0600) | join 이 같은 CA 를 복사 (개인키는 공유 store 에 두지 않는다) |
| mTLS CA | `runtime/_secrets/agent_mtls/`(0700, 버전무관) | 〃 |
| 서명 키 (§5.1) | — | 대칭키 HS256 유지 (상위 결정 대기) |

**주입 경로** — `_materialize_deploy_config` 가 배포 job 을 디스패치할 때 base 의 현재 값을
config.json 에 실체화한다. 대상은 두 부류다:

| 선언 | 모듈 | 주입 값 |
|---|---|---|
| `meta.gateway.routes` | csc, oam-svc | JwtSecret · CimsRuntimeDir · Mgmt.Cidr · (빈 경우) ServiceLogging.Dir |
| `meta.shared_identity` | **base `oam`** | 위 + `CimsAuth.BuiltinAccounts` |

**패키지에는 시크릿을 동봉하지 않는다.** 하드코딩 상수는 예측 가능해서 그 값으로 뜬 노드의
토큰이 위조될 수 있다. 해석 순서는 **설정(overlay/oam.json) → `_secrets/jwt_secret` →
노드 로컬 1회 생성(0600, 경고 로그)** 이다. 마지막 단계는 "관리평면이 아예 못 뜨는" 상황을
피하기 위한 것이고, 그 노드 토큰은 피어와 호환되지 않으므로 기동 로그에 경고를 남긴다 —
이중화에서 두 노드를 같은 신원으로 맞추는 것은 위 주입 경로(그리고 §9 join)의 책임이다.

**인증서 SAN 자동 보강** — 필요한 SAN 은 `hostname`·`127.0.0.1`·`Server.AgentOamUrl` 의
host(= 이중화에서는 VIP) ∪ `Server.CertSans`(운영자 지정: 피어 노드 IP·별칭)다. 기동 시 현
인증서가 이를 다 담고 있지 않으면 **그룹 CA 로 재발급**한다. 단 **운영자가 넣은 상용
인증서(주체가 CIMS 가 아니고 우리 CA 서명도 아님)는 건드리지 않고 경고만** 남긴다.
브라우저는 그룹 CA 를 1회 신뢰하면 절체로 노드가 바뀌어도 경고가 없다.

### 5.1 토큰 서명 대칭키 — 이번 범위 밖 (상위 결정 대기)

현재 콘솔 토큰은 `HS256`(대칭키, `handlers/auth.py`)이고 `_materialize_deploy_config` 가
같은 키를 csc·oam-svc 배포 설정에 복사한다. **대칭키는 검증키와 서명키가 같으므로, 검증만
필요한 모듈이 admin 토큰 발급 능력까지 보유**한다 — 모듈 한 곳이나 그 설정 파일이 유출되면
관리자 토큰 위조가 가능하다.

정석 해법은 비대칭 서명(OAM 만 개인키, 모듈은 공개키만)이지만 **PyJWT 의 EdDSA/RS256 은
`cryptography` 패키지를 요구**하고(`vendor/jwt/algorithms.py` 의 `has_crypto` 게이트),
현재 oam·csc vendor 트리에 없다. 폐쇄망 전 노드에 **컴파일 확장 의존성을 새로 도입**하는
결정이므로 HA 이중화 작업과 함께 처리하지 않는다.

- **상위 결정 사항**: `cryptography` 를 CIMS 배포물의 상시 의존성으로 채택할 것인가
  (플랫폼·파이썬 버전별 wheel 관리, OS 업그레이드 시 재검증 부담 포함).
- 채택 불가 시 대안: **게이트웨이 종단 검증** — base 만 토큰을 검증하고 모듈은 서명키를
  갖지 않는다. 라이브러리 추가 없이 같은 목적을 달성하지만 `gateway.py` 의 "각 모듈이 동일
  JwtSecret 로 독립 검증" 불변식을 개정해야 한다(모듈 bind 범위·직접 접근 차단 검토 동반).
- **이번 범위에서 하는 완화**(라이브러리 불요):
  - 배포 설정 조회 API 가 시크릿을 **평문 반환**하고 있다(`_get_deployment_config` 가 overlay
    그대로). P0-6(부분 병합 저장)이 선행되면 조회 시 마스킹으로 바꿀 수 있다.
  - 시크릿을 담은 `config.json` 파일 권한·로그 노출 점검.

**주의**: 이 항목을 보류해도 §5 의 **그룹 공통 신원(같은 `JwtSecret` 을 양 노드에 배포)은
그대로 필요**하다 — 없으면 절체 후 전 세션 무효 + 모듈 401. 둘은 별개 문제다.

### 5.2 인증서 수명주기 — 발급자와 소비자를 나눈다

**모듈은 인증서를 만들지 않는다.** 관리평면 3모듈(`oam`·`oam-svc`·`csc`)은 전부 HTTPS 로
떠야 한다 — agent 의 health-gate 가 HTTPS 전용이고, 게이트웨이는 업스트림을 `https://` 로
등록한다(`register_module_routes`). 발급 주체가 모듈 자신이면 **부트스트랩 순환**이 생긴다.

| 역할 | 주체 | 근거 |
|---|---|---|
| **트러스트 앵커(그룹 CA)** | 그룹 자산. 설치·join 때 노드에 심긴다 | 절체해도 같은 CA 서명이라 브라우저 경고 없음 |
| **발급·배치** | **lifecycle 엔진**(`cims-svc` → `agent/lib/cert.sh`) — 모듈 기동 **전** | 모듈보다 항상 먼저 실행, 노드 로컬 특권, `ha.json` 으로 VIP 를 알아 SAN 계산 가능 |
| **CA 생성** | 같은 엔진. 없으면 만들고, **join 이 심어둔 CA 가 있으면 그대로 쓴다** | 두 노드가 같은 CA 라야 절체 후에도 브라우저가 CA 하나만 신뢰하면 된다 |
| **소비** | 모듈 — 정해진 경로를 읽기만 한다 | 발급 시점·순서와 무관해진다 |

발급 위치는 **버전무관** `<component_root>/../../runtime/cert/{server.key,server.crt}`
(key `0600`)다. 배포본은 `<prefix>/modules/<mod>/runtime/cert`, 개발 서버는
`<repo>/build/runtime/cert` 로 같은 규칙에서 유도된다. 버전 디렉터리에 두면 업그레이드가
그 디렉터리를 갈아치우며 인증서가 사라진다.

그룹 CA 는 `<oam 모듈>/runtime/_secrets/ca/{ca.crt,ca.key}`(key `0600`, 디렉터리 `0700`)
에 둔다 — 어느 모듈을 띄우든 **노드 단위로 같은 자리**를 보고, §9 join 이 피어에 복사하는
경로와 같다. 서버 인증서는 이 CA 로 서명한다(유효기간 825일). CA 를 만들 수도 읽을 수도
없으면 self-signed 로 내려간다 — 기동을 막지 않는 것이 우선이고 브라우저 경고만 감수한다.

보증은 기동마다 실행되며 세 갈래다:

| 상태 | 처리 |
|---|---|
| 인증서 없음 | 발급 |
| 있고 SAN 부족 + **CIMS 발행**(`O=CIMS` 또는 `CIMS-OAM-CA` 서명) | **재발급** — VIP 추가·접속 주소 변경을 추종 |
| 있고 **운영자 상용 인증서** | 손대지 않는다. SAN 이 부족하면 경고만 (그 주소로 접속 시 브라우저 경고가 난다는 안내) |

SAN 이 충분하면 아무것도 하지 않으므로 재기동이 인증서를 갈아치우지 않는다. `openssl` 이
없거나 발급이 실패하면 경고만 남기고 통과한다 (모듈 자체 폴백이 뒤를 받쳐 기동을 막지
않는다).

모듈 쪽에는 발급 코드가 없다. `oam_app` 의 SAN 재발급(`_ensure_cert_sans`)과 그룹 CA
생성·서명 헬퍼는 제거했다 — 기동 중에 재발급하면 이미 뜬 모듈과 인증서가 갈린다.
`_resolve_oam_cert` 의 self-signed 생성만 엔진을 거치지 않은 기동(수동 실행)용 최후
폴백으로 남는다.

**핫리로드** — SAN 이 바뀌는 사건(VIP 부여·접속 주소 변경)은 모듈이 이미 떠 있을 때도
일어난다. 그래서 `httpsrv.HttpServer` 가 uvicorn 의 `SSLContext` 를 잡아 두고 인증서
파일(mtime·size)이 바뀌면 `load_cert_chain` 으로 교체한다 — **이후 handshake 부터** 새
인증서가 쓰이고 기존 연결은 끊기지 않는다. 주기는 30초(`CIMS_CERT_WATCH_SEC` 로 조정).

교체는 두 겹으로 보호한다. 키와 인증서는 한 번에 바꿀 수 없으므로 "짝이 안 맞는" 창이
반드시 생기는데,

1. **엔진**은 새 파일을 만든 뒤 이동한다(`.new.*` → `server.*`) — 창을 rename 두 번으로 줄인다.
2. **모듈**은 버리는 `SSLContext` 에 먼저 적재해 **검증한 뒤에만** 실제 컨텍스트에 적용한다.
   `load_cert_chain` 은 키가 맞지 않으면 예외를 던지지만 **그 전에 인증서를 이미 갈아
   끼우므로**, 살아있는 컨텍스트에 바로 적용하면 예외를 잡아도 그 뒤 모든 handshake 가
   실패한다(실측). 검증에 실패하면 기존 인증서를 유지하고 다음 주기에 재시도한다.

SAN 은 `hostname` · `127.0.0.1` · 노드 IPv4 · **`ha.json` 의 VIP** 를 처음부터 담는다.
VIP 가 빠지면 oam 이 기동 시 SAN 부족으로 그룹 CA 재발급을 하는데, 그때 **이미 뜬
oam-svc 는 옛 인증서를 계속 서빙한다**(모듈에 핫리로드가 없다) — 애초에 맞는 SAN 으로
만들어 그 왕복을 없앤다.

> **왜 이 구조인가** — 원래 설계가 이랬다. 부트스트랩 `install.sh` 가 발급하고 모듈은
> 읽기만 했다. 콘솔 설치 경로가 생겼을 때 그 경로엔 발급자가 없어 배포가 `deploying` 에
> 고착했고, 대응으로 oam 의 인증서 해석 **최후순위에 self-signed 자동 생성**이 들어갔다.
> 임시 폴백이 그대로 남아 "oam 이 발급자" 처럼 굳은 것이 사고의 뿌리다: oam 은 자기 기동
> **끝자락**에 인증서를 만들므로(실측 +3초), 그 사이에 뜬 oam-svc 는 평문으로 bind 하고
> 다시 확인하지 않는다. 기동 순서는 set 순회라 절체마다 달라져 재현이 들쭉날쭉했다.
> 실측: 첫 승격 노드에서 oam-svc(14:45:26) → oam(14:45:27) → 인증서(14:45:30) 순으로
> 2초 차이로 지면서 `/api/v1/stats` 등 9개 세그먼트가 `RECORD_LAYER_FAILURE` 로 불통.

**남은 과제**
- **게이트웨이 업스트림 스킴 실측** — 지금은 `https://` 고정이라 업스트림이 평문이면
  세그먼트가 통째로 죽는다. agent 의 health 프로브는 이미 https→http 폴백을 한다.
- **패키지 동봉 인증서 제거** — `csc/cert/{server.key,server.crt}` 가 레포에 커밋돼 패키지로
  나간다. 모든 설치가 같은 개인키를 쓰므로 회전 대상이다. 노드 인증서 보증이 자리잡으면
  동봉본은 불필요하다(`csc` 는 이제 노드 인증서를 먼저 본다).

## 6. HA 편입 (D5·D6)

### 6.1 service descriptor

`oam`/`oam-svc` 는 descriptor(`service_descriptors_seed/cims.json`)에 1급 등록되어 있다.
descriptor 에 없으면 `_agent_daemon_modules` 가 포트 있는 모듈만 뽑으므로
`cold_modules`·`relevant_modules`·헬스 대상에서 영구 제외된다 — `pkg.json` 의
`ha_capability=active_standby` 는 install 허용 플래그일 뿐 HA 관리와 무관하다.

```jsonc
{ "name": "oam", "port": 4419, "proto": "tcp", "controllable": true,
  "health": { "config_key": "Server.Port", "http_path": "/health" } },
{ "name": "oam-svc", "port": 4480, "proto": "tcp", "controllable": true,
  "health": { "config_key": "Server.Port", "http_path": "/health" } }
```

- `_HEALTH_MODULE_PRIORITY` 에서 `oam`/`oam-svc` 는 **맨 뒤**다 — 서비스 모듈과 동거하는
  그룹에서 대표(`health_module`)를 가로채지 않게. 모듈별 감시는 `module_health` 맵이 담당한다.
- **descriptor 업서트 마이그레이션**: `seed_if_empty` 는 store 가 비었을 때만 주입하므로,
  기동 시 `merge_seed_modules` 가 같은 `id` descriptor 에 **없는 모듈만 추가**한다(기존 엔트리·
  label·alert_rules 등 운영자 편집 보존, 멱등). 이게 없으면 신규 모듈이 기존 노드에서 영구히
  descriptor 밖에 남아 HA 대상이 되지 못한다.

### 6.2 module_specs

안전 등급은 **descriptor 가 데이터로 선언**하고(`modules[].safety`), 그룹의
`module_specs.<mod>` 가 있으면 키 단위로 덮는다 — 코드 상수가 아니므로 그룹마다 수동 설정
없이도 올바른 등급이 적용된다.

```jsonc
// descriptor (기본값)                     // 실효 명세 (그룹 미설정 시)
"safety": { "class": "shared_writer",      ha:     { failover_mode: cold, failover_relevant: true }
            "requires_leader_lease": true } safety: { class: shared_writer,
                                                     latch_clear_mode: manual,   // class 에서 파생
                                                     requires_leader_lease: true }
```

`requires_leader_lease` 는 §4.4 리스와 연결된다. `latch_clear_mode` 는 class 에서 파생하므로
데이터에 중복 선언하지 않는다(운영자가 class 를 바꾸면 파생값도 따라간다). `manual` 이므로
래치 영속화가 전제이며, 그것은 §10 에 반영돼 있다.

### 6.3 선언 집행 — 전제 미충족이면 HA 에 넣지 않는다

`requires_leader_lease` 는 **선언으로 끝나면 안 된다.** 선언만 있고 집행이 없으면
`oam`/`oam-svc` 가 공유 store 없이도 cold 모듈로 편입되고, 절체하면 신 Active 가 **자기 노드의
빈 store** 를 들고 뜬다 — 콘솔에는 서버·그룹·배포가 전부 사라진 화면이 보인다. 그래서 판정을
모듈 이름이 아니라 **선언을 키로** 한 게이트로 집행한다.

```text
전제 미충족(_lease_precondition_unmet) =
  실효 명세의 safety.requires_leader_lease 가 true 이고
  그룹 mode == active_standby 이고
  그룹 shared_store 가 유효하지 않다      → 'no_shared_store'
```

집행 지점은 **위험한 액션**이다. 설치는 위험하지 않다 — 설치만 된 standby 는 렌더 제외로
HA 가 건드리지 않으므로 승격돼도 기동되지 않는다. 위험한 것은 **공유 store 없이 두 노드에서
동시에 기동**하는 것이고, 거기서 막는다.

| 층 | 동작 |
|---|---|
| **렌더** (`_render_ha_for_agent`) | 해당 모듈을 `cold_modules`·`relevant_modules`·`module_health`·`health_module` 후보에서 제외. 사유를 ha.json 서비스 엔트리 `ha_excluded{모듈: 사유}` 와 그룹 조회 응답에 실어 보내고 경고 로그를 남긴다 |
| **설치** (`_create_deployment`) | **허용하고 알린다** — 응답 `warning`/`warning_code=leader_lease_precondition` + 서버 경고 로그. 콘솔은 이 경고를 모듈 추가 직후 띄운다 |
| **기동** (`_queue_job`, start·restart) | 같은 그룹의 **다른 노드에서 그 모듈이 이미 running/deploying** 이면 409 `leader_lease_precondition`. 상대가 정지 상태면 허용(공유 store 없는 수동 이관 경로 §9.4). `force: true` 로 우회 가능 |

설치를 막지 않는 이유는 두 가지다. ① 설치만 된 상태는 렌더 제외로 **무해**하다.
② 공유 store 설정은 인프라 준비(마운트)에 딸린 작업이라, 설치를 그보다 먼저 요구하면
**이중화 구축 순서가 뒤집혀** 아무것도 진행할 수 없다.

- 조용히 빠지면 운영자는 이중화가 되는 줄 안다. 그래서 제외는 **반드시 사유와 함께** 노출되며,
  콘솔 HA 화면(공유 store 패널)이 "이 모듈은 이중화되지 않습니다 — oam, oam-svc" 로 경고한다.
- 공유 store 를 설정하면 다음 렌더에서 자동 편입된다. 별도 조작이 없다.
- 그룹 `module_specs.<mod>.safety.requires_leader_lease=false` 오버라이드는 그대로 존중한다 —
  운영자가 명시적으로 "이 모듈은 리스가 필요 없다" 고 선언한 경우.
- `csc` 는 DB primary 라 `requires_leader_lease` 가 아니므로 이 게이트와 무관하다.
- 안전 가드의 409 는 **막다른 골목이 아니다** — 콘솔이 사유를 보여주고 강행 여부를 묻는다
  (`force: true`). 가드는 기본을 안전하게 두는 장치이고 판단은 운영자 몫이다.

### 6.4 관리평면 자기보존 — cold 규칙의 예외

`oam` 은 cold 모듈이라 "MASTER 가 아니면 정지" 규칙을 그대로 받는다. 그런데 관리평면을 끄는
순간 **래치를 풀거나 그룹을 시작할 통로(콘솔) 자체가 사라진다.** 실측 데드락:

```text
oam 재기동(설정 저장 → 인증서 재발급으로 콜드스타트 20초+)
  → 기동 유예 3초 상수로 판정 → "프로세스는 있고 readiness 실패" = zombie 오판
  → 절체 래치(FAILOVER_LATCHED, reasons=["zombie:oam"])
  → vrrp_eligible=false → keepalived VIP 없음 → role=FAULT
  → cold 규칙: MASTER 아니면 정지 → reconcile stop oam 반복
  → 래치는 shared_writer 라 latch_clear_mode=manual → 자동 해제 없음
  → **래치를 풀 콘솔이 없다** = 복구 불가
```

두 층에서 끊는다.

**① 좀비 판정은 시간이 아니라 readiness 관측으로 한다.** 좀비 = "프로세스는 살아있는데 준비가
안 됨" 인데, 이를 **경과 시간**으로만 재면 콜드스타트가 느린 모듈이 오판된다. 실측 사고:
공유 store(NFS)로 옮긴 뒤 관리평면 콜드스타트(마운트 확인·리스 획득·마이그레이션·CA/인증서
재발급·bind)가 **75초**를 넘겨, 60초 유예 직후 좀비로 찍히고 **절체 래치**가 걸렸다 —
승격은 성공했는데 전 모듈이 정지하고 절체가 실패했다.

지금 판정은 **이번 기동에서 readiness=True 를 본 적이 있는가**다.

```text
readiness 를 본 적 없음 + 상한 미경과   → starting     (콜드스타트 진행 중)
readiness 를 본 적 없음 + 상한 경과     → start_failed (아예 못 뜬다 = 실패)
readiness 를 봤다가 실패로 바뀜         → zombie       (로컬 복구가 못 고침)
```

상한(`startup_grace_sec`, oam/oam-svc=180초)은 "그래도 안 뜨면 실패로 본다"는 **백스톱**이고
1차 기준이 아니다. 재기동하면 관측 기록이 초기화된다.

**①-1 모듈별 기동 유예(상한).** 옛 구현은 `_OP_GRACE_SEC`(3초) 하나로 모든 모듈을 재던 것이
결함이었다. CSP/CMP 는 바인드가 1초 내지만 관리평면은 python 콜드스타트 + config 로드 +
store 마이그레이션 + CA/인증서 발급 + bind 로 20초를 넘긴다(자기 health-gate 기본값도
`CIMS_OAM_HEALTH_TIMEOUT=20`). 이제 `module_health[mod].startup_grace_sec`(descriptor
`health.startup_grace_sec`, oam/oam-svc = 60)를 쓰고, **연속 생존 시간**이 그 값 미만이면
`starting` 으로 본다. 명시값이 없어도 `http_path` 를 쓰는 모듈은 기본 60초다(HTTP 200 은
초기화 완료를 뜻하므로 느리다) — descriptor 가 그 필드를 갖지 않은 기존 설치본 대비 이중
방어이며, `merge_seed_modules` 가 기존 descriptor 의 `health` 에 **없는 키만** 채워 소급
적용한다(운영자 값은 덮지 않는다).

**② 자기보존(정지 보류).** cold 규칙의 목적은 **두 노드 동시 기동 방지**다. 상대가 그 모듈을
서비스하고 있지 않다면 나까지 내려서 얻는 것이 없다. 대상은 **복구 통로를 제공하는 모듈**,
즉 콘솔을 서빙하는 base `oam` 뿐이다(ha.json `console_modules`). `oam-svc` 는 게이트웨이 뒤의
서비스라 그것만 살아 있어도 콘솔이 열리지 않으므로 제외한다 — 초기 구현은
`requires_leader_lease` 전체를 대상으로 삼아, 승격 실패한 노드에 **oam-svc 만 남아 도는**
상태를 만들었다(실측).

```text
role == MASTER   → 기동 (변화 없음)
role == BACKUP   → **정지**. VRRP 가 상대를 Active 로 선출했다는 뜻이다 — 여기서 붙잡으면
                   구 Active 가 리스를 놓지 않아 **신 Active 가 read-only** 로 뜬다(실측).
                   절체의 목적이 이관이므로 물러난다; 복구 통로는 신 Active 가 제공한다.
role == FAULT / UNKNOWN     → 아무도 Active 가 아닐 수 있다
  상대가 콘솔 서비스 중       → 정지 (정상 강등)
  상대 미서비스 / 판정 불가  → **정지 보류** + 경고 로그 (콘솔 유지)
```

상대 서비스 여부 판정에 쓰는 `peer_ip` 는 **ha.json 최상위**에 있다(서비스 엔트리에는 없다).
엔트리에서 읽어 항상 '판정 불가'가 되던 버그가 이 사고의 직접 원인이었다.

상대 서비스 여부는 **노드 로컬 판정**이다 — `peer_ip` + `module_health[mod].port` 로 직접
TCP 접속을 시도한다(크로스노드 시각 비교 없음, §15). 판정 불가(주소·포트 미상)는 보존 쪽으로
기운다. 동시 기동 위험은 **소유권 리스**가 담당한다: 리스를 못 잡은 쪽은 read-only 로
강등되므로 데이터는 갈라지지 않는다(§4.4). 즉 "VIP 없으면 아무데서도 관리평면이 안 뜬다"를
"관리평면은 최소 한 노드에서 뜨고, write 는 리스가 통제한다"로 바꾼다.

### 6.5 reconcile 과의 정합 (검증 결과)

| 항목 | 상태 |
|---|---|
| `_pgrep_module` 이 `oam`/`oam-svc` 를 인식 | **이미 지원** (`<stem>_app.py` 매칭, 하이픈→언더스코어 정규화) |
| `_NON_DAEMON_MODULES = {agent, console}` | oam 미포함 → reconcile 대상 가능 |
| legacy watchdog 과의 이중 제어 | `supervise_tick` 이 `_ha_managed_modules()` 를 skip → 부트스트랩이 심은 `supervised.json` 의 `oam` 항목은 자동 무효화 |
| cold 기동 경로 | `lifecycle.sh start_oam` 존재 + `_oam_preflight` 로 자기검증 |
| 자기 자신 정지 | 강등된 노드의 agent 가 자기 노드 OAM 을 정지 — job 을 주던 OAM 이 사라지지만 Supervisor 는 로컬 판정이라 계속 동작(§9 watchdog 조건에서 OAM 연결 제외) |
| job 결과 유실 | `_deliver_report` 재시도 + `_flush_pending_reports` 로 신 Active 에 뒤늦게 전달 |

### 6.6 `OAM_ROLE` 배포 스펙화 (D6)

역할의 SoT 는 **배포 설정 `Server.Role`**(oam `config_template`, 기본 `base`)이고,
`lifecycle.sh start_oam` 이 배포 config 에서 읽어 `--role` 을 정한다. 우선순위는
**env `OAM_ROLE`(개발 오버라이드) > `Server.Role` > `all`(코드 기본)** 이며, 값이
`base`/`all` 이 아니면 경고 후 `all` 로 폴백한다.

옛 구조에서는 `OAM_ROLE=base` 가 **부트스트랩이 만든 systemd drop-in** 에만 있고 reconcile
기동은 환경변수를 상속했다(`_run_cims_svc` 의 `env=dict(os.environ)`) — drop-in 이 없는 노드가
승격되면 `role=all` 로 떠서 **게이트웨이 프록시를 아예 마운트하지 않아**(`oam_app.py`:
`role=all` 은 서비스 핸들러 in-process) 승격 직후 서비스 API 가 전면 장애났다.

## 7. 계획 절체 — 신 Active 가 이어받는다 (D7)

관리평면의 계획 절체는 **source 가 자기 자신을 정지시키는** 유일한 경우다. 자동 장애 절체는
OAM 에 의존하지 않으므로(로컬 agent + keepalived) 그대로 성립하지만, 계획 절체는 OAM
operation 상태머신이 구동하므로 다음이 필요하다.

```text
nodeA(OAM Active)                       nodeB
 ├ op 생성(RELEASING) → 공유 store
 ├ target 홀드 선해제 job → nodeB agent
 ├ planned_release job → nodeA agent
 │    → verdict eligible=false → track_script fail → FAULT → VIP 반납
 │    → role MASTER→BACKUP → reconcile 이 nodeA 의 oam/oam-svc 정지 ← 오케스트레이터 소멸
 │                                       ├ keepalived MASTER → VIP 인수
 │                                       ├ reconcile 이 oam/oam-svc 기동 (승격 grace)
 │                                       ├ 리스 획득(epoch+1)
 │                                       └ sweep_ha_operations 가 공유 op 를 resume
 │                                          → WAIT_VIP_MOVE → VERIFYING → COMMITTED
 │                                          → clear_source job → nodeA planned_release 해제
```

보정 규칙:

1. **관측 불가는 롤백 사유가 아니다** — "target 이 VIP 를 못 잡았다" 는 **확정 관측**이
   있을 때만 롤백한다. 신 Active OAM 이 막 뜬 직후에는 heartbeat 수집 창 때문에
   `vip_observation` 이 `active_agent_id=None` 을 낼 수 있는데, 옛 구현은 그것을 그냥
   타임아웃으로 세어 **이미 정상 완료된 절체를 `ROLLED_BACK` 으로 오기록**했다.
   - `active is None` → `_OP_OBSERVE_GRACE`(기본 180s) 안에서는 **대기**
   - 그 창을 넘기면 롤백이 아니라 `FAILED(observation_unavailable)` 로 종결(실제 VIP 위치를
     확인하라는 note 를 남긴다) — 잘못된 방향으로 되돌리지 않는다
   - `VERIFYING` 중의 관측 불가도 동일(실패 아님, 유예 내 대기)
   - 이 규칙은 모든 op 에 공통 적용되고, 관리평면 op 은 `self_orchestrated: true` 로 표시된다
2. **`clear_source` 는 신 Active 가 보낸다** — 공유 store 라 가능하다. 종결 전이 시점의
   인라인 전송(COMMITTED 포함)이 그대로 성립한다.
3. **콘솔 세션 단절은 정상 동작이다** — 토큰은 그룹 공통 서명키라 재로그인 없이 유지되고,
   브라우저는 VIP 로 재접속하면 신 Active 가 응답한다.
4. `nopreempt` 로 fail-back 이 없으므로 역방향 절체는 다시 계획 절체로 수행한다 —
   `planned_release` 가 COMMIT 에서도 해제되어야 한다는 기존 규칙이 그대로 적용된다.

### 7.1 self-upgrade 순서 — standby 먼저

관리평면은 **자기 자신을 업그레이드하는 유일한 모듈**이다. Active 를 먼저 올리면 새 버전이
기동에 실패할 때 콘솔이 사라져 **롤백을 지시할 통로가 없다**. 안전한 순서:

```text
① standby 에 upgrade  →  ② 정상 기동·standby_ready 확인  →  ③ 수동(계획) 절체
                       →  ④ 구 Active 에 upgrade         →  ⑤ (원하면) 역방향 절체
```

`POST /deployments/{id}/job {job_type:"upgrade"}` 는 **그 노드가 현재 Active 이고 standby 가
있으면 409 `upgrade_order_active_first`** 로 거부하고 순서를 안내한다(`force:true` 로 우회
가능 — 판단은 운영자 몫이되 기본은 안전한 순서다). 단일 노드·standby·Active 판정 불가에는
제약이 없다.

> 전 과정을 하나의 API 로 자동화하는 것(설치→검증→절체→구 Active 업그레이드)은 **의도적으로
> 넣지 않았다**: 오케스트레이터 자신을 교체하는 절차라 자동화의 실패 모드가 "콘솔 없음" 이고,
> 그 상태에서는 자동 복구를 지시할 주체도 없다. 순서 가드 + 운영자 확인이 더 안전하다.

## 8. 주소·네트워크 (D8)

| 항목 | 현재 | 목표 |
|---|---|---|
| VIP | 서비스 그룹 VIP(.140) 그대로 | **신규 VIP 없음.** 관리평면이 그 VIP 를 공유하므로 slot·NIC 매핑·`agent.md` 원칙 모두 무변경 |
| agent 의 OAM 주소 | unit 인자 + **상태 파일**(`<state-dir>/oam_url`, 우선) | `POST /agents/oam-url`(전체) 또는 `/agents/{id}/oam-url` → `set_oam_url` job. **각 agent 가 새 주소로 `/health` 를 찔러 도달 확인한 뒤** 기록·재기동하므로, VIP 가 아직 없으면 주소를 바꾸지 않고 실패한다(fleet 단절 방지). agent 자기 업그레이드에도 상태 파일이 보존된다 |
| `detect_mgmt_ip` | egress 주소가 그 NIC 의 **secondary(VIP)** 면 같은 NIC 의 **primary** 로 보정 | 보정 없으면 VIP 보유 노드에서 mgmt IP 가 VIP 로 잡혀 인터페이스 배지·`Mgmt.Cidr` 판정이 절체마다 바뀐다 |
| `Server.Ip` | **VIP 입력 거부**(400 `bind_ip_is_vip`) — HA 그룹 VIP 집합과 대조 | VIP 를 보유하지 않은 노드에서 bind 실패 → watchdog 이 같은 설정으로 재기동 반복 = 기동 불능 루프(관리평면이면 콘솔 소멸). `0.0.0.0` 이 정답 |
| 콘솔 접속 주소 | 설정값 없음(오해 소지) | UI 에 "콘솔 주소는 설정이 아니라 접속한 IP" 명시 + VIP 안내 |
| `AgentOamUrl` | 노드 IP | 그룹 VIP. `Mgmt.Cidr` 검증 대상에 VIP 포함 |
| oam-svc 배치 | bind `127.0.0.1`, `Server.GatewayHost` 필드 없음 | **base 와 동거**(같은 노드 쌍, 함께 cold) → upstream 은 항상 `127.0.0.1`. `GatewayHost` 필드는 원격 배치 예외용으로 추가하되 기본 공란 |

## 9. 설치 — join 모드 (D9)

콘솔 배포 경로로 2번 노드에 `oam` 을 설치하는 것은 **성립하지 않는다**: `_infra` 값(시크릿·
런타임 경로)은 부트스트랩이 1번 노드의 deployment overlay 에만 넣고, 템플릿 default 가 전부
빈 값이라 `_template_defaults` 가 제외한다 → 반드시 잘못된 설정으로 뜬다.

### 9.1 합류 토큰 발급 (1번 노드)

```bash
# admin 인증 필요. 1회용 + TTL(기본 900초). 평문은 이 응답에만 나온다(저장은 sha256).
curl -sk -X POST https://<nodeA>:4419/api/v1/ha/join-token \
     -H "Authorization: Bearer <admin-token>" -H 'Content-Type: application/json' \
     -d '{"ttl_sec":900}'
# → {"token":"…","expires_at":"…"}      발급/사용 이력: GET /api/v1/ha/join-token
```

### 9.2 2번 노드 합류

```bash
sudo ./install.sh --join \
     --peer-url https://<nodeA 또는 VIP>:4419 \
     --join-token <위 토큰> \
     --runtime-dir  /opt/cims-agent/modules/oam/shared/runtime \
     --runtime-mount /opt/cims-agent/modules/oam/shared \
     --mgmt-ip 121.161.164.137 --server-name ctrl02
```

절차: ① 패키지 전개 ② peer 에서 **그룹 공통 신원 수령**(`POST /api/v1/ha/join` — 1회용 토큰
인증, 응답에 JwtSecret·admin 계정·그룹 CA·mTLS CA + **agent enrollment token**) ③ 신원을
**노드 로컬** `_secrets/`(0700, key 0600)에 전개 — 개인키는 공유 store 에 두지 않는다 ④ `oam.json`
구성(peer 와 같은 포트·역할·AgentOamUrl·Mgmt.Cidr·SAN + `CimsRuntimeDir`/`CimsRuntimeMount`)
⑤ **OAM 을 기동하지 않는다**(cold standby 이고, 마운트 없이 뜨면 로컬 store 를 만든다)
⑥ agent 설치·enroll — **대상은 peer/VIP** 다(이 노드 OAM 이 아니다).

콘솔 배포 경로로 두 번째 노드에 `oam` 을 설치하는 것은 성립하지 않는다: `_infra` 값(시크릿·
런타임 경로)은 부트스트랩이 1번 노드의 deployment overlay 에만 넣고 템플릿 default 가 전부
빈 값이라 `_template_defaults` 가 제외한다 → 반드시 잘못된 설정으로 뜬다.

### 9.3 합류 후 (콘솔)

1. 공유 store 마운트 확인 — 이 서버에 상대 노드와 **같은 경로**가 붙어 있어야 한다
   (콘솔 시스템/인프라 > 서버 > 마운트 관리 로 추가하면 fstab 에 영속)
2. HA 그룹에 이 서버를 멤버로 추가 + **공유 store** 설정 (그룹 편집 → "공유 store")
3. 이 서버에 `oam`/`oam-svc` 패키지 설치 — 배포설정에 그룹 공통 신원이 자동 주입된다(§5)
4. 그룹 서비스 시작 → VIP 보유 노드에서만 OAM 이 뜬다
5. 전 agent 를 VIP 로 재지정 — `POST /api/v1/agents/oam-url`(§8). 각 agent 가 도달 확인 후 전환

### 9.4 1번 노드 전환 (단일 → 이중화) — 콘솔 원클릭

단일 노드로 운영하던 store 는 **노드 로컬**에 있다. 경로만 공유 마운트로 바꾸면 데이터가
따라가지 않아 빈 콘솔이 되므로 **이관**이 필요하다. 이 작업은 콘솔 한 번의 조작으로 끝난다:

```text
HA 그룹 상세 편집 > 공유 store > 경로 입력 > [이 경로로 이관]
  → POST /api/v1/ha-groups/{id}/shared-store/migrate  {mount_point}
```

서버가 하는 일 (`_migrate_shared_store`):

| # | 동작 |
|---|---|
| 1 | 그룹에 `shared_store` 저장 — 이 시점부터 oam/oam-svc 가 HA 편입 대상(§6.3) |
| 2 | 멤버의 oam/oam-svc 배포 overlay 에 `CimsRuntimeDir`(=`<mount>/runtime`)·`CimsRuntimeMount` 병합 — **현재 store 에 기록**되므로 3의 복사에 함께 실려 신 store 와 일관해진다 |
| 3 | store 를 들고 있는 노드(`status=running`)에 `migrate_oam_store` job |
| 4 | 나머지 멤버는 `update_config` 만 — 그 노드는 같은 공유 store 를 읽게 된다 |

agent 가 하는 일 (`job_migrate_oam_store`) — **OAM 은 자기 store 를 자기가 옮길 수 없다**
(복사 중 자기가 떠 있으면 write 가 섞이고, 자기를 멈추면 이어서 지시할 통로가 없다). agent 는
OAM 수명과 무관하고 이미 그 모듈의 lifecycle 을 소유하므로 이 작업의 주체다.

```text
1. 전제 확인 — target_mount 가 실제 마운트인지 + write 가능한지
   (실패하면 모듈을 건드리지 않고 즉시 실패 — 가용성 손실 없음)
2. op grace 표시 — watchdog·reconcile 이 복사 중 끼어들지 않게
3. 모듈 정지 → 4. 복사 → 5. config.json 기록 → 6. 모듈 기동
```

- **source 가 항상 이긴다 — 묻지 않는다.** 이관은 "지금 도는 OAM 의 store 를 이 위치로
  옮긴다" 는 뜻이고 source 는 정의상 살아있는 정본이다. target 에 있는 것은 이전 시도의
  잔재이거나 부분 복사본이며 정본일 수 없다(정본이었다면 OAM 이 이미 그것을 쓰고 있어
  `source == target` 이 되고, 그 경우는 거부된다). 그래서 **확인 없이 덮는다.**
  - 초기 구현은 "없는 항목만 복사"로 멱등을 노렸다가 **낡은 store 를 정본으로 승격**시켰다
    (실측 사고: 전날 시도가 남긴 snapshot 때문에 agent 6대 중 2대만 든 store 로 OAM 이 붙어
    전 노드 heartbeat 401 → 콘솔에 2대만·전부 offline, 모듈 상태는 전날 값).
  - 그 다음에 넣은 "운영자에게 확인" 도 같은 실수였다 — **코드가 답을 아는 것을 물어보면
    안 된다.** target 이 이겨야 하는 시나리오는 존재하지 않는다(백업에서 되살리는 것은
    이관이 아니라 복원이며 별개 기능이다).
  - 다만 **지우지는 않는다**: 기존 target 은 `<target>.stale-<시각>` 으로 보관하고, 양쪽
    레코드 수를 job 로그에 남긴다.
- `_secrets/`·`cert/` 는 **제외** — 개인키는 노드 로컬에 남는다(§5).
- 복사·config 기록이 실패하면 **구 설정으로 되돌려 기동**한다. 데이터도 가용성도 잃지 않는다.
- 3~6 동안 OAM 이 재기동되므로 **콘솔이 30초 내외 끊긴다**(정상). job 결과는
  `_deliver_report` 재시도 + `_flush_pending_reports` 로 신 OAM 에 늦게라도 전달된다.

**경로만 저장하는 실수를 막는다.** 그룹 `shared_store` 를 PUT 으로 저장할 때, 멤버의
oam/oam-svc 배포설정 `CimsRuntimeDir` 이 그 마운트 하위가 아니면 **409 `store_path_not_shared`**
로 거부한다(`_store_path_conflicts`). 그대로 두면 HA 편입은 되는데 데이터는 노드별 로컬이라
절체 시 빈 콘솔이 되는데, 이는 정확히 과거 사고 상태다. 콘솔은 이 응답을 받으면 이관을
권하고 동의 시 그대로 이어서 실행한다(막다른 골목을 만들지 않는다).

**기본 경로는 "옵션 없는 install.sh + 콘솔 클릭"** 이다. 부트스트랩은 노드 로컬 store 로
설치되고(옵션 불필요), 이중화는 나중에 콘솔에서 이 이관 한 번으로 전환한다 — 설치 절차에
특별한 인자를 요구하지 않는다는 뜻이다. 처음부터 공유 마운트를 가리키고 싶으면
`--runtime-mount`/`--runtime-dir` 도 있지만(§9.2) 필수가 아니다.

이관 대상(복사를 수행할 노드)은 **1건이 반드시 선정된다**: hostname 일치 → `status=running`
→ 첫 배포 순. 대상 0건을 허용하면 경로만 바뀌고 복사가 조용히 빠져 절체 시 빈 콘솔이 된다.

## 9.4.1 agent 접속 주소 전환 — 절체의 필수 단계

절체는 VIP 를 옮기는 것이므로, agent 가 **구 Active 의 노드 IP** 를 보고 있으면 절체 후 그
주소가 죽어 **fleet 전체가 OAM 과 단절**된다. 실측 사고: 절체는 정상 완료(137 이 VIP 보유,
3개 모듈 기동, 리스 정상)됐는데 콘솔에 **전 노드 offline**, 모듈 상태는 절체 직전 값으로
고착됐다 — 6개 agent 가 모두 `--oam-url https://<01 노드 IP>:4419`(또는 loopback)를 보고
있었고, 신 Active 는 heartbeat 를 **한 건도** 받지 못했다.

이 전환은 백엔드 API 만 있고 **콘솔 화면이 없어서 운영자가 할 수 없었다**. 세 곳을 잇는다.

| 층 | 동작 |
|---|---|
| **보고** | agent 가 heartbeat 에 자기 `oam_url` 을 싣는다. OAM 은 그것을 agent 레코드에 보관하고 `GET /agents` 로 노출한다 — 이 값 없이는 어긋남을 감지할 수 없다 |
| **경고** | 그룹 조회 응답 `agents_not_on_vip[]`(VIP 가 아닌 주소로 보고하는 agent). 콘솔 HA 화면이 붉은 배너로 "이대로 절체하면 단절된다" 를 표시 |
| **차단** | `POST /ha-groups/{id}/failover` 가 사전 점검해 **409 `agents_not_one_vip`** 로 거부. 콘솔은 사유와 대상을 보여주고 **전환을 바로 실행**할 수 있게 한다. `force: true` 로 우회 가능 |
| **실행** | 콘솔 `[⇢ OAM 주소 VIP 전환]` → `POST /agents/oam-url {url}`. 각 agent 가 새 주소로 `/health` **도달 확인 후에만** 적용하므로, VIP 가 아직 없을 때 눌러도 fleet 이 끊기지 않는다 |

**판정은 관리평면(oam)을 호스팅하는 그룹에서만** 한다. agent 는 OAM 주소 하나만 보므로,
Signaling·Media 처럼 oam 이 없는 그룹의 VIP 와 비교하면 전원이 "어긋남" 으로 잡혀 **그 그룹의
절체까지 막힌다**(실측: Signaling 절체 시 agent 6대가 전부 경고에 걸려 테스트 불가). 그런
그룹은 절체해도 OAM 주소와 무관하다. 호스팅 여부는 그룹 멤버에 `oam` 배포(status≠removed)가
있는지로 판정한다.

loopback(`127.0.0.1`)도 어긋남으로 본다 — 그 노드 자신의 OAM 을 가리키므로 Active 가 바뀌면
역시 끊긴다. 보고가 없는 구 버전 agent 는 판정을 유보한다.

## 9.4.2 절체 래치 가시성

래치(`state/ha/latch/<svc>.json`)는 **노드 로컬 파일**이라 OAM 이 볼 수 없었다. 그래서 래치로
**승격 불가가 된 노드**를 콘솔이 표시하지 못했고, 운영자는 "절체를 눌렀는데 아무 일도 안
일어난다" 만 겪었다(실측). agent 가 heartbeat 에 HA 판정 요약을 실어 보낸다.

```jsonc
"ha_state": { "Control": { "role": "FAULT", "state": "FAILOVER_LATCHED",
                           "eligible": false, "reasons": ["latched"], "latched": true } }
```

OAM 이 agent 레코드에 보관하고 `GET /agents` 로 노출하며, 콘솔 HA 화면이 **"절체 래치 —
승격 불가: <노드>"** 를 사유와 함께 표시한다. 해제는 기존 경로(모듈 start/restart 또는
홀드 해제)를 쓴다.

## 9.5 설정 자가 복구 — "설정 하나로 콘솔을 잃을 수 없다"

관리평면은 **자기 자신이 복구 통로**다. 잘못된 설정으로 기동에 실패하면 그것을 되돌릴 화면이
같이 사라져 SSH 없이는 복구가 불가능해진다. 이 형태의 사고가 반복해서 났다 — store 경로
오지정, 마운트가 아닌 경로, 잘못된 포트·주소. 공통 구조는 언제나 같다:

```text
설정 저장 → 관리평면 기동 실패 → 되돌릴 통로 없음 → SSH
```

설정 키마다 가드를 붙이는 방식은 **새 키가 생길 때마다 다시 뚫린다**(실제로 그렇게 됐다).
그래서 설정 내용과 무관한 **한 곳**에서 막는다: `lifecycle.sh start_oam` 의 health-gate.

```text
기동 후 /health 200  → 그 설정을 config.json.last-good 으로 **승격**
기동 실패            → last-good 으로 되돌려 **1회 재기동**
                       실패 설정은 config.json.failed-<시각> 으로 보관
                       되돌린 사실을 config.json.rolled-back 마커에 기록
```

- **last-good 에는 성공한 설정만 들어간다** — 실패한 설정이 복구 기준이 되지 않는다.
- 현재 설정이 이미 last-good 이면 되돌리지 않는다 — 설정 탓이 아니므로 무한 롤백을 막는다.
- 되돌린 사실은 `GET /api/v1/gateway/health` 의 `config_rolled_back`(ISO 시각)으로 나가고,
  콘솔 상단에 **"설정 되돌림"** 배너가 뜬다. 조용히 되돌리면 운영자는 자기 설정이 적용된
  줄 알기 때문이다.
- 이 장치는 §4.3 mount guard·§6.4 자기보존과 목적이 같다(관리평면 가용성 보전). 다만 그
  둘은 특정 실패 원인을 다루고, 이것은 **원인을 몰라도 되는 마지막 안전망**이다.

## 10. 전제 — 이미 반영된 동작

관리평면을 HA 로 올리면 기존 HA 결함의 파급이 **콘솔 자체의 가용성**으로 바뀐다. 아래는 그
전제로서 현재 코드에 반영된 동작이다(§11 의 1단계 이후가 이 위에 올라간다).

| 동작 | 왜 관리평면 HA 의 전제인가 |
|---|---|
| `cims-ha install`/`config`/`apply` **세 단계 모두 실패 시 job 실패**. install 은 `dpkg-query` 상태로 멱등 판정하고, postinst 데몬 기동은 **유닛 단위 `systemctl mask`** 로 막으며(호스트 전역 `policy-rc.d` 아님), dpkg 락은 재시도 후 포기하고 실패를 5분 backoff 로 기억한다 | keepalived 없이 "VIP 적용 성공" 으로 보고되면 VIP 주인이 없어 **cold 인 OAM 이 어느 노드에서도 안 뜬다** = 콘솔 소멸. 반대로 호스트 전역 차단이 새면 **모든 패키지의 서비스 기동이 막힌다**(실측 사고) |
| `requires_leader_lease` **선언을 렌더·설치 두 층에서 집행** (§6.3) | 전제 없이 편입되면 절체 후 신 Active 가 **빈 store** 로 뜬다 = 콘솔에서 관리 데이터 전체 소실 (실측 사고) |
| store 경로 폴백이 **버전 무관 노드 로컬** + legacy 위치 1회 복구 (§4.3) | 폴백이 cwd 로 가면 store 가 `releases/<version>/` 안에 생겨 **업그레이드가 관리 데이터를 삭제**한다 (실측 사고) |
| job 실행이 **heartbeat 루프와 분리된 worker 스레드** | OAM 재기동 job 이 그 노드 heartbeat 를 끊으면 `vip_observation` 이 stale → 계획 절체 오판·auto-sync 오작동 |
| 재기동 backoff 는 **연속 생존 60초** 이후에만 리셋 | 수 초 생존 후 죽는 OAM crash-loop 이 스로틀 없이 반복되는 것을 차단 |
| failover 래치가 `state/ha/latch/<svc>.json` 에 **영속** | `shared_writer`(oam/oam-svc/csc)가 agent 재기동·재부팅만으로 승격 후보로 복귀하지 않게 |
| install/upgrade 성공 시 **`service.json` 시딩**(`update_module_spec` 자동 큐잉) | 갓 설치된 노드의 module_specs 가 기본값으로 남아 감시·cold/hot 이 노드마다 어긋나지 않게 |
| **SIGUSR1/SIGHUP 을 무시**(기록만) — `_install_signal_guards`, config 로드보다 먼저 설치 | agent 의 `update_config` 는 설정 파일을 쓴 뒤 모듈에 SIGUSR1 을 보낸다. 파이썬 기본 동작이 **프로세스 종료**라, 핸들러가 없으면 **oam 설정을 저장하는 것만으로 OAM 이 죽고**(실측 사고) 그 상태에서 되돌릴 통로도 없다. OAM 은 bind·store 경로·시크릿을 기동 시점에 읽어 부분 reload 가 안전하지 않으므로 반영은 명시적 restart 로 한다 |
| 배포설정 저장은 **변경분 병합**(명시 삭제 = `null`) + 조회 시 **시크릿 마스킹** | 빈칸으로 보이는 신원 필드를 저장해도 시크릿이 소실되지 않게 (소실 시 패키지 기본값 회귀 → 전면 401) |
| 승격 grace 가 그룹 설정 `failover_options.health.grace_sec` 를 따름 | 공유 store 확인 + OAM 콜드스타트가 옛 상수(20s)를 넘겨 **승격 직후 VIP 를 반납하는 flap** 이 나지 않게 |

## 11. 남은 작업

코드 구현은 완료됐다(§1~§10 은 현재 동작). 남은 것은 **사이트 작업과 실환경 검증**이다.

| 항목 | 내용 |
|---|---|
| 인프라 | 양 노드에 **같은 공유 경로 마운트** (콘솔 마운트 관리 → fstab `_netdev,nofail`). NAS 에서 `flock` 이 강제되는지 확인 — no-op 이면 리스가 무력해진다(§4.4, 기동 시 자동 검증돼 read-only 로 드러남) |
| 구성 | HA 그룹에 공유 store 설정 → 2번 노드 합류(§9) → `oam`/`oam-svc` 설치 → 그룹 시작 → agent 주소 VIP 전환 |
| 검증 | `S6-SCN-FAILOVER-OAM` 을 공유 store 붙은 2-node 에서 LIVE 실행 (§12) |
| 보류 | 토큰 서명 비대칭 전환 — 상위 결정 대기(§5.1). 그때까지 시크릿 마스킹·파일 권한만 적용 |

## 12. 검증 시나리오

`verify/lib/items/stage6/scn_failover_oam.py` (`S6-SCN-FAILOVER-OAM`) — **전제 점검이
FAIL 로 드러난다**: ha.json 에 oam 이 관리 모듈로 있는 구성인데 `shared_store` 스펙이 없거나
`module_health.oam`·`relevant_modules` 에 oam 이 빠져 있으면 FAIL(조용한 SKIP 아님).
관리평면 HA 미편입 환경은 SKIP. 절체 본체(아래 1~8)는 공유 store 가 붙은 2-node 실환경에서
수행한다. peer 의 OAM 포트가 닫혀 있는 것은 **정상**이다(cold standby — 승격 전까지 미기동).

1. **자동 절체** — Active 의 `oam` 프로세스 kill 반복(restart_limit 소진) → VIP 이동 →
   신 Active 콘솔 200 → **agent 재인증 없이** heartbeat 지속 → 게이트웨이 라우트 유효
   (`/api/v1/users` 200) → in-flight job 이어받기
2. **계획 절체** — 콘솔 수동 절체 → op 이 신 Active 에서 COMMITTED 로 종결(ROLLED_BACK
   오기록 없음) → source `planned_release` 해제 확인 → 역방향 절체 가능
3. **공유 store 확인 순서** — 승격 로그가 `VIP → store 확인 → 리스 → 모듈 기동` 순서임을
   확인. store 를 umount 시키면 모듈이 기동되지 않고 **승격 실패로 VIP 가 상대에게** 넘어간다
4. **접근 불가 노드 승격 금지** — 한 노드에서 NAS 를 끊은(또는 read-only 로 만든) 뒤 상대를
   kill → 그 노드는 승격되지 않고 VIP 공백(데이터 없이 서비스하는 것보다 안전)
5. **epoch fence** — 두 노드에서 동시에 OAM 을 띄워 강제 모의 → write 는 한 노드만 성공,
   다른 노드는 read-only + `CIMS-HA-LEASE` 알람
5b. **잠금 자기검증** — `nolock` 으로 마운트한 경로를 store 로 주면 기동 후 리스가
   `locking_not_enforced` 로 남아 **read-only** 다(조용히 write 하지 않는다)
6. **mount guard** — 마운트를 지운 채 OAM 기동 시도 → **기동 거부**(마운트 포인트 하부 로컬
   디스크에 빈 store 를 만들지 않음)
7. **유지보수** — 한 노드 EXCLUDE_NODE → 상대 노드로 승격되지 않음 확인, 양 노드 제외 시
   경고 노출
8. **모듈별 좀비 감지** — `oam` 포트를 살린 채 핸들러만 정지(SIGSTOP) → readiness 실패가
   `oam` 에 한정되어 잡히고, 셋 다 relevant 이므로 로컬 복구 소진 후 그룹 절체 (§3.1·§3.2)
9. **선언 집행** (§6.3) — 공유 store 없는 AS 그룹에서: ha.json 의 `cold_modules`·`module_health` 에
   `oam`/`oam-svc` 가 **없고** `ha_excluded` 에 사유가 있음 → 콘솔 공유 store 패널 경고 노출 →
   2번 노드에 `oam` **설치는 성공**하고 경고가 뜸 → 그 노드에서 `start` 시도 시
   (1번 노드 running 상태) **409 `leader_lease_precondition`**, `force` 로는 통과 →
   1번 노드 `oam` 정지 후에는 start 허용 → 공유 store 저장 후 다음 렌더에서 자동 편입(경고 소멸)
10. **store 위치** (§4.3) — `CimsRuntimeDir` 미지정으로 기동 → store 가 패키지 루트
    `runtime/` 에 생기고 `releases/<version>/` **안이 아님** → oam 업그레이드 후에도 관리
    데이터 유지

### 12.5 설정 계층 — 패키지 기본값 vs 노드 overlay

| 파일 | 역할 | 수명 |
|---|---|---|
| `<pkg>/oam/config/oam.json` | **패키지 기본값** — 노드 종속 값을 갖지 않는다 | 업그레이드가 교체 |
| `<pkg>/oam/config.json` | **노드/인스턴스 overlay**(flat dotted) — 경로·포트·시크릿·계정 | 버전 간 이관, OAM 이 SoT |

`load_config()` 가 base 를 읽고 그 위에 overlay 를 적용한다. **노드 값은 언제나 overlay 가
정한다** — 콘솔로 설치한 모듈이 쓰는 메커니즘이 이것이고, 부트스트랩도 같은 것을 쓴다.

옛 부트스트랩은 첫 기동을 위해 `oam.json` 을 **직접 고쳤다.** 그러면 같은 버전 패키지가
노드마다 내용이 달라지고, 무엇보다 **패키지 기본값의 결함이 부트스트랩 노드에서만 가려진다**:
실측 사고에서 패키지 `oam.json` 에 빌드 머신 절대경로가 들어 있었는데, 부트스트랩 노드는
덮어써서 정상이었고 **콘솔로 설치한 노드만** 그 경로로 기동하다 죽었다(→ 승격 실패 → 절체
미완 + 콘솔 소멸). 지금은 부트스트랩도 `config.json` 에만 쓰고 패키지 파일은 손대지 않는다.

### 13.0 배포 설정 이식성 — 빌드 머신 경로 금지

패키지에 실려 나가는 설정에 **빌드 머신의 절대경로**가 있으면 안 된다. 실측 사고:
`ems/core/oam/config/oam.json` 의 `CimsRuntimeDir` 에 개발 머신 경로
(`/home/<user>/work/cims/build/dist/ext_mnt/runtime`)가 커밋된 채 배포됐다. 부트스트랩
노드는 설치 스크립트가 그 값을 덮어써서 드러나지 않았지만, **콘솔로 설치된 모듈**은 패키지
기본값을 그대로 써서 OAM 이 그 경로에 `makedirs` 하다 `PermissionError` 로 죽었다 →
`/health` 무응답 → 승격 실패 → role FAULT → **수동 절체가 완료되지 못하고 콘솔도 소멸**.

3층으로 막는다.

| 층 | 수단 |
|---|---|
| 소스 | 패키지 설정의 경로 키는 **빈 값**. 노드 경로는 설치(부트스트랩)나 배포설정이 정한다 |
| 검증 | `S1-CONFIG-PORTABILITY` — 배포 설정 JSON 에 `/home/<user>/work/…` 류 절대경로가 있으면 FAIL (주석성 키 제외) |
| 런타임 | `file_store.runtime_root` 가 명시 경로를 **쓸 수 있는지 확인**하고, 불가하면 노드 로컬로 폴백 + 경고. 단 `CimsRuntimeMount` 가 설정된 공유 구성에서는 폴백하지 않는다(store 분기 방지 — 그 판정은 mount guard 의 몫) |

## 13. 미구현 · 잔여 위험

- **NAS 가 SPOF 다** — NAS 가 죽으면 관리평면(+ 같은 그룹의 csc)이 멈춘다. 통화 서비스
  (CSP/CMP)는 별 그룹이고 NAS 와 무관하며, 서비스 로그가 이미 NAS 의존이므로 새로운 종류의
  SPOF 는 아니다(§4.1 수용 사항). NAS 이중화는 스토리지 트랙이고 이 문서 범위 밖이다.
- **파일시스템 펜싱이 없다** — 양 노드가 동시에 마운트하므로 단일 writer 를 만드는 것은
  **리스 하나뿐**이다. 그래서 잠금 자기검증(§4.4)이 필수이고, 그것이 실패하면 OAM 은
  read-only 에 머문다(조용히 write 하지 않는다). 블록 복제로 이 층을 되살리는 선택지는
  여유 블록 장치가 확보되면 재검토 대상이다.
- **양 노드 승격 불가 = VIP 공백** — NAS 접근 불가가 양쪽에 동시에 생기면 관리평면이
  내려간다. 통화 서비스(CSP/CMP)는 별 그룹이라 무영향이지만 같은 그룹의 csc 는 함께 내려간다
  (§3.2 의 수용 사항). 비상 밸브는 `CIMS_HA_DISABLE`(판정 얼림).
- **NFS 잠금 재수립** — NFS 서버 재시작 시 잠금이 유실될 수 있다. 리스는 주기 재확인 +
  epoch 로 이 창을 좁히지만, 그 창 동안 두 노드가 모두 자기 소유라고 볼 가능성이 남는다.
  그 이상은 fencing(STONITH)·witness 도입 영역(ha_service_model.md §14).
- **metric write 부하** — heartbeat 2s × 노드수의 jsonl append 가 NAS 로 간다. NFS 지연이
  heartbeat 처리 경로를 물면 관리평면 응답이 느려진다. retention(기본 3일)과 별개로 실측 후
  필요 시 metrics 만 노드 로컬로 분리 검토(그 경우 절체 시 그래프 이력이 리셋된다).
- **래치 자동 해제** — `shared_writer` 이므로 수동(`latch_clear_mode: manual`) 유지.
  안전등급별 자동 해제는 ha_service_model.md §19 후속.
- **oam-svc 검증 이력** — `verify_runs` 를 공유 store 레이아웃에 포함시켰으나, oam-svc 는
  `file_store` 를 쓰지 않고 cwd 상대 경로로 접근하므로 경로 주입 확인이 필요하다.
- **토큰 서명 대칭키** — §5.1. 상위 결정(신규 컴파일 의존성 채택 여부) 전까지 HS256 유지.
  그동안 csc·oam-svc 는 서명 능력을 보유한 상태로 남는다(마스킹·권한 점검으로 노출만 축소).
- **관리평면 self-upgrade 전자동화 미도입** — 순서 가드(§7.1) + 운영자 확인까지만. 자동화의
  실패 모드가 "콘솔 없음" 이고 그 상태에서 자동 복구를 지시할 주체가 없어 의도적으로 남긴다.
- **`S6-SCN-FAILOVER-OAM` LIVE 본체** — 전제 점검까지 구현. kill→절체→A~E 확인은 공유 store 가
  붙은 2-node 실환경 검증 라운드에서 채운다(§12).
