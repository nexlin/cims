# 자동 배포

> 인벤토리(접속 자격증명) + 블루프린트(배포 정의) 두 YAML 로 **agent 설치부터 모듈 설치까지**
> (선택적으로 설정·기동·검증까지) 일괄 수행한다. **base OAM 내장** — 별도 패키지·프로세스·
> 포트가 없고, 콘솔 **관리 > 릴리스 > 자동 배포** 페이지가 UI 를 제공한다.
>
> 콘솔의 수동 4탭(시스템/서버 구성 → 패키지 설치 → 패키지 설정 → 패키지 제어)과 **병렬 경로**다.
> 수동 경로를 폐기하지 않으며, 자동 배포가 만든 결과를 그 탭들에서 점검·수정한다.
>
> 내부 이름(코드·API 경로)은 `provision` 이다: `/api/v1/provision/*`,
> `ems/core/oam/src/services/provision/`. 화면 이름만 '자동 배포'.

## 1. 범위 경계

```
[전제 — 범위 밖]              [자동 배포 담당]
0번 노드 OAM 부트스트랩    →    ① agent 설치·enroll (SSH)
(deployment/bootstrap/          ② 시스템(HA 그룹)·VIP 구성
 install.sh 로 수동)            ③ 패키지 설치
                                ④ 설정 주입 (선택 — overlay + collection)
                                ⑤ 순서 기동 (선택 — start: false 로 생략 가능)
                                ⑥ 헬스 검증
```

OAM(+console+로컬 agent)이 이미 기동해 있는 상태를 입력으로 받는다. 0번 노드 자체의
부트스트랩은 종전대로 [02_deployment.md §2.1](../02_deployment.md) 의 `install.sh` 가 담당한다 —
엔진이 OAM 안에 있으므로 자기 자신을 띄울 수는 없다.

**기본 사용은 ①~③ 이다.** 설정과 기동은 콘솔의 [패키지 설정]·[패키지 제어] 탭이 이미 담당하는
영역이므로, 블루프린트에 설정을 적는 것은 선택이다 (반복 구축·형상 관리가 필요할 때만).

`deployment/` 의 env.yaml / scenario.yaml / render.py 계열과는 **독립**이다. 그쪽은 config 번들
생성기(파일 산출)이고, provisioner 는 OAM 엔티티(agent·ha_group·deployment·job)를 직접
실체화하는 오케스트레이터다. 스키마도 공유하지 않는다.

## 2. 배치 — base OAM 내장

**base OAM 내장**이다. 별도 패키지·프로세스·포트·인증서·게이트웨이 라우트가 없고, OAM 이
떠 있으면 그 순간부터 동작한다.

```
브라우저 ──HTTPS 4419──> base OAM  (/api/v1/provision/* 내장 핸들러)
                            │
                            ├─ SSH ──> 대상 호스트 (agent 설치)  ← AGENT phase 만
                            └─ REST ─> 자기 자신 (agents/ha-groups/deployments/job)
```

별도 모듈로 두지 않는 이유는 **닭-달걀**이다. 자동 배포는 "모듈을 아직 하나도 안 깐 상태"에서
쓰는 기능인데, 엔진 자체가 배포해야 하는 모듈이면 그것부터 수동으로 설치·기동해야 한다
(패키지 업로드 → deployment 생성 → install → start). 그 수동 절차를 없애는 것이 이 기능의
목적이므로, 엔진은 부트스트랩만으로 존재해야 한다.

**불변식**
- OAM 의 **공개 REST 만** 호출한다. file_store 를 직접 읽고 쓰지 않는다 — OAM 이 소유한
  부수효과(install_history 기록, 게이트웨이 라우트 재등록, JwtSecret 주입)를 우회하지 않기 위함.
  같은 프로세스 안이지만 loopback HTTP 로 자기 자신을 호출한다.
- 인증은 요청의 `Authorization` 을 admin 으로 검증하고 **그 토큰을 그대로** OAM 호출에 재사용한다.
  provisioner 는 자격증명을 발급하지 않는다.
