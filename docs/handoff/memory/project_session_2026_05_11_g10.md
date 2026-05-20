---
name: 2026-05-11 G10 세션 (외부 peer inbound auth-skip + IBCF trunk LIVE PASS)
description: G10 구현 (PendingRouteMap.Has → auth skip) + psip ParseUser '@' first-split 발견 + rule field 수정으로 S6-SCN-IBCF-TRUNK SKIP → LIVE PASS 전환
type: project
originSessionId: b25db11b-005b-4c51-b4f6-4ca2e1ded329
---
## 세션 결과

LIVE pipeline-full **35/34 PASS / 0 FAIL / 1 SKIP** in 260s (직전 baseline 35/33 + 2 SKIP, 370s).

S6-SCN-IBCF-TRUNK SKIP → LIVE PASS.
남은 SKIP: S6-SCN-CERT-ROTATE (mTLS, 환경 의존).

## 핵심 변경

### G10 — 외부 peer inbound auth-skip (CSP 본체)
- `csp/CspPendingRouteMap.{h,cpp}` — peek-only `Has(callId)` 메서드 추가
- `csp/ModuleDispatcher.cpp` L383~ — `EventIncomingRequestAuth` 진입부에 early-return:
  ```cpp
  std::string strCallId; pclsMessage->GetCallId( strCallId );
  if ( gclsPendingRouteMap.Has( strCallId ) ) return true;
  ```
- 의미: RecvRequest 의 AclPolicy + routing_policies 매칭으로 외부 peer trust 가 이미 확립된 콜은 From-user 가 user map 에 없어도 401 challenge 우회.
- 옵션 A (정석) 채택. RecvRequest 의 AclPolicy/routing 평가 통과가 trust 확립 → auth 우회의 흐름이 코드 구조로 명시됨.

### psip URI 파싱 발견 — `req_uri_user` → `req_uri_host` rule 수정
- `ext/psip/SipParser/SipUri.cpp` `ParseUser` (L300) 는 **첫 `@`** 로 분리.
- cspsim 이 보내는 `INVITE sip:9000@trunk.peer.test@127.0.0.5 SIP/2.0` 가:
  - user = "9000"
  - host = "trunk.peer.test@127.0.0.5" (`@` 가 host 에 포함)
- 직전 세션의 rule (`req_uri_user contains "trunk.peer.test"`) 은 절대 매칭 안 됨 (user 는 "9000").
- 수정: `verify/lib/common/ibcf_routing.py` — field 를 `req_uri_host` 로 전환. contains 매칭 성공.

### scn_ibcf_trunk.py swap (SKIP → LIVE)
- SKIP body 제거, `_scn_ibcf_trunk_full` 본체로 swap. 로그 라벨 정합.

## 부수 픽스 — 인프라 3건 (commit `bdca6b5`)

### A. `cmd_pkg` tar exclude 에 `csc/packages_tb` 추가
- TB-CSC upload 분이 `dist/csc/packages_tb/` 에 누적 (이번 발견 시점 3.9GB).
- `cims.sh pkg` 의 exclude 는 `csc/packages` 만 제외, `packages_tb` 는 포함 → csc-0.0.1.tar.gz 가 2.6GB (정상은 ~5MB).
- 영향: S5-CSC-DEPLOY-INSTALL 의 60s poll timeout → 전체 pipeline FAIL.
- 픽스 후 csc tarball **177KB** 로 정상화.
- `cache_tb`, `packages_trash` 도 함께 제외.

### B. `cmd_pkg` 끝에 stale 버전 cleanup
- `cims.sh pkg --no-bump` 은 현 pkg.json 버전만 생성하지만 dist/packages/ 의 옛 버전 tarball 이 잔재 (이번 발견 시점 2026-05-08 시점 0.0.2 12개).
- `_latest_tarball()` 의 natural-sort 가 stale 0.0.2 를 선택 → deploy 가 OLD binary 사용.
- 픽스: targets 의 component 만 mtime 기준 최신 1개만 보존, 나머지 제거.

### C. `_stop_one all` 에 P1/P2 배포본 인스턴스 추가
- dev COMPONENTS (cmp/csp/cwrtc/csc/console/phone) 외에 variant 들은 별도 dist/<agent>/ 하위에서 동작 → stop all 이 누락.
- 픽스: 6개 agent (volte-sip/media-server + ptt-sip/media-server + ibcf-sip/media-server) install_path 하위의 pid 를 enumerate, cims_agent.py 제외 후 SIGTERM → 3s wait → SIGKILL.
- 발견: 변종 csp/cmp 가 SIGTERM 무응답 (dev csp 의 stop_one 도 SIGKILL 으로 폴백). escalation 필수.

## 검증 흐름 (g10 동작 trace)

ISP log (`build/dist/ibcf-sip-server/isp/log/csp_20260511_1.log`):
1. `[19:00:09] SIGUSR1 reload`
2. `RoutingPolicyEngine: sync complete, 1 policies` (rp-ibcf-out)
3. 회차 후 caller INVITE 도착 →
   ```
   RoutingPolicyEngine: policy='rp-ibcf-out' route_set='rs-mock-peer'
     picked_route='r-mock-peer' → RemoteNode rn-mock-peer
     (127.0.0.1:6800 UDP) [pending callId=...]
   ```
4. `EventIncomingRequestAuth` → my G10 early-return (Has=true)
5. `EventIncomingCall` → `Take(callId)` → B-leg INVITE to mock peer
6. caller 측 `CALL STARTED (1003ms)` → peer 측 `Call OK/End 0/1`

## 환경 변동성 안정화 — 검증

세션 후반 사용자 요청으로 백로그 "환경 변동성 안정화" 진행.

**검증**: pipeline-full 3 연속 회차 PASS (prep-reset 무관, 평균 263s):
- 19:20 (post infra fix) → 35/34 PASS / 1 SKIP, 266.8s
- 19:33 (manual stop test 후) → 35/34 PASS / 1 SKIP, 263.9s
- 19:38 (services running 상태에서) → 35/34 PASS / 1 SKIP, 257.9s

이전 변동성 ("LIVE 회차마다 fail 다름") 의 근본 원인은 인프라 픽스 3건이 모두 해결:
- csc tarball 폭증 (60s install timeout 의 원인) — `bdca6b5` 픽스 A
- stale tarball deploy (옛 binary 가 deploy 되어 비결정적 결과) — 픽스 B
- 좀비 변종 process (다음 회차 시작 시 port 충돌) — 픽스 C

추가 방어 (`step_21` wildcard kill / `step_22` 강제 stop) 는 기존 `kill_stray` + `.prev` rename 과 중복이라 생략. 정책: 관찰된 문제가 없으면 추가 안 함.

## 다음 진입 권장

1. **B1 상용 환경 검증** — 운영 배포본 셋업 시점
2. **WebRTC / cwrtc** — 사용자 별도 요청 예정
3. **S6-SCN-CERT-ROTATE** — mTLS 환경 의존
