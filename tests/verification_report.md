# CIMS 시스템 검증 결과 리포트

**검증 일시:** 2026-04-20 15:37:34
**소요 시간:** 297.0초
**검증 환경:** 127.0.0.1 (CSC:4420, CSP:5060/4421, CMP:9000)

---

## 종합 결과

| 항목 | 값 |
|------|----|
| 총 검증 항목 | 123건 |
| 성공 (PASS) | 106건 |
| 실패 (FAIL) | 17건 |
| 건너뜀 (SKIP) | 0건 |
| **합격률** | **86.2%** |

## 모듈별 요약

| 모듈 | 전체 | PASS | FAIL | SKIP | 합격률 |
|------|------|------|------|------|--------|
| CMP | 12 | 12 | 0 | 0 | 100% |
| CSP | 11 | 8 | 3 | 0 | 73% |
| CSC | 23 | 23 | 0 | 0 | 100% |
| E2E | 4 | 4 | 0 | 0 | 100% |
| VoLTE-서비스 | 13 | 9 | 4 | 0 | 69% |
| PTT-서비스 | 18 | 14 | 4 | 0 | 78% |
| 미디어-녹취 | 11 | 5 | 6 | 0 | 45% |
| SIP-RUNTIME | 31 | 31 | 0 | 0 | 100% |