- run 은 백그라운드 스레드에서 돈다. `POST .../runs` 는 run_id 만 즉시 반환하고 진행률은
  폴링으로 읽는다 — 수 분짜리 작업을 HTTP 요청으로 붙잡지 않는다.

**주소 두 개를 구분한다.** 자기호출은 loopback(`127.0.0.1:<Server.Port>`), agent 가 enroll 할
주소는 `handlers.agents._oam_public_url`(= 콘솔 install-command 와 동일: `Server.AgentOamUrl` →
Host 헤더 → `Server.Ip:Port`). 원격 노드에 loopback 을 알려주는 사고를 막기 위함이며, enroll
주소는 run 생성 시점 값을 run 레코드에 고정해 resume 때도 같은 OAM 을 가리킨다.

> **트레이드오프**: OAM 업그레이드가 프로세스를 재기동하므로 진행 중인 run 이 끊긴다.
> 체크포인트가 있어 `resume` 으로 이어지지만, 별도 프로세스였을 때보다 영향을 받는다.
> 배포 작업과 OAM 업그레이드를 동시에 하지 않는 것으로 운영한다.

## 3. 입력 — 인벤토리와 블루프린트

**두 파일로 분리한다.** 인벤토리는 사이트 고유 + 비밀정보, 블루프린트는 구조 정의 + 재사용 대상.
같은 블루프린트를 여러 사이트 인벤토리에 적용할 수 있다.

| 파일 | 답하는 질문 | 성격 |
|---|---|---|
| `inventory.yaml` | 서버가 **어디** 있고 **어떻게 로그인**하나 | 사이트 고유 · 비밀 포함 · 반출 금지 |
| `blueprint.yaml` | **무엇을** 어떤 구조로 깔 것인가 | 사이트 무관 · 비밀 없음 · 형상 관리 대상 |

분리 근거는 셋이다. **보안 등급** — blueprint 는 비밀이 없어 git 에 넣고 리뷰·복사할 수 있지만
inventory 는 마스킹·반출 금지 대상이다. 한 파일이면 blueprint 까지 비밀 취급을 받아 형상 관리가
막힌다. **재사용 단위** — 하나의 blueprint 를 사이트별 inventory 에 각각 적용한다.
**변경 빈도** — inventory 는 서버·자격증명이 바뀔 때, blueprint 는 모듈 구성이 바뀔 때 서로
무관하게 변한다.

### 3.0 입력 경로 — 업로드가 기준, 편집은 웹에서

파일 작성이 정식 입력이되, **업로드 후의 열람·수정은 콘솔에서 한다.** 폐쇄망 운영 현장에서
값 하나 고치려고 대상 노드에 들어가 vim 을 여는 비용을 없애는 것이 목적이다.

```
[YAML 업로드] → 파싱·검증 → ┌─ [구성 보기/편집]  표·폼 (기본 편집 수단)
                             └─ [원문 보기/편집]  텍스트 (대량 수정·붙여넣기)
                                        │
                                 [계획 확인] → [배포 실행]
```

- 업로드본은 **원문 그대로 보관**하고, 파싱된 구조를 편집 대상으로 삼는다.
- 구성 뷰에서 저장하면 구조에서 YAML 을 재생성한다 — 이때 **원본 주석은 소실된다.** 저장 전
  경고하고, 보관된 원문은 [원문 보기]·다운로드로 계속 접근 가능하다. 원문 뷰에서 직접 고치는
  경로에서는 주석이 유지된다.
- 주석의 통상 용도(값의 의미 설명)는 구성 뷰가 필드별 설명·검증으로 대체한다.
- 콘솔에서 고친 결과는 YAML 로 다시 내려받아 형상 관리에 반영한다.

### 3.1 inventory.yaml — 접속 자격증명

