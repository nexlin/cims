# 콘솔 디자인 시스템

CIMS 웹 콘솔(`ems/core/console` + `ems/service/console`)의 **시각 계약 정본**이다.
색·타이포·간격·컴포넌트 배리언트·상태 표현 규칙은 여기와 여기가 가리키는 핸드오프 패키지가
정본이고, 화면 기능·라우팅·위젯 플랫폼 구조는 [console_platform.md](console_platform.md) 가 정본이다.

## 0. 네 출처의 관계

| 출처 | 무엇의 정본인가 | 다루는 방식 |
|---|---|---|
| **Figma 원본** `3nHGkPN8XAaHQxPNGn608v` | **그림의 정본** — 구조·치수·문구·색 | Figma MCP 로 **직접 읽는다** (§6.1) |
| `cims-design-handoff/` | 그 그림의 **해설 + 우리 스택으로의 변환** — shadcn 대응표·도메인 규칙·`decisions.md` 근거 | **읽기 전용.** 손대지 않는다. v2 를 받으면 폴더째 교체 |
| **이 문서** | 우리 쪽 **적용 범위·스택 결정·충돌 판정·이행 계획** | 위 둘에 없거나 위 둘이 틀린 것을 여기서 정한다 |
| 콘솔 소스 | 현재 동작 | 문서와 어긋나면 코드가 사실, 차이는 문서 갱신으로 해소 |

Figma 와 핸드오프가 어긋나면 **구조·치수·문구·색은 Figma 가 맞다** — 핸드오프는 그림을 글로 옮긴
것이라 누락·의역이 있다(실제로 있다, §5.2). 반대로 핸드오프에만 있는 것(shadcn 컴포넌트 대응표,
도메인 규칙, 변경 근거)은 핸드오프가 정본이다.

**판정 원칙 — 이 둘이 전부다.**

1. **핸드오프가 다루면 핸드오프가 정본이다.** 색·폰트 같은 시각뿐 아니라 **화면 구조·섹션 편성·
   필드 배치·메뉴 위치도 시안을 따른다.** "기존 구현이 이러니까" 는 유지 근거가 되지 않는다.
2. **핸드오프의 침묵은 삭제 지시가 아니다.** 시안에 없는 기존 기능은 **보존한다.** 핸드오프는
   2026-09-02 웹 실측 기준이라 그 뒤 코드와 실측 당시 안 보이던 것(개발자 모드 전용 등)을 모른다.

둘이 부딪히면 §7 의 판정이 우선한다. §7 에도 없으면 **지우지 말고 사용자에게 묻는다.**

## 1. 적용 범위

디자이너가 준 것은 **시스템/인프라 화면과 공통 셸의 도안뿐**이다. 그렇다고 적용 범위가 그 화면만인
것은 아니다 — 거기 쓰인 **방식과 규칙은 콘솔 전체에 적용하고**, 그 화면에만 있는 **구체 값은 그
화면에만** 적용한다. 세 층으로 갈린다.

| 층 | 무엇 | 정본 | 적용 범위 |
|---|---|---|---|
| **① 방식** — 무엇으로 만드는가 | 하드코딩 HTML/CSS·인라인 `style` 을 걷어내고 **shadcn 컴포넌트를 임포트해 쓴다** · Tailwind 유틸리티 · 토큰 변수 · Lucide 임포트 | Figma **`02 Components`** (§4) | **전 31 라우트 + 공통 셸** |
| **② 규칙** — 어떻게 보여야 하는가 | 절대 규칙 7 · 컴포넌트 배리언트·상태 · 토큰 값(라이트/다크) · 타입 스케일 | Figma **`01 Design style sheet`** + **`02 Components`** (§3·§4·§5) | **전 31 라우트 + 공통 셸** |
| **③ 값** — 이 화면은 구체적으로 | S1 네트워크 5섹션 · 스코프별 필드 이동 · 섹션 뎁스 · 문구 · 컬럼 폭 | Figma **`03 Screens`** + `screens/*.md` | **`/deploy/servers` · 공통 셸만** |

**01·02 가 뼈대다.** 03 은 그 뼈대로 조립한 시스템/인프라 화면의 *예시*이고, 03 이 없는 나머지
30개 라우트도 **01·02 에 맞춰 다시 만든다.**

①②는 그 페이지의 그림이 없어도 적용한다. 예: `AlertsPage` 의
`<button className="btn btn--ghost btn--sm" style={{…}}>↻</button>` →
`<Button variant="ghost" size="sm"><RotateCw /></Button>`.
**어떤 버튼을 어디 놓을지(화면 구성)는 그대로 두고, 무엇으로 만드는지를 바꾼다.**

