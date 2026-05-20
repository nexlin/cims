---
name: CIMS TB 3종 인프라 알려진 이슈 (다음 세션 착수 목록)
description: 2026-04-27 갱신. I1/I2/블록A/I7/I8 해결. I3~I6 남음 (Minor). Test-Console 8080 로그인 불가는 구조적 한계.
type: project
originSessionId: cd6b1a78-c813-4476-94ff-9e4c03c1033d
---
## ✅ I1. reset 후 TB-agent 재enroll 필요 (해결 2026-04-24, commit 1e663c1)

해결: `cims.sh cmd_reset` 의 DB TRUNCATE 리스트에서 `cims_agent` 제거 후
`DELETE FROM cims_agent WHERE name <> 'tb-agent-local'` 로 전환.
`CIMS_TB_AGENT_NAME` env 로 override 가능. docs §0.3 에 "보존(TB-agent 레코드)" 행 추가.
검증: reset --db 후 tb-agent-local 보존, 1126 heartbeat 중 401 0건.

## ✅ I2. admin 기본 계정 부트스트랩 scheme 불일치 (해결 2026-04-24, commit 1e663c1)

해결: `sql/migrate_auth.sql` 재작성 (idempotent) —
- login_id/password/role 컬럼 IF NOT EXISTS 로 보장
- 레거시 email 컬럼 존재 시 값 복사 후 DROP (DO 0 로 no-op 분기)
- login_id UNIQUE 인덱스 (uq_login_id) 추가
- 기본 admin 계정 (admin/1234 — tests/test_env.json 일치) ON DUPLICATE KEY UPDATE
검증: TB-CSC(4419) /api/v1/auth/login admin/1234 → 200 + JWT.

## ✅ Phase 2 v2 — start/health/stop 자동화 (해결 2026-04-24, commit 7dad064)

Phase 2 v1 (89b2db2) 이 install-only 였음. v2 에서 end-to-end 완성.

해결 요약:
- `POST /api/v1/deployments` 가 `config` 필드 수용 → `agent_deployment.config_json` 저장 → `_queue_job` 이 자동으로 params.config 전달.
- `job_health_check` 가 params.config 의 Server.Port / ServerPort override 지원 (flat dot-path 수용).
- `cims.sh start_csc/stop_csc` 가 install_path/config.json 의 Server.Port 를 읽어 기동 (overlay-aware). DIST_DIR 포함 절대경로 pattern 으로 Phase 1/2 csc 상호 kill 방지.
- `_verify_phase2` 에 §12 overlay 검증 + §13 start(4445 LISTEN) + §14 health_check(tcp:4445=open) + §15 stop 단계 추가.
- **포트 선택**: Phase 2 csc start 는 **Server.Port=4445** overlay 로. cwrtc(8080), csc-mcptt(4430), tb-csc-mcptt(4431), Phase 1 Test-CSC(4421), 배포본 운영(4420) 어느 것과도 충돌 없음.
- **Test-agent --heartbeat-sec 3** 으로 job pickup 지연 최소화.
- 3회 연속 PASS (verify_reports/20260424_115517~115601_phase2.md).

### 중요 워크플로 노트
- verify phase2 --skip-pkg 사용 시 **소스 수정 후 반드시 `cims.sh sync all` + pkg 재생성** 필요. tarball 속 cims.sh/cims_agent.py 가 stale 하면 start/health 가 옛 로직으로 동작.
- TB-CSC 는 소스 변경 후 반드시 재기동 (csc_app.py 프로세스 교체). dist 의 `csc/src/` 로부터 import 되므로.

## ✅ Phase 1 Test-* 포트 전환 + Console 3분화 (해결 2026-04-24, commits 4f53b7d → e0c44a7)

| 역할 | 이전 | 현재 (e0c44a7) |
|---|---|---|
| Test-CSC (build/dist/csc/) | 4420 | **4421** |
| Dev-Console (소스 vite dev) | 3001 → 3011 | **3001** (e0c44a7 에서 복원) |
| Test-Console (dist HTTPS serve) | — | **8080** (cwrtc 8443 이전 후 단독) |
| 배포본 console (운영) | — | 80 (cap_net_bind 설계 후) |
| 배포 대상 csc (Phase 2 csc-server/csc/) | 4420 | 4420 유지 |

