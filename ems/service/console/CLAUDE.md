# 서비스 콘솔 팩 — 작업 전 확인

core 콘솔과 **같은 규칙**을 따른다. 정본 두 개를 먼저 읽는다.

- 시각 계약 — [docs/design/console_design_system.md](../../../docs/design/console_design_system.md)
- 콘솔 규칙 요약 — [ems/core/console/CLAUDE.md](../../core/console/CLAUDE.md)

이 팩은 자체 `package.json`·`node_modules` 없이 core 것을 심볼릭 링크로 공유한다
(`ems/core/console/scripts/ensure-svc-modules.mjs`). 의존성을 여기서 따로 추가하지 않는다.
