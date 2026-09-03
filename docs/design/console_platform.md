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

### 3.0 캔버스 — 화면 한 장 (1920×1080 기준)

**관제 화면이라 스크롤이 곧 결함이다.** 알람이 화면 밖으로 밀려나면 안 보이고, 벽면 표시기는
아무도 스크롤하지 않는다. 그래서 배치 캔버스는 "내용만큼 자라는 영역"이 아니라 **화면 한 장**이다.

- **설계 캔버스** = 1920×1080 에서 셸 크롬을 뺀 박스 = `1648 × 968px`
  (`index.css` 의 `--canvas-w/-h` — `--design-w/h`, `--header-h`, `--sidebar-w-full`, `--main-pad` 에서 계산).
- 그 박스를 **48열 × 48행**(`gridLayout.GRID_COLS`/`GRID_ROWS`)으로 나눈다 → 셀 ≈ `34.3 × 20.2px`.
  열도 행도 `1fr` 이라 캔버스가 콘텐츠 영역을 정확히 채운다.
- **세로 48행이 곧 예산이다.** 어떤 배치도 `y+h > 48` 이 될 수 없다 — seed 도, 편집 조작도.

창 크기가 달라질 때의 규율(리플로 없음):

| 창 | 동작 |
|---|---|
| = 1920×1080 | 정확히 한 화면, 스크롤 없음 |
| > 설계 크기 | 캔버스가 남는 공간을 채운다. 열·행이 `1fr` 이라 **배치 비율은 그대로** |
| < 설계 크기 | **재배열하지 않고 그대로** 두고 스크롤이 생긴다 (`min-width/min-height` 하한) |

운영자가 위치를 기억하고 쓰는 화면이라 브레이크포인트 리플로를 두지 않는다 — 예전의
`@media (max-width: 900px)` 단일열 collapse 는 그래서 제거했다. 애매하게 작은 화면(1600×900 등)은
**브라우저 줌이 fit 기능** 역할을 한다(관제 PC 는 줌 100% 전제).

**예산이 유한하므로 "키우면 누군가 줄어든다".** 그 대상을 운영자가 정하는 장치가 `locked` 다.

- 카드 헤더 `[🔒/🔓]` → `WidgetPlacement.locked`. 잠기면 **위치·크기 고정** — 밀리지도, 줄어들지도
  않고(`compact` 가 못 미는 장애물), 드래그·리사이즈 핸들도 사라진다.
- 위젯을 키우면 `gridLayout` 이 **가장 아래까지 내려간 위젯의 열 띠** 안에서 잠기지 않은 위젯을
  아래쪽부터 최소 높이까지 줄여 자리를 만든다. 엉뚱한 곳의 위젯이 갑자기 작아지지 않게 같은 띠로
  한정한다.
- 최소 높이 = `WidgetDef.minSize.h` (미지정 시 `MIN_ROWS`). 여기까지만 줄어든다.
- 그래도 모자라면 **조작을 거절**한다(원래 배치를 그대로 돌려준다) — 조용히 넘치지 않는다.
  위젯 추가도 같은 규칙이라, 자리가 없으면 추가되지 않고 알린다.
- 편집 툴바에 `남은 세로 N/48행` 을 띄운다 — "꽉 채워야 한다"가 눈에 보이게.

**이동은 크기를 바꾸지 않는다.** 자리를 옮겼을 뿐인데 옆 카드가 작아지면 조작과 결과가 어긋난다.
그래서 크기 조절(리사이즈)만 남의 자리를 빌리고, 이동은 다음 순서로 처리한다.

1. 빈 자리로 들어가면 그냥 이동한다.
2. **다른 카드 위에 놓으면 둘이 자리를 맞바꾼다**(`trySwap`) — 크기는 서로 그대로 두고 x/y 만
   교환하며, 폭이 달라 캔버스를 넘칠 땐 안쪽으로 밀어 넣는다. 잠긴 카드와는 교환하지 않는다.
   캔버스가 꽉 찬 관제 화면에서는 "빈 자리로 옮기기"가 대부분 불가능하므로 이 교환이 기본 동작이다.
3. 둘 다 안 되면 **이동을 거절**한다(원래 배치를 그대로 돌려준다).

**되돌리기는 초기화와 다르다** — `[↶ 되돌리기]` 는 마지막 한 수만 취소한다(최근 50단계).
배치를 바꾸는 모든 조작이 draft 를 거치므로 화면 배치와 카드 안 배치가 스택 하나로 함께 덮인다.

**내용도 칸을 따라가야 한다.** 차트 높이를 px 로 박아 두면 카드를 키워도 여백만 생기고 줄이면
잘린다. 그래서 시계열/계열 차트는 막대 높이를 플롯 영역 대비 **비율(%)** 로 그리고, 플롯 영역이
`flex:1` 로 남은 높이를 전부 가져간다(`shapes/renderers.tsx`). 표처럼 본질적으로 한 화면에 안
들어가는 것은 위젯 안에서 스크롤 + 페이저로 넘긴다.

