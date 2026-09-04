# 콘솔 디자인 시스템

CIMS 웹 콘솔(`ems/core/console` + `ems/service/console`)의 **시각 계약 정본**이다.
색·타이포·간격·컴포넌트 배리언트·상태 표현 규칙은 여기와 여기가 가리키는 핸드오프 패키지가
정본이고, 화면 기능·라우팅·위젯 플랫폼 구조는 [console_platform.md](console_platform.md) 가 정본이다.

## 0. 세 출처의 관계

| 출처 | 무엇의 정본인가 | 다루는 방식 |
|---|---|---|
| `cims-design-handoff/` | 디자이너 원본 — 토큰 값·컴포넌트 대응표·화면 스펙 | **읽기 전용.** 손대지 않는다. v2 를 받으면 폴더째 교체 |
| **이 문서** | 우리 쪽 **적용 범위·스택 결정·충돌 판정·이행 계획** | 핸드오프에 없거나 핸드오프가 틀린 것을 여기서 정한다 |
| 콘솔 소스 | 현재 동작 | 문서와 어긋나면 코드가 사실, 차이는 문서 갱신으로 해소 |

**충돌 판정 순서** — ① 이 문서의 §7 판정 → ② 핸드오프 → ③ 현재 코드.
핸드오프는 웹 실측(2026-09-02) 기반이라 그 뒤 바뀐 코드를 모른다. 핸드오프가 현재 코드와
어긋나면 **임의로 코드를 지우지 말고** §7 에 판정이 있는지 먼저 본다. 없으면 사용자에게 묻는다.

## 1. 적용 범위

**규칙(§2~§5)은 콘솔 전 화면에 적용한다.** core 25 · service 12 = 페이지 37개와 공통 셸 전부.
"시스템/인프라만" 이 아니다 — 핸드오프 `DESIGN-RULES.md` 첫 줄의 "이 저장소에서 UI 를 만들 때
매번" 이 의도한 범위 그대로다.

**단, 화면 스펙(`screens/`)은 `/deploy/servers`(시스템/인프라) 것만 존재한다.**

| | 대상 | 근거 |
|---|---|---|
| 규칙 (토큰·컴포넌트 계약·절대 규칙) | 전 37 페이지 + 공통 셸 | 이 문서 §2~§5 |
| 화면 구조·문구 스펙 | `/deploy/servers` (S/G/A/SA·모달·빈 상태) | `cims-design-handoff/screens/` |
| 나머지 36 페이지 | **화면 구조·문구는 현행 유지**, 규칙만 갈아끼운다 | 스펙 없음 |

스펙 없는 화면의 레이아웃·문구를 **지어내지 않는다.** 규칙 적용만으로 답이 안 나오면 물어본다.

## 2. 스택 (결정)

- **Tailwind + shadcn/ui + Radix 로 전면 이행한다.** Mantine 은 폐기. 두 체계를 섞지 않는다.
- **`ems/core/console/src/index.css` (1463줄) 는 폐기 대상**이다. 현재 이 파일이 37개 페이지의
  유일한 스타일 출처이고 `--bg` `--surface` `--text` 같은 자체 토큰 체계를 갖고 있다.
  핸드오프 `tokens/globals.css` 는 **값은 같고 이름 체계가 다르다**(`--bg`↔`--background`,
  `--sidebar-bg`↔`--sidebar`). 그래서 통째 교체가 아니라 §8 의 단계 이행으로 걷어낸다.
- 아이콘은 **Lucide 만** (`lucide-react` 는 이미 의존성에 있다). 이모지·텍스트 글리프 금지.
- 폰트는 Pretendard Variable(본문) · JetBrains Mono(IP·경로·버전) 두 종
  (`cims-design-handoff/tokens/fonts.md` 의 타입 스케일 포함).

**빌드 쪽 파급** — 디자인 문서가 아니라 빌드 계약이라 여기 남긴다.
- Tailwind content 스캔 경로에 **`ems/service/console/src` 를 반드시 포함**한다. 서비스 팩은
  자체 `package.json`·`node_modules` 없이 core 것을 심볼릭 링크로 공유한다
  (`ems/core/console/scripts/ensure-svc-modules.mjs`). 빠뜨리면 서비스 팩 12개 페이지의
  클래스가 전부 purge 된다.
