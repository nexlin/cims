"""Stage 1 — 정적 검사 (lint / format / unit test).

- S1-PY-SYNTAX           : python3 -m py_compile (검증 도구 코드 위생)
- S1-FRONTEND-LINT       : npm run lint (ems/core/console)
- S1-FRONTEND-TYPECHECK  : tsc -b --noEmit
- S1-CPP-FORMAT          : clang-format --dry-run --Werror (csp/.clang-format)
- S1-UNIT-VERIFY-LIB     : python3 -m unittest tests.test_verify_lib
- S1-UNIT-HA-INTENT      : python3 -m unittest tests.test_ha_intent
- S1-UNIT-CSC            : python3 -m unittest tests.test_csc_dispatch_rbac tests.test_csc_subscription_realm (관제 그룹 편입 RBAC · 가입 번호 realm 해석)
- S1-UNIT-CONSOLE-LAYOUT : python3 -m unittest tests.test_console_layouts
- S1-UNIT-GRID-BUDGET    : node tests/frontend/grid_budget.test.mjs (그리드 세로 예산 + 잠금)
"""
