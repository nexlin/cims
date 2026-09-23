# 사이트 디렉터리 레이아웃 — `CimsSiteDir` 하나에서 성격별 영역 디렉터리 유도

> 사이트 데이터(관리 store·패키지·콘텐츠·녹취·로그·통계·상태·계측기)가 놓이는 위치의 정본. 운영자가 적는 값은
> 사이트 루트 `CimsSiteDir` 하나이고, 성격별 영역 경로는 OAM 이 유도해 각 모듈에 준다. 각 영역 안의 파일 형식
> (녹취 세션 디렉터리·SIP 5분 버킷·통계 피라미드·알람 jsonl)은 해당 기능 문서가 정본이다.

## 1. 원칙

- **위치는 경로 설정이다.** NAS 든 로컬 디스크든 운영 상황이 정한다. 마운트는 [시스템/인프라] 의 일이고 OAM 은 마운트 여부를
  기동 조건으로 삼지 않는다(`CimsRuntimeMount` 는 공유 스토리지 이중화 사이트가 mount guard 를 켜는 선택값 — [oam_ha.md](oam_ha.md) §4.1).
- **입력은 하나, 유도는 여덟.** 운영자는 사이트 루트 `CimsSiteDir` 하나를 적고, 영역별 디렉터리는 거기서 유도된다. 영역 키를
  따로 적으면 그 값이 이긴다("녹취만 다른 볼륨" 같은 요구를 키 하나로 처리).
- **같은 취급을 받는 데이터끼리 한 영역.** 나누는 기준은 백업 대상인가 · 보존 기간이 있는가 · 용량이 큰가 · 재생성 가능한가.
  섞여 있으면 보존·백업·용량 정리를 한 규칙으로 걸 수 없다.
- **위치를 정하는 창구는 base `oam` 하나.** 모듈은 자기 영역의 루트 경로를 배포 때 받고 그 아래 고정 이름만 쓴다. 모듈마다
  경로를 입력받으면 CSP 가 쓰는 녹취 위치와 OAM 이 읽는 위치가 갈라진다.
- 사이트 디렉터리는 **모듈이 도는 모든 노드에서 같은 경로**여야 한다(원격 CMP 가 쓴 녹취를 OAM 이 읽는다) — 다중 노드 사이트는
  공유 스토리지를 같은 지점에 마운트한다.

## 2. 레이아웃

```
<CimsSiteDir>/                 예 /mnt/cims/test48 (NAS 하나를 여러 사이트가 나눠 쓸 때 사이트 디렉터리) 또는 /var/lib/cims (로컬)
  runtime/      관리 store — control/ collections/ console/ provision/ services/ … + .owner.json·.owner.lock (단일 writer)
  packages/     패키지 저장소 — <모듈>-<버전>.tar.gz, 삭제분 보관 .trash/
  content/      서비스 콘텐츠 — announcements/{catalog.jsonl, op/, sub/} (안내음성 라이브러리) · mcdata_fd/ (MCData FD 원본)
  recordings/   녹취·통신 기록 — volte/<연>/<월>/<일>/<시>/<접두>/<발신>/<키>.d/ (+ <시>/index.json)
                · ptt/<그룹>/group.json · ptt/<그룹>/<연>/<월>/<일>/<시>/S…_N/ (+ floor.jsonl)
                · message/<그룹>/<연>/<월>/<일>/<시>/messages.jsonl · message_direct/<연>/<월>/<일>/<시>/messages.jsonl
  log/          관측 로그 — sip/<연>/<월>/<일>/<시>/<sysid>*.{msg,flow}.<mm5>.jsonl (전 모듈 SIP·제어 메시지·flow 5분 버킷)
                · alerts/ · events/ · fm_catalog/ (FM 수집) · leak_reclaim/ (CMP 누수 회수)
  stats/        통계·색인 — 1m/ 1h/ 1d/ 1M/ + .rollup_state.json (SIP 통계 피라미드) · ptt_index/ (PTT 세션 색인)
                · ptt_attempts/<연월일>.jsonl (PTT 시도 장부)
  state/        휘발성 — volte/ ptt/ (진행 중 세션, 가입자별 1파일) · stats_rollup.lock
                (CSP 유휴 write probe `.probe` 는 recordings/·state/ 각각에 쓰고 곧바로 지운다)
  tester/       계측기 DataDir — topologies/ scenarios/ runs/ samples/
```

