# MC 규격 갭 보완 계획 — .48 · .45 · Windows 분담, 세션 단위 진행

> 대상은 규격 갭 목록 세 벌 — [mcptt_conformance_gaps.md](mcptt_conformance_gaps.md)(147) · [mcdata_conformance_gaps.md](mcdata_conformance_gaps.md)(43) ·
> [mcvideo_conformance_gaps.md](mcvideo_conformance_gaps.md)(64), 합계 254 항목(급 A 46 · B 84 · C 85 · D 39)이다. 목록은 전부 **코드 읽기** 결과라
> 항목마다 규격 원문과 지금 코드를 다시 확인한 뒤 고친다. 일을 **WP(작업 묶음) 42개**(+ 준비·문서 X00·X01)로 나눴고, **세션 하나가 WP 하나**를 맡는다. 호스트는
> **.48(호 제어·미디어 서버) · .45(설정 평면 서버 + 단말 + 라이브 반영) · Windows(관제 앱)** 셋이다. 세션 수는 추정이다.

## 1. 원칙

- **규격 우선** — CLAUDE.md «설계 우선순위» 그대로. 규격이 정한 절차·응답 코드·요소를 따르고, 남기는 편차는 정본 문서의 편차 표에 사유와 함께 적는다.
- **재확인 먼저** — 목록의 판정·줄 번호를 사실로 받지 않는다. 원문 절과 지금 코드를 다시 읽어 **확정 · 오판 · 이미 반영 · 편차로 남김** 넷 중 하나로 판정한 뒤
  손댄다(§4 2단계). △ 항목은 .48 에서 재현해 확정한다.
- **규격 형식만, 전환기 없음**(K4) — 개발 단계라 이미 나간 단말(협력업체 APK 포함)·계측기·cspsim 의 옛 형식은 판단 기준에 넣지 않는다.
  서버는 규격 형식만 받고 규격이 정한 거절을 바로 넣는다 — 이중 수용·호환 스위치·`log` 단계를 새로 두지 않고, 이미 둔 것은 그 WP 재확인 때 규격 쪽으로 걷는다.
  우리 SDK·앱은 같은 묶음 안에서 짝으로 맞춘다(배포 순서만 맞춘다). 계측기·cspsim 은 팀원 트랙이라 요구로 넘긴다.
- **세션 = WP 하나** — WP 는 한 세션에 재확인·구현·시험·문서까지 끝나는 크기로 잡았다(«세션» 칸이 2·3 이면 그만큼 나눠 이어 간다).
  하다 보니 넘치면 남은 항목을 새 WP 로 떼어 §5 에 적고 끝낸다.
- **끝 = 목록에서 지움** — 고쳤거나(확정) 틀린 판정이었으면(오판·이미 반영) 갭 목록에서 그 행을 지운다. 편차로 남기면 정본 문서 편차 표로 옮기고 지운다.

## 2. 분담

| 호스트 | 맡는 것 | 소유 경로(이 트랙의 주 편집자) | 시험·배포 |
|---|---|---|---|
| **.48** (media01, 테스트베드) | 호 제어·미디어 서버 — S01~S25 + 준비 X00 | `csp/` · `cmp/` · `cmdp/` · `ext/psip/` · `docs/design/modules/{csp,cmp}.md` · `docs/api/cmp_media_api.md` · `docs/design/features/mcptt_floor_defs.yaml`(생성물 둘 다) · `tests/cmp_*`·`tests/fixtures/mcptt/sip/`(새 골든) | .48 배포본(마음대로 올리고 내린다) |
| **.45** (라이브 · 단말 빌드 환경) | 설정 평면 서버 C01~C07 + 단말 U01~U09 + 문서 X01 + **.45 라이브 반영** | `csc/` · `sql/`(추가만) · `ems/*/console`(그룹 편집·가입자 칸) · `sdk/` · `android/ptt-client/` · `android/volte-client/` · `sdk/core/cli/` · `ext/pjproject/` · `docs/design/modules/csc.md` | 개발 시험은 .48 에 원격 배포(`scripts/oam-deploy.py`, `OAM_URL=https://121.161.164.48:4419`) · 단말은 .48 대상 `cimsue-cli` · .45 라이브는 §8 |
| **Windows** | 관제 앱 두 벌의 짝 — W01 | `windows/dispatch-desktop/` · `android/dispatch-tablet/` · `sdk/windows`(빌드) | 관제석 실기 |

- CSC 를 .45 에 둔 까닭 — 설정 문서를 만드는 쪽(CSC)과 읽는 쪽(SDK)이 한 호스트라 문서 기본값·XCAP 경로 전환(C01·C03·C04 ↔ U07)을 한 세션 안에서 맞출 수 있고,
  Python(CSC) 과 C++(CSP·CMP) 으로 경로가 겹치지 않으며, 일이 두 호스트에 고르게 나뉜다(.48 ≈ 35 세션 · .45 ≈ 20 세션).
- 한 호스트가 비면 상대 WP 를 넘겨받는다 — dev_share 로 묻고 답을 받은 뒤, 그 WP 동안만 상대 소유 경로를 편집한다(웨이브 5 는 .45 가 S22·S24 를 받을 수 있다).
- 계측기(`ems/tester`·`tester/worker`·`cspsim`·libcsim)는 팀원 트랙이다 — 바꿀 일이 생기면 요구만 넘긴다(S18 선행).

## 3. 결정

**정해진 것**

- **K1 분담** — §2 대로(CSC = .45).
- **K2 커밋·푸시** — WP 마다 S1 이 통과하면 `main` 에 커밋·푸시하고 dev_share 로 알린다(이 트랙의 허가).
- **K3 .45 라이브 반영** — 트랙 중에는 하지 않는다. 끝에 한 번 따로(§8). 그때까지 개발·실측은 전부 .48.
- **K4 개발 단계 — 규격 준수가 최우선** — 이미 나간 단말·계측기의 형편은 고려하지 않는다(사용자 지시). §1 «규격 형식만, 전환기 없음» 이 이 결정의 적용이고,
  아래 D 가운데 옛 단말을 위해 둔 단계(D4 `log`·D5·D9·D13)는 K4 에 맞춰 고쳐 적었다.