**고정 페이지 라우트도 같은 규율을 따른다.** `component:` 라우트는 `App.tsx` 가
`page:<경로>` 위젯 하나로 감싸는데, 그 seed 도 **캔버스를 통째로 차지하는 grid 배치**
(`x:0,y:0,w:48,h:48`)다 — 예전엔 legacy flow(`w:12`, 높이 미지정)라 페이지가 내용만큼 자라
브라우저가 스크롤됐다. 본문은 `.page-scroll`(칸을 채우고 넘치면 그 안에서 스크롤)로 감싼다.
그래서 페이지 컴포넌트는 **`height: calc(100vh - N)` 같은 뷰포트 산술을 쓰지 않는다** — 셸 크롬
높이를 페이지가 다시 계산하는 셈이라 캔버스 크기가 바뀌면 어긋난다. `flex: 1; min-height: 0` 으로
받은 칸을 채우면 된다.

> 전수 확인: 29개 라우트 전부 1920×1080 에서 캔버스 `1648×968` · 페이지 스크롤 0
> (`~/.cims-scratch/probe-all.mjs`).

#### 3.0.1 카드 안 배치 — 같은 좌표계, 같은 편집기

여러 블록을 담은 카드(성능 통계, 서비스 정의)는 **카드 안도 48×48 셀 그리드**다. 별도 좌표계를
두지 않은 덕에 한 벌의 규칙이 두 층에 그대로 적용된다.

| | 화면(캔버스) | 카드 안 |
|---|---|---|
| 좌표 | `WidgetPlacement{x,y,w,h}` | 같음 |
| 기본 배치 | `PageLayout`(seed) | `WidgetDef.cardLayout` |
| 운영자 저장본 | `/console/layouts/<id>` | `placement.config.layout` |
| 렌더 | `GridRenderer` → `.grid-canvas` | `CardLayout` → `.card-canvas`(min-size 없음) |
| 편집 | `GridEditor` | **같은 `GridEditor`** |
| 예산·잠금·최소 크기 | §3.0 | 같음 |

편집 진입은 화면 편집 모드에서 카드 헤더의 `[⚙]` **한 번**. 카드(= `cardLayout` 을 가진 위젯)의
`⚙` 는 설정 패널을 열지 않고 **바로 카드 안 편집으로 들어간다** — 진입에 중간 단계를 두지 않는다.
그 밖의 위젯에서는 `⚙` 가 배치 설정 패널을 연다(소스·지표·제목 등). 버튼을 따로 늘리지 않으려고
진입을 `⚙` 에 얹었으므로, **카드 위젯은 `configFields` 를 선언하지 않는다**(선언하면 가려진다). **그 카드는 자기 자리에 그대로 있고 본문만 중첩 편집기가 된다** — 표면을 갈아끼우면 "지금 어디를, 어디까지 고칠 수 있는지"가 사라진다.
편집 가능한 영역이 곧 그 카드의 박스라는 걸 화면이 그대로 말해 주도록:

- 편집 중인 카드만 또렷하게(실선 강조 테두리 + 옅은 배경), **나머지 카드는 흐려지고 조작이 잠긴다**
  (`.grid-widget--inside` / `--dimmed`). 바깥 배치 드래그·리사이즈도 함께 잠긴다 — 한 번에 한 층만.
- 그 카드의 제목줄만 **겹치지 않고 자기 줄을 차지**한다(평소엔 오버레이). 안쪽 블록의 제목줄과
  겹치면 아래 버튼을 누를 수 없기 때문이며, 편집 중에만 생기는 도구 띠라 뷰에는 영향이 없다.
- 들어간 자리에서 끝낸다 — 카드 헤더에 `[↶ 되돌리기] [초기화] [저장] [취소] [완료]`.
  상단 툴바와 같은 묶음이라 "여기서 시작해 여기서 끝난다"가 성립한다.
  버튼의 **범위는 전부 그 카드**다 — `초기화`=이 카드 기본 배치로 · `취소`=**이 카드에서 한 편집만**
  진입 시점으로 되돌리고 화면 배치로 복귀(전역 `[취소]`의 "편집 전체 버리기"와 다르다) ·
  `완료`=변경을 두고 화면 배치로 복귀 · `저장`=레이아웃 저장 후 편집 종료(이것만 전역).
  상단 툴바는 `카드 안 편집: <이름>` 만 알린다.
- `+ 위젯 추가` 와 `남은 세로 N/48행` 은 **그 카드 기준**으로 동작한다 — 카드 안에 다른 위젯을
  얹는 것까지 같은 조작으로 된다.
- 카드 안에서도 **탭(`visibleWhen`)이 그대로 동작**한다(§3.5) — 알람/이벤트처럼 같은 자리를
  갈아끼우는 블록은 카드 안에서도 조건부 표시로 서술한다. 편집 중에는 전부 보인다.

카드 안 배치를 가진 화면: 성능 통계 3개(`cims.stats.*`) · 누수 회수 · 알람·이벤트 이력 ·
유형별 분석 · 서비스 정의 · 내 대시보드 구성 · 비정상 세션 이력. 블록은 모두 레지스트리에 남아 있어 다른 화면에 낱개로 얹을 수도 있다.

블록들이 **같은 편집 초안을 공유해야 하는** 화면(내 대시보드 구성: 상태·프로파일·위젯 목록이 한
초안을 다룬다)은 그 초안을 **모듈 store 로 끌어올린다**(`pages/myLayoutStore.ts`). 컴포넌트 하나가
상태를 쥐고 있으면 블록으로 나눌 수 없기 때문이며, 조회만 공유하면 되는 화면은 `makeSharedByKey`
로 충분하다.

