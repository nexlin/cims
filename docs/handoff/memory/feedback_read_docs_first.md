---
name: 검증/프로세스 작업 전 docs/ 먼저 읽기
description: 검증 · 배포 · 프로세스 관련 작업을 설계/구현할 때 반드시 docs/ 내부의 해당 프로세스 문서를 먼저 읽고 시작. 추측으로 구현 후 사용자 지적 반복 금지.
type: feedback
originSessionId: 11577459-2194-4246-a25b-bcf3d20b6c95
---
검증/배포/릴리즈 프로세스 관련 작업 시작 전 반드시 `docs/` 내 해당 프로세스 문서를 먼저 읽는다.

**Why**: 2026-04-24 세션에서 `cims.sh verify phase2` 를 사용자가 요청. 문서를 읽지 않은 상태에서
"Phase 2 = tarball 구조 검증 + CSC 업로드 smoke" 로 추측해 구현했으나, 실제로 
`docs/VERIFICATION_PROCESS.md` 에서는 Phase 2 가 **TB-CSC(4419) 경유 배포 기능 검증** 으로 
구체적으로 정의돼 있어 전부 재설계 필요. 사용자가 문서를 지목하고서야 확인 → 구현물 폐기.

**How to apply**:
- "verify", "phase", "deploy", "release", "QA", "검증" 등 키워드의 작업이 들어오면
  우선 `docs/` 아래에서 해당 주제의 문서를 grep/find 로 검색.
- 문서를 찾으면 full-read 로 정독한 뒤 구현 설계 제안.
- 문서가 메모리에 이미 요약돼 있으면 (예: `project_verification_process.md`) 그것을 먼저 읽고 
  필요 시 원본 문서로 확장.
- 관련 문서 없으면 사용자에게 "참고할 프로세스 문서가 있습니까?" 라고 선제 질문.