**WP 별 결정** — 걸리는 WP 의 «결정» 칸이 이 번호다.

| # | 정할 것 | 정한 것 | 걸리는 WP |
|---|---|---|---|
| D1 | 그룹 호 모델 | **규격대로** — chat 은 서버 초대 없이 각자 합류·참가자 1명 이하면 해제(TS 23.379 §10.6.2.3.1.2.1 · TS 24.379 §6.3.8.1), 편성 그룹은 합류 때 재초대를 멈추고 새로 제휴·복귀한 단말을 서버가 초대(late call entry, §10.1.1.4.6), 제휴 해제 = 그 호에서 BYE(§10.1.1.4.4.3), 제휴한 멤버에게만 초대·배포(§6.3.5.5 · TS 24.282 §6.3.4 — `require_affiliation` 스위치는 없앤다). 현장 앱의 conference NOTIFY 자체 합류는 S14 의 짝으로 정리한다 | S14 |
| D2 | 동시 그룹 호 상한 N6 | **규격대로** — N6 는 사용자마다의 MCPTT user profile 값이다(TS 24.484 §8.3.2.1 `<MCPTT-group-call>` `<MaxSimultaneousCallsN6>`). 서버는 그 사용자의 N6 를 넘는 개시·합류에 486 + `103`(TS 24.379 §10.1.1.3.1.1 5) 등), 단말·앱은 문서의 N6 를 읽어 그 안에서 동작하고 103 을 사용자에게 알린다. **기본값 = 관제 10 · 그 밖의 단말 5**(설정 키 둘, 사이트에서 바꿀 수 있다). «관제» = 관제 역할이 배정된 사용자([mcptt_authorization.md](../design/features/mcptt_authorization.md) 역할) — CSC(문서)와 CSP(집행)가 같은 판정을 쓰도록 판정 자리를 한 곳으로 정한다(C01·S01 재확인) | S01 · C01 |
| D3 | 전이중 개별 호 SDP | TS 24.379 §11.1.2.2(`m=application` 없음)가 정본 — `mc_no_floor_ctrl` 등 옛 형식은 받지 않는다(D10) | S17 |
| D4 | IdMS 클라이언트 등록·서명 | IDM-3·IDM-4 = 클라이언트 등록 저장소 + `enforce·log·off` 3단(`IdMs.ScopeEnforcement` 와 같은 방식), **`enforce`**(K4 — 등록 목록 = 우리 앱·도구의 client_id·redirect_uri, `log` 는 진단용으로만 남긴다) · IDM-6 = RS256 서명을 더하고 검증 쪽은 전환기 동안 HS256 도 받는다(끝 = 모든 소비자가 RS256 검증) | C06 |
| D5 | 엄격 검사 시점 | **바로 적용**(K4) — 스위치·`log` 단계 없이 규격대로 거절한다. 다른 WP 가 «enforce → S18» 로 미룬 것(REG-3 · SDS-4)도 S18 에서 켠다. 우리 SDK 짝(U04·U05)이 규격형이 된 뒤면 된다 | S18 |
| D6 | 서비스 설정(poc-settings) | **이번 트랙에 넣는다** — 웨이브 5 | S25 · U09 |
| D7 | MCVideo 선호 코덱 | **(a)** — CSC 관리 API·XCAP PUT·콘솔이 서버가 집행하는 코덱(음성 AMR-WB · 영상 H.264)만 그룹 선호로 받는다(VSDP-1 → C02). 단말 offer 는 그룹 선호를 따른다(VGU-4) | C02 · U09 |
| D8 | T2 제외 대상 | 코드대로 긴급만 — 문서 셋(conformance F4 · cmp_media_api §7.7 · mcptt_timers §5.2)을 고친다 | S20 |
| D9 | 서비스 인가 바인딩 없는 요청 404 + 141 | **바로 적용**(K4) — S08 이 만든 판정을 S18 에서 켠다(계측기·cspsim 의 등록 없는 송신은 그쪽이 따라온다) | S08 · S18 |
| D10 | 규격형 요청·서비스 설정의 전환기 | **두지 않는다** — S17·S25 는 규격 형식만 받도록 바로 적용한다(옛 단말 요청을 함께 받는 경로를 만들지 않는다). 짝인 SDK 몫(U04·U05·U09)과 배포 순서를 맞춘다 — 서버를 먼저 올리면 옛 SDK 의 해당 요청이 거절된다 | S17 · S25 |
| D11 | 공유 DB 제휴 키 | **바꿔도 된다** — 제휴를 클라이언트 단위 키로(마이그레이션은 공유 DB 에 적용, .45·.48 공용이라 적용 시점을 dev_share 로 알린다) | S12 · S13 |
| D12 | 서버 산출 형식 변경(SDK 가 읽는 것) | **.45 와 협의 — 합의됨**(dev_share `20261003-1025_45_reply-S15-S16-sdk-formats.md`): SDK 는 규격 형식을 이미 읽는다, 서버가 먼저 바꿔도 된다. 숫자 그룹 ID `tel:+<id>`(GCS-17·SDS-8)는 SDK 가 SDS-8 때 같이 맞추고, FD-4 Mandatory download IE 의 해석·따름은 .45 U05 | S15 · S16 |
| D13 | 세션 밖 채널 접속자 표시 | **S27 은 S13·U06 을 기다리지 않는다**(K4) — GCS-14 를 바로 규격대로(진행 중 세션이 아닌 conference 구독 = 404 + 137, 비참가자 = 403 + 138, TS 24.379 §10.1.3.3·§10.1.3.4.1). 참여하지 않은 채널의 접속자 표시는 규격 자리인 제휴 상태 구독(§9.2.1.3 — AFF-12·VAFF-8, U06)이 들어올 때까지 빠진다 | S27 |

## 4. 세션 절차

세션 하나 = WP 하나. 시작 문구 예 — «docs/dev/conformance_gap_plan.md §4 절차로 WP S03 진행해줘».

