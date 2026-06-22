---
name: 2026-05-08 세션 (4 라운드) — 패키징 메뉴 재정비
description: /testbed → /release URL, 카드 4단계, 빌드+패키징 통합, 빌드 시 버전 결정, manifest 자동 생성. 모두 unstaged.
type: project
originSessionId: db0badef-da01-4396-bc02-9d22bd9c19ab
---
# 2026-05-08 세션 (4 라운드) — 패키징 메뉴 재정비

**상태**: **commit + push 완료** (`f4d90ef`, 2026-05-08).

## 변경 파일 (5개, ~1500줄)

```
ems/core/console/src/routes.tsx
ems/core/console/src/pages/ServicesPage.tsx
ems/core/console/src/pages/VerificationV2Page.tsx
ems/core/console/src/pages/VerificationHistoryPage.tsx
ems/core/console/src/api/build.ts
csc/src/handlers/build.py
csc/config/config_template.json
cims.sh
```

## 4 라운드 변경 흐름

### R1. 카드 모델 분리 (프로세스/패키지 축)
- `BuildCard.variants` → `key` (프로세스, ServiceName) + `packageVariants?` (패키지 산출물)
- `cardPackages(c)` 헬퍼
- 백엔드 `service_control._ALLOWED` 가 변종 (psp/isp/pmp/imp) 거부 — UI 도 그에 맞춤

### R2. 카드 워크플로우 4단계 (¹설정/²실행/³패키징/⁴다운로드)
- 헤더 + 2col×2row 그리드. 단계 라벨 ¹²³⁴
- 사용자 결정: "항상 노출 (조건부 X)"
- `hasProcess=false` 카드는 ² 실행만 자리 표시

### R3. URL/메뉴 정리
- `/testbed` → `/release`, `verify-v2` → `verify`, `modules` → `package`
- 메뉴 라벨 "빌드·검증" → **"패키징"**
- defaultPath `/release/verify` (메뉴 클릭 시 검증부터)
- VerificationV2Page 의 모든 `/testbed/...` 링크 + `.verify-v2-page` CSS 클래스 갱신

### R4. 빌드+패키징 통합 + 정리 + 빌드 시 버전 결정
- 사용자 결정: **빌드 시점에 버전 결정** (cims.sh build -v X.Y.Z 가 모든 pkg.json 갱신)
- 헤더 단일화: `[v X.Y.Z] [▶ 빌드 & 패키징] [🗑 정리] [↻ 새로고침]`
- 카드 ³ 패키징 영역 통째 제거 → ³ 다운로드만 (전체 폭, 라벨에 버전 표시)
- 일괄 액션 바 제거, 카드 체크박스 제거 (개별 ▶/■/↻ 만 충분)
- 미사용 정리: `selectedVersion`, `setSelectedVersion`, `curVer`, `startPkgForCard`, `bulkAct`, `selected`, `PROCESS_NAMES`, `isCriticalCard`

## 백엔드 변경

### `cims.sh`
- `cmd_build [-v X.Y.Z]` 추가 — 빌드 후 8개 컴포넌트 (csp/cmp/csc/cwrtc/cspsim/agent/ems/core/console/cims-phone) 의 source `pkg.json` 일괄 갱신. 변종 (psp/isp/pmp/imp) 은 base 의 pkg.json 공유.
- `cmd_pkg` 끝에 **manifest.json 자동 생성** (verify S4-PKG-MANIFEST 와 같은 로직 inline python heredoc — sha256/size/mtime/git/host)

### `csc/src/handlers/build.py`
- `_start_build` body version → `cims.sh build -v` 전달
- `_start_pkg` body version → `cims.sh pkg -v` 전달 (호환 유지, 현재 UI 안 씀)
- **`/build/release` POST endpoint** — `cims.sh build [-v X.Y.Z] && cims.sh pkg --no-bump` 한 job. job kind='release'.
- **`/build/clean` POST endpoint** — `_DIST_PKG_DIR/*.tar.gz` + `_DIST_PKG_DIR/manifest.json` 삭제. 빌드 결과는 유지.
- 정규식 검증 `[0-9A-Za-z._+\-]{1,64}` (shell injection 방지)

### `csc/config/config_template.json`
- `Packages.Dir` / `Packages.BackupDir` 의 `"hidden": true` **제거** + help 텍스트 추가
- 사용자가 CSC 카드의 ¹ 설정 모달에서 "패키지 저장소" 그룹 직접 편집 가능

## 검증 완료

| 영역 | 결과 |
|---|---|
| tsc -b / eslint / vite build | ✅ 모두 통과 |
| python ast.parse build.py | ✅ |
| bash -n cims.sh | ✅ |
| **두 디렉토리 분리 검증** | ✅ `agents.py:_create_package` (line 701-705) 만 csc.json `Packages.Dir` 사용. `build.py:_DIST_PKG_DIR` 와 무관. 코드 레벨 완전 분리. |

## 적용 상태 (런타임)

- dist sync csc + scripts ✅ (4월 28일 → 5월 8일 23:xx)
- tb-csc 재시작 ✅ (pid 1828365 → 1839241 → 1848582)
- CSC 본체 (pid 1682014) **재시작 안 됨** — prod console (4400) 사용 시 옛 build.py. dev console (3000) 만 사용하면 무관.

## 다음 세션 시작 시 확인할 것

```bash
cd /home/nex/work/cims
git status                                      # 8 파일 unstaged
git diff --stat                                 # ~1500 줄 변경
./cims.sh status                                # tb-csc 살아있는지
ls build/dist/packages/                         # 빌드 산출물
ls build/dist/csc/packages/                     # 배포 업로드 (별개)
cat build/dist/packages/manifest.json | python3 -m json.tool | head    # 새 manifest 정상?
```

## 미해결/추후 의제

- **commit + push** — 사용자 의도 확인 후
- **CSC 본체 재시작** — prod console (4400) 도 새 build.py 쓰려면
- **빌드 산출물 ↔ 배포 업로드 자동 동기화** (선택) — 사용자가 빌드 후 업로드 페이지에 자동 반영 원할 시
- **0.0.10 vs 0.0.2 alphabetical sort 문제** (잠재) — manifest 의 packages 가 alphabetical 정렬 시 frontend tarballByModule 마지막 entry 가 잘못된 버전 가능. 큰 버전 패치 시 보강 필요.