저장은 바깥 레이아웃 저장에 함께 실린다(`config.layout` 은 `widgets[]` 안이라 OAM PUT 이 통째 보존).

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
| 같은 대상의 지표 묶음 | 서로 비교하며 읽는다 | VoLTE 요약(통화 중·호출 중·평균 통화·등록·RTP 풀) |
| 조회 조건 + 그 조건으로 읽는 출력 | 조건과 출력이 한 조작 단위 — 조건만 떼면 무엇을 거는지, 출력만 떼면 무엇을 보는지 알 수 없다 | 알람·이벤트 이력(탭+기간+표), 유형별 분석(탭+기간+블록), 성능 통계(구간+지표+추이+분포+표) |
| 한 영역에서 탭으로 오가는 상세 | 자리를 공유하는 것이 목적 | 서비스 상세(호·그룹·이벤트·부서·조회) |

| 떼어내는 것 | 이유 | 예 |
|---|---|---|
| 서로 다른 축의 독립 지표 | 하나만 봐도 말이 된다 | 가입자 / 번호 / 활성 호 / 그룹 / RTP 점유, 비정상 세션(탐지 수 / 치명 / 스캐너 종류 / 발신 IP 수), 유형별 분석 요약 타일(발생·해소 건수 / 미해소 조합 수 / 평균 지속 시간, 총 통지·상태변화·감사 건수 / 유형 종류 수) |
| 같은 축의 분포 낱개 | 어느 심각도를 화면에 둘지가 운영자마다 다르다 — 낱개로 두고 배치로 고른다 | 활성 알람 심각도 타일(Critical/Major/Minor/Warning/Indeterminate) |

부속 규칙:
- 한 카드 안의 두 값이 **비율로 읽혀야** 하면(등록/전체, 활성/전체, 사용/총) 그건 한 단위다 — 쪼개지 않는다.
- 판정은 **축이 같은가**(한 축의 분포인가)이지 *대상이 같은가*가 아니다. 같은 화면·같은 기간의
  지표라도 세는 축이 다르면(건수 / 종류 수 / 개수) 떼어낸다.
- 같은 성격에 소스만 다른 출력은 위젯을 늘리지 않는다(§4) — shape 위젯 + 소스 등록으로 처리한다.
- 분해한 지표 카드는 **선언 표 하나(정본) + 팩토리**로 생성한다(지표별 컴포넌트 금지):
  `service/console/src/widgets/statCards.tsx` 의 `HEALTH_METRICS`, 활성 알람 심각도 타일은
  `utils/alarmLabels.ts` 의 `SEVERITY_ORDER`/`SEVERITY_LABEL`, 비정상 세션 지표는 `ABN_METRICS`,
  유형별 분석 요약 타일은 `pages/AlarmAnalysisPage.tsx` 의 `ALARM_TOTAL_TILES`/`EVENT_TOTAL_TILES`.
- 값 하나짜리 지표 위젯은 **칸을 채우고 값은 세로 중앙**이다(지표 카드 공통 표시 규칙).
- **화면 하나가 통째로 한 벌이면 카드 하나로 두되, 카드 안 구성은 선언으로 남긴다** —
  `WidgetDef.cardLayout`(`widgets/CardLayout.tsx`). 좌표계는 **바깥 캔버스와 같은 48×48 셀**이라
  렌더·편집·잠금·예산 규칙을 한 벌로 쓴다(§3.0.1). 블록은 레지스트리의 **같은 위젯**을 id 로 꺼내
  쓰므로 낱개로 배치했을 때와 동작·설정이 같다(파라미터 버스 공유). `[API]` 배지는 카드가
  `apiSources: cfg => cardSources(...)` 로 대신 낸다.
- **블록 칸(`.card-block`)은 세로 스택이다** — `.widget-fixed` 와 같은 규칙. 루트가 여러 조각인
  위젯(툴바 + 표)을 row 로 두면 조각들이 가로로 반씩 나눠 갖는다(실제로 알람 이력에서 그랬다).
- 위젯 본문에서 `.panel` 을 쓸 때 주의: `.panel` 은 **flex 컬럼 컨테이너**라 그 안에 텍스트와 `<b>`
  같은 인라인 요소를 직접 두면 각각 flex item 이 되어 줄바꿈된다 — 본문은 `<div>` 로 한 번 감싼다.
- 분해 단위를 바꿀 때는 **전이 규칙을 남긴다** — 이미 저장된 레이아웃이 옛 id 를 참조하면 로드 시
  갈아끼워 보여준다(`widgets/legacyLayout.ts`). 두 방향이 있다: 묶음 1개→부품 N개는 `CORE_SPLITS` /
  `ServiceManifest.splits`(부품이 원래 상자를 나눠 담는다), 부품 N개→위젯 1개는 `CORE_MERGES` /
  `ServiceManifest.merges`(부품들이 차지하던 합집합 상자에 하나가 들어앉고 탭 조건은 떨어진다).
  저장본을 고쳐 쓰지는 않는다 — 운영자가 편집·저장할 때 그 형태로 굳는다.
  단 **카드로 올리는 합치기(CardLayout)에는 id 규칙을 쓰지 않는다** — 카드가 부품을 id 로 찾아 그리므로
  그 id 는 그대로 살아 있어야 하고(범용 위젯이면 다른 화면도 같은 id 를 쓴다), 규칙을 걸면 운영자가
  일부러 낱개로 놓은 배치까지 카드로 바뀐다. 그 경우 전이 경로는 `seedVersion` 상승(§3.4)이다.

