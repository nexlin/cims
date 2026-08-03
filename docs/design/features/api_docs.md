# API 문서 (개발자 모드) — 위젯별 사용 API 노출

## 목적·원칙

각 위젯이 **어떤 API 를 호출하는지** 를 그 위젯 자리에서 바로 확인한다. 별도 카탈로그 탭은 두지 않는다.

원칙 넷:

1. **내용은 API 를 구현한 모듈이 소유한다.** 각 핸들러 파일이 자기 엔드포인트를 `*_API_DOCS` 로 선언한다.
   중앙 카탈로그(descriptor/JSON)에 모아두지 않는다 → 경로·파라미터를 고칠 때 같은 파일에서 문서도 고친다.
2. **소비 관계는 위젯이 선언한다.** `WidgetDef.apis` / `RouteDef.apis` 가 **API id 목록만** 갖는다.
   백엔드는 자기를 누가 쓰는지 모른다(콘솔 위젯 id 가 백엔드로 새지 않는다). 표시되는 내용은 100% 백엔드
   에서 오므로 "콘솔은 읽어서 표시만 한다" 는 그대로다.
3. **모듈이 설치·가용해야 그 API 문서도 존재한다.** 선언이 모듈 코드와 같이 배포되므로 별도 게이팅 코드가
   없다. csc 미설치면 가입자/조직/PTT그룹 API 는 애초에 수집되지 않고, 그 위젯의 배지도 뜨지 않는다.
4. **개발자 모드 ON 일 때만** 노출 (`utils/devMode.ts` — 릴리스 메뉴 게이팅과 동일 스위치).

## 1. 선언 — 각 모듈의 `*_API_DOCS`

`HANDLER_LIST` 바로 옆에 리스트로 둔다. 엔트리 스키마:

| 키 | 설명 |
|---|---|
| `id` | 고유 id (예: `stats.service.volte`) — **위젯의 `apis` 가 참조하는 키** |
| `module` | 제공 모듈 — `'csc'` · `'oam-svc'` · `None`(base 상주). **가용 판정 키** |
| `method` | HTTP 메서드 |
| `path` | `/api/v1` 을 포함한 전체 경로. path 파라미터는 `{name}` |
| `summary` | 한 줄 설명 |
| `params` | `[{name, in(query\|path\|body), type, required, enum?, desc}]` |
| `response` | 응답 요약 (전체 JSON 스키마는 미도입) |
| `auth` | 필요 권한 (예: `'Bearer JWT (monitor)'`) |

선언 위치 (= 구현 위치):

| 파일 | `*_API_DOCS` | `module` | 대상 |
|---|---|---|---|
| `ems/core/oam/src/handlers/agents.py` | `CIMS_AGENT_API_DOCS` | `None`(base) | `/agents`(노드 사양) · `/agents/{id}/metrics`(자원 사용률) — **조회만** |
| `ems/core/oam/src/handlers/stats.py` | `CIMS_STATS_API_DOCS` | `oam-svc` | `/stats/*` (health·subscribers·messages·leak-reclaims + service KPI) |
| `ems/core/oam/src/handlers/recording.py` | `CIMS_RECORDING_API_DOCS` | `oam-svc` | `/recordings*` (목록·상세·세그먼트·스트리밍) |
| `ems/core/oam/src/services/flow_logger.py` | `FLOW_API_DOCS` | `oam-svc` | `/call/logs` · `/flow/*` · `/ptt/history*` · `/security/abnormal-sessions` |
| `csc/src/handlers/admin.py` | `CIMS_ADMIN_API_DOCS` | `csc` | `/users*` · `/ptt/groups*` (CRUD) |
| `csc/src/handlers/org.py` | `CIMS_ORG_API_DOCS` | `csc` | `/organizations*` (CRUD) |

현재 선언 수: base 2건(노드 사양·사용률), oam-svc 29건(stats 13 · recording 6 · flow 10),
csc 25건(admin 18 · org 7) = **56건**.

### 무엇을 선언하고 무엇을 안 하나 (범위 정책)

선언 = **외부(사용관리 웹 등)에 넘길 수 있는 것**. 내부 운영은 선언하지 않는다.

