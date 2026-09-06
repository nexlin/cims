> 작업 요청서(서버 세션 → 단말/Windows 세션). 단말 구현·문서 반영이 끝나면 이 파일은 삭제하고 내용은 각 설계 정본(ue_sdk.md·dispatch_desktop_ui.md)에 최종 상태로만 남긴다. 서버 파트(PTT 그룹 CRUD·활성 세션 발견·conference 인가·통합 이력·1:1 메시지 보관)는 **전부 구현 완료**로 정본 문서에 반영됐다(요청서는 삭제됨): 계약 [../design/features/android_ue_provisioning.md](../design/features/android_ue_provisioning.md) §3-2·[../design/features/dispatch_center.md](../design/features/dispatch_center.md) §5.6/§5.7a·[../api/mcptt_api.md](../api/mcptt_api.md) §2.

# Windows 후속 요청 — 관제조작반 PTT 그룹 CRUD 실기 연동 · 계약 접점 반영 · 다음 서버 과제 대비

> **단말 세션 반영 상태(2026-09-06)**: §1 재빌드 완료(cimsue.dll·CimsUe.dll·앱 Release, 시험 통과 — C API/구조체 식별자 rename 은 ABI 변경이라 보류).
> §2 반영 — 그룹 uri 정규형 `tel:g-<hex8>` 로 생성, `ResponseText.GroupError`(본문 `error` → 문구, `unknown_member` 번호 표시),
> `uri_taken` 409 는 id 재생성 1회 재시도, `etag_mismatch` 412 는 문서 재조회 후 재편집, 삭제 404 는 목록 재조회. §4 문서 번호 재키잉 반영
> (`dispatch_desktop_ui.md` §14). §3 앱 e2e 체크리스트 = **사용자 실기 대기**(앱은 로그인 창 상태로 실행 중). §5 P2 는 앱 구조 그대로 대응,
> P3 은 `Area.PttListen` 403 문구 기존 항목으로 흡수·재시도 없음, P3b 폴링 클라이언트는 `/provisioning/history` 계약 확정 후 착수.

작성 주체: 서버(CSC/CSP) 쪽 세션. 대상: Windows 관제조작반(`windows/dispatch-desktop`) + 단말 SDK(`sdk/`) 세션.
원칙: CLAUDE.md 설계 우선순위(규격 → 체계성 → 최소보완 지양). 문서 현행화는 최종 상태만.

---

## 0. 서버 상태 (2026-09-06 기준 — 라이브 csc 0.2.103)

서버 요청서 §1(PTT 그룹 CRUD)은 **구현·배포·실기 e2e 완료**다. 계약 정본 = [mcptt_api.md §2](../api/mcptt_api.md), 인가 = [mcptt_authorization.md §4.1](../design/features/mcptt_authorization.md).

| 항목 | 라이브 상태 |
|---|---|
| 자격 플래그 | `ptt_user_profile.allow_create_group`(컬럼 적용됨) / 프로파일 XML `<cims:allow-create-group>` / 프로비저닝 **`ptt.allowCreateGroup`** — disp01·disp02 에 **부여 완료(true)** |
| GMS PUT/DELETE | 본문 파싱·DB 기록·캐시 동기화·CSP `GROUP_CHANGED`(xcap-diff NOTIFY) 전부 동작 |
| 목록 GET | 항목에 `is_owner`(소유 그룹은 비멤버라도 포함) |
| 실측 | `cimsue-cli`(관제사 disp01): 생성 201 → 목록 `owner:true` → disp02 삭제 **403** → 갱신 200(새 ETag) → 삭제 ok → CSP DELETE + NOTIFY. DB 잔여 0 |

관련 커밋: 서버 `9bf59d26`, 단말 와이어 키 정렬 `718b42a0`(아래 §1).

---

## 1. 즉시 — 빌드·정렬 (필수)

1. **`718b42a0` 를 받아 SDK·.NET 파사드·WPF 앱을 재빌드**한다. 서버가 내는 프로비저닝 키가 확정안 `ptt.allowCreateGroup` 이라, SDK 파서(`csc_client.cpp` — 최상위 `ptt` 와 `services[kind=ptt]` 두 경로)·시험·문서의 **JSON 키만** 그쪽에 맞췄다. 이 빌드 전의 앱은 `allowGroupCreation` 을 읽어 [새 그룹] 이 열리지 않는다.
2. (선택, 정리) 구조체 식별자 `allowGroupCreation` / C API `allow_group_creation` 은 와이어가 아니라 그대로 두었다. 이름을 `allowCreateGroup` / `allow_create_group` 으로 맞추려면 C API 필드 rename = ABI 변경이니 .NET 파사드·ABI 테스트를 함께 바꾼다. 안 해도 동작엔 무관.
3. 확인: `cimsue-cli --csc-host <CSC> --user disp01 --pw 1234 --from-profile ptt --json groups` 가 `"allow_group_creation":true` 를 찍으면 정렬 완료.

---