용어 주의 — **KPI(성능지표)와 현황값을 구분한다.** 성공률·시도수·평균 통화시간처럼 *기간에 대한
측정치*가 KPI(성능 메뉴 `/stats` 의 지표 카드)다. 가입자·번호·그룹 총수(재고, FCAPS 의 Configuration)와
등록 단말·활성 호·RTP 포트 사용률(현재 점유)은 *시점 현재값*이므로 KPI 가 아니라 **현황 지표**이고,
위젯 카테고리도 `metric`(현황 지표)에 둔다.

### 3.2 페이지 파라미터 버스 (`widgets/pageParams.tsx`)

분해하면 같은 조회 조건(기간·단위)을 여러 위젯이 함께 봐야 한다. 조건은 **레이아웃 단위**로 한 곳에 둔다.

- 컨트롤 위젯 `core.page-filter`(카테고리 `control`)가 조건을 소유(`usePageControl('period')`),
  데이터 위젯은 `usePageParam('from'|'to'|'gran')` 으로 읽는다.
- **조회 기간은 구간(`from`~`to`)으로 정한다.** 기준일 하나(`date`)+단위 조합은 "8/28 에 '월' 단위"
  처럼 무엇을 보는지가 모호했다. 먼저 구간을 정하고 그 안을 `gran` 단위로 쪼갠다.
  단위는 `5분/10분/1시간/일/월/년` 6종이며, 단위마다 **최대 조회 범위**가 있다
  (`GRAN_MAX_DAYS` — 5분 3일 / 10분 7일 / 1시간 30일 / 일 730일). 버킷이 800개 근처를 넘으면
  차트가 읽히지 않고 스캔 비용만 늘기 때문이며, 상한은 그 근방을 사람이 읽기 좋은 값으로 반올림했다.
  범위를 넘는 단위 버튼은 비활성으로 보이고, 구간을 넓히면 `bestGran` 이 맞는 단위로 올려준다.
  서버도 같은 표(`stats.py::_GRAN_MAX_DAYS`)로 **끝에서부터** 잘라내고 `truncated` 로 알린다.
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
- **표시 선택도 파라미터로 둔다.** 계열 차트에서 무엇을 그릴지는 `series`(계열 키 쉼표 목록)를
  `core.series-select` 가 소유하고 `shape.series-bar` 가 읽는다. 비어 있으면 전 계열이라,
  링크에 아무것도 안 붙은 상태가 곧 기본 화면이다. 계열은 이미 받은 응답 안에 다 들어 있으므로
  **켜고 끌 때 다시 조회하지 않는다** — 표시 문제와 조회 문제를 섞지 않는다.
- 구간 기본값(URL 에 `from`/`to` 가 없을 때)은 **마운트 때 한 번** 정하고 그대로 쓴다. 매번
  `지금`을 다시 계산하면 계열 토글처럼 표시만 바꾸는 조작이 조회 창까지 움직여, 같은 화면의
  지표 타일과 차트가 서로 다른 끝시각으로 조회하는 일이 생긴다. 창을 지금으로 당기는 건
  `[오늘]`·`[↺]` 가 명시적으로 한다(URL 에 값을 쓴다).
- **화면의 뜻은 `ⓘ` 로 접는다**(`components/InfoDot`). "한 번 읽으면 되는 설명"을 상자로 깔면
  매번 읽지 않는데도 계속 세로를 먹고, 한 화면 예산(§3.0)에서는 그만큼 표가 줄어든다.
  **접지 않는 것**: 지금 조치가 필요한 신호(⚠ 경고), 조건에 따라 달라지는 상태 표기, 빈 화면 안내.
  적용한 곳: 누수 회수 · 비정상 세션 이력 · 내 대시보드 구성 · MCPTT 정책 · 모듈 운영 명세 ·
  멤버별 패키지 배포 현황 · Agent→OAM 보고 주소.
- 화면이 보내는 시각은 `YYYY-MM-DD HH:MM`(초 없음)이다. 서버는 입구에서 `_norm_dt` 로 한 번만
  초까지 채운다 — 파싱하는 자리마다 방어하면 자리가 늘 때 또 빠진다.
- 새 파라미터는 `PAGE_PARAM_KEYS` 표에만 추가한다.

### 3.3 배치 단위 설정 · 표시 이름

- `WidgetDef.configFields[]` 선언 → 편집기 카드 헤더 `[⚙]` 패널이 폼을 그리고 `placement.config` 에
  저장한다. `select` 의 `options` 는 함수도 허용해 런타임 카탈로그(데이터 소스 등)를 채울 수 있다.
  같은 위젯을 다른 설정으로 여러 벌 놓을 수 있다(예: 소스가 다른 시계열 차트 2개).
- `[⚙]` 는 **할 수 있는 일이 있을 때만** 눌린다(배치 설정 항목 또는 카드 안 편집). 빈 패널만 여는
  톱니바퀴는 두지 않는다.