```yaml
version: 1
defaults:
  ssh:  { user: cims, port: 22 }
  sudo: { method: password }        # password | nopasswd
  install_dir: /opt/cims-agent

servers:
  # OAM 부트스트랩 노드 — agent 가 이미 enroll 되어 있어 SSH 하지 않는다.
  # name 은 OAM 에 등록된 agent 이름과 정확히 일치해야 한다.
  - name: oam01
    host: 10.0.0.10
    agent_preinstalled: true

  - name: csc01                     # 블루프린트가 참조하는 논리명
    host: 10.0.1.11
    ssh:  { password: &pw_csc01 "…" }
    sudo: { password: *pw_csc01 }   # sudo.method=nopasswd 면 생략
  - name: csc02
    host: 10.0.1.12
    ssh:  { password: &pw_csc02 "…" }
    sudo: { password: *pw_csc02 }
```

**인증은 비밀번호만 지원한다** — 운영 환경이 SSH 키를 쓰지 않으므로 키/ssh-agent 경로는 두지
않는다. ssh 비밀번호와 sudo 비밀번호는 보통 같은 계정 비밀번호라 앵커(`&pw`)로 한 번만 적고
참조(`*pw`)한다. 둘이 다른 경우는 sudoers 에 `Defaults targetpw`/`rootpw` 가 설정된 환경이다.

`agent_preinstalled: true` 는 agent 가 이미 설치·enroll 된 노드 표시다. **OAM 부트스트랩 노드가
항상 이 경우**이며(install.sh 가 로컬 agent 를 이미 붙여놓는다), 이 노드에는 SSH 하지 않으므로
접속 정보를 적지 않는다. 그 외 서버는 비밀번호가 없으면 오류로 표시하고 run 을 시작하지
않는다(부분 실행 방지).

### 3.2 blueprint.yaml — 배포 정의

OAM 엔티티에 1:1 대응한다 — `systems` = HA 그룹, `members` = agent, `modules` = deployment.

```yaml
version: 1
name: prod-volte-ptt
description: VoLTE + PTT 4-노드 상용

systems:
  - name: Control-Server
    mode: active_standby            # active_standby | all_active | standalone
    vips:
      # slot = VIP 의 용도 라벨(생략 시 'service'). OAM vip_bindings 가 이 키로 VIP↔NIC 를
      # 잇고, slot 이 비면 keepalived 렌더에서 조용히 버려진다.
      - { ip: 10.0.1.13, prefix: 24, interface: eth1, slot: service }
    failover:                       # 생략 시 OAM 기본값 (_FAILOVER_DEFAULTS)
      health:  { interval: 2, fall: 2, rise: 2, grace_sec: 30 }
      preempt: nopreempt
    members:
      - { server: ctrl01, role: master }
      - { server: ctrl02, role: backup }
    modules:
      - package: csp
        version: latest             # latest | 0.2.31
        process_name: CSP
        start: true                 # 생략 시 true. false 면 START/VERIFY 대상에서 빠진다
                                    #   (데몬이 아니거나 수동 기동할 모듈)
        config:                     # 그룹 공통 overlay (전 멤버 동일)
          Setup.Sip.UdpThreadCount: 4
        per_server:                 # 서버 개별 overlay
          ctrl01: { Setup.LocalIp: 10.0.1.11 }
          ctrl02: { Setup.LocalIp: 10.0.1.12 }
        collections:                # config/*.jsonl
          local_nodes:
            - { id: 1, name: sip-udp, bind_ip: 10.0.1.13, bind_port: 5060,
                protocol: udp, enabled: true, is_primary: true }

  - name: Media-Server
    mode: all_active
    members:
      - { server: media01 }
      - { server: media02 }
    modules:
      - { package: cmp, version: latest, process_name: CMP }

start_order:                        # 기동 의존 (생략 시 modules 선언 순)
  - Media-Server
  - Control-Server
```

`version: latest` 는 run 시작 시점에 OAM 패키지 저장소의 최신 버전으로 **고정(pin)** 되어 run
레코드에 기록된다 — 같은 run 을 resume 해도 버전이 흔들리지 않는다.

### 3.3 두 파일의 연결

```
blueprint.yaml                       inventory.yaml
  systems[].members[]                  servers[]
    - server: csc01  ──────────────►    - name: csc01
                                           host: 10.0.1.11
                                           ssh:  { user, password }
                                           sudo: { password | method: nopasswd }
```

