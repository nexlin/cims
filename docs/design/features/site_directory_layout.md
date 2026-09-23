# 사이트 디렉터리 레이아웃 — `CimsSiteDir` 하나에서 성격별 영역 디렉터리 유도

> 설계 정본(2026-09-23 확정, 구현 예정). 현재 구현은 `ServiceLogging.Dir` 하나를 모든 모듈이 루트로 받아 그 바로 아래에
> 고정 이름(`2026/`·`volte/`·`ptt/`·`state/`·`stats/`·`alerts/`…)을 만드는 구조다. 이 문서가 바꾸는 것은 **디렉터리 분류와
> 설정 키**이고, 각 영역 안의 파일 형식(녹취 세션 디렉터리·SIP 5분 버킷·통계 피라미드·알람 jsonl)은 그대로다.

## 1. 원칙

- **위치는 경로 설정이다.** NAS 든 로컬 디스크든 운영 상황이 정한다. 마운트는 [시스템/인프라] 의 일이고 OAM 은 마운트 여부를
  기동 조건으로 삼지 않는다(`CimsRuntimeMount` 는 공유 스토리지 이중화 사이트가 mount guard 를 켜는 선택값 — [oam_ha.md](oam_ha.md) §4.1).
- **입력은 하나, 유도는 여덟.** 운영자는 사이트 루트 `CimsSiteDir` 하나를 적고, 영역별 디렉터리는 거기서 유도된다. 영역 키를
  따로 적으면 그 값이 이긴다("녹취만 다른 볼륨" 같은 요구를 키 하나로 처리).
- **같은 취급을 받는 데이터끼리 한 영역.** 나누는 기준은 백업 대상인가 · 보존 기간이 있는가 · 용량이 큰가 · 재생성 가능한가.
  섞여 있으면 보존·백업·용량 정리를 한 규칙으로 걸 수 없다.
- **모듈 안에 고정 하위 이름을 두지 않는다.** 모듈은 자기 영역의 루트 경로를 설정으로 받고 그 아래만 쓴다.

## 2. 레이아웃