- `make dist` 콘솔 번들은 vite 산출물을 그대로 담으므로 패키징 변경은 없다.
- preflight(전역 reset) 를 켜는 시점 = 기존 `index.css` 의 reset 을 걷어내는 시점이다. 둘을
  동시에 켜두면 버튼·표 높이가 어긋난다 (§8 T0).

## 3. 절대 규칙

핸드오프 `DESIGN-RULES.md` §1 이 정본. 요약:

1. **hex 금지.** `#4f46e5` 대신 `bg-primary`. 새 색이 필요하면 토큰을 먼저 추가하고 이유를 남긴다.
2. **포커스 링은 모든 인터랙티브 요소에.** `globals.css` 의 `:focus-visible` 전역 규칙을 지우지 않는다.
3. **표 헤더는 세로로 줄바꿈되지 않게.** 헤더 셀 `whitespace-nowrap`, 값 셀은 `break-all` 대신 `min-w-*`.
4. **빈 셀은 `—`** (em dash) + `muted-foreground`. 빈 문자열·`null` 노출 금지.
5. **IP · CIDR · 경로 · 버전 · 마스크는 `font-mono`.**
6. **모듈 이름은 소문자 고정** — `oam` `oam-svc` `csc` `csp` `cmp` `cmdp`.
7. **0건에 경고색을 쓰지 않는다.** `드리프트 0` 은 Neutral.

## 4. 컴포넌트 계약

정본은 핸드오프 `components/contracts.md` + `DESIGN-RULES.md` §2, 대응표는 `components/MAPPING.md`.
Figma 27종 중 18종은 shadcn 그대로, 9종은 `components/custom/` 의 참조 구현을 이식한다.

자주 틀리는 것만:
- **Button** — Primary(`default`)는 화면당 1개. 파괴적 액션은 행 단위 `outline`,
  그룹/전체 단위에서만 `destructive`. 비활성은 `disabled` 속성 + **사유 병기**(불투명도 금지).
- **Badge vs StatusDot** — 값·분류는 Badge, 살아있는 상태(running/stopped/online)는 StatusDot.
- **StatusDot 톤(고정)** — `Success`=online·running·mounted / `Info`=approved(등록됐으나
  heartbeat 없음) / `Neutral`=stopped·미설정 / `Warning`=드리프트·미적용(0건이면 Neutral) /
  `Danger`=offline·unreachable·critical.
- **Switch** — 즉시 적용되는 on/off 전용. **저장 버튼 뒤의 폼 값에 쓰지 않는다.** 선택지는 ToggleGroup.
- **Alert / Toast** — 화면당 1개 원칙. Toast 는 **같은 원인을 한 장으로 묶고 건수는 `×N`**,
  화면 최대 3장, 헤더 알람 배지와 같은 소스를 본다.
- **Table** — 세로 칼럼 구분선 없음. 행 전체 배경 tint 금지.
- **Collapsible** — 중첩 2단까지. **접힌 섹션에 저장 버튼을 노출하지 않는다.**

## 5. 토큰

`cims-design-handoff/tokens/globals.css` 가 유일한 출처다. 두 층으로 되어 있다.

- shadcn 표준 변수(`--primary` `--background` `--border` `--ring` `--sidebar-*` …) — shadcn
  컴포넌트가 그대로 집어간다.
- `--cims-*` 도메인 변수 — shadcn 에 없는 의미색(성공/경고/정보/중립 등).

컴포넌트 파일에 hex 를 쓰지 않는다. 차트 계열색은 "무엇인가"를 뜻하므로 상태색
(success/warning/danger)을 계열에 쓰지 않는다.

## 6. 핸드오프 자료 맵

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

`screens/FIGMA-LINKS.md` 의 링크는 **사람이 연다.** 시각 확인이 필요하면 해당 링크를 사용자에게
제시하고 답을 기다린다 — 모르면 지어내지 않는다.

## 7. 충돌 판정 — 핸드오프 vs 현재 코드

핸드오프는 2026-09-02 웹 실측 기준이라 아래를 모른다. **이 판정이 핸드오프보다 우선한다.**

