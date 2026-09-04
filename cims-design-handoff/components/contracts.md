# 컴포넌트 계약서

Figma Dev Mode 로는 알 수 없는 것들 — 배리언트 목록과 **언제 무엇을 쓰는가**.
치수·간격은 `tokens/` 와 화면 스펙을 보세요.

## Button — 4 variant × 2 size × 2 state

| variant | shadcn | 쓰는 곳 |
|---|---|---|
| Primary | `default` | **화면당 1개.** 저장, 생성 |
| Secondary | `outline` | 대부분의 액션. 행 단위 파괴적 액션도 여기 |
| Ghost | `ghost` | 보조. 되돌리기, 새로고침 |
| Danger | `destructive` | **그룹/전체 단위 파괴적 액션만** (시스템 삭제, 일괄 중지) |

- size: `sm` 26px / `md` 36px
- **Disabled**: 불투명도 금지. 배경 `neutral-soft`(solid 계열) 또는 배경 유지(outline), 라벨·아이콘 `text-disabled`. **사유를 반드시 함께 표시** — 툴팁이 아니라 눈에 보이게.

## Badge — 6 tone × 2 style

Soft 가 기본. Solid 는 개수·심각도처럼 눈에 띄어야 할 때만. `badge-variants.ts` 참조.

## StatusDot — 5 tone

톤 매핑 고정: `Success`=online·running·mounted / `Info`=**approved** / `Neutral`=stopped·미설정 / `Warning`=드리프트·미적용(0건이면 Neutral) / `Danger`=offline·unreachable·critical

## TextInput / Select — 4 state

Default · Focus · Error · Disabled. 라벨 + 필수(`*`) + 값 + 헬프텍스트 + 우측 마커 배지(`재기동` / `즉시`)가 한 세트입니다.
**마커 배지**: `재기동`=저장 후 재기동해야 반영 / `즉시`=저장 즉시 반영. 필드마다 반드시 하나.

## CollapsibleSectionHeader — Level 1·2

Level 1: 굵은 라벨 + 굵은 화살표. Level 2: 중간 굵기 + 얇은 화살표 + 좌측 들여쓰기 레일.
중첩은 **2단까지**. 헤더에 항목 수를 괄호로 붙입니다 — `IP / Routing (3)`.
**접힌 섹션에 저장/적용 버튼을 노출하지 마세요.**

## Alert (SectionMessage) — 4 tone

Info=설명 / Warning=주의(진행 가능) / Danger=서비스 영향 / Success=완료.
**화면당 1개.** 두 개가 필요하면 하나는 필드 헬프텍스트로 내리세요.

## Table

- 세로 칼럼 구분선 **없음**. 외곽 `border-strong` > 내부 rule `border`
- 빈 셀은 `—` (muted)
- 헤더는 `whitespace-nowrap`. **폭이 좁아 글자가 세로로 쪼개지는 일이 없게** 컬럼 최소폭을 잡으세요
- 행 전체 배경 tint 금지 — 상태는 배지와 셀 표시로

## Dialog (Modal) — Tone Default·Danger

헤더(제목 + ✕) / 구분선 / 본문 / 푸터(취소 + 확인).
Danger 는 되돌릴 수 없는 액션에만, 확인 버튼이 `destructive`.

## Toast (Sonner) — 4 tone + 건수

**같은 원인은 한 장으로 묶고 `×N`.** 화면에 최대 3장.
헤더 알람 배지와 같은 소스를 봐야 합니다 — 현재 웹은 토스트 6장이 뜨는데 배지는 `알람 0` 입니다.

## DropdownMenu (MenuItem) — Tone 2 × State 3

Default · Hover · **Disabled**. Disabled 는 우측에 사유를 짧게 (`그룹 삭제로만 가능`).

## TreeItem — Kind 2 × State 2 + hasControl

`hasControl` 은 **AA 그룹에서만**: 그룹 행 `+`(새 멤버 자동 생성) · 멤버 행 `×`(그룹에서 제거).
