# Phase C — 후속 작업

> 2026-04-22 업데이트: 🔴 필수 수정 3건 모두 완료.
> 남은 항목은 Phase C 본과제 (#4~#6) + 이후 검토 항목 (#7~#11).

## ✅ 완료된 필수 수정 (2026-04-22)

### ✅ 1. Manager id 타입 불일치 — 해결됨
`include/SimpleJson.h` 근처에 두기 애매해서 `CspConfigCache.h` 에 inline helper 추가:
`CspUuidToIntId(uuid)` → std::hash<string> 31-bit masking.
4개 매니저에서 `row.GetInt("id")` → `CspUuidToIntId(row.GetString("id"))` 로 변경.
로그에 `id=1256475311` 같은 full hash 확인됨.

### ✅ 2. CSP scalar config 연결 — 해결됨
`SipServerSetup::Read()` 에 deployment overlay 로직 추가.
`install_path/config.json` (flat `{"Setup.Sip.AuthRealm": "..."}` 형태) 를
재귀 dot-path setter 로 root 에 merge.
로그: `SipServerSetup: overlay config/../../config.json applied (17 keys)`.
CLog 초기화 전에 Read() 가 호출되므로 overlay 결과를 setup 멤버에 저장했다가
SIPServerStart 에서 로그 출력.

### ✅ 3. SIGUSR1 e2e 검증 — 해결됨
cims.sh 의 start_csp 가 이미 `run/csp.pid` 에 pid 를 기록한다는 것을 확인.
Agent → PUT collection → `signaled:[<pid>]` 응답 정상 동작 검증 완료.

---

## 🔴 과거 필수 수정 원본 기록 (참고용)

### 1. Manager 측 id 타입 불일치 (listener/trunk/route/acl)
**문제**:
- jsonl 의 record `id` 는 16 hex char UUID 문자열 (`"5994a45560f24413"`)
- C++ 매니저들은 `row.GetInt("id")` 로 int 파싱 → UUID 앞 숫자만 추출되거나 0
- 결과: 같은 레코드가 매번 다른 관리 ID 로 인식 → remove/add 반복될 가능성

**증거**: CSP 로그
```
ListenerManager: id=5994 0.0.0.0:5060 already bound by bootstrap — skip
ListenerManager: skip non-UDP id=4 proto=TCP     ← r2 UUID "4e82e7e..." 의 "4"
```

**영향받는 파일**:
- `csp/CspListenerManager.cpp:57` — `m.id = (int)row.GetInt("id");`
- `csp/CspTrunkManager.cpp:70`
- `csp/CspAccessControl.cpp:46`
- `csp/CspRouteEngine.cpp:97`

**수정안**: 각 매니저의 내부 관리 ID 타입을 `std::string` 으로 변경 (UUID string 그대로 사용). 또는 std::hash<std::string> 으로 int 해시 값을 내부용으로 사용.

### 2. CSP PID 파일 → SIGUSR1 경로
**문제**: Agent 는 `install_path/run/*.pid` 를 찾아 SIGUSR1 전송. cims.sh 로 start 시 `install_path/run/csp.pid` 에 자동 기록됨 **확인됨** (cims.sh 내부 save_pid 로직).

→ 실제 배포에서 동작하는지 **end-to-end 검증 필요**: `agent install → agent start (queue job) → pid 파일 생성 확인 → collection PUT → SIGUSR1 수신 → 리로드` 전 사이클.

### 3. scalar config.json 이 CSP 에 실제 적용되지 않음
**문제**: 템플릿 sections 에서 저장한 값은 `install_path/config.json` 에 기록되지만, CSP 는 여전히 `--config csp.json` (tarball 내 config/csp.json) 만 읽음. 
→ **scalar template 값이 실제 런타임에 반영 안 됨**.

**해결 옵션**:
- A. cims.sh 가 start 시 template config.json 의 값으로 csp.json 을 merge/overlay → CSP 에 전달
- B. CSP 기동 인자로 `--overlay install_path/config.json` 지원 + CSP 가 merge
- C. CSP 가 기동 시 install_path/config.json 을 추가로 읽어 csp.json 과 deep-merge

→ 옵션 C 가 가장 깔끔. `SipServerSetup` 에 overlay 로직 추가.

## ✅ Phase C 완료 과제 (2026-04-22)

### ✅ 4. CSP 의 DB/HTTP pull 모드 완전 제거 — 완료
- `CspConfigCache`: `_httpGet/RefreshEntity/RefreshAll/_loadFromFile/_saveToFile/_applyPullResponse` 모두 제거 → jsonl 전용.
- `Init()` 시그니처 단순화: `Init(const std::string& jsonlDir)` 하나만.
- `CscInterface`: 5 개 config-change 이벤트(LISTENER_CHANGED 등) 핸들러 제거.
- `CspServer.cpp` 메인 루프: SIGUSR1 처리 분기 제거 (jsonl 단일 경로).

### ✅ 5. Agent sync REST mTLS 전환 — 완료 (opt-in)
- `csc.json` 의 `Agent.MtlsEnabled: true` 설정 시 활성화.
- CSC 자체 CA 자동 생성 (`cert/agent_mtls/ca.{crt,key}` + `csc_client.{crt,key}`).
- Enroll 시 agent 별 server cert 발급, 응답에 `mtls.server_cert/server_key/ca_cert` 포함.
- Agent 는 state 에 저장 후 sync REST 를 `CERT_REQUIRED + CA` 로 기동.
- CSC proxy (`_agent_proxy_call`) 는 csc_client 로 mTLS 연결.
- **주의**: MtlsEnabled 전 enroll 된 agent 는 mTLS 없음 → 해당 agent 는 mtls_enabled 컬럼(향후) 으로 per-agent 분기 필요.

### ✅ 6. deprecated 테이블 rename — 완료
- `csp_listener`, `sip_trunk`, `routing_rule`, `routing_access_list`, `sip_service` → `*_deprecated` (5개).
- 충분한 운영 후 DROP 예정.

## 🟢 이후 검토 항목 (우선순위 낮음)

### 7. Collection UI 개선
- `records` 전체 치환 방식 → 대용량 시 부담. 행 단위 PATCH API 고려 (`POST /collection/{name}/row` 등)
- "변경 감지" 시각화: 저장하지 않은 행 ● 표시
- 고급 필드 토글이 탭 전체 대신 행 단위로 적용되게
- drag-n-drop 으로 priority 재정렬 (routes/acl 에 유용)

### 8. Deployment 의 sync_port 미보고 상태 처리
- Agent heartbeat 이전엔 `sync_port=NULL` → collection API 호출 시 502
- UI 쪽에서 "Agent sync 준비 중" 안내 필요 (현재는 오류 메시지만)

### 9. CSP 재시작 없이 scalar 설정 즉시 반영 (`restart:false` 항목)
- 현재: 재기동 경고만 표시, 실제 핫리로드 지원 X
- LogLevel 등 일부 필드는 SIGHUP 처리 가능

### 10. UI 문제 가능성 (실기 확인 필요)
- 콘솔에서 "모듈 추가 → process 선택 → functions 체크" UI 실제 동작
- Collection 편집 후 저장 → signaled 배열이 비어있으면 "프로세스 기동 중이 아닙니다" 안내
- 배포 상태 (pending/stopped/running) 에 따른 버튼 활성/비활성화 룰

### 11. cspsim/console/phone/cwrtc/agent 용 config_template.json 작성
- 현재 csp/cmp/csc 만 템플릿 있음
- 최소한 `agent`, `console` 은 있으면 운영 편의성 ↑

## 📋 테스트 체크리스트 (내일 오전)

아래 순서로 수행해서 실제 동작을 확인하는 게 좋을 듯:

```
[ ] deployment #43 (csp) 에 start job 큐잉 (콘솔: ▶ Start 버튼)
[ ] install_path/run/csp.pid 가 자동 생성되는지 확인
[ ] 콘솔 Collection 탭에서 listener 추가 → 저장
[ ] signaled 배열에 pid 가 들어가는지 확인
[ ] CSP 로그에 "SIGUSR1: reloading jsonl config" 확인
[ ] 새 listener 포트가 실제로 bind 되는지 `ss -unlp | grep csp` 확인
→ 여기서 실패하면 위 "필수 수정 #1" 이 원인
```

## 🗂️ 관련 파일 인덱스

- 본 문서: `docs/PHASE_C_TODO.md`
- 구현 상세: `docs/design/02_deployment.md`, `docs/design/features/sip_runtime_config.md`
- API: `docs/api/collection_api.md`, `docs/api/agent_api.md`
- 이전 sessions 마이그레이션:
  - `sql/migrate_package_config_template.sql` (적용됨)
  - `sql/migrate_agent_sync_port.sql` (적용됨)
  - `sql/migrate_deprecate_csp_runtime_tables.sql` (**미적용 — Phase C 끝난 뒤**)

## 🔧 현재 동작 상태 요약 (2026-04-22 업데이트)

| 컴포넌트 | 상태 | 비고 |
|---|---|---|
| Package upload + meta/template 추출 | ✅ 동작 | csp/cmp/csc 에 template |
| Agent enroll/heartbeat/report | ✅ 동작 | sync_port 보고 포함 |
| Agent sync REST 9900 | ✅ 동작 | jsonl read/write + SIGUSR1 |
| CSC collection 프록시 | ✅ 동작 | validation + UUID auto-id |
| 프론트 Collection 편집 UI | ✅ 빌드/타입 통과 | 실기 검증 미완료 |
| CSP jsonl 로더 + SIGUSR1 | ✅ 동작 | ID 파싱 수정 완료 |
| CSP scalar config overlay | ✅ 동작 | 17 키 적용 확인됨 |
| 배포 start/stop/restart e2e | ✅ 검증됨 | install → start → 설정저장 → 재기동 정상 |