## CMP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CMP-CMD-01 | alive 헬스체크 | PASS | 0 | response={'response': 'OK', 'service': 'system', 'sesid': '::cmp::20260420153237 |
| CMP-CMD-02 | stats 상태 조회 | PASS | 0 | response={'status': 'OK', 'sessions': 0, 'groups': 2, 'rtp_ports_total': 20, 'rt |
| CMP-CMD-03 | add 세션 생성 | PASS | 0 | local_port=50076 |
| CMP-CMD-04 | remove 세션 삭제 | PASS | 0 | response=OK |
| CMP-GRP-01 | addgroup 그룹 생성 | PASS | 0 | response={'floor_port': 54014, 'ip': '192.168.0.2', 'port': 52014, 'status': 'OK |
| CMP-GRP-02 | joingroup 멤버 참여 | PASS | 0 | response=OK |
| CMP-GRP-03 | leavegroup 멤버 탈퇴 | PASS | 0 | response=OK |
| CMP-GRP-04 | removegroup 그룹 삭제 | PASS | 0 | response=OK |
| CMP-RTP-01 | 포트 할당/해제 정합성 | PASS | 0 | before=0, after_add=2(+2), after_remove=0(-2) |
| CMP-FLOOR-01 | 그룹 플로어 상태 확인 | PASS | 0 | group_details count=3, found=True |
| CMP-TMR-01 | 세션 타임아웃 설정값 확인 | PASS | 0 | session_timeout=600s |
| CMP-TMR-02 | 세션 생성/삭제 후 리소스 정합성 | PASS | 0 | 생성 후 sessions=1, 삭제 후 sessions=0 |

## CSP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSP-IF-01 | stats 요청 | **FAIL** | 3001 | 응답 없음 (timeout) — CSP가 최신 바이너리인지 확인 |
| CSP-IF-02 | user_change 통지 | PASS | 3000 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-IF-03 | group_change 통지 | PASS | 3000 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-TMR-01 | 타이머 설정값 확인 | **FAIL** | 3003 | 응답 없음 (timeout) |
| CSP-TMR-02 | 등록해제 시 DB logout_time 갱신 | PASS | 6211 | register_time=2026-04-20 15:32:49, logout_time=2026-04-20 15:32:51 |
| CSP-SIP-01 | SIP REGISTER 성공 | PASS | 5212 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 0, 'CmsOk': 0, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-02 | SIP REGISTER 인증 실패 | PASS | 15017 | stats={} |
| CSP-SIP-03 | GMS SUBSCRIBE | PASS | 5211 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-04 | CMS SUBSCRIBE | PASS | 5211 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-CALL-01 | VoIP 1:1 통화 | PASS | 9519 | stats={'RegOk': 2, 'RegFail': 0, 'GmsOk': 0, 'CmsOk': 0, 'NotifyRecv': 0, 'CallO |
| CSP-CALL-02 | PTT 그룹 통화 | **FAIL** | 30002 | stats={} |

## CSC 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSC-AUTH-01 | 로그인 성공 | PASS | 29 | status=200 |
| CSC-AUTH-02 | 로그인 실패 (잘못된 비밀번호) | PASS | 22 | status=401 |
| CSC-AUTH-03 | 세션 조회 (me) | PASS | 3 | status=200, keys=['_status', 'id', 'name', 'login_id', 'role', 'call_subscriptio |
| CSC-AUTH-04 | 비밀번호 변경 | PASS | 57 | change=200, relogin=200 |
| CSC-USER-01 | 가입자 생성 | PASS | 4 | status=201, id=343 |
| CSC-USER-02 | 가입자 조회 | PASS | 3 | status=200, name=_vtest_user1 |
| CSC-USER-03 | 가입자 목록 조회 | PASS | 12 | status=200, count=13 |
| CSC-USER-04 | 가입자 수정 | PASS | 7 | status=200, name=_vtest_user1_mod |
| CSC-VSUB-01 | VoIP 구독 추가 | PASS | 4 | status=201 |
| CSC-VSUB-02 | VoIP 구독 조회 | PASS | 3 | status=200, count=1 |
| CSC-VSUB-03 | VoIP 구독 수정 (DND) | PASS | 4 | status=200 |
| CSC-VSUB-04 | VoIP 구독 삭제 | PASS | 3 | status=200 |
| CSC-PSUB-01 | PTT 구독 추가 | PASS | 3 | status=201 |
| CSC-PSUB-02 | PTT 구독 조회 | PASS | 3 | status=200, count=1 |
| CSC-PSUB-03 | PTT 구독 수정 (DND) | PASS | 4 | status=200 |
| CSC-PSUB-04 | PTT 구독 삭제 | PASS | 3 | status=200 |
| CSC-GRP-01 | PTT 그룹 생성 | PASS | 3 | status=201 |
| CSC-GRP-02 | PTT 그룹 조회 | PASS | 3 | status=200 |
| CSC-GRP-03 | PTT 그룹 수정 | PASS | 3 | status=200 |
| CSC-GRP-04 | PTT 그룹 삭제 | PASS | 6 | delete=200, verify=404 |
| CSC-STAT-01 | 헬스체크 | PASS | 3 | status=200, health={'csp': 'up', 'cmp': 'up', 'db': 'up'} |
| CSC-STAT-02 | 가입자 상태 조회 | PASS | 4 | status=200 |
| CSC-STAT-03 | 서비스 통계 조회 | PASS | 24 | status=200, keys=['_status', 'granularity', 'from', 'to', 'voip', 'ptt'] |

## E2E 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| E2E-HEALTH-01 | CSC→CSP+CMP+DB 헬스 연동 | PASS | 3 | csp=up, cmp=up, db=up |
| E2E-SYNC-01 | 사용자 변경 → CSP 동기화 | PASS | 15 | sub=201, mod=200 (CSP notify는 CSC 내부에서 자동 발송) |
| E2E-SYNC-02 | 그룹 변경 → CSP 동기화 | PASS | 11 | create=201, modify=200 |
| E2E-OPS-01 | 전체 운용 흐름 (생성→구독→그룹→조회→정리) | PASS | 48 | 사용자 생성:OK / VoIP 구독 추가:OK / PTT 구독 추가:OK / 그룹 생성:OK / 사용자 조회 (구독 확인):OK / 그룹 조회  |

## VoLTE-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| VoLTE-REG-01 | SIP 등록 성공 | PASS | 4208 | stats={'RegOk': 1, 'RegFail': 0, 'CallOk': 0, 'CallEnd': 0, 'CallFail': 0} |
| VoLTE-REG-02 | SIP 등록 실패 (인증 오류) | PASS | 15008 | stats={} |
| VoLTE-CALL-01 | 1:1 통화 + 대시보드/이력 정합성 | **FAIL** | 17584 | cspsim CallOk>=1:OK / 종료 후 active_calls=0:OK / 종료 후 active_voip 비어있음:OK / 종료 후 r |
| VoLTE-CALL-02 | 통화 중 실시간 대시보드/상태 정합성 | **FAIL** | 18598 | 등록 후 VoIP 접속자>=1:OK / 통화 중 active_calls>=1:OK / active_calls<=registered/2:OK /  |
| VoLTE-DND-01 | DND 설정/해제 → CSP 동기화 | PASS | 530 | DND 설정/해제 완료 (pid=3) |
| VoLTE-DASH-01 | 대시보드 정합성 (active<=registered/2) | PASS | 7 | reg=3, calls=0, calls<=reg/2=True, roles={'CSCF': True, 'TAS': True, 'PTT_AS': T |
| VoLTE-DASH-02 | 서비스 상태 VoIP 가입자 목록 | PASS | 6 | VoIP 가입자=8, 접속중=0 |
| VoLTE-DASH-03 | VoIP 서비스 통계 | PASS | 5 | attempts=3, success=3, rate=100.0% |
| VoLTE-DASH-04 | VoIP 통화 이력 조회 | PASS | 6 | total=17, recent=17건 |
| VoLTE-DASH-05 | 시험 종료 후 잔류 데이터 없음 확인 | **FAIL** | 13 | registered=0:NG / active_calls=0:OK / active_voip 비어있음:OK / ringing 잔류 없음:OK / V |
| VoLTE-TMR-01 | CSP 타이머 설정 활성 상태 | **FAIL** | 3003 | CSP 응답 없음 |
| VoLTE-TMR-02 | CMP 세션 타임아웃 설정 활성 상태 | PASS | 0 | SessionTimeout=600s |
| VoLTE-TMR-03 | 통화 종료 후 CMP VoIP 세션 정리 확인 | PASS | 0 | sessions=0, rtp_free=20/20, session_timeout=600s |

## PTT-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| PTT-MCPTT-01 | IdMS 인증 (PKCE) + 토큰 발급 | PASS | 70 | access_token=OK, refresh=OK |
| PTT-MCPTT-02 | GMS 그룹 목록 조회 | PASS | 2 | status=200, groups=1건 |
| PTT-MCPTT-03 | CMS 사용자 프로필 조회 | PASS | 4 | status=200, content_type=application/vnd.3gpp.mcptt-user-profile+ |
| PTT-MCPTT-04 | CMS 서비스 설정 조회 | PASS | 3 | status=200 |
| PTT-REG-01 | PTT 등록 + GMS/CMS 구독 | PASS | 5214 | Reg=1, GMS=1, CMS=1 |
| PTT-CALL-01 | 그룹 통화 중 실시간 대시보드/상태 + 이력 정합성 | **FAIL** | 44230 | 통화 중 active_ptt>=1:OK / 통화 중 registered>=4:OK / 통화 중 RTP 포트 사용>0:NG / 통화 중 PTT 접 |
| PTT-CALL-02 | Conference NOTIFY 수신 확인 | **FAIL** | 30003 | ConfNotify=0, CallOk=None |
| PTT-GRP-01 | 그룹 생성 → CMP 동기화 | PASS | 1049 | CMP 그룹 존재=True |
| PTT-GRP-02 | 그룹 멤버 추가 → CMP 반영 | PASS | 1524 | 멤버추가=201, CMP멤버={'group_id': '+8299995001', 'members': 0} |
| PTT-GRP-03 | 그룹 삭제 → CMP 제거 확인 | PASS | 2018 | CMP에서 제거됨=True |
| PTT-DASH-01 | 대시보드 CMP 상태 정합성 | PASS | 7 | groups=2, rtp=0/20 |
| PTT-DASH-02 | 서비스 상태 PTT 가입자 목록 | PASS | 7 | PTT 가입자=11, 접속중=3 |
| PTT-DASH-03 | PTT 서비스 통계 | PASS | 25 | total_calls=9, avg_dur=24.8s |
| PTT-DASH-04 | PTT 통화 이력 조회 | PASS | 6 | total=17, recent=17건 |
| PTT-DASH-05 | 대시보드 CSP 역할 상태 | PASS | 7 | roles={'CSCF': True, 'TAS': True, 'PTT_AS': True, 'IBCF': True} |
| PTT-DASH-06 | 시험 종료 후 잔류 데이터 없음 확인 | **FAIL** | 5043 | PTT 접속자=0:NG / PTT 그룹참여=0:NG / active_ptt 비어있음:NG (online=3, in_grp=3, active_pt |
| PTT-TMR-01 | CSP 타이머 설정 상태 | **FAIL** | 3003 | CSP 응답 없음 |
| PTT-TMR-02 | 그룹콜 종료 후 CMP 자원 회수 | PASS | 0 | sessions=0, groups=2, stale_test=0, rtp_free=20/20, session_timeout=600s |

## 미디어-녹취 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| MEDIA-RTP-01 | AMR-WB 미디어 파일 준비 확인 | PASS | 0 | file_size=45750, frames=750 |
| MEDIA-RTP-02 | AMR-WB RTP 전송 + VoIP 통화 | PASS | 9524 | 미디어 로딩:OK / 통화 성공:OK |
| MEDIA-RTP-03 | AMR-WB RTP 전송 + PTT 그룹콜 | **FAIL** | 30003 | 미디어 로딩:NG / 그룹콜 성공:NG |
| MEDIA-REC-01 | CSP 녹취 설정 활성화 상태 | **FAIL** | 3003 | CSP 응답 없음 |
| MEDIA-REC-02 | CMP 녹취 raw 파일 기록 확인 | **FAIL** | 2214 | file=_vtest_rec_1776667042_a.rtp, size=0 |
| MEDIA-REC-03 | 녹취 raw 파일 RTP 정합성 | **FAIL** | 0 | 비어있지 않은 녹취 파일 없음 |
| MEDIA-REC-04 | 녹취 raw 파일 내용 확인 | **FAIL** | 0 | rtp_files=0, non_empty=0, total_size=0 |
| MEDIA-TRANS-01 | ffmpeg 설치 상태 | PASS | 62 | ffmpeg: ffmpeg version 6.1.1-3ubuntu5 Copyright (c) 2000-2023 the FFmpeg develop |
| MEDIA-TRANS-02 | 녹취 API 엔드포인트 확인 | PASS | 42 | status=200, total=19 |
| MEDIA-TRANS-03 | RTP→AMR-WB 스트리핑 동작 확인 | **FAIL** | 0 | 녹취 파일 없음 |
| MEDIA-CMP-01 | CMP RTP 릴레이 동작 확인 | PASS | 2455 | sent=10, relayed=10, cmp_port=50072 |

## SIP-RUNTIME 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| P1-01 | CSC 헬스 체크 | PASS | 8 | status=200 |
| P1-02 | CSC file snapshot 5개 파일 | PASS | 0 | /home/nex/work/cims/build/dist/csc/cache → missing=none |
| P1-03 | CSP local file cache 존재 | PASS | 0 | files=['access.json', 'listeners.json', 'routes.json', 'trunks.json'] |
| P2-01 | 리스너 목록 조회 | PASS | 6 | status=200 |
| P2-02 | 리스너 생성 (port 5093) | PASS | 39 | id=14 |
| P2-03 | 실제 UDP bind 확인 (ss) | PASS | 2 | port 5093 bound |
| P2-04 | 리스너 수정 (PUT) | PASS | 20 | status=200 |
| P2-05 | 중복 bind_ip:port:protocol 거부 | PASS | 5 | status=409 |
| P2-06 | 리스너 삭제 → 포트 해제 | PASS | 19 | unbind completed |
| P3-01 | 트렁크 목록 조회 | PASS | 4 | status=200 |
| P3-02 | Alive 트렁크 생성 (self:5060) | PASS | 14 | id=19 |
| P3-03 | Dead 트렁크 생성 (TEST-NET) | PASS | 18 | id=20 |
| P3-04 | 헬스 alive/dead 감지 (~20s) | PASS | 1503 | alive=True rtt=0 / dead=False fails=0 |
| P3-05 | 트렁크 삭제 | PASS | 35 | all deleted |
| P4-01 | 라우팅 타겟 트렁크 준비 | PASS | 16 | trunk_id=21 |
| P4-02 | Prefix 라우팅 규칙 생성 | PASS | 27 | id=11 |
| P4-03 | Dry-run: 91234 매칭 | PASS | 5 | matched=True rule_id=11 |
| P4-04 | Dry-run: 1234 비매칭 | PASS | 6 | matched=False |
| P4-05 | Reject 규칙 생성 (priority 10) | PASS | 20 | id=12 |
| P4-06 | Priority 검증 (reject 먼저 매칭) | PASS | 6 | matched=True rule_name=_vruntime_reject-scanner expected=_vruntime_reject-scanne |
| P4-07 | 규칙 삭제 후 비매칭 | PASS | 555 | matched=False |
| P5-01 | ACL 목록 조회 | PASS | 5 | status=200 |
| P5-02 | deny ACL 생성 (TEST-NET-3) | PASS | 16 | id=10 |
| P5-03 | CIDR ACL 생성 | PASS | 17 | id=11 |
| P5-04 | Invalid match_type 거부 | PASS | 3 | status=400 |
| P5-05 | ACL 삭제 | PASS | 59 | remaining=0 |
| P6-01 | 서비스 상태 API | PASS | 37 | status=200 |
| P6-02 | CMP restart via API | PASS | 2106 | status=200 rc=0 |
| P6-03 | CMP 재시작 후 running 상태 | PASS | 2041 | status=200 len=326 |
| P6-04 | 알 수 없는 서비스 거부 | PASS | 3 | status=400 |
| P6-05 | 감사 로그에 RESTART 기록 | PASS | 1 | audit rows=4 |

## 실패 항목 분석

### CSP-IF-01 — stats 요청
- **상세:** 응답 없음 (timeout) — CSP가 최신 바이너리인지 확인

### CSP-TMR-01 — 타이머 설정값 확인
- **상세:** 응답 없음 (timeout)

### CSP-CALL-02 — PTT 그룹 통화
- **상세:** stats={}

### VoLTE-CALL-01 — 1:1 통화 + 대시보드/이력 정합성
- **상세:** cspsim CallOk>=1:OK | 종료 후 active_calls=0:OK | 종료 후 active_voip 비어있음:OK | 종료 후 registered=0:NG | 서비스상태 VoIP 접속자=0:OK | 이력 존재:OK | 이력 callee 정확:OK | 이력 state=ended:OK

### VoLTE-CALL-02 — 통화 중 실시간 대시보드/상태 정합성
- **상세:** 등록 후 VoIP 접속자>=1:OK | 통화 중 active_calls>=1:OK | active_calls<=registered/2:OK | 종료 후 active_calls=0:OK | 종료 후 registered=0:NG

### VoLTE-DASH-05 — 시험 종료 후 잔류 데이터 없음 확인
- **상세:** registered=0:NG | active_calls=0:OK | active_voip 비어있음:OK | ringing 잔류 없음:OK | VoIP 접속자=0:OK (reg=3, calls=0, voip_active=0, online=0)

### VoLTE-TMR-01 — CSP 타이머 설정 활성 상태
- **상세:** CSP 응답 없음

### PTT-CALL-01 — 그룹 통화 중 실시간 대시보드/상태 + 이력 정합성
- **상세:** 통화 중 active_ptt>=1:OK | 통화 중 registered>=4:OK | 통화 중 RTP 포트 사용>0:NG | 통화 중 PTT 접속자>=4:OK | 통화 중 그룹참여>=1:NG | 종료 후 registered=0:OK | 서비스상태 PTT 접속자=0:OK | 서비스상태 PTT 그룹참여=0:OK | PTT 이력 존재:NG

### PTT-CALL-02 — Conference NOTIFY 수신 확인
- **상세:** ConfNotify=0, CallOk=None

### PTT-DASH-06 — 시험 종료 후 잔류 데이터 없음 확인
- **상세:** PTT 접속자=0:NG | PTT 그룹참여=0:NG | active_ptt 비어있음:NG (online=3, in_grp=3, active_ptt=1)

### PTT-TMR-01 — CSP 타이머 설정 상태
- **상세:** CSP 응답 없음

### MEDIA-RTP-03 — AMR-WB RTP 전송 + PTT 그룹콜
- **상세:** 미디어 로딩:NG | 그룹콜 성공:NG

### MEDIA-REC-01 — CSP 녹취 설정 활성화 상태
- **상세:** CSP 응답 없음

### MEDIA-REC-02 — CMP 녹취 raw 파일 기록 확인
- **상세:** file=_vtest_rec_1776667042_a.rtp, size=0

### MEDIA-REC-03 — 녹취 raw 파일 RTP 정합성
- **상세:** 비어있지 않은 녹취 파일 없음

### MEDIA-REC-04 — 녹취 raw 파일 내용 확인
- **상세:** rtp_files=0, non_empty=0, total_size=0

### MEDIA-TRANS-03 — RTP→AMR-WB 스트리핑 동작 확인
- **상세:** 녹취 파일 없음

---
*리포트 생성: 2026-04-20 15:37:34*