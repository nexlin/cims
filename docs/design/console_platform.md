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

### 3.1 분해 단위 판정 기준

"화면 하나 = 위젯 하나"는 배치를 편집할 여지가 없다. 반대로 무엇이든 쪼개면 함께 봐야 의미가 있는 것이
흩어진다. 그래서 화면의 구성 요소를 아래 세 갈래로 판정한다.

| 분류 | 기준 | 처리 |
|---|---|---|
| **A. 출력 블록** | 지표/표/차트 하나로 읽히고, 자기 데이터만 있으면 성립 | **위젯으로 분해** |
| **B. 마스터-디테일** | 선택 상태를 공유해야 성립 (목록↔상세, 히트맵↔필터) | 파라미터 버스(§3.2)로 조건을 외부화한 뒤 분해 |
| **C. 작업 트랜잭션** | 순차 마법사 · 폼+저장 원자성 (배포/검증/자동배포/설정 편집) | **분해하지 않는다** — 1위젯 유지 |

**판정 기준은 "하나만 떼어 놓아도 말이 되는가" 다.** "전부 쪼갠다"가 아니다.

| 묶어 두는 것 | 이유 | 예 |
|---|---|---|
| 같은 축의 분포 | 하나만 보면 "전체 중 얼마"인지 알 수 없다 | 알람 심각도(CRIT/MAJOR/…), 이벤트 종류(STC/AUD) |
| 같은 대상의 지표 묶음 | 서로 비교하며 읽는다 | VoLTE 요약(통화 중·호출 중·평균 통화·등록·RTP 풀), VoLTE 통계 지표(시도·성공률·평균·성공) |
| 분포를 필터로 쓰는 목록 | 타일↔목록이 한 조작 단위 | 활성 알람(심각도 타일 + 목록) |
| 한 영역에서 탭으로 오가는 상세 | 자리를 공유하는 것이 목적 | 서비스 상세(호·그룹·이벤트·부서·조회) |

| 떼어내는 것 | 이유 | 예 |
|---|---|---|
| 서로 다른 축의 독립 지표 | 하나만 봐도 말이 된다 | 가입자 / 번호 / 활성 호 / 그룹 / RTP 점유 |
| 페이지 통짜 위젯 | 조회 조건·출력이 한 덩어리라 재배치가 불가능 | `/stats/*` → 조회 조건 + 지표 + 추이 + 분포 |

부속 규칙:
- 한 카드 안의 두 값이 **비율로 읽혀야** 하면(등록/전체, 활성/전체, 사용/총) 그건 한 단위다 — 쪼개지 않는다.
- 같은 성격에 소스만 다른 출력은 위젯을 늘리지 않는다(§4) — shape 위젯 + 소스 등록으로 처리한다.
- 분해한 지표 카드는 **선언 표 하나(정본) + 팩토리**로 생성한다(지표별 컴포넌트 금지):
  `service/console/src/widgets/statCards.tsx` 의 `HEALTH_METRICS`.
- 위젯 본문에서 `.panel` 을 쓸 때 주의: `.panel` 은 **flex 컬럼 컨테이너**라 그 안에 텍스트와 `<b>`
  같은 인라인 요소를 직접 두면 각각 flex item 이 되어 줄바꿈된다 — 본문은 `<div>` 로 한 번 감싼다.
- 묶음을 실제로 없앨 때는 **전개 규칙(splits)을 남긴다** — 이미 저장된 레이아웃이 옛 id 를 참조하면
  로드 시 부품으로 펼쳐 보여준다(`widgets/legacySplit.ts`, 코어 `CORE_SPLITS` / `ServiceManifest.splits`).
  저장본을 고쳐 쓰지는 않는다 — 운영자가 편집·저장할 때 펼친 형태로 굳는다.

용어 주의 — **KPI(성능지표)와 현황값을 구분한다.** 성공률·시도수·평균 통화시간처럼 *기간에 대한
측정치*가 KPI(성능 메뉴 `/stats` 의 지표 카드)다. 가입자·번호·그룹 총수(재고, FCAPS 의 Configuration)와
등록 단말·활성 호·RTP 포트 사용률(현재 점유)은 *시점 현재값*이므로 KPI 가 아니라 **현황 지표**이고,
위젯 카테고리도 `metric`(현황 지표)에 둔다.

### 3.2 페이지 파라미터 버스 (`widgets/pageParams.tsx`)

분해하면 같은 조회 조건(기간·단위)을 여러 위젯이 함께 봐야 한다. 조건은 **레이아웃 단위**로 한 곳에 둔다.

