# API 문서 (개발자 모드) — 메뉴별 사용 API 노출

## 목적·원칙

콘솔의 각 메뉴가 **어떤 API 를 호출하는지** 를 그 메뉴 화면에서 바로 확인한다. 별도 카탈로그 탭은 두지
않는다.

원칙 셋:

1. **문서는 API 를 구현한 모듈이 소유한다.** 각 핸들러 파일이 자기 엔드포인트를 `*_API_DOCS` 로 선언한다.
   중앙 카탈로그(descriptor/JSON)에 모아두지 않는다 → 경로·파라미터를 고칠 때 같은 파일에서 문서도 고친다.
2. **모듈이 설치·가용해야 그 API 문서도 존재한다.** 선언이 모듈 코드와 같이 배포되므로 별도 게이팅 코드가
   없다. csc 미설치면 가입자/조직/PTT그룹 API 는 애초에 수집되지 않는다.
3. **콘솔은 읽어서 표시만 한다.** 프런트에 API 목록·경로·파라미터를 두지 않는다.

노출은 **개발자 모드 ON** 일 때만 (`utils/devMode.ts` — 릴리스 메뉴 게이팅과 동일 스위치).

## 1. 선언 — 각 모듈의 `*_API_DOCS`

`HANDLER_LIST` 바로 옆에 리스트로 둔다. 엔트리 스키마:

| 키 | 설명 |
|---|---|
| `id` | 고유 id (예: `stats.service.volte`) |
| `module` | 제공 모듈 — `'csc'` · `'oam-svc'` · `None`(base 상주). **가용 판정 키** |
| `method` | HTTP 메서드 |
| `path` | `/api/v1` 을 포함한 전체 경로. path 파라미터는 `{name}` |
| `screens` | 이 API 를 쓰는 콘솔 메뉴 라우트 경로 목록 (예: `['/stats/volte']`) |
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
GET /api/v1/api-docs                 가용 모듈의 API 문서 전체
GET /api/v1/api-docs?screen=<path>   그 콘솔 메뉴가 쓰는 API 만
→ { screen, modules[], count, apis[] }
```

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

## 3. 콘솔 — `[API]` 버튼

- `ems/core/console/src/api/apiDocs.ts` — `apiDocsApi.get(screen?)`.
- `ems/core/console/src/components/ApiDocsButton.tsx` — 개발자 모드 전용 버튼 + 모달. 메서드 배지·경로·
  요약, 펼치면 파라미터 표·응답·인증·id, `경로`/`curl` 복사.
  - **개발자 모드 OFF 면 조회 자체를 하지 않는다** (평시 트래픽 0).
  - 이 메뉴가 쓰는 API 가 0건이면 **버튼도 렌더하지 않는다** (모듈 미설치/미가용 또는 선언 없음).
  - 조회 결과를 조회 대상 경로와 함께 보관해, 메뉴 전환 직후 이전 메뉴의 목록이 보이지 않게 한다.
- 배선은 `widgets/EditableLayout.tsx` **한 곳**: 헤더 슬롯(`#layout-edit-slot`)으로 portal. 모든 라우트가
  `EditablePageHost` → `EditableLayout` 을 거치므로 화면별 배선 없이 전 메뉴에 붙는다. 현재 메뉴 경로는
  `useLocation().pathname`.

## 4. 유지 규칙

- 엔드포인트의 경로·메서드·파라미터를 바꾸면 **같은 커밋에서 같은 파일의 `*_API_DOCS` 를 갱신**한다.
- 새 메뉴가 기존 API 를 쓰기 시작하면 그 API 엔트리의 `screens` 에 경로를 추가한다.
- 새 모듈이 API 를 제공하면 그 모듈 파일에 `*_API_DOCS` 를 두고 `handlers/api_docs.py` 의 로더 목록에
  한 줄 추가한다. 그 모듈이 **다른 서버에 배포될 수 있으면** csc 처럼 자기 `/api/v1/api-docs` 도 서비스한다.
- 선언을 바꾼 뒤에는 **해당 모듈을 재배포**해야 반영된다 (문서는 코드와 함께 배포된다). 원격 조회분은
  최대 60초 캐시된다.

## 5. 미도입

- 전체 응답 JSON 스키마 (현재 `response` 한 줄 요약).
- OpenAPI 산출물 — 외부 팀 인계 수단은 별도 결정 대기 (접근 모델·인증·CORS·base URL 미정).
- 코어 운영 API(배포/HA/packages/build/verification/console/accounts/gateway) 문서 — **의도적 제외**
  (위 범위 정책). 내부 운영용으로 필요해지면 같은 방식으로 선언을 추가하되 외부 공유분과 구분할 표식이
  먼저 필요하다.
- csc MCPTT(4430) 12개 경로 — 단말용 규격 인터페이스라 제외. 넘길 필요가 생기면 `csc/src/services/mcptt.py`
  에 선언을 두고 `csc/src/handlers/api_docs.py` 에서 합친다.
- 라우트 테이블과 선언을 대조하는 CI 체크 (선언 누락·경로 오타 탐지).
