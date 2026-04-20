# 14. SIP 런타임 설정 (Hot-reload) 설계

Console 에서 SIP 리스너/트렁크/라우팅 규칙/접근제어/프로세스를 재기동 없이 제어할 수 있도록 설계된 런타임 설정 계층. 2026-04 feature branch `feature/sip-console-runtime`.

---

## 1. 구조 개요

```
 ┌──────────┐   HTTPS 4420   ┌──────────┐   HTTP 4422 (loopback)   ┌──────────┐
 │ Console  │───────────────▶│   CSC    │◀─────────────────────────│   CSP    │
 │ (React)  │   admin JWT    │ (Python) │   X-Csp-Internal-Token   │  (C++)   │
 └──────────┘                │   ↕ DB   │   UDP notify 4421        │  ↕ mem$  │
                             │   ↕ mem$ │─────────────────────────▶│  ↕ file$ │
                             │   ↕ file$│                          └──────────┘
                             └──────────┘
                                   ▼
                             ┌──────────┐
                             │  MariaDB │
                             └──────────┘
```

### 회복탄력성 (P1)

| 고장 상태 | CSP 영향 | Console |
|-----------|----------|---------|
| DB 다운 | 없음 (mem+file) | 변경 503, 조회 OK |
| CSC 다운 | 없음 (자체 file$) | 전체 불가 |
| DB+CSC | 없음 | 불가 |
| CSP 재기동 + DB 다운 | CSC 자체 file$ 응답 → 정상 부팅 | 변경 불가 |
| CSP 재기동 + CSC 다운 | 로컬 file$ 로 부팅, 기본 서비스 동작 | — |
| 모든 캐시 없음 최초 부팅 | `csp.json` Bootstrap 블록 fallback | — |

---

## 2. DB 스키마

`sql/migrate_csp_runtime_config.sql` 로 도입된 7개 테이블:

| 테이블 | 내용 | Phase |
|--------|------|-------|
| `csp_listener` | SIP 리스너 (bind_ip/port/protocol/service) | P2 |
| `sip_trunk` | 원격 SIP 서버 + health 설정 | P3 |
| `routing_rule` | 라우팅 규칙 헤더 | P4 |
| `routing_rule_match` | 매칭 조건 (rule_id 당 N개) | P4 |
| `routing_rule_transform` | 변환 액션 (rule_id 당 N개) | P4 |
| `routing_access_list` | IP/CIDR/UA allow/deny | P5 |
| `csp_config_audit` | 변경 감사 로그 (actor/entity/action/before/after) | P1 |

---

## 3. Phase 별 기능

### P1 — 3층 캐시 (DB ↔ mem ↔ file)

- **CSC**: `csc_config_cache.py` — DB 로드 → 메모리 → `csc/cache/*.json` write-through
- **CSP**: `CspConfigCache` — 로컬 `csp/cache/*.json` 우선 → CSC HTTP pull (ETag 304 지원)
- **CSC 내부 API**: `http://127.0.0.1:4422/api/internal/config/{entity}[/id]` (loopback + `X-Csp-Internal-Token`)
- **Notify 채널**: CSC→CSP UDP 4421, 이벤트 `LISTENER_CHANGED`/`TRUNK_CHANGED`/`ROUTE_RULE_CHANGED`/`ACCESS_LIST_CHANGED`/`CSC_RESTART`

### P2 — SIP 리스너 hot-reload

- **psip 확장**: `CSipStack::AddUdpListener`/`RemoveUdpListener`/`GetUdpListenerInfo`. 다중 UDP 리스너 + 리스너별 recv mutex.
- **CSP**: `CspListenerManager` — cache ↔ psip 집합 diff 계산. 포트 충돌 시 bootstrap 과 겹치면 skip.
- **API**: `/api/v1/csp/listeners` CRUD
- **UI**: `SipListenersPage` (폼/JSON 모드, TLS 필드 조건부)
- **한계**: TCP/TLS 는 단일 리스너 유지. 다중화는 cert/session map 분리 필요.
- **무중단 검증**: 진행 중 VoLTE 2-leg 호 상태에서 listener add/remove → 호 정상 종료 (`status=200`, `fail=0`)

