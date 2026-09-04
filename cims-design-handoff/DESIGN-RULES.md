# CIMS Console — 디자인 시스템 규칙

이 파일은 **이 저장소에서 UI 를 만들 때 매번 지켜야 하는 계약**입니다.
화면을 새로 만들거나 컴포넌트를 고칠 때 먼저 읽으세요.

출처: Figma `CIMS 콘솔 재설계` · 웹 실측 2026-09-02 (`121.161.164.140:4419`)

---

## 0. 스택

- **shadcn/ui + Tailwind + Radix** 로 갑니다. 이전 계획에 있던 **Mantine 은 폐기**입니다. 두 라이브러리를 섞지 마세요.
- 색·간격·radius·그림자는 **전부 `tokens/globals.css` 의 CSS 변수**에서 옵니다. 컴포넌트 파일에 hex 를 쓰지 마세요.
- 아이콘은 **Lucide 만** 씁니다. 이모지(`🩺 ↻ ⤺ ✎ 🔒 ★ 🔁 ⚡ ▶ ■`)와 텍스트 글리프는 **금지**입니다 — 현재 웹의 가장 큰 시각적 결함입니다.
- 아이콘은 `width`/`height` 만 지정하고 `stroke-width` 는 2 고정. SVG 를 손으로 다시 그리지 마세요.

## 1. 절대 규칙

1. **hex 금지.** `#4f46e5` 대신 `bg-primary`. 새 색이 필요하면 토큰을 먼저 추가하고 이유를 남기세요.
2. **포커스 링은 모든 인터랙티브 요소에.** `globals.css` 에 `:focus-visible` 전역 규칙이 있습니다. 지우지 마세요. (현재 웹에는 포커스 표시가 없습니다.)
3. **표 헤더는 절대 세로로 줄바꿈되지 않게.** 현재 웹은 `액션` 이 `액`/`션` 으로 쪼개지고 NFS 경로가 6줄로 분해됩니다. 헤더 셀에 `whitespace-nowrap`, 값 셀에 `break-all` 대신 `min-w-*` 로 폭을 확보하세요.
4. **빈 셀은 `—`** (em dash), `muted-foreground`. 빈 문자열이나 `null` 을 그대로 두지 마세요.
5. **IP · CIDR · 경로 · 버전 · 마스크는 `font-mono`.**
6. **모듈 이름 표기는 소문자 고정** (`oam` `oam-svc` `csc` `csp` `cmp` `cmdp`). 현재 웹은 같은 화면에서 `oam` / `OAM-SVC` / `CSC` 가 섞이고, 같은 그룹의 두 멤버가 서로 다르게 나옵니다.
7. **0건에 경고색을 쓰지 마세요.** `드리프트 0` 은 Neutral 입니다. 현재 웹은 0건에도 노란 ⚠ 를 띄웁니다.

## 2. 컴포넌트 사용 계약

- **Button** — Primary(`default`) 는 **화면당 1개**. 파괴적 액션은 **행 단위에서 `outline`**, 그룹/전체 단위에서만 `destructive`. 비활성은 `disabled` 속성 + `text-disabled` 토큰(불투명도 금지) + **사유를 반드시 함께 표시**.
- **Badge vs StatusDot** — 값·분류는 Badge, 살아있는 상태(running/stopped/online)는 StatusDot.
- **StatusDot 톤 매핑 (고정)** — `Success`=online·running·mounted / `Info`=**approved**(등록·승인됐으나 heartbeat 없음) / `Neutral`=stopped·미설정 / `Warning`=드리프트·미적용(0건이면 Neutral) / `Danger`=offline·unreachable·critical.
- **Switch** — 즉시 적용되는 on/off 에만. 라이트/다크처럼 선택지는 ToggleGroup. **저장 버튼 뒤에 있는 폼 값에는 절대 쓰지 마세요.**
- **Alert(SectionMessage)** — Info=설명 / Warning=주의(진행 가능) / Danger=서비스 영향. **화면당 1개** 원칙.
- **Collapsible 섹션** — Level 1(굵은 라벨·굵은 화살표) / Level 2(중간 굵기·얇은 화살표). 중첩은 2단까지. **접힌 섹션에 저장 버튼을 노출하지 마세요** (현재 웹의 `절체 조건` 이 그렇습니다).
- **Table** — 세로 칼럼 구분선 없음. 외곽 `border-strong` > 내부 rule `border`. 행 전체 배경 tint 는 쓰지 마세요(배지와 셀 표시로 충분).
- **Dialog(Modal)** — 확인 대화상자와 생성 폼의 공통 셸. 되돌릴 수 없는 액션만 `destructive` 확인 버튼.
- **Sonner(Toast)** — **같은 원인은 한 장으로 묶고 건수는 `×N`**. 화면에 최대 3장. 현재 웹은 노드 수만큼(6장) 쌓입니다. 헤더 알람 배지와 **같은 소스**를 봐야 합니다.

## 3. 도메인 규칙 (UI 가 지켜야 하는 업무 규칙)

- **스코프에 따라 필드가 이동합니다.** 서버가 AS/AA 그룹 멤버면 서버 화면의 `패키지 설정` 은 4필드(bind IP · OAM bind 포트 · OAM 역할 + Infrastructure)이고, 나머지 **공통 9필드는 그룹 화면**에 있습니다. **SA(단일 서버)면 그룹이 없으므로 13필드가 전부 서버 화면**에 나옵니다.
- **AS 그룹 멤버는 단독 삭제 불가** — 삭제 버튼은 비활성 + 사유(`그룹 삭제로만 가능`).
- **AA(all_active) 그룹**은 절체 개념이 없습니다 — `절체 조건` 섹션 없음, `auth_pass` 없음, 멤버 표 4컬럼, `점검`·`복귀` 없음, 일괄 제어 3버튼(수동 절체 없음). 멤버 수가 가변이라 **트리에서 `+`/`×` 로 증감**합니다.
- **저장 ≠ 이관.** 관리 store 경로는 저장으로 바뀌지 않습니다. `이관`(정지→복사→기동)을 써야 하고, **이관은 HA 그룹 멤버에서만** 가능합니다(SA 는 비활성).
- **모듈이 배포되지 않은 그룹**은 `패키지 설정` 탭 본문이 통째로 빈 상태 한 줄로 대체됩니다.

## 4. 파일 구조

```
tokens/globals.css        색·타이포·그림자 (여기가 유일한 출처)
tokens/tailwind.config.ts 토큰 → 유틸리티 매핑
components/MAPPING.md     Figma 컴포넌트 → shadcn 대응표
components/contracts.md   variant · state 목록
components/custom/        shadcn 에 없는 9종 참조 구현
screens/                  화면별 구조·문구 스펙
screens/FIGMA-LINKS.md    화면별 Figma 바로가기 (view-only, 무료 계정으로 열림)
decisions.md              왜 이렇게 바꿨는지 (되돌아가기 방지)
```

## 5. 작업 순서 권장

1. `tokens/globals.css` + `tailwind.config.ts` 적용, Pretendard·JetBrains Mono 로드
2. `npx shadcn@latest add` 로 MAPPING.md 의 1:1 컴포넌트부터
3. `components/custom/` 의 9종 이식
4. 화면은 `screens/` 순서대로 — 공통 셸(AppBar · Sidebar · TreePanel · Tabs) 먼저

**모르면 지어내지 말고 물어보세요.** 이 문서에 없는 화면·상태는 실물 확인이 안 된 것입니다.
시각적으로 확인이 필요하면 `screens/FIGMA-LINKS.md` 의 해당 화면 링크를 사람에게 열어보게 하세요.
