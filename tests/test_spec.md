# CIMS 검증시험 항목서/절차서

**문서번호:** CIMS-TS-2026-001
**작성일:** 2026-04-03

---

## CSC 모듈 검증 항목

### CSC-AUTH: 인증 API

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-AUTH-01 | 로그인 성공 | POST /api/v1/auth/login {login_id, password} | 200, token 반환 | status=200, token 존재 |
| CSC-AUTH-02 | 로그인 실패 (잘못된 비밀번호) | POST /api/v1/auth/login {login_id, wrong_pw} | 401 에러 | status=401 |
| CSC-AUTH-03 | 세션 조회 | GET /api/v1/auth/me (Bearer token) | 200, 사용자 정보 반환 | status=200, login_id 일치 |
| CSC-AUTH-04 | 비밀번호 변경 | PUT /api/v1/auth/password {old, new} | 200 | status=200 |

### CSC-USER: 가입자 관리

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-USER-01 | 가입자 생성 | POST /api/v1/users {name, ...} | 201, id 반환 | status=201, id > 0 |
| CSC-USER-02 | 가입자 조회 | GET /api/v1/users/{id} | 200, 사용자 정보 | status=200, name 일치 |
| CSC-USER-03 | 가입자 목록 | GET /api/v1/users | 200, users 배열 | status=200, 배열 포함 |
| CSC-USER-04 | 가입자 수정 | PUT /api/v1/users/{id} {name: new} | 200 | status=200, 이름 변경 확인 |
| CSC-USER-05 | 가입자 삭제 | DELETE /api/v1/users/{id} | 200 | status=200, 재조회 404 |

### CSC-VSUB: VoIP 구독 관리

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-VSUB-01 | VoIP 구독 추가 | POST /api/v1/users/{pid}/call {id, passwd, auth_id} | 201 | status=201 |
| CSC-VSUB-02 | VoIP 구독 조회 | GET /api/v1/users/{pid}/call | 200, 구독 목록 | status=200, 구독 존재 |
| CSC-VSUB-03 | VoIP 구독 수정 | PUT /api/v1/users/{pid}/call/{msisdn} {dnd: true} | 200 | status=200 |
| CSC-VSUB-04 | VoIP 구독 삭제 | DELETE /api/v1/users/{pid}/call/{msisdn} | 200 | status=200 |

### CSC-PSUB: PTT 구독 관리

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-PSUB-01 | PTT 구독 추가 | POST /api/v1/users/{pid}/ptt {id, passwd, auth_id} | 201 | status=201 |
| CSC-PSUB-02 | PTT 구독 조회 | GET /api/v1/users/{pid}/ptt | 200, 구독 목록 | status=200, 구독 존재 |
| CSC-PSUB-03 | PTT 구독 수정 | PUT /api/v1/users/{pid}/ptt/{msisdn} {dnd: true} | 200 | status=200 |
| CSC-PSUB-04 | PTT 구독 삭제 | DELETE /api/v1/users/{pid}/ptt/{msisdn} | 200 | status=200 |

### CSC-GRP: PTT 그룹 관리

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-GRP-01 | 그룹 생성 | POST /api/v1/ptt/groups {id, name, members} | 201 | status=201 |
| CSC-GRP-02 | 그룹 조회 | GET /api/v1/ptt/groups/{id} | 200, 그룹 정보+멤버 | status=200 |
| CSC-GRP-03 | 그룹 수정 | PUT /api/v1/ptt/groups/{id} {name: new} | 200 | status=200 |
| CSC-GRP-04 | 그룹 삭제 | DELETE /api/v1/ptt/groups/{id} | 200 | status=200, 재조회 404 |

### CSC-STAT: 통계/헬스

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSC-STAT-01 | 헬스체크 | GET /api/v1/stats/health | 200, health 객체 | status=200, health.db 존재 |
| CSC-STAT-02 | 가입자 상태 | GET /api/v1/stats/subscribers | 200, subscribers 배열 | status=200 |
| CSC-STAT-03 | 서비스 통계 | GET /api/v1/stats/service/summary | 200, 통계 데이터 | status=200 |

---

## CSP 모듈 검증 항목

### CSP-IF: CscInterface UDP 명령

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSP-IF-01 | stats 요청 | UDP {event:stats} → 4421 | JSON 응답 (status:OK) | status=OK, registered_users 존재 |
| CSP-IF-02 | user_change 통지 | UDP {event:user_change, action:PUT} → 4421 | 캐시 갱신 (로그 확인) | 메시지 수신 확인 |
| CSP-IF-03 | group_change 통지 | UDP {event:group_change, action:PUT} → 4421 | 그룹 설정 리로드 | 메시지 수신 확인 |

