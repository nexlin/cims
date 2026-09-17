# 계측기 콘솔 팩 — 작업 전 확인

서비스 팩([ems/service/console/CLAUDE.md](../../service/console/CLAUDE.md))과 **같은 규칙**을 따른다. 정본 두 개를 먼저 읽는다.

- 시각 계약 — [docs/design/console_design_system.md](../../../docs/design/console_design_system.md)
- 콘솔 규칙 요약 — [ems/core/console/CLAUDE.md](../../core/console/CLAUDE.md)

화면 정의는 [docs/design/features/test_instrument.md](../../../docs/design/features/test_instrument.md) §7 이 정본이다.
이 팩은 자체 `package.json`·`node_modules` 없이 core 것을 심볼릭 링크로 공유한다
(`ems/core/console/scripts/ensure-svc-modules.mjs`). 의존성을 여기서 따로 추가하지 않는다.
core 참조는 `@core/*`, 자기 참조는 `@tester/*`. 섹션은 `requiresService: 'oam-cims-tester'` —
모듈이 배포되지 않은 콘솔에는 메뉴가 나타나지 않는다.