- **표시 이름 변경은 두지 않는다.** 관제 화면에서 위젯 이름을 바꾸면 그게 원래 무엇인지 가려져
  위험하다(`identifier_model.md` — 동작은 불변 id 로, 표시는 정의된 이름으로).
  `placement.title` 은 옛 저장본 호환으로만 남아 렌더러가 캡션으로 그린다.
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
seed 가 쓰는 곳은 지금 없다 — 장애 화면의 알람/이벤트(`atab`)는 위젯 하나가 안에서 갈아끼우는 쪽으로
바뀌었다. 저장된 레이아웃과 운영자가 만드는 배치를 위해 기능은 그대로 둔다.

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
**세로는 48행이 전부**(§3.0)라 seed 도 그 안에 들어가야 한다 — 모자라면 세로로 쌓지 말고 가로를 쓴다.
대시보드가 그 예다: 예전엔 거의 전부 전폭(48칸) 스택이라 172vh 로 자라 페이지가 스크롤됐는데,
위젯 구성은 그대로 두고 2~3열로 접어 48행에 맞췄다(알람·이벤트 / 현황 카드 7 / 형상·리소스 /
활성 VoIP·PTT·호 추이).
편집 배지는 실제 차지 비율(가로%×세로%)을 표시. legacy seed 폭은 12-칸 기준이라 migrate 시 `COL_SCALE`(×4)
환산. legacy flow 배치는 `h` 를 vh(1~100)|px(>100)로 하위호환 해석(`widgetHeightCss`). 카드 간 간격은
레이아웃 단위 `PageLayout.gap`(px, 편집 툴바 슬라이더) — 트랙 gap 이 아니라 **카드 margin**(`--card-gap`)이라
칸수와 무관하게 안전. OAM PUT 이 top-level `gap` 을 보존. `.widget-fixed` 가 패널 채움/스크롤. 코어 위젯(`widgets/core/`): `SystemTopologyWidget`(EMS 노드 형상 +
외부 시스템 점선 노드) · `SystemResourceWidget`(서버×지표 추이) · `SystemCardsWidget` · `PageFilterWidget`
(조회 조건). 대시보드의 현황 카드는 서비스 pack 의 지표별 위젯 `cims.stat.*`(`statCards.tsx` 선언 표에서
생성, 데이터는 `stats.health` 공유 폴러 1개) — 서로 다른 축이라 낱개로 뗐다. 구 묶음 위젯
`cims.kpi` 만 삭제하고 전개 규칙(splits)을 남겼다. 대시보드의 `cims.active-alarms`·`cims.recent-events`·
`cims.svc-volte-kpi`·`cims.svc-ptt-kpi`·`cims.svc-detail`·`shape.kpi` 는 **그대로 유지**한다 — 대시보드는
한눈에 훑는 요약 화면이라 알람 타일+목록이 한 카드로 붙어 있어야 하고, 낱개로 보는 건 장애 메뉴가 한다. 데이터 정의는 `features/monitoring.md` §1.7~1.8.

장애(fault) 메뉴 분해 결과 (운영자 확정):
- `/alerts/active` = 심각도 타일 **5장이 각각 위젯**(`core.alarm-severity.<심각도>`) + `core.alarm-list`
  (검색·목록·상세). 타일 클릭은 페이지 파라미터 `sev` 로 목록에 걸리므로, 타일을 몇 장 놓든 순서를
  어떻게 바꾸든 필터는 그대로 동작한다. 타일은 심각도별 컴포넌트가 아니라 `SEVERITY_ORDER` 선언
  표 + 팩토리로 만든다(`widgets/core/faultWidgets.tsx`).
- `/alerts/catalog` = `core.alarm-catalog`(코드 사전) + `core.alarm-rules`(평가 규칙). 조회 API 가 서로 다르다.
- `/alerts/history` = `core.alarm-event-history` **하나**. 전환 탭 · 기간 선택 · 이력 표는 함께 조작하는
  한 벌(고른 탭과 기간이 곧 표의 의미)이라 카드 안에 둔다. 카드 안은 **전환 탭 한 줄**(전폭) +
  **조회 조건 한 줄**(기간과 그 탭의 필터를 **한 블록**에 담는다 — 따로 두면 조회 조건이 두
  덩어리로 갈려 보인다) + **표가 나머지 전부**다. 필터와 표는
  같은 필터 상태를 봐야 하므로 `pages/alertsHistoryStore.ts` 로 끌어올렸고, 조회 자체는 `days` 를
  키로 공유한다(블록이 몇 개든 요청 1회).
- `/alerts/analysis` = `core.alarm-event-analysis` **하나**. 알람 블록(요약 타일 4장·심각도 분포·일별
  발생량·코드별 표·유형별 표)과 이벤트 블록(요약 타일 4장·일별 통지량·유형별 표·소스별 표)은 같은
  기간 창을 여러 각도에서 보는 한 벌이라 함께 둔다. **요약 타일은 활성 알람 심각도 타일과 같이
  1장 = 위젯 1개**다(`core.{alarm,event}-analysis.totals.<키>` — 세는 축이 건수·종류 수·시간으로
  서로 달라 한 장만 놓아도 말이 된다). 이벤트 상한(최신 5000건) 도달 표기는 절단된 수치인
  '총 통지' 타일에만 붙는다.
  같은 `days` 를 보는 블록이 여러 개여도 조회는 **1회**(AlarmAnalysisPage 의 공유 로더 — 캐시를
  보여주면서 뒤에서 갱신하므로 수동 새로고침 버튼이 없다).
- 두 화면 모두 조건은 그대로 페이지 파라미터(`atab`/`days`)다 — 딥링크가 화면을 재현하고, 컨트롤
  위젯(`core.alarm-event-tabs` / `core.days-filter`)을 따로 얹는 배치도 성립한다. 그 경우 위젯은
  `useHasPageControl` 로 자기 안의 같은 컨트롤을 접는다(§3.2).

