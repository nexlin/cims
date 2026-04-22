# Phase 1 재검증 요약 (Blocker 해결 후)

**일시**: 2026-04-22 20:55
**브랜치/커밋**: `feature/sip-console-runtime` @ `3efbbdb` + uncommitted (v3 SIP 재구조화 + Blocker fix)

---

## 합격 판정: **✅ PASS**

---

## 1. 이전 Phase 1 FAIL 원인 해결

이전 리포트 (`20260422_204200_phase1_analysis.md`) 에서 식별된 Blocker/Major 전부 해소:

| 항목 | 조치 내용 |
|---|---|
| **Blocker 1**: subscriptions ↔ access_services 매핑 단절 | `voip_subscriptions` / `ptt_subscriptions` 스키마 변경: `service_id INT` → `service_ref VARCHAR(64)` (access_services.name 참조). `sql/migrate_subscriptions_service_ref.sql` 작성/적용. |
| **Blocker 2**: access_services.jsonl 자동 시드 | `cims.sh verify phase1` 7.0 블록 재작성 — DB 에서 subscription.service_ref 읽고 access_services.jsonl 에 해당 name 이 없으면 자동 seed (domain='csp', kind=voip/ptt) + SIGUSR1 |
| **Major 3**: TypeScript 빌드 오류 | `ModuleConfigEditor.tsx:62-64` — `as [string, string[]]` 튜플 타입 명시 |
| **Major 4**: verify script 의 Setup.Realm 파싱 | v3 에서는 access_services.jsonl 이 SOT. 7.0 블록이 jsonl 을 직접 읽고/쓰도록 재작성 |

v3 C++ 측 연결:
- `CspUser.h`: `m_iServiceId` (int) → `m_strServiceRef` (string) 로 변경
- `DbManager.cpp`: `SELECT service_id` → `SELECT service_ref` + JOIN sip_service 제거
- `CspServiceMap.{h,cpp}`: `GetByName(string)` 추가
- `CscfModule.cpp`: `GetById(m_iServiceId)` → `GetByName(m_strServiceRef)`
- `cspsim/CspsimMain.cpp`: DB 쿼리에서 sip_service JOIN 제거 (domain='csp' hardcode)

---

## 2. 각 단계 결과

| 단계 | 항목 | 결과 | 비고 |
|---|---|---|---|
| 1.1 | Reset | ✅ PASS | 23 테이블 TRUNCATE, service_ref 값 포함 가입자 7 테이블 보존 |
| 1.2 | Build | ✅ PASS | CSP + cspsim + Console 전부 warning/error 0 |
| 1.3 | Configure | ✅ PASS | 7 config 파일 재생성 |
| 1.4 | Start (cmp → csp → csc) | ✅ PASS | 3 프로세스 기동 |
| 1.5 | Health | ✅ PASS | 로그 ERROR/FATAL 0 |
| 1.6 | 회귀 7.1 VoIP 2자 통화 | ✅ PASS | **REGISTER 2/2, 통화 1/1, 녹취 4 파일** |
| 1.6 | 회귀 7.2 PTT 그룹콜 (5 member) | ✅ PASS | **REGISTER 5/5, Conference NOTIFY v12, 녹취 14 파일** |

---

## 3. 주요 로그 스냅샷

### 3.1 VoIP 2자 통화
```
===== STATISTICS (2 sessions) =====
  Registered   : 2 / 2  (fail=0)
  Active Calls  : 1
  Call OK/End   : 1 / 1  (fail=0)
  Avg Call Setup: 1002ms
=====================================

녹취: 4개 파일 정상
  seg_0001_va.rtp (285K)  — caller audio
  seg_0001_vb.rtp (224K)  — callee video
  seg_0001_a.rtp  (18K)   — caller video
  seg_0001_b.rtp  (17K)   — callee audio
```

