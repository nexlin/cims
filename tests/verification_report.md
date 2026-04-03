# CIMS 시스템 검증 결과 리포트

**검증 일시:** 2026-04-03 20:02:46
**소요 시간:** 135.4초
**검증 환경:** 127.0.0.1 (CSC:4420, CSP:5060/4421, CMP:9000)

---

## 종합 결과

| 항목 | 값 |
|------|----|
| 총 검증 항목 | 66건 |
| 성공 (PASS) | 63건 |
| 실패 (FAIL) | 3건 |
| 건너뜀 (SKIP) | 0건 |
| **합격률** | **95.5%** |

## 모듈별 요약

| 모듈 | 전체 | PASS | FAIL | SKIP | 합격률 |
|------|------|------|------|------|--------|
| CMP | 10 | 10 | 0 | 0 | 100% |
| CSP | 9 | 8 | 1 | 0 | 89% |
| CSC | 23 | 23 | 0 | 0 | 100% |
| E2E | 4 | 4 | 0 | 0 | 100% |
| VoLTE-서비스 | 9 | 7 | 2 | 0 | 78% |
| PTT-서비스 | 11 | 11 | 0 | 0 | 100% |

## CMP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CMP-CMD-01 | alive 헬스체크 | PASS | 0 | response={'response': 'OK', 'trans_id': 30687} |
| CMP-CMD-02 | stats 상태 조회 | PASS | 0 | response={'status': 'OK', 'sessions': 0, 'groups': 0, 'rtp_ports_total': 20, 'rt |
| CMP-CMD-03 | add 세션 생성 | PASS | 0 | local_port=50076 |
| CMP-CMD-04 | remove 세션 삭제 | PASS | 0 | response=OK |
| CMP-GRP-01 | addgroup 그룹 생성 | PASS | 2 | response={'ip': '192.168.0.2', 'port': 50076, 'status': 'OK', 'video_port': 5007 |
| CMP-GRP-02 | joingroup 멤버 참여 | PASS | 0 | response=OK |
| CMP-GRP-03 | leavegroup 멤버 탈퇴 | PASS | 0 | response=OK |
| CMP-GRP-04 | removegroup 그룹 삭제 | PASS | 0 | response=OK |
| CMP-RTP-01 | 포트 할당/해제 정합성 | PASS | 0 | before=1, after_add=3(+2), after_remove=1(-2) |
| CMP-FLOOR-01 | 그룹 플로어 상태 확인 | PASS | 2 | group_details count=1, found=True |

## CSP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSP-IF-01 | stats 요청 | PASS | 0 | response={'status': 'OK', 'registered_users': 0, 'active_calls': 0, 'db_connecte |
| CSP-IF-02 | user_change 통지 | PASS | 3000 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-IF-03 | group_change 통지 | PASS | 3003 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-SIP-01 | SIP REGISTER 성공 | PASS | 104 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 0, 'CmsOk': 0, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-02 | SIP REGISTER 인증 실패 | PASS | 15017 | stats={} |
| CSP-SIP-03 | GMS SUBSCRIBE | PASS | 304 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-04 | CMS SUBSCRIBE | PASS | 304 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-CALL-01 | VoIP 1:1 통화 | **FAIL** | 25017 | stats={} |
| CSP-CALL-02 | PTT 그룹 통화 | PASS | 6133 | stats={'RegOk': 4, 'RegFail': 0, 'GmsOk': 4, 'CmsOk': 4, 'NotifyRecv': 0, 'CallO |

## CSC 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSC-AUTH-01 | 로그인 성공 | PASS | 24 | status=200 |
| CSC-AUTH-02 | 로그인 실패 (잘못된 비밀번호) | PASS | 24 | status=401 |
| CSC-AUTH-03 | 세션 조회 (me) | PASS | 3 | status=200, keys=['_status', 'id', 'name', 'login_id', 'role', 'call_subscriptio |
| CSC-AUTH-04 | 비밀번호 변경 | PASS | 56 | change=200, relogin=200 |
| CSC-USER-01 | 가입자 생성 | PASS | 4 | status=201, id=90 |
| CSC-USER-02 | 가입자 조회 | PASS | 4 | status=200, name=_vtest_user1 |
| CSC-USER-03 | 가입자 목록 조회 | PASS | 10 | status=200, count=13 |
| CSC-USER-04 | 가입자 수정 | PASS | 8 | status=200, name=_vtest_user1_mod |
| CSC-VSUB-01 | VoIP 구독 추가 | PASS | 3 | status=201 |
| CSC-VSUB-02 | VoIP 구독 조회 | PASS | 2 | status=200, count=1 |
| CSC-VSUB-03 | VoIP 구독 수정 (DND) | PASS | 4 | status=200 |
| CSC-VSUB-04 | VoIP 구독 삭제 | PASS | 3 | status=200 |
| CSC-PSUB-01 | PTT 구독 추가 | PASS | 3 | status=201 |
| CSC-PSUB-02 | PTT 구독 조회 | PASS | 2 | status=200, count=1 |
| CSC-PSUB-03 | PTT 구독 수정 (DND) | PASS | 3 | status=200 |
| CSC-PSUB-04 | PTT 구독 삭제 | PASS | 2 | status=200 |
| CSC-GRP-01 | PTT 그룹 생성 | PASS | 3 | status=201 |
| CSC-GRP-02 | PTT 그룹 조회 | PASS | 2 | status=200 |
| CSC-GRP-03 | PTT 그룹 수정 | PASS | 3 | status=200 |
| CSC-GRP-04 | PTT 그룹 삭제 | PASS | 4 | delete=200, verify=404 |
| CSC-STAT-01 | 헬스체크 | PASS | 3 | status=200, health={'csp': 'up', 'cmp': 'up', 'db': 'up'} |
| CSC-STAT-02 | 가입자 상태 조회 | PASS | 4 | status=200 |
| CSC-STAT-03 | 서비스 통계 조회 | PASS | 3 | status=200, keys=['_status', 'granularity', 'from', 'to', 'voip', 'ptt'] |

## E2E 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| E2E-HEALTH-01 | CSC→CSP+CMP+DB 헬스 연동 | PASS | 4 | csp=up, cmp=up, db=up |
| E2E-SYNC-01 | 사용자 변경 → CSP 동기화 | PASS | 13 | sub=201, mod=200 (CSP notify는 CSC 내부에서 자동 발송) |
| E2E-SYNC-02 | 그룹 변경 → CSP 동기화 | PASS | 9 | create=201, modify=200 |
| E2E-OPS-01 | 전체 운용 흐름 (생성→구독→그룹→조회→정리) | PASS | 38 | 사용자 생성:OK / VoIP 구독 추가:OK / PTT 구독 추가:OK / 그룹 생성:OK / 사용자 조회 (구독 확인):OK / 그룹 조회  |

## VoLTE-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| VoLTE-REG-01 | SIP 등록 성공 | PASS | 104 | stats={'RegOk': 1, 'RegFail': 0, 'CallOk': 0, 'CallEnd': 0, 'CallFail': 0} |
| VoLTE-REG-02 | SIP 등록 실패 (인증 오류) | PASS | 15017 | stats={} |
| VoLTE-CALL-01 | 1:1 통화 + 대시보드/이력 정합성 | **FAIL** | 21077 | cspsim CallOk>=1:NG / 종료 후 active_voip 비어있음:OK / 이력 존재:NG |
| VoLTE-CALL-02 | 통화 중 실시간 대시보드/상태 정합성 | **FAIL** | 21035 | 등록 후 VoIP 접속자>=1:OK / 통화 중 active_calls>=1:NG / active_calls<=registered/2:OK /  |
| VoLTE-DND-01 | DND 설정/해제 → CSP 동기화 | PASS | 519 | DND 설정/해제 완료 (pid=3) |
| VoLTE-DASH-01 | 대시보드 정합성 (active<=registered/2) | PASS | 4 | reg=5, calls=0, calls<=reg/2=True, roles={'CSCF': True, 'TAS': True, 'PTT_AS': T |
| VoLTE-DASH-02 | 서비스 상태 VoIP 가입자 목록 | PASS | 4 | VoIP 가입자=8, 접속중=1 |
| VoLTE-DASH-03 | VoIP 서비스 통계 | PASS | 3 | attempts=0, success=0, rate=0% |
| VoLTE-DASH-04 | VoIP 통화 이력 조회 | PASS | 3 | total=1, recent=1건 |

## PTT-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| PTT-REG-01 | PTT 등록 + GMS/CMS 구독 | PASS | 304 | Reg=1, GMS=1, CMS=1 |
| PTT-CALL-01 | 그룹 통화 중 실시간 대시보드/상태 + 이력 정합성 | PASS | 13185 | 통화 중 active_ptt>=1:OK / 통화 중 registered>=4:OK / 통화 중 RTP 포트 사용>0:OK / 통화 중 PTT 접 |
| PTT-CALL-02 | Conference NOTIFY 수신 확인 | PASS | 6314 | ConfNotify=19, CallOk=6 |
| PTT-GRP-01 | 그룹 생성 → CMP 동기화 | PASS | 1028 | CMP 그룹 존재=True |
| PTT-GRP-02 | 그룹 멤버 추가 → CMP 반영 | PASS | 1514 | 멤버추가=201, CMP멤버={'group_id': '+8299995001', 'members': 0} |
| PTT-GRP-03 | 그룹 삭제 → CMP 제거 확인 | PASS | 2008 | CMP에서 제거됨=True |
| PTT-DASH-01 | 대시보드 CMP 상태 정합성 | PASS | 5 | groups=2, rtp=10/20 |
| PTT-DASH-02 | 서비스 상태 PTT 가입자 목록 | PASS | 4 | PTT 가입자=11, 접속중=4 |
| PTT-DASH-03 | PTT 서비스 통계 | PASS | 3 | total_calls=1, avg_dur=0.0s |
| PTT-DASH-04 | PTT 통화 이력 조회 | PASS | 4 | total=1, recent=1건 |
| PTT-DASH-05 | 대시보드 CSP 역할 상태 | PASS | 4 | roles={'CSCF': True, 'TAS': True, 'PTT_AS': True, 'IBCF': True} |

## 실패 항목 분석

### CSP-CALL-01 — VoIP 1:1 통화
- **상세:** stats={}

### VoLTE-CALL-01 — 1:1 통화 + 대시보드/이력 정합성
- **상세:** cspsim CallOk>=1:NG | 종료 후 active_voip 비어있음:OK | 이력 존재:NG

### VoLTE-CALL-02 — 통화 중 실시간 대시보드/상태 정합성
- **상세:** 등록 후 VoIP 접속자>=1:OK | 통화 중 active_calls>=1:NG | active_calls<=registered/2:OK | 종료 후 active_calls 감소:OK

---
*리포트 생성: 2026-04-03 20:02:46*