- 컨트롤 위젯 `core.page-filter`(카테고리 `control`)가 조건을 소유(`usePageControl('period')`),
  데이터 위젯은 `usePageParam('date'|'gran')` 으로 읽는다.
- 그 페이지에 컨트롤 위젯이 **없으면** 데이터 위젯은 자기 컨트롤을 쓴다 — 배치만으로 두 형태가 성립하며
  위젯 설정을 늘리지 않는다. (`shape.*` 는 컨트롤이 있으면 자기 날짜/단위 컨트롤을 접고 값만 표기)
- **조회 대상도 같은 규칙으로 다룬다.** 한 화면에서 대상을 갈아 보는 구성(메시지 통계의 인터페이스)은
  위젯마다 dropdown 을 들지 않는다 — 차트는 SIP, 표는 CMP 를 보는 상태가 만들어지고 대상 표기도 두 곳에
  흩어지기 때문. 컨트롤 위젯 `core.source-picker` 가 파라미터 `src` 를 소유하고 `shape.*` 가 그 값을
  따른다(컨트롤이 없으면 자기 배치의 `config.source`).
  후보는 배치가 `config.sources`(소스 **id** 목록)로 열거한다 — 열거하지 않으면 그 shape 계약을 만족하는
  카탈로그 전체가 후보라 화면 목적과 무관한 소스(VoLTE/PTT 서비스 KPI)까지 섞인다.
- **URL 쿼리가 유일한 정본**(`?date=`·`?gran=`) — 딥링크/뒤로가기가 조건까지 재현한다. 버스가 소유한
  키만 건드리므로 페이지가 쓰는 `?group=`·`?agent=`·`?t=`·`?q=` 와 충돌하지 않는다.
- 새 파라미터는 `PAGE_PARAM_KEYS` 표에만 추가한다.

### 3.3 배치 단위 설정 · 표시 이름

- `WidgetDef.configFields[]` 선언 → 편집기 카드 헤더 `[⚙]` 패널이 폼을 그리고 `placement.config` 에
  저장한다. `select` 의 `options` 는 함수도 허용해 런타임 카탈로그(데이터 소스 등)를 채울 수 있다.
  같은 위젯을 다른 설정으로 여러 벌 놓을 수 있다(예: 소스가 다른 시계열 차트 2개).
- `placement.title` = 이 배치의 표시 이름(운영자 지정). 위젯이 모르는 값이라 렌더러가 캡션으로 얹는다.
- 뷰 모드에서 바꾼 조회 조건(기간·단위·대상)은 **URL 에만 남는다** — 기본값은 편집 모드에서 정한다.
  뷰의 조작이 공유 레이아웃을 조용히 바꾸지 않게 하는 경계.

### 3.5 탭 — 배치의 조건부 표시 (`placement.visibleWhen`)

한 화면을 탭으로 갈아끼우되 블록은 **각각 위젯으로 유지**하는 방법. 배치에
`visibleWhen: { param, equals }` 를 달면 렌더러가 페이지 파라미터 값과 비교해 해당 배치만 보이고,
숨겨진 자리는 `compact` 로 접혀 위로 당겨진다. **편집 모드도 뷰와 같은 배치만 보여준다**(EditableLayout 의 `EditSurface`) — 같은 자리를 갈아끼우는
위젯을 편집 화면에서 위아래로 늘어놓으면 실제와 달라지기 때문. 숨은 배치는 좌표를 보존한 채 저장에
함께 실린다. 대신 **컨트롤 위젯은 편집 중에도 조작 가능**(`.grid-widget-body--live`)이라 탭을 바꿔가며
각 탭의 배치를 편집할 수 있고, 위젯 추가도 현재 탭에서 보이는 배치 기준으로 자리를 잡는다.
카드 헤더에는 `atab=events` 배지가 붙어 어느 탭 소속인지 보인다.
탭 버튼은 그 파라미터를 쓰는 컨트롤 위젯이 그린다(카드 껍데기 없이 기존 탭 모양 그대로).

### 3.4 seed 개편 전파 (`PageLayout.seedVersion`)

저장본은 seed 를 덮으므로, seed 를 개편해도 한 번이라도 저장한 페이지는 옛 배치에 고정된다. 그래서
seed 를 개편하면 `seedVersion` 을 올린다 — 저장본의 값이 더 낮으면 admin 에게 "기본 위젯 배치가
갱신됨 [기본값 적용] [이 배치 유지]" 배너를 띄운다(자동으로 저장본을 버리지 않는다: 운영자가 만든
배치다). 저장 시 프론트가 현재 seed 세대를 저장본에 각인하고, OAM PUT 이 이 필드를 보존한다.

