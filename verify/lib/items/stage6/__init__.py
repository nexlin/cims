"""Stage 6 — 통합 검증 (배포본 대상 시나리오).

10 항목:
- ENTRY-CHECK: 4 ports LISTEN + immutability gate (target 별 csc/console 포트)
- SEED: 가입자/그룹 선택 + access_services.jsonl 시드 + csp reload
- SCN-VOLTE-VOICE / VOLTE-VIDEO: 2자 통화 (B2BUA)
- SCN-PTT-VOICE / PTT-VIDEO: 그룹 통화 (5인)
- SCN-SUBSCRIBE: GMS+CMS SUBSCRIBE/NOTIFY e2e (cspsim PTT subscribe)
- SCN-DB-SYNC: admin CRUD → notify_csp UDP → CSP 캐시 갱신 (csp 로그 grep)
- SCN-CERT-ROTATE: mTLS cert rotation e2e (Agent.MtlsEnabled=true 시)
- SUMMARY: 녹취/SIP/ERROR 카운트
"""
