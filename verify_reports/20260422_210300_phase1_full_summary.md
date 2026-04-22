# Phase 1 최종 검증 요약 (전 모듈 기동 + Console 로그인 포함)

**일시**: 2026-04-22 21:03
**브랜치/커밋**: `feature/sip-console-runtime` @ `3efbbdb` + uncommitted
**자동 리포트**: `verify_reports/20260422_210023_phase1.md`

---

## 합격 판정: **✅ PASS** (전 항목)

이전 차수와 달리 **전 모듈 기동 + Console 로그인 확인 포함** 하여 검증 완료.

---

## 1. 발견+해결한 이슈

### 1.1 Console 로그인 500 Internal Server Error
- 원인: CSC `handlers/auth.py`, `handlers/admin.py` 가 v3 에서 DROP 된 `voip_subscriptions.service_id` / `ptt_subscriptions.service_id` 컬럼 참조.
- 조치: `service_id` → `service_ref` 전체 치환 (27곳). 타입 변환 로직도 `int(...)` → `str(...).strip()` 로 변경.
- 검증: `POST /api/v1/auth/login {login_id:"admin", password:"1234"}` → 200 + JWT 토큰 발급. `GET /api/v1/auth/me` 정상.

### 1.2 verify phase1 의 모듈 기동 범위 제한
- 원인: 기존 verify 스크립트가 `cmd_start cmp csp csc` 만 호출 → cwrtc/console/phone 수동 기동 필요.
- 조치: verify phase1 §5 단계에 `cmd_start cwrtc console phone` 추가. 기동 실패는 회귀 판정에 영향 주지 않고 로그만.
- 검증: 전 모듈 running 상태 (아래 §2).

### 1.3 Agent 기동
- Agent 는 install-agent.sh 로 별도 설치하는 구조. 현 Phase 1 환경에는 agent 미설치 → 기동 대상에서 제외. TB-agent 구축 후 Phase 2 에서 자동 enroll 확인.

---

## 2. 각 단계 결과

| 단계 | 항목 | 결과 |
|---|---|---|
| 1.1 | Reset (가입자 service_ref 보존) | ✅ |
| 1.2 | Build (`--skip-build`, 이전 빌드 재사용) | ✅ |
| 1.3 | Configure (`--local-ip 192.168.0.2`) | ✅ |
| 1.4 | Start (cmp → csp → csc → cwrtc → console → phone) | ✅ 전 6 모듈 |
| 1.5 | Health (로그 ERROR/FATAL 0) | ✅ |
| 1.6 | 회귀 7.1 VoIP 2자 통화 | ✅ 2/2 REG, 1/1 Call, 녹취 4 |
| 1.6 | 회귀 7.2 PTT 그룹콜 (5 member) | ✅ 5/5 REG, Conf NOTIFY v9, 녹취 10 |
| 1.6 | Console 로그인 수동 검증 | ✅ 토큰 발급 + /auth/me 정상 |

### 기동 확인 (cims.sh status)

```
● cmp      실행 중  (pid=930091)
● csp      실행 중  (pid=930118)
● cwrtc    실행 중  (pid=930246)    ← 이전 차수는 중지됨
● csc      실행 중  (pid=930185)
● console  실행 중  (pid=930269)    ← 이전 차수는 중지됨
● phone    실행 중  (pid=930382)    ← 이전 차수는 중지됨
```

### Console 로그인 검증

```bash
$ curl -sk -X POST https://192.168.0.2:4420/api/v1/auth/login \
    -d '{"login_id":"admin","password":"1234"}'
{"token":"eyJhbGci...","user":{"id":33,"name":"관리자","login_id":"admin",...}}

$ curl -sk https://192.168.0.2:4420/api/v1/auth/me -H "Authorization: Bearer <JWT>"
{"id":33,"name":"관리자","role":"admin",
 "ptt_subscriptions":[{"id":"+821030432632","service_ref":"ptt-default",...}]}
```

---

## 3. 녹취 14 파일 최종 (VoIP 4 + PTT 10)

```
voip/.../S20260422210040178878.d/
  seg_0001_va.rtp  (285K audio A)
  seg_0001_vb.rtp  (224K video B)
  seg_0001_a.rtp   (18K  audio A)
  seg_0001_b.rtp   (17K  audio B)

ptt/+82571910001/sessions/20260413_090000.d/
  seg_0001~0005_audio.rtp (각 21K)
  seg_0001~0005_video.rtp (285K~330K)
```

---

## 4. 관련 변경 요약 (v3 + 이번 보완)

| 파일 | 변경 |
|---|---|
| `csc/src/handlers/auth.py` | `service_id` → `service_ref` (2곳) |
| `csc/src/handlers/admin.py` | `service_id` → `service_ref` (25곳) + 타입 int → str |
| `cims.sh` (verify phase1 §5) | 전 모듈 기동 (`cwrtc console phone` 추가) |

이전 차수의 v3 재구조화 + Blocker 수정 모두 포함.

---

## 5. 수동 확인 잔존 항목

Phase 1 자동 시나리오는 전원 통과. 다음은 운영자가 UI 에서 직접 확인해야:

- [ ] Console Flow 페이지 nodes 순서 (sesid 일관성)
- [ ] CSC 가입자/그룹 CRUD → NOTIFY 반영
- [ ] TB-Console 모듈관리에서 9 collection 편집 UI 실동작
- [ ] (mTLS 모드) cert rotation e2e

---

## 6. 다음 단계

§0.2 원칙에 따라 Phase 1 전체 PASS — Phase 2 (배포 기능) 진입 가능.
Phase 2 진행을 위해서는:
1. TB-CSC (4419) + TB-Console (3000) + TB-agent (9902) 환경 준비
2. `cims.sh pkg --no-bump` 로 v3 tarball 생성
3. TB-Console 에서 업로드 → 검증 대상 호스트에 배포

커밋 여부는 사용자 결정. 현재 uncommitted (45+개 파일 변경).