1. **시작**
   - 작업 트리는 다른 세션의 미커밋과 섞이지 않는 worktree 다 — .45 `/home/cims/work/.wt-1002`(빌드 산출물 있음) · .48 `/home/cims/work/.wt-gap`
     (X00 에서 `git worktree add`, 첫 빌드는 외부 의존성 때문에 길다). `git fetch origin && git merge --ff-only origin/main`.
   - 이 문서 §5 상태 칸, `/mnt/cims/dev_share/` 의 새 메시지, WP 의 선행·결정을 확인한다. 선행이 «완료» 가 아니거나 결정이 비면 다른 WP 를 고른다.
   - dev_share 에 `YYYYMMDD-HHMM_<45|48|win>_claim-<WP>.md` 를 쓴다 — 진행 중 표시는 이 메시지가 정본이다(두 세션이 같은 WP 를 잡지 않게, §5 는 끝에 고친다).
2. **재확인** — 항목마다:
   - 규격 원문 절을 원문 텍스트에서 다시 읽는다(`/mnt/cims/dev_share/spec/`, 판은 README). 목록이 인용한 판과 다르면 최신 판 기준으로 다시 본다.
   - 지금 코드를 다시 찾는다. 목록의 줄 번호는 대조 커밋(MCPTT `4bd4c086` · MCData·MCVideo `74f5c195`) 기준이라 움직였을 수 있다 — 함수 이름으로 찾는다.
   - 판정: **확정**(고친다) · **오판**(규격 또는 코드를 잘못 읽음) · **이미 반영**(다른 변경이 고침) · **편차로 남김**(사유가 있다). △ 는 .48 에서 재현해 정한다.
   - 결정이 필요한 것이 나오면 그 항목만 멈추고 사용자에게 묻는다(§3 에 D 행 추가). 나머지는 이어 간다.
3. **설계** — 규격 절차·응답 코드를 그대로 쓴다. 전환기는 두지 않는다(K4) — 짝(SDK·앱)은 같은 묶음에서 맞추고 배포 순서만 맞춘다.
   한 자리를 고치면 같이 풀리는 항목(MCData·MCVideo 목록이 «MCPTT ○○ 와 같은 뿌리» 로 적은 것)은 함께 본다. 코드·시험 주석에 근거 절 번호를 남긴다.
4. **구현·시험** — 관련 S1(`./cims-verify run --items S1-UNIT-CSP,S1-UNIT-CMP,S1-UNIT-CSC,S1-UNIT-PSIP,S1-CPP-FORMAT,S1-PY-SYNTAX` 중 해당분 ·
   SDK `cimsue_test` · Android 단위). 고친 절차마다 단위시험을 더한다(이름·주석에 절 번호).
5. **실측** — 서버 WP 는 .48 에 배포하고(배포 id = oam 1 · oam-svc 2 · csc 3 · cmp 4 · cmdp 5 · csp 6 — `OAM_URL=https://121.161.164.48:4419`
   `OAM_LOGIN=admin` `OAM_PASSWORD=1234`, 시험 서버라 기록 허가) `cimsue-cli` 두 대·계측기 동봉 시나리오·`tests/cmp_smoke_*.py`
   로 그 절차를 확인한다. 배포 전후로 dev_share 에 알린다(§7). 단말 WP 는 .48 을 겨눈 `cimsue-cli`(사내 단말은 라이브 반영 때 — §8).
6. **문서** — 정본 문서(mcptt_standard_conformance.md · mcdata_messaging.md · mcvideo.md · 모듈 문서·API 문서)를 고치고, 갭 목록에서 행을 지우고 §1 요약 수를 맞춘다.
   목록 §5 «문서 정정» 에 그 항목이 있으면 그 줄도 처리한다.
7. **커밋·푸시**(K2) — S1 이 통과하면 바로. 자기 경로만 `git add`(`commit -a` 금지), `git push origin HEAD:main`. 메시지에 WP 와 오판·이미 반영 항목의 사유 한 줄씩.
8. **인계·끝** — 짝 몫이 생기면 dev_share `…_done-<WP>.md`(상대가 할 일 · 커밋 · 시험 방법). §5 상태를 «완료 <커밋>» 으로.
   세션 메모리에 다음 후보 WP 한 줄.

## 5. WP 목록

«항목» 은 MCPTT 번호가 앞, `MCData` 접두가 MCData, `V…`·`TCS`·`TCU`·`RCS`·`RCU` 가 MCVideo 다. «급» 은 그 WP 의 급별 항목 수, «세션» 은 추정.
«상태» = 대기 · 완료 <커밋> · 보류(<이유>) — 진행 중인지는 dev_share 의 claim 메시지로 본다.

### 5.1 .48 — 호 제어·미디어 서버 (27 WP · 157 항목 · ≈ 36 세션)