**「손대지 말 것」의 뜻** — CLAUDE.md 가 `EditableLayout`·`GridEditor`·`CardLayout` 과
위젯 격자 CSS 를 손대지 말라고 한 것은 **그 기능을 지우거나 재설계하지 말라**는 뜻이다
(시안이 위젯 편집을 다루지 않아 §7 #1~#3 에서 「현행 유지」로 판정했다). 시트 1·2 —
토큰·컴포넌트 계약·아이콘 — 는 **그 파일에도 적용된다.** 구조·동작·문구는 그대로 두고
색과 컴포넌트만 갈아끼운다. (T4-3 에서 그렇게 했다. 처음엔 이 규칙을 「파일을 아예 열지
말라」로 과하게 읽어 `.btn`·`.form-input` 을 남겨 뒀는데, 사용자가 정정했다.)

**기능은 건드리지 않는다** — 메뉴 소속·라우팅·동작·데이터. 같은 파일 안에서도 성격으로 가른다:
공통 셸의 폭·색·선택 표시·AppBar 배치는 ①②라 시안대로 가지만, **어떤 메뉴 항목이 어느 그룹에
속하는가는 서비스 구조라 그대로 둔다.**

**시안에 없으면 그냥 둔다 — 양방향이다.**
- 시안에 없는 기존 기능을 **지우지 않는다**
- 시안에 없는 것을 **개선·정리하지도 않는다.** 일관성·정돈을 이유로도 손대지 않는다

스펙 없는 화면의 레이아웃·문구를 **지어내지 않는다.** ①②만으로 답이 안 나오면 물어본다.

**시안이 구체 값을 준 구조 변경** (③층 — `/deploy/servers`·공통 셸 한정):

- **Collapsible 중첩 2단 상한** (`DESIGN-RULES.md` §2)
- **S1 네트워크 재편** — Level 1 하나 아래 Level 2 5섹션(IP/Routing · 라우팅 · 마운트 ·
  OAM 접속 주소 · 네트워크 튜닝) (`server-scope.md`)
- **스코프별 필드 이동** — 그룹 멤버면 서버 화면 4필드 + 공통 9필드는 그룹 화면, SA 면 13필드
  전부 서버 화면 (`DESIGN-RULES.md` §3)
- **서버 액션** 7개 나열 → `메트릭 · 점검 · [더보기 ▾]` (`decisions.md` §4)
- **그룹 ContextBar 를 4탭 전부에 유지** (`decisions.md` §3 — 현재는 1탭에만 있다)
- **PageHeaderRow breadcrumb 신설** — `관리 › 시스템 › 시스템/인프라` (`shell.md`). 현재 없다
- 표에서 `build`·`git` 별도 컬럼 분리 / 일괄 제어를 프로세스 제어와 HA 절체로 구분선 분리
  (`decisions.md` §5·§6)

사이드바 뎁스는 바뀌지 않는다 — 현재도 시안도 3단(영역 → 그룹 → 항목)이고 그룹 편성도 같다.

## 2. 스택 (결정)

- **Tailwind + shadcn/ui + Radix 로 전면 이행한다.** Mantine 은 폐기. 두 체계를 섞지 않는다.
- **스타일 출처는 시안 `globals.css` 하나로 간다.** 현재는 `ems/core/console/src/index.css`
  (1463줄)가 전 라우트의 유일한 출처이고 `--bg` `--surface` `--text` 같은 자체 토큰 체계를
  갖고 있다 — **이 파일은 폐기 대상**이다. 시안과 값은 대체로 같고 **이름 체계가 다르므로**
  코드 전체를 시안 이름으로 개명한다 (매핑 = §5.1, 실행 = §8 T0a). 별칭 층은 두지 않는다.
- **CSS 를 직접 쓰는 방식 자체를 접는다.** 하드코딩 스타일 대신 shadcn 컴포넌트를 임포트해 쓰는
  것이 시안이 정한 방식(§1 ①층)이므로, 인라인 `style={{ }}` 2,707곳을 Tailwind 유틸리티
  `className` 으로 옮기는 것이 이행의 본체다 (§8 T3).
- 아이콘은 **Lucide 만** (`lucide-react` 는 이미 의존성에 있다). 이모지·텍스트 글리프 금지.
- 폰트는 Pretendard Variable(본문) · JetBrains Mono(IP·경로·버전) 두 종
  (`cims-design-handoff/tokens/fonts.md` 의 타입 스케일 포함).

**빌드 쪽 파급** — 디자인 문서가 아니라 빌드 계약이라 여기 남긴다.
- Tailwind content 스캔 경로에 **`ems/service/console/src` 를 반드시 포함**한다. 서비스 팩은
  자체 `package.json`·`node_modules` 없이 core 것을 심볼릭 링크로 공유한다
  (`ems/core/console/scripts/ensure-svc-modules.mjs`). 빠뜨리면 서비스 팩(15 라우트)의
  클래스가 전부 purge 된다.
- `make dist` 콘솔 번들은 vite 산출물을 그대로 담으므로 패키징 변경은 없다.
- preflight(전역 reset) 를 켜는 시점 = 기존 `index.css` 의 reset 을 걷어내는 시점이다. 둘을
  동시에 켜두면 버튼·표 높이가 어긋난다 (§8 T0b → T4).

## 3. 절대 규칙

핸드오프 `DESIGN-RULES.md` §1 이 정본. 요약:

1. **hex 금지.** `#4f46e5` 대신 `bg-primary`. 새 색이 필요하면 토큰을 먼저 추가하고 이유를 남긴다.
2. **포커스 링은 모든 인터랙티브 요소에.** `globals.css` 의 `:focus-visible` 전역 규칙을 지우지 않는다.
3. **표 헤더는 세로로 줄바꿈되지 않게.** 헤더 셀 `whitespace-nowrap`, 값 셀은 `break-all` 대신 `min-w-*`.
4. **빈 셀은 `—`** (em dash) + `muted-foreground`. 빈 문자열·`null` 노출 금지.
5. **IP · CIDR · 경로 · 버전 · 마스크는 `font-mono`.**
5-1. **아이콘은 `size` 만 주고 `strokeWidth` 는 2 고정.** Figma `Sec/Icon`(`34:21`) 에 적힌
   `2 × (크기/24)` 환산표(20→1.67 · 16→1.33 · 12→1.0)는 **Figma 캔버스에 그릴 때 쓰는 값**이다 —
   브라우저는 SVG 축소 시 stroke 도 같이 줄이므로 코드에서는 2 를 그대로 둔다. SVG 를 손으로 다시
   그리지 않는다. Figma `Sec/Icon` 의 31개는 **"앱에서 추출"한 현황 스냅샷**이지 허용 목록이 아니다 — 디자이너가
화면을 조립하려고 당시 번들에 있던 것을 옮겨 둔 것이다. 글리프를 아이콘으로 바꾸면 목록에 없는
아이콘(`RotateCw` `Play` `Square` 등)이 필연적으로 늘어난다. **Lucide 안에서 고르되 뜻이 겹치는
아이콘을 새로 만들지 않는다**가 규칙이다.
6. **모듈 이름은 소문자 고정** — `oam` `oam-svc` `csc` `csp` `cmp` `cmdp`.
7. **0건에 경고색을 쓰지 않는다.** `드리프트 0` 은 Neutral.

## 4. 컴포넌트 계약

**정본 = Figma `02 Components` (`15:2`).** 컴포넌트 27종의 배리언트·상태가 여기에 전부 정의돼 있고,
섹션마다 "왜 이 컴포넌트가 있는가"까지 적혀 있다(예: `TableHeaderCell` — *"세로 줄바꿈 방지가 이
컴포넌트의 존재 이유"*). 핸드오프 `components/contracts.md`·`MAPPING.md` 는 이걸 글로 옮긴 요약본이고,
shadcn 대응표는 거기가 정본이다. 어긋나면 Figma 가 맞다.

**새 화면을 만들 때도, 스펙 없는 페이지를 고칠 때도 여기 있는 것만 쓴다** — 목록에 없는 컴포넌트를
새로 만들지 않는다.

| 컴포넌트 | 배리언트 · 상태 | 노드 |
|---|---|---|
| Button | Primary·Secondary·Ghost·Danger × sm(26px)·md(36px) × Default·Disabled = 16 | `16:5` |
| Badge | Neutral·Success·Warning·Danger·Info·Brand × Soft·Solid = 12 | `16:33` |
| StatusDot | Success·Warning·Danger·Neutral·Info | `17:18` |
| TextInput (Field) | Default·Focus·Error·Disabled (라벨·필수·도움말·에러 포함) | `17:58` |
| Select | Default·Focus·Disabled | `18:27` |
| Checkbox · Radio | Unchecked·Checked / Unselected·Selected | `163:53` · `43:33` |
| Switch | Off·On | `81:43` |
| SegmentedItem | Default·Selected | `17:26` |
| TabItem | Default·Selected | `18:43` |
| TreeItem | Group·Node × Default·Selected | `19:25` |
| CollapsibleSectionHeader | Level 1·2 × Expanded·Collapsed | `19:37` |
| SectionMessage | Info·Warning·Danger·Success | `20:23` |
| Toast | Info·Success·Warning·Danger | `393:117` |
| Modal | Default·Danger | `392:108` |
| MenuItem · Menu | Default·Danger × Default·Hover·Disabled = 6 / 드롭다운 | `62:176` · `63:53` |
| TableHeaderCell | Align Left·Right | `20:40` |
| EmptyState | 단일 | `20:30` |
| ContextBar | Scope Server·Group | `21:68` |
| **셸 6종** | AppBar · Sidebar · TreePanel · Tabs · PageHeaderRow · StickySaveBar | `457:5431` · `457:5485` · `458:9157` · `458:9160` · `458:9163` · `459:7508` |
| SyncStatusRow | `hasDrift` 로 드리프트 경고 on/off — 0건에 경고색 금지 | `459:7511` |
| Form · 시스템 추가 | Modal content 로 들어가는 조건부 폼 | `460:302` |
| Icon (Lucide) | 실제 번들 31개 (`34:21`) | `34:21` |

shadcn 매핑은 `components/MAPPING.md` — 18종은 `npx shadcn@latest add` 그대로, 9종은
`components/custom/` 참조 구현을 이식한다.

**자주 틀리는 것**
- **Button** — Primary 는 화면당 1개. 파괴적 액션은 행 단위 `outline`, 그룹/전체 단위에서만
  `destructive`. 비활성은 `disabled` 속성 + **사유 병기**(불투명도 금지).
- **Badge vs StatusDot** — 값·분류는 Badge, 살아있는 상태(running/stopped/online)는 StatusDot.
- **StatusDot 톤(고정)** — `Success`=online·running·mounted / `Info`=approved(등록됐으나
  heartbeat 없음) / `Neutral`=stopped·미설정 / `Warning`=드리프트·미적용(0건이면 Neutral) /
  `Danger`=offline·unreachable·critical.
- **Switch** — 즉시 적용되는 on/off 전용. **저장 버튼 뒤의 폼 값에 쓰지 않는다.** 선택지는 SegmentedItem.
- **Checkbox** — 16×16 · 라운드 6 · 꺼짐 `--surface` + `--border-strong` 테두리 · 켜짐 `--primary`
  채움에 **테두리 없음** + check 12px (`163:53` 실측). 폼 안에서는 **라벨을 위가 아니라 오른쪽에**
  붙여 한 줄로 그린다(§7-29).
- **Modal** — 확인 대화상자와 생성 폼의 공통 셸. 헤더(제목+✕)·본문 슬롯·푸터(취소+확인).
  되돌릴 수 없는 액션만 `Tone=Danger`.
- **Toast** — 좌측 강조선 + 톤 점. **같은 원인은 한 장으로 묶고 `×N`**, 화면 최대 3장,
  헤더 알람 배지와 같은 소스.
- **Alert(SectionMessage) · Toast** — 화면당 1개 원칙.
- **Table** — 세로 칼럼 구분선 없음. 행 전체 배경 tint 금지.
- **Collapsible** — 중첩 2단까지. **접힌 섹션에 저장 버튼을 노출하지 않는다.**
  **Level 1** = 높이 36 · 굵은 라벨 · **머리 아래 가로 구분선** · 본문 들여쓰기 없음.
  **Level 2** = 높이 32 · 중간 굵기 · 가로선 없음 · 본문에 **세로 레일**
  (`ml-2` + `border-l --border-strong` + `pl-13`, 머리와 12 띄우고 섹션끼리 18) — `174:2860` 실측.
  기본은 **펼침**이다(§7-27)
- **Badge 치수** — 라운드 **6**(`radius/sm`)이지 알약이 아니다. 좌우 6 · 상하 2 · 12px SemiBold ·
  줄높이 1.2 (`16:8` 실측). 알약(999)은 모듈 칩의 **개수 꼬리** 같은 다른 요소다
- **폼 필드(`FormField`)** — 도안의 `field` 는 가로 두 칸이다: 왼쪽 「라벨·컨트롤·도움말」 한 덩어리,
  오른쪽 폭 **62 고정** 마커(사이 10). **마커는 라벨 줄에 맞춰 위로 붙인다** — 표가 들어가는
  필드에서는 가운데 정렬이 성립하지 않는다(§7-28). 라벨·도움말은 마커 밑으로 흐르지 않는다
- **MenuItem Disabled** — 우측에 사유를 짧게 (`그룹 삭제로만 가능`).

## 5. 토큰

**정본 = Figma `01 Design style sheet`.** 라이트·다크가 **대칭으로 두 프레임** 그려져 있고, 섹션
구성도 같다.

| 프레임 | 노드 | 섹션 |
|---|---|---|
| Design style sheet (Light) | `8:2` | Primitives `8:7` · Semantic `8:12` · Typography `8:17` · Space `8:22` · Elevation `8:27` |
| **Design style sheet · Dark** | `13:2` | Primitives `13:7` · **Semantic `13:166`** · Typography `13:323` · Space `13:403` · Elevation `13:469` |

핸드오프 `tokens/globals.css` 는 이걸 shadcn 이름으로 옮긴 것이고 **라이트·다크 모두 값이 정확히
일치한다**(대조 완료). 두 층으로 되어 있다.

- shadcn 표준 변수 28개(`--primary` `--background` `--border` `--ring` `--sidebar-*` …) — shadcn
  컴포넌트가 그대로 집어간다.
- `--cims-*` 도메인 변수 26개 — shadcn 에 없는 의미색(성공/경고/정보/중립·elevation·focus-ring 등).

컴포넌트 파일에 hex 를 쓰지 않는다.

### 5.1 개명 매핑 (현재 `index.css` 46개 → 시안)

이름이 그대로 겹치는 것은 `--border` · `--primary` · `--radius` 셋뿐이다. 나머지는 아래 표대로
일괄 치환한다 (`var(--...)` 참조 1,601곳 = tsx 1,300 + css 301).

**A. 개명만 — 값 동일**

| 지금 | 시안 | | 지금 | 시안 |
|---|---|---|---|---|
| `--bg` | `--background` | | `--danger` | `--destructive` |
| `--bg-soft` | `--muted` | | `--danger-h` | `--cims-danger-hover` |
| `--surface` | `--card` | | `--success` | `--cims-success` |
| `--surface-2` | `--secondary` | | `--warning` | `--cims-warning` |
| `--hover` | `--accent` | | `--shadow` | `--cims-elevation-sm` |
| `--text` | `--foreground` | | `--shadow-lg` | `--cims-elevation-lg` |
| `--text-muted` | `--muted-foreground` | | `--shell-border` | `--sidebar-border` |
| `--header-muted` | `--cims-neutral` | | `--sidebar-bg` | `--sidebar` |
| `--header-bg` | `--cims-surface-header` | | `--sidebar-fg` | `--sidebar-foreground` |
| `--primary-h` | `--cims-brand-hover` | | `--sidebar-hover` | `--sidebar-accent` |
| `--primary-soft` | `--cims-brand-soft` | | `--sidebar-fg-active` | `--sidebar-primary` |
| `--border` · `--primary` · `--radius` | 그대로 (`8px` = `0.5rem`) | | | |

**B. 개명 + 값이 시안 값으로 바뀐다** — 원칙 1 (시안이 정본)

| 지금 | 값 | 시안 | 값 |
|---|---|---|---|
| `--success-soft` | `#ecfdf5` | `--cims-success-soft` | `#f0fdf4` |
| `--warn-soft` | `#fff8e1` | `--cims-warning-soft` | `#fffbeb` |
| `--danger-soft` | `#fff1f2` | `--cims-danger-soft` | `#fef2f2` |
| `--header-fg` | `#1e2433` | `--cims-text-header` | `#1e293b` |
| `--sidebar-active-bg` | `#eef2ff` | `--sidebar-accent` | `#f1f5fb` |

**다크에서는 10개가 바뀐다** (Figma `13:166` 대조). 라이트보다 많고, soft 계열은
**투명 → 불투명** 이라 눈에 띄는 변화다 — 투명은 뒤에 뭐가 깔리느냐에 따라 표 위·카드 위·모달 위에서
색이 제각각으로 보인다. 시안이 그걸 고정한 것이다.

| 지금 (다크) | 값 | 시안 | 값 |
|---|---|---|---|
| `--primary-soft` | `rgba(99,102,241,.16)` | `--cims-brand-soft` | `#22284b` |
| `--danger` | `#f04848` | `--destructive` | `#ef4444` |
| `--danger-soft` | `rgba(240,68,68,.14)` | `--cims-danger-soft` | `#382222` |
| `--success-soft` | `rgba(34,197,94,.14)` | `--cims-success-soft` | `#173733` |
| `--warning` | `#fbbf24` | `--cims-warning` | `#f59e0b` |
| `--warn-soft` | `rgba(234,179,8,.13)` | `--cims-warning-soft` | `#393126` |
| `--sidebar-fg` | `#94a3b8` | `--sidebar-foreground` | `#93a1b8` |
| `--sidebar-fg-active` | `#a5b4fc` | `--sidebar-primary` | `#6366f1` |
| `--sidebar-active-bg` | `rgba(99,102,241,.18)` | `--sidebar-accent` | `#1b2435` |
| `--sidebar-hover` | `rgba(255,255,255,.06)` | `--sidebar-accent` | `#1b2435` |

`--sidebar-hover` 와 `--sidebar-active-bg` 가 시안에서는 **`--sidebar-accent` 하나로 합쳐진다** —
라이트·다크 공통이다 (`shell.md`: 선택 항목 = `sidebar-accent` 배경 + primary 텍스트).

**C. 시안에 없다 → 이름·값 그대로 둔다.** 개명하지 않는다 — 시안에 없는 것은 정리도 하지 않는다(§1).

| 갈래 | 변수 | 비고 |
|---|---|---|
| 차트 계열색 13 | `--chart-1`~`--chart-12` · `--chart-muted` | 계열색은 "무엇인가"라서 상태색(success/warning/danger)을 쓰지 않는다는 우리 규칙이 붙어 있다. **위젯 차트는 앞 5색을 순환**하고(`seriesColor`), 발언자 색처럼 계열이 많은 목록이 6~12 를 쓴다 — 12색은 그 때문에 늘렸다 |
| 레이아웃 치수 8 | `--sidebar-w` · `--sidebar-w-full` · `--header-h` · `--main-pad` · `--canvas-w/h` · `--design-w/h` | 위젯 격자·셸 치수. 시안은 색 토큰만 다룬다 |
| 기타 2 | `--dev-accent`(개발자 모드) · `--radius-lg` 를 쓰는 곳 | 시안에 개발자 모드 개념이 없다 |

**D. 시안에만 있는 신규 20여 개** — 순증이라 매핑 불필요.
`--ring` · `--input` · `--popover*` · 각 색의 `*-foreground` 짝 · `--cims-focus-ring` ·
`--cims-info-*` · `--cims-neutral-*` · `--cims-text-disabled` · `--cims-border-strong` ·
`--cims-surface-raised` · `--sidebar-primary*` · `--sidebar-ring` · `--cims-on-solid`(§7-18).
§3 의 절대 규칙(포커스 링 필수 · 0건은 Neutral · 비활성은 불투명도 대신 토큰)을 지키려면 필요한 것들이다.

### 5.2 Figma 원본에만 있는 것 — 핸드오프 누락

**색 값은 라이트·다크 모두 정확히 옮겨졌다.** 다만 색이 아닌 것 몇 가지가 `tokens/globals.css` 에
안 담겼다. 그 항목은 Figma 를 본다.

- **Figma 변수는 우리 현재 이름을 쓴다.** `--bg` `--surface` `--text` `--text-muted` `--primary`
  `--border` `--sidebar-bg` `--header-bg` `--shell-border` `--radius` `--radius-lg` `--surface-2`
  `--bg-soft` — 즉 §5.1 의 shadcn 개명은 **디자이너의 결정이 아니라 핸드오프 작성자의 변환**이다.
  개명 자체는 유지한다(shadcn 컴포넌트가 표준 이름을 집어가야 하므로). 다만 Figma 를 읽으면
  옛 이름이 나온다는 걸 알고 §5.1 표로 옮겨 읽는다.
- **Figma 에는 Mantine 토큰이 남아 있다** — `--mantine-font-size-*` · `--mantine-spacing-*` ·
  `--mantine-radius-sm`. Mantine 계획 시절의 잔재다. **이름은 버리고 값만 취한다.**
- **`--radius-lg: 14`** 는 Figma 에 실재한다. 핸드오프 `globals.css` 가 빠뜨렸을 뿐이므로
  §5.1-C 의 보존이 아니라 **시안 값 그대로 채택**한다 (`--radius: 8` = `0.5rem`, `radius/sm` = 6).
- **간격 스케일 `2 · 4 · 6 · 8 · 10 · 12 · 14`** — 핸드오프 문서 어디에도 없다.
- **`--text-on-solid: #ffffff`** — solid 배지·버튼 위 글자색.

**텍스트 스타일 (Figma Typography 컬렉션 — 굵기·행간까지)**

| 스타일 | 크기 | 굵기 | 행간 | 자간 |
|---|---|---|---|---|
| `Heading/lg` | 18 | SemiBold 600 | 1.3 | -1 |
| `Heading/md` | 16 | SemiBold 600 | 1.4 | -1 |
| `Heading/sm` | 14 | SemiBold 600 | 1.4 | -1 |
| `Body/base-strong` | 14 | Medium 500 | 1.5 | -1 |
| `Body/dense` | 13 | Regular 400 | 1.5 | -1 |
| `Body/dense-strong` | 13 | Medium 500 | 1.5 | -1 |
| `Label/table-head` | 12 | SemiBold 600 | 1.4 | **+1** |
| `Label/badge` | 12 | SemiBold 600 | 1.2 | 0 |
| `Label/form` | 12 | Medium 500 | 1.4 | 0 |
| `Caption/xs` | 11 | Regular 400 | 1.4 | 0 |
| `Mono/code` | 12 | JetBrains Mono Regular 400 | 1.4 | 0 |

`fonts.md` 의 타입 스케일(11·12·13·14·16·18)과 일치한다. 굵기·행간·자간은 여기에만 있다.

## 6. 자료 맵

디자인 정본은 Figma 01·02(§6.1)이고, 아래 핸드오프 폴더는 그 해설 + shadcn 변환 + 화면 스펙이다.

```
cims-design-handoff/
  DESIGN-RULES.md          매 작업 규칙 (원본의 CLAUDE.md — 개명만 함)
  README.md                패키지 개요
  tokens/globals.css       색·타이포·그림자 — 유일한 출처
  tokens/tailwind.config.ts 토큰 → 유틸리티 매핑
  tokens/fonts.md          폰트 2종 + 타입 스케일
  components/MAPPING.md    Figma 27종 → shadcn 대응표 + 설치 명령
  components/contracts.md  배리언트·상태 목록
  components/custom/       shadcn 에 없는 9종 참조 구현 (.tsx)
  screens/INDEX.md         화면 목록 (S/G/A/SA 스코프 구조)
  screens/shell.md         AppBar · Sidebar · TreePanel · Tabs
  screens/server-scope.md  S1~S4        screens/as-group.md   G1·G2·G3-*·G4
  screens/aa-group.md      A1~A4        screens/sa-scope.md   SA1·SA3
  screens/modals.md        M3 외        screens/empty-states.md 빈 상태 5종
  screens/FIGMA-LINKS.md   화면별 Figma 바로가기 (view-only)
  decisions.md             웹과 다르게 그린 곳과 이유
```

### 6.1 Figma 원본 직접 읽기

Figma MCP 커넥터(`claude.ai Figma`)가 붙어 있으면 **링크를 사람에게 넘기지 않고 직접 읽는다.**
`/mcp` 로 인증한 세션에서만 동작하므로, 도구가 없으면 사용자에게 인증을 요청한다.

- 파일 키 **`3nHGkPN8XAaHQxPNGn608v`** · 좌석은 Full/Dev 여야 한다 (View 는 월 6회, Full 은 200/일·10/분)
- **페이지 지도** — `get_metadata` 를 nodeId 없이 부르면 첫 페이지만 돌려주니 노드로 직접 들어간다

  | 페이지 | 노드 | 용도 |
  |---|---|---|
  | 01 Design style sheet | `8:2`(Light) · `13:2`(Dark) | 토큰 — §5 |
  | 02 Components | `15:2` | 컴포넌트 27종 — §4 |
  | 03 Screens | 화면별 node-id 는 `screens/FIGMA-LINKS.md` (33개) | 화면 값 — ③층 |

  `01`(`1:2`) 을 통째로 부르면 응답이 커서 잘린다 — `8:2`·`13:2` 나 섹션 노드로 좁혀 부른다.
- `get_metadata(fileKey, nodeId)` — 프레임 트리를 XML 로. 레이어 이름·좌표·크기까지 나오므로
  표 컬럼 폭·섹션 간 여백처럼 글 스펙에 없는 값이 여기서 정확히 확인된다
- `get_screenshot(fileKey, nodeId, maxDimension)` — PNG. 짧은 수명의 URL 을 주므로 `curl` 로 받아 읽는다
- `get_variable_defs(fileKey, nodeId)` — 그 노드가 쓰는 토큰 정의 (§5.2 가 이 결과다)
- `get_design_context(fileKey, nodeId)` — 참조 코드. 호출 전 `figma-design-to-code` 스킬을 먼저 읽는다

문서 스펙(`screens/*.md`)과 그림이 어긋나면 **그림이 맞다.**

`screens/FIGMA-LINKS.md` 는 화면 ↔ node-id 대응표로 쓴다 (§6.1). MCP 가 없는 세션에서는 링크를
사용자에게 제시하고 답을 기다린다 — 모르면 지어내지 않는다.

## 7. 충돌 판정 — 핸드오프 vs 현재 코드

§0 의 판정 원칙을 구체 사례에 적용한 결과다. **이 표가 핸드오프보다 우선한다.**

| # | 핸드오프 기술 | 실제 | 판정 | 근거 |
|---|---|---|---|---|
| 1 | `decisions.md` §2 — 위젯 편집 모드 미채택(미결) | **살아있는 기능이고 개발 중.** `widgets/EditableLayout.tsx` · `GridEditor.tsx` · `CardLayout.tsx`, 카드 안 배치 편집까지 있다 | **현행 그대로 보존.** 디자이너에게 확인한 결과 **시안 누락**이었다 — 제외 의도가 아니다. 페이지 편집·카드 안 편집 모두 현재 구성 유지 | 사용자 확정 |
| 2 | 시안 AppBar 에 `✎` 가 없다 — Figma `457:5542` utilities 는 **알람·설정·계정 셋뿐**이고, 설정 드롭다운·계정 메뉴에도 없다(D6 스펙 = 2+2). `app-bar.tsx` 주석도 "위젯 편집 → 제거" | 현재 `EditableLayout.tsx` 가 전역 헤더 슬롯(`.app-header-editslot`)에 portal 로 꽂는다 | **현행 유지 — AppBar 에 편집 슬롯을 남긴다.** 시안 utilities 는 넷이 된다(알람·설정·계정 + 편집). 누락분을 채우는 것이라 시안 위배가 아니다 | 사용자 확정 |
| 3 | `index.css` 를 전부 걷어내는 것으로 읽힘 | 위젯 편집·2D 그리드 CSS(`.grid-canvas` `.card-canvas` 계열)는 스타일이 아니라 **플랫폼 인프라** | 그리드/카드 편집 CSS 는 **보존**. Tailwind 로 옮기더라도 동작 동일성 우선 | 원칙 2 (#1 의 일부) |
| 4 | `decisions.md` §1 — "알람 배지 **신설**" | **이미 있다.** `components/Header.tsx:37` 의 `<AlarmIndicator />`. 헤더 아이콘도 이미 Lucide(`KeyRound` `LogOut` `Sun` `Moon` `Code`) | 결과는 시안과 같다. 헤더 재편(설정 드롭다운·계정 메뉴 분리)만 시안대로 진행 | 서술만 stale |
| 5 | "Figma 링크를 사람에게 열어보게 하세요" | **Figma MCP 로 직접 읽는다** — 구조·치수·문구·스크린샷 전부 접근된다 | 사람에게 넘기지 않는다. §6.1 대로 직접 읽고, 스펙 문서와 어긋나면 **그림을 따른다** | 전제가 바뀜 |
| 6 | 실측 대상 `121.161.164.140:4419` 등 | 운영/개발 서버 | 실서버 배포·화면 검증은 **사용자 담당** | 작업 방식 |
| 8 | 시안 AppBar 의 **접속 환경 칩** `[운영]` — 무엇이 환경인지 시안에 없다 | 콘솔에 "운영/개발" 개념이 따로 없다. `VITE_CONSOLE_PROFILE`(base/svc)은 다른 축 | **개발자 모드 스위치가 곧 환경 축이다**(사용자 확정) — 켜짐=`개발` / 꺼짐=`운영`. `utils/devMode` 의 브라우저 로컬 값이라 서버 속성이 아니라 "지금 내가 보는 모드"다. 우측에 따로 있던 「개발자 모드」 배지를 이 칩이 대신한다 | 사용자 확정 |
| 9 | 시안 참조 구현은 알람 카운트 배지를 `count > 0` 일 때만 그린다 | 0건도 상시 표시하는 것은 **의도적 설계** — "표시 없음 = 정상"과 "표시 없음 = 표시 고장"을 구분한다(alarm_pipeline.md §8.2) | 벨 + 배지 **형태는 시안대로**, **0건은 숨기지 않고 `neutralSoft`** 로. `DESIGN-RULES` §1-7 도 *0건에 경고색을 쓰지 말라*(= Neutral 로 내라)이지 숨기라는 게 아니다 | 원칙 2 + §3-7 |
| 10 | 핸드오프 README 가 **미결**로 남긴 "그룹 화면의 저장 방식 — 섹션별 적용 vs 하단 통합 저장" | **시안 그림에는 답이 있다** — G1(42:428) 하단에 StickySaveBar `변경 n건 · 전 멤버 적용` / `저장 — 전 멤버 적용` | **통합 저장(A′)** — 단, **필드 단위 dirty** 로 바뀐 필드만 보낸다. backend `_update_group` 은 `if k in body` 부분 갱신이라 안 보낸 필드는 손대지 않는다. 그러면 지금(섹션 단위 dirty)보다 **사정거리가 오히려 좁아진다.** keepalived 재렌더도 섹션 수만큼 → 1회 | 시안 그림(§6.1) + 사용자 확정 |
| 11 | 핸드오프 참조 구현 `badge-variants.ts` 의 Soft 6종이 `border-transparent` | **시안 Badge 는 테두리가 있다** — Figma `16:33` 실측으로 채움 `--*-soft`, **테두리·글자 `--*-on-soft`**(brand `#4338ca` · success `#15803d`) | Soft 를 `border-current` 로 켠다(글자색이 곧 테두리색이다). Solid 은 테두리색=배경색이라 `border-transparent` 그대로 | 그림이 정본(§6.1) |
| 12 | 참조 구현 `empty-state.tsx` 가 `border-dashed` + `bg-muted/40` | **시안 EmptyState 에 테두리가 없다** — Figma `20:26` 실측: 채움 `--bg-soft` · 라운드 14 · 여백 20 · 제목 14px Medium · 설명 11px muted | 테두리 없이 `bg-muted`. 문구는 `empty-states.md` ES-1~5 그대로 | 그림이 정본 |
| 13 | shadcn 기본 Alert(+ 참조 구현)는 사방 1px 테두리 | **시안 SectionMessage 는 Atlassian 패턴** — Figma `20:23` 실측: **좌측 3px 바 `--*`** + 채움 `--*-soft`, 사방 테두리 없음 | `border-l-[3px]` 로 바꾼다. 4톤·화면당 1개 원칙은 그대로 | 그림이 정본 |
| 14 | `contracts.md` §Button — "행 단위 파괴적 액션도 Secondary" | **시안 G1 은 갈린다** — VIP 행 `삭제` 는 Danger(`44:696`, `#dc2626` 실측)인데 마운트 행 `삭제` 는 Ghost(`187:2943`) | 그림대로 각각 쓴다. 글로 된 규칙은 기본값이고, 그 화면 그림이 있으면 그림이 이긴다 | 그림이 정본 |
| 15 | 멤버 표 `접속` 셀 — G1 그림(43:674)은 **Badge**(`online` 아웃라인), A1 그림(189:3238)은 **StatusDot** | 두 그림이 갈린다 | **StatusDot** 으로 통일. `DESIGN-RULES` §2 가 "값·분류는 Badge, **살아있는 상태(running/stopped/online)는 StatusDot**" 이라고 명시한다 — 그림끼리 어긋날 때는 글로 된 규칙이 결정한다. ContextBar 는 그림대로 **점 + Badge 둘 다**다(21:68) | 글 규칙이 결정 |
| 16 | `DESIGN-RULES` §2 톤 매핑(고정) — **Neutral = stopped** | **시안 네 장이 전부 `stopped` 을 Danger 로 그렸다** — S2(91:1809)·S4(92:2051) 는 `dangerSoft` 배지(`#b91c1c`/`#fef2f2`), G2(184:2897)·G4 는 빨강 StatusDot(`#dc2626`) | **모듈 프로세스 상태는 Danger** 로 간다(그림). 그 표는 **노드/에이전트** 톤 맵으로 두고 — 노드가 `stopped` 인 경우는 없다 — 모듈이 죽어 있는 것은 서비스 영향이라 색이 달라야 한다. 「고정」이라고 적힌 표와 어긋나므로 **디자이너 확인 대상**(§9) | 그림이 정본 + 도메인 |
| 17 | 같은 값의 모양이 화면마다 다르다 — 모듈 상태가 서버 화면(S2·S4)은 **Badge**, 그룹 화면(G2·G4)은 **StatusDot** | 네 그림이 스코프별로는 서로 일치한다(서버 2장 Badge · 그룹 2장 StatusDot) | **각 화면 그림 그대로** 간다 — 스코프마다 밀도가 달라 내린 선택으로 본다. 톤만 두 모양에서 같게 맞춘다. #15(멤버 표 `접속`)와 다른 판정인 이유: 거기서는 **같은 표의 같은 컬럼**이 두 가지로 그려져 그림끼리 모순이었다 | 그림이 정본 |
| 18 | 핸드오프 Badge Solid 6종이 글자색을 `text-white` 로 적는다 — 유틸리티 클래스라 인라인 `style` 에서 못 쓴다 | 채도 있는 솔리드 채움 위 글자는 라이트·다크 **양쪽 다 흰색**이어야 한다(핸드오프가 두 테마 모두 `text-white`). `--*-foreground` 짝은 solid 상태색(`--cims-info` 등)에는 없다 | 값이 같은 토큰 `--cims-on-solid: #ffffff` 를 두 테마에 넣고 인라인 `style` 은 이것을 쓴다. 클래스를 쓸 수 있는 자리는 `text-white` 그대로 — 값이 같아 갈라지지 않는다 | 시안 침묵(수단), 값은 그림대로 |
| 25 | **섹션 머리를 Level 2 하나로만 구현해 뒀다** — 시안 `CollapsibleSectionHeader`(19:37)는 **Level 1·2 두 종류**다 | G1 의 다섯 구획(그룹 설정·절체 조건·멤버·VIP·마운트)과 S1 의 `정보`·`네트워크` 는 도안에서 **전부 Level 1** 이다 — 굵은 라벨 + **머리 아래 구분선** · 본문 들여쓰기 없음(높이 36). Level 2 는 그 안의 하위 구획(IP / Routing…)으로 중간 굵기 + 얇은 화살표 + 좌측 들여쓰기(32). 게다가 `절체 조건`(ServersPage)과 `정보`/`네트워크`(InspectorSection)는 **회색 띠 머리**를 따로 갖고 있었다 | `SubSection` 에 `level` 을 넣고 두 층을 다 그린다. 회색 띠 머리 둘도 같은 Level 1 로 통일. **`절체 조건` 은 기본 펼침**(#27) | 그림이 정본 |
| 24 | 시트 2 에 있는데 여태 네이티브로 둔 것들 — 체크박스 52 · 텍스트 입력 10 · `alert()` 3 · `prompt()` 3 | `Checkbox`(163:53)·`TextInput`(17:58)·`Toast`(393:117)·`Modal`(392:108)이 시트 2 에 있다. `ExternalSystemsPage` 의 입력칸 9개는 클래스가 폭뿐이라 **테두리 없는 브라우저 기본 입력칸**으로 그려지고 있었다 | 전부 계약 컴포넌트로. `prompt()` 는 시트 2 에 「Prompt」가 따로 없어 **Modal + TextInput 을 조합**했다(`custom/prompt.tsx`) — `confirm.tsx` 와 같은 방식이고, 목록에 없는 컴포넌트를 새로 만든 게 아니다. 3상태 체크박스는 Radix 가 `checked="indeterminate"` 를 정식으로 받아 ref 로 DOM 을 만지던 코드가 없어졌다 | ②층 |
| 23 | 손으로 만든 모달이 8개 더 있었다 — `.modal-overlay` + `.modal-box` 를 직접 쌓고 Esc 는 `window` 리스너로 흉내 냈다. T2-17 은 **확인 대화상자**만 Dialog 로 옮긴 것이었다 | `dialog.tsx` 도 shadcn 기본값(`p-6 gap-4 rounded-lg bg-background`)이라 세 영역을 한 상자로 본다. Figma `Sec/Modal`(392:108) 실측은 **헤더 51 + 구분선 · 본문 여백 20 · 푸터 우측(간격 8)** 로 갈려 있고, T2-17 은 그 값을 confirm 쪽 인스턴스 클래스로 얹어 두었다 | 계약을 **base 로 올린다** — `DialogContent`/`Header`/`Footer`/`Title` 이 실측값을 갖고, `confirm.tsx` 의 덮어쓰기는 걷는다. 제목은 Figma `Body/dense-strong`(13px Medium) — T2-17 의 `text-base`(14px)는 base 로 맞춘다. `Modal` 은 호출부 API(title·onClose·wide·width)를 그대로 두고 안에서 Dialog 를 쓴다 | 그림이 정본 + ②층 |
| 22 | `.empty` 한 클래스에 **뜻이 셋** 섞여 있었다 — 「비어 있음」·「조회 실패」·「로딩 중」이 전부 같은 회색 가운데 글자였다 (104곳) | 시안은 셋을 다르게 다룬다 — 빈 상태는 `EmptyState`(20:26), 오류는 `SectionMessage` Danger(20:23), 로딩은 **아예 다루지 않는다** | 뜻대로 가른다. 빈 상태 54 → `EmptyState` · 조회 실패 8 → `Alert variant="danger"` · 로딩 42 → 옛 `.empty` 가 주던 것(가운데·여백 32·muted)을 유틸리티로. **문구는 화면 것 그대로** 둔다(③층) — `empty-states.md` ES-1~5 는 도안이 있는 화면에만. 배치도 T2 도안 표와 같게 **블록**으로 둔다 — `.empty` 의 `flex:1` 세로 가운데 정렬은 레거시 동작이지 계약이 아니다 | ②층 + 그림 |
| 21 | 네이티브 `<select>` 77곳 — Figma `Sec/Select`(18:27) 설명이 "네이티브 셀렉트/날짜입력은 전부 이 컴포넌트와 DatePicker 로 교체" 라고 못박았다 | Radix Select 로 옮기면 **빈 문자열 value 를 못 쓴다**(빈 값은 placeholder 전용이라 `<SelectItem value="">` 이 던진다). 그런데 필터 28곳이 `<option value="">전체</option>` 로 `''` 를 「전체」의 값으로 쓴다 | 상태의 뜻은 그대로 두고 **컴포넌트 경계에서만** 표식으로 바꾼다 — `custom/select-value.ts` 의 `toSel`/`fromSel`/`NONE`. 저장·API 로 나가는 값은 여전히 `''` 다. **DatePicker 는 아직 없다** — shadcn 설치 목록에도 `calendar`·`popover` 가 없어 날짜 입력은 네이티브로 남는다(§9) | 그림이 정본 + ②층 |
| 20 | `input.tsx` 만 **shadcn 기본값 그대로**였다 — Button·Toggle 은 시안 실측으로 맞췄는데 여기만 `shadow-sm` · `focus-visible:ring-1` · `disabled:opacity-50` | Figma `Sec/TextInput (Field)`(17:58) 실측: 컨트롤 **높이 33** · 좌우 10 · 글자 12px · 라운드 8 · 채움 `--surface`, 포커스는 **테두리 primary + `Focus/ring`**(0 0 0 3px #4F46E559 = `--cims-focus-ring`) | 그림대로 고친다. 그림자 없음 · 포커스는 `shadow-focus` · **비활성은 불투명도 대신** `--secondary` 채움 + muted 글자. 레거시 `.form-input` 은 13px 에 **포커스 링이 아예 없었다** — 165곳이 §3-2 위반이었다 | 그림이 정본 + ②층 |
| 19 | 「한 축에서 하나만 고르는 버튼 묶음」이 30여 곳 — 고른 것만 Primary 채움, 나머지는 Ghost/Outline 인 **손으로 만든 세그먼트**였다 (기간·시간 단위·추세 창·가입자 상태·종류 필터·`.tab-btn` 서브탭) | 시안에 이 모양의 컴포넌트가 이미 있다 — `Sec/SegmentedItem`(17:26). 손으로 만든 쪽은 선택 상태가 버튼 변형에 얹혀 있어 **키보드 화살표 이동·`role=radiogroup`** 이 없다 | **ToggleGroup 으로 옮긴다** — 배타 선택은 `type="single"`, 「종류」처럼 여러 개 켜는 것은 `type="multiple"`. 판정 기준은 **한 축의 값을 고르는가**(→ 세그먼트) 대 **동작을 실행하는가**(→ Button). 그래서 `[시작]/[정지]`·재생 토글·「선택 N건」 처럼 상태에 따라 색이 바뀌는 **액션** 버튼은 Button 으로 둔다 | ②층(컴포넌트 계약) |
| 7 | `modals.md` "미작성 — 확인 대화상자 · 웹에 있는지 **미확인**" | **있다. 51곳** — 전부 브라우저 네이티브 `window.confirm()` (20개 파일, `ServersPage` 18) | 껍데기는 **시안 Dialog(Tone=Danger)로 교체** — §1 ①층(방식)이다. 네이티브는 토큰·Lucide·포커스 링·`destructive` 가 전부 안 먹어 §3 을 만족할 수 없다. **어느 액션에 붙일지와 문구는 현행 유지** | ①층(껍데기) + 시안 침묵(대상·문구) |
| 26 | **`패키지 설정` 화면을 도안과 한 줄씩 대조하니 여덟 군데가 어긋났다** (사용자 지적) | ①`SubSection` Level 2 에 **세로 레일이 없고** 대신 섹션마다 가로 구분선을 그렸다 — 도안 `SectionBody`(174:2860)는 `rail`(폭 8) + `border-l --border-strong` + `pl-13` 이고 가로선은 **Level 1 전용**이다(19:36 네 상태). ②모듈 칩이 `rounded-md`·13px·회색 채움에 개수는 **primary 솔리드 알약** — 도안(266:4698)은 `rounded-full`·14px SemiBold·흰 채움에 개수는 `--surface`/`--bg-soft` 알약 + muted 글자. ③`Badge` 가 **알약**(`rounded-full px-2`) — 도안(16:8)은 **라운드 6 · 좌우 6 · 줄높이 1.2**. ④저장바 버튼이 sm(26) — 도안(459:7238)은 md(36)이고 바 높이 60 = 12+36+12. ⑤`FormField` 라벨 간격 4·마커 폭 가변 — 도안은 6·**고정 62**. ⑥`관리 store` 이관 안내가 **노란 경고 상자** — 도안(G3-1·SA3 462:8305)은 안내문 + Secondary 버튼뿐. ⑦`멤버 비교` 표가 회색 머리띠 카드 + **행 전체 배경 tint** + 인라인 `style` — 도안(G3-2 162:2565)은 Level 2 머리 + 레일 + 시안 표 껍데기이고 구분은 **배지**(공통/드리프트/개별)다. 표 계약도 「행 배경 tint 로 상태를 나타내지 않는다」. ⑧`모듈 운영 명세` 가 Level 2 + 기본 접힘 — 도안(164:2765)은 **높이 36 = Level 1** 이고 펼쳐져 있다 | 여덟 가지 모두 도안 값으로 고쳤다. ③은 콘솔 전 화면의 배지 모양이 바뀌는 변경이지만 컴포넌트 계약이라 그대로 간다. **칩 제목의 개수는 되살렸다** — S3 도안(169:2822)만 `서버 개별 설정` 이고 SA3 도안(462:7876)과 글 스펙 둘 다 `서버 개별 설정 (13)` 이라 **도안끼리 갈릴 때는 다수·최신 쪽**을 따랐다. 제목은 배포 화면이면 단독 서버(SA)에서도 `서버 개별 설정` 이다(SA3) | 그림이 정본 |
| 27 | 글 스펙이 **기본 접힘**이라 적은 섹션 셋 — `절체 조건`(as-group.md:21) · `모듈 운영 명세`(as-group.md:49) · `Infrastructure (내부 전용)`(server-scope.md:72) | 도안은 **셋 다 펼쳐서** 그렸다(G1·G3-1 164:2765·S3 174:2860). `CollapsibleSectionHeader` 에 Collapsed 변형이 있는데도 `03 Screens` 어느 프레임도 쓰지 않았다 | **전부 기본 펼침.** CLAUDE.md 가 「글 스펙과 그림이 어긋나면 그림이 맞다」라고 정해 뒀고, 사용자가 보는 것은 그림이다. 접기 **기능**은 그대로 남는다 — 접힘은 운영자가 고르는 상태이지 초기값이 아니다 | 그림이 정본 |
| 28 | **필드 안쪽에 그려지는 것이 도안과 달랐다** — 「설정 내부 형태가 다르다」(사용자, 두 번째 지적). S3·SA3 만 보고 맞췄더니 **모든 필드 유형이 다 나오는 G3-3(212:3255)** 과 어긋났다 | ①마커 배지가 **입력칸 높이 가운데**에 있었다. G3-3 은 모든 `field` 에서 `marker` 가 **y=0**(라벨 줄)이고, `CMP probe 엔드포인트`(214:3714)처럼 **키 큰 위젯**(191px)에서는 가운데 정렬 자체가 성립하지 않는다 — 실제로 배지가 표 중간에 붕 떠 있었다. ②`object_list` 편집기가 **점선 상자 + 스타일 없는 표 + 빨간 Danger `×`** 였다. 도안(214:3718)은 **시안 표 껍데기**(머리띠 37 · 행 38) + 우측 `×` + 표 아래 8 띄운 [+ 항목](55×26) + 도움말이다. ③섹션 머리의 **제목이 줄바꿈**됐다 — 힌트가 길면 제목이 쪼개졌다(csc `AuC — IMS AKA 인증 벡터…`). 도안의 머리는 한 줄이고 힌트가 잘린다. ④`ModuleConfigEditor` 에 인라인 `style` 4곳(행 편집 그리드·검증 노트·**선택 행 배경 tint**)과 행 단위 Danger `[삭제]` 가 남아 있었다 | ①**마커는 위로 붙인다**(`items-start`). S3·SA3·G3-1 은 가운데였지만 그 셋은 한 줄 입력만 있어 이 경우를 정하지 못한다 — **모든 위젯이 나오는 도안이 결정한다.** 겸사겸사 `field` 를 도안 구조 그대로 「라벨·컨트롤·도움말 한 덩어리 + 폭 62 마커」 가로 두 칸으로 바꿨다(라벨·도움말이 마커 밑으로 흐르지 않는다). ②`ObjectListEditor` 를 `DataTable`/`Th`/`Td` 로, `×` 는 Ghost, [+ 항목]은 Secondary. ③제목 `shrink-0` · 힌트 `min-w-0 truncate`. ④인라인 style 을 유틸리티로, 선택 행은 `TrLink selected`(표 계약상 행 배경은 선택·hover 피드백 전용), 행 삭제는 Secondary | 그림이 정본 + ②층 |
| 29 | **`C · AS 그룹 · 패키지 설정 (모듈 × 뷰)` 여섯 장을 다 열어 「설정값을 쓰는 자리」를 대조했다** (사용자 지적 세 번째). 앞선 두 번은 oam(G3-1·G3-2)까지만 봤는데, **위젯 종류가 갈리는 것은 oam-svc·csc 쪽**이다 | ①**bool 필드의 모양이 완전히 달랐다.** 우리는 「라벨 줄 / 체크박스 줄 / 도움말 줄」 3줄(70px)로 그렸는데, 도안 G3-5 `field · checkbox`(221:3984)는 **한 줄 18px** — `checkboxRow` = Checkbox 16 + 8 + 라벨(12px)이다. ②`Checkbox` 컴포넌트가 shadcn 기본 그대로였다 — 도안(163:53)은 라운드 6 · 꺼짐 `--surface`+`--border-strong` · 켜짐 `--primary` **테두리 없음** + check **12px**. 우리는 그림자 · 항상 primary 테두리 · check 16px · `ring-1` 포커스 · 비활성 불투명도. ③3상태 체크박스가 「일부 선택」에도 **체크 표시**를 그려 전체 선택과 구별되지 않았다(VerificationV2Page 그룹 체크박스). ④`멤버 비교` 멤버 컬럼 머리의 `↗` 를 T5-4 에서 뺐는데 G3-4(233:4045)에는 있다 — 그 셀이 편집 화면으로 가는 통로라는 표식이다. ⑤배열 값(`CMP probe 엔드포인트`)을 `, ` 로 이어 한 줄에 그렸다. 도안은 **항목마다 줄을 바꾼다** | ①`FormField` 에 `inlineLabel` 을 넣고 bool 에만 켠다. ②③Checkbox 를 도안 실측으로 고치고, 3상태는 시안에 없는 상태라 켜짐과 같은 채움에 **표식만 가로줄**로 갈랐다. ④`↗` 복원. ⑤`itemText` 로 항목마다 한 줄, object_list 항목은 `ip:port` 로 잇는다 | 그림이 정본 |
| 30 | **컬렉션이 많은 모듈에서 뷰 세그먼트가 패널 밖으로 잘렸다** — 시안(G3 `공통 설정 \| 멤버 비교`)은 칸이 둘뿐이라 드러나지 않던 자리 | csp 는 컬렉션이 9개라 세그먼트가 **11칸**이 되고, `ToggleGroup` base 가 `flex`(줄바꿈 없음)라 `Access Service`·`멤버 비교` 두 칸이 잘려 **클릭조차 못 했다**. csp 를 로컬 테스트베드에 실제로 설치해 보고서야 나왔다 | `ToggleGroup` base 에 `flex-wrap`. 칸이 둘인 도안 화면은 그대로고, 넘칠 때만 줄이 나뉜다 — 시안에 없는 경우라 **구조를 새로 만들지 않고** 같은 컴포넌트가 접히게만 했다 | ②층(시안 침묵) |
| 31 | **「다른 메뉴들도 시트 1·2 대로 됐나」를 31 라우트 전수로 재확인했다** (사용자 확인 요청). T0~T4 가 처리한 갈래(hex·네이티브 요소·레거시 클래스·대화상자)는 **깨끗했다** — 코드의 hex 73건은 전부 §9 의 다섯 예외(다이어그램 팔레트·`@media print`·레터박스)이고, 네이티브 `<select>` 0 · `window.confirm/alert/prompt` 0 · 레거시 클래스는 위젯 격자 훅 셋(`.panel`·`.toolbar`·`.scroll-fill`)뿐이다 | 하지만 **토큰 스케일**은 전 라우트에 닿아 있지 않았다: ①스케일 밖 라운드 `rounded-[2·3·4·5·10px]` **40곳**(토큰은 sm6/md8/lg14뿐). ②스케일 밖 글자 `text-[9·10·11.5·12·12.5·15·22px]` **120곳** — 그중 **42곳이 `<Badge className="text-[9px]">`** 처럼 배지를 계약(12px SemiBold) 밑으로 줄인 것이었다. ③버튼·링크 라벨의 글리프 화살표 8곳(위젯 「더 보기」 링크 5 · 페이지 넘김 2 · 변경요약 표 1) — T3-2 가 「라벨 자리면 아이콘」으로 정해 놓고 **위젯은 훑지 않았다.** ④손으로 만든 안내상자 3곳(`bg-*-soft` + 라운드 4)·배지 10곳·입력칸 3곳·버튼 2곳. ⑤`ServersPage` 트리 행이 아직 인라인 `style` 이었다(패딩·`AS`/`M`/`B` 칩·상태 점·`+`/`×` 버튼) — **도안이 있는 셸인데도** 남아 있었다 | 막대·트랙(높이 ≤10, 반경=높이/2)은 `rounded-full`, 나머지는 sm/md 로. 배지 크기 오버라이드는 **지운다**(계약이 크기를 정한다). 화살표는 Lucide, 안내상자는 `Alert`, 칩은 `Badge`, 트리 행은 `StatusDot`+`Badge`+`Button`. 죽은 색 헬퍼 `agentStatusColor`·`depStatusColor` 도 함께 걷었다(상태색 정본 = StatusDot 톤) | ②층 |
| 32 | **「기존엔 그냥 CSS 만 썼던 부분들이 전부 시트 1·2 로 바뀌어야 한다」** (사용자 재확인). §7-31 은 *위반*(hex·글리프·네이티브 요소)을 셌지 **컴포넌트/토큰으로 지어졌는가**를 세지 않았다 | `index.css` 클래스가 `className` 에서 **194회** 쓰이고 있었다 — `.panel` 68 · `.toolbar` 26 · `.scroll-fill` 15 · `.auth-*`(로그인 전체) 18 · `.toast*` · `.alarm-drawer*` · `.docs-content`(**hex `#1e1e2e`/`#cdd6f4` 박힘**) · `.form-grid` · `.tab-bar` · `.panel-header/title` · `.info-dot*`(아이콘이 텍스트 글리프 **`ⓘ`**) · `.widget-api-badge*` · `.app-logo-text`. 그중 `.toast--ok/err/alarm` 은 **규칙이 아예 없어 톤이 안 그려지고 있었다** — 성공도 실패도 같은 흰 상자였다 | **CSS 에는 ①리셋(preflight 대체) ②토큰 ③위젯 2D 격자 훅만 남긴다.** 시각 선언(색·테두리·라운드·여백·타이포)은 전부 유틸리티/토큰으로. `.panel`·`.toolbar`·`.scroll-fill` 은 **선언 없는 이름**만 남겼다(격자가 `.widget-fixed > .panel` 로 잡으므로 이름을 걷으면 배치가 깨진다). `Toast` 는 시트 2 계약(393:116)대로 다시 지었다 — 좌측 3px 강조선 + 톤 점 + **같은 원인 ×N 묶음 · 최대 3장**. `InfoDot` 의 `ⓘ` → Lucide `Info`. 죽은 규칙 13개(`.badge`·`.data-table`·`.empty`·`.form-input`·`.modal-*`·`.toggle*`…)도 걷었다. 결과 **index.css 769 → 526줄**, className 안의 plain-CSS **194 → 105회**(전부 격자·셸 훅) | ①층·②층 |
| 33 | **「대시보드·장애·서비스·성능·구성 메뉴 전부 끝났나」** 확인. §7-32 는 CSS **파일**을 걷었지, JS 안에 **스타일 객체로 만든 평행 체계**는 세지 않았다 | `style={{ ... }}`·`style={obj}` 중 **정적인 것 142곳**이 남아 있었다 — `forms.tsx`(구성>서비스 정의) 35 · `ExternalSystemsPage` 14 · `AutoDeployPage` 14(`SEC`·`H3`) · `VerificationHistoryPage` 21(`th`·`td`·`btnPrimary/Secondary/Danger`·`modalBackdrop`·`modal`·`modalHeader`·`totalsBox`·`tableStyle`·`selectStyle`) · PTT 화면들(`thStyle`·`tdStyle`) · `DispatchGroups`/`PttGroupsWorkbench`(`panel`·`panelHead`) · `statCards`(`CARD_BOX`) · `ServersPage`(`LOCK_FIELDSET_STYLE`) 등. 인라인 `style` 은 **클래스를 이긴다** — `forms.tsx` 의 `style={inp}`(13px)가 `Input` 계약(12px)을 덮고 있었고, `PttGroupActivity` 는 `<Th style={{...thStyle}}>` 로 **계약 컴포넌트 위에** 인라인을 얹고 있었다. `VerificationHistoryPage` 의 회차 상세는 **손으로 만든 모달**이라 포커스 트랩·Esc 가 없었다(§7-23 이 9개를 옮길 때 빠졌다) | 전부 유틸리티/계약 컴포넌트로. 표는 `Th`/`Td`, 버튼은 `Button` 변형, 카드는 토큰. 클래스 이름이 `@media print` 훅인 것(`verify-history-modal*`)은 이름만 남기고 시각만 옮겼다. **정적 인라인 style 142 → 8곳** — 남은 8은 `...style` prop 통과(4) · 데이터에서 오는 `color`(3) · Sparkline `height`(1) 로 전부 값이 런타임에 정해지는 것이다. 예외 두 파일(`VerificationPrintReport`=인쇄 보고서, `SegmentPlayer`=영상 무대)은 §9 그대로 | ①층 |
| 34 | **「그래서 전부 된 거 맞나」에 답하려고 아직 안 센 층을 찾았다** — 그 결과 셋이 더 나왔다 | ①**`rgba()` 로 박힌 색 29곳.** hex 검사(`#rrggbb`)가 통째로 놓친 갈래다. `rgba(74,144,217,…)`(=#4A90D9)는 **토큰에 아예 없는 파랑**인데 선택 행 틴트로 12개 화면에 퍼져 있었다. 위험 `rgba(220,53,69/231,76,60/220,38,38)` · 경고 `rgba(245,158,11)` · 정상 `rgba(34,197,94)` · 오버레이 `rgba(0,0,0,.18~.45)` 도 같은 갈래. `ServiceStatusPage` 의 히트맵은 계열색 rgb 삼원색을 배열에 박아 두고 알파를 만들어 썼다. ②**완전 정적 인라인 style 45곳** — 앞선 검사가 `'var(--…)'` 를 *동적*으로 잘못 세어 빠졌다. ③**`VerificationV2Page` 의 액션 버튼 4개**(전체검증·데이터 초기화·보고서 출력·stage 검증)가 인라인 style 로 만든 버튼이었고, `AutoDeployPage` 는 손으로 만든 세그먼트 + **테두리 없는 네이티브 input 5개**(§7-24 가 ExternalSystemsPage 에서 잡은 그 결함) | 색은 전부 토큰으로(선택 틴트=`brandsoft`, 위험=`dangersoft`, 경고·정상=각 soft, 오버레이=`bg-black/40`, 그림자=`--cims-elevation-*`). 히트맵 계열색은 **`--chart-*` + `color-mix`** 로 바꿨다 — 삼원색을 박아 두면 다크에서 그대로 남는다. 정적 style 45곳은 CSS-in-JS→유틸리티 변환기를 만들어 기계로 옮기고(22파일) 남은 11곳은 손으로. 버튼은 `Button` 변형, 세그먼트는 `ToggleGroup`, input 은 `Input`. **결과: 완전 정적 인라인 style 0 · 토큰 아닌 rgba 0 · 스케일 밖 반경·글자 0 · Tailwind 팔레트 색 0.** 남은 인라인 style 240곳은 전부 런타임 값(상태색·좌표·퍼센트)이다 | ①층·②층 |
| — | **여백·치수의 임의 px(`p-[3px]`·`w-[110px]` 등 407곳)은 위반이 아니다** | `spacing` 을 px 고정 allow-list 로 둔 이유는 **rem 기반 스케일이 `:root` 14px 때문에 어긋나는 것**을 막으려는 것이다. 임의값 `[3px]` 은 이미 px 고정이라 그 문제가 없다. 게다가 도안 자체가 3·5·7·11·13·18·22 와 컬럼 폭 110·130·190·349 를 쓴다 | **실측값을 그대로 쓴다.** 라운드·글자 크기는 Figma 에 닫힌 토큰 컬렉션(sm/md/lg, xs~4xl)이 있어 벗어나면 위반이지만, 여백·치수에는 그런 컬렉션이 없다 | 판정 근거 |
| 35 | **T8 이 API 배지를 깨뜨렸다** — 개발자 모드 `[API n]` 배지가 위젯 **우상단**이 아니라 상단 전폭으로 길게 늘어났다 (사용자 지적) | 2D 격자의 `.widget-fixed > * { width: 100% }` 가 원인이다. `index.css` 는 `@tailwind utilities` **뒤**에 있어 같은 명시도에서 유틸리티를 이긴다 — 그래서 `w-auto` 가 먹지 않았다. 옛 CSS 는 `.widget-api-badge--overlay { width: auto }` 로 **뒤에 오는 같은 명시도 규칙**을 써서 상쇄하고 있었고, 그 주석에 이유까지 적혀 있었는데 T6 에서 유틸리티로 옮기며 그 장치를 잃었다 | `!w-auto` — 이 한 자리에는 `!` 가 **정확한 도구**다. 격자 규칙(플랫폼 레이아웃)을 한 요소만 예외로 빼는 것이고, CSS 쪽에 예외 규칙을 새로 만들면 「화면을 그리는 CSS 는 없다」(§7-32)가 다시 흐려진다. **같은 갈래를 훑는 검사기를 만들었다** — `~/.cims-scratch/css-vs-tw.mjs`(§8.3). `tw-override-check.py` 는 소스만 보므로 `X > *` 같은 **다른 클래스의 자손 규칙**은 원리적으로 못 잡는다. 전 라우트를 훑어 나머지 13건은 값이 같거나(`flex-1`=`flex:1 1 0%`) 격자가 이기는 것이 설계대로(위젯 안 패널 스크롤)임을 확인했다 | 격자 CSS 우선 + ①층 |
| 36 | **API 팝업 크기 고정이 풀렸고, 알람 드로어는 바깥을 눌러도 닫히지 않았다** (사용자 지적) | ①팝업은 원래 `width: min(940px,96vw)` + **`height: min(76vh, calc(100vh-80px))`** 로 크기를 못박아 「항목 수·상세 펼침과 무관하게 항상 같은 창」이었다. **T3-10 에서 Radix Dialog 로 옮길 때 `Modal` 이 `width` 만 받아 height 가 빠졌고**, 그 뒤로 내용만큼 쪼그라들었다(항목 1건이면 183px). T6/T8 이 아니라 T3-10 부터 그랬는데 배지를 고치며 눈에 걸렸다. ②알람 드로어는 `{open && <div>}` 뿐이라 **바깥 클릭·Esc 로 닫히지 않았다** — 벨을 다시 누르거나 ✕ 를 눌러야 했다. 헤더의 톱니바퀴·계정 메뉴는 Radix `DropdownMenu` 라 그 동작을 공짜로 얻는데 드로어만 달랐다 | ①`Modal` 에 `height` prop 을 넣어 `width` 와 대칭으로 만들고 원래 값을 되살렸다(940×912 실측). ②시트 2 에 **Drawer/Popover 가 없다** — 새 컴포넌트를 만들지 않고, `InfoDot` 이 이미 갖고 있던 「바깥 mousedown + Esc」 패턴을 `hooks/useDismiss.ts` 로 뽑아 드로어와 InfoDot 이 같이 쓴다. 트리거를 판정 래퍼 안에 둬서 「벨 다시 눌러 닫기」가 바깥 클릭으로 두 번 처리되지 않게 했다. 6가지 경우(열기·바깥·재열기·Esc·안쪽 클릭 유지·토글) 실측 통과 | ②층 + 시안 침묵 |

`decisions.md` 의 나머지 항목(§1 헤더 재정리 · §3 ContextBar 4탭 유지 · §4 더보기 묶기 ·
§5 build/git 컬럼 분리 · §6 일괄 제어 분리 · §7 제안 3건 · §8 웹 결함 8건)은 **전부 시안을 따른다** —
원칙 1 이 그대로 적용되어 충돌이 아니다.

## 8. 이행 계획

`index.css` 는 **리셋 + 토큰 + 위젯 2D 격자 CSS 만** 남았다(1,502 → 526줄, §8.4·§7-32).
`.panel`·`.toolbar`·`.scroll-fill` 은 **선언 없는 이름**이다 — 격자 규칙이 훅으로 잡을 뿐
시각은 전부 유틸리티다. 화면을 그리는 CSS 는 더 이상 없다.
preflight 는 **끈 채로 둔다**(§8.5 — 켜 보고 되돌린 근거). 다만 한 번에 못
바꾸므로 아래 순서로 간다. 규모는 `var(--...)` 참조 **1,601곳**, 인라인 `style={{ }}` **2,707곳**,
`className=` 1,349곳 — **개명보다 인라인 스타일 걷어내기가 훨씬 큰 작업**이다.

**출처** — 핸드오프 `DESIGN-RULES.md` §5 의 작업 순서(①토큰+config ②`shadcn add` ③custom 9종
④화면은 `screens/` 순서대로·셸 먼저)는 **시스템/인프라 화면만** 대상이다. 아래 T0a~T4 는 그 순서를
**전 라우트로 확장한 우리 계획**이다 — 핸드오프의 지시가 아니다.

**이행 중 충돌 차단 (T0b~T3)** — 레거시 클래스명이 Tailwind 유틸리티와 겹치면 Tailwind 가 조용히
스타일을 얹는다. `tailwind.config.ts` 의 `blocklist` 로 막아 두었다: `text-muted`(6곳 — 우리 CSS 에
정의가 없는 무동작 클래스인데 Tailwind 로는 `color: var(--muted)` = 흰 배경에 흰 글씨가 된다.
muted 글자는 `text-muted-foreground` 가 맞다) · `table`(5곳 — 전부 `<table>` 이라 무해하지만 의도한
적용이 아니다). 해당 페이지를 T3 에서 옮기면 걷는다. **페이지를 옮길 때마다 이 충돌 감사를 다시 돌린다.**

**이행 중 반드시 도는 검사 — 레거시 CSS 가 Tailwind 를 덮는다**

`@tailwind` 지시문이 `index.css` 맨 앞이라 **뒤에 오는 우리 CSS 가 같은 명시도에서 이긴다**.
그래서 컴포넌트를 Tailwind 로 옮길 때 **그 클래스의 옛 규칙을 같이 지우지 않으면 새 스타일이
조용히 무시된다.** T1-1 AppBar 에서 실제로 났다 — `.app-header` 의 `padding`·`gap`·`height`·
`background` 가 남아 있어 `px-5 gap-3 h-[58px] bg-[…]` 가 전부 덮였고, 빌드도 통과했다.

→ 컴포넌트를 옮길 때마다 **`~/.cims-scratch/tw-override-check.py <옮긴 tsx…>`** 를 돌린다.

**소스 검사만으로는 반쪽이다 — 자손 규칙은 DOM 이 있어야 보인다.** `tw-override-check.py` 는
`className` 과 같은 클래스명을 쓰는 규칙만 대조하므로 `.widget-fixed > * { width:100% }` 처럼
**다른 클래스에 걸린 자손·자식 규칙**이 유틸리티를 덮는 것을 못 잡는다(§7-35 에서 실제로 났다 —
API 배지가 상단 전폭으로 늘어났다). → **`~/.cims-scratch/css-vs-tw.mjs`** 를 함께 돌린다.
남은 격자 CSS 규칙 29개를 전 라우트의 실제 DOM 에 대고, CSS 선언값이 계산값과 같은데
같은 속성의 유틸리티가 붙어 있으면(=CSS 가 이겼으면) 보고한다. 격자가 이기는 것이 설계대로인
곳(위젯 안 패널 스크롤)은 스크립트 안 `EXPECTED` 에 적어 둔다.
클래스가 겹치는 것만이 아니라 **같은 CSS 속성을 건드리는 경우**만 잡아 준다. 지울 수 없는
레거시 규칙(예: `grid-area`, 인쇄용 숨김)은 그 속성만 남기고 나머지를 걷는다.

### 8.2 preflight 를 끈 대가 — 직접 넣어야 하는 것들

`corePlugins.preflight: false` 는 Tailwind 의 전역 reset 을 통째로 끈다. 그래서 **유틸리티가
동작하려면 preflight 가 깔아 주던 토대를 직접 넣어야 한다.** 전부 T1-1 에서 실제로 겪은 것이고,
`index.css` 의 `@layer base` 에 들어 있다. **T4 에서 preflight 를 켜면 이 블록을 걷는다.**

| 없으면 | 증상 |
|---|---|
| `*,::before,::after { border-style: solid; border-width: 0 }` | `border-b` 를 줘도 **선이 안 그려진다** — 유틸리티는 width 만 설정하고 style 은 preflight 에 의존한다 |
| `button{ background-color: transparent }` 등 폼 리셋 | 클래스 없는 `<button>` 이 **브라우저 기본 크롬**(배경 `#efefef` · Arial 13.3px)을 쓴다 |

**간격 스케일은 px 로 고정한다.** 기본 스케일은 rem 기반인데 이 앱의 `:root` 는 `font-size: 14px`
(시안 본문 크기)이라 `p-5` 가 20px 이 아니라 **17.5px** 이 된다 — Figma 실측값과 전부 어긋난다.
`tailwind.config.ts` 의 `theme.spacing` 을 `n × 4px` 로 두어 루트 크기와 무관하게 했다.
width·height·gap·inset 이 모두 이 스케일을 쓴다.

### 8.3 화면 확인 — 빌드 통과는 증거가 아니다

T1-1 에서 **빌드도 통과하고 클래스도 생성됐는데 화면에는 하나도 안 먹는** 상태가 나왔다.
원인이 셋이었고 전부 브라우저를 봐야만 보였다:

1. **dev 서버가 `postcss.config.js` 를 못 읽고 있었다** — Vite 는 PostCSS 설정을 **기동 시점에**
   읽는다. 설정을 추가·수정하면 **`cims.sh tb restart console`** 로 재기동해야 한다
2. 레거시 CSS 가 Tailwind 를 덮고 있었다 (§8 의 덮임 검사)
3. preflight 를 끈 대가 (§8.2)

→ 컴포넌트를 옮길 때마다 **스크린샷으로 확인한다.** `~/.cims-scratch/shot.mjs`
(Playwright, 레포 루트 `node_modules` 에 설치):

```
CIMS_USER=admin CIMS_PASS=1234 node ~/.cims-scratch/shot.mjs /deploy/servers out.png
CLIP=0,0,1440,60 …          # 영역만 잘라 Figma 프레임과 대조
node ~/.cims-scratch/probe.mjs   # 계산된 스타일·콘솔 에러 확인
```

**T0a·T0b 는 화면이 안 변하는 게 성공 판정**이고, T0c 부터 화면이 변한다 — 단계마다 판정 기준을
하나만 두어 회귀 원인을 가릴 수 있게 나눴다.

| 단계 | 내용 | 완료 판정 |
|---|---|---|
| **T0a 토큰 개명** | `index.css` 의 라이트/다크 토큰 블록을 Figma `01`(`8:2`/`13:2`) 기준으로 다시 쓰고 §5.1 매핑대로 `var(--...)` 를 일괄 치환(실측 **1,158곳 · 76파일**). C 갈래는 이름·값 그대로. 테마 스위치(`data-theme`)는 안 건드린다 | **라이트** — 5개 색만 변화(+`rgba()`→`rgb()` 표기 2건), 나머지 값 동일. **다크** — 10개 변화, soft 계열 4종이 투명→불투명이라 **눈에 띄는 의도된 변화**(§5.1-B). 실측 라이트 31/38·다크 26/36 동일 |
| **T0b Tailwind 도입** | Tailwind **v3** + Radix + shadcn 설치, `tailwind.config.ts` 이식 — content 에 `../../service/console/src` 포함, `darkMode` 를 `.dark` 가 아니라 우리 스위치 `[data-theme="dark"]` 에 결선, **preflight off**. `@tailwind` 지시문은 `index.css` **맨 앞** — 기존 CSS 가 뒤에 와서 이긴다(이행 중 충돌 시 현행 유지). shadcn 20종을 `src/components/ui/` 에 생성 | 기존 화면 회귀 0 — **JS 번들 불변**(컴포넌트 미임포트라 트리셰이킹) · 유틸리티↔레거시 클래스 충돌 0 |
| **T0c 폰트·아이콘** | ① **Pretendard Variable · JetBrains Mono 자체 호스팅** — `font-family` 에 이름만 있고 로드가 없어 시스템 폰트로 떨어져 있었다. 온프레미스라 CDN 대신 npm(`pretendard` 동적 서브셋 92조각 · `@fontsource-variable/jetbrains-mono`). ② **`DESIGN-RULES` §0 지목 글리프 93줄/28파일**(`🩺 ↻ ⤺ ✎ 🔒 ★ 🔁 ⚡ ▶ ■ ⏹ ❚❚ ▼ ▲ ◀ ⚙`)을 Lucide 로 1:1 치환 — 뜻을 재해석하지 않는다. 아이콘만 남는 버튼엔 `title` 을 넣는다 | **화면은 변하고 기능은 안 변한다.** 지목 글리프 잔존 0 · 빌드 통과 |
| **T1 공통 셸** | AppBar · Sidebar · TreePanel · Tabs + **breadcrumb 줄 신설** (`screens/shell.md`). 메뉴 항목의 그룹 소속은 그대로 둔다 | 전 라우트 셸 회귀 확인 · 위젯 편집 `✎` 슬롯 유지(§7-2) |
| **T2 시스템/인프라** | `/deploy/servers` — `screens/` 스펙이 있는 유일한 본문 화면. S1~S4 · G1~G4 · A1~A4 · SA · 모달 · 빈 상태. 구조 변경(§1 목록)까지 반영 | 스펙 대조 |
| **T3 나머지 30 라우트** | 화면 구조·문구 유지, 인라인 `style` → `className` 교체 + **네이티브 `confirm()` 51곳을 시안 Dialog 로**(§7-7 — 대상·문구는 그대로, 껍데기만) + **글리프 2차분**(§8.1) + **상태 점 → StatusDot** | `index.css` 참조 소거 · `window.confirm` 0건 · 글리프 2차분 0건 |
| **T4 정리** | `index.css` reset 제거 → **preflight 켜기**. 잔여 폐기 (그리드/카드 편집 CSS 제외 — §7-3) | Tailwind 단일 출처 |

`ServersPage.tsx` 는 175KB 단일 파일이지만 **위젯으로 분해하지 않는다.**
[console_platform.md](console_platform.md) §3.1 의 **C 갈래(작업 트랜잭션 — 폼+저장 원자성,
배포·검증·설정 편집)** 이고, 그 판정은 "1위젯 유지"다. 판정 기준은 *하나만 떼어 놓아도 말이 되는가* —
관리 화면은 트리 선택·탭·폼·저장이 한 조작 단위라 떼면 말이 안 된다. 실제로 위젯 합성 페이지 16개는
전부 `운영`(조회·모니터링), 통짜 15개는 전부 `관리`(편집·설정)로 한 건도 안 어긋난다.

T2 는 시안대로 재작성하면서 **파일만** 읽기 좋은 단위로 나눈다 — 라우팅 등록(`component: ServersPage`)과
위젯 플랫폼 관계는 그대로다.

**함정 — dev 서버가 서비스 팩을 어제 것으로 물고 있었다.** `vite.config.ts` 의 `@svc` 는
프로젝트 루트 **밖**(`../../service/console/src`)을 가리킨다. 하루 켜 둔 dev 서버는 그 바깥
파일의 변경을 모듈 그래프에 반영하지 못했고, 그래서 **서비스 팩 12 화면을 몇 커밋 전 코드로
검사하고 「ok」 를 받았다.** 렌더된 DOM 에 T3-11 에서 이미 걷어낸 인라인 `style` 이 남아 있는
것을 보고 알아챘다. → **전 라우트 감사 전에는 `cims.sh tb restart console`.** 캡처 한 장이
현재 소스의 것이라는 보장은 「빌드 통과」가 아니라 **서버를 새로 띄운 시점**이 준다.

### 8.3 인라인 style 을 옮길 때 — 실제로 밟은 함정 셋

T3-11 에서 1,498곳을 유틸리티로 옮겼다. 기계 치환이라 **틀리면 조용히 틀린다** — 아래 셋은
전부 실제로 났고, 세 번째는 verify 51개 검사가 못 잡아 캡처로 잡았다.

1. **`tailwind.config.ts` 의 `spacing`·`borderRadius` 는 재정의된 허용 목록**이다.
   spacing 은 n×4px 이지만 `13`·`15`·`17` 같은 키가 **없고**, radius 는 `sm=6 · md=8 · lg=14`
   다(기본값 아님). 목록에 없는 클래스를 뿜으면 CSS 가 나오지 않아 **스타일이 사라진다** —
   `p-13`, `rounded-[2px]`→`rounded-sm`(6px) 같은 식으로 어긋난다. 없는 값은 임의값(`p-[52px]`)으로.
2. **중첩 식 안의 style 을 바깥 태그로 끌어올리면 안 된다** — `<Tile label={…} value={<span
   style={{…}}>}` 에서 `style` 은 안쪽 `<span>` 것이다. 태그의 속성 텍스트를 훑을 때
   **중괄호 깊이 0 인 것만** 잡는다. (`className` 을 받지 않는 지역 컴포넌트라 `tsc` 가 잡아 줬다.)
3. **className 을 받으면서 자기 인라인 `style` 도 쓰는 컴포넌트에는 걸면 안 된다** —
   `style` 이 클래스를 이기므로 호출부가 className 으로 넘긴 값이 **조용히 죽는다**
   (`OrgTreePanel` 에서 폭 200px 지정이 컴포넌트 기본 150px 로 죽었다). 우리 컴포넌트를
   전수 조사해 그런 곳이 하나뿐임을 확인하고, 기본값을 className 으로 옮겨 twMerge 가
   판정하게 고쳤다. **컴포넌트의 시각 기본값은 `style` 이 아니라 className 에 둔다** —
   그래야 호출부가 덮어쓸 수 있다.
4. **클래스 충돌 정리는 축별로** 해야 한다. `w-`/`min-w-`, `p-`/`px-` 를 한 갈래로 묶어
   「뒤에 온 것만 남기기」를 하면 `h-[34px] w-full`, `px-3.5 pt-3` 같은 **정상 짝이 지워진다.**
   갈래는 **가장 긴 접두사**로 정한다. 충돌 정리 자체는 필요하다 — T3-9 의 `.ts`(`text-sm
   text-muted-foreground`)와 이번에 옮긴 `fontSize`/`color` 가 한 className 에서 겹친다.
   남기는 쪽은 **뒤에 온 것** — 인라인 style 이 클래스를 이기던 동작 그대로다.

### 8.1 글리프 2차분 — T3 로 넘긴 것

`DESIGN-RULES` §0 은 글리프를 **예시로만** 나열했다. 소스 전체를 유니코드 기호로 훑으면 T0c 가
처리한 93줄 외에 **약 200곳**이 더 있다. 페이지 맥락을 봐야 분류가 되므로 그 페이지를 옮기는
T3 에서 함께 정리한다.

| 갈래 | 건수 | 처리 |
|---|---|---|
| `⚠` | 27 | `AlertTriangle` |
| `＋`(전각) | 23 | `Plus` |
| `✕ ✗ ❌` | 28 | `X` |
| `✓ ✅` | 26 | `Check` |
| `▾ ▸`(작은 캐럿) | 23 | `Chevron*` |
| 이모지 40여 종 | ~70 | `🔄 🗑 📋 🔑 💬 🔐 🚩 📄 📦 📡 📁 🔗 🔍 🔧 🎤 📞 👁 📌 ⏸ ⏭ ⏳ …` |
| **상태 원** `● ○ ◐ 🟢 🟡 🔴` | ~25 | **Lucide 가 아니라 StatusDot 컴포넌트** — 톤 매핑(Success/Warning/Danger/Neutral/Info)이 화면별 판단이라 이식과 함께 |

**대상이 아닌 것 — 건드리지 않는다** (약 140곳)

| 갈래 | 건수 | 왜 |
|---|---|---|
| `─ └` 박스 드로잉 | 126 | `FlowPage` 의 시퀀스 다이어그램·트리 그림 문자다. 아이콘이 아니다 |
| `≈ ≠ ≤ −` 수학 기호 | 15 | 문구 안의 값 비교 표현 |
| 산문의 `→` `×N` `…` `«»` | 다수 | `정지 → 복사 → 기동` 은 문장 부호, `×3` 은 재통지 횟수, `«패키지 제어»` 는 인용부호다. 아이콘 자리가 아니다 |

**아이콘이 붙는지는 그림의 「버튼 폭」으로 가른다** — 글 스펙이 `↻ 재적용` 이라고 적어도
도안의 그 버튼이 **52px** 이면 아이콘이 들어갈 자리가 없다(아이콘 14 + 간격 6 = 20px 더 필요).
T5-2 에서 실제로 이 함정을 밟았다 — 글만 보고 `↻` 를 넣었다가 실측으로 되돌렸다.

**T3-2 에서 실제로 처리한 화살표·기호** — 위 「줄마다 판단」의 결과다. 판정 기준은 **버튼·링크의
라벨 자리에 있으면 아이콘, 문장 안이면 부호**.

| 자리 | 옮긴 것 |
|---|---|
| 페이지 이동 | `«` `»` → `ChevronsLeft/Right` · `← 이전` `다음 →` → `ArrowLeft/Right` |
| 순서 변경·추세 | `↑` `↓` → `ArrowUp/Down` |
| 내려받기·이관 | `⤓` → `Download` · `⇢` → `ArrowRight` |
| 제거 | `×` → `X` |
| 추가 | `＋`(전각) 12곳 → `Plus`. 그 라벨을 인용하던 빈 상태 문구·주석도 `[추가]` 로 |
| 재생 | `&#9654;` 2곳 → `Play` — **HTML 엔티티라 T2-18 글리프 검사가 못 잡았다.** 문자만 훑으면 새는 구멍이다 |

**절대 손대지 않는 두 곳**
- `ems/core/console/src/api/services.ts` 의 `/●\s+(\w+)/` — `cims-svc status` **출력을 파싱**하는
  정규식이다. 바꾸면 모듈 상태 표시가 통째로 깨진다
- `GroupConfigCompareView.tsx` 의 `'●●●'` — 비밀번호 마스크 문자열

### 8.4 죽은 CSS 를 걷는 기준

T4-1 에서 `index.css` **1,502 → 812줄**(147규칙)로 줄였다. 기준은 하나 —
**소스가 그 클래스 이름을 한 번도 쓰지 않는 규칙만** 지운다. 판정은 두 콘솔 트리의
`.ts`/`.tsx` 전체에서 낱말 경계 검색으로 하고, 다음은 손대지 않는다:
- 요소 선택자가 섞인 규칙 · `:root`/다크 블록 · `@layer`/`@media` 안쪽
- **위젯 편집·2D 그리드 계열**(`.grid-canvas` `.card-canvas` `.widget-*` `.panel` `.toolbar`
  `.scroll-fill` `.empty`) — `EditableLayout`/`GridEditor`/`CardLayout` 이 아직 쓴다(CLAUDE.md)

지운 것의 대부분은 T3 에서 옮긴 것들(`.btn--*` `.badge--*` `.modal-*` `.tab-btn*`
`.search-input` `.empty-cell` `.actions` `.table-wrap` `.toggle-track` …)과, 콘솔에
남아 있던 **소프트폰(`sp-*` `phone-*`) 스타일 60여 규칙**이다. 소프트폰은 `cims-phone/`
에 자기 `index.css` 가 따로 있어 콘솔 쪽은 옛 복사본이었다.

**정의가 없는 클래스도 함께 잡았다** — `text-muted` 6곳은 CSS 에 정의가 없어 muted 색이
아예 안 먹고 있었고(`text-muted-foreground` 로 정정), `<table className="table">` 5곳은
`data-table` 이 아니라 T3-6 이 놓쳐 **브라우저 기본 표**로 그려지고 있었다(껍데기로 이행).
둘이 사라져 `tailwind.config.ts` 의 `blocklist` 도 필요 없어졌다.

### 8.5 preflight 는 **끈 채로 둔다** — 켜 보고 되돌린 근거

§8 의 이행 계획은 마지막에 preflight 를 켜기로 했다. T4-2 에서 실제로 켜 보고
**되돌렸다.** 켜기 전/후 17개 라우트의 레이아웃 지문(요소별 박스·display·line-height·
여백)을 떠서 비교한 결과다.

**preflight 가 우리에게 주는 것은 두 갈래뿐이고, 그건 이미 손으로 넣어 뒀다** —
전역 `border-style: solid`(없으면 `border-b` 가 선을 안 그린다)와 폼 요소 폰트 상속.
box-sizing·여백 0 은 자체 reset 이 더 넓게 덮고, 제목 크기 리셋은 모든 제목이 이미
명시돼 있어 필요 없다.

**켜면 깨지는 것 둘:**
1. `svg { display: block; vertical-align: middle }` — 아이콘이 블록이 되어 **인라인 흐름의
   텍스트를 다음 줄로 밀어낸다.** flex 컨테이너 안에서는 무해하지만(flex 항목은 어차피
   블록화) 그렇지 않은 곳에서 깨진다. 실측: 대시보드 한 화면의 svg 54개 중 `inline*`
   클래스가 없는 것이 **52개**. 실제로 접힌 것이 AppBar 의 `[✎ 편집]` 버튼인데,
   그 파일이 **`EditableLayout.tsx` — CLAUDE.md 의 「손대지 말 것」이라 고칠 수 없다.**
   preflight 를 켜는 것과 그 제약이 정면으로 부딪힌다.
2. `html { line-height: 1.5 }` — 전역 행간이 붙는다. 시안 Typography 는 크기마다 행간을
   따로 정하고(`text-sm`=1.4 · `text-md`=1.5 …) 그걸 유틸리티가 이미 붙이므로,
   전역값은 유틸리티가 없는 자리에만 영향을 줘 오히려 예측을 흐린다.

그래서 `corePlugins: { preflight: false }` 를 **유지**하고, 대신 preflight 가 주던 두
갈래를 `@layer base` 에 명시해 둔 현행 구조를 정본으로 삼는다. 켜는 것은
**위젯 편집 3파일을 손댈 수 있게 된 뒤**의 과제다.

## 9. 남은 항목

**미결은 없다.** 착수를 막는 결정 사항이 남아 있지 않다.

- **backend 과제 — 관리 store 에 낙관적 동시성이 없다.** `file_store.save()` 는
  last-write-wins 다: `update_time` 을 기록만 하고 **비교하지 않으며**, `_update_group` 도
  기대 버전을 받지 않는다. `lease.assert_writable()` 은 OAM 인스턴스 단위 단일 writer
  보증(oam_ha.md §4.4)이지 요청 단위 충돌 감지가 아니다. 두 사람이 같은 그룹을 동시에 열면
  나중 저장이 먼저 것을 조용히 덮는다 — **지금도 그렇다.** 통합 저장(§7-10)이 만드는 위험이
  아니라 이미 있는 위험이고, 필드 단위 dirty 로 사정거리를 좁혀 두었다. 제대로 막으려면
  `file_store` 에 기대 `update_time` 비교를 넣고 전 도메인 핸들러가 태워야 한다 —
  **디자인 리팩토링 범위 밖의 backend 과제**로 분리한다
- **미저장 상태로 선택을 바꿀 때 경고할지** — 통합 저장이 되면서 "변경을 들고 있다가 저장"
  흐름이 생긴다. 시안에 규정이 없어 우리가 정해야 한다

- **도안이 `저장 (0 변경)` 을 활성으로 그렸지만 비활성으로 둔다** — S3(459:7238)·G3-1 둘 다
  변경 0건인 상태의 저장 버튼을 primary 채움으로 그렸다. 저장할 것이 없을 때 눌리는 것은
  **동작**이고 「기능은 건드리지 않는다」가 우선이다. 비활성 모양은 계약대로 불투명도가 아니라
  `neutral-soft` 채움 + `text-disabled` 다
- **G3-1 저장바 버튼에는 아이콘이 없다** — S3 인스턴스에는 `←`·`✓` 가 있고 컴포넌트 정의
  (459:7237)도 둘을 갖는다. 인스턴스 하나가 아이콘을 지운 것으로 보아 **컴포넌트 정의**를 따랐다

- ~~**「패키지 설정」의 숫자 두 곳**~~ — **정해졌다(T5-1).** 탭 카운트는 그 탭에서 다루는
  **큰 항목 = 모듈 수**이고, 스코프마다 따로 센다(서버는 서버 스코프 설정이 있는 모듈,
  그룹은 그룹 공통 설정이 있는 모듈 종류). 도안의 `패키지 설정 9` 는 정의가 아니라 **예시
  숫자**였다 — S3(서버 개별 설정 4필드)·G3-1(9필드)·G3-3(oam-svc 16필드)이 **전부 똑같이 9**
  로 그려져 화면 내용과 무관하다. 모듈 칩은 종전대로 그 화면에 나오는 필드 수다.
  아래 옛 서술은 근거로 남긴다:

- **(근거) 시안의 숫자가 갈린 지점** — 탭 카운트와 모듈 칩 카운트의 정의가 시안에서 갈린다.
  탭은 `Sec/Tabs (신규)`(458:9160)가 `hasCount` 로 켜고 G1 그림(458:8626)이
  `패키지 설치 3 · 패키지 설정 9 · 패키지 제어 3` 을 보인다 — 설치·제어는 **모듈 수**라 붙였고,
  설정의 `9` 는 정의가 불분명해 **비워 두었다**. 모듈 칩(S3 266:4697)은 `oam 9 · oam-svc 6 ·
  csc 4` 인데 실제 템플릿은 oam 이 **서버 4 · 그룹 9 · 합 13**(시안 S3/G3-1/SA3 의 4/9/13 과
  정확히 일치)이고 oam-svc·csc 는 6·4 와 전혀 다르다 — 그림의 숫자가 예시값이다.
  그래서 칩은 **이 화면에 실제로 나오는 필드 수**(스코프 필터 결과)로 구현했다. 디자이너 확인이
  오면 탭 카운트도 같은 정의로 붙인다 — 틀린 수를 띄우지 않으려고 미룬 것이다

- **일부러 남긴 hex 다섯 갈래** — §3-1 은 hex 금지지만 아래는 **테마 토큰을 쓰면 틀린다.**
  ① `FlowPage.tsx`·`VolteHistoryPage.tsx` 의 **시퀀스 다이어그램 팔레트만** — 프로토콜 계열
  (`PROTO_COLOR`·`SIP_METHOD_COLOR`·`SIP_FAIL_COLOR`·`NODE_COLOR`·발신/착신색·TX/RX)이다.
  `--chart-*` 12색으로도 모자라고 선 종류·화살표까지 색으로 구분한다. 옮길 때는 팔레트를
  먼저 토큰으로 정의한다. **T3-1 에서 이 두 파일을 파일 단위로 제외한 것은 과했다** —
  테두리·본문색·선택 강조 같은 크롬 hex 24곳이 함께 남아 다크에서 밝은 선으로 보였다.
  T3-12 에서 그것만 토큰으로 옮겼다. 제외는 **파일 단위가 아니라 그 색이 계열색인지**로 가른다. ② `VerificationPrintReport.tsx`(35개)와
  두 검증 페이지의 `@media print` 블록 — 흰 종이에 찍으므로 다크 토큰이 상속되면 흐려진다.
  ③ `SegmentPlayer` 의 영상 무대 `#000` — 테마 표면이 아니라 레터박스다.
  나머지 전 라우트는 토큰만 쓴다

- **시트 2 에 없는 컴포넌트는 네이티브로 둔다** — DatePicker(날짜 입력 12곳)·Slider(1곳).
  `Sec/Select` 설명이 "날짜입력은 DatePicker 로 교체" 라고 적었지만 `02 Components` 에도
  핸드오프 shadcn 설치 목록에도 그 컴포넌트가 없다. **목록에 없는 컴포넌트를 새로 만들지
  않는다**(CLAUDE.md)는 규칙이 우선이고, 사용자도 그대로 두라고 확정했다. 껍데기는 시안
  계약(높이 33·포커스 링)을 따르되 달력·슬라이더 조작부는 브라우저 것이다
- **textarea 도 목록에 없다** — 패키징 화면의 설정 편집기 한 곳뿐이라 컴포넌트를 새로 만들지
  않고(§CLAUDE.md「02 목록에 없는 컴포넌트를 새로 만들지 않는다」) 테두리·포커스 링만
  유틸리티로 맞췄다

- **`.panel` · `.toolbar` · `.scroll-fill` 은 클래스 이름을 남긴다** — 시안 `02 Components`
  에 Panel/Card 가 없어 옮길 컴포넌트가 없고, 무엇보다 **위젯 격자 CSS 가 이 이름을 훅으로
  잡는다** — `.widget-fixed > .panel`, `.widget-stack > .toolbar`, `.card-block > .panel` 이
  위젯 한 칸 안의 채우기·스크롤을 정한다(레이아웃 = 기능). 이름을 걷으면 배치가 깨진다.
  대신 **시각 속성은 시트 2 계약에 맞췄다**(T4-3) — `.panel` 의 `box-shadow` 를 뺐다.
  도안 표면에 그림자가 없다(T2 실측). `--cims-elevation-sm` 토큰은 떠 있는 것이 쓴다.
  이름을 없애는 것은 격자 CSS 를 다시 쓸 수 있게 된 뒤의 과제다

- **디자이너에게 회신할 것** — `modals.md` 가 "확인 대화상자 문구는 확인 후 작성" 으로 열어둔 자리에
  현행 **55곳**의 대상 액션과 문구를 넘기면 시안이 완성된다 (§7-7). 껍데기는 이미 시안 Dialog
  (`components/custom/confirm.tsx`)로 옮겼고 **문구는 현행 그대로**다 — 제목만 액션 이름으로
  새로 달았다(Dialog 에 제목 슬롯이 있어서). 회신이 오면 문구만 갈아 끼우면 된다.
  `prompt()`(서버 이름 변경) 한 곳은 시안에 대응 컴포넌트가 없어 네이티브로 남아 있다.