- `widgets/types.ts` — `WidgetDef`(id/title/category/component/defaultSize{w,h}/`configFields`) · `WidgetPlacement`(+`title`) · `PageLayout`(+`seedVersion`).
- `widgets/registry.ts` — 코어 위젯 + 서비스 `manifest.widgets` 병합(lazy — 순환 import 안전). `widgetsByCategory(query)` 로 편집 드롭다운을 카테고리(metric/infra/service/stats/view/control/event/page/etc)로 그룹 + 제목·id 검색.
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
  카드 헤더 `[⚙]` = 배치 단위 설정·표시 이름(§3.3), 툴바 검색창 = 위젯 목록 좁히기.
  **편집 화면은 실제 화면과 같아야 한다** — 카드 제목줄은 본문을 밀지 않고 위에 겹쳐 뜨고(평소 반투명,
  hover 시 또렷), 툴바의 `[👁 미리보기]` 는 제목줄·핸들·점선을 감춰 저장 후 모습 그대로를 보여준다.
  **높이 미지정(자동) 배치**는 grid 에 대응 개념이 없다 — 고정 페이지 seed(`page:<경로>`)가 그렇다.
  편집 진입 시 상수를 박으면 모든 페이지가 같은 크기(30%)로 잡히므로, `flowToGrid` 의 `measureRows`
  로 **지금 화면에 그려진 높이**를 재서 초기값으로 쓴다(카드 margin=`gap` 을 더해 행으로 환산).
  높이가 이미 있는 배치는 이 경로를 타지 않아 영향이 없다.
  **배치한 칸을 위젯이 채워야 한다** — 안 그러면 편집에서 잡은 크기와 실제 보이는 크기가 어긋난다
  (데이터가 비었을 때 특히 두드러진다). 위젯 루트별 규칙: `.panel` 하나면 그대로 / 여러 블록을 쌓으면
  `.widget-stack` 으로 감싼다 / 컨트롤(`.toolbar`·`.tab-nav`)은 칸을 채우고 내용을 세로 중앙에 둔다.
  빈 상태 안내(`.empty`)는 남은 영역 전체의 중앙에 온다 — 위젯 본문의 스크롤 영역은
  `.scroll-fill`(flex 컬럼 + overflow auto)로 통일해 두었다. 인라인 `overflow:auto` div 를 새로
  만들면 그 안의 `.empty` 가 위쪽에 붙어 카드 아래가 휑해진다. 표도 마찬가지 —
  빈 상태를 `<td className="empty">` 로 넣으면 표 안에 갇히므로, 데이터가 없으면 **표 대신**
  `.empty` 를 그린다. 새 위젯을 만들면 이 규칙을 지켰는지 확인한다
  — 전 화면을 열어 래퍼 높이와 내용 높이를 비교하면 한 번에 훑을 수 있다.
  `EditableLayout` 이 `PageParamsProvider`(§3.2)로 레이아웃을 감싸므로 파라미터 버스는 페이지 단위다.

영속: OAM `/api/v1/console` (`console_layouts` / `console_menu` 도메인). PUT 은 `widgets[]` 를 필드 필터
없이 통째로 저장 → placement 의 `x/y`·`config`·`title` 등 확장 필드가 그대로 보존된다. **top-level 은
화이트리스트**(`id`/`title`/`widgets`/`gap`/`seedVersion`) — 새 레이아웃 단위 속성을 추가하면 `console.py`
PUT 에도 함께 넣어야 유실되지 않는다. 저장본 없으면 프론트 seed.