### CSP-SIP: SIP 등록/구독

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSP-SIP-01 | SIP REGISTER 성공 | cspsim -scenario register -count 1 | 등록 성공 | iRegOk=1 |
| CSP-SIP-02 | SIP REGISTER 인증 실패 | cspsim -password wrong | 등록 실패 | iRegFail=1 |
| CSP-SIP-03 | SIP SUBSCRIBE GMS | cspsim -scenario subscribe -mode ptt | GMS 구독 성공 | iGmsOk=1 |
| CSP-SIP-04 | SIP SUBSCRIBE CMS | cspsim -scenario subscribe -mode ptt | CMS 구독 성공 | iCmsOk=1 |

### CSP-CALL: VoIP/PTT 통화

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CSP-CALL-01 | VoIP 1:1 통화 | cspsim -scenario call -count 2 -mode voip | 통화 성공 | iCallOk=2 |
| CSP-CALL-02 | PTT 그룹 통화 | cspsim -scenario group-call -count 4 -mode ptt | 그룹콜 성공 | iCallOk=4 |

---

## CMP 모듈 검증 항목

### CMP-CMD: 제어 명령

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CMP-CMD-01 | alive | UDP {cmd:alive} → 9000 | "OK" 응답 | response 존재 |
| CMP-CMD-02 | stats | UDP {cmd:stats} → 9000 | 상태 JSON | sessions, rtp_ports_total 존재 |
| CMP-CMD-03 | add 세션 | UDP {cmd:add, session_id:test_s1} → 9000 | 포트 할당 응답 | local_port > 0 |
| CMP-CMD-04 | remove 세션 | UDP {cmd:remove, session_id:test_s1} → 9000 | OK 응답 | 성공 |

### CMP-GRP: 그룹 명령

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CMP-GRP-01 | addgroup | UDP {cmd:addgroup, group_id:test_g1} → 9000 | 공유 포트 할당 | ip, port 존재 |
| CMP-GRP-02 | joingroup | UDP {cmd:joingroup, group_id:test_g1, session_id:s1} | OK 응답 | 성공 |
| CMP-GRP-03 | leavegroup | UDP {cmd:leavegroup, group_id:test_g1, session_id:s1} | OK 응답 | 성공 |
| CMP-GRP-04 | removegroup | UDP {cmd:removegroup, group_id:test_g1} | OK 응답 | 성공 |

### CMP-RTP: RTP 포트 관리

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CMP-RTP-01 | 포트 할당/해제 | add 2개 → stats → remove 2개 → stats | used 증감 확인 | used 변화량 일치 |

### CMP-FLOOR: 플로어 제어

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| CMP-FLOOR-01 | 플로어 상태 확인 | addgroup → stats (group_details) | floor_holder 존재 | group_details 배열 확인 |

---

## 연동/E2E 검증 항목

### E2E-SYNC: CSC-CSP 동기화

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| E2E-SYNC-01 | 사용자 변경 동기화 | CSC에서 사용자 수정 → CSP stats 확인 | CSP 캐시 갱신 | notify_csp 호출 성공 |
| E2E-SYNC-02 | 그룹 변경 동기화 | CSC에서 그룹 수정 → CSP stats 확인 | 그룹 리로드 | notify_csp 호출 성공 |

### E2E-HEALTH: 헬스체크 연동

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| E2E-HEALTH-01 | CSC→CSP+CMP 헬스 | GET /api/v1/stats/health | CSP/CMP/DB 상태 | health.csp=up, health.cmp=up |

### E2E-CALL: 단대단 통화

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| E2E-CALL-01 | VoIP E2E | cspsim 2세션 통화 → 통화이력 확인 | 이력 기록 | call_logs에 기록 존재 |
| E2E-CALL-02 | PTT E2E | cspsim 4세션 그룹콜 → 통화이력 확인 | 이력 기록 | call_logs에 기록 존재 |

### E2E-OPS: 운용 시나리오

| ID | 항목 | 절차 | 기대 결과 | 판정 기준 |
|----|------|------|-----------|-----------|
| E2E-OPS-01 | 전체 운용 흐름 | 사용자생성→구독추가→그룹편성→SIP등록→통화→이력확인→정리 | 전 단계 성공 | 모든 단계 Pass |