```
<CimsSiteDir>/                 예 /mnt/cims/test48 (NAS 하나를 여러 사이트가 나눠 쓸 때 사이트 디렉터리) 또는 /var/lib/cims (로컬)
  runtime/      관리 store — control/ collections/ console/ provision/ services/ … (단일 writer, 작음, 백업 필수)
  packages/     패키지 저장소 — <모듈>-<버전>.tar.gz (재등록 가능, 큼)
  content/      운영자가 넣는 서비스 콘텐츠 — mcdata_fd/ (MCData FD 파일) · announcements/ (op 음원)  (백업 대상)
  recordings/   녹취 — volte/<연/월/일/시>/<발신>/… · ptt/<그룹>/<연/월/일/시>/S…_N/…  (법적 보존, 가장 큼; 변환본 mp4·peaks 는 원본 옆)
  log/          관측 로그 — sip/<연/월/일/시>/… (SIP 메시지·flow 5분 버킷) · alerts/ · events/ · fm_catalog/ · modules/ (선택: 프로세스 로그)
  stats/        통계·색인 — 1m/ 1h/ 1d/ 1M/ (SIP 통계 피라미드) · ptt_index/ (세션 색인) · ptt_attempts/ (PTT 시도 장부)  (원천에서 재생성 가능)
  state/        휘발성 — volte/ ptt/ (진행 중 세션 상태) · 잠금(.writer.lock) · write probe  (재기동 때 비워도 됨)
  tester/       계측기 DataDir — topologies/ scenarios/ runs/ samples/  (계측기 전용)
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

`runtime/` 이름은 유지한다 — HA 이관(`_migrate_shared_store`)·agent 승격 preflight·`paths.runtime_store_dir` 가 `runtime` 을 가정하며,
실익은 이름이 아니라 `log/` 밑에 섞인 것을 성격별로 빼내는 데 있다. 시크릿·인증서(`modules/oam/runtime/_secrets`·`cert`)는 지금처럼
**노드 로컬**이고 사이트 디렉터리로 오지 않는다.

## 3. 설정 모델

base `oam` 배포설정(템플릿 `store` 섹션) — 전부 `scope=service`(그룹 멤버 동일):

| 키 | 의미 | 비면 |
|---|---|---|
| `CimsSiteDir` | 사이트 루트. **운영자가 적는 값 하나** | (옛 사이트) `CimsRuntimeMount` 가 있으면 그 마운트, 없으면 노드 로컬 `modules/oam/runtime` 의 부모 |
| `CimsRuntimeDir` | 관리 store | `<site>/runtime` |
| `Packages.Dir` | 패키지 저장소 | `<site>/packages` (**변경**: 지금은 `<store>/pkg_files`) |
| `Content.Dir` | 서비스 콘텐츠 | `<site>/content` |
| `Recording.Dir` | 녹취 | `<site>/recordings` |
| `ServiceLogging.Dir` | 관측 로그 | `<site>/log` |
| `Stats.Dir` | 통계·색인 | `<site>/stats` |
| `State.Dir` | 휘발성 상태 | `<site>/state` |
| `CimsRuntimeMount` | (선택) mount guard 용 마운트 지점 | 검사 없음 |

유도는 `services/paths.py` 한 곳(`site_dir(cfg)` + 영역별 `xxx_dir(cfg)` = 명시값 > `<site>/<영역>`), 실체화(`agents._materialize_deploy_config`)가
매 디스패치마다 여덟 키를 계산해 각 모듈의 `config.json` 에 넣는다 — 모듈은 자기가 쓰는 영역 키만 읽는다(oam-svc 는 store·log·stats,
csc 는 store·log·state·content, csp 는 log·state, cmp 는 recordings·content(announcements), cmdp 는 log·content, 계측기는 `Tester.DataDir`).
`_store_source` 는 지금처럼 oam 배포설정에서 유도한다.

부트스트랩 `install.sh --site-dir DIR` 하나로 끝난다(`--runtime-dir`·`--log-dir` 는 override 로 남긴다). 대화식 [6/7] 은 "사이트 디렉터리
[노드 로컬]" 을 묻고, 공유 스토리지 마운트는 그 뒤 선택 문항이다.

## 4. 모듈별 변경 — 고정 하위 이름 → 영역 키

| 모듈 | 지금 | 바꿀 것 |
|---|---|---|
| CSP `SipMessageLogger`·flow 로거 | `<ServiceLogging.Dir>/<연>/<월>/<일>/<시>/…` | `<ServiceLogging.Dir>/sip/<연>/…` (`log/sip`) |
| CSP `CallDir` | `<ServiceLogging.Dir>/state/volte|ptt` + write probe | `State.Dir` (`state/volte|ptt`, probe 도 여기) |
| CSC 로거·CallDir | 위와 같음 | 위와 같음 |
| CMP 녹취기(`Recorder`) | `<ServiceLogging.Dir>/volte`·`/ptt/<그룹>` | `Recording.Dir` (`recordings/volte`·`recordings/ptt/<그룹>`) |
| CMP 안내 재생기 | 모듈 트리 `announcements/op` (업그레이드 때 이어받기) | `Content.Dir/announcements` (이어받기 코드 제거) |
| CMDP | `McDataFd.Dir`(지금 `<log>/mcdata_fd`) | `Content.Dir/mcdata_fd` 로 유도, 키는 유지 |
| oam-svc FM 수집기(`fm_ingest`) | `<log>/alerts`·`/events`·`/fm_catalog` | 그대로 `log/` 아래(관측 로그) |
| oam-svc 통계(`stats_rollup`) | `<log>/stats` + `.writer.lock` | `Stats.Dir` (`stats/1m…`), 잠금은 `State.Dir` |
| OAM PTT 색인·시도 장부(`_sweep_ptt_index`·Y6) | `<log>/ptt/index`·`/ptt/attempts` | `Stats.Dir/ptt_index`·`ptt_attempts` (녹취 스캔 원천은 `Recording.Dir/ptt`) |
| OAM 녹취 조회(`handlers/recording.py`) | `_service_log_dir` 기준 `volte/`·`ptt/` | `Recording.Dir` 기준 |
| OAM 패키지 서빙 | `<store>/pkg_files` | `Packages.Dir` = `<site>/packages` |
| 계측기 `Tester.DataDir` | 독립 키 | `<site>/tester` 로 유도, 키 유지 |
| 누수 회수(`leak_reclaim`) | `<log>/leak_reclaim` | `log/leak_reclaim` 그대로 |

콘솔 화면(녹취·이력·통계·알람)은 전부 OAM/oam-svc 가 경로를 계산하므로 변경 없음. 계측기 토폴로지 `nodes.*.logs`(SSH 관측 대상 로그 경로)와
`target_evidence recording_created` 는 새 경로를 본다.

## 5. 이행

- **.48**: 새 설치 사이트라 데이터 이동 없음 — OAM·모듈 재패키징 → `install.sh --site-dir /mnt/cims/test48` 로 재설치(또는 base oam
  overlay 에 `CimsSiteDir` 넣고 모듈 재기동) → 계측기 `VOLTE-CALL-BASIC`·`PTT-GROUP-CALL-VIDEO`(녹취 증거)·통계 화면으로 확인.
  절차 뼈대는 [docs/dev/testbed_48_site_migration.md](../../dev/testbed_48_site_migration.md) 그대로.
- **옛 사이트(.45 등)**: `CimsSiteDir` 를 비워 두면 `CimsRuntimeMount`/`ServiceLogging.Dir` 명시값이 지금 경로를 유지한다 — 업그레이드만으로
  경로가 옮겨지지 않는다. 옮길 때는 정지창에서 디렉터리를 새 영역으로 `mv` 한 뒤 `CimsSiteDir` 를 넣는다(같은 볼륨이면 즉시).
- 옛 이름 폴백은 두지 않는다 — 코드는 영역 키만 읽는다(문서 원칙: 최종 상태만).

## 6. 구현 순서 (다음 세션)

1. `services/paths.py` 영역 유도 함수 + `config_template.json` `store` 섹션에 여덟 키 + `oam.json` 기본값 + 실체화 주입(`agents.py`) + `_store_source`.
2. `install.sh --site-dir`(대화식 [6/7] 문구 포함) + `oam-deploy.py` 는 변경 없음.
3. 모듈: CMP 녹취/안내 · CSP/CSC 로거·CallDir · oam-svc 통계·FM·PTT 색인 · OAM 녹취 조회 · CMDP · 계측기 유도.
4. 문서: [02_deployment.md](../02_deployment.md) 레이아웃 그림, [oam_ha.md](oam_ha.md) §4.1, [recording.md](recording.md)·[flow_logging.md](flow_logging.md)·
   [sip_statistics.md](sip_statistics.md)·[alarm_pipeline.md](../alarm_pipeline.md)·[announcements.md](announcements.md) 경로 표,
   [initial_install.md](../../user-manual/initial_install.md) §2 옵션.
5. .48 재배포 + 계측기 확인, `testbed_48_site_migration.md` 의 레이아웃을 이 문서 기준으로 갱신.