| 넣는다 | 안 넣는다 |
|---|---|
| 사용량·이력·통계 (`/stats/*`, `/call/logs`, `/flow/*`, `/ptt/history*`, `/security/abnormal-sessions`) | 배포·패키지·sync/drift (`/deployments`, `/packages`, `/csp/sync`, `/csp/drift`, `/services`) |
| 녹취 (`/recordings*`) | agent 등록·승인·삭제·제어 (`/agents` 의 변이 메서드, `/api/agent/*`) |
| 가입자·조직·PTT그룹 (`/users*`, `/organizations*`, `/ptt/groups*`) | HA (`/ha-groups`), 게이트웨이(`/gateway`), 외부시스템, 콘솔 계정·레이아웃, 서비스 정의 |
| **노드 사양·자원 사용률** (`/agents`, `/agents/{id}/metrics` — GET, `monitor`) | 빌드·검증·자동배포 (`/build`, `/verification`, `/provision`) |
| | 인증·토큰 (`/auth/*`) |
| | csc MCPTT 서버(4430) — IdMS/GMS/CMS/KMS·`/provisioning/*`. **단말(UE)용 3GPP 규격 인터페이스** |

CSP·CMP 는 **HTTP API 가 없다** (SIP/RTP + UDP JSON 제어). 두 모듈의 상태·통계는 oam-svc 의
`/stats/*` 로 나오므로 그쪽 선언이 대신한다.

`/api/v1/recordings` 는 `FLOW_HANDLER_LIST` 에도 있으나 `handlers/recording.py` 가 우선 등록되어 소유한다 →
문서도 recording 쪽에만 둔다.

`csc/src/handlers/admin.py`·`org.py` 는 **csc 모듈에 속한 파일**이다. 단일 호스트(`role=all`) 배포에서는
csc 배포 시 OAM `handlers/` 에 설치되어 `oam_app.py` 의 `try: from handlers.admin import ...` 선택 로드로
살아난다. **분리 배포**(csc 가 다른 서버)에서는 OAM 에 그 코드가 없으므로 import 로 문서를 얻을 수 없다 →
csc 가 자기 문서를 직접 서비스하고 OAM 이 가져와 병합한다 (§2 소스 2).

## 2. 수집 — `handlers/api_docs.py` (base 상주)

```
GET /api/v1/api-docs   가용 모듈의 API 문서 전체 → { modules[], count, apis[] }
```

**소비처 필터는 없다.** 전부 내려주고 콘솔이 위젯의 `apis` id 로 골라 쓴다 (한 화면에 배지가 여럿이라
콘솔이 요청을 공유 캐시로 1회만 보낸다).

소스 두 갈래를 병합한다:

1. **로컬 import** — 모듈 코드가 이 OAM 에 있는 경우(base 상주 + 동일 호스트 서비스 모듈). 모듈별 로더를
   **각각 독립 try** 로 호출한다. import 실패 = 그 모듈 없음 → 그 API 문서 없음.
2. **업스트림 조회** — 가용하지만 코드가 없는 모듈(분리 배포의 csc). 게이트웨이 라우트 테이블의
   `upstream` 으로 `GET <upstream>/api/v1/api-docs` 를 호출해 병합한다. 호출자의 Bearer 토큰을 그대로
   전달(`subscriber_import` 와 같은 규약)하고, 결과는 `_REMOTE_TTL`(60초) 캐시한다. 같은 세그먼트에
   라우트가 여럿이면(HA/잔재) id 역순으로 응답하는 첫 업스트림을 쓴다.
   - csc 측: `csc/src/handlers/api_docs.py` (`CSC_API_DOCS_HANDLER_LIST`) — `csc_app.py` 가 admin 서버에
     등록. `monitor` 권한 필요.

- 가용 판정은 `handlers/console_layouts.installed_services()` 를 쓴다 (`role=base` 는 게이트웨이 라우트
  테이블이 권위, `role=all` 은 알려진 서비스 전체). `module` 이 가용 집합에 없으면 제외, `module=None`
  (base 상주) 은 항상 포함.
  - **모듈명은 소문자로 정규화해 비교한다.** 라우트 테이블은 배포 모듈명을 대문자(`OAM-SVC`/`CSC`)로
    저장하는데 비교 대상(`_KNOWN_SERVICES`, 위젯 `requires_service`, API 문서 `module`)은 소문자라,
    정규화 없이는 `role=base` 에서 전부 미가용으로 오판한다 (위젯 가용성 판정도 같은 경로).
- 메타데이터만 읽으므로 `base_rules` 에 등록 — `role=base`/`all` 동일 동작. stats/recording DB·파일을
  건드리지 않는다.