**Console 3분화** (e0c44a7): `cims.sh start_console` 가 SRC_CONSOLE 존재 여부로 Dev (3001) ↔ Test (8080) 자동 분기. 4f53b7d 의 "Test-Console 3011" 은 Dev/Test 통합 오독으로 부분 롤백.

### 추가 주의
- CSP `CscInterface` 가 **UDP 4421** 상시 리슨 (`csp/CspServer.cpp:259`). Test-CSC 는 **TCP 4421** 이라 proto 분리로 충돌 없음. 혼동 주의 — 번호 재배치는 후속 과제.
- TB overlay 의 `KmsClientReqUrl` replace 는 `:4420/` 및 `:4421/` 둘 다 수용하도록 개선 (base csc.json 이 deploy_value 로 4420 유지).
- `cmd_sync` 가 이제 `csc/config/config_template.json` 도 dist 로 복사 (기존 I6 의 partial 해결).

## ✅ 블록 A. cwrtc 8080 → 8443 + Test-Phone WebRTC target 갱신 (해결 2026-04-24, commit d90a08c)

해결 완료:
- `cwrtc/config/cwrtc.json.template` `Setup.WsPort`: 8080 → 8443
- `configure.sh` `VITE_CWRTC_TARGET=...:8443`
- `cims.sh`: `_svc_port_proto cwrtc 8443:tcp`, start_cwrtc fallback 8443, reset/preflight 포트 리스트 갱신
- `cims-phone/vite.config.ts` cwrtcTarget default `wss://127.0.0.1:8443`
- `cims-phone/nginx.conf` `/cwrtc proxy_pass https://127.0.0.1:8443` (WSS)

검증: cwrtc 8443 LISTEN OK + UDP 5062 SIP UA UNCONN OK + Test-Console 8080 dist HTTPS 단독 기동 (curl 200) + verify phase2 PASS.

### ⚠ cwrtc StartServer failed 진단 결과
e0c44a7 시점의 `StartServer failed` 는 **8080 충돌이 아닌** SIP UA UDP 5062 bind 실패. 원인은 cwrtc.json `Setup.LocalIp` 가 stale 한 IP(192.168.0.2) → 호스트 ens160 (192.168.199.129) 와 불일치. configure 를 올바른 ens160 IP 로 재실행 후 정상 기동. project_ports.md 주의 항목으로 메모 보존.

## I3. TB-Console dist 배포 경로 미정 (Minor)

**현재 설계**: vite dev proxy 전용 (SRC_CONSOLE 필수). 운영 환경에서 nginx 로 배포하려면 dist + `/api → :4419` proxy conf 별도 필요.

**현 한계**: `cims.sh start tb-console` 이 dist 트리에서는 실행 불가. 소스 트리에서만.

**해결 옵션** (실측 진입 전 불필요):
- (a) `cims-console/nginx-tb.conf` 템플릿 추가 + `start tb-console` dist 모드 분기
- (b) vite preview 모드로 dist 서빙 + proxy 검증 (가능 여부 확인 필요)
- (c) 현 설계 유지 (TB = 개발/검증 환경 전용이라 명시)

docs/VERIFICATION_PROCESS.md 부록 A 의 "TB-Console 빌드 분기: `VITE_ADMIN_TARGET=https://127.0.0.1:4419 npm run build` → `build/dist/console-tb/dist`" 문구는 현 구현과 불일치. 문서 교정 필요 (dev 모드 전제 추가).

## I4. TB-CSC 가 McpttServer(4431) 도 기동 (Minor)

**증상**: `csc_app.py` 는 `Server` + `McpttServer` 두 개 서버 항상 기동. TB 용도에는 MCPTT 불필요하지만 포트 4431 에서 리슨.

**영향**: 포트/리소스 낭비만. 기능 영향 없음.

