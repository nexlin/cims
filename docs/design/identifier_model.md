# 식별자 모델 — 불변 id 와 표시 이름

시스템이 참조하는 것은 **불변 식별자(id)** 하나뿐이고, 사람이 부르는 **이름(name)** 은
언제든 바뀔 수 있는 라벨이다. 이 문서는 그 경계와 표시 규약, 그리고 키가 바뀔 때의
재키잉 절차의 정본이다.

## 1. 규칙

> **동작은 id 로, 표시는 name 으로.**
>
> 1. 엔티티는 생성 시 **불변 id** 를 받는다. 이후 어떤 이유로도 바뀌지 않는다.
> 2. **코드가 참조하는 모든 키는 id 다** — 레코드 참조, 파일·디렉터리 이름, 설정 식별자,
>    job 파라미터, 이력·알람의 상관 키.
> 3. `name` 은 **어디에도 키로 쓰지 않는다.** 바뀌어도 아무것도 깨지지 않아야 한다.
> 4. 사람이 읽는 자리에서만 id 를 이름으로 **해석해 함께 보여준다** (§4).

규칙의 대상은 **운영자가 편집할 수 있는 이름**이다. 패키지/모듈 타입명(`csp`·`cmp`·`oam`)은
사용자가 바꾸는 라벨이 아니라 타입 식별자이므로 이 규칙의 대상이 아니다.

## 2. 왜 — 이름을 키로 쓰면 무엇이 깨지는가

이름을 키로 쓰면 **rename 이 곧 재키잉**이 되고, 그 순간 두 부류의 사고가 난다.

**(a) 동작이 깨진다.** 옛 이름으로 만들어진 상태 파일·설정 식별자가 남고, 새 이름으로는
아무것도 없는 상태에서 판정이 시작된다. HA 경로에서는 이것이 **안전장치 소실**로 직결된다 —
절체 래치(`state/ha/latch/<키>.json`)가 조용히 사라져, 운영자 확인이 필요하던 노드가
검증 없이 승격 후보로 되돌아온다.

**(b) 이력이 끊긴다.** 상관 키가 이름이면 rename 전후의 기록을 이어붙일 근거가 없다.
같은 노드에서 난 사건인데 `test1/csp` 와 `test2/csp` 로 갈라져 남는다.

반대로 id 를 키로 쓰면 rename 은 **표시 문자열만 바뀌는 일**이 된다. 상태 파일도, 설정
식별자도, 이력의 상관 키도 그대로다.

## 3. 적용 — 무엇이 id 이고 무엇이 라벨인가

| 엔티티 | 시스템이 참조하는 키 | 표시 라벨 |
|---|---|---|
| agent(서버) | `agent.id` — deployment·job·metric 이 전부 `agent_id` 참조 | `agent.name` |
| ha_group(서버 그룹) | `group.id` → HA 서비스 키 `g<id>` (§5.1) | `group.name` |
| 알람·이벤트 `mo_instance` | `a<서버id>` / `g<그룹id>` (§5.2) | 조회 시 해석 (`mo_label`) |

세 경우 모두 **이름은 어떤 키에도 등장하지 않는다.** 그래서 개명은 라벨 갱신으로 끝나고,
감지·보상 동작이 필요 없다.

## 4. 표시 규약

식별자가 사람 눈에 닿는 자리는 두 종류이고, 요구가 다르다.

| 자리 | 규약 |
|---|---|
| **상관 키** — 파일·디렉터리명, 설정 식별자, `mo_instance`, `origin`, job 파라미터 | **id 만.** 이름을 섞지 않는다 — 섞는 순간 rename 이 다시 키를 바꾼다 |
| **표시** — 로그 본문, 알람 메시지, 콘솔, health-check 출력 | **id 와 이름을 함께.** `1 | test2` 또는 `test2 (#1)` |

로그 본문에 이름을 **그 시점 스냅샷으로** 함께 남기는 것이 핵심이다. id 로 이어붙이고,
이름으로 읽는다:

```
10:xx  INFO | 1 | test1 | ...
11:xx  INFO | 1 | test2 | ...      ← 이름이 바뀌어도 1 로 같은 대상임을 안다
```

이름만 찍으면 rename 전후를 잇지 못하고, id 만 찍으면 사람이 못 읽는다. 둘 다 남긴다.

## 5. 적용 상세

### 5.1 HA 서비스 키

```python
# ems/core/oam/src/handlers/ha_groups.py:906,931
services[group['name']] = entry
```

이 키 하나에서 아래가 전부 파생된다.

| 파생물 | 예 |
|---|---|
| keepalived 인스턴스·스크립트 | `vrrp_instance VI_CONTROL`, `vrrp_script check_Control` |
| track/notify 인자 | `cims-health Control`, `cims-notify Control` |
| 런타임 판정 파일 | `run/ha/{verdict,role,promotion}/Control.json` |
| **영속 안전 상태** | `state/ha/latch/Control.json`, `planned_release/Control`, `maintenance/Control` |
| 절체 로그 | `/var/log/cims-ha/notify_Control.log` → OAM `ha_flap` 알람 입력 |
| 이력 | `ha_operations.service` |

**키의 형태**: `g<id>` (`ha_service_key()`). 식별에 기여하는 값은 `id` 뿐이고, `g` 접두는
운영자가 `keepalived.conf`·상태 디렉터리에서 이 토큰의 정체를 알아보게 하는 표시다.
그래서 keepalived 는 `vrrp_instance VI_G1` / `vrrp_script check_g1`, 상태는
`run/ha/verdict/g1.json` 이 된다. 이름은 ha.json 의 `services.<키>.name` 에 **라벨로만**
함께 실린다(키가 아니다 — agent 가 로그·표시에 쓸 수 있게).

