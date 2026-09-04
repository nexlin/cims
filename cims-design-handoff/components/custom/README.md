# custom 컴포넌트

shadcn 에 대응물이 없어서 직접 만들어야 하는 것들입니다.
그대로 복사해 쓰거나, 구조와 규칙만 참고해서 다시 쓰셔도 됩니다.

- `status-dot.tsx` — 상태 점. **톤 매핑이 이 파일에 고정**되어 있습니다
- `empty-state.tsx` — 빈 상태 5종 공용
- `tree-panel.tsx` — 좌측 시스템 트리 (AA 그룹 `+`/`×` 포함)
- `context-bar.tsx` — 서버/그룹 식별 줄
- `sticky-save-bar.tsx` — 폼 하단 저장바
- `sync-status-row.tsx` — 그룹 패키지 설정 상단 상태 줄
- `app-bar.tsx` — 상단 셸
- `badge-variants.ts` — shadcn badge 를 Tone 6 × Style 2 로 확장
- `alert-variants.ts` — shadcn alert 를 4 tone 으로 확장

`app-sidebar.tsx` 는 shadcn 의 `sidebar` 블록을 받아 메뉴 데이터만 갈아끼우면 됩니다.
메뉴 구조는 `screens/shell.md` 참조.