- `GET` 외 405, 하위 경로 404.

## 3. 소비 선언 — 위젯의 `apis`

위젯이 자기가 부르는 **API id 만** 선언한다. 두 자리:

- **`WidgetDef.apis`** (`widgets/types.ts`) — 실제 위젯. 예: `cims.service-stats` →
  `['stats.service.volte', 'stats.service.ptt', 'stats.messages']`.
- **`RouteDef.apis`** (`nav-types.ts`) — 고정 페이지(`component:` 라우트). 페이지 전체가 위젯 1개
  (`page:<path>`)로 감싸지므로, `App.tsx` 가 `route.apis` 를 그 page 위젯 def 로 전달한다.

## 4. 콘솔 — `[API]` 배지

- `ems/core/console/src/api/apiDocs.ts` — `loadApiDocs()` 가 `/api-docs` 를 받아 `id → ApiDoc` Map 으로
  준다. 한 화면에 배지가 여럿이므로 **in-flight 프라미스를 공유**해 요청은 페이지당 1회.
- `ems/core/console/src/components/WidgetApiBadge.tsx` — 개발자 모드 전용 배지 + 모달. 메서드 배지·경로·
  제공 모듈·요약, 펼치면 파라미터 표·응답·인증·id, `경로`/`curl` 복사.
  - **개발자 모드 OFF 면 조회 자체를 하지 않는다** (평시 트래픽 0).
  - 위젯에 `apis` 선언이 없거나 문서 0건이면 **배지를 렌더하지 않는다** (모듈 미설치/미가용 포함).
- 배선 두 곳:
  - **보기 모드** — `widgets/GridRenderer.tsx` (grid·flow 양쪽). 위젯 래퍼(`.widget-api-host`) 우상단
    오버레이(`.widget-api-badge--overlay`). 평소 흐리고 hover 시 또렷 — 내용 가림 최소화.
  - **편집 모드** — `widgets/GridEditor.tsx` 카드 헤더, 크기 배지와 `✕` 사이 인라인. 배치하면서 이
    위젯이 뭘 부르는지 바로 확인.

## 5. 유지 규칙

- 엔드포인트의 경로·메서드·파라미터를 바꾸면 **같은 커밋에서 같은 파일의 `*_API_DOCS` 를 갱신**한다.
- 위젯이 호출하는 API 가 바뀌면 그 **위젯의 `apis`** 를 갱신한다. id 가 백엔드에 없으면 그 항목만 조용히
  빠진다(배지 건수 감소) — 오타가 에러로 드러나지 않으니 주의.
- 새 모듈이 API 를 제공하면 그 모듈 파일에 `*_API_DOCS` 를 두고 `handlers/api_docs.py` 의 로더 목록에
  한 줄 추가한다. 그 모듈이 **다른 서버에 배포될 수 있으면** csc 처럼 자기 `/api/v1/api-docs` 도 서비스한다.
- 선언을 바꾼 뒤에는 **해당 모듈을 재배포**해야 반영된다 (문서는 코드와 함께 배포된다). 원격 조회분은
  최대 60초 캐시된다.

## 6. 미도입

- 전체 응답 JSON 스키마 (현재 `response` 한 줄 요약).
- OpenAPI 산출물 — 외부 팀 인계 수단은 별도 결정 대기 (접근 모델·인증·CORS·base URL 미정).
- 코어 운영 API(배포/HA/packages/build/verification/console/accounts/gateway) 문서 — **의도적 제외**
  (위 범위 정책). 내부 운영용으로 필요해지면 같은 방식으로 선언을 추가하되 외부 공유분과 구분할 표식이
  먼저 필요하다.
- csc MCPTT(4430) 12개 경로 — 단말용 규격 인터페이스라 제외. 넘길 필요가 생기면 `csc/src/services/mcptt.py`
  에 선언을 두고 `csc/src/handlers/api_docs.py` 에서 합친다.
- 라우트 테이블과 선언을 대조하는 CI 체크 (선언 누락·경로 오타 탐지) + **위젯 `apis` id 가 백엔드
  선언에 존재하는지** 대조 (오타가 조용히 누락되는 것 방지).
- 콘솔이 호출하지 않는 선언(현재 `flow.list` · `recording.list` · `recording.delete` ·
  `recording.audio` · `stats.service.summary`)은 배지에 안 뜬다 — `/api-docs` 응답에는 있으므로 인계에는
  포함된다.