이 키는 `group.id` 에서만 유도한다(`ha_service_key`).

- **agent 는 코드 변경이 없다.** agent 는 이 키를 해석하지 않고 **불투명 토큰**으로만 쓴다 —
  dict 키, 파일명 조각(`_latch_path()` 등 `cims_agent.py:3063,3153,3272`), CLI 인자.
  키 값을 비교·파싱하는 코드가 없다.
- OAM 의 렌더·job·이력 지점(9곳)이 `group.id` 를 쓰도록 바꾼다.
- 기존 현장은 **1회 재키잉**이 필요하다 (§6).
- 그룹 이름에 형식 제약이 사라진다 — 라벨이므로 공백·한글 무엇이든 허용된다.
  (현재는 검증이 `if not name` 뿐이라 `My Group!` 같은 이름이 `vrrp_instance VI_MY GROUP!`
  으로 렌더돼 keepalived 설정을 깨뜨릴 수 있다. id 로 가면 이 구멍도 함께 닫힌다.)

### 5.2 알람·이벤트 `mo_instance`

`mo_instance` 는 표시 문자열이 아니라 **활성 알람 식별키의 절반**이다 —
활성키가 `code@mo_instance` 이고(`alarm_sweeper.transition`), 열린 알람을 닫을 때 같은
키로 찾는다. 그런데 그 루트가 서버명/그룹명이었다:

```python
host = agent.get('name') or str(agent.get('id'))
mo   = f"{host}/disk"          # → akey = "A-QOS-001@vm1/disk"
```

서버를 개명하면 다음 스윕이 `A-QOS-001@vm2/disk` 를 만든다 → **옛 알람은 영영 닫히지
않고, 새 이름으로 중복이 열린다.**

그래서 루트는 불변 id 파생이다 — 서버 `a<id>`, 그룹 `g<id>`
(`alarm_sweeper.server_mo_root`·`group_mo_root`, 주소 해석기 `build_mo_root_resolver`).

- **표준과 충돌하지 않는다.** X.733/3GPP 은 식별자(DN)와 표시 이름(userLabel)을 나눈다.
  [alarm_standardization.md](alarm_standardization.md) §3.4(b) 가 둘을 하나로 합쳐
  "루트 = 서버명/그룹명" 으로 둘을 합쳐 쓰고 있었고, 그 절은 이 모델에 맞춰 개정돼 있다.
- **표시는 잃지 않는다.** 메시지의 `{host}` 는 그 시점 이름으로 렌더되고, 조회 API 가
  `source.mo_label`(루트를 **현재** 이름으로 해석)을 실어 콘솔이 그것을 표기한다.
  `mo_instance` 는 tooltip·검색어로 남는다.
- **파티션 판정은 무영향** — `partition_of` 는 `detected_by` 가 유일 키다(mo 형식을 보는
  분기는 `detected_by` 없는 구 레코드 폴백뿐).
- **모듈 자기보고는 대상 밖** — 루트가 모듈이 선언한 자기 node 신원(`SystemId`)이라
  OAM 의 서버 이름과 무관하고, 개명의 영향을 받지 않는다.
- **키가 바뀌던 시점에 열려 있던 알람**은 스윕의 "평가되지 않은 agent 알람 → close" 경로가 **원 akey
  로 종결**하므로 로그의 open/close 짝이 맞는다. 지속 중인 조건은 새 루트로 다시 열린다.

## 6. 재키잉 절차 (`key_migration`)

키를 바꾸는 것은 rename 과 같은 위험을 갖는다 — 옛 키의 상태가 남고 새 키로는 비어 있는
창이 생긴다. 그래서 **OAM 이 대응표를 실어 보내고 agent 가 순서대로** 처리한다.

```
OAM   : update_ha params 에 key_migration {"Control": "1"} 동봉
agent : ① 상태 파일 이동 — run/ha/{verdict,role,promotion}/,
             state/ha/{latch,planned_release,maintenance}/
        ② 그 다음 ha.json 기록 → cims-ha apply (keepalived 재렌더)
```

**①이 ②보다 먼저여야 한다.** 순서가 뒤집히면 새 `check_<id>` 가 아직 없는 verdict 를 찾아
`cims-health` 가 rc1 을 내고, `interval × fall` 만에 VIP 가 이양될 수 있다. 그리고 래치가
①에서 옮겨져야 "절체당한 노드" 표시가 보존된다.

이 경로는 일회성이 아니다 — 앞으로의 모든 재키잉이 같은 통로를 쓴다.

## 7. 이름 변경 기능

이름 변경은 **라벨 갱신**이다 — 감지도, 보상 동작도 없다.

| | 필요한 것 |
|---|---|
| 서버(agent) 이름 | 콘솔 편집 UI + `_update_agent` 의 **중복 이름 검사** (생성에는 있으나 수정에는 없다) |
| 서버 그룹 이름 | 그룹 [메타] 의 이름 + [적용]. 키가 `g<id>` 라 파급이 없다(§5.1) |
| 둘 다 | **추가 조치 없음** — 이름이 어떤 키에도 안 남으므로(§5.1·§5.2) 개명에 딸린 동작이 없다 |

이름의 제약은 **비어 있지 않을 것**과 **유일할 것** 둘뿐이다(사람이 이름으로 대상을 지목하므로). 형식 제약은 없다 — 공백·한글 모두 허용된다. 키가 아니기 때문이다.

노드 로컬의 `state.json` name 과 systemd unit 의 `--name` 은 설치 시점 값이라 rename 후에도
옛 이름으로 남는다. 인증·보고는 토큰과 `agent_id` 로 이뤄지므로 기능 영향은 없고, 표시 계층이
OAM 레코드를 정본으로 쓰면 된다.