성능 메뉴(`/stats/*`)는 **VoLTE 통계 · PTT 통계 · 인터페이스 통계 · 누수 회수** 4개다.
세 질문이 각각 한 자리를 갖고, 그 밖의 메뉴는 두지 않는다.

| 질문 | 보는 곳 |
|---|---|
| 호가 얼마나·잘 되나 | VoLTE 통계 / PTT 통계 — 호 단위(call.json) 집계라 성공률·통화시간·종료사유까지 |
| 어떤 메시지가 얼마나 오가나 | 인터페이스 통계 — 메시지 로그 집계, 메서드별·서비스별 |

메시지 로그의 INVITE 를 세어 "호 시도"를 따로 만들지 않는다 — 재전송·re-INVITE 가 섞여 호 단위
집계와 숫자가 어긋난다. 호는 호 통계가, 메시지는 메시지 통계가 센다.

네 화면 모두 **화면 하나 = 카드 하나**다. 조회 구간·지표·추이·분포·표는 같은 구간을 함께 읽는 한
벌이라 하나만 떼면 화면이 성립하지 않는다(무엇을 언제 기준으로 본 값인지 사라진다). VoLTE·PTT·
인터페이스 카드 안의 블록 구성은 `service/console/src/widgets/statsScreens.tsx` 가, 누수 회수는
`widgets/leakReclaimWidgets.tsx` 가 정본이고, 블록 자체는
코어 위젯(`core.page-filter`·`core.source-picker`·`core.series-select`·`shape.*`)을 id 로 그대로 쓴다
(§3.1 부속 규칙 — `widgets/CardLayout.tsx`). 저장본이 있는 화면은 **`seedVersion` 상승 안내**로
새 배치를 받는다 — 부품이 범용 위젯이라 id 전이 규칙을 쓸 수 없다.

- VoLTE/PTT 통계 카드 = 조회 조건 + **지표 카드(`shape.stat`) 낱개** + 추이 + 분포.
  메뉴가 이미 대상별로 갈려 있으므로 **소스 선택 UI 를 노출하지 않는다**(`config.source` 고정).
  지표는 `config.item`(소스 kpi 계약의 0-based 인덱스)로 하나씩 고른다.
  메시지는 여기 두지 않는다 — 메서드 종류(INVITE·REGISTER·…)는 인터페이스 통계가 이미 그린다.
- 인터페이스 통계 카드 = 인터페이스를 **한 화면에서 갈아 보는** 구성이라 메뉴를 4개로 두지 않고
  `core.source-picker`(파라미터 `src`)로 대상을 고른다 — 차트·분포·표가 함께 따라간다.
  **서비스축(VoLTE/PTT)은 메뉴가 아니라 계열이며, SIP 에만 있다** — CMP/CSC 는 제어 메시지(JSON),
  HTTPS 는 관리 트래픽이라 서비스 구분이 없다. 그 셋은 계열을 하나(`전체`)만 선언해 같은 위젯으로
  그리되 없는 구분을 지어내지 않는다(계열이 하나면 선택 타일도 합계 하나만 낸다). `core.series-select` 가 계열 카드(색 + 이름 +
  구간 합계)를 그리고 파라미터 `series` 를 소유하며, 시계열(`shape.series-bar`)과 메서드 비중
  (`shape.distribution`)이 **함께** 그 선택을 따른다 — 한 화면의 두 그림이 다른 대상을 보지 않게.
  계열은 `VoLTE / PTT / 미분류` 로 서로 겹치지 않아 전부 켠 막대가 곧 전체다. 그래서 '전체 메시지'
  타일은 계열이 아니라 **전부 선택 버튼**이다(`config.allLabel` 로 이름 지정).
  구간 내내 0 인 계열은 카드에서 숨긴다 — '미분류'처럼 정상일 때 비어 있는 계열이 자리만 먹지 않게.