### P3 — 트렁크 레지스트리 + OPTIONS 헬스

- **CspTrunkManager**: 주기적 OPTIONS ping (설정 `options_ping_sec`). Call-ID 로 응답 매칭.
- **상태 머신**: 응답 성공 → `alive=true`. 연속 실패 ≥ `options_dead_threshold` → `alive=false`.
- **SIP 응답 해석**: 2xx / 3xx / 4xx(≠408) / 405 / 501 → alive. 408 / 5xx → fail.
- **자동 OPTIONS 응답**: `ModuleDispatcher::RecvRequest` 가 수신 OPTIONS 에 자동 200 OK (RFC 3261 §11.2).
- **API**: `/api/v1/csp/trunks` CRUD. STATS `trunks` 배열에 `alive/rtt/fail_count`.
- **UI**: `SipTrunksPage` — 10초 주기 헬스 배지.

### P4 — 라우팅 규칙 엔진

- **CspRouteEngine**: priority 정렬 + first-match-wins 평가.

**매칭 필드**: `method`, `req_uri_user`/`_host`, `from_uri`/`to_uri`, `source_ip`, `source_trunk`, `header:<NAME>`
**매칭 연산자**: `equals`, `not_equals`, `prefix`, `suffix`, `contains`, `regex`, `cidr`

**변환 액션**: `set_req_uri_user`/`_host`, `set_from_host`, `add_header`/`remove_header`/`replace_header`, `strip_prefix`/`add_prefix`

**타겟 모드**:
- `trunk` (단일 트렁크) — P4 에서 완전 지원
- `reject` — fail_code 로 응답
- `priority_list` / `round_robin` / `weighted` — 뼈대 (후속 phase)

**실패 처리**: `action` (`reject`/`fallback`/`next_rule`), `code`, `reason`, `fallback_trunk_id`, `timeout_ms`, `retry_count`

- **Dry-run**: `POST /api/v1/csp/routes/dryrun` — 샘플 SIP 입력 → 매칭 결과 (순수 Python).
- **UI**: `SipRoutesPage` — 규칙 목록 + 빌더(match/transform 동적 추가/삭제) + JSON 모드 + Dry-run 패널.

### P5 — 접근제어 + Rate limit

- **CspAccessControl**: routing_access_list 로드 → per-request `Check(src_ip, listener_id, ua)`.
- **매칭**: `ip` / `cidr` / `ua_regex`. Scope: `global` / `listener` / `trunk`.
- **평가**: priority 낮을수록 먼저 → deny 매칭이 먼저면 차단, allow 매칭이 먼저면 통과, 매칭 없으면 기본 허용.
- **Rate limit**: per-IP 토큰 버킷 (기본 비활성. `SetRateLimit(rps, burst)`).
- **응답**: ACL deny → SIP 403. Rate 초과 → SIP 503 (429 SIP 매핑 없음).
- **UI**: `SipAccessPage` — allow/deny 배지.

### P6 — 프로세스 제어

- **API**: `GET /api/v1/services` (status) / `POST /api/v1/services/{name}/{start|stop|restart}`
- **드라이버**: `cims_sh` (기본) 또는 `systemd` (환경변수 `CIMS_SERVICE_DRIVER`).
- **서비스 목록**: `cmp`, `csp`, `cwrtc`, `csc`, `console`, `phone`
- **안전장치**: CSC 자기 중지는 UI 에서 확인 필수. 모든 제어는 `csp_config_audit` 에 감사 기록.
- **UI**: `ServicesPage` — 실행/중지 배지, PID, 최근 명령 출력 패널.

---

## 4. 설정 변경 흐름 (write-through)