| 영역 | 백업 | 보존 | 용량 | 재생성 |
|---|---|---|---|---|
| runtime | 필수 | 영구 | 작음 | 불가 |
| packages | 선택 | 버전 정책 | 큼 | 재등록 |
| content | 필수 | 영구 | 중간 | 불가 |
| recordings | 정책 | 법적 보존 | 가장 큼 | 불가 |
| log | 불필요 | 기간 후 삭제 | 큼 | 불가 |
| stats | 불필요 | 기간 | 중간 | 원천에서 가능 |
| state | 불필요 | 없음 | 작음 | 자동 |
| tester | 선택 | 운영자 | 중간 | 부분 |

녹취 id(콘솔·관제 앱이 쓰는 세션 디렉터리의 상대 경로 — `volte/…`, `ptt/<그룹>/…`)는 녹취 영역 루트 기준이다.
시크릿·인증서(`modules/oam/runtime/_secrets`·`cert`)는 **노드 로컬**이고 사이트 디렉터리로 오지 않는다(oam_ha.md §5).
관리 store 이름 `runtime/` 은 HA 이관·agent 승격 preflight 가 가정하는 이름이다.

## 3. 설정 모델

### 3.1 base `oam` 배포설정 — 템플릿 `store` 섹션 [사이트 디렉터리] (`scope=service`, 그룹 멤버 동일)

| 키 | 의미 | 비면 (사이트 디렉터리) | 비면 (단일 루트 — `CimsSiteDir` 없음) |
|---|---|---|---|
| `CimsSiteDir` | 사이트 루트 — **운영자가 적는 값 하나** | — | — |
| `CimsRuntimeDir` | 관리 store | `<site>/runtime` | `<CimsRuntimeMount>/runtime` > 노드 로컬 `modules/oam/runtime` |
| `Packages.Dir` | 패키지 저장소 | `<site>/packages` (보관소 `.trash/`) | `<store>/pkg_files` (보관소 `<store>/pkg_files_trash`) |
| `Content.Dir` | 서비스 콘텐츠 | `<site>/content` | `<log>` |
| `Recording.Dir` | 녹취 | `<site>/recordings` | `<log>` |
| `ServiceLogging.Dir` | 관측 로그 | `<site>/log` | 노드 로컬 `modules/oam/runtime/service_log` |
| `Stats.Dir` | 통계·색인 | `<site>/stats` | `<log>/stats` |
| `State.Dir` | 휘발성 상태 | `<site>/state` | `<log>/state` |
| `CimsRuntimeMount` | (선택) mount guard 마운트 지점 | 검사 없음 | 검사 없음 |

- **유도는 `services/paths.py` 한 곳**이다(`site_dir`·`runtime_store_dir`·`packages_dir`·`content_dir`·`recordings_dir`·
  `service_log_dir`·`sip_log_dir`·`stats_dir`·`state_dir`·`tester_dir`, 묶음 `area_dirs`·`layout_values`). OAM·oam-svc 의
  읽기 코드도, 부트스트랩도 이 모듈을 부른다.
- **단일 루트 레이아웃** — `CimsSiteDir` 가 비어 있는 사이트(옛 사이트·마운트 없이 설치한 부트스트랩 노드)는 영역이 전부 서비스
  로그 루트 아래로 유도된다. 업그레이드만으로 녹취·통계·상태·패키지 경로가 옮겨지지 않는다. 사이트 디렉터리로 옮기는 절차는 §6.
- 계측기 DataDir 기본값은 `<site>/tester` 다(단일 루트면 계측기 모듈 기본값 — `<모듈>/runtime/data`).

### 3.2 실체화 — 모듈 템플릿의 `site_area`

모듈은 자기가 쓰는 영역을 **템플릿 필드 속성**으로 선언한다: `"site_area": "<영역>"`. 실체화(`agents._materialize_deploy_config`)가
매 디스패치마다 base `oam` 배포설정에서 영역 경로를 유도해(`_site_source` — `_store_source` 와 같은 출처 규칙: overlay(desired
state)가 레이아웃을 정했으면 그것, 아니면 돌고 있는 OAM 의 값, 멤버 간 값이 갈리면 현재 설정) 그 키에 넣는다.