크기는 **가로·세로 모두 그리드 셀 단위**로 통일(`w`=열 span 1~48 ≈가로 2%/칸, `h`=행 span). 행 높이는
화면 세로 비율(`gridLayout.ROW_H_VH`, 기본 2%vh) — 가로(48칸)·세로(2%vh) 모두 ~2% 세밀도로 동일하다.
정수 셀(조작감)은 유지하되 한 행의 실제 크기가 vh 라 **모든 해상도에서 같은 세로 비율**로 보인다.
seed 는 grid 좌표(x/y/w/h)로 직접 쓴다 — 폭을 48칸 단위로 지정하므로 한 줄에 7장 같은 배치도 정확하다.
편집 배지는 실제 차지 비율(가로%×세로%)을 표시. legacy seed 폭은 12-칸 기준이라 migrate 시 `COL_SCALE`(×4)
환산. legacy flow 배치는 `h` 를 vh(1~100)|px(>100)로 하위호환 해석(`widgetHeightCss`). 카드 간 간격은
레이아웃 단위 `PageLayout.gap`(px, 편집 툴바 슬라이더) — 트랙 gap 이 아니라 **카드 margin**(`--card-gap`)이라
칸수와 무관하게 안전. OAM PUT 이 top-level `gap` 을 보존. `.widget-fixed` 가 패널 채움/스크롤. 코어 위젯(`widgets/core/`): `SystemTopologyWidget`(EMS 노드 형상 +
외부 시스템 점선 노드) · `SystemResourceWidget`(서버×지표 추이) · `SystemCardsWidget` · `PageFilterWidget`
(조회 조건). 대시보드의 현황 카드는 서비스 pack 의 지표별 위젯 `cims.stat.*`(`statCards.tsx` 선언 표에서
생성, 데이터는 `stats.health` 공유 폴러 1개) — 서로 다른 축이라 낱개로 뗀 유일한 사례다. 구 묶음 위젯
`cims.kpi` 만 삭제하고 전개 규칙(splits)을 남겼다. `cims.active-alarms`·`cims.recent-events`·
`cims.svc-volte-kpi`·`cims.svc-ptt-kpi`·`cims.svc-detail`·`shape.kpi` 는 위 표의 "묶어 두는 것"이라
**그대로 유지**한다. 데이터 정의는 `features/monitoring.md` §1.7~1.8.

장애(fault) 메뉴 분해 결과 (운영자 확정):
- `/alerts/active` = `core.alarm-severity`(심각도 타일 **묶음** 1개) + `core.alarm-list`(검색·목록·상세).
  타일 클릭은 페이지 파라미터 `sev` 로 목록에 걸린다 — 타일을 낱개로 쪼개지는 않는다.
- `/alerts/catalog` = `core.alarm-catalog`(코드 사전) + `core.alarm-rules`(평가 규칙). 조회 API 가 서로 다르다.
- `/alerts/analysis` = `core.alarm-event-tabs`(탭) + `core.days-filter`(기간) + 알람 블록 5
  (요약 타일 묶음·심각도 분포·일별 발생량·코드별 표·유형별 표) + 이벤트 블록 4.
  같은 `days` 를 보는 블록이 여러 개여도 조회는 **1회**(AlarmAnalysisPage 의 공유 로더 — 캐시를
  보여주면서 뒤에서 갱신하므로 수동 새로고침 버튼이 없다).
- `/alerts/history` = 같은 컨트롤 2개(`core.alarm-event-tabs` + `core.days-filter`) +
  `core.alarm-history` / `core.event-history`. 두 이력은 **같은 좌표**를 공유한다.
- 컨트롤은 **전환 탭 / 기간 선택을 각각 위젯**으로 둔다 — 두 화면이 같은 컨트롤을 공유하고,
  필요 없는 쪽만 빼는 것도 배치로 된다.
- 탭은 위젯을 합치지 않고 **배치의 `visibleWhen`** 으로 구현한다(§3.5) — 블록은 각각 떼어낼 수 있는
  위젯으로 두고, 어느 쪽을 보일지만 파라미터가 정한다.

성능 메뉴(`/stats/*`)는 **VoLTE 통계 · PTT 통계 · 메시지 통계 · 누수 회수** 4개다.
- VoLTE/PTT 통계 = `core.page-filter` + **지표 카드(`shape.stat`) 낱개** + 추이 + 분포. 메뉴가 이미
  대상별로 갈려 있으므로 **소스 선택 UI 를 노출하지 않는다**(`config.source` 고정). 지표는 `config.item`
  (소스 kpi 계약의 0-based 인덱스)로 하나씩 고른다.
- 메시지 통계 = 인터페이스(SIP/CMP/CSC/HTTPS)를 **한 화면에서 갈아 보는** 구성이라 메뉴를 4개로
  두지 않고 `config.pickSource: true` 로 소스 선택을 노출한다.
