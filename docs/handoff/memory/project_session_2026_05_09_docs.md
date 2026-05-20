---
name: 2026-05-09 세션 — docs/ 현행화 + SSOT 단일화
description: 22 문서 전수 검토 후 옛 정보 제거·중복 통합·현행 코드 정렬. commit 5868984.
type: project
originSessionId: b0494a4a-248b-4aaf-a999-89d6c822fe61
---
# 2026-05-09 세션 — docs/ 현행화

**상태**: commit + push 완료 (`5868984`).

## 산출물

**신규**
- `docs/design/features/build_and_packaging.md` (195줄) — 콘솔 `/release/package` 4단계 워크플로우, ▶ 빌드 & 패키징 통합 job, manifest.json 스키마 SSOT, csc.json:Packages.Dir ↔ build/dist/packages 분리

**삭제 (과감히)**
- `docs/PHASE_C_TODO.md` 전체 — Phase C 완료 후 historical
- `VERIFICATION_PROCESS.md` 부록 D (옛 Phase 1/2/3 매핑)
- `VERIFICATION_MANUAL.md` ~160줄 (inject-fail 시연 / retention / webhook 중복)

**SSOT 일원화**
- P1 토폴로지 (5 server) → `VERIFICATION_PROCESS.md §1 S5` SSOT
- 변종 12종 staging → `build_and_packaging.md §2` SSOT
- manifest.json 스키마 → `build_and_packaging.md §5` SSOT

**현행 코드 정렬**
- `01_overview.md` 핵심 클래스: 옛 `CSipServer/CUserMap` → 현재 `CModuleDispatcher` + 4 모듈
- `csc.md` 핸들러: 옛 `cims_*.py` 7개 → 현재 `csc/src/handlers/{auth,admin,users,org,stats,recording,verification,build,agents,agent_api,modules,service_control,csp_runtime}.py` + `services/{flow_logger,mcptt}.py`
- `admin_api.md` §13 검증 API: 옛 GET /run 1개 → 12 endpoint
- `admin_api.md` §14 빌드/패키징 API 신설
- 옛 `csc/bin/csc_pihttp/` 경로 → `csc/src/` 일괄
- 링크 `13_Flow_Logging_Design.md` 5건 → `../features/flow_logging.md`

총 변경: 15 파일 수정 (-580 / +429) + 1 신규 + 1 삭제

## 검증 (grep 0 hit)

`csc_pihttp | cims_admin | cims_auth | 13_Flow_Logging | verify_phase2 | /testbed/ | tarball 5 | 모듈관리 | Phase A | Phase B | Phase C` 모두 0 hit (옛 ↔ 신 매핑 설명 제외).

## 다음 세션 시 참고

문서는 이제 정확함. 새 코드 변경이 있을 때 docs/ 동기화는:
- URL/메뉴 변경 → README.md, VERIFICATION_MANUAL.md, deployment_workflow.md
- 빌드/패키지 변경 → build_and_packaging.md (SSOT), package_and_template.md
- 검증 파이프라인 변경 → VERIFICATION_PROCESS.md (SSOT), MANUAL 은 체크리스트만
- 토폴로지 변경 → VERIFICATION_PROCESS.md §1 S5 (SSOT), 02_deployment.md 는 링크
- API 변경 → admin_api.md (CSC 4420), agent_api.md (Agent), collection_api.md (jsonl)
- CSC handler 추가 → csc.md §2.2 + §9 파일 구조 + admin_api.md
