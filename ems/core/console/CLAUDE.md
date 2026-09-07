# 콘솔 소스 — 작업 전 확인

**시각 계약 정본 = [docs/design/console_design_system.md](../../../docs/design/console_design_system.md).**
UI 를 만지기 전에 읽는다. 화면 기능·라우팅·위젯 플랫폼 구조는
[docs/design/console_platform.md](../../../docs/design/console_platform.md) 가 정본이다.

**적용은 세 층** (정본 문서 §1) — ①**방식**(하드코딩 CSS·인라인 `style` 을 걷고 shadcn 컴포넌트를
임포트해 쓴다)과 ②**규칙**(절대 규칙·컴포넌트 계약·토큰)은 **전 라우트 + 공통 셸**에 적용한다.
그 페이지의 도안이 없어도 적용한다. ③**화면별 구체 값**(섹션 편성·필드 배치·문구·컬럼 폭)은
도안이 있는 `/deploy/servers`·공통 셸에만.

**기능은 건드리지 않는다** — 메뉴 소속·라우팅·동작·데이터. **시안에 없으면 지우지도 고치지도
않는다**(일관성·정돈을 이유로도). 부딪히면 정본 문서 §7 판정, 거기에도 없으면 **묻는다.**

- **스택은 Tailwind + shadcn/ui + Radix 로 이행 중**이다. Mantine 금지, 두 체계 혼용 금지.
  `src/index.css` 는 폐기 대상이지만 아직 전 라우트가 여기에 의존한다 — 통째로 걷어내지 않는다.
- **손대지 말 것**: `src/index.css` 의 위젯 편집·2D 그리드 CSS(`.grid-canvas` `.card-canvas` 계열)와
  `src/widgets/EditableLayout.tsx` · `GridEditor.tsx` · `CardLayout.tsx`. 시안에 없는 것은
  **디자이너 누락으로 확인됐다** — 페이지 편집·카드 안 편집·AppBar 의 `✎` 슬롯 모두 현행 유지
  (정본 문서 §7 의 #1·#2·#3).
- **hex 직접 사용 금지**(토큰만) · **이모지/텍스트 글리프 아이콘 금지**(Lucide 만).
- 화면 스펙은 **공통 셸(`screens/shell.md`)과 `/deploy/servers`** 것만 있다 — 이 둘은 **구조까지**
  시안을 따른다. **나머지 30개 라우트는 화면 구성·문구를 그대로 두고 방식·규칙만 갈아끼운다.**
  스펙 없는 화면을 지어내지 않는다.
- 서비스 팩(`ems/service/console/src`)은 자체 `node_modules` 없이 여기 것을 링크로 공유한다
  (`scripts/ensure-svc-modules.mjs`). Tailwind content 스캔 경로에 반드시 포함한다.
- **디자인 뼈대는 Figma 두 페이지다** — 파일 키 `3nHGkPN8XAaHQxPNGn608v`,
  **`01 Design style sheet`**(토큰 — Light `8:2` / Dark `13:2`)와 **`02 Components`**(`15:2`,
  컴포넌트 27종). 도안이 없는 페이지도 **이 둘에 맞춰 만든다.** `02` 목록에 없는 컴포넌트를 새로
  만들지 않는다. `03 Screens` 는 시스템/인프라 화면 값이라 그 화면에만.
  MCP 커넥터 `claude.ai Figma` 로 직접 읽는다(Full/Dev 좌석 필요 — 정본 문서 §6.1). 글
  스펙(`screens/*.md`)과 그림이 어긋나면 **그림이 맞다.** 도구가 안 보이면 `/mcp` 인증을 요청한다.
- 실서버 배포·화면 검증은 사용자가 한다.