- 차트/표 제목은 `config.title` 로 배치에서 정한다(소스가 고정된 화면에서는 소스명보다 "무엇을
  그리는가"가 읽기 쉽다 — 호 시도 추이, 종료 사유 분포).
구 통짜 위젯 `cims.service-stats`/`cims.message-stats` 는 저장본 하위호환으로만 등록해 둔다.
- 누수 회수(`/stats/leak-reclaims`)도 같은 구성 — 조회 조건(단위 버튼 없이 날짜만: `config.showGran:false`)
  + 지표 카드 4(총 회수·무RTP·RTP후 미해제·노드별) + 회수 세션 목록.
  화면의 뜻(0건이 정상)은 자리를 차지하지 않게 목록 헤더의 `ⓘ`(hover=툴팁, 클릭=펼침)로 접어 둔다.
서비스 메뉴(`/service/*`)와 구성 메뉴의 워크벤치(조직·사용자·PTT 그룹·MCPTT 정책)는 **분해하지 않는다**
— 현황→이력→상세, 목록→편집으로 이어지는 흐름 화면이다.

서비스 정의(`/deploy/service-defs`)는 `core.service-picker`(드롭다운 + 서비스 추가, 파라미터 `svc` 소유)
+ 서비스 헤더(이름·JSON·삭제) + **모듈 / 알람 규칙 / 데이터 소스** 3개 위젯이다.
- 세 컬렉션은 모두 선택된 서비스에 종속이라, 서비스마다 카드를 반복하지 않고 **하나를 골라 그 서비스의
  3종을 본다**로 바꿔 위젯으로 뗐다(`svc` 파라미터).
- 편집은 **항목 단위 인라인 CRUD 로 통일** — 각 위젯 헤더에 `[편집]`(토글) + `[＋ 추가]` 가 있고,
  토글을 켰을 때만 행에 `[수정] [삭제]` 가 나타난다(늘 떠 있으면 표가 산만해진다).
  예전에는 데이터 소스만 인라인이고 모듈·알람 규칙은 서비스 편집 모달 안에 있어 조작 방법이 갈렸다.
  `ServiceForm` 은 id/label 만 다루고(신규 추가), 항목 폼은 `ModuleForm`/`AlertRuleForm`/`DataSourceForm`.
  저장은 각 폼이 "현재 문서를 읽어 해당 배열만 갈아끼워 PUT" 한다(백엔드가 문서 전체를 받으므로).

여러 위젯이 같은 응답을 보는 화면은 조회 조건을 키로 삼는 공유 로더(`widgets/sharedFetch.ts` 의
`makeSharedByKey`)를 쓴다 — 블록이 몇 개든 요청은 1회, 캐시를 보여주며 뒤에서 갱신한다.

## 4. shape 위젯 + 데이터 소스 (완전 데이터 구동)

**데이터 성격(shape)이 같고 소스만 다른 출력**(차트/표/KPI/분포)을 위젯마다 만들지 않는다.
코어가 **shape**(presentation)를, 서비스가 **데이터 소스**(데이터)를 제공 → shape 위젯이 소스를 선택.

- shape: `time-bar` · `stat` · `distribution` · `table` — 코어 위젯 `shape.*`. `kpi` 는 **데이터 계약**
  (descriptor 가 선언하는 지표 목록)으로만 존재하고, 화면에 놓는 것은 그중 하나를 그리는 `stat`
  (지표 카드, category `metric`)이다. 소스가 선언한 지표 라벨은 `DataSource.kpiItems` 로 노출돼
  편집기 `[⚙]` 의 지표 선택지가 된다.
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
- API: `ems/core/oam/src/handlers/service_descriptors.py` — `GET /api/v1/service-descriptors[/{id}]` · `/modules` · `/data-sources` · `PUT`(modules+alert_rules+data_sources 보존) · `DELETE`.
- 콘솔: `/deploy/service-defs`(ServiceDescriptorsPage) — **폼 편집**(`pages/descriptors/forms.tsx`의 `ServiceForm`: id/label·모듈·alert_rules 행 추가/삭제, `DataSourceForm`: shapes 체크 + shape별 매핑 폼) + 전체 JSON "고급" fallback. 데이터 소스는 카드의 데이터 소스 섹션에서 추가/편집/삭제.
> API 문서(개발자 모드 `[API]` 버튼)는 descriptor 가 아니라 **각 API 를 구현한 모듈의 코드**가 소유한다.
> 정본: [features/api_docs.md](features/api_docs.md).

## 6. 서비스 정규화 전략

범용 코어를 먼저 안정화하고, 서비스 종속 데이터(통계/이력 응답)는 **하나씩 정규화**해 소스로 등록한다.
백엔드 응답을 바꾸기 어려운 경우 매핑 DSL(§4)이 정규화 계층 역할을 한다. 새 서비스는
nav 매니페스트 + 위젯 + descriptor(modules/alert_rules/data_sources)만 추가하면 코어 위에 얹힌다.

## 관련
- `02_deployment.md` (Agent/Package/Deployment) · `oam_csc_split.md` (CSC↔OAM 분리)
- `features/monitoring.md` (통계/이력/모니터링 데이터)
