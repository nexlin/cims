# Phase 1 재재검증 — volte/ptt 명명 통일 + 실제 도메인 + 이슈 해결

**일시**: 2026-04-22 21:53
**자동 리포트**: `verify_reports/20260422_215041_phase1.md`
**합격 판정**: **✅ PASS**

---

## 이번 세션 해결 항목

### 1. Console 로그인 500 → 200
- CSC `auth.py`/`admin.py` 의 `service_id` 컬럼 참조를 `service_ref` 로 교체
- 로그인 응답 구조 분리 (`/auth/login`, `/users/me`, `/users/me/subscriptions`)

### 2. 전 모듈 기동 + Agent 고려
- verify phase1 §5 에 `cwrtc console phone` 기동 추가
- Agent 는 install-agent.sh 필요로 별도

### 3. 모듈관리 UI 누락 → 해결
- verify phase1 §5.5 에 pkg auto-upload (admin JWT 로 모든 tarball POST /api/v1/packages)
- 8개 package 업로드 완료, cims_package 테이블 채워짐

### 4. VoLTE Flow 에서 CSP 메시지 누락 → 해결
- `SipMessageLogger.cpp` 의 subid 조건 "phone||volte" → "volte" 로 통일
- CSP SIP 메시지가 Call-ID subid 를 flow.jsonl 에 기록 → Flow 페이지에서 조회 가능

### 5. VoLTE Flow 에 PTT 호 섞임 → 해결
- AccessService domain 분리 (voip_ref=ims.mnc033..., ptt_ref=ptt.mnc033...)
- `BuildDomainToKindMap` 이 한 domain 에 한 kind 만 매핑 → service 분류 명확
- CMP/CSC 메시지 필터링이 sesid_set 기반으로 작동

### 6. 명명 통일 (voip/phone → volte)
- **DB 테이블 rename** (`sql/migrate_voip_to_volte.sql`):
  - `voip_subscriptions` → `volte_subscriptions`
  - `voip_call_logs` → `volte_call_logs`
  - `voip_call_participants` → `volte_call_participants`
- **C++**: CspUser/SipMessageLogger/DbManager/CallDir/CspServiceMap/CscfModule/ModuleDispatcher/CspServer 등에서 "voip"/"phone" → "volte"
- **cspsim**: serviceType/ filterMode 전부 "volte"
- **Python**: handlers 전체 (auth/admin/users/csp_runtime/stats/service_control/flow_logger)
- **Console/Phone TS**: RecordingsPage, StatsPage, CallLogsPage 등 문자열 리터럴
- **config_template.json**: kind enum `["voip","ptt"]` → `["volte","ptt"]`
- **cims.sh**: verify seed 도메인, subscription 테이블명

### 7. 실제 도메인 반영
- VoLTE: `ims.mnc033.mcc450.3gppnetwork.org`
- PTT:   `ptt.mnc033.mcc450.3gppnetwork.org`
- configure.sh 기본값 + verify seed 모두 적용

### 8. 부수 — SIGUSR1 재로드 타이밍 race 수정
- verify 7.0 seed 후 `time.sleep(2)` 로 CSP ReloadFromJsonl 대기
- 덮어쓰기 모드 (`'w'`) 로 이전 kind 레코드 제거

---

## 단계별 결과

| 단계 | 결과 | 비고 |
|---|---|---|
| 1.1 Reset | ✅ | 25 테이블 TRUNCATE + 보존 7 테이블 (이름 변경 반영) |
| 1.2 Build | ✅ skip-build | dist 에 새 바이너리 기 배포 |
| 1.3 Configure | ✅ | ims.mnc033 도메인 반영 |
| 1.4 Start | ✅ | cmp/csp/csc/cwrtc/console/phone 전 6 모듈 |
| 1.5 Health | ✅ | ERROR/FATAL 0 |
| **1.6 §5.5 pkg upload** | ✅ | 8 tarball 업로드 OK |
| 1.6 §7.1 VoIP 2자 | ✅ | **2/2 REGISTER, 1/1 통화 (1003ms), 녹취 4** |
| 1.6 §7.2 PTT 5명 | ✅ | **5/5 REGISTER, Conf NOTIFY v11, 녹취 10** |
| **총 녹취** | ✅ | **14 파일 정상** |

---

## Console 로그인 + /users/me + /users/me/subscriptions 실 응답

```
POST /auth/login {login_id:"admin", password:"1234"}
→ 200  {token, user:{id,name,login_id,role}}

GET /users/me (Bearer token)
→ 200  {id,name,role,org_id,create_time,...}

GET /users/me/subscriptions (Bearer token)
→ 200  {
    call_subscriptions: [],
    ptt_subscriptions: [{
      id:"+821030432632",
      service_ref:"ptt-default",
      imsi:"jcryu74",
      domain:"ptt.mnc033.mcc450.3gppnetwork.org",
      auth_id:"jcryu74@ptt.mnc033.mcc450.3gppnetwork.org",
      passwd:"1234", ...
    }]
  }
```

---

## 주요 파일 변경

**SQL migration (신규 2개)**
- `sql/migrate_subscriptions_service_ref.sql`
- `sql/migrate_voip_to_volte.sql`

**C++ (13 파일)**
- CspUser.h, CspServer.cpp, CscfModule.cpp, CspServiceMap.{h,cpp}
- DbManager.{h,cpp}, ModuleDispatcher.cpp, SipMessageLogger.{h,cpp}
- CallDir.h, GroupCallService.cpp, SipServerSetup.h

**Python (7 파일)**
- csc/src/handlers/auth.py, admin.py, users.py (신규), csp_runtime.py
- stats.py, service_control.py
- csc/src/services/flow_logger.py, mcptt.py

**TS (10 파일)**
- cims-console: api/auth.ts, users.ts, cspRuntime.ts, recordings.ts, stats.ts, calls.ts
- cims-console: pages (RecordingsPage, StatsPage, CallLogsPage, VolteHistoryPage, VolteMsisdnPage, SegmentPlayer)
- cims-phone: api/auth.ts, contexts/AuthContext.tsx, pages/PhonePage.tsx

**설정/스크립트**
- csp/config/csp.json.template (Setup.Realm/AuthRealm 제거)
- csp/config_template.json (kind enum → volte)
- configure.sh (기본 VOLTE_DOMAIN ims.mnc033)
- cims.sh (verify seed + pkg upload + domain + reset 보존 리스트)

**cspsim**
- CspsimMain.cpp: DB 쿼리 변경 (domain 하드코드 제거, -domain CLI 인자 기반 authId 조립)

---

## 남은 수동 검증 (Console 접속 후)

- [ ] 테스트베드 > 모듈관리: 업로드된 8개 패키지의 버전/설정 템플릿/설정 표시
- [ ] VoLTE 서비스 이력 → Flow 페이지: CSP SIP 메시지 포함 + 해당 호 sesid 만 (타 호 섞임 없음)
- [ ] 녹취 재생 (voip seg_*_va.rtp, ptt seg_*_audio.rtp)
- [ ] CSC 가입자/그룹 CRUD → NOTIFY 반영

---

## Phase 2 진입 준비

Phase 1 자동 + 수동 모두 통과 시 Phase 2 (배포 기능 검증) 진입 가능.
TB 3종 (TB-CSC 4419 / TB-Console 3000 / TB-agent 9902) 구축 후 수행 권장.
