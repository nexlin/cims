# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> 이 문서는 **프로젝트 개요·빌드 환경·문서 맵·작업 원칙**만 담는다. 아키텍처/모듈/기능의
> 상세 설계는 [docs/](docs/) 의 해당 문서가 정본이며, 여기서는 그 경로만 가리킨다.

## 프로젝트 개요

CIMS 는 3-tier PTT/VoIP 서버다. **CSP** 가 SIP 시그널링을, **CMP** 가 RTP 미디어를 담당하고,
**cspsim** 이 단말을 시뮬레이션한다. 운영·관리 평면(OAM)과 콘솔, 가입자/MCPTT 서버(CSC)가 이를
구성·배포·감시한다.

```
cspsim  ←─ SIP (UDP 5060 / TCP 25061 / TLS 5061) ──→  CSP
                                                         │
                                               UDP JSON (port 9001)
                                                         │
                                                        CMP  ←─ RTP ─→ clients
```

| 구성요소 | 역할 | 정본 설계 문서 |
|---|---|---|
| **CSP** (`csp/`) | IMS 역할 기반 모듈형 SIP 서버 (CSCF/TAS/PTT-AS/IBCF) | [docs/design/modules/csp.md](docs/design/modules/csp.md) |
| **CMP** (`cmp/`) | RTP relay + MCPTT floor control (CSP 의 UDP JSON 제어) | [docs/design/modules/cmp.md](docs/design/modules/cmp.md) |
| **CMDP** (`cmdp/`) | MCData media plane — 대용량 SDS MSRP 종단 + FD 스토어 (CSP 의 UDP JSON 제어) | [docs/design/features/mcdata_messaging.md](docs/design/features/mcdata_messaging.md) §4.7 |
| **cspsim** (`cspsim/`) | SIP/RTP 부하·기능 시험용 단말 시뮬레이터 | — |
| **CSC** (`csc/`) | 가입자 관리 + MCPTT(IdMS/GMS/CMS/XCAP) 서버 | [docs/design/modules/csc.md](docs/design/modules/csc.md) |
| **OAM/Console** (`ems/`) | 운영·관리 평면 게이트웨이 + 웹 콘솔 (core/service 분리) + 자동 배포 엔진 내장 | [docs/design/console_platform.md](docs/design/console_platform.md), [docs/design/oam_csc_split.md](docs/design/oam_csc_split.md) |
| **Agent** (`agent/`) | 노드 에이전트 (배포/HA/업그레이드 supervised) | [docs/design/modules/agent.md](docs/design/modules/agent.md) |

전체 아키텍처 개요는 [docs/design/01_overview.md](docs/design/01_overview.md), 배포 아키텍처는
[docs/design/02_deployment.md](docs/design/02_deployment.md) 를 본다.

## 개발/빌드 환경

**Prerequisites**: `cmake`, `build-essential`, `libssl-dev`, `libmariadb-dev`, `git`, `clang-format`

```bash
sudo apt-get install -y cmake build-essential libssl-dev libmariadb-dev clang-format
```

`libmariadb-dev` 는 MariaDB **클라이언트 라이브러리·헤더** (DB 서버 아님) — CSP 빌드 필수
(`csp/CMakeLists.txt` 가 없으면 configure 중단). MariaDB 서버는 별도 장비에 둘 수 있다
([docs/DEV_SERVER_SETUP.md](docs/DEV_SERVER_SETUP.md) §1.2, §6).
`clang-format` 은 검증 stage 1 (`S1-CPP-FORMAT`) 의 정적 검사용. 미설치 시 SKIP.

**Build** (out-of-source, 레포 루트에서):
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

첫 빌드는 외부 의존성(oneTBB, opencore-amr, vo-amrwbenc, googletest, psip/pasf)을 내려받아
컴파일한다 (CMake `ExternalProject_Add`). 바이너리는 `build/bin/` 에 생성된다.

**배포 패키지**:
```bash
cd build && make dist     # build/dist/ (컴포넌트별 tarball + manifest)
```

**개발 원스톱**: `./cims.sh up [--skip-build]` — build → configure -y → 전체 재시작.

**단일 프론트/엔진 경계**: 개발 서버에서는 `cims.sh` 가 단일 진입점(프론트) — 빌드/설정/패키징에
더해 기동·상태·로그(`start|stop|restart|status [--full]|log`)와 검증(`verify`)도 서브커맨드로
위임 실행한다. 정본(엔진)은 `agent/bin/cims-svc`(운영 lifecycle — 배포본·agent·OAM 이 직접 호출)
와 `./cims-verify`(검증, 소스 트리 전용). 엔진은 dist 계약만 취급 — 소스 트리 전용 동작(vite dev
콘솔 등)은 프론트에만 둔다.