## 2. 계약 접점 반영 (앱·SDK)

서버가 확정한 규칙 중 앱 코드가 알아야 하는 것. 대부분 이미 맞고, 오류 문구만 비어 있다.

- **그룹 uri 정규형은 `tel:<id>`** (시스템 관례 — GROUPS 키·CSP·제휴가 전부 tel:). 앱의 `NewGroupUri()` 가 만드는 `sip:g-<8hex>@<PTT도메인>` 은 서버가 수용해 정규화하고, **응답 문서·목록은 `tel:g-…`** 로 돌아온다. `RefreshGroupsAsync` 가 목록 id 로 affiliation·conference 를 거니 현재 흐름은 호환된다. 생성 직후 새 그룹을 선택/강조할 때는 앱이 만든 `sip:` 문자열이 아니라 **PUT 응답 문서의 `list-service@uri`(또는 목록의 uri)** 를 정본으로 써라.
- **신규 id 규칙**: `g-` + 소문자 hex 8자리(현재 `Guid.NewGuid():N` 앞 8자리 — 적합). `adhoc-`/`priv-` 접두사 거부. `sip:` 형이면 도메인이 PTT 도메인이어야 함(프로비저닝 `sip.domain` 과 같은 값이라 적합).
- **오류 코드 → 문구 사전(`ResponseText.cs`)에 그룹 CRUD 항목 추가** (현재 Group 영역 없음). SDK `httpFail` 이 `"putGroup 403: {\"error\":\"…\"}"` 형태로 상태+본문을 넘기므로 본문의 `error` 로 분기한다.

  | HTTP | `error` | 뜻 | 앱 동작 |
  |---|---|---|---|
  | 403 | `group_creation_not_allowed` | 생성 자격 없음 | 문구 — "그룹 생성 자격이 없습니다(관리자 부여 필요)". [새 그룹] 은 원래 `allowCreateGroup=false` 면 숨김 |
  | 403 | `not_group_owner` | 내 소유가 아님(콘솔이 만든 소유자 없는 그룹 포함) | 문구 — "본인이 만든 그룹만 편집·삭제할 수 있습니다". 목록 `is_owner=false` 면 [편집]/[삭제] 숨김 |
  | 409 | `uri_taken` | 타인 소유 id 충돌(클라이언트 명명) | **id 재생성 후 1회 자동 재시도**, 그래도 409 면 문구 |
  | 400 | `unknown_member` (`detail`=번호 배열) | PTT 미가입 번호 | 해당 번호를 표시하고 편집 폼으로 복귀 |
  | 400 | `invalid_group_id` / `reserved_prefix` / `invalid_group_document` | 형식 위반 | 개발 결함 — 로그 + 일반 문구 |
  | 412 | `etag_mismatch` (`etag`=현재값) | 편집 중 타인이 갱신(If-Match) | 문서 재조회 후 재편집 안내 |
  | 404 | `not_found` | 삭제 대상 없음 | 목록 재조회 |

- **PUT 본문 규칙**(SDK `GroupDoc` 직렬화는 이미 서버 GET 포맷과 요소·네임스페이스 동일 — 실측 호환): 없는 요소는 갱신 시 기존값 유지, `<list>` 가 있으면 멤버 전체 교체. entry uri 는 PTT 가입 번호(`tel:+E.164`, `sip:` 형도 됨). `<mcpttgi:authorized-user>` 는 서버가 정함(보내도 무시). floor 정책(`floor_policy`/`max_talkers`)은 관리 API 전용이라 GMS 로 못 바꾼다.
- **If-Match**: 앱이 편집 시작 시 ETag 를 보내는 구조(`PutGroupAsync(..., ifMatch)`)는 서버 412 와 맞는다. ETag 는 내용 파생이라 같은 내용이면 같은 값.

---

## 3. Windows 실기 e2e 체크리스트 (라이브 csc 0.2.103)

관제 앱으로 아래를 한 번 돈다. 서버 쪽은 `cimsue-cli` 로 같은 사이클을 이미 통과했으니 앱 층만 본다.

1. disp01/1234 로그인 → PTT 주소록 [그룹] 탭에 **[새 그룹]** 이 보인다(`allowCreateGroup=true`).
2. 새 그룹 생성(이름·멤버 2~3명, disp02 포함) → 201 → 목록에 `is_owner=true` 로 나타나고 **affiliation·conference 구독이 자동 적용**된다.
3. **disp02 앱**(별도 로그인)에서 xcap-diff NOTIFY 로 목록이 **자동 갱신**돼 새 그룹이 보인다(멤버라서 포함, `is_owner=false` → [편집]/[삭제] 없음).
4. disp01 에서 이름·멤버 편집 → 200 → 양쪽 목록 갱신. If-Match 경로: 편집 창을 연 채 다른 경로로 바꾼 뒤 저장 → 412 처리 확인.
5. disp02 가 (UI 우회 등으로) 삭제 시도 시 403 `not_group_owner` 문구.
6. disp01 삭제 → 목록에서 사라지고 affiliation·conference 해제, disp02 도 NOTIFY 로 제거.
7. 존재하지 않는 번호를 멤버로 넣고 저장 → 400 `unknown_member` 문구에 번호 표시.