**논리명이 유일한 연결고리**다. blueprint 는 "csc01 에 CSC 를 깐다"만 알고, 그것이 어느 IP 이며
어떤 자격증명으로 접속하는지는 모른다. 검증기는 blueprint 가 참조한 모든 `server` 가 inventory 에
존재하는지 확인하고, 없으면 run 을 시작하지 않는다.

### 3.4 YAML 처리

OAM 런타임 vendor 에는 YAML 파서가 없었다(`import yaml` 사용처는 `deployment/bin/*.py` 뿐이며
개발 장비의 시스템 파이썬에서 동작). **PyYAML 을 `ems/core/oam/vendor/yaml` 에 동봉**한다 —
순수 파이썬 경로로 동작하므로 C 확장 빌드가 필요 없고, 기존 vendor 관례와 같다.

파싱은 `yaml.safe_load` 만 사용한다(임의 객체 역직렬화 차단). 문서 상단 `version` 키로 스키마
버전을 판별하며, 모르는 최상위 키는 오류로 처리한다 — 오타로 인한 무시(silent drop)를 막는다.

## 4. 실행 모델 — run / phase / step

run 은 phase 6단계, 각 phase 는 대상(서버 또는 모듈)별 step 으로 나뉜다. **모든 step 은 멱등**이고
완료 즉시 run 레코드에 체크포인트된다.

| phase | 대상 | 동작 | 멱등 판정 |
|---|---|---|---|
| `AGENT` | 서버 | SSH → `install-agent.sh` → enroll 완료 폴링 | agent 가 이미 `online` 이면 skip |
| `TOPOLOGY` | 시스템 | ha-group 생성 + 멤버 편입 + VIP + 절체조건 | 동명 그룹 존재 시 재사용·차이만 PUT |
| `INSTALL` | 모듈×서버 | deployment 생성 → `install` job → 완료 폴링 | 같은 버전이 이미 설치됐으면 skip |
| `CONFIG` | 모듈×서버 | overlay `update_config` + collection PUT | 값이 같으면 skip |
| `START` | 모듈×서버 | `start_order` 순서로 기동 → job 완료 폴링 | 이미 `running` 이면 skip |
| `VERIFY` | 시스템 | `health_check` job · A/S 는 실측 ACTIVE 확인 | — (항상 수행) |

> **TOPOLOGY 는 그룹만 만든다 — VIP·절체조건은 만들지 않는다.** 콘솔 `＋ 시스템 추가` 와
> 같은 범위다. 이 단계가 없으면 A/S 로 선언한 서버들이 트리에서 SA 로 떨어지므로 생성은
> 배포의 일이고, VIP·keepalived 설정은 그 뒤 콘솔 [시스템/서버 구성] 에서 한다.
>
> **`auth_pass` 를 블루프린트에 적지 않는다.** OAM 은 active_standby 그룹 생성에 VRRP
> 인증값(8자)을 요구하지만, HA 값을 배포 정의에 끌어들이지 않기 위해 미지정이면 TOPOLOGY 가
> 자동 생성한다(운영자가 그룹 탭에서 변경 가능). 기존 그룹을 재사용할 때는 그 값을 보존해
> keepalived 를 흔들지 않는다. 명시하고 싶으면 `auth_pass:` 로 넣는다. all_active 는
> keepalived 를 쓰지 않으므로 빈 값이다.
>
> **standalone 은 그룹을 만들지 않는다.** OAM `POST /api/v1/ha-groups` 는 mode 를
> `active_standby | all_active` 로만 받는다. `ha_group: false` 로 A/S·AA 에서도 생성을
> 생략할 수 있다 — 다만 모듈 설치 자체는 그룹 없이도 된다(`_check_ha_capability` 가 그룹
> 미소속 agent 를 통과시킨다).
>
> **A/S 에 VIP 가 없어도 지적하지 않는다.** 설치만 하는 블루프린트에서는 없는 것이 정상이며,
> 그룹만 만들어지고 keepalived 는 무장되지 않은 상태로 남는다. `agent_preinstalled` 노드
> (= OAM 부트스트랩 노드)가 블루프린트에 안 쓰이는 것도 통상이므로 미사용 지적에서 제외한다.
>
> **A/S 의 실측 ACTIVE 는 경고 수준이다.** VIP 보유 판정(`active_agent_id`)은 heartbeat 의
> `interfaces[]` 관측이라 기동 직후에는 아직 미확정(None)일 수 있다. VERIFY 는 이를 실패로
> 보지 않고 안내만 남긴다.

