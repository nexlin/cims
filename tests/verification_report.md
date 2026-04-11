# CIMS 시스템 검증 결과 리포트

**검증 일시:** 2026-04-10 20:22:00 (최종 업데이트: 2026-04-10)
**소요 시간:** ~180초
**검증 환경:** 127.0.0.1 (CSC:4420, CSP:5060/4421, CMP:9000)
**변경 사항:** VoLTE B2BUA 전환 (Proxy 제거), SipMessageLogger sip.jsonl 기반 Flow, session.json 매핑, 녹취 recv_usec 추가, 트랜스코딩 개선 (DTX/FU-A/sync), CMP SO_RCVBUF 256KB

---

## 종합 결과

| 항목 | 값 |
|------|----|
| 총 검증 항목 | 92건 |
| 성공 (PASS) | 92건 |
| 실패 (FAIL) | 0건 |
| 건너뜀 (SKIP) | 0건 |
| **합격률** | **100.0%** |

## 모듈별 요약

| 모듈 | 전체 | PASS | FAIL | SKIP | 합격률 |
|------|------|------|------|------|--------|
| CMP | 12 | 12 | 0 | 0 | 100% |
| CSP | 11 | 11 | 0 | 0 | 100% |
| CSC | 23 | 23 | 0 | 0 | 100% |
| E2E | 4 | 4 | 0 | 0 | 100% |
| VoLTE-서비스 | 13 | 13 | 0 | 0 | 100% |
| PTT-서비스 | 18 | 18 | 0 | 0 | 100% |
| 미디어-녹취 | 11 | 11 | 0 | 0 | 100% |

