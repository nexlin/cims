---
name: 검증 6단계 재설계 + V2 UI 프로토타입 (2026-04-29)
description: 6단계(S1~S6) 재설계 합의안 + 30 부모/14 자식 매핑 + 그룹핑 인프라 + Console UI 프로토타입 (ems/core/console/src/pages/VerificationV2Page.tsx, /testbed/verify-v2 route). Phase A.5 거의 완료 미커밋. 다음 세션 Phase B (인프라 마이그레이션) 또는 검증 이력 페이지 신설.
type: project
originSessionId: 2e28cf33-1d54-41ed-9641-9d6c549f654f
---
## SOT — 2026-04-29 6단계 재설계 + V2 프로토타입

사용자 결정: 상용 배포 전 검증 절차를 6단계로 재설계.
이번 세션은 A.5 (Console UI 프로토타입) 거의 완료. mock 데이터로 백엔드 미연결 상태.

### 6단계 정의

| Stage | 이름 | scope | gate |
|---|---|---|---|
| **S1** | 정적 검사 | lint / format / unit test | 코드 위생 |
| **S2** | 빌드 | preflight + cmake build | 컴파일 통과 |
| **S3** | 스모크 검증 | configure + start (dev) + 1콜 VoIP/PTT | 빠른 회귀 (~1분) |
| **S4** | 패키지화 | tarball + hash 기록 (immutability) | S6 매칭 보증 |
| **S5** | 로컬배포 | reset → install (S4 산출물) → start → health | 배포 절차 회귀 |
| **S6** | 통합 검증 | VoLTE/PTT 음성·영상 + summary | 상용 진입 (~10분) |

**fail 처리**: gate 차단 + 사람이 재개 위치 결정. 자동 재시작 X. S4 → S6 패키지 hash 매칭 강제.
**단계 1 외부**: 사람 코드작업 (Claude Code 사용) 은 절차 외부. S1 은 자동 정적 검사만.

### 30 부모 항목 + 14 자식 매핑

```
S1 (5, 평면)
  S1-PY-SYNTAX           — python3 -m py_compile
  S1-FRONTEND-LINT       — npm run lint (ems/core/console)
  S1-FRONTEND-TYPECHECK  — tsc -b --noEmit
  S1-CPP-FORMAT          — clang-format --dry-run --Werror (csp/.clang-format)
  S1-UNIT-VERIFY-LIB     — python3 -m unittest tests.test_verify_lib (31 tests)

S2 (2, 평면)
  S2-PREFLIGHT  ← P1-PREFLIGHT
  S2-BUILD      ← P1-BUILD (산출물 자체검증 포함)

S3 (7, 평면)
  S3-CONFIGURE     ← P1-CONFIGURE
  S3-RESET         ← P1-RESET (dev 환경 wipe)
  S3-START         ← P1-START
  S3-SEED          ← P1-SEED
  S3-HEALTH        ← P1-HEALTH
  S3-SCN-VOIP-SMOKE ← P1-REGRESS-VOIP
  S3-SCN-PTT-SMOKE  ← P1-REGRESS-PTT

S4 (2, 평면)
  S4-PKG-BUILD     — cmd_pkg --no-bump (5개 tarball)
  S4-PKG-MANIFEST  — hash + timestamp 기록 (NEW, S6 매칭용)

S5 (7 부모 + 14 자식, 그룹화)  ← P2-RUN-ALL 22단계 분해
  S5-RESET                       (단계 1: cleanup)
  S5-CSC-DEPLOY [그룹]
    ├ AGENT-ENROLL  (5,6,7: TB-CSC login + agent enroll + Test-agent 9903)
    ├ PKG-UPLOAD    (8: csc/console → 4419)  ← P1-PKG-UPLOAD 흡수
    └ INSTALL       (9,10: deployment + install + poll)
  S5-CSC-VERIFY [그룹]
    ├ FILES         (11: meta.json + config/)
    └ OVERLAY       (12: overlay 반영)
  S5-CSC-RUN [그룹]
    ├ CSC-START     (13: 4445 LISTEN)
    ├ CSC-HEALTH    (14: health_check job)
    └ CONSOLE-START (15: 8081 LISTEN)
  S5-MODULES-DEPLOY [그룹]
    ├ AUTH          (16: 배포본 csc 4445 admin login)
    ├ PKG-UPLOAD    (17: csp/cmp/sim → 4445)
    ├ AGENT-ENROLL  (18: 3 agent + Test-agent 9904/5/6)
    └ INSTALL       (19,20: deployment + install)
  S5-MODULES-RUN [그룹]
    └ START         (21: csp 5060/udp + cmp 9000/udp)
  S5-FINALIZE                    (단계 22, 옵션 --stop-after)

S6 (7, 평면)
  S6-ENTRY-CHECK       ← P3-ENTRY-CHECK
  S6-SEED              ← P3-SEED
  S6-SCN-VOLTE-VOICE   ← P3-SCN-VOLTE-VOICE
  S6-SCN-VOLTE-VIDEO   ← P3-SCN-VOLTE-VIDEO
  S6-SCN-PTT-VOICE     ← P3-SCN-PTT-VOICE
  S6-SCN-PTT-VIDEO     ← P3-SCN-PTT-VIDEO
  S6-SUMMARY           ← P3-SUMMARY
```