**병렬성**: 같은 phase 안의 서로 다른 서버는 병렬, phase 간에는 배리어. `START` 만 예외로
`start_order` 를 따르는 직렬이며, active_standby 그룹은 master 를 먼저 올린다(VIP 선점).

**재개**: `POST .../runs/{id}/resume` 는 마지막 성공 체크포인트 다음 step 부터 재실행한다.
20대 중 17대째 실패 시 처음부터 다시 돌지 않는다.

**중단 정책**: 기본은 fail-fast(첫 실패에서 phase 배리어를 넘지 않음). `on_error: continue` 를
주면 같은 phase 의 나머지 대상은 마저 진행하고 실패 목록을 모아 보고한다.

**롤백**: `POST .../runs/{id}/rollback` 은 그 run 이 **생성한 것만** 역순으로 되돌린다
(기동한 프로세스 stop → 생성한 deployment 제거 → 편입한 멤버 해제 → 생성한 그룹 삭제).
run 시작 전부터 존재하던 엔티티는 건드리지 않는다. agent 설치는 롤백 대상이 아니다
(제거는 대상 호스트의 `uninstall.sh` 수동).

### 4.1 dry-run

`POST .../runs?dry_run=true` 는 아무것도 바꾸지 않고 **계획 diff** 만 반환한다: 생성될 그룹/VIP,
설치될 모듈×서버×버전, 변경될 설정 키, skip 될 항목(사유 포함). 콘솔은 이 결과를 실행 전
확인 화면에 띄운다.

## 5. SSH 러너

`sshpass` 의존 없이 stock OpenSSH 로 동작한다. **비밀번호 인증 전용**이다.

| 항목 | 방식 |
|---|---|
| SSH 비밀번호 | `SSH_ASKPASS=<헬퍼> SSH_ASKPASS_REQUIRE=force` (OpenSSH 8.4+) — TTY·sshpass 불요 |
| 키 시도 차단 | `-o PubkeyAuthentication=no` — 로컬에 우연히 있는 키가 먼저 시도되어 `Too many authentication failures` 로 끊기는 것을 막는다 |
| 호스트키 | `-o StrictHostKeyChecking=accept-new` (TOFU). 기지 호스트의 키 변경은 오류 |
| sudo 비밀번호 | `sudo -S -p ''` 의 **stdin** 으로 주입 (원격 argv 에도 남지 않는다) |
| 스크립트 전달 | `install-agent.sh` 를 `scp` 로 대상 `/tmp` 에 놓고 원격 실행 후 삭제 |

원격 명령의 stdin 은 sudo 비밀번호 전용이므로 스크립트를 `bash -s` 로 흘려넣지 않는다 —
stdin 이 두 용도로 경합하면 sudo 가 스크립트 본문을 비밀번호로 읽는다.

askpass 헬퍼는 `0700` 임시 파일로 생성하고 사용 직후 삭제한다. 비밀번호는 **명령행 인자로
절대 넘기지 않는다** (`/proc/*/cmdline` 노출 방지).

agent 설치 호출은 이미 완전 비대화형이라 스크립트 수정이 필요 없다
(`agent/install-agent.sh` — `--oam-url --enrollment-token --name --install-dir --svc-user`):

```bash
scp install-agent.sh <host>:/tmp/
ssh <host> "printf '%s\n' \"\$SUDO_PW\" | sudo -S -p '' bash /tmp/install-agent.sh \
    --oam-url https://<oam>:4419 --enrollment-token <tok> --name <논리명> \
    --install-dir /opt/cims-agent --svc-user <계정>; rc=\$?; rm -f /tmp/install-agent.sh; exit \$rc"
```

