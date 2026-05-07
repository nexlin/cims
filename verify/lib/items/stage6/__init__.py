"""Stage 6 — 통합 검증 (배포본 대상 시나리오).

13 항목:
- ENTRY-CHECK: 4 ports LISTEN + immutability gate (target 별 csc/console 포트)
- SEED: 가입자/그룹 선택 + access_services.jsonl 시드 + csp reload
- SCN-VOLTE-VOICE / VOLTE-VIDEO: 2자 통화 (B2BUA)
- SCN-PTT-VOICE / PTT-VIDEO: 그룹 통화 (5인)
- SCN-SUBSCRIBE: GMS+CMS SUBSCRIBE/NOTIFY e2e (cspsim PTT subscribe)
- SCN-DB-SYNC: admin CRUD → notify_csp UDP → CSP 캐시 갱신 (csp 로그 grep)
- SCN-CERT-ROTATE: mTLS cert rotation e2e (Agent.MtlsEnabled=true 시)
- L7-SUBSCRIBE-NOTIFY: NOTIFY body XML namespace/구조 검증 (read-only)
- CMP-GROUP-SYNC: admin → CSP → CMP roster 동기화 (CMP STATS group_details)
- MCPTT-FLOOR-GRANT: PTT floor REQUEST/GRANT/RELEASE/IDLE 시그널 검증
- SUMMARY: 녹취/SIP/ERROR 카운트
"""