**해결 옵션**:
- `csc-tb.json` 에 `McpttServer.Enabled = false` 같은 플래그 추가 + csc_app.py 에서 enable 체크
- 또는 overlay 에서 `McpttServer` 키 전체 제거 → csc_app.py 가 기본값(4430) 으로 기동 → 이건 4430 점유하므로 더 나쁨. 기동 자체를 건너뛰는 플래그가 필요.

## I5. configure 재실행 시 JWT secret 랜덤 재생성 (Existing)

이번 세션과 무관. 기존 이슈. configure 를 돌리면 `csc.json` 의 JWT secret 이 새로 생성 → 기존 로그인 세션 전부 무효. 이번에 `csc-tb.json` 도 같은 규칙 상속. 운영상 configure 후엔 반드시 서비스 재기동.

## I6. csc_app.py 소스↔dist sync 수동 필요 (Existing)

이번 세션 중 발견 — `cims.sh sync csc` 나 `make dist` 없이 소스만 바꾸면 `build/dist/csc/src/csc_app.py` 는 stale. TB-CSC 기동 시 포트 반영 실패로 원인 추적에 시간 소요.

**해결**: `cims.sh sync` 에 csc 소스도 포함되어 있다면 명시적으로 실행 필요. 아니면 dev 모드에서 `ln -s` 로 dist↔src 심볼릭 고려.

## ✅ I7. TB-Console 모듈관리 → csc start 시 csc-tb.json env 누수 (해결 2026-04-27)

**증상**: TB-Console 의 `/testbed/modules` 에서 csc 시작 → returncode 0 인데 실제로는 4419/4431 bind 실패로 죽음. status 는 "실행 중" 으로 거짓 표시.

**원인**: TB-CSC 가 `CIMS_CSC_CONFIG=csc-tb.json` 환경변수로 떠있는 상태에서 `service_control.py` 의 `_invoke_cims_sh` 가 subprocess 호출 시 환경을 그대로 상속 → 자식 `cims.sh start csc` → `python3 csc_app.py` 도 csc-tb.json 읽음 → TB-CSC 와 같은 4419/4431 bind 시도 → 충돌.

**해결** (csc/src/handlers/service_control.py):
- `_BLOCKED_ENV_KEYS = {"CIMS_CSC_CONFIG", "CIMS_AGENT_SYNC_PORT"}`
- `_sanitized_env()` 헬퍼로 차단 키 제거한 환경 dict 생성
- `_run_cmd(env=...)` 파라미터 추가, `_invoke_cims_sh` + status 호출에 적용

**검증**: TB-CSC 재기동 후 모듈관리 csc start API → 자식 csc_app.py 가 base csc.json (4421/4430) 으로 정상 기동.

**관련**: 8080 (Test-Console `serve dist`) 는 별개 이슈로 `/api` proxy 부재 → 로그인 불가 (구조적 한계). dist UI 검증은 GET / 200 으로 충분.

## ✅ I8. start_console / start_phone npx PID race + stop_one stray 누락 (해결 2026-04-27)

**증상**: `cims.sh stop console` 후에도 8080 에 orphan node serve 가 남음. status 는 "중지됨" 으로 거짓 표시.

**원인**: `start_console` 의 `npx --yes serve dist ...` 백그라운드 실행 시 `save_pid console $!` 가 **npx wrapper PID** 저장. npx 가 자식 node 를 spawn 후 종료하면 cims.sh 추적 PID 는 stale, 실제 serve 는 reparent 되어 살아남음. 후속 stop 은 PID 파일 없으면 warn 후 무시.

**해결** (cims.sh):
- start_console / start_phone: `sleep 2` 후 `_pid_by_port "$port:tcp"` 로 실제 listener PID 갱신
- stop_console / stop_phone 신규: stop_one + 양 모드 포트(`vite.*cims-console`/`serve dist -l 8080`) kill_stray
- _stop_one 디스패처에 console/phone case 추가
- status_one: console 의 경우 보조 포트(Dev 3001 ↔ Test 8080) 도 확인 → orphan 시 "실행 중(stray)" 표시
