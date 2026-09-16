"""Stage 1 — 정적 검사 (lint / format / unit test).

- S1-PY-SYNTAX           : python3 -m py_compile (검증 도구 코드 위생)
- S1-FRONTEND-LINT       : npm run lint (ems/core/console)
- S1-FRONTEND-TYPECHECK  : tsc -b --noEmit
- S1-CPP-FORMAT          : clang-format --dry-run --Werror (csp/.clang-format)
- S1-UNIT-VERIFY-LIB     : python3 -m unittest tests.test_verify_lib
- S1-UNIT-HA-INTENT      : python3 -m unittest tests.test_ha_intent
- S1-UNIT-CSC            : python3 -m unittest tests.test_csc_dispatch_rbac tests.test_csc_subscription_realm (관제 그룹 편입 RBAC · 가입 번호 realm 해석)
- S1-UNIT-CONSOLE-LAYOUT : python3 -m unittest tests.test_console_layouts tests.test_console_static (레이아웃 영속 · 가용 서비스 · 콘솔 번들 해석)
- S1-UNIT-TESTER         : python3 -m unittest tests.test_tester_models tests.test_tester_handler tests.test_tester_run tests.test_gateway_stream (계측기 계약·핸들러·오케스트레이터·게이트웨이 SSE)
- S1-UNIT-GRID-BUDGET    : node tests/frontend/grid_budget.test.mjs (그리드 세로 예산 + 잠금)
- S1-UNIT-STORE-ALARM    : python3 -m unittest tests.test_store_alarm (공유 store 접근 불가 알람 A-PRC-028 — 감지기 데드라인 + 규칙 정합)
- S1-UNIT-OAM-PTT        : python3 tests/oam_ptt_index_test.py · tests/oam_ptt_sessions_live_test.py (PTT 세션 인덱스 · 진행중 병합 규약)
- S1-UNIT-OAM-HTTPSRV    : python3 -m unittest tests.test_httpsrv_bind (HttpServer 기동 계약 — bind 성공이라야 기동 성공, 떠 있는 척 차단)
- S1-UNIT-OAM-STATS      : python3 tests/test_oam_stats_classify.py · test_stats_probe.py · test_stats_rollup_range.py · test_stats_store.py (SIP 통계 서비스축 · 미디어 프로브 · 구간 조회 계층 선택 · 집계 저장소 단일 writer)
- S1-UNIT-PSIP           : g++ tests/psip_leg_dest_test.cpp ← build/csp/psip_build/*.a → 127.0.0.1 루프백 실행 (서버 발신 in-dialog 요청 목적지 재해석; 라이브러리 없으면 SKIP)
"""