**합계**: 5 + 2 + 7 + 2 + (7+14) + 7 = **44** (부모 30 + 자식 14)
**폐기**: P1-PKG-UPLOAD (S5-CSC-DEPLOY.PKG-UPLOAD 흡수), P2-RUN-ALL (S5 전체로 분해), S2-DIST-VERIFY 미신설

### 그룹핑 인프라 (Phase B 에서 구현)

```python
@verify_item(
    id="S5-CSC-DEPLOY", stage=5,
    is_group=True,           # 신설 — 자식만 가짐
    timeout_s=600,
)

@verify_item(
    id="S5-CSC-DEPLOY.AGENT-ENROLL", stage=5,
    parent="S5-CSC-DEPLOY",  # 신설
    timeout_s=60,
)
```

UI 표시:
- 평소: 부모만 (`▼ S5-CSC-DEPLOY ⏳ 2/3`)
- 펼침: 자식 들여쓰기 + 진행률
- 부모 status = 자식의 worst (FAIL 자식 있으면 부모 FAIL)
- 부모 체크박스: 자식 모두 토글 + indeterminate 표시
- stdout 마커: `child-result` 재활용

## V2 UI 프로토타입 (이번 세션 산출물 — 미커밋)

**신규 파일**: `ems/core/console/src/pages/VerificationV2Page.tsx` (1224 lines, mock 데이터)
**route 추가**: `ems/core/console/src/routes.tsx` 의 testbed section 에 `/testbed/verify-v2` 추가
**확인 URL**: `http://192.168.199.129:3000/testbed/verify-v2`

### 디자인 합의안 (Option C 하이브리드)

**1. Stepper (수평 6단계 흐름)**
- 원 120×120px, 흰 배경
- 안에 Stage ID (26px, 큰 글자) + 단계 이름 (12px) + 완료/전체 (11px)
- 외곽 테두리가 진행률 표시 (CSS `conic-gradient`):
  - PENDING: 회색 0%
  - RUNNING: 진행률 비율로 파란 호
  - PASS: 초록 가득 / FAIL: 빨강 가득 / BLOCKED: 노랑 가득
- 별도 status 아이콘 X (테두리 색상으로 충분)
- Stage 간 연결선: PASS 시 초록, 그 외 회색
- **클릭 = 재개 지점 설정** (accordion toggle 아님)

**2. Global Header**
- 시작/중단 toggle 버튼: `▶ 전체검증` ↔ `⏹ 전체검증 중단` (minWidth 160 고정)
- 재개 지점 dropdown (Run 버튼 옆, 파란 outline)
- 패키지 hash 카드 (`cims-2026.04.29-a3f2b1c (S4 ✅)`)
- 보고서 출력 버튼 (`📄 보고서 출력`)
- 전체 status 배지