| WP | 이름 | 항목 | 급 | 세션 | 선행 | 결정 | 짝 | 상태 |
|---|---|---|---|---|---|---|---|---|
| **S01** | on-network-disabled·정원·N6 집행 | GCS-19 · GCS-5 · GCS-6 / VGC-3 | A3 · B1 | 1 | — | D2 | C01(`<on-network-disabled>`·정원·사용자별 N6 산출) · U03(SDK·앱이 N6 를 따르고 103 을 알림) · W01 | 완료 3309f08f |
| **S02** | MCVideo 헤더·fmtp·Supported 한두 줄 | AFF-3 · GCS-15 / VGC-4 · VGC-5 · VGC-6 · VGC-7 · VSDP-4 · VAFF-3 | C8 | 1 | — | — | — | 완료 4cb4a0bc |
| **S03** | 경보·긴급 인가 | EMG-1 · EMG-2 · EMG-5 / MCData EMG-1 / VGC-1 | A2 · B2 · C1 | 1 | — | — | U01(단말 경보 대상) | 완료 f2cc799c |
| **S04** | 긴급 지시자·상태 통지 | EMG-10 · EMG-11 · EMG-13 · EMG-14 · EMG-15 · EMG-16 · EMG-17 | B1 · C6 | 1 | S03 | — | — | 완료 494fda56 (EMG-17 남김 — CSP↔CMP 암묵 발언 요청 계약) |
| **S05** | MCData 인가·배포 대상 | MCData FD-1 · AFF-3 · AFF-4 · AFF-5 · DISP-2 | A1 · C3 · D1 | 1 | — | — | C07(FD-6 GET 수신 제어) | 완료 29bd2806 (FD-1 남은 몫 → S26) |
| **S06** | SDP 협상값 집행(우선순위·큐잉 상한) | SDP-3 · PRV-9 · FCS-22 / TCS-4 · TCS-6 · RCS-3 | A1 · B1 · C2 · D2 | 1 | — | — | U02(SDK answer 값) | 완료 117d4a86 |
| **S07** | MCVideo NAT 합류 알림(실측) | RCS-1 | A1 | 1 | — | — | — (서버만으로 닫힘 — CMP 가 latch 뒤 다시 알린다) | 완료 976d6d97 |
| **S08** | Warning·응답 코드 — MCPTT | GCS-7 · GCS-8 · GCS-11 · REG-3 · ADH-4 · PRV-6 · PRV-7 · GCS-20 | B5 · C2 · D1 | 1 | — | D9 | U03 · W01(문구 사전) | 완료 a2837b34 (REG-3 enforce → S18 · PRV-7 본문 전달 남김) |
| **S09** | Warning·응답 코드 — MCData·MCVideo | MCData WRN-1 · WRN-2 · WRN-3 · SDS-4 / VPRV-1 | C4 · D1 | 1 | — | — | W01(문구 사전) | 완료 bdf3d172 (SDS-4 enforce → S18) |
| **S10** | 개별·애드혹 인가 판정(CSP 몫) | PRV-2 · PRV-4 · PRV-5 · PRV-8 · EMG-4 · ADH-5 | A3 · B3 | 1 | C03 | — | W01(Capabilities 게이트) | 완료 95d7debb (PRV-4 단말 몫 → SDK 묶음 7 · 공유 DB 개별 호 열 마이그레이션 대기) |
| **S11** | 애드혹 호 절차·인원 상한 | ADH-2 · ADH-3 · ADH-6 · ADH-7 · ADH-9 · ADH-10 | A4 · C1 · D1 | 1 | C03 | — | U04(SDK BYE Reason·`adhoc`) | 완료 096cd123 (ADH-7 SDK 몫 → U04) |
| **S12** | 제휴 — 클라이언트 단위 키·해제·판정 | AFF-2 · AFF-4 · AFF-5 · AFF-6 / MCData AFF-6 / VAFF-1 · VAFF-2 · VAFF-4 · VAFF-5 · VGC-11 | A4 · B2 · C4 | 2 | — | D11 | U04(`<mcptt-client-id>`) · U06 | 대기 |
| **S13** | 제휴 — 통지·정리·서비스 분리 | AFF-7 · AFF-8 · AFF-9 · AFF-10 · EMG-3 / MCData AFF-1 · REG-2 / VAFF-6 · VAFF-7 · VREG-4 | A3 · B3 · C3 · D1 | 2 | S12 | D11 | U06 | 대기 |
| **S14** | 그룹 호 모델(chat·재초대·late call entry) | GCS-1 · GCS-2 · GCS-3 · GCS-4 · GCS-21 · GCS-22 · AFF-11 / MCData AFF-2 / VGC-2 · VGC-12 | A7 · B1 · C2 | 3 | S12 | D1 | .45 현장 앱(conference NOTIFY 자체 합류) · W01 | 대기 |
| **S15** | MCPTT 서버 산출 정합(NOTIFY·ID·SDP) | GCS-12 · GCS-13 · GCS-16 · GCS-17 · GCS-18 | A2 · B2 · C1 | 1 | — | D12 | — | 대기 |
| **S16** | MCData 서버 산출 정합 | MCData SDS-2 · SDS-8 · MSRP-2 · MSRP-6 · FD-4 | B3 · C2 | 1 | — | D12 | U05(Mandatory download 따름) | 대기 |
| **S17** | 규격형 요청 수용 | PRV-1 · PRV-3 · GCS-14 / MCData SDS-1 · SDS-5 · CFG-1 | A1 · B3 · C1 · D1 | 2 | — | D3 · D10 | U04 · U05 · C01(MCData PSI 광고) | 완료 036c4594 (GCS-14 → S27 · csp 0.2.217 .48 배포 · SDK 짝 U04·U05 묶음 C 4a5b64e4 실측 통과) |
| **S18** | 엄격 검사 켜기 | GCS-9 · GCS-10 · EMG-12 · REG-3 / MCData MSRP-5 · SDS-4 | B2 · C3 · D1 | 1 | U04·U05 | D5 · D9 | — | 대기 |
| **S19** | xcap-diff 구독·통지 | GMS-14 · GMS-15 · CMS-5 / VCMS-1 | B4 | 1 | C04 | — | U07 | 일부 b83c94e1 · a80dd27b · 84f9583b (VCMS-1 · CMS-5 그룹 몫 · GMS-14 본문 몫 · GMS-15 삭제 몫 끝 — GMS-14 토큰 신원·GMS-15 ETag·직렬화·CMS-5 CSC 몫 남음) |
| **S20** | 발언권 메시지 필드·타이머 | FCS-4 · FCS-5 · FCS-7 · FCS-8 · FCS-17 · FCS-18 · FCS-19 · FCS-20 · FCS-21 · FCS-23 · FCS-24 | A2 · B4 · C2 · D3 | 2 | — | D8 | U08(SDK Ack) | 대기 |
| **S21** | 발언권 확장 형식·수신 전용 멤버 | FCS-6 · FCS-10 · FCS-11 · FCS-12 · FCS-13 · FCS-14 · FCS-15 · FCS-16 · SDP-2 / TCS-10 | B6 · C3 · D1 | 2 | S20 | — | U08(SDK 코덱 생성물) · C05(그룹 문서 요소) | 대기 |
| **S22** | MCVideo 송출·수신 제어 서버 세부 | TCS-1 · TCS-2 · TCS-3 · TCS-5 · TCS-7 · TCS-8 · TCS-9 · TCS-11 · RCS-2 | C4 · D5 | 2 | — | — | U08(TCU-1) | 일부 1354591e · 7d77120f · 9e395281 (TCS-1·3·5·7·9·11·RCS-2 끝 — TCS-2·TCS-8 남음) |
| **S23** | MCVideo 호 세부(초대 offer·T5·conference·PSI) | VGC-8 · VGC-9 · VGC-10 · VGC-13 | C3 · D1 | 1 | — | — | — | 일부 02e3e4de · CSC 짝 80cc816b (VGC-9·VGC-13 끝 — VGC-8·VGC-10 남음) |
| **S24** | MCData 미디어 평면 수명·색인·재전달 | MCData MSRP-3 · SDS-9 · DISP-1 | C3 | 1 | — | — | U05(MSRP-4) | 대기 |
| **S25** | 서비스 설정·인가(poc-settings) | REG-2 · REG-4 / MCData REG-3 · REG-4 / VREG-2 | B3 · C1 · D1 | 2 | — | D6 · D10 | U09(VREG-1) · U03(Answer-Mode) | 대기 |
| **S26** | MCData FD 파일 존재 확인(§6.7.3 HEAD) | MCData FD-1(S05 의 남은 몫) | C1 | 1 | C07(콘텐츠 서버 HEAD §6.7.3.2 · FD URL 을 PublicUrl base 로) | — | — | 완료 25870583 |
| **S27** | conference 구독 = 진행 중 세션(S17 에서 뗌) | GCS-14 | B1 | 1 | — | D13 | U04(GCC-7 — Request-URI 세션 식별자·Expires 2^32-1·mcptt-info) · 계측기(그룹 AoR conference 구독 시나리오) · W01 | 대기 |

