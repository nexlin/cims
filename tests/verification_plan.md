# CIMS 시스템 검증 계획서

**문서번호:** CIMS-VP-2026-001
**작성일:** 2026-04-03
**버전:** 1.0

---

## 1. 개요

### 1.1 목적
CIMS 시스템의 3개 핵심 모듈(CSC, CSP, CMP)에 대한 체계적 기능 검증을 통해
시스템의 안정성과 정합성을 확인한다.

### 1.2 검증 대상

| 모듈 | 역할 | 프로토콜 |
|------|------|----------|
| **CSC** | REST API 서버 (관리, 인증, MCPTT) | HTTP/HTTPS (4420, 4430) |
| **CSP** | SIP 시그널링 서버 (CSCF/TAS/PTT-AS/IBCF) | SIP/UDP (5060), UDP JSON (4421) |
| **CMP** | RTP 미디어 서버 (릴레이, 그룹 믹싱, 플로어) | UDP JSON (9000), RTP/RTCP |

### 1.3 검증 환경

- **서버:** 192.168.0.2 (Ubuntu Linux)
- **DB:** MariaDB (127.0.0.1:3306, cims)
- **빌드:** build/dist/ 최신 바이너리
- **검증 도구:** Python 자동화 스크립트 + cspsim

---

## 2. 검증 전략

### 2.1 검증 수준

```
Level 1: 단위 검증 (모듈별 인터페이스)
  ├─ CSC REST API 엔드포인트별 CRUD
  ├─ CSP CscInterface UDP 명령
  └─ CMP UDP 제어 명령

Level 2: 연동 검증 (모듈간 인터페이스)
  ├─ CSC → CSP 실시간 동기화 (user_change, group_change)
  ├─ CSP → CMP RTP 세션 제어
  └─ CSC Stats → CSP/CMP 상태 수집

Level 3: 단대단 검증 (시나리오 기반)
  ├─ VoIP 1:1 통화 전체 흐름
  ├─ PTT 그룹 통화 전체 흐름
  └─ 관리자 운용 시나리오
```

### 2.2 검증 방법

| 방법 | 도구 | 적용 |
|------|------|------|
| API 자동 검증 | Python requests | CSC REST API |
| UDP 프로토콜 검증 | Python socket | CSP/CMP 제어 인터페이스 |
| SIP 시나리오 검증 | cspsim | SIP 등록/통화/그룹콜 |
| DB 정합성 검증 | pymysql | 데이터 CRUD 후 DB 상태 확인 |

### 2.3 판정 기준

- **Pass:** 기대 결과와 실제 결과 일치
- **Fail:** 기대 결과와 불일치 또는 오류 발생
- **Skip:** 선행 조건 미충족으로 수행 불가

---

## 3. 검증 범위

### 3.1 CSC 검증 항목 (24항목)

| 분류 | 항목수 | 내용 |
|------|--------|------|
| 인증 API | 4 | 로그인, 회원가입, 세션조회, 비밀번호 변경 |
| 가입자 관리 | 5 | 사용자 CRUD, 목록조회 |
| VoIP 구독 관리 | 4 | 구독 추가/조회/수정/삭제 |
| PTT 구독 관리 | 4 | 구독 추가/조회/수정/삭제 |
| PTT 그룹 관리 | 4 | 그룹 CRUD |
| 통계/헬스 | 3 | 헬스체크, 가입자상태, 서비스통계 |

### 3.2 CSP 검증 항목 (10항목)

| 분류 | 항목수 | 내용 |
|------|--------|------|
| CscInterface | 3 | stats, user_change, group_change |
| SIP 등록 | 2 | 등록 성공, 인증 실패 |
| SIP 구독 | 2 | GMS/CMS SUBSCRIBE-NOTIFY |
| VoIP 호 | 2 | 발신-응답-종료, 부재 |
| PTT 그룹 호 | 1 | 그룹 발신-참여-플로어-종료 |

### 3.3 CMP 검증 항목 (10항목)

| 분류 | 항목수 | 내용 |
|------|--------|------|
| 제어 명령 | 4 | alive, add, remove, stats |
| 그룹 명령 | 4 | addgroup, removegroup, joingroup, leavegroup |
| RTP 포트 관리 | 1 | 할당/해제/풀 상태 |
| 플로어 제어 | 1 | MCPTT 플로어 상태 확인 |

### 3.4 연동/E2E 검증 항목 (6항목)

| 분류 | 항목수 | 내용 |
|------|--------|------|
| CSC→CSP 동기화 | 2 | 사용자/그룹 변경 통지 |
| 헬스체크 연동 | 1 | CSC→CSP+CMP stats 수집 |
| VoIP E2E | 1 | cspsim 2세션 통화 |
| PTT E2E | 1 | cspsim 4세션 그룹콜 |
| 관리 시나리오 | 1 | 사용자 생성→구독추가→그룹편성→통화→이력확인 |

---

## 4. 검증 일정

| 단계 | 내용 | 산출물 |
|------|------|--------|
| 1 | 검증 계획/항목 수립 | 본 문서, 검증 항목서 |
| 2 | 검증 시뮬레이터 구현 | Python 스크립트 |
| 3 | 검증 수행 | 자동화 실행 |
| 4 | 결과 리포트 | verification_report.md |

---

## 5. 산출물 목록

| 파일 | 내용 |
|------|------|
| `tests/verification_plan.md` | 검증 계획서 (본 문서) |
| `tests/test_spec.md` | 검증 항목서/절차서 |
| `tests/conftest.py` | 공통 설정/유틸리티 |
| `tests/test_csc.py` | CSC 모듈 검증 스크립트 |
| `tests/test_csp.py` | CSP 모듈 검증 스크립트 |
| `tests/test_cmp.py` | CMP 모듈 검증 스크립트 |
| `tests/test_e2e.py` | 연동/E2E 검증 스크립트 |
| `tests/run_all.py` | 전체 검증 실행 및 리포트 생성 |
| `tests/verification_report.md` | 검증 결과 리포트 |