> cwrtc(WebRTC 게이트웨이)·cims-phone(웹 단말)은 **재설계 예정** — 빌드/패키징/기동 대상에서
> 제외 (소스만 보존).

**실행**: `./cims.sh start` (기동 순서·pid/log 관리는 엔진이 처리). 개별 바이너리 직접 실행 시
CMP 를 CSP 보다 먼저 기동:
```bash
./bin/cmp ../cmp/cmp.json
./bin/csp ../csp/csp.json -n          # -n=foreground (백그라운드는 csp.sh)
```

**시뮬레이터(cspsim)·검증 파이프라인(S1~S6)** — 단말 시뮬레이터 사용법과 상용 배포 전 검증
게이트는 검증 문서가 정본이다. 절차·게이트 정의는
[docs/VERIFICATION_PROCESS.md](docs/VERIFICATION_PROCESS.md), 실행/시뮬레이터 사용법은
[docs/VERIFICATION_MANUAL.md](docs/VERIFICATION_MANUAL.md) 를 본다. 진입점은 `cims-verify`
또는 콘솔 `/testbed/verify-v2`.

### 디렉토리 구조

```
csp/        CSP — SIP 서버 (소스 + csp.json)
cmp/        CMP — 미디어 서버 (소스 + cmp.json, config_template.json)
cmdp/       CMDP — MCData 미디어평면 (MSRP 종단, cmdp.json)
cspsim/     단말 시뮬레이터
csc/        CSC — 가입자/MCPTT 서버 (Python), csc.json
ems/        OAM + 콘솔
  core/     { oam/ (base 게이트웨이=`oam` 패키지), console/ (공통 셸·base 메뉴) }
  service/  { oam/ (서비스 모듈=`oam-svc` 패키지), console/ (서비스 팩) }
agent/      노드 에이전트
deployment/ 부트스트랩·DB 부트스트랩 모듈
verify/     S1~S6 검증 인프라 (verify/lib/)
tests/      테스트
scripts/    개발 파이프라인 단위 스크립트 (sync.sh, package.sh, lib/common.sh) + 운영/시험 스크립트
sql/        DB 스키마/마이그레이션
ext/, pkg/  외부 의존성 소스 / 설치 산출물
docs/       설계·API·사용자 매뉴얼 문서 (아래 참조)
```

> **설정 파일 위치**: `csp/csp.json`, `cmp/cmp.json`, `csc/bin/csc_pihttp/config/csc.json`.
> 가입자/그룹 데이터는 DB(MariaDB) primary, `csp/User/`·`csp/Group/` JSON fallback.
> 각 설정 키의 의미는 해당 모듈 설계 문서를 참조한다.

## 참조 문서 (docs)

상세 설계·동작은 아래 문서가 정본이다. 기능 작업 전 관련 문서를 먼저 읽는다.

**최상위**
- [docs/README.md](docs/README.md) — docs 인덱스
- [docs/DEV_SERVER_SETUP.md](docs/DEV_SERVER_SETUP.md) — 개발 서버 셋업
- [docs/VERIFICATION_PROCESS.md](docs/VERIFICATION_PROCESS.md) / [VERIFICATION_MANUAL.md](docs/VERIFICATION_MANUAL.md) — 검증 절차·매뉴얼