### 5.2 .45 — 설정 평면 서버 CSC (7 WP · 52 항목 · ≈ 9 세션)

| WP | 이름 | 항목 | 급 | 세션 | 선행 | 결정 | 짝 | 상태 |
|---|---|---|---|---|---|---|---|---|
| **C01** | MCPTT 설정·그룹 문서 값 | CMS-4 · GMS-7 · GMS-13 · GMS-11 · CMS-7 · CMS-6 · CMS-8 · GMS-12 · GMS-18 · GCC-10 | A4 · B3 · C3 | 1 | — | D2 | S01 · W01(GMS-11·GMS-18 폼) | 완료 647584ae |
| **C02** | MCData·MCVideo 문서 값·선호 코덱 검증 | MCData GRP-1 · GRP-2 · GRP-3 / VCMS-3 · VCMS-5 · VGMS-3 · VSDP-1 | B3 · C1 · D3 | 1 | — | D7 | U09(VGU-4 — 단말 offer 가 그룹 선호를 따름) | 완료 e3ea9f97 |
| **C03** | user profile·service config 인가 요소(+SDK «없음 = false») | CMS-3 · ADH-1 / VCMS-2 | A1 · B1 · C1 | 1 | — | — | S10 · S11 · W01 | 완료 d9a762a1 |
| **C04** | XCAP 주소·문서 이름(양쪽 수용) | GMS-1 · GMS-6 · CMS-1 · CMS-2 · CMS-9 · CMS-11 · CMS-12 / VCMS-4 · VCMS-6 | B5 · C2 · D2 | 1 | — | — | S19(NOTIFY `sel`) · U07 | 완료 b8d64562 (GMS-1·GMS-6 의 SDK 몫 → U07) |
| **C05** | XCAP 쓰기 의미·오류 형식 | GMS-2 · GMS-3 · GMS-4 · GMS-5 · GMS-8 · GMS-9 · GMS-10 / VGMS-1 · VGMS-2 | B7 · C1 · D1 | 2 | C04 | — | W01(그룹 편집 PUT 본문) | 진행 1f58b045(GMS-2·GMS-4·GMS-9·GMS-10 읽기·VGMS-2) · 298af175(GMS-5 요소 단위 XCAP) · 852fb430(GMS-10 — 자체 요소를 `cims:` 이름공간으로, CSC GET·SDK 쓰기) — 남은 것 GMS-3·GMS-8·VGMS-1(PUT = 교체·«없음» 의 뜻 — 관제 앱이 문서를 보존해 PUT 하게 된 뒤, `20261003-0453_45_note-C05-transition.md`) |
| **C06** | IdMS·토큰 | IDM-1 · IDM-2 · IDM-3 · IDM-4 · IDM-6 · IDM-7 · IDM-8 · IDM-9 · CMS-10 | A3 · B1 · C4 · D1 | 2 | — | D4 | U02(IDM-5) · 앱 로그인(client_id 등록 목록) | 완료 ef37031a · 88f0f149 |
| **C07** | MCData 콘텐츠 서버 | MCData FD-2 · FD-3 · FD-5 · FD-6 | B2 · C2 | 1 | — | — | U05(`uploadFd` 규격형·Location) · S26(HEAD) | 완료 082eaef9 (FD-7 → U05) |

### 5.3 .45 — 단말 SDK·현장 앱 (9 WP · 44 항목 · ≈ 10 세션)

