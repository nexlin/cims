# 콘솔 플랫폼화 (OAM Console Platformization)

OAM 콘솔을 **CIMS 전용 화면 모음**에서 **범용 O&M 포털**로 전환. 코어는 서비스를 모르고,
서비스(CIMS 등)는 **데이터/매니페스트**로 자신을 등록한다. 한 코드베이스 + 데이터 구동.

## 1. 레이어

| 레이어 | 책임 | 비고 |
|---|---|---|
| **코어** | 셸(헤더/사이드바/테마) · 위젯 합성 엔진 · shape 위젯 · 레이아웃/메뉴 영속 · 패키징/배포/문서 | 서비스 무지 |
| **서비스 pack** (CIMS) | nav 섹션 · 위젯 · **데이터 소스** · 백엔드(CSC) · Service Descriptor 데이터 | `services/<id>/` + descriptor |

## 2. nav 합성 — OAM 표준(FCAPS) 2-레벨 펼침형

메뉴 정보구조(IA)는 통신 OAM 표준 **FCAPS**(ITU-T M.3400) + EMS 관례(Nokia NetAct 의
Monitor/Administer, Huawei U2000 Topo/Fault/Perf/Config, TM Forum eTOM Assurance/Fulfillment)를
따른다. 사이드바는 **[영역 → 그룹 → 하위항목]** 2-레벨 펼침형(accordion).

