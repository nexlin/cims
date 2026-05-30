# 콘솔 플랫폼화 (OAM Console Platformization)

OAM 콘솔을 **CIMS 전용 화면 모음**에서 **범용 O&M 포털**로 전환. 코어는 서비스를 모르고,
서비스(CIMS 등)는 **데이터/매니페스트**로 자신을 등록한다. 한 코드베이스 + 데이터 구동.

## 1. 레이어

| 레이어 | 책임 | 비고 |
|---|---|---|
| **코어** | 셸(헤더/사이드바/테마) · 위젯 합성 엔진 · shape 위젯 · 레이아웃/메뉴 영속 · 패키징/배포/문서 | 서비스 무지 |
| **서비스 pack** (CIMS) | nav 섹션 · 위젯 · **데이터 소스** · 백엔드(CSC) · Service Descriptor 데이터 | `services/<id>/` + descriptor |

## 2. nav 합성

`routes.tsx` = `CORE_SECTIONS`(대시보드/패키징/배포/문서) + 서비스 매니페스트 섹션(order 병합).
- `nav-types.ts` — `RouteSection` / `ServiceManifest`.
- `services/registry.ts` — `SERVICE_MANIFESTS = [cimsManifest]`. 새 서비스는 여기에 매니페스트만 추가.
- 메뉴 override(순서/라벨/표시)는 `console_menu`(OAM 영속), 코드 SECTIONS 가 SoT.

## 3. 위젯 합성 (page = 레이아웃)

page 는 고정 화면이 아니라 **위젯 배치(PageLayout)**.

- `widgets/types.ts` — `WidgetDef`(id/title/category/component/defaultSize) · `WidgetPlacement` · `PageLayout`.
- `widgets/registry.ts` — 코어 위젯 + 서비스 `manifest.widgets` 병합(lazy — 순환 import 안전). `widgetsByCategory()` 로 편집 드롭다운을 카테고리(infra/service/stats/view/event/etc)로 그룹.
- `widgets/GridRenderer.tsx` — 12-col 그리드. 위젯은 자체 chrome 렌더.
- `widgets/EditableLayout.tsx` — admin `[✎ 편집]` → 위젯 추가/제거/순서/폭 → `PUT /console/layouts/<id>` 영속(없으면 seed).
- `widgets/LayoutRoute.tsx` — route 를 EditableLayout 으로 렌더(출력 섹션 page = 합성 레이아웃).

영속: OAM `/api/v1/console` (`console_layouts` / `console_menu` 도메인). 저장본 없으면 404 → 프론트 seed.

## 4. shape 위젯 + 데이터 소스 (완전 데이터 구동)

**데이터 성격(shape)이 같고 소스만 다른 출력**(차트/표/KPI/분포)을 위젯마다 만들지 않는다.
코어가 **shape**(presentation)를, 서비스가 **데이터 소스**(데이터)를 제공 → shape 위젯이 소스를 선택.

- shape: `time-bar` · `kpi` · `distribution` · `table` (`widgets/shapes/types.ts`). 코어 위젯 `shape.*` (category `view`).
- 렌더러: `widgets/shapes/renderers.tsx` — shape 데이터만 받아 그린다(소스/fetch 무관).
- **데이터 소스 등록 = Service Descriptor 의 `data_sources[]` (백엔드 데이터)**. 모듈/alert_rules 와 동일하게 descriptor 로 등록 — 새 소스는 **descriptor 편집만**(프론트 코드 0).
- 스펙 → DataSource 빌더 `widgets/shapes/dataSourceSpec.ts` — 선언적 매핑을 해석하는 **정규화 계층**:
  - `endpoint` + `query`(date/granularity) → fetch
  - shape별 `map`: `from`(중첩 경로) · `label`(필드 폴백 `["hour","date"]`) · `value` · `fromObject`(dict→행/항목) · `path` · `format`(duration 등)
  - 이질적 응답(`voip.buckets` vs `buckets`, `hour` vs `date`, dict형 `method_counts`)을 shape 계약으로 정규화.
- 카탈로그 로드: `widgets/shapes/sourceRegistry.ts` — `GET /service-descriptors/data-sources` 1회 fetch(모듈 싱글톤 + 구독 훅 `useDataSourceCatalog`) + `(src|date|gran)` 단기 캐시. `ShapeWidget` 이 소스 dropdown 으로 소비.

### data_source 스펙 예 (descriptor `data_sources[]`)
```json
{ "id": "cims.msg.sip", "label": "SIP 메시지", "shapes": ["time-bar","table"],
  "endpoint": "/stats/messages/sip", "query": ["date"],
  "map": { "time-bar": { "from": "buckets", "label": ["hour"], "value": "count" },
           "table":    { "fromObject": "method_counts", "columns": ["메서드","건수"] } } }
{ "id": "cims.svc.volte", "label": "VoLTE 서비스", "shapes": ["kpi","time-bar","distribution"],
  "endpoint": "/stats/service/volte", "query": ["date","granularity"],
  "map": { "kpi": { "items": [ {"label":"호 시도","path":"voip.total_attempts","unit":"건"},
                               {"label":"평균 통화시간","path":"voip.avg_duration_sec","format":"duration"} ] },
           "time-bar": { "from":"voip.buckets","label":["hour","date"],"value":"attempts" },
           "distribution": { "fromObject":"voip.end_reasons","totalPath":"voip.total_attempts" } } }
```

## 5. Service Descriptor (백엔드, 데이터 구동)

OAM 코어의 CIMS 하드코딩(모듈맵/빌드 화이트리스트/제어 허용목록/alert/데이터 소스)을 descriptor 데이터로 분리.

- 저장: file_store `services` 도메인. 시드: `csc/src/services/service_descriptors_seed/*.json`(CIMS=`cims.json`). store 비면 1회 주입.
- 집계: `csc/src/services/service_registry.py` — `all_modules` / `valid_module_names` / `controllable_modules` / `alert_rules`(코어 host 규칙 disk_high/module_down 병합) / `data_sources`.
- API: `oam/src/handlers/service_descriptors.py` — `GET /api/v1/service-descriptors[/{id}]` · `/modules` · `/data-sources` · `PUT`(modules+alert_rules+data_sources 보존) · `DELETE`.
- 콘솔: `/deploy/service-defs`(ServiceDescriptorsPage) — **폼 편집**(`pages/descriptors/forms.tsx`의 `ServiceForm`: id/label·모듈·alert_rules 행 추가/삭제, `DataSourceForm`: shapes 체크 + shape별 매핑 폼) + 전체 JSON "고급" fallback. 데이터 소스는 카드의 데이터 소스 섹션에서 추가/편집/삭제.

## 6. 서비스 정규화 전략

범용 코어를 먼저 안정화하고, 서비스 종속 데이터(통계/이력 응답)는 **하나씩 정규화**해 소스로 등록한다.
백엔드 응답을 바꾸기 어려운 경우 매핑 DSL(§4)이 정규화 계층 역할을 한다. 새 서비스는
nav 매니페스트 + 위젯 + descriptor(modules/alert_rules/data_sources)만 추가하면 코어 위에 얹힌다.

## 관련
- `02_deployment.md` (Agent/Package/Deployment) · `oam_csc_split.md` (CSC↔OAM 분리)
- `features/monitoring.md` (통계/이력/모니터링 데이터)
