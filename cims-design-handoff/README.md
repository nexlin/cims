# CIMS 콘솔 — 디자인 핸드오프 패키지

Figma 원본을 열 수 없는 환경을 전제로 만든 패키지입니다.
**이 폴더를 저장소 루트에 통째로 넣고**, Claude 에게 `DESIGN-RULES.md 읽고 시작해` 라고 하면 됩니다.

> **CIMS 리포 편입 상태** — 이 폴더는 **디자이너 원본 그대로** 보존한다 (v2 를 받으면 폴더째 교체).
> 원본의 `CLAUDE.md` 는 리포 루트 `CLAUDE.md` 와 충돌하지 않도록 **`DESIGN-RULES.md` 로만 개명**했고
> 내용은 손대지 않았다. 우리 쪽 적용 범위·스택 결정·충돌 판정·이행 계획은 리포 정본 문서
> [`docs/design/console_design_system.md`](../docs/design/console_design_system.md) 가 갖는다 — **그쪽을 먼저 읽는다.**

작성: 2026-09-02 · 근거: Figma `CIMS 콘솔 재설계` + 웹 실측(`121.161.164.140:4419`, `121.161.164.45:4419`)

---

## 5분 요약

1. **스택이 바뀝니다** — shadcn/ui + Tailwind + Radix. 이전에 논의된 **Mantine 계획은 폐기**입니다.
2. **색은 전부 `tokens/globals.css`** 에 있습니다. shadcn 표준 변수(`--primary` 등)에 CIMS 값이 들어가 있고, shadcn 에 없는 의미색은 `--cims-*` 로 따로 있습니다.
3. **컴포넌트 27종 중 18종은 shadcn 그대로**, 9종만 직접 만듭니다 (`components/custom/` 에 참조 구현 있음).
4. **화면은 `screens/`** 에 구조·문구·상태까지 적어뒀습니다. 문구는 웹 실측 원문입니다.
   실물이 궁금하면 **`screens/FIGMA-LINKS.md`** 의 링크를 여세요 — 피그마 유료 좌석 없이 브라우저에서 봅니다.
5. **`decisions.md` 를 리뷰 전에 읽으세요** — 웹과 다르게 그린 곳과 이유가 있습니다. 안 읽으면 "원래대로" 로 되돌아갑니다.

## 파일

```
DESIGN-RULES.md           ★ 매 작업마다 지켜야 하는 규칙. Claude 에게 이걸 먼저 읽히세요
tokens/
  globals.css             색·타이포·그림자 (유일한 출처)
  tailwind.config.ts      토큰 → 유틸리티 매핑
  fonts.md                Pretendard Variable · JetBrains Mono + 타입 스케일
components/
  MAPPING.md              Figma 27종 → shadcn 대응표 + 설치 명령
  contracts.md            배리언트 목록과 "언제 무엇을 쓰는가"
  custom/                 shadcn 에 없는 9종 참조 구현 (.tsx)
screens/
  INDEX.md                화면 목록과 스코프 구조
  FIGMA-LINKS.md          ★ 화면별 Figma 바로가기 (무료 계정으로 열립니다)
  shell.md                AppBar · Sidebar · TreePanel · Tabs · 레이아웃 규칙
  server-scope.md         S1~S4
  as-group.md             G1 · G2 · G3-* · G4
  aa-group.md             A1~A4 (AS 와 다른 점만)
  sa-scope.md             SA1 · SA3 (단일 서버)
  modals.md               M3 시스템 추가 외
  empty-states.md         빈 상태 5종
decisions.md              웹과 다르게 그린 곳과 이유
```

## 작업 순서

```bash
npx shadcn@latest init
# tokens/globals.css 로 src/globals.css 교체, tailwind.config.ts 반영
npx shadcn@latest add button badge input label select checkbox radio-group switch \
  toggle-group tabs alert dialog sonner dropdown-menu collapsible table tooltip separator scroll-area
# badge / alert 는 variant 확장 필요 → components/custom/*-variants.ts
# components/custom/ 9종 이식
# 화면은 screens/shell.md → server-scope.md 순서로
```

## 아직 정해지지 않은 것

`decisions.md` 마지막 절과 `screens/modals.md` 하단을 보세요. 특히:
- 위젯 편집 모드(페이지를 위젯으로 조립하는 기능) 재도입 여부
- 그룹 화면의 저장 방식 — 섹션별 적용 vs 하단 통합 저장
- 확인 대화상자 유무 (운영 서버라 실제로 눌러보지 못함)

이 세 가지는 **디자이너에게 물어보고 시작**하세요.