**3. Stage Accordion**
- Stage 헤더 클릭 → accordion toggle (펼침/접힘)
- 헤더 우측: 단독 실행 toggle 버튼 (`▶ 검증` ↔ `⏹ 중단`, minWidth 110)
- 재개 지점인 stage 는 파란 border + glow + Stepper 상단 🚩 라벨
- 펼침 영역: 항목 테이블

**4. 항목 테이블**
- colgroup 컬럼 비율 (tableLayout: fixed):
  - 체크박스 32 / # 36 / 항목 28% / 설명 42% / 진행률 120 / 소요 70 / 결과 90
- 그룹 행: GroupCheckbox (자식 일부 선택 시 indeterminate)
- 그룹 cascade: 부모 토글 → 자식 모두, 자식 토글 → 부모 sync
- 자식 행: paddingLeft 32 들여쓰기, 옅은 배경, 작은 폰트

**5. PDF 보고서 (printable)**
- "📄 보고서 출력" → 모든 stage/그룹 펼침 → `window.print()`
- print CSS: 사이드바/헤더/서브탭/.v2-no-print/.stage-card 모두 `display: none`
  (visibility:hidden 은 layout 차지해서 빈 공간 생김 → display:none 필수)
- @page margin: 3mm 15mm 2mm 15mm (위 3 / 좌우 15 / 아래 2)
- **PrintReport 컴포넌트가 보고서 전체 그림** (화면 캡쳐 X):
  - **표지**: CIMS 검증 보고서 + 발행 일시 / 호스트 / git / 패키지 / 재개 지점
  - **1. 검증결과 요약**: 종합 판정 박스 + 1.1 단계별 요약 표 + 1.2 실패 항목 표 (있을 때만)
  - **2. 검증항목별 결과**: 전체 평탄 list 의 단순 표 (그룹 자식 들여쓰기 포함)
  - **3. 검증 상세내용**: stage 별 섹션 + 항목별 박스 (좌측 status 색 border) + 자식 표
  - **푸터**: 자동 생성 안내 + 발행 시각

**브라우저 인쇄 다이얼로그 안내**: `@page margin` 적용을 위해 인쇄 다이얼로그의 "여백" 을 "사용자 정의" 또는 "없음" 으로 설정해야 함 (Chrome "기타 설정"). "기본" 은 브라우저 자체 마진이 우선.

### 라벨 정리 (현재 적용된 표현)

| 위치 | 라벨 |
|---|---|
| 글로벌 시작 | `▶ 전체검증` |
| 글로벌 중단 | `⏹ 전체검증 중단` |
| Stage 시작 | `▶ 검증` |
| Stage 중단 | `⏹ 중단` |
| 재개 지점 dropdown 라벨 | `🚩 재개 지점` |
| 보고서 출력 | `📄 보고서 출력` |

### 페이지 안내 박스

`📝 프로토타입 안내` (.v2-no-print, print 시 숨김):
- mock 데이터, 백엔드 미연결
- "Run Full Pipeline" mock 시뮬레이션 (700ms 간격)
- S3 Seed FAIL → 후속 BLOCKED demo
- S5-CSC-DEPLOY 펼친 상태로 그룹핑 demo

## 다음 세션 우선 작업

### 🟢 옵션 1 — Phase B 인프라 마이그레이션 (큰 작업)

목표: V2 UI 가 실제 백엔드와 동작하도록 구조 변경.

| 작업 | 내용 |
|---|---|
| B1 | `verify/lib/items/phase{1,2,3}/` → `stage{1..6}/` 디렉토리 이동 |
| B2 | `@verify_item(phase=N)` → `stage=N` + `is_group` + `parent` 필드 신설 |
| B3 | CLI `--phase` → `--stage`, `cims.sh verify phaseN` → `stageN` |
| B4 | Backend API `/verification/phases/<N>` → `/stages/<N>` |
| B5 | V2 페이지를 mock 에서 실제 API 호출로 전환 |
| B6 | presets 재명명 (`stage1-full` ~ `stage6-full`, `pipeline-full`, `pre-package`, `post-deploy`) |
| B7 | `tests/test_verify_lib.py` 31개 갱신 |
| B8 | 메모리 + CLAUDE.md 갱신 |