- 기본 = **무조건**(derived) — 모듈 overlay 에 남은 옛 경로도 덮는다. 콘솔은 그 필드를 `자동 채움` 배지로 보여 준다.
- `"site_area_mode": "default"` = 비어 있을 때만(운영자 입력이 이긴다) — 계측기 `Tester.DataDir` 가 이것이다.
- base `oam` 자신은 입력 창구라 `site_area` 를 쓰지 않는다 — 실체화가 유도한 영역 경로 전부(`layout_values`)를 `config.json` 에
  구체값으로 적는다. 레이아웃 입력(`CimsSiteDir`·`CimsRuntimeMount`·영역 키)은 overlay 명시값이 우선이고, overlay 가 정하지
  않았으면 돌고 있는 OAM 의 레이아웃을 전파한다(아직 정하지 않은 노드에 그룹 값 주입).

| 모듈 | 받는 키 (`site_area`) |
|---|---|
| csp | `Setup.ServiceLogging.Dir`(log) · `Setup.Recording.Dir`(recordings) · `Setup.State.Dir`(state) · `Setup.Stats.Dir`(stats) |
| cmp | `ServiceLogging.Dir`(log) · `Recording.Dir`(recordings) · `Content.Dir`(content) |
| cmdp | `ServiceLogging.Dir`(log) · `Content.Dir`(content) — `McDataFd.Dir` 명시값이 없으면 `<content>/mcdata_fd` |
| csc | `ServiceLogging.Dir`(log) · `Recording.Dir`(recordings) · `State.Dir`(state) · `Content.Dir`(content) — `McDataFd.Dir` 동일 규칙 |
| oam-svc | `ServiceLogging.Dir`(log) · `Recording.Dir`(recordings) · `Stats.Dir`(stats) · `State.Dir`(state) · `Content.Dir`(content) |
| oam-cims-tester | `Tester.DataDir`(tester, `default` 모드) |

모듈은 키가 비어 있으면(실체화를 거치지 않은 소스 트리 실행) 같은 단일 루트 규칙으로 해석한다 — 녹취·콘텐츠 = 로그 루트,
상태 = `<로그>/state`, 통계 = `<로그>/stats`. 관리 store 경로(`CimsRuntimeDir`)는 영역과 별개로 store 를 쓰는 모듈(oam-svc·csc)에만
주입된다(oam_ha.md §4.1).

## 4. 영역별 쓰는 쪽·읽는 쪽

| 영역 | 경로 | 쓰는 쪽 | 읽는 쪽 |
|---|---|---|---|
| log | `sip/<연/월/일/시>/` msg·flow 5분 버킷 | CSP `SipMessageLogger` · CMP · CMDP · CSC `logger` · OAM `logger` | OAM flow/통계 조회(`flow_logger`·`handlers/stats`), oam-svc 1분 집계 |
| log | `alerts/` `events/` `fm_catalog/` | oam-svc FM 수집기(`fm_ingest`)·알람 sweeper | 콘솔 알람·이벤트 |
| log | `leak_reclaim/` | CMP | 콘솔 누수 회수 조회 |
| recordings | `volte/…/<키>.d/` (call.json·participants.jsonl·세그먼트) + `<시>/index.json` | CSP `CallDir`(통화 기록) + CMP 녹취기(세그먼트 — 세션 디렉터리는 CSP 가 `record_dir` 로 준다) | OAM 녹취 조회·이력·통계, CSC 관제 앱 이력 |
| recordings | `ptt/<그룹>/…` (group.json·session.json·events.jsonl·floor.jsonl·세그먼트) | CSP `CallDir` + CMP | OAM PTT 색인·이력·녹취, CSC 관제 앱 이력 |
| recordings | `message/` `message_direct/` | CSP `CallDir`(SDS 보관) | OAM 메시지 조회, CSC 관제 앱 이력 |
| stats | `1m/ 1h/ 1d/ 1M/` | oam-svc `stats_rollup` | OAM·oam-svc 통계 조회 |
| stats | `ptt_index/` | OAM PTT 색인 스윕(`ptt_index`) | `/ptt/history`·`/ptt/sessions` |
| stats | `ptt_attempts/` | CSP `CallDir` | oam-svc 집계 |
| state | `volte/` `ptt/` | CSP `CallDir` | OAM 활성 세션·PTT 색인, CSC 관제 앱 이력 |
| recordings·state | `.probe` (쓰고 곧바로 지움) | CSP 유휴 write probe — 무호 구간의 볼륨 소실을 A-PRC-013 으로 | — |
| state | `stats_rollup.lock` | oam-svc(집계 writer 잠금) | — |
| content | `announcements/` | OAM 안내음성 라이브러리(원본·카탈로그) → agent `/module-file` 가 각 CMP 노드의 `Content.Dir` 에 배치 | CMP 안내 재생기(op·sub 음원 — sys 는 패키지 동봉) |
| content | `mcdata_fd/` | CSC(`POST /mcdata/fd`)·CMDP | CSC·CMDP |
| tester | `topologies/ scenarios/ runs/ samples/` | 계측기 | 계측기 |