enrollment 토큰은 phase 시작 시 `POST /api/v1/agents` 로 서버마다 1회용으로 발급받는다.

## 6. REST API

모두 admin 인증. base OAM 4419 를 통해 프록시된다.

| method | path | 설명 |
|---|---|---|
| GET / POST | `/api/v1/provision/blueprints` | 목록 / 업로드 (YAML 원문 또는 구조 JSON) |
| GET / PUT / DELETE | `/api/v1/provision/blueprints/{id}` | 1건(구조+원문) / 수정 / 삭제 |
| GET | `/api/v1/provision/blueprints/{id}/raw` | 보관된 YAML 원문 (다운로드) |
| POST | `/api/v1/provision/blueprints/validate` | 스키마 + 인벤토리 참조 검증 |
| GET / POST | `/api/v1/provision/inventories` | 목록(마스킹) / 업로드 |
| GET / PUT / DELETE | `/api/v1/provision/inventories/{id}` | 1건(마스킹) / 수정 / 삭제 |
| POST | `/api/v1/provision/inventories/{id}/preflight` | SSH·sudo 접속만 확인 (변경 없음) |
| POST | `/api/v1/provision/runs` | run 시작 → `{run_id}`. `?dry_run=true` 는 계획 diff |
| GET | `/api/v1/provision/runs` | run 목록 |
| GET | `/api/v1/provision/runs/{id}` | 진행 상태 (phase·step·로그 tail) |
| POST | `/api/v1/provision/runs/{id}/resume` | 실패 지점부터 재개 |
| POST | `/api/v1/provision/runs/{id}/abort` | 진행 중단 (현재 step 완료 후 정지) |
| POST | `/api/v1/provision/runs/{id}/rollback` | 이 run 이 만든 것 역순 제거 |

run 상태 응답:

```json
{
  "id": 7, "status": "running", "blueprint": "prod-volte-ptt", "started_at": "…",
  "phases": [
    {"key": "AGENT", "status": "done",
     "steps": [{"target": "ctrl01", "status": "done", "detail": "enrolled (agent#3)"},
               {"target": "ctrl02", "status": "skipped", "detail": "이미 online"}]},
    {"key": "TOPOLOGY", "status": "running", "steps": [...]}
  ],
  "checkpoint": {"phase": "TOPOLOGY", "step_index": 1}
}
```

## 7. 콘솔 UI

**위치**: `관리 > 릴리스 > 자동 배포` (`/release/auto-deploy`, `adminOnly` + `devOnly`).
사이드바 `릴리스` 섹션의 마지막 leaf — 검증 → 패키징 다음 단계가 배포다. 컴포넌트는
`pages/AutoDeployPage.tsx`. 릴리스 섹션의 모든 leaf 와 동일하게 **개발자 모드 ON 에서만**
노출된다(헤더 `</>` 토글, admin 전용). 옛 경로 `/deploy/auto-deploy` 는 `App.tsx` 에서
리다이렉트한다.

**탭이 아니라 독립 페이지인 이유** 셋:
- `시스템/인프라` 의 4탭은 좌측 서버 트리를 공유하고 우측 인스펙터만 바꾸는 패턴인데, 자동
  배포는 트리를 쓰지 않는다(배포 대상이 아직 없는 상태에서 시작한다). 탭으로 두면 탭 전환에
  화면 구조가 바뀐다.
- 실행이 수 분 걸린다. run 이력·재개·롤백이 URL 과 영속 화면을 요구하므로 모달로도 부적합하다.
- 4탭은 구성→설치→설정→제어 라이프사이클 순서다. 그 앞에 끼면 '1단계' 로 읽히지만 실제로는
  4탭 전체를 대체하는 병렬 경로다.