| WP | 이름 | 항목 | 급 | 세션 | 선행 | 결정 | 짝 | 상태 |
|---|---|---|---|---|---|---|---|---|
| **U01** | 긴급·경보 단말 | EMG-6 · EMG-7 · EMG-8 · EMG-9 | A3 · C1 | 1 | — | — | W01(EMG-8) | 완료 df892fa2 |
| **U02** | SDK 보안·협상 상한 | IDM-5 / VSDP-2 · TCU-2 | A1 · C1 · D1 | 1 | — | — | — | 완료 43421903 |
| **U03** | 거절 응답·Answer-Mode 해석 | GCC-6 / VGU-1 · VGU-2 · VGU-3 | B1 · C1 · D2 | 1 | — | — | S01(N6·103 의 단말 몫) · W01(GCC-6·VGU-3) | 완료 b5149796 · 45acbc7f(Warning 문구 번호 해석 정정 — `399 "NNN text"` 의 NNN 을 `CallInfo`·`RequestResult.warningCode` 로) |
| **U04** | MCPTT 요청 규격화 | REG-1 · GCC-1 · GCC-2 · GCC-3 · GCC-4 · GCC-5 · GCC-7 · GCC-8 · GCC-9 · ADH-8 · SDP-1 | B9 · D2 | 2 | S17 | — | W01(엔진 재빌드) | 진행 e1767d20(ADH-7 애드혹 해제 BYE Reason·ADH-8 SDK `session-type adhoc`·현장 앱 거절 문구 — S11·S08 의 짝) · c6c534b1(PRV-4 SDK — 개별 호 개시 방식 요청 옵션) · c053e4b6(GCC-9 착신 그룹 = `<mcptt-calling-group-id>`) · 0f611de0(GCC-5 `i=speech`) · 9d8ae0ab(S17 짝 묶음 A/B — REG-1 등록 태그 · GCC-1 PSI·Accept-Contact·PPS · GCC-2 Contact 태그 · GCC-3 client-id · GCC-4 chat · SDP-1 SDK 몫 `udp MCPTT`, .48 0.2.215 실측) · 4a5b64e4(묶음 C — PRV-1 개별 호 PSI·resource-lists · PRV-3 floor 없는 개별 호 판정 = m=application 유무, .48 0.2.217 실측) · 4c30cf8f(계약 골든 대조 `McxRequestGolden` — floor 없는 개별 호 text 슬롯·i=speech 고침) — 남은 것 GCC-8(재합류 R-URI = 세션 식별자) · GCC-7(S27 과 같이) · C API·.NET 칸(W01) · ADH-8 CSP 멤버 INVITE(.48) · PRV-4 앱 선택 UI |
| **U05** | MCData 요청 규격화·수신 파서 | MCData REG-1 · SDS-3 · SDS-6 · SDS-7 · SDS-10 · MSRP-1 · MSRP-4 · FD-7(SDK — Metadata `file-selector:`·`uploadFd` 규격형, CSP 생성분은 S16) | B3 · C2 · D3 | 1 | S17 | — | — | 진행 e6f50241(SDS-3 그룹 SDS·FD `<mcdata-client-id>` · SDS-6 선택 IE·응용 대상 메시지) · 453d7c37(SDS-7 일부 — TEXT·HYPERLINKS payload 여러 개) · 7b476278(SDS-10 현장 앱 — 보내기 전 검사) · 9d8ae0ab(REG-1 — SDS·FD 등록 태그 `mcdataMsrp`·`mcdataFd`) · 4a5b64e4(묶음 C — MESSAGE R-URI = MCData PSI·Accept-Contact·PPS · 1:1 SDS·FD resource-lists · MSRP-1 · CFG-1 CSC 광고 기본 켬, .48 csc 0.2.163) · 4c30cf8f(계약 골든 대조 · MSRP offer accept-types) — 남은 것 SDS-7(LOCATION·CODED TEXT·BINARY) · MSRP-4 · FD-7 |
| **U06** | 제휴 상태 구독 | AFF-12 / VAFF-8 | B1 · C1 | 1 | S13 | — | W01(VAFF-8) | 대기 |
| **U07** | XCAP 단말 전환 | GMS-16 · GMS-17 · GMS-1(SDK — global tree 조회) · GMS-6(SDK — 멤버 제외 조회) | B4 | 1 | C04·S19 | — | W01(GMS-16 PSI) | 완료 65046fec(GMS-1·GMS-17·GMS-6 코어) · 1c475d6e(GMS-6 바인딩) · 161e338d(GMS-16 규격형 xcap-diff 구독 — 엔진·SDK·현장 앱) — 관제 앱 두 벌의 구독 전환은 W01 |
| **U08** | 발언권·송출 제어 단말 세부 | FCC-5 / TCU-1(코어) · TCU-3 · TCU-4 · RCU-1 | C3 · D2 | 1 | — | — | — | 완료 9e9a8169 (TCU-1 앱 결선 → U09) |
| **U09** | MCVideo 단말 호 절차 | VREG-1 · VREG-3 · VGU-4 · VGU-5 · VGU-6 · VSDP-3 · TCU-1(앱 결선 — Kotlin 파사드·C API·현장 앱) | B1 · C3 · D2 | 1 | — | D7 | W01(VGU-6) | 진행 6c2c411a(VSDP-3·VGU-6 현장 앱) · 0251aa8e(VREG-1 코어 — 선택 옵션) · 354bc013(VREG-3 코어·VGU-5 코어·VGU-4 재확인 충족) · 1c475d6e(Kotlin 파사드·C API 함수·현장 앱 결선 — TCU-1·VGU-5·VREG-3) — 남은 것 VREG-1 켜기(S25 뒤) · C API 구조체 칸(W01 과 배치 맞춤) |

### 5.4 Windows — 관제 앱 (W01)

W01 이 주인인 항목은 CMS-13(CMS 변경 구독, 지금 5분 폴링) 하나다. 나머지는 서버·SDK WP 가 끝난 뒤 앱이 맞출 짝이다 — 엔진 재빌드와 함께 두 번에 나눠 한다(웨이브 2 끝 · 웨이브 4 끝).

| 짝 | 할 일 | 선행 |
|---|---|---|
| GCC-6 · VGU-3 | 원치 않는 초대 거절 = 480 + Warning 110(486·`hangup` 대신) | U03 |
| EMG-8 | 긴급 개시·상향·경보의 대상 그룹 판정(전용 긴급 그룹) | U01 |
| GMS-11 · GMS-18 | 그룹 편집 폼 — 정원 0(무제한)으로 되돌리기, 우선순위 0~255 | C01 |
| 문구 사전 · Capabilities | 116·120·122·123·141·206·213·217·198 등 응답 문구, 인가 요소가 늘어난 Capabilities 게이트 | S08 · S09 · C03 · S10 |
| GMS-16 · CMS-13 | xcap-diff 구독 = 설정된 PSI·본문, CMS 축 구독 | C04 · S19 · U07 |
| VAFF-8 · VGU-6 | 제휴 상태 NOTIFY 로 N2 판단, 세션 식별자 재합류 | U06 · U09 |
| FD-4 · SDS-6 · SDS-7 | Mandatory download 를 받으면 바로 받기, 앱 대상 SDS 는 말풍선 아님, HYPERLINKS·LOCATION 표시 | S16 · U05 |
| 그룹 편집 PUT | PUT 이 «교체» 가 된다(C05) — 앱이 보내는 본문이 문서 전체인지 확인 | C05 |