콘솔 화면(녹취·이력·통계·알람)은 전부 OAM/oam-svc 가 경로를 계산하므로 영역을 모른다. 안내음성 원본이 콘텐츠 영역에 있으므로
CMP 업그레이드 때 이어받을 모듈 자원 파일이 없다(카탈로그 `config/announcements.jsonl` 은 컬렉션과 같이 이어받는다).

## 5. 설치·합류·이관

- **부트스트랩** — `install.sh --site-dir DIR` 하나로 끝난다. `--mount-src`/`--runtime-mount` 만 주면 사이트 디렉터리 = 그 마운트
  지점이다. `--runtime-dir`·`--log-dir` 는 영역 override 로 남는다. 대화식 [6/7] 은 "사이트 디렉터리 [노드 로컬]" 을 묻고, 공유
  스토리지를 새로 붙일 원본은 그 뒤 선택 문항이다. 아무것도 주지 않으면 노드 로컬 단일 루트다. 부트스트랩이 쓰는 base oam
  `config.json` 의 유도 경로는 패키지의 `services/paths.py`(`layout_values`)로 계산한다 — 실체화와 같은 모양(설치 직후 A-PRC-003 방지).
- **관리평면 합류** — 합류 노드는 peer 의 `CimsSiteDir`·마운트 지점을 계승한다(`/api/v1/ha/join` 신원 번들 `runtime.CimsSiteDir`).
- **공유 store 이관**(`POST /ha-groups/{id}/shared-store/migrate`, oam_ha.md §9.4) — 사이트 디렉터리 구성이면 **사이트 전체를 마운트
  지점으로 옮긴다**: base oam overlay 의 `CimsSiteDir` 를 마운트 지점으로 바꾸고, agent `migrate_oam_store` job 이 정지창에 관리
  store 와 패키지·콘텐츠 영역을 복사(`site_copies`)한 뒤 기동하고, 로그·녹취·통계 영역은 기동 뒤 백그라운드로 합친다
  (`site_merges` — 시간축 분할 구조라 상대 경로 그대로 합쳐도 겹치지 않는다). 영역 경로를 따로 적은 영역은 옮기지 않는다.
  단일 루트 구성은 store 복사 + 노드 로컬 서비스 로그 합류다.

## 6. 단일 루트 사이트를 사이트 디렉터리로 옮기기

`CimsSiteDir` 가 비어 있는 사이트는 그대로 동작한다. 옮길 때는 정지창에서:

1. 모듈 정지(oam 포함) — 녹취·통계 기록이 멈춰야 한다.
2. 새 사이트 디렉터리 아래로 이동(같은 볼륨이면 `mv` 즉시): `<store>` → `runtime/`, `<store>/pkg_files/*` → `packages/`,
   `<log>/volte`·`<log>/ptt/<그룹>`·`<log>/message*` → `recordings/`, `<log>/<연>/` → `log/sip/<연>/`, `<log>/alerts`·`events`·
   `fm_catalog`·`leak_reclaim` → `log/`, `<log>/stats/*` → `stats/`, `<log>/ptt/index` → `stats/ptt_index`,
   `<log>/ptt/attempts` → `stats/ptt_attempts`, `<log>/mcdata_fd` → `content/mcdata_fd`, `<log>/state` 는 버린다(휘발성).
3. base oam 배포설정에 `CimsSiteDir` 를 넣고 `CimsRuntimeDir`·`ServiceLogging.Dir` 명시값을 지운 뒤 모든 배포에 update_config →
   기동. 안내음성 운영자 음원은 콘솔 [안내음성] [배포] 로 새 콘텐츠 영역에 다시 내린다.

## 7. 남은 것

- 영역별 보존·정리 정책의 일원화(현재는 기능별 보존 설정 — 알람·이벤트·통계·계측기 run 각자).
- `log/modules/`(모듈 프로세스 로그를 사이트 디렉터리로 모으는 선택 영역) — 현재 프로세스 로그는 모듈 설치 트리 `log/`.