### 🟢 옵션 2 — 검증 이력 페이지 신설 (NEW, 사용자 요청)

**필요성** (2026-04-29 사용자 요청): 과거 검증 실행 결과를 별도 페이지에서 조회/관리 필요.

**제안 사양** (다음 세션 기획 시 구체화):
- **route**: `/testbed/verify-history` (또는 `/testbed/verify/history`)
- **list view**:
  - 과거 검증 회차 list (역순)
  - 컬럼: 실행 일시 / 트리거 (사용자/cron/CI) / 재개 지점 / 결과 (PASS/FAIL) / 패키지 manifest hash / 소요시간 / 보고서
  - 필터: 결과 status / 기간 / 패키지 hash
  - 검색: git revision / 패키지 hash
- **detail view**:
  - 클릭 시 해당 회차의 보고서 표시 (PrintReport 재사용)
  - PDF 다시 출력 가능
  - 항목별 stdout/stderr log 보기 (옵션)
- **통계** (옵션):
  - 실패 추세 chart (시간순)
  - stage 별 성공률
  - 평균 소요시간
- **Backend**:
  - 신규 테이블: `verification_run` (id, started_at, finished_at, trigger, resume_stage, package_hash, status, summary_json)
  - 신규 테이블: `verification_run_item` (run_id, item_id, status, elapsed_ms, stdout, stderr)
  - V2 검증 실행 시 자동 기록 (Phase B5 작업 시 함께 wiring)
- **API**:
  - `GET /verification/runs?limit=N&offset=M&status=X` — list
  - `GET /verification/runs/<id>` — detail (item 결과 포함)
  - `DELETE /verification/runs/<id>` — 삭제 (옵션)

**우선순위**: Phase B5 (V2 → 실제 API) 작업과 함께 진행하는 것이 자연스러움. B5 가 검증 실행 결과를 DB 에 저장하는 것이 시작점.

### 🟢 옵션 3 — V2 UI 추가 다듬기

이번 세션 미해결 사항이 있으면 마무리:
- 미정 (사용자가 V2 페이지 검토 후 추가 요청 사항이 있으면)

## 미커밋 작업 (이번 세션)

```
M ems/core/console/src/routes.tsx                  # /testbed/verify-v2 route 추가
?? ems/core/console/src/pages/VerificationV2Page.tsx  # 1224 lines 신규
```

별도 변경 없는 항목:
- `verify/lib/` 인프라 (Phase 1/2/3 옛 체계 그대로 살아있음)
- backend `csc/src/handlers/verification.py` (옛 endpoint 그대로)

**커밋 시점**: Phase B 시작 전 또는 V2 UI 완성 후. 이번 세션은 사용자가 명시적으로 커밋 지시 없음.

## 함정 및 주의

- **immutability**: S6 검증은 S4-PKG-MANIFEST 가 기록한 hash 와 매칭 강제. 빌드 새로 했으면 S4 부터 다시.
- **그룹핑 vs sub-stage**: 6단계 컨셉 깨지 않으려고 그룹핑 채택. S5 안의 7 부모는 sub-stage 아님 (전부 stage=5).
- **S3-RESET vs S5-RESET**: 둘 다 `cmd_reset --all` 호출이지만 의미 다름. 분리 ID 유지.
- **Phase B 미진행 동안**: 기존 verify/lib (P1/P2/P3) 인프라 그대로 살아있음. /testbed/verify 옛 페이지 정상 동작.
- **print CSS 함정**: `visibility: hidden` 은 layout 공간 차지함. `display: none` 사용해야 보고서 출력 시 빈 공간 안 생김.
- **브라우저 인쇄 다이얼로그**: @page margin 적용을 위해 "여백" 을 "사용자 정의" 또는 "없음" 으로 설정 필요. "기본" 이면 브라우저 자체 마진이 우선.