> 새 사이드바 **섹션**을 만들지 않고 `release` 섹션의 leaf 로 두는 이유: base 콘솔(부트스트랩
> 동봉본)은 `routes.tsx` 의 `BASE_PROFILE_SECTION_KEYS`(`system`·`release`)만 렌더한다.
> 자동 배포는 풀 콘솔이 도착하기 전에 쓰는 기능이므로 이 두 섹션 중 하나여야 하고, 소프트웨어
> 릴리스 라이프사이클(검증→패키징→배포)상 `release` 에 속한다. `system` 섹션은 배포가 끝난
> 노드의 운용(구성·설치·설정·제어)을 담는다.
>
> 설정·기동·업그레이드까지 YAML 로 확장되면 독립 섹션으로 승격하고 하위에 블루프린트/
> 인벤토리/실행 이력을 둔다 — leaf → 섹션 이동은 라우트 한 줄이라 비용이 낮다.

화면 흐름:

1. **업로드/선택** — `inventory.yaml` · `blueprint.yaml` 을 올리거나 저장된 항목 선택
2. **검토·편집** — 아래 두 뷰를 토글하며 값 확인·수정 (§7.1)
3. **[계획 확인]** — dry-run 결과를 생성/변경/skip 으로 분류해 표시
4. **[배포 실행]** — phase×step 트리로 진행률 표시 (1초 폴링). step 별 상태·소요·로그 tail
5. **실패 시** — 실패 step 을 펼쳐 stderr 확인 → [재개] / [롤백] / [중단]

`시스템/인프라` 의 4탭은 손대지 않는다 — 자동 배포가 만든 결과를 그 탭들에서 점검·수정한다.

### 7.1 두 편집 뷰

| 뷰 | 용도 | 편집 단위 |
|---|---|---|
| **구성 보기** (기본) | 값 확인·소규모 수정 | 시스템/서버/모듈 트리 + 필드 표. 필드별 설명·검증·행 추가/삭제 |
| **원문 보기** | 대량 수정·붙여넣기·주석 유지 | YAML 텍스트 전체 |

구성 뷰가 기본이다 — 업로드 직후 "어떤 값이 어떻게 잡혔는지"가 표로 펼쳐지고, IP 형식·참조
무결성·패키지 존재 여부가 필드 옆에 인라인으로 표시된다. 인벤토리의 비밀 필드는 `••••` 로
표시하고, 비워두면 저장된 값을 유지한다(마스킹 값을 그대로 되돌려 덮어쓰는 사고 방지).

> **에디터 라이브러리를 추가하지 않는다.** 콘솔 의존성은 react·react-dom·react-router-dom·
> lucide-react 넷뿐이고 폐쇄망 번들 최소화가 전제다. 원문 뷰는 monospace `textarea` + 줄번호 +
> 오류 줄 하이라이트로 구현한다 (Monaco/CodeMirror 미도입).

검증은 항상 서버에서 수행한다(`POST .../validate`) — 브라우저 검증은 즉시성 보조일 뿐,
실행 가능 여부의 판정자는 provisioner 다.

## 8. 보안

- **인벤토리 저장**: provisioner 런타임의 `_secrets/` 아래 `0600`. 응답에서 비밀번호 필드는
  항상 마스킹 — 저장된 값을 평문으로 돌려주는 API 는 없다. blueprint 는 `/raw` 로 원문을
  내려받을 수 있지만 **inventory 에는 `/raw` 가 없다**(원문에 비밀이 있으므로).
- **마스킹 왕복 방지**: 인벤토리 수정 시 비밀 필드를 비워 보내면 저장값을 유지한다. 마스킹
  문자열이 그대로 되돌아와 실제 비밀번호를 덮어쓰는 사고를 구조적으로 차단한다.
- **로그 마스킹**: SSH/sudo 의 stdout·stderr 는 알려진 비밀값을 치환한 뒤에만 run 로그와
  콘솔로 나간다. 예외 트레이스·명령 에코도 같은 필터를 통과한다.
- **명령행 비노출**: 비밀번호는 argv 에 싣지 않는다 (askpass 파일 / stdin 만 사용).
- **수명**: 복호화된 자격증명은 run 실행 중 메모리에만 유지하고 종료 시 폐기한다. 임시 askpass
  파일은 step 종료 시 삭제한다.