- **2 대영역(`NavArea`)**: `ops`(운용=Assurance) / `admin`(관리=Fulfillment). `nav-types.ts` 의 `NAV_AREA_ORDER`/`NAV_AREA_LABELS`.
- **그룹(`RouteSection`)** = FCAPS 영역. `area` 필드로 대영역 귀속:
  - 운용: **대시보드**(`dashboard`) · **장애**(`fault`, Fault — 활성알람/이력) · **성능**(`perf`, Performance — 서비스현황/통계) · **기록**(`records`, Accounting — 호·세션 이력)
  - 관리: **구성**(`config`, Configuration — 가입자/서비스정의) · **시스템**(`system`, Inventory/Maint — 시스템·인프라/HA/패키지/**외부 시스템**) · **릴리스**(`release`, SW Mgmt — 검증/패키징) · **문서**(`docs`)
- `routes.tsx` `CORE_SECTIONS`(대시보드/장애/시스템/릴리스/문서) + 서비스 매니페스트 섹션(구성/성능/기록) order 병합.
- 섹션이 자기 `basePath` 밖 route 도 가질 수 있어(예: 구성↔`/deploy/service-defs`) `findSectionByPath` 는 route 멤버십으로 매칭.
- `Sidebar.tsx` 가 area 로 버킷 → 그룹 펼침(현재 그룹 자동 펼침), 단일-leaf 그룹은 헤더 클릭 직행. SubTabs(상단 탭)는 폐기.
- `nav-types.ts` — `RouteSection`(+`area`) / `ServiceManifest`.
- `services/registry.ts` — `SERVICE_MANIFESTS = [cimsManifest]`. 새 서비스는 매니페스트에 `area` 지정 섹션 추가.
- 메뉴 override(순서/라벨/표시)는 `console_menu`(OAM 영속), 코드 SECTIONS 가 SoT.

## 3. 위젯 합성 (page = 레이아웃)

page 는 고정 화면이 아니라 **위젯 배치(PageLayout)**. `App.tsx` 의 `EditablePageHost` 가 **모든 route**
를 `EditableLayout` 으로 감싸므로, 대시보드뿐 아니라 어느 페이지에서도 admin 이 위젯을 얹고 배치할 수
있다(고정 페이지는 자신이 `page:<경로>` 단일 위젯으로 들어간 seed).

- `widgets/types.ts` — `WidgetDef`(id/title/category/component/defaultSize{w,h}) · `WidgetPlacement` · `PageLayout`.
- `widgets/registry.ts` — 코어 위젯 + 서비스 `manifest.widgets` 병합(lazy — 순환 import 안전). `widgetsByCategory()` 로 편집 드롭다운을 카테고리(infra/service/stats/view/event/etc)로 그룹.
- `widgets/gridLayout.ts` — **자유 2D 그리드 엔진(순수 함수, 무의존)**. 48칸(`GRID_COLS`, 칸당 ≈2%)×N행 셀 좌표계에서
  `overlap`/`compact`(겹침 아래로 밀기 + 빈 행 상단 compaction/중력) · `moveItem`/`resizeItem` ·
  `addToFirstFree`/`removeAt` · `flowToGrid`(legacy→grid migrate) · 배치모드 판별(`isGridLayout`). 결정적·idempotent.
- `widgets/GridRenderer.tsx` — 뷰 렌더. **2 모드 하위호환**: placement 에 `x/y` 있으면 12칸×N행 CSS grid
  (`grid-column`/`grid-row`, `grid-auto-rows` = 셀 높이), 없으면 legacy flow(합>12 wrap). 위젯은 자체 chrome 렌더.
- `widgets/EditableLayout.tsx` + `widgets/GridEditor.tsx` — admin `[✎ 편집]` → **헤더 드래그 이동·8방향 핸들
  리사이즈**(귀퉁이=가로·세로, 상/하=세로, 좌/우=가로; 위·왼쪽은 위치 x/y 도 이동. 포인터 이벤트, 마우스+터치)·
  위젯 추가/제거. 커밋 시 gridLayout(`moveItem`/`applyBox`) 이 충돌/compaction 계산.
  편집 진입 시 legacy(flow) 레이아웃은 grid 로 1회 migrate. → `PUT /console/layouts/<id>` 영속(없으면 seed).
  편집(드래그/리사이즈)은 데스크톱 전용(`useIsDesktop`), 좁은 화면은 단일열 뷰로 collapse.

영속: OAM `/api/v1/console` (`console_layouts` / `console_menu` 도메인). PUT 은 `widgets[]` 를 필드 필터
없이 통째로 저장 → placement 의 `x/y` 등 확장 필드가 그대로 보존(백엔드 무변경). 저장본 없으면 프론트 seed.

크기는 **가로·세로 모두 그리드 셀 단위**로 통일(`w`=열 span 1~48 ≈가로 2%/칸, `h`=행 span). 행 높이는
화면 세로 비율(`gridLayout.ROW_H_VH`, 기본 2%vh) — 가로(48칸)·세로(2%vh) 모두 ~2% 세밀도로 동일하다.
정수 셀(조작감)은 유지하되 한 행의 실제 크기가 vh 라 **모든 해상도에서 같은 세로 비율**로 보인다.
편집 배지는 실제 차지 비율(가로%×세로%)을 표시. legacy seed 폭은 12-칸 기준이라 migrate 시 `COL_SCALE`(×4)
환산. legacy flow 배치는 `h` 를 vh(1~100)|px(>100)로 하위호환 해석(`widgetHeightCss`). 카드 간 간격은
레이아웃 단위 `PageLayout.gap`(px, 편집 툴바 슬라이더) — 트랙 gap 이 아니라 **카드 margin**(`--card-gap`)이라
칸수와 무관하게 안전. OAM PUT 이 top-level `gap` 을 보존. `.widget-fixed` 가 패널 채움/스크롤. 코어 대시보드 위젯(`widgets/core/`):
`KpiWidget`(7 KPI 카드) · `SystemTopologyWidget`(EMS 노드 형상 + 외부 시스템 점선 노드) ·
`SystemResourceWidget`(서버×지표 추이). 데이터 정의는 `features/monitoring.md` §1.7~1.8.

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
- API: `ems/core/oam/src/handlers/service_descriptors.py` — `GET /api/v1/service-descriptors[/{id}]` · `/modules` · `/data-sources` · `PUT`(modules+alert_rules+data_sources+**shareable_apis** 보존) · `DELETE`.
- 콘솔: `/deploy/service-defs`(ServiceDescriptorsPage) — **폼 편집**(`pages/descriptors/forms.tsx`의 `ServiceForm`: id/label·모듈·alert_rules 행 추가/삭제, `DataSourceForm`: shapes 체크 + shape별 매핑 폼) + 전체 JSON "고급" fallback. 데이터 소스는 카드의 데이터 소스 섹션에서 추가/편집/삭제.
- **`shareable_apis[]`**(외부 인계용 공유 READ API): `service_registry.shareable_apis` 집계 → `handlers/api_catalog.py`
  가 `GET /api/v1/api-catalog`(목록) · `/api-catalog/openapi.json`(OpenAPI 3 산출) 로 노출. 콘솔은 관리 영역
  **연동/API** 탭(서비스팩 매니페스트 기여 → 서비스팩 있을 때만 노출). 정본: `features/api_catalog.md`.

## 6. 서비스 정규화 전략

범용 코어를 먼저 안정화하고, 서비스 종속 데이터(통계/이력 응답)는 **하나씩 정규화**해 소스로 등록한다.
백엔드 응답을 바꾸기 어려운 경우 매핑 DSL(§4)이 정규화 계층 역할을 한다. 새 서비스는
nav 매니페스트 + 위젯 + descriptor(modules/alert_rules/data_sources)만 추가하면 코어 위에 얹힌다.

## 관련
- `02_deployment.md` (Agent/Package/Deployment) · `oam_csc_split.md` (CSC↔OAM 분리)
- `features/monitoring.md` (통계/이력/모니터링 데이터)