### 5.5 준비·문서

| WP | 이름 | 호스트 | 세션 | 할 일 | 상태 |
|---|---|---|---|---|---|
| **X00** | 준비 | .48 | 1 | .48 첫 세션 — 지시 = dev_share `20261003-0027_45_kickoff-48-gap-track.md`. ① worktree `/home/cims/work/.wt-gap`(브랜치 `gap48`, 공유 트리는 팀원 계측기 트리라 pull 도 하지 않는다)·첫 빌드·S1 기준선(S1-UNIT-CSP·CMP·PSIP) ② 배포 id·`cimsue-cli` 시험 신원(MCPTT·MCData — M2 runbook 의 test023~025·gmv1/gmv2 방식)·계측기 시나리오 목록 ③ dev_share `…_48_done-X00-48.md`. 규격 원문은 `/mnt/cims/dev_share/spec/` 에 갖춰졌다 | 완료 4fa3b567 |
| **X01** | 문서 정정 잔여 | .45 | 1 | 세 목록 §5 중 항목 번호가 없는 줄(근거 절 번호·Warning 절 §4.4→§4.9·mcdata_messaging §3·§5 파일 이름·mcx_identity_scope §10·fixtures README N2·mcvideo.md §9 hang-time 메모) + 세 목록 §4 «미구현 목록에 빠진 기능» 을 정본 미구현 목록(mcptt_standard_conformance §0-R · mcdata_messaging §8 · mcvideo §6 V8)으로 옮긴다. 코드 주석 정정(`csp/CscfModule.cpp` N2 주석 등)은 그 파일을 고치는 WP(S13)가 함께 한다 | 완료 6cdfc219 |

## 6. 순서 — 웨이브

```mermaid
flowchart LR
  W0["웨이브 0<br/>X00"] --> W1["웨이브 1<br/>급 A · 한두 줄"]
  W1 --> W2["웨이브 2<br/>응답 코드 · 인가 판정"]
  W2 --> W3["웨이브 3<br/>제휴 · 호 모델(D1)"]
  W3 --> W4["웨이브 4<br/>요청 규격화 · XCAP"]
  W4 --> W5["웨이브 5<br/>발언권 · 송출 제어 · 나머지"]
  W5 --> W6["웨이브 6<br/>엄격 검사 · 회귀"]
  W6 --> L(["라이브 반영(§8)"])
```

| 웨이브 | .48 | .45 | Windows | 끝에 |
|---|---|---|---|---|
| **0** 준비 | X00 | — | — | — |
| **1** 급 A · 한두 줄 · 인가 구멍 | S03 · S05 · S01 · S06 · S07 · S02 (6) | C01 · C03 · U01 · C06(2) · X01 (6) | — | — |
| **2** 응답 코드 · 인가 판정 · 애드혹 | S10 · S11 · S08 · S09 · S04 (5) | C02 · C07 · U02 · U03 (4) | W01 1차 | .48 회귀(계측기 동봉 시나리오) |
| **3** 제휴 · 호 모델 | S12(2) · S13(2) · S14(3) (7) | C04 · U06 · U08 (3) | — | D1 이 이 웨이브 전 |
| **4** 요청 규격화 · XCAP | S17(2) · S15 · S16 · S19 · S27 (6) | C05(2) · U04(2) · U05 · U07 (6) | W01 2차 | .48 회귀 |
| **5** 발언권 · 송출 제어 · 나머지 | S20(2) · S21(2) · S22(2) · S23 · S24 · S25(2) · S26 (11) | U09 (1) + .48 WP 넘겨받기(S22·S24) | — | — |
| **6** 마감 | S18 (1) | S3 회귀 · 남은 편차 정본화 | 관제 실기(.48) | .48 전체 회귀 → 라이브 반영(§8) |

- 웨이브 안에서는 급 A 가 많은 WP 부터 고른다. 선행이 없는 WP 는 웨이브를 앞당겨도 된다.
- 웨이브 3·4 는 .48 이 임계 경로다 — S12 → S13·S14, S17 → U04·U05 → S18.
- 결정 D1·D3·D6·D7 은 해당 웨이브 전에 받아야 그 WP 가 선다. 늦으면 그 WP 만 «보류(D?)» 로 두고 다음을 고른다.

## 7. 협업 규칙

- **작업 트리** — 이 트랙은 worktree 에서만(§4). 공유 트리 `/home/cims/work/cims` 는 .45 는 다른 세션의 미커밋, .48 은 팀원 계측기 트리라 손대지 않는다.
- **경로 소유** — §2 표. 상대 경로를 고쳐야 하면 짝으로 넘긴다(dev_share). 한 줄짜리라도 같다 — 같은 파일을 두 호스트가 동시에 고치지 않기 위해서다.
- **함께 쓰는 파일**
  - 갭 목록 세 벌 · 이 문서 §5 — 자기 WP 의 행만 고친다. 고치기 직전 `git pull`, 고친 뒤 곧바로 커밋(충돌 창을 좁힌다).
  - 정본 문서(mcptt_standard_conformance.md · mcdata_messaging.md · mcvideo.md) — 절 단위로 고친다. 같은 절을 두 WP 가 동시에 고치면 나중 쪽이 pull 뒤 합친다.
  - 발언권 정의 `mcptt_floor_defs.yaml` — .48 소유(S20·S21). 생성물(CMP·SDK 헤더)은 같은 커밋, .45 가 리뷰.
  - 요청 형식 계약 = 새 골든 `tests/fixtures/mcptt/sip/`(S17 — .48 소유, MCVideo K3 와 같은 방식). 서버 해석 시험과 SDK 생성 시험(U04·U05)이 같은 파일을 읽는다.