### 3.2 PTT 그룹콜 (5명)
```
[0] REGISTERED User=+82571900001 (36007ms)
[1] REGISTERED User=+82571900002 (38006ms)
[2] REGISTERED User=+82571900003 (40007ms)
[3] REGISTERED User=+82571900004 (42007ms)
[4] REGISTERED User=+82571900005 (43008ms)

[4] [CONF] Conference NOTIFY received (v12)
  user=+82571900005 status=connected

녹취: 14개 파일 정상 (VoIP 4 + PTT group 10)
  PTT group seg_0001_audio ~ seg_0005_audio (각 21K)
  PTT group seg_0001_video ~ seg_0005_video (285K~330K)
```

### 3.3 AccessService 자동 시드 동작 확인
`dist/config/access_services.jsonl`:
```jsonl
{"id":"2873c940ca0c4d55b29bd663576d948e","name":"voip-default","kind":"voip","domain":"csp","auth_realm":"csp","tags":["verify-seed"]}
{"id":"518d41ad465c43ff87de8370895a169a","name":"ptt-default","kind":"ptt","domain":"csp","auth_realm":"csp","tags":["verify-seed"]}
```

CSP 로그:
```
CspConfigCache: reloaded from jsonl (9 entities)
ServiceMap: sync complete, 0 services     ← 초기 (access_services 시드 전)
...
SIGUSR1: reloading jsonl config (v3 9-collection)
ServiceMap: sync complete, 2 services     ← 시드 후 reload
```

---

## 4. v3 재구조화 최종 상태

| 영역 | 상태 |
|---|---|
| 9 collection jsonl (local_nodes/remote_nodes/routes/route_sets/rules/rule_sets/routing_policies/acl_policies/access_services) | ✅ 로드/Sync 정상 |
| CSP RoutingPolicyEngine (ROUTE_SET/ACCESS_SERVICE/REJECT) | ✅ ModuleDispatcher 에 배선 |
| CSP AclPolicyEngine (scope=global/local_node/route/route_set) | ✅ 접근제어 통과 |
| CSP RuleEvaluator (10 op, AND/OR) | ✅ 공유 엔진 동작 |
| psip v3: CSipMessage.m_iListenerId | ✅ UDP 수신 시 세팅 |
| LocalNodeMap.GetByIntId (listener_id ↔ name) | ✅ ModuleDispatcher 에서 사용 |
| AccessService.inbound_policy=restricted | ✅ IsInboundAllowed 헬퍼 |
| DB subscriptions.service_ref → access_services.name | ✅ 매핑 통과 |
| 구 CspRouteEngine/CspTrunkManager/CspAccessControl | ✅ 제거됨 |
| Console UI: ref/ref_list/object_list/string_list + tag filter | ✅ 렌더/TS 컴파일 통과 |
| 마이그레이션 스크립트 `tools/migrate_csp_sip_config_v3.py` | ✅ 작성 완료 (이번 세션에 직접 실행은 안함 — hard cutover 환경) |

---

## 5. Phase 2/3 진입 준비 완료

문서 §0.2 원칙에 따라 Phase 1 전원 PASS — Phase 2 (배포 기능) 및 Phase 3 (New-CSC 경유) 진입 가능. 다만 배포 검증은 TB 3종 (TB-CSC 4419 / TB-Console 3000 / TB-agent 9902) 구축 이후 수행 권장.

---

## 6. 수동 확인 필요 (Console 접속)

Phase 1 합격은 자동 시나리오 기준. 남은 수동 검증은 운영 시 확인:
- [ ] Console Flow 페이지 nodes 순서 (sesid 일관성)
- [ ] CSC 가입자/그룹 CRUD → NOTIFY 반영
- [ ] (mTLS 모드) cert rotation e2e
- [ ] TB-Console 모듈관리에서 9 collection 편집 UI 실동작

---

## 7. 참조 파일

- 자동 리포트: `verify_reports/20260422_205123_phase1.md`
- 이전 FAIL 분석: `verify_reports/20260422_204200_phase1_analysis.md`
- Phase 0 분석: `verify_reports/20260422_203601_phase0.md`
- Migration: `sql/migrate_subscriptions_service_ref.sql`
- 검증 절차: `docs/VERIFICATION_PROCESS.md`
