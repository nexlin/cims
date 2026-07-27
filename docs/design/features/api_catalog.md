# 연동/API 카탈로그 (외부 팀 인계용 공유 API)

CIMS 콘솔은 **우리가 패키지를 배포·운영**하는 내부 OAM 도구다. 실제 VoLTE/PTT **사용량 관리 웹은 별도
팀**이 만들며, 그쪽이 CIMS의 **조회(READ) API**를 소비해야 한다. "연동/API" 탭은 공유 가능한 READ
엔드포인트를 브라우징하고 **OpenAPI 3 스펙을 다운로드**(실제 인계물)하는 허브다.

설계 원칙: 새 인프라·의존성 없이 **Service Descriptor 파이프라인 재사용**. 코어는 범용 셸, 노출 목록은
descriptor 데이터로 구동(플랫폼 §4~6과 동일). 무의존(Swagger UI 없이 표 직접 렌더).

## 1. 데이터 모델 — descriptor `shareable_apis[]`

Service Descriptor(`services` file_store, 시드 `service_descriptors_seed/cims.json`)에 top-level
`shareable_apis[]` 를 둔다. 위젯 계약인 `data_sources[]`(shapes/map)와 **분리** — 인계 계약은 별도가 깔끔.

엔트리 스키마:

| 필드 | 의미 |
|---|---|
| `id` | 안정적 operationId (예: `share.stats.volte`) |
| `method` | HTTP 메서드(기본 GET). **READ 전용 정책** — 비-GET 은 카탈로그에서 배제 |
| `path` | `/api/v1` 없는 경로 (`data_sources.endpoint` 관례와 동일). path 파라미터는 `{name}` |
| `summary` | 한 줄 설명 |
| `category` | `stats` \| `history` \| `recording` \| `subscriber` |
| `params[]` | `{name, in(query\|path), type, required?, enum?, desc?}` |
| `auth` | 인증 노트(기본 Bearer JWT) → OpenAPI securityScheme |
| `audience` | 소비 주체(예: VoLTE/PTT 사용관리 웹) |
| `response_desc?` · `example?` | 응답 설명 + **합성** 예시(실데이터 금지). 예시가 OpenAPI 200 example 이 됨 |

**공유 대상(정책)**: 통계(`/stats/service/*`, `/stats/subscribers`, `/stats/health`, `/stats/messages[/{iface}]`),
이력(`/call/logs`, `/flow/*`, `/ptt/history`), 녹취(`/recordings*`), 가입자/그룹/조직 **조회**(`/users`,
`/ptt/groups`, `/organizations` GET). **내부 운영 API(배포/HA/agent/packages/build/verification/console/
accounts/gateway/external-systems/service-descriptors)는 제외.** 알람은 성격 정리 후 보류.

**PUT 보존**: `handlers/service_descriptors.py` 의 PUT 은 top-level 를 화이트리스트로 재조립하므로
`shareable_apis` 도 보존 절(節)을 둔다(없으면 첫 PUT 에 유실). 시드는 store 가 빌 때만 주입(`seed_if_empty`)
→ 기존 배포엔 `PUT /service-descriptors/cims` 로 주입.

## 2. 백엔드 — `handlers/api_catalog.py` (base 상주)

집계: `services/service_registry.py` `shareable_apis(config)` — 전 descriptor 병합 + `service_id` 부착
(`data_sources()` 미러).

| Route | 응답 |
|---|---|
| `GET /api/v1/api-catalog` | `{generated_at, count, categories[], endpoints[]}` — 탭 소비 |
| `GET /api/v1/api-catalog/openapi.json` | 생성된 OpenAPI 3.0.3 (`Content-Disposition: attachment; filename="cims-openapi.json"`) — 인계물 |

`_build_openapi()`: shareable_apis → `paths`(method/params(`in`·`required`·`enum`)/summary/`tags`=category/
example 있으면 200 example, 없으면 `schema:{type:object}`), `servers:[{url:'/api/v1'}]`,
`securitySchemes.bearerAuth`(http/bearer/JWT). 응답 스키마는 점진 도입(MVP = params + example).

**base 상주**: descriptor + 시드는 `oam_app.py` 가 role 무관 주입하고, 핸들러는 `base_rules` 에 등록되며
**메타데이터만** 읽는다(실제 stats/recording 엔드포인트는 role=base 에서 게이트웨이 프록시). → `role=base`/`all`
동일 출력.

## 3. 프론트 — 코어 셸 + 서비스 게이팅

- `api/apiCatalog.ts` — `get()`→`/api-catalog`, `getOpenApi()`→`/api-catalog/openapi.json`(파싱 JSON; 복사/Blob 다운로드).
- `pages/ApiCatalogPage.tsx`(**코어**) — category별 검색 표, 엔드포인트 펼침(파라미터 표·응답 예시·경로/curl 복사),
  상단 **[OpenAPI 다운로드]/[복사]**. 공유 API 없으면 empty-state("서비스 미설치/shareable_apis 없음").
- **게이팅 = CIMS 서비스 매니페스트 기여**: `ems/service/console/src/manifest.tsx` `cimsManifest.sections` 에
  `integration`(label `연동/API`, area `admin`, order 80 — 릴리스와 문서 사이, route `/integration/api`) 추가. base 콘솔은
  `SERVICE_MANIFESTS=[]`(빌드타임 DCE)라 섹션이 아예 없음 → **서비스팩 있을 때만 노출**(구성/성능/기록과 동일
  메커니즘). 코어 page 컴포넌트를 서비스 섹션이 참조하는 것은 `ServiceDescriptorsPage` 선례와 동일.

## 4. 보류(향후)

- **인증/CORS/서비스계정 provisioning** — 상대 팀 접근 모델 미정. 스펙엔 bearerAuth 만 표기.
- **무인증 usage 핸들러 보안 하드닝**(stats/recording/flow/alerts 는 현재 in-handler 인증 게이트 없음) — 안정화 시 별도.
- **전체 응답 JSON 스키마** — 우선 params+example, 점진 확장.
- **알람 shareable 포함 여부** — 성격 정리 후.
- 라이브 연결 기반 nav 게이팅(설치가 아닌 실시간 health) — 문서 노출엔 불필요, net-new.

## 5. 리스크 — doc-vs-code drift

`shareable_apis[]` 는 수기 메타라 실제 핸들러와 어긋날 수 있다. 완화: 시드를 `data_sources` 옆 `cims.json`
에 함께 두고, CSC 소유(users/orgs/groups)는 csc 모듈에서 파라미터 확인, 향후 라우트 테이블 대조 CI 체크.
인클루전 정책(READ 전용, 내부 운영 금지)을 준수.

## 관련
- `console_platform.md` §5 (Service Descriptor) · §2 (nav 합성)
- `../../api/admin_api.md` (§10 통계·§11 이력·§12 녹취) · `oam_csc_split.md` (OAM↔CSC 경계)
- `monitoring.md` (통계/이력/모니터링 데이터 정의)
