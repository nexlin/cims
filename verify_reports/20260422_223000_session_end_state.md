# 세션 종료 상태 — v3 SIP 재구조화 + 명명 통일

**종료 시각**: 2026-04-22 22:30
**브랜치**: `feature/sip-console-runtime` (미커밋, 마지막 commit `3efbbdb` 이후 ~98 파일 변경)

---

## 이번 세션 범위 요약

CSP 설정 구조를 **Telco 스타일 9 collection** (LocalNode / RemoteNode / Route / RouteSet / Rule / RuleSet / RoutingPolicy / AclPolicy / AccessService) 으로 재구조화하고, 로그인/프로파일/가입자 API 분리, 명명 `voip/phone` → `volte` 통일, 실제 3GPP 도메인 반영, 파일 기반 call log 체계로 정리.

---

## 검증 현황 (Phase 1 — PASS)

| 항목 | 결과 |
|---|---|
| 전 6 모듈 기동 (cmp/csp/csc/cwrtc/console/phone) | ✅ |
| VoLTE 2/2 REGISTER + B2BUA 통화 1/1 (setup 1003ms) + 녹취 4 | ✅ |
| PTT 5/5 REGISTER + Conference NOTIFY v11 + 녹취 10 | ✅ |
| Console `/auth/login` (admin/1234) + JWT 발급 | ✅ |
| `/users/me` 프로파일 + `/users/me/subscriptions` (auth_id 조립) | ✅ |
| 파일 기반 `/api/v1/call/logs?call_type=volte` 조회 | ✅ (이력 표시됨) |
| CSP/CMP/CSC ERROR/FATAL 로그 | 0 |
| psip `m_iListenerId` 확장 + AclPolicy scope=local_node 활성화 | ✅ |

---

## DB 상태 (현재)

| 테이블 | 상태 |
|---|---|
| `volte_subscriptions` (기 voip_subscriptions) | 8 rows, service_ref='volte' |
| `ptt_subscriptions` | 11 rows, service_ref='ptt' |
| `volte_call_logs` / `volte_call_participants` | **DROPPED** (파일 기반 SOT) |
| `ptt_call_logs` / `ptt_call_participants` | **DROPPED** |
| `sip_service_deprecated` / `sip_trunk_deprecated` 등 | 유지 (다음 정리 대상) |

---

## 파일 구조 변경

**신규 (미커밋)**
```
csp/CspLocalNodeMap.{h,cpp}
csp/CspRemoteNodeMap.{h,cpp}
csp/CspRouteMap.{h,cpp}
csp/CspRouteSetMap.{h,cpp}
csp/CspRuleEvaluator.{h,cpp}
csp/CspRoutingPolicyEngine.{h,cpp}
csp/CspAclPolicyEngine.{h,cpp}
csc/src/handlers/users.py
csc/src/handlers/modules.py
sql/migrate_subscriptions_service_ref.sql
sql/migrate_voip_to_volte.sql
sql/migrate_drop_call_logs.sql
tools/migrate_csp_sip_config_v3.py
```

**삭제**
```
csp/CspRouteEngine.{h,cpp}
csp/CspTrunkManager.{h,cpp}
csp/CspAccessControl.{h,cpp}
```

**이동**
```
csp/config_template.json → csp/config/config_template.json
cmp/config_template.json → cmp/config/config_template.json
```

**수정 (주요)**
- `csp/` 대부분 (CspServer, CscfModule, ModuleDispatcher, SipMessageLogger, DbManager, CallDir, RecordPath, CspUser, CspServiceMap, SipServerSetup, GroupCallService, ...)
- `ext/psip/SipParser/SipMessage.{h,cpp}` + `ext/psip/SipStack/SipStackComm.hpp`
- `csc/src/handlers/{auth,admin,users,stats,csp_runtime,service_control}.py`
- `csc/src/services/flow_logger.py`, `csc_app.py`
- `cims-console/src/api/{auth,users,cspRuntime,recordings,stats,calls}.ts` + 다수 page
- `cims-phone/src/{api/auth.ts, contexts/AuthContext.tsx, pages/PhonePage.tsx}`
- `csp/config/csp.json.template`, `csp/config_template.json` (v2), `configure.sh`, `cims.sh`
- 3 design doc (sip_service_model.md v3, sip_runtime_config.md v3, VERIFICATION_PROCESS.md)

---

## 도메인 / 식별자 정리

| 용어 | 값 |
|---|---|
| VoLTE domain | `ims.mnc033.mcc450.3gppnetwork.org` |
| PTT domain | `ptt.mnc033.mcc450.3gppnetwork.org` |
| access_services.name | `volte` / `ptt` |
| subscriptions.service_ref | `volte` / `ptt` (access_services.name 참조) |
| kind enum (access_services) | `volte` / `ptt` |

---

## 알려진 TODO (다음 세션 후보)

### 기능 방향 결정
1. **실시간 활성통화 조회** — `stats.py.active_voip/ptt` 현재 빈 stub. 파일 스캔 vs DB 상태 테이블 방향 결정 필요.
2. **csp.json.template 통합** — `config_template.json` 단일화 + `hidden` 속성 + `configure.sh` 재작성 (A안). 범위 커서 별도 세션.

### 마감 작업
3. **커밋** — 현재 uncommitted 98 파일. 단일 feat 커밋 vs 분할.
4. **구 deprecated 테이블 DROP** — `sip_service_deprecated` 등 (migration 기존 준비됨, 실행만 남음).
5. `_agent_to_json` 직렬화에 `sync_port`, `mtls_enabled`, `cert_*_at` 필드 추가 (UI 표시용).
6. `install-agent.sh` 에 `--sync-port` CLI 인자 추가.

### Phase D 이후 잔여
7. `cims.sh start_csc` kill_stray 패턴 문제 (Phase D 의 #10)
8. Agent start job report status=0 (Phase D 의 #12)
9. Phase 2/3 진입 준비 — TB 3종 구축

---

## 다음 세션 첫 행동

1. `project_phase_status.md` (이 메모리) 읽기
2. `verify_reports/20260422_223000_session_end_state.md` (이 문서) 확인
3. 사용자가 우선순위 지정하면 해당 작업 시작
4. 커밋 여부 확인 (이번 세션 변경사항의 운명)

---

## 참고

- Phase 1 검증 통과 후 현재 6 모듈 계속 기동 상태:
  - cmp pid=968974, csp (restart 후 최신), csc (restart 후 최신), cwrtc, console (Vite dev), phone (Vite dev)
- VoLTE 호 이력 22:18 분 것이 Console `/api/v1/call/logs?call_type=volte` 에서 정상 조회됨