```
Console → CSC HTTPS 4420 (JWT)
    ├─ DB INSERT/UPDATE/DELETE (트랜잭션)
    ├─ CSC 메모리 캐시 갱신
    ├─ csc/cache/{entity}.json.tmp → rename (atomic)
    ├─ csp_config_audit 에 actor/action/before/after 기록
    └─ UDP 4421 → CSP: {"event":"X_CHANGED", "uri":"entity/id", "etag":...}

CSP 수신
    ├─ CscInterface → CspConfigCache.RefreshEntity(entity)
    │     └─ HTTP GET csc:4422 (If-None-Match ETag)
    │     └─ csp/cache/{entity}.json 원자 교체
    └─ 해당 매니저 Sync() → 메모리 + psip 반영
         (ListenerManager / TrunkManager / RouteEngine / AccessControl)
```

---

## 5. Console UI 사이드바

```
CSP 설정
├── SIP 리스너           (SipListenersPage)
├── SIP 트렁크           (SipTrunksPage, 헬스 배지)
├── 라우팅 규칙          (SipRoutesPage, dry-run)
├── 접근제어             (SipAccessPage)
└── 프로세스 제어        (ServicesPage)
```

---

## 6. 관련 파일

### CSP (C++)

| 파일 | 역할 |
|------|------|
| `csp/CspConfigCache.{h,cpp}` | 로컬 JSON 캐시 + CSC HTTP pull (raw socket + chunked) |
| `csp/CspListenerManager.{h,cpp}` | UDP 리스너 diff/sync |
| `csp/CspTrunkManager.{h,cpp}` | 트렁크 헬스 체크 스레드 |
| `csp/CspRouteEngine.{h,cpp}` | 매칭/변환/타겟 평가 |
| `csp/CspAccessControl.{h,cpp}` | ACL + rate limit |
| `csp/ModuleDispatcher.cpp` | RecvRequest 훅 (access → OPTIONS → CSCF → routing → IBCF) |
| `ext/psip/SipStack/SipStackListener.h` | `CSipStackUdpListener` 추상화 |
| `ext/psip/SipStack/SipStack.{h,cpp}` | AddUdpListener/RemoveUdpListener API |

### CSC (Python)

| 파일 | 역할 |
|------|------|
| `csc/.../csc_config_cache.py` | DB ↔ mem ↔ file 3층 캐시 |
| `csc/.../csc_internal.py` | CSP 전용 내부 HTTP API (loopback only) |
| `csc/.../cims_csp_runtime.py` | `/api/v1/csp/{listeners,trunks,routes,access}` CRUD + dryrun |
| `csc/.../cims_service_control.py` | `/api/v1/services/{name}/{action}` |
| `csc/.../csc_service.py` | `notify_config_change` + `audit_config_change` 헬퍼 |

### Console UI

| 파일 | 역할 |
|------|------|
| `cims-console/src/api/cspRuntime.ts` | 런타임 설정 API 클라이언트 |
| `cims-console/src/api/services.ts` | 서비스 제어 API 클라이언트 |
| `cims-console/src/pages/SipListenersPage.tsx` | 리스너 CRUD |
| `cims-console/src/pages/SipTrunksPage.tsx` | 트렁크 + 헬스 |
| `cims-console/src/pages/SipRoutesPage.tsx` | 라우팅 규칙 + dry-run |
| `cims-console/src/pages/SipAccessPage.tsx` | ACL |
| `cims-console/src/pages/ServicesPage.tsx` | 프로세스 제어 |

---

## 7. 미구현/후속 작업

| 항목 | 대상 Phase | 비고 |
|------|-----------|------|
| TCP/TLS 다중 리스너 | P2 확장 | cert별 SSL_CTX / session map 분리 필요 |
| priority_list/round_robin/weighted 타겟 | P4 확장 | target_json 기반 |
| CAC (call admission control) | P5 확장 | 트렁크별 max_concurrent_calls 강제 |
| Circuit breaker | P5 확장 | 트렁크 장애율 임계치 초과 시 자동 bypass |
| 히트 카운터 DB persist | P4 확장 | 현재 메모리만 |
| CSipServerMap 최종 제거 | — | 빈 맵으로 무해한 fallback — 추후 정리 |