- **빌드** — 호스트마다 한 번에 하나(`pkg/` 공유). 패키지는 `./cims.sh pkg <모듈>`(auto-bump), md5 를 빌드 산출물과 대조한다.
- **.48 테스트베드** — 팀원 계측기와 같이 쓴다. 배포 전 dev_share `…_deploy-<모듈>-<버전>.md`, 팀원 run 이 돌고 있으면 기다린다.
- **공유 DB**(.45·.48·.135) — 표·열 추가만. 지우기·바꾸기는 모든 사이트가 새 빌드가 된 뒤 따로 정한다.
- **dev_share** — `/mnt/cims/dev_share/README.md` 규칙 그대로(메시지 = 파일 하나, 상대 파일 수정 금지, `.tmp` → `mv`). 정본은 git — 결론은 문서에 남긴다.

## 8. 배포·실측

- **.48** — 트랙의 개발·실측 대상 전부. 서버 WP 마다 올린다(§4 5단계). .45 의 CSC·단말 WP 도 .48 에 원격 배포해 시험한다.
  단말 시험은 .48 을 겨눈 `cimsue-cli` 로 한다 — 사내 단말(W999·MF52)은 .45 라이브에 붙어 있어 새 SDK 를 깔면 아직 반영되지 않은 서버와 어긋난다.
  - **시험 신원** = test026 · test027 · test028(`+82500000026~28`, 로그인 비밀번호 = 계측기 `creds/volte.jsonl` 의 같은 login 행 `loginPw`, MCVideo 자격 있음) · 그룹 `gap1`(prearranged, T4 10 s) ·
    `gap2`(chat) — 세 신원만 멤버, SDS·FD·긴급 허용, MCVideo 속성 있음. 계측기 PTT 신원(test001~006·011·012·023~025)은 팀원 워커가 등록을
    잡고 있어 쓰지 않는다. 계정 = `cimsue-cli --csc-host 127.0.0.1 --user test026 --pw-env <변수> --no-tls-verify --from-profile ptt
    --server 121.161.164.48 --port 15060 --affiliate gap1 …`(프로파일이 주는 `127.0.0.1:15060` 에는 CSP 가 없어 `--server` 가 필요하다).
    OAM 관리자 = [oam_api_deploy_runbook.md](oam_api_deploy_runbook.md) §0.
- **.45 라이브 반영**(협력업체 단말) — 웨이브 6 의 .48 전체 회귀가 통과한 뒤 한 번, 사용자 go·시각 지정 뒤. 실행은 사용자가 `!` 로 한 줄씩
  (분류기가 배포 호출을 막는다 · 여러 줄을 붙이면 첫 줄만 돈다). 한 창 안에서 단계를 나눈다:
  1. 서버 전부(CSC → CMP → CMDP → CSP → OAM) — 규격 형식만 받으므로 옛 APK 의 해당 요청은 이때부터 거절된다(K4).
  2. 사내 단말 APK(W999·MF52) → 실기 확인.
  3. 협력업체 APK·소스 번들 전달(같은 창).
- overlay 가 정본인 설정(csc `config.json`)은 overlay 로 넣는다. 각 단계 전후로 계측기 동봉 시나리오(MCPTT·MCData·MCVideo 축)로 회귀를 본다.

## 9. 진행 관리

- 상태는 §5 의 «상태» 칸 하나다(진행 기록을 따로 두지 않는다 — 이력은 git). 항목 단위 진행은 갭 목록에서 지워진 것으로 본다.
- 갭 목록의 웹 보기는 목록 md 에서 다시 만든다(지운 항목은 사라지고 WP·호스트 열을 붙일 수 있다).
- 웨이브가 끝나면 이 문서 §5 의 남은 WP·결정만 남기고, 끝난 WP 행은 지운다(최종 상태만).

## 10. 위험

| # | 위험 | 대응 |
|---|---|---|
| R1 | 코드 읽기 판정의 오판 | 재확인에서 기각을 허용한다(§4 2단계). △ 는 실측 전에 고치지 않는다 |
| R2 | 규격 형식만 받으므로 옛 APK·계측기·cspsim 이 거절된다 | 개발 단계라 받아들인다(K4) — 우리 SDK·앱은 짝으로 같이, 계측기는 팀원에게 요구, 협력업체 APK 는 라이브 반영 창에서 바꾼다 |
| R3 | 라이브 반영이 끝에 한 번이라 변경이 크다 | .48 전체 회귀 뒤, 한 창 안에서 서버 → 사내 APK → 협력업체 APK 순으로 나눈다(§8) |
| R4 | 공유 DB | 추가만(§7) |
| R5 | 두 호스트가 같은 문서를 동시에 고침 | 행·절 단위, pull 직전·커밋 직후 |
| R6 | .48 테스트베드를 팀원과 같이 씀 | 배포 알림, 팀원 run 중에는 대기 |
| R7 | 원문 판 차이(V18·V19·V20) | X00 README 에 판 고정, 다르면 최신 판으로 다시 본다 |
| R8 | TS 24.582 원문이 V17.1.0 으로 TS 24.282(V18·V19)보다 오래됐다 | S24·U05 재확인에서 판 차이로 보이면 새 판을 받아 다시 본다 |
| R9 | 호 모델(D1) 변경의 운용 영향 | 현장 앱·관제 운용 시나리오를 S14 재확인에서 먼저 돌려 본다 |

## 11. 완료 기준

- 갭 목록 세 벌이 비었거나, 남은 행이 전부 정본 문서의 «규격 대비 편차» 표로 옮겨졌다.
- 규격 형식만 받는 서버에서 사내·협력업체 단말 실기가 통과한다(계측기는 팀원 트랙이 규격형으로 맞춘 시나리오로).
- .45 라이브 반영(§8 의 세 단계)과 협력업체 APK 번들이 나갔다.
