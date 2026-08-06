# CIMS 문서

CIMS (CIMS IMS) 는 PTT/VoLTE 통합 서비스 서버입니다. 이 디렉토리는 설계서·API 명세·사용자 매뉴얼을 담고 있습니다.

## 디렉토리 구조

```
docs/
├── README.md                        본 문서
├── VERIFICATION_PROCESS.md          6단계 (S1~S6) 파이프라인 SSOT
├── VERIFICATION_MANUAL.md           검증 실행 체크리스트 + CLI/API 사용
├── design/               설계 문서 (아키텍처·모듈·기능)
│   ├── 01_overview.md               전체 시스템 아키텍처
│   ├── 02_deployment.md             분산 배포 아키텍처 (Agent/Package/Deployment)
│   ├── console_platform.md          콘솔 플랫폼화 (위젯 합성 · shape 위젯 · 데이터 소스 descriptor 등록)
│   ├── alarm_standardization.md      알람 표준화 설계 (X.733 / 3GPP TS 32.111 — code·severity6·eventType·probableCause·source)
│   ├── modules/                     각 모듈별 상세 설계
│   │   ├── csp.md                   CSP — SIP 시그널링 (CSCF/TAS/PTT-AS/IBCF)
│   │   ├── cmp.md                   CMP — RTP 릴레이 + PTT 믹싱
│   │   ├── csc.md                   CSC — 관리/인증/MCPTT API
│   │   └── agent.md                 Agent — 배포/프로세스 제어 데몬
│   └── features/                    기능별 설계
│       ├── volte_flows.md           VoLTE 호처리 Flow
│       ├── ptt_flows.md             PTT 그룹콜 Flow
│       ├── ue_nat_traversal.md      단말 NAT traversal (leg 포트 · 목적지 latch · 정책)
│       ├── recording.md             녹취
│       ├── monitoring.md            모니터링·이력·통계
│       ├── flow_logging.md          Flow 로깅/상관관계 (sesid)
│       ├── sip_runtime_config.md    SIP 런타임 설정 (jsonl + SIGUSR1)
│       ├── sip_service_model.md     SIP 서비스 모델 (Service/Trunk/Listener)
│       ├── ha_service_model.md      HA 서비스 운영 모델 (책임 분리·선언적 verdict·절체 판정 — 설계 정본, 단계적 이행)
│       ├── package_and_template.md  패키지 포맷 + config_template.json 스키마
│       └── build_and_packaging.md   빌드/패키징 워크플로우 (콘솔 /release/package) · manifest.json SSOT
├── api/                  REST API 명세
│   ├── admin_api.md                 관리자 API (가입자/그룹/검증/빌드/패키지/서버/배포)
│   ├── agent_api.md                 Agent ↔ CSC (enroll/heartbeat/report)
│   ├── collection_api.md            Collection 프록시 (/deployments/{id}/collection)
│   ├── cmp_media_api.md             CMP 미디어 서비스 제어 API (UDP JSON envelope v2)
│   └── mcptt_api.md                 3GPP MCPTT (IdMS/GMS/CMS/KMS)
└── user-manual/          사용자 매뉴얼
    ├── ue_interface.md              단말 연동 개요
    ├── volte_ue.md                  VoLTE 단말 연동 가이드
    ├── ptt_ue.md                    PTT(MCPTT) 단말 연동 가이드
    └── deployment_workflow.md       Console 배포 작업 순서
```

## 빠른 시작

| 목적 | 먼저 볼 문서 |
|---|---|
| 시스템 전체 이해 | `design/01_overview.md` → `design/02_deployment.md` |
| 관리자 API 사용 | `api/admin_api.md` |
| 서버 배포 진행 | `user-manual/deployment_workflow.md` |
| 모듈 소스 수정 | `design/modules/<module>.md` |
| UE 연동 개발 | `user-manual/<volte|ptt>_ue.md` |
| 빌드/패키징 워크플로우 | `design/features/build_and_packaging.md` |
| 검증 절차 (S1~S6) | `VERIFICATION_PROCESS.md` (SSOT) → `VERIFICATION_MANUAL.md` |

## 관련 자료

- 빌드/실행: 저장소 루트 `README` (CLAUDE.md 의 Build 섹션 참조)
- SQL 스키마/마이그레이션: `sql/`
- 런타임 설정 템플릿: `csp/config/config_template.json`, `cmp/config/config_template.json`, `csc/config/config_template.json`
- 검증 절차 SoT: `VERIFICATION_PROCESS.md` (실행 가이드는 `VERIFICATION_MANUAL.md`)
- 콘솔 메뉴 구조:
  - `대시보드` / `가입자관리` / `서비스` / `통계`
  - `패키징` (`/release/...`) — 검증 실행 / 검증 이력 / 빌드 & 패키징
  - `배포` (`/deploy/...`) — 패키지 업로드 / 서버 등록
