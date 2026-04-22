# SIP 런타임 설정 (jsonl + SIGUSR1)

> 버전: 2.0 (2026-04-21, Phase B — jsonl 기반으로 재설계)
> 이전 버전 (DB + UDP notify 경로) 은 `jsonl` 모드 도입과 함께 deprecated

Console 에서 SIP 리스너/트렁크/라우팅 규칙/접근제어를 **재기동 없이 제어**할 수 있도록 하는 런타임 설정 계층.

## 1. 기본 원리

```
 ┌──────────┐  PUT    ┌──────────┐  PUT/GET  ┌──────────┐  SIGUSR1  ┌──────────┐
 │ Console  │────────▶│   CSC    │──────────▶│  Agent   │──────────▶│   CSP    │
 │  (React) │  JWT    │          │ token     │ :9900    │           │  (C++)   │
 └──────────┘         │          │           │          │           │          │
                      │ 템플릿   │           │  jsonl   │           │ jsonl    │
                      │ schema   │           │ 원자쓰기 │           │ 재로드    │
                      │ 검증     │           │          │           │  + Sync  │
                      └──────────┘           └──────────┘           └──────────┘
```

- **원천(Source of truth)**: 각 Deployment 의 `<install_path>/config/*.jsonl`
- CSC 는 DB 에 저장하지 않고 Agent 에 프록시만 수행
- CSP 는 시작 시 jsonl 읽어 메모리 캐시로 로드, SIGUSR1 수신 시 재로드

## 2. 데이터 모델

각 collection 한 파일:

```
<install_path>/config/
├── listeners.jsonl
├── trunks.jsonl
├── routes.jsonl
├── acl.jsonl
└── services.jsonl   (향후)
```

한 줄 = 한 레코드. 레코드 `id` 는 서버가 생성한 16 hex UUID.

예 (listeners.jsonl):
```jsonl
{"id":"d60bccef...","name":"voip-udp","bind_ip":"0.0.0.0","bind_port":5060,"protocol":"UDP","service":"volte"}
{"id":"335dc240...","name":"mcptt-tcp","bind_ip":"0.0.0.0","bind_port":25061,"protocol":"TCP","service":"mcptt"}
```

## 3. 스키마 선언

각 패키지 tarball 의 `config_template.json` 에 `collections[]` 로 정의 (파일 명세는 `package_and_template.md` 참조).

CSC 는 이 스키마를 이용해:
- UI 폼을 동적 렌더
- PUT 시 type/enum/required validation
- 자동 ID 부여 (`id_type: "uuid"` 인 경우)

## 4. CSP 런타임 통합

### 4.1 시작 시

`csp.json` 의 `Setup.ConfigJsonlDir` 가 설정되면 **jsonl 모드**로 동작:
```cpp
gclsCspConfigCache.Init(cacheDir, cscHost, cscPort, token, jsonlDir);
// jsonlDir 이 비어있지 않으면 내부적으로 _loadFromJsonl() 사용
gclsCspConfigCache.LoadInitial();
```

ConfigJsonlDir 가 비면 기존 HTTP pull 모드 (하위호환).

### 4.2 Reload

CSP 메인 루프가 SIGUSR1 플래그 감지 → 모든 entity 재로드 + 각 관리자 Sync():
```cpp
if (g_reloadFlag && gclsCspConfigCache.IsJsonlMode()) {
    gclsCspConfigCache.ReloadFromJsonl();
    gclsListenerManager.Sync();   // bind/unbind 실제 소켓
    gclsTrunkManager.Sync();
    gclsRouteEngine.Sync();
    gclsAccessControl.Sync();
    gclsServiceMap.Sync();
}
```

## 5. Agent 측 책임

- `PUT /collection?install_path=&name=` 수신 시:
  1. 임시 파일(`.tmp`) 쓰기 → `rename()` 으로 원자 치환
  2. `signal=true` 면 `install_path/run/*.pid` 찾아 SIGUSR1 전송
  3. 응답에 signaled pid 목록 포함
- `GET /collection?...` 은 그대로 jsonl 파싱해서 반환

## 6. 회복탄력성

| 고장 상태 | CSP 영향 | Console |
|-----------|----------|---------|
| Agent 오프라인 | 이미 로드된 설정으로 동작 | 조회/편집 불가 (502) |
| CSC 다운 | 무관 (CSP ↔ Agent 직접 경로 없음, 수정 불가) | 전체 불가 |
| CSP 재기동 | install_path/config/ 다시 로드 → 정상 복구 | — |
| jsonl 없음 / 손상 | 빈 배열로 동작 (listener 0개 등) | 정상 편집 가능 |

## 7. 마이그레이션

이전 버전(DB + UDP notify) 에서 전환:
1. `csp.json` 에 `Setup.ConfigJsonlDir` 추가
2. CSP 재기동 → jsonl 모드 진입 확인 (로그: `ConfigCache initialized (mode=jsonl ...)`)
3. Console 의 "리스너/트렁크/라우팅/ACL" 탭에서 값 입력 → 저장
4. `sql/migrate_deprecate_csp_runtime_tables.sql` 실행 (csp_listener 등을 `*_deprecated` 로 rename)
5. 일정 기간 이상 없으면 deprecated 테이블 DROP

## 8. 관련 소스

- `csp/CspConfigCache.{h,cpp}` — jsonl 로더, Init 의 jsonlDir 인자
- `csp/CspServer.cpp` — SIGUSR1 핸들러 + 메인 루프 reload
- `csp/CspListenerManager.cpp` 등 — Sync() 는 캐시 items 읽어 delta 적용 (기존 로직 재사용)
- `agent/cims_agent.py` — Sync REST 서버 (`/collection`)
- `csc/src/handlers/agents.py` — `_get/_put_deployment_collection` 프록시