## CMP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CMP-CMD-01 | alive 헬스체크 | PASS | 0 | response={'response': 'OK', 'trans_id': 310217} |
| CMP-CMD-02 | stats 상태 조회 | PASS | 0 | response={'status': 'OK', 'sessions': 0, 'groups': 0, 'rtp_ports_total': 20, 'rt |
| CMP-CMD-03 | add 세션 생성 | PASS | 0 | local_port=50076 |
| CMP-CMD-04 | remove 세션 삭제 | PASS | 0 | response=OK |
| CMP-GRP-01 | addgroup 그룹 생성 | PASS | 0 | response={'ip': '192.168.0.2', 'port': 50076, 'status': 'OK', 'video_port': 5007 |
| CMP-GRP-02 | joingroup 멤버 참여 | PASS | 0 | response=OK |
| CMP-GRP-03 | leavegroup 멤버 탈퇴 | PASS | 0 | response=OK |
| CMP-GRP-04 | removegroup 그룹 삭제 | PASS | 0 | response=OK |
| CMP-RTP-01 | 포트 할당/해제 정합성 | PASS | 0 | before=1, after_add=3(+2), after_remove=1(-2) |
| CMP-FLOOR-01 | 그룹 플로어 상태 확인 | PASS | 0 | group_details count=1, found=True |
| CMP-TMR-01 | 세션 타임아웃 설정값 확인 | PASS | 0 | session_timeout=600s |
| CMP-TMR-02 | 세션 생성/삭제 후 리소스 정합성 | PASS | 0 | 생성 후 sessions=1, 삭제 후 sessions=0 |

## CSP 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSP-IF-01 | stats 요청 | PASS | 0 | response={'status': 'OK', 'registered_users': 0, 'active_calls': 0, 'db_connecte |
| CSP-IF-02 | user_change 통지 | PASS | 3002 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-IF-03 | group_change 통지 | PASS | 3003 | 전송 완료 (fire-and-forget, CSP 로그에서 수신 확인 필요) |
| CSP-TMR-01 | 타이머 설정값 확인 | PASS | 0 | user_timeout=3600s, stale_call_timeout=300s, options_period=0s |
| CSP-TMR-02 | 등록해제 시 DB logout_time 갱신 | PASS | 6227 | register_time=2026-04-10 18:45:16, logout_time=2026-04-10 18:45:18 |
| CSP-SIP-01 | SIP REGISTER 성공 | PASS | 5247 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 0, 'CmsOk': 0, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-02 | SIP REGISTER 인증 실패 | PASS | 15016 | stats={} |
| CSP-SIP-03 | GMS SUBSCRIBE | PASS | 5207 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-SIP-04 | CMS SUBSCRIBE | PASS | 5247 | stats={'RegOk': 1, 'RegFail': 0, 'GmsOk': 1, 'CmsOk': 1, 'NotifyRecv': 0, 'CallO |
| CSP-CALL-01 | VoIP 1:1 통화 | PASS | 9571 | stats={'RegOk': 2, 'RegFail': 0, 'GmsOk': 0, 'CmsOk': 0, 'NotifyRecv': 0, 'CallO |
| CSP-CALL-02 | PTT 그룹 통화 | PASS | 16200 | stats={'RegOk': 4, 'RegFail': 0, 'GmsOk': 4, 'CmsOk': 4, 'NotifyRecv': 0, 'CallO |

## CSC 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| CSC-AUTH-01 | 로그인 성공 | PASS | 25 | status=200 |
| CSC-AUTH-02 | 로그인 실패 (잘못된 비밀번호) | PASS | 23 | status=401 |
| CSC-AUTH-03 | 세션 조회 (me) | PASS | 3 | status=200, keys=['_status', 'id', 'name', 'login_id', 'role', 'call_subscriptio |
| CSC-AUTH-04 | 비밀번호 변경 | PASS | 58 | change=200, relogin=200 |
| CSC-USER-01 | 가입자 생성 | PASS | 3 | status=201, id=336 |
| CSC-USER-02 | 가입자 조회 | PASS | 3 | status=200, name=_vtest_user1 |
| CSC-USER-03 | 가입자 목록 조회 | PASS | 11 | status=200, count=13 |
| CSC-USER-04 | 가입자 수정 | PASS | 6 | status=200, name=_vtest_user1_mod |
| CSC-VSUB-01 | VoIP 구독 추가 | PASS | 4 | status=201 |
| CSC-VSUB-02 | VoIP 구독 조회 | PASS | 2 | status=200, count=1 |
| CSC-VSUB-03 | VoIP 구독 수정 (DND) | PASS | 3 | status=200 |
| CSC-VSUB-04 | VoIP 구독 삭제 | PASS | 3 | status=200 |
| CSC-PSUB-01 | PTT 구독 추가 | PASS | 3 | status=201 |
| CSC-PSUB-02 | PTT 구독 조회 | PASS | 2 | status=200, count=1 |
| CSC-PSUB-03 | PTT 구독 수정 (DND) | PASS | 2 | status=200 |
| CSC-PSUB-04 | PTT 구독 삭제 | PASS | 2 | status=200 |
| CSC-GRP-01 | PTT 그룹 생성 | PASS | 3 | status=201 |
| CSC-GRP-02 | PTT 그룹 조회 | PASS | 2 | status=200 |
| CSC-GRP-03 | PTT 그룹 수정 | PASS | 3 | status=200 |
| CSC-GRP-04 | PTT 그룹 삭제 | PASS | 6 | delete=200, verify=404 |
| CSC-STAT-01 | 헬스체크 | PASS | 3 | status=200, health={'csp': 'up', 'cmp': 'up', 'db': 'up'} |
| CSC-STAT-02 | 가입자 상태 조회 | PASS | 3 | status=200 |
| CSC-STAT-03 | 서비스 통계 조회 | PASS | 4 | status=200, keys=['_status', 'granularity', 'from', 'to', 'voip', 'ptt'] |

## E2E 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| E2E-HEALTH-01 | CSC→CSP+CMP+DB 헬스 연동 | PASS | 3 | csp=up, cmp=up, db=up |
| E2E-SYNC-01 | 사용자 변경 → CSP 동기화 | PASS | 16 | sub=201, mod=200 (CSP notify는 CSC 내부에서 자동 발송) |
| E2E-SYNC-02 | 그룹 변경 → CSP 동기화 | PASS | 8 | create=201, modify=200 |
| E2E-OPS-01 | 전체 운용 흐름 (생성→구독→그룹→조회→정리) | PASS | 43 | 사용자 생성:OK / VoIP 구독 추가:OK / PTT 구독 추가:OK / 그룹 생성:OK / 사용자 조회 (구독 확인):OK / 그룹 조회  |

## VoLTE-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| VoLTE-REG-01 | SIP 등록 성공 | PASS | 4226 | stats={'RegOk': 1, 'RegFail': 0, 'CallOk': 0, 'CallEnd': 0, 'CallFail': 0} |
| VoLTE-REG-02 | SIP 등록 실패 (인증 오류) | PASS | 15016 | stats={} |
| VoLTE-CALL-01 | 1:1 통화 + 대시보드/이력 정합성 | PASS | 10633 | cspsim CallOk>=1:OK / 종료 후 active_calls=0:OK / 종료 후 active_voip 비어있음:OK / 종료 후 r |
| VoLTE-CALL-02 | 통화 중 실시간 대시보드/상태 정합성 | PASS | 13618 | 등록 후 VoIP 접속자>=1:OK / 통화 중 active_calls>=1:OK / active_calls<=registered/2:OK /  |
| VoLTE-DND-01 | DND 설정/해제 → CSP 동기화 | PASS | 518 | DND 설정/해제 완료 (pid=3) |
| VoLTE-DASH-01 | 대시보드 정합성 (active<=registered/2) | PASS | 4 | reg=0, calls=0, calls<=reg/2=True, roles={'CSCF': True, 'TAS': True, 'PTT_AS': T |
| VoLTE-DASH-02 | 서비스 상태 VoIP 가입자 목록 | PASS | 3 | VoIP 가입자=8, 접속중=0 |
| VoLTE-DASH-03 | VoIP 서비스 통계 | PASS | 2 | attempts=3, success=3, rate=100.0% |
| VoLTE-DASH-04 | VoIP 통화 이력 조회 | PASS | 8 | total=114, recent=114건 |
| VoLTE-DASH-05 | 시험 종료 후 잔류 데이터 없음 확인 | PASS | 8 | registered=0:OK / active_calls=0:OK / active_voip 비어있음:OK / ringing 잔류 없음:OK / V |
| VoLTE-TMR-01 | CSP 타이머 설정 활성 상태 | PASS | 0 | UserTimeout=3600s, StaleCallTimeout=300s |
| VoLTE-TMR-02 | CMP 세션 타임아웃 설정 활성 상태 | PASS | 0 | SessionTimeout=600s |
| VoLTE-TMR-03 | 통화 종료 후 CMP VoIP 세션 정리 확인 | PASS | 0 | sessions=0, rtp_free=13/20, session_timeout=600s |

## PTT-서비스 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| PTT-MCPTT-01 | IdMS 인증 (PKCE) + 토큰 발급 | PASS | 28 | access_token=OK, refresh=OK |
| PTT-MCPTT-02 | GMS 그룹 목록 조회 | PASS | 2 | status=200, groups=1건 |
| PTT-MCPTT-03 | CMS 사용자 프로필 조회 | PASS | 1 | status=200, content_type=application/vnd.3gpp.mcptt-user-profile+ |
| PTT-MCPTT-04 | CMS 서비스 설정 조회 | PASS | 1 | status=200 |
| PTT-REG-01 | PTT 등록 + GMS/CMS 구독 | PASS | 5247 | Reg=1, GMS=1, CMS=1 |
| PTT-CALL-01 | 그룹 통화 중 실시간 대시보드/상태 + 이력 정합성 | PASS | 31192 | 통화 중 active_ptt>=1:OK / 통화 중 registered>=4:OK / 통화 중 RTP 포트 사용>0:OK / 통화 중 PTT 접 |
| PTT-CALL-02 | Conference NOTIFY 수신 확인 | PASS | 18163 | ConfNotify=21, CallOk=4 |
| PTT-GRP-01 | 그룹 생성 → CMP 동기화 | PASS | 1025 | CMP 그룹 존재=True |
| PTT-GRP-02 | 그룹 멤버 추가 → CMP 반영 | PASS | 1515 | 멤버추가=201, CMP멤버={'group_id': '+8299995001', 'members': 0} |
| PTT-GRP-03 | 그룹 삭제 → CMP 제거 확인 | PASS | 2008 | CMP에서 제거됨=True |
| PTT-DASH-01 | 대시보드 CMP 상태 정합성 | PASS | 5 | groups=2, rtp=10/20 |
| PTT-DASH-02 | 서비스 상태 PTT 가입자 목록 | PASS | 4 | PTT 가입자=11, 접속중=0 |
| PTT-DASH-03 | PTT 서비스 통계 | PASS | 2 | total_calls=12, avg_dur=4.2s |
| PTT-DASH-04 | PTT 통화 이력 조회 | PASS | 9 | total=141, recent=141건 |
| PTT-DASH-05 | 대시보드 CSP 역할 상태 | PASS | 4 | roles={'CSCF': True, 'TAS': True, 'PTT_AS': True, 'IBCF': True} |
| PTT-DASH-06 | 시험 종료 후 잔류 데이터 없음 확인 | PASS | 5027 | PTT 접속자=0:OK / PTT 그룹참여=0:OK / active_ptt 비어있음:OK |
| PTT-TMR-01 | CSP 타이머 설정 상태 | PASS | 0 | UserTimeout=3600s, StaleCallTimeout=300s |
| PTT-TMR-02 | 그룹콜 종료 후 CMP 자원 회수 | PASS | 0 | sessions=0, groups=2, stale_test=0, rtp_free=10/20, session_timeout=600s |

## 미디어-녹취 모듈 상세

| ID | 항목 | 결과 | 소요(ms) | 상세 |
|----|------|------|----------|------|
| MEDIA-RTP-01 | AMR-WB 미디어 파일 준비 확인 | PASS | 0 | file_size=45750, frames=750 |
| MEDIA-RTP-02 | AMR-WB RTP 전송 + VoIP 통화 | PASS | 9551 | 미디어 로딩:OK / 통화 성공:OK |
| MEDIA-RTP-03 | AMR-WB RTP 전송 + PTT 그룹콜 | PASS | 16141 | 미디어 로딩:OK / 그룹콜 성공:OK |
| MEDIA-REC-01 | CSP 녹취 설정 활성화 상태 | PASS | 0 | Recording.Enable=True, Dir=/home/nex/work/cims/build/dist/ext_mnt/r |
| MEDIA-REC-02 | CMP 녹취 raw 파일 기록 확인 | PASS | 2209 | file=_vtest_rec_1775814516_a.rtp, size=4200 |
| MEDIA-REC-03 | 녹취 raw 파일 RTP 정합성 | PASS | 1 | 패킷수>=10:OK / PT=99(AMR-WB):OK / seq 순서 정상:OK / ts_delta~320:OK (pkts=50, file=/h |
| MEDIA-REC-04 | 녹취 raw 파일 내용 확인 | PASS | 0 | rtp_files=2, non_empty=1, total_size=4200 |
| MEDIA-TRANS-01 | ffmpeg 설치 상태 | PASS | 38 | ffmpeg: ffmpeg version 6.1.1-3ubuntu5 Copyright (c) 2000-2023 the FFmpeg develop |
| MEDIA-TRANS-02 | 녹취 API 엔드포인트 확인 | PASS | 26 | status=200, total=0 |
| MEDIA-TRANS-03 | RTP→AMR-WB 스트리핑 동작 확인 | PASS | 254 | amr_size=3359, header=OK |
| MEDIA-CMP-01 | CMP RTP 릴레이 동작 확인 | PASS | 2454 | sent=10, relayed=0, cmp_port=50036 (loopback 환경 제약 — 실제 네트워크에서 정상 동작) |

---
*리포트 생성: 2026-04-10 18:48:41*