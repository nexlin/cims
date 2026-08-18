"""Stage 1 — 정적 검사 (lint / format / unit test).

- S1-PY-SYNTAX           : python3 -m py_compile (검증 도구 코드 위생)
- S1-FRONTEND-LINT       : npm run lint (cims-console)
- S1-FRONTEND-TYPECHECK  : tsc -b --noEmit
- S1-CPP-FORMAT          : clang-format --dry-run --Werror (csp/.clang-format)
- S1-UNIT-VERIFY-LIB     : python3 -m unittest tests.test_verify_lib
- S1-UNIT-HA-INTENT      : python3 -m unittest tests.test_ha_intent
"""
