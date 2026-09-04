# Figma 컴포넌트 → shadcn 대응표

Figma 파일에 등록된 컴포넌트 **27종**(세트 18 + 단독 9)의 대응입니다.

## A. shadcn 그대로 (`npx shadcn@latest add ...`)

| Figma | shadcn | 배리언트 매핑 | 비고 |
|---|---|---|---|
| **Button** | `button` | Primary→`default` · Secondary→`outline` · Ghost→`ghost` · Danger→`destructive` · State=Disabled→`disabled` 속성 | Size sm(26px h) / md(36px h). shadcn 기본보다 작으므로 `size` 를 sm/default 로 재정의 |
| **Badge** | `badge` | Tone 6종(Brand·Success·Warning·Danger·Info·Neutral) × Style 2종(Soft·Solid) | shadcn 기본 4 variant 로는 부족 → **variant 확장 필요** (아래 코드 참조) |
| **TextInput** | `input` + `label` | State: Default·Focus·Error·Disabled | 라벨·필수표시·헬프텍스트를 묶는 `FormField` 래퍼를 하나 만들어 쓰세요 |
| **Select** | `select` | State 3종 | 같은 `FormField` 래퍼 사용 |
| **Checkbox** | `checkbox` | Unchecked·Checked | |
| **Radio** | `radio-group` | | 표 안의 MASTER 지정에 사용 |
| **Switch** | `switch` | Off·On | 즉시 적용 전용 |
| **SegmentedItem** | `toggle-group` (`type="single"`) | Default·Selected | `공통 설정 / 멤버 비교` 전환 |
| **TabItem / Tabs** | `tabs` | Default·Selected + `hasCount` | 카운트 배지는 웹 미구현(제안 항목) |
| **SectionMessage** | `alert` | Info·Warning·Danger·Success | shadcn `alert` 는 2 variant → **4 tone 확장 필요** |
| **Modal** | `dialog` | Tone Default·Danger | 본문은 children 으로 |
| **Toast** | `sonner` | Tone 4종 + `hasCount` | 묶기 규칙은 DESIGN-RULES.md §2 |
| **MenuItem / Menu** | `dropdown-menu` | Tone Default·Danger × State Default·Hover·**Disabled** | Disabled 는 사유(hint)를 우측에 표시 |
| **CollapsibleSectionHeader** | `collapsible` | Level 1·2 × State | 헤더는 직접 작성(라벨 + 힌트 + caret) |
| **TableHeaderCell / Table** | `table` | Align Left·Right, `sortable` | 세로 구분선 없음 |

## B. shadcn 에 없음 → 직접 (`components/custom/`)

| Figma | 파일 | 설명 |
|---|---|---|
| **StatusDot** | `status-dot.tsx` | 점 + 라벨. 톤 매핑은 DESIGN-RULES.md §2 |
| **EmptyState** | `empty-state.tsx` | 제목 + 설명. 5종 사용처는 `screens/empty-states.md` |
| **TreeItem / TreePanel** | `tree-panel.tsx` | 시스템 트리. Kind(Group/Node) × State + `hasControl`(AA 전용 `+`/`×`) |
| **ContextBar** | `context-bar.tsx` | 선택 객체 식별 줄. Scope(Server/Group) |
| **StickySaveBar** | `sticky-save-bar.tsx` | 하단 저장바. 변경 건수 배지 + 안내문 + 되돌리기/저장 |
| **SyncStatusRow** | `sync-status-row.tsx` | 그룹 패키지 설정 상단. 동기화 · ACTIVE 노드 · 드리프트(`hasDrift`) |
| **AppBar** | `app-bar.tsx` | 상단 셸 |
| **Sidebar** | `app-sidebar.tsx` | 좌측 내비. shadcn `sidebar` 블록은 구조가 달라 **참고만** |
| **Form · 시스템 추가** | `system-create-form.tsx` | Dialog 안에 들어가는 조건부 폼 |

## C. 설치 명령

```bash
npx shadcn@latest init
npx shadcn@latest add button badge input label select checkbox radio-group switch \
  toggle-group tabs alert dialog sonner dropdown-menu collapsible table tooltip separator scroll-area
```

`tooltip` 은 비활성 버튼의 사유 표시에 필요합니다.