- **권장 구성**: SSH 키 + `NOPASSWD` sudo. 이 경우 인벤토리에 비밀번호를 적지 않아도 된다.
- **감사**: run 은 시작 계정·블루프린트·대상 서버 목록과 함께 알람/이벤트 이력에 남는다.

## 9. 실패 모드

| 증상 | 원인·처리 |
|---|---|
| `ssh_auth_failed` | 키/비밀번호 불일치. 인벤토리 수정 후 [재개] |
| `sudo_failed` | sudo 비밀번호 불일치 또는 대상 계정이 sudoers 미등록 |
| `host_key_changed` | 기지 호스트의 키 변경 — 중간자 가능성. 자동 수용하지 않고 오류 |
| `enroll_timeout` | agent 설치는 됐으나 heartbeat 미도달. 대상에서 `systemctl --user status cims-agent` |
| `package_missing` | 블루프린트가 참조한 패키지가 저장소에 없음. `관리 > 시스템 > 패키지` 에 업로드 후 [재개] |
| `install_job_failed` | agent 의 job report stderr 를 step 상세에 그대로 표시 |
| `verify_listen_failed` | 기동은 됐으나 포트 미개방. 설정(bind IP/포트)·방화벽 확인 |

## 10. 구성 요소

```
ems/core/oam/
├── src/handlers/provision.py            /api/v1/provision/* REST (oam_app.py 가 마운트)
├── src/services/provision/
│   ├── schema.py                        두 YAML 파싱·검증·교차검증·정규화
│   ├── ssh.py                           SSH 러너 (askpass 비밀번호 인증, 마스킹)
│   ├── oam_client.py                    OAM REST 클라이언트 (job 완료 폴링 포함)
│   ├── store.py                         블루프린트/인벤토리/run 원자적 영속
│   ├── engine.py                        phase×step 오케스트레이션 + 체크포인트/재개
│   └── phases/                          agent · topology · install · config · start · verify
├── vendor/yaml/                         PyYAML(순수 파이썬) 동봉
├── assets/install-agent.sh              AGENT phase 가 원격에 밀어넣는 원본
│                                        (패키징 시 agent/install-agent.sh 에서 갱신)
└── examples/provision/                  inventory.yaml · blueprint.yaml 견본

scripts/prov                             CLI — 같은 엔진을 명령행에서 구동
provision-site/                          실사이트 인벤토리/블루프린트 (.gitignore)
```

영속 위치는 runtime store 아래 `provision/` — `file_store.runtime_root(config)` 기준이라
`modules/oam/runtime/provision/` 이 되고, 버전 디렉토리 밖이므로 업그레이드·롤백에 생존한다.

**CLI** — 콘솔과 같은 엔진을 명령행에서 돌린다. OAM 이 안 떠 있어도 검증·계획이 가능하다.

```bash
scripts/prov validate  -b blueprint.yaml -i inventory.yaml
scripts/prov preflight -i inventory.yaml            # SSH/sudo 접속만 확인
scripts/prov plan      -b … -i …                    # dry-run
scripts/prov apply     -b … -i … --oam-url … --token …
scripts/prov resume    --run 3 --oam-url … --token …
```

인벤토리 파일이 `0600` 이 아니면 CLI 가 로드를 거부한다. run 기록 기본 위치는
`~/.cims-provision` (`--runtime` 으로 변경).

## 11. 관련 문서

- [../02_deployment.md](../02_deployment.md) — 배포 아키텍처 (버전 단위 설치·job 파이프라인·부트스트랩)
- [oam_base_service_split.md](oam_base_service_split.md) — 게이트웨이 뒤 서비스 모듈 패턴
- [../modules/agent.md](../modules/agent.md) — Agent job 타입·프로토콜
- [../../user-manual/deployment_workflow.md](../../user-manual/deployment_workflow.md) — 수동 배포 절차 (대조군)
- [../../api/admin_api.md](../../api/admin_api.md) — provisioner 가 호출하는 OAM REST
