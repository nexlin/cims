---
name: CIMS 배포 아키텍처 핵심 사실
description: Agent/Package/Deployment/Collection 데이터 모델과 흐름 — 작업 시 빠른 참조용
type: project
originSessionId: 078c269a-6e1d-46ac-b647-d08d4f8bc5ac
---
**Why:** 분산 배포 시스템 작업 시 흔한 실수 방지 (e.g., instance_id 로 scope 하려 함 → 잘못된 방향).

**How to apply:** 배포 관련 작업 전에 이 메모 먼저 확인.

**핵심 엔티티 (2026-04-21 이후)**:
- `cims_package` — 업로드된 모듈 tarball. meta_json + config_template_json 컬럼
- `cims_agent` — 각 호스트 agent. sync_port 컬럼 (heartbeat 로 보고)
- `agent_deployment` — "Agent × Package × process × functions" 인스턴스. process_name 은 "CSP/PSP/ISP" 처럼 바이너리 변종, service_functions 는 "volte,ptt,ibcf" 콤마 문자열
- 기존 `csp_listener`, `sip_trunk`, `routing_rule`, `routing_access_list`, `sip_service` 테이블은 **deprecated** (사용 중단 예정)

**설치 경로 규칙**:
```
<agent 설치>/modules/<module>/<version>/<process>/
  ├── config.json      ← scalar settings (template sections)
  └── config/*.jsonl   ← collections (listeners, trunks, ...)
```
버전별 디렉토리 공존, 업그레이드 시 agent 가 이전 config 자동 복사.

**스코프 원칙**:
- Collection 은 deployment_id 가 아닌 **install_path** 로 구분 (각 deployment 는 자기 디렉토리)
- DB 는 scalar config 만 보관, collection 은 agent 호스트 파일이 원천

**흔한 함정**:
- "instance_id" 로 listener 묶으려 하지 말 것 — 구조 문제 있음 (Phase B 설계 시 검토 완료)
- UUID 를 int 로 파싱하는 코드 있음 (C++ managers) — Phase C 에서 수정 예정