- 차트/표 제목은 `config.title` 로 블록 선언에서 정한다(소스가 고정된 화면에서는 소스명보다 "무엇을
  그리는가"가 읽기 쉽다 — 호 시도 추이, 종료 사유 분포).
구 통짜 위젯 `cims.service-stats`/`cims.message-stats` 는 옛 페이지 컴포넌트를 감싼 것으로, 저장본
하위호환으로만 등록해 둔다(seed 는 쓰지 않는다).
- 누수 회수(`/stats/leak-reclaims`) 카드 = 조회 조건(날짜) + **지표 카드 4 낱개**(총 회수·무RTP·
  RTP후 미해제·노드별 — 세는 축이 서로 달라 떼어낸다) + 회수 세션 목록. 화면의 뜻(0건이 정상)은
  조회 조건 블록의 `ⓘ` 로 접는다. 화면의 뜻(0건이 정상)은 자리를 차지하지 않게 툴바의 `ⓘ`(`components/InfoDot`
  — hover=요약 title, 클릭=말풍선, 바깥클릭/Esc=닫기)로 접어 둔다.
서비스 메뉴(`/service/*`)와 구성 메뉴의 워크벤치(조직·사용자·PTT 그룹·MCPTT 정책)는 **분해하지 않는다**
— 현황→이력→상세, 목록→편집으로 이어지는 흐름 화면이다.

서비스 정의(`/deploy/service-defs`)도 **화면 하나 = 카드 하나**(`core.service-defs`)다. 카드 안은
`core.service-picker`(드롭다운 + 서비스 추가, 파라미터 `svc` 소유) + 서비스 헤더(이름·JSON·삭제) +
**모듈 / 알람 규칙 / 데이터 소스** 순으로, 구성은 `SERVICE_DEF_CARD_ROWS` 선언이 정본이다.
- 세 컬렉션은 모두 선택된 서비스에 종속이라, 서비스마다 카드를 반복하지 않고 **하나를 골라 그 서비스의
  3종을 본다**로 바꿨다(`svc` 파라미터). 선택을 떼거나 컬렉션 하나만 떼면 무엇에 대한 목록인지
  알 수 없어 카드 하나로 둔다 — 블록은 위젯으로도 등록돼 있어 다른 화면에 낱개로 얹을 수 있다.
- 편집은 **항목 단위 인라인 CRUD 로 통일** — 각 위젯 헤더에 `[편집]`(토글) + `[＋ 추가]` 가 있고,
  토글을 켰을 때만 행에 `[수정] [삭제]` 가 나타난다(늘 떠 있으면 표가 산만해진다).
  예전에는 데이터 소스만 인라인이고 모듈·알람 규칙은 서비스 편집 모달 안에 있어 조작 방법이 갈렸다.
  `ServiceForm` 은 id/label 만 다루고(신규 추가), 항목 폼은 `ModuleForm`/`AlertRuleForm`/`DataSourceForm`.
  저장은 각 폼이 "현재 문서를 읽어 해당 배열만 갈아끼워 PUT" 한다(백엔드가 문서 전체를 받으므로).

여러 위젯이 같은 응답을 보는 화면은 조회 조건을 키로 삼는 공유 로더(`widgets/sharedFetch.ts` 의
`makeSharedByKey`)를 쓴다 — 블록이 몇 개든 요청은 1회, 캐시를 보여주며 뒤에서 갱신한다.

## 3.6 색은 토큰으로만 (라이트/다크 공용)

테마는 `index.css` 의 **디자인 토큰**(`:root` / `:root[data-theme="dark"]`)으로만 갈린다.
화면 코드는 색 리터럴을 쓰지 않는다 — 인라인 스타일이든 CSS 문자열이든 `var(--…)` 로 적는다.

| 쓰임 | 토큰 |
|---|---|
| 카드·패널 바탕 | `--surface` / 눌린 바탕 `--surface-2` / 페이지 바탕 `--bg`·`--bg-soft` |
| 선 | `--border` · 마우스오버 `--hover` |
| 글자 | `--text` / 보조 `--text-muted` |
| 상태 | `--primary` `--success` `--warning` `--danger` + soft 배경 `--primary-soft` `--success-soft` `--warn-soft` `--danger-soft` |
| 차트 계열 | `--chart-1` ~ `--chart-5` — 색상(hue)이 서로 뚜렷이 다른 5색, 선언 순서대로 배정. `--chart-muted` 는 '미분류'처럼 값이 아니라 빈자리를 뜻하는 계열용(소스가 `color` 로 직접 지정) |

계열 색에는 상태색(`--success`/`--warning`/`--danger`)을 쓰지 않는다 — 계열은 "무엇인가"이지
"좋다/나쁘다"가 아니라서, 섞으면 초록 막대가 정상을 뜻하는 것처럼 읽힌다.

세 가지 함정이 실제로 다크 화면을 깨뜨렸다:

- **없는 토큰 + 라이트 폴백**(`var(--muted, #6b7280)`, `var(--bg-elevated, #fff)`) — 토큰이 없으니
  폴백이 **항상** 적용돼 다크에서 흰 배경·회색 글자가 그대로 남는다. 폴백은 안전장치가 아니라
  테마를 무력화하는 장치다. 토큰 이름을 정확히 쓰고 폴백은 적지 않는다.
  토큰이 **정의돼 있어도** 마찬가지다 — 그 폴백은 지금 죽은 코드이고, 토큰 이름이 바뀌는 날
  라이트 색을 조용히 되살린다. `var(--surface, #fff)` 같은 표기는 남기지 않는다.
- **CSS 에 없는 클래스에 기대기**(`className="card"` — `.card` 규칙이 존재하지 않음) — 배경/테두리가
  아예 안 붙어 다크에서 카드가 사라진다. 카드는 `.panel` 을 쓴다.
- **badge/tag 변형의 파스텔 배경**(`.badge--blue { background:#dbeafe }`) — 라이트용 색이 다크에서
  흰 알약으로 뜬다. soft 토큰으로 적고, 전경 대비가 모자라면 `:root[data-theme="dark"]` 오버라이드를
  짝지어 둔다.
- **배경을 아예 안 적은 폼 컨트롤** — `.btn` 이 배경을 비워 두면 **브라우저 기본 버튼 색**이 나온다.
  `:root` 의 `color-scheme` 이 다크에서 그 기본을 회색으로 바꾸므로, 얹은 글자(`--danger` 등)와
  대비가 무너진다. 버튼·입력은 배경과 글자색을 반드시 명시한다.

예외로 색 리터럴을 남기는 곳은 넷뿐이다 — ① `@media print` 블록과 **인쇄 전용 컴포넌트**
(`VerificationPrintReport` — 종이는 늘 흰색이라 토큰을 쓰면 다크 테마로 인쇄할 때 글자가 흐려진다),
② 의도적으로 어두운 코드·로그 뷰(`#0d1117` 계열, 라이트에서도 어둡게 유지), ③ 진한 배경 위에
흰 글자를 얹는 상태 배지(`color:'#fff'`), ④ 그 배지의 **배경**.

④가 특히 함정이다. 상태 토큰(`--success` `--warning` `--danger` `--primary`)은 **전경용**이라
다크에서 밝아진다 — 배경으로 쓰면 흰 글자와의 대비가 오히려 무너진다(다크 `--success`=#22c55e 위
흰 글자 = 2.3:1). 흰 글자를 받는 배경은 테마와 무관하게 **어두운 쪽으로 고정**한다
(`#15803d` 초록 4.5:1 · `#b45309` 주황 5.0:1 · `#6b7280` 회색 4.0:1).

점검: `~/.cims-scratch/audit-dark.mjs` — 다크로 전환해 **밝은 배경이 남은 요소**와 **대비 3:1 미만**
텍스트를 화면별로 집계한다. 토큰 오타는 정의 목록과 소스의 `var(--…)` 를 대조해 잡는다.

## 4. shape 위젯 + 데이터 소스 (완전 데이터 구동)

**데이터 성격(shape)이 같고 소스만 다른 출력**(차트/표/KPI/분포)을 위젯마다 만들지 않는다.
코어가 **shape**(presentation)를, 서비스가 **데이터 소스**(데이터)를 제공 → shape 위젯이 소스를 선택.

- shape: `time-bar` · `series-bar` · `stat` · `distribution` · `table` — 코어 위젯 `shape.*`.
  `kpi` 는 **데이터 계약**(descriptor 가 선언하는 지표 목록)으로만 존재하고, 화면에 놓는 것은
  그중 하나를 그리는 `stat`(지표 카드, category `metric`)이다. 소스가 선언한 지표 라벨은
  `DataSource.kpiItems` 로 노출돼 편집기 `[⚙]` 의 지표 선택지가 된다.
- `series-bar` 는 한 버킷이 값 하나가 아니라 **계열별 값**을 갖는 시계열이다. 버킷마다 막대는
  하나이고 고른 계열이 색으로 **쌓인다** — 막대 높이 = 고른 계열의 합. 계열은 map 에 선언 순서대로
  적고, 그 순서가 곧 색(`--chart-1`~`5`)이자 쌓는 순서(아래→위)이며 범례·카드 순서다.
  **순서를 바꾸면 보던 색이 바뀐다** — 계열을 추가할 때는 뒤에 붙인다.
- **계열은 겹치지 않게 쪼갠다.** 쌓기는 "부분의 합"이라 막대 높이가 곧 고른 계열의 합이고, 그래서
  *전부 켠 상태 = 전체*가 성립해야 한다. 포함관계인 값을 계열로 두면(전체 ⊃ VoLTE) 더한 값이
  실제보다 커진다. 그래서 소스가 분해값을 내고(`_svc_bucket`: `volte + ptt + unknown == count`),
  '전체' 계열은 두지 않는다 — 전부 켜면 그게 전체다.
- 분포(`distribution`)도 같은 계열로 쪼갤 수 있다: `partsObject` 가 항목별 계열 값을 가리키면
  (`method_service[메서드] = {volte,ptt,unknown}`) 막대 하나를 계열 색으로 나눠 칠하고, 계열 선택이
  걸리면 값·합계를 고른 계열 기준으로 다시 센다.
- 그래도 겹치는 계열을 선언할 여지는 남긴다: 계열에 `includes`(품고 있는 계열 키)를 적으면 렌더러가
  그 조합을 골랐을 때만 범례 아래에 한 줄로 알린다(선택을 막지는 않는다).
- 마우스를 올리면 그 버킷의 전 계열 값과 합계가 말풍선으로 뜨고, 가리킨 계열이 굵게 강조된다.
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

- 저장: file_store `services` 도메인. 시드: `ems/core/oam/src/services/service_descriptors_seed/*.json`(CIMS=`cims.json`).
- 집계: `ems/core/oam/src/services/service_registry.py` — `all_modules` / `valid_module_names` / `controllable_modules` / `alert_rules`(코어 host 규칙 disk_high/module_down 병합) / `data_sources`.
- **시드 반영은 두 단계다.** `seed_if_empty` 는 store 가 **비었을 때만** 전체 주입하므로, 이미
  운용 중인 노드에는 뒤에 추가된 것이 영원히 닿지 않는다. 그래서 기동 시 `merge_seed_updates` 가
  차이를 덧댄다. 무엇을 덮고 무엇을 보존하는지는 **그 값의 정본이 누구인가**로 갈린다:
  - 운영자 정본(덮지 않음) — 모듈 엔트리, `alert_rules`, `label`, 이미 값이 있는 `health` 키.
    없는 것만 추가/보강한다.
  - 코드 정본(seed 를 그대로 반영) — seed 가 가진 데이터 소스의 `shapes`·`map`·`endpoint`·`query`
    ·`label`. 이 값들은 "어느 필드를 어떤 축으로 읽는가"라는 렌더러와의 계약이다. 없는 것만 채우는
    정책은 양방향으로 막힌다 — 매핑을 고쳐도 옛 노드에 안 닿고, shape 를 빼도 store 에 남아 빈
    화면을 만든다. 그래서 **제거까지 전파**한다. 운영자가 직접 추가한 소스(seed 에 없는 id)는
    건드리지 않는다.
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