| # | 핸드오프 기술 | 실제 | 판정 |
|---|---|---|---|
| 1 | "`tokens/globals.css` 로 `src/globals.css` 교체" | `globals.css` 는 없다. `src/index.css` 1463줄이 37개 페이지의 유일한 출처 | 통째 교체 **금지**. §8 단계 이행으로 걷어낸다 |
| 2 | `decisions.md` §2 — 위젯 편집 모드 미채택(미결) | **살아있는 기능이고 개발 중.** `widgets/EditableLayout.tsx` · `GridEditor.tsx` · `CardLayout.tsx`, 카드 안 배치 편집까지 있다 | **존치.** 디자이너가 몰라서 뺀 것 — 제거하지 않는다. 재배치(전역 헤더 → breadcrumb 줄)는 §9 미결 |
| 3 | `index.css` 를 전부 걷어내는 것으로 읽힘 | 위젯 편집·2D 그리드 CSS(`.grid-canvas` `.card-canvas` 계열)는 스타일이 아니라 **플랫폼 인프라** | 그리드/카드 편집 CSS 는 **보존**. Tailwind 로 옮기더라도 동작 동일성 우선 |
| 4 | `screens/shell.md` — 셸 재설계 | 셸(`components/Header.tsx` · `Sidebar.tsx`)은 37개 페이지 공유 | 셸도 **범위에 포함**(사용자 결정). 셸 변경은 전 페이지 회귀 확인 대상 |
| 5 | "Figma 링크를 사람에게 열어보게 하세요" | Claude 는 Figma 를 못 연다 | 링크를 **사용자에게 제시하고 묻는다** |
| 6 | 실측 대상 `121.161.164.140:4419` 등 | 운영/개발 서버 | 실서버 배포·화면 검증은 **사용자 담당** |

## 8. 이행 계획

한 번에 못 바꾼다. 이행 기간에는 `index.css` 와 Tailwind 가 **의도적으로 병존**한다.

| 단계 | 내용 | 완료 판정 |
|---|---|---|
| **T0 기반** | Tailwind·Radix·shadcn 설치, `tokens/globals.css`·`tailwind.config.ts` 이식, 폰트 로드, content 스캔에 `ems/service/console/src` 포함, preflight 정책 확정 | 기존 화면이 시각적으로 그대로 (회귀 0) |
| **T1 공통 셸** | AppBar · Sidebar · TreePanel · Tabs (`screens/shell.md`) | 37개 페이지 셸 회귀 확인 |
| **T2 시스템/인프라** | `/deploy/servers` — `screens/` 스펙이 있는 유일한 화면. S1~S4 · G1~G4 · A1~A4 · SA · 모달 · 빈 상태 | 스펙 대조 |
| **T3 나머지 36 페이지** | 화면 구조·문구 유지, 토큰·컴포넌트만 교체 | `index.css` 참조 소거 |
| **T4 정리** | `index.css` 잔여 폐기 (그리드/카드 편집 CSS 제외 — §7-3) | Tailwind 단일 출처 |

`ServersPage.tsx` 는 175KB 단일 파일이다. T2 는 [console_platform.md](console_platform.md) 의
위젯 분해 작업과 겹치므로 **분해와 스타일 교체를 같은 변경에 섞지 않는다** — 순서를 먼저 정한다.

## 9. 미결 — 시작 전 결정 필요

- **위젯 편집 진입점 위치** — 존치는 확정(§7-2). 전역 헤더 ✎ 를 유지할지, `decisions.md` 제안대로
  breadcrumb 줄 오른쪽 페이지 액션으로 옮길지.
- **그룹 화면 저장 방식** — 섹션별 적용 vs 하단 통합 저장 (`decisions.md` 마지막 절).
- **확인 대화상자 유무** — 디자이너가 운영 서버라 실제로 눌러보지 못한 구간 (`screens/modals.md` 하단).
- **T2 와 위젯 분해의 순서** — 어느 쪽을 먼저 할지.
- `decisions.md` §7 의 제안 3건(저장바 되돌리기 · 탭 카운트 배지 · G4 동시 정지 경고) 채택 여부.