---

## 4. 관제석 번호 재키잉 반영 (09-06)

관제석 VoLTE 번호가 재키잉됐다: 관제1석 **`+821310001001`**, 관제2석 **`+821310001002`**, 그룹 대표번호 **`+821310001000`** (구 `+8231…` 은 삭제됨). PTT `+82510001001/2` 는 그대로. 내선 라벨 = E.164 끝자리 **1001 / 1002 / 1000(대표)**. 앱 directory(내선→E.164 매핑, `directory.sample.csv` 의 `ext` 행 관례)와 관제 그룹원 상태 띠·대표번호 대기열의 번호 참조를 새 값으로 맞춘다. 서버 프로비저닝 `dispatch.pilotId` 는 이미 `+821310001000` 을 준다.

---

## 5. 다음 서버 과제와 단말 준비 (서버 착수 순서 P2 → P3 → P3b)

| 서버 과제 | 단말이 지금 해둘 것 |
|---|---|
| **P2 ✅ 구현(csc 0.2.104)** `/provisioning/me` `dispatch.members[]`(userId·name·volteAor·pttId·extension·**groupId**) / `pttTargets[]`(id·**uri=tel: 형**·name) / `etag` + 응답 **`ETag`/`If-None-Match` 304** — 계약 [android_ue_provisioning.md §3](../design/features/android_ue_provisioning.md) | SDK `DispatchMember` 에 `groupId` 필드 추가(C API·.NET 파사드 동반). 앱: dialog watch 대상 = `members[]` 전체, **③ 그룹원 띠 = `groupId == dispatch.groupId`** 인 항목만(`listed`/`all` 은 자기 그룹 밖 가입자를 포함한다). conference 구독 = GMS 멤버 그룹 ∪ `pttTargets[]`(id 병합). 주기 재조회(예 60초)에 `If-None-Match: <ETag>` 를 붙여 304 면 무변경, 200 이면 `dispatch.etag` 비교 후 구독 집합 재적용. `extension` 은 서버 설정 자릿수(기본 4) |
| **P3 ✅ 구현(csp)** conference SUBSCRIBE 인가 — 규격형(TS 24.379 §10.1.3.4.1): 그룹 문서 `<on-network-allow-conference-state>`(GMS 문서 `<cp:actions>` 요소, 기본 true — `GroupDoc` 파서는 미지 요소로 무시해도 됨) + 비멤버 관제사 청취 범위 해석, 불허 시 **403 + `Warning: 138 CIMS "subscription of conference events not allowed"`**, 브로드캐스트 그룹 **480 + Warning 105** | 청취 범위 밖·자격 없는 `pttTargets[]`/비멤버 그룹 구독은 403 이 온다 — `Area.PttListen` 403 문구("청취 자격이 없거나 범위 밖")로 흡수하고 재시도 루프를 돌지 않게. Warning 138/105 는 로그에 남긴다. 480 은 "브로드캐스트 그룹은 참가자 정보를 제공하지 않음" 문구. 배포 = csp 다음 릴리스 + DB 마이그레이션 |
| **P3b ✅ 구현(csc 0.2.104)** 통합 이력 API `GET /provisioning/history?kind=call\|ptt\|message&since=&limit=` (PKCE, 관제 그룹 범위 게이트·커서 `since`/`nextSince`, 관제 미소속 403, 감사 E-AUD-016 `tap_mode=history`). 메시지 모니터링 = 실시간 사본 없이 "수초 내 이력 조회만"(kind=message = 그룹 SDS + 1:1). 1:1 SDS 는 CSP `Setup.McData.StoreOneToOneSds`(기본 off) 켜야 보관. 진행 중(live) 상태는 종전 RFC 4235/4575 구독(폴링 대체 안 함). 계약 [../design/features/android_ue_provisioning.md](../design/features/android_ue_provisioning.md) §3-2 | ②PTT 내역·④통화 내역·메시지 모니터링 패널이 `nextSince` 커서로 2~3초 폴링. 항목 형태(call/ptt/message)는 계약 §3-2. `CscClient.history(kind, since, limit)` |

---

## 6. 참고

- 서버 GMS 코드: `csc/src/services/mcptt.py` (`handle_group_management`·`parse_group_document_xml`·`gms_write_group`), HTTP 계층 `csc/src/httpsrv/controller.py`(XML media type 원시 바이트 전달 — 종전 415 가 XCAP PUT 본문 유실의 근본 원인이었다).
- 서버 단위시험: `tests/test_csc_gms_group_crud.py`(파서 왕복·인가 게이트·가짜 DB SQL 조립).
- 관제사 계정: disp01/disp02 (pw 1234), PTT `+82510001001/2`, 관제 그룹 `dg-dispatch01`.
