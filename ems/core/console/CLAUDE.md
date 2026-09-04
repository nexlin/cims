# 콘솔 소스 — 작업 전 확인

**시각 계약 정본 = [docs/design/console_design_system.md](../../../docs/design/console_design_system.md).**
UI 를 만지기 전에 읽는다. 화면 기능·라우팅·위젯 플랫폼 구조는
[docs/design/console_platform.md](../../../docs/design/console_platform.md) 가 정본이다.

- **스택은 Tailwind + shadcn/ui + Radix 로 이행 중**이다. Mantine 금지, 두 체계 혼용 금지.
  `src/index.css` 는 폐기 대상이지만 아직 37개 페이지가 여기에 의존한다 — 통째로 걷어내지 않는다.
- **손대지 말 것**: `src/index.css` 의 위젯 편집·2D 그리드 CSS(`.grid-canvas` `.card-canvas` 계열)와
  `src/widgets/EditableLayout.tsx` · `GridEditor.tsx` · `CardLayout.tsx`. 디자인 시안에 없지만
  **살아있는 기능**이다 (정본 문서 §7-2, §7-3).
- **hex 직접 사용 금지**(토큰만) · **이모지/텍스트 글리프 아이콘 금지**(Lucide 만).
- 화면 구조·문구 스펙은 `/deploy/servers` 것만 있다(`cims-design-handoff/screens/`). **나머지 36개
  페이지는 구조·문구 현행 유지**하고 토큰·컴포넌트만 교체한다. 스펙 없는 화면을 지어내지 않는다.
- 서비스 팩(`ems/service/console/src`)은 자체 `node_modules` 없이 여기 것을 링크로 공유한다
  (`scripts/ensure-svc-modules.mjs`). Tailwind content 스캔 경로에 반드시 포함한다.
- 실서버 배포·화면 검증은 사용자가 한다.