**design/** (아키텍처·플랫폼)
- [01_overview.md](docs/design/01_overview.md) — 전체 아키텍처 개요
- [02_deployment.md](docs/design/02_deployment.md) — 배포 아키텍처/절차
- [ha_design.md](docs/design/ha_design.md) — HA 설계
- [db_schema.md](docs/design/db_schema.md) — DB 스키마 (file_store SoT 포함)
- [console_platform.md](docs/design/console_platform.md) — 콘솔 플랫폼
- [oam_csc_split.md](docs/design/oam_csc_split.md) — OAM/CSC 분리 경계·인증·토폴로지
- [csc_config_server.md](docs/design/csc_config_server.md) — CSC config server
- [csp_control_plane_load_hardening.md](docs/design/csp_control_plane_load_hardening.md) — CSP 제어평면 부하 대책
- [alarm_standardization.md](docs/design/alarm_standardization.md) — 알람 표준화
- [alarm_self_reporting.md](docs/design/alarm_self_reporting.md) — 모듈 알람/이벤트 자기보고(FM push) 경로
- [alarm_pipeline.md](docs/design/alarm_pipeline.md) — 알람/이벤트 파이프라인 — 발생→전달→수집/보관→가시화 전 구간 절차·연동 계약 정본
- [alarm_module_catalog.md](docs/design/alarm_module_catalog.md) — 모듈 자기감지(L2) 알람/이벤트 카탈로그 설명서 (목록 정본 = [alarm_module_catalog.csv](docs/design/alarm_module_catalog.csv))
- [alarm_function_catalog.md](docs/design/alarm_function_catalog.md) — IMS 기능(CSCF/IBCF/TAS/PTT-AS/MRF) 관점 필요 알람/이벤트 **요구 카탈로그** — 구현 무관 정본 (목록 = [alarm_function_catalog.csv](docs/design/alarm_function_catalog.csv))
- [vibcf_pod_alarms.md](docs/design/vibcf_pod_alarms.md) — 사내 vIBCF/TrGW POD 알람/Fault 카탈로그 변환 참고자료 (CIMS 대조 = alarm_standardization §7.2)
- [runtime_store_design.md](docs/design/runtime_store_design.md) / [runtime_store_v2_module_namespacing.md](docs/design/runtime_store_v2_module_namespacing.md) — 런타임 스토어

**design/modules/** (컴포넌트별 상세)
- [csp.md](docs/design/modules/csp.md) · [cmp.md](docs/design/modules/cmp.md) · [csc.md](docs/design/modules/csc.md) · [agent.md](docs/design/modules/agent.md)

**design/features/** (기능별 상세)
- [ptt_flows.md](docs/design/features/ptt_flows.md) — PTT(MCPTT) 케이스·메시지 flow
- [volte_flows.md](docs/design/features/volte_flows.md) — VoLTE 호 flow
- [mcptt_authorization.md](docs/design/features/mcptt_authorization.md) — MCPTT 권한/RBAC
- [mcptt_emergency_modes.md](docs/design/features/mcptt_emergency_modes.md) — 긴급/임박/알림/ad-hoc 모드
- [mcptt_standard_conformance.md](docs/design/features/mcptt_standard_conformance.md) — MCPTT 서버(CSC/CSP/CMP) 3GPP TS 규격 정합 보완 사항(단말 interop 전제) + §0-R 미반영 로드맵
- [mcptt_csp_cmp_roadmap_contract.md](docs/design/features/mcptt_csp_cmp_roadmap_contract.md) — 로드맵 기능(private call·dual/multi-talker·pre-established 등) CSP↔CMP 연동 메시지 규격, Call Control/Media Plane 2파트 분담 계약
- [mcdata_messaging.md](docs/design/features/mcdata_messaging.md) — MCData 그룹 메시징(SDS) — TS 24.282 그룹 SDS·TS 24.481 그룹별 게이트·disposition
- [ue_nat_traversal.md](docs/design/features/ue_nat_traversal.md) — 단말 NAT traversal (시그널링·미디어 leg 포트·목적지 latch·정책)
- [sip_tls_signaling.md](docs/design/features/sip_tls_signaling.md) — SIP TLS 시그널링 (transport 별 도달 모델 = 목적지 주소 vs 연결 열쇠, latch 갱신 규율, 접속점 개설 실패 격리·A-PRC-012, 단말 전환 계획 — 서버측 구현 완료, 단말 미구현)
- [leg_liveness.md](docs/design/features/leg_liveness.md) — 비정상 종료 leg 감지 (SIP 세션 타이머 RFC 4028 — BYE 없이 사라진 leg 의 시한 회수. 설계 정본, 구현 착수 전)
- [recording.md](docs/design/features/recording.md) — 녹취 구조 (슬롯 트랙·믹스/단독 재생·PTT 세션 이력 UI)
- [flow_logging.md](docs/design/features/flow_logging.md) — SIP/Flow 로깅 (sesid 규칙·5분 버킷)
- [monitoring.md](docs/design/features/monitoring.md) — 모니터링
- [sip_service_model.md](docs/design/features/sip_service_model.md) / [sip_runtime_config.md](docs/design/features/sip_runtime_config.md) — SIP 서비스 모델·런타임 설정
- [ha_service_model.md](docs/design/features/ha_service_model.md) — HA 서비스 운영 모델 (책임 분리·선언적 verdict·절체 판정 — 설계 정본, 단계적 이행)
- [oam_base_service_split.md](docs/design/features/oam_base_service_split.md) / [oam_self_upgrade.md](docs/design/features/oam_self_upgrade.md) — OAM base/service 분리·self-upgrade
- [oam_ha.md](docs/design/features/oam_ha.md) — 관리평면(OAM) 이중화 A/S (서비스 그룹 동거·모듈별 health, 공유 스토리지(NAS) + 소유권 리스 펜싱, 그룹 공통 신원, 자기 계획절체 — 설계 정본)
- [auto_deployment.md](docs/design/features/auto_deployment.md) — 자동 배포 (인벤토리+블루프린트 YAML → SSH agent 설치·시스템 구성·모듈 설치. **OAM 내장** — `services/provision/`, 콘솔 `관리>릴리스>자동 배포`, CLI `scripts/prov`)
- [api_docs.md](docs/design/features/api_docs.md) — 위젯별 사용 API 노출 (모듈이 코드 옆에 자기 API 선언 + 위젯이 쓰는 id 선언 → 개발자 모드 `[API]` 배지)
- [csc_standalone_module.md](docs/design/features/csc_standalone_module.md) — CSC 독립 모듈화
- [build_and_packaging.md](docs/design/features/build_and_packaging.md) / [package_and_template.md](docs/design/features/package_and_template.md) — 빌드·패키징·템플릿
- [android_ue_client.md](docs/design/features/android_ue_client.md) / [android_ue_m1_pjsip_integration.md](docs/design/features/android_ue_m1_pjsip_integration.md) — Android UE 클라이언트
- [mcptt_ue_multitalker_media.md](docs/design/features/mcptt_ue_multitalker_media.md) — 단말 동시 발언 미디어 평면(U10) 선택지·구현 설계 + floor 코덱 공유/정의 단일화 검토 (pjproject·안드로이드 빌드 환경 필요)
- [android_ue_provisioning.md](docs/design/features/android_ue_provisioning.md) — UE 로그인·자동 프로비저닝(서비스별 프로파일, CSC `/provisioning/me`)

**api/**
- [admin_api.md](docs/api/admin_api.md) · [collection_api.md](docs/api/collection_api.md) · [agent_api.md](docs/api/agent_api.md) · [mcptt_api.md](docs/api/mcptt_api.md) · [cmp_media_api.md](docs/api/cmp_media_api.md) — CMP 미디어 서비스 제어 API (UDP JSON envelope v2)

**user-manual/**
- [deployment_workflow.md](docs/user-manual/deployment_workflow.md) · [ue_interface.md](docs/user-manual/ue_interface.md) · [volte_ue.md](docs/user-manual/volte_ue.md) · [ptt_ue.md](docs/user-manual/ptt_ue.md)

## 개발/기능보완/문서 현행화 원칙

**설계 우선순위 (기능 보완/추가 시)** — 아래 순서로 판단한다.
1. **표준규격 준수가 최우선.** 관련 3GPP TS/RFC 규격에 부합하는지를 가장 먼저 본다. 규격과
   어긋나는 편의 구현은 채택하지 않는다.
2. **규격에 부합하는 선에서 체계성·일관성을 중시한다.** 자원 모델·명령 체계·명명·계약을
   통일된 구조로 설계한다 (일회성 예외를 늘리지 않는다).
3. **기존 구현에 얹는 최소 보완(band-aid) 구조 제안은 최소화한다.** "기존 코드에 최소 변경"을
   이유로 규격·체계성을 희생하는 방향은 우선 제안하지 않는다 — 올바른 구조를 먼저 제시하고,
   최소 보완안은 명시적으로 요청받거나 정당한 근거(호환 전환기 등)가 있을 때만 대안으로 든다.

**코드**
- 주변 코드의 관습(명명·주석 밀도·구조)을 따른다. 독자적 스타일을 새로 도입하지 않는다.
- C/C++ 는 out-of-source `build/` 에서 빌드하고, 배포는 `make dist` 산출물(`build/dist/`)을 통한다.
  레포 소스 직접 실행과 배포본은 구분한다.
- 기능 작업 전 **관련 design/features 문서를 먼저 읽는다.** 문서와 코드가 어긋나면 코드를 정본으로
  보되, 그 차이를 문서 갱신으로 해소한다.

**기능 보완 = 코드 + 문서 동시 갱신**
- 동작·인터페이스·설정 키를 바꾸면 해당 docs 문서를 같은 변경에서 갱신한다.
- 새 기능은 design/features 에 정본 문서를 두고, 본 CLAUDE.md 의 참조 인덱스와 개요 표에 한 줄로 등록한다.
- 검증이 필요한 변경은 S1~S6 게이트로 확인한다 (최소 관련 stage).

**문서 현행화 = 최종 상태만 기술**
- 문서는 **현재 동작(최종 상태)** 만 서술한다. 변경 히스토리·진행 로그는 남기지 않는다.
- 제거 대상: `작성일`/`최종 수정`/`버전 N (날짜)` 헤더, 하단 "변경 이력" 표, `Phase/P1~PN ✅완료`
  진행 로그, `구(old) X → 새 Y` before/after 주석, 날짜·배포버전 결합 검증 이력, 사후 회고 노트.
- `구 X → 새 Y` 가 최종 동작 Y 를 서술하는 유일한 곳이면, 이력 부분만 떼고 **Y 를 현재 사실로
  재작성**해 정보 손실을 막는다.
- 보존 대상: 메시지 flow 다이어그램, 표, 설정/스키마, 규격 참조(TS/RFC), 교차링크, 미구현/향후 과제.
- 변경 이력은 git 으로 추적한다. 문서 안에 중복 기록하지 않는다.
