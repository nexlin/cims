# CSP 제어평면 부하 견고화 — 버그 분석 및 수정 (2026-06-06)

> VoLTE 다중 cps 부하 시험 중 발견·수정한 CSP 측 결함 5건의 근본원인·증거·수정 정리.
> 후속 세션이 같은 맥락을 빠르게 잡을 수 있도록 작성. 관련 메모리:
> `project_session_2026_06_06_4cps_500_cmpclient_nfs_logging`.

## 0. 배경 / 증상

ctrl01 단일 4코어 box 에 **cspsim(부하생성기) + csp + csc + oam + console 동거** 상태에서 VoLTE 음성호 부하 시험:

- **1 cps / HT20**: 완전 정상 (30/30 200, setup ~1.1s, 누수 0).
- **4 cps / HT20**: **10~19 콜이 `status=500`**, setup p50 5~9초로 폭증.

500 의 정체 = CSP 가 SIP `INVITE` 처리 중 CMP 에 보낸 제어요청(`ADD_SESSION` 등)의 응답을 제때 못 받아
`SendRequestAndWait` 가 타임아웃 → CSP 가 `500 Internal Server Error` 생성.

핵심 관찰: **CMP 는 항상 5~25ms 안에 정상 응답**(NFS 의 CMP 측 flow 로그로 확인)했는데 **CSP 만 2초 타임아웃**.
즉 병목은 CMP(미디어노드)가 아니라 **CSP 측 CmpClient/로깅 경로**. 또한 시험 내내 **csp 프로세스 CPU 는 3~5%로
CPU-bound 가 전혀 아님** — 스레드가 "연산"이 아니라 "대기"로 막혀 있었다(직렬화/유실 신호).

---

## 버그 1 — SipMessageLogger: 동기 NFS 로깅이 디스패치 임계경로를 블록

### 근본원인
`CSipMessageLogger` 가 **open-per-write**(매 줄 `fopen`/`fprintf`/`fclose`)로 NFS(`/mnt/cims`)에 기록.
모든 로깅이 전역 `m_mtx` 하에 직렬화되고, 그 락을 **NFS I/O 동안 보유**했다.

특히 `CCmpClient::RecvLoop`(단일 수신 스레드)가 CMP 응답마다:
`recvfrom` → `gclsSipLogger.LogMessage(...)`(**NFS 동기 기록**) → **그 다음에야** `OnTransactionComplete`(대기 caller `cv.notify`).
4cps 버스트에서 NFS 쓰기가 밀리면 **단일 수신 스레드가 막혀 뒤따르는 모든 CMP 응답 dispatch 가 head-of-line 블로킹** → 2초 타임아웃.

### 증거
- csp CPU 낮음(NFS I/O wait, D-state) · 양 미디어노드 동시 타임아웃(단일 수신 스레드가 양쪽 응답 처리) ·
  HEARTBEAT 응답마저 타임아웃 · 1cps 는 무사(쓰기 완료 여유).

### 수정 — 비동기 배치 writer
`csp/SipMessageLogger.{h,cpp}`
- 생산자(`Print`/`LogMessage`, `m_mtx` 보유)는 **포맷·seq 부여까지만**(전부 in-memory, NFS 없음) 후
  `EnqueueLine(파일경로, 라인)` 으로 큐 적재하고 즉시 반환.
- 단일 **writer 스레드**가 ① 큐 임계(128줄) 또는 ② **주기 flush(100ms)** 마다 큐 전체 swap →
  **파일경로별로 라인을 합쳐 경로당 1회 `fopen`→`fwrite`→`fclose`** (open-per-write → open-per-batch).
- 단일 writer + FIFO 라 **파일 줄 순서 = enqueue(=seq) 순서** 유지 → `flow.seq` ↔ `msg` 줄번호 cross-ref 정합 보존.
- 백프레셔: 큐 상한 20만 줄 초과 시 oldest drop + 카운터(`m_ulDroppedLogs`). 종료 시 `StopWriter` 가 잔여 전량 flush.
- 효과: `m_mtx` 보유시간이 NFS지연(수백 ms) → µs. **이 자체로 RecvLoop notify 가 즉시 일어나 HOL 해소.**

### 주의
- SIGKILL 강제종료 시 버퍼의 ≤100ms 분량 로그 유실(관측로그라 허용).
- ⚠️ **동일 open-per-write 패턴이 CMP(미디어노드)·CSC/OAM(Python)에도 존재** → 같은 비동기화 적용 후보(미적용).

---

## 버그 2 — CmpClient: condition_variable lost-wakeup (술어 없는 wait_for)

### 근본원인
`CCmpClient::_SendOnEndpoint`(`csp/CmpClient.cpp`)의 응답 대기가 **술어(predicate) 없는** `wait_for`:
```cpp
std::unique_lock<std::mutex> lock( pTrans->mutex );
if ( pTrans->cv.wait_for( lock, 2000ms ) == cv_status::timeout ) { ... }   // ← 버그
```
`sendto` 직후 sender 가 `wait_for` 에 **진입하기 전에** CMP 응답(~25ms)이 도착하면,
`RecvLoop` 의 `OnTransactionComplete`→`notify_one()` 이 **대기자 없이 유실**. 이후 sender 는 술어 없이 wait 진입 →
**이미 완료된 트랜잭션을 2초 꼬박 대기**(lost-wakeup → 거짓 타임아웃 → 500).

### 증거 (반직관적)
- 버그 1 수정(비동기 로깅) **후 오히려 500 증가**(v1 10건 → v2 12건). 이유: RecvLoop notify 가 빨라져
  **sender 진입 전 notify 가 더 자주 발생** → lost-wakeup 노출. 즉 버그1 fix 가 잠재 버그2 를 드러냄.
- 타임아웃난 trans 들이 CMP 측에선 5~25ms OK 응답(정상)인데 csp 만 2초 타임아웃.

### 수정
```cpp
bool bDone = pTrans->cv.wait_for( lock, ..., [&pTrans]{ return pTrans->bCompleted; } );
```
`OnTransactionComplete` 이 `pTrans->mutex` 하에 `bCompleted=true` 설정 후 notify → 술어가 notify 선행/spurious 모두 안전 처리.

---

## 버그 3 — CmpClient: 제어소켓 SO_RCVBUF 미설정 (수신 버퍼 overflow)

### 근본원인
`Init` 가 socket 생성 후 **SO_RCVBUF/SNDBUF 를 설정하지 않음** → 기본 ~212KB.
단일 소켓에 양 미디어노드 응답이 몰리는 버스트에서 수신 버퍼 overflow → **CMP 응답 datagram 커널 드롭**.
드롭된 응답은 `OnTransactionComplete` 미호출 → 트랜잭션 미완료 → 타임아웃.

### 증거
버그2 수정 후에도 잔존한 타임아웃 36건 분류: **24건 = CMP RX+TX 정상응답했는데 csp 미수신**(수신측 유실),
12건 = 요청 미도달(송신측 유실).

### 수정
`Init` 에서 `setsockopt(SO_RCVBUF/SO_SNDBUF, 4MB)` (커널 `net.core.{r,w}mem_max`=4MB 로 캡, 보고값 8MB).
기본 대비 ~40배. non-root 가능(rmem_max 한도 내).

---

## 버그 4 — CmpClient: 제어요청 재전송 부재 (단발 UDP 유실 = 즉시 호 실패)

### 근본원인
UDP 제어평면인데 **재전송이 없음** → datagram 하나만 유실돼도 2초 타임아웃 → 500.

### 안전성 — CMP 멱등 확인 (재전송 전제)
`cmp/PCmpServer.cpp::processAdd` 가 **`session_id` 기준 멱등**: 기존 세션이면 새 자원 할당 없이 동일 포트 반환.
MODIFY/REMOVE 도 동일. → **같은 `trans_id`+payload 재전송이 안전**(중복 relay/누수 없음).

### 수정
`_SendOnEndpoint` 의 단일 2초 wait → **재전송 루프**: **100ms × 최대 3회(총 ceiling 300ms)**.
미완료 시 동일 패킷 재송 후 재대기. 단발 유실을 100ms 내 복구, 진성 실패는 300ms 에 빠르게 판정.
- CMP 정상 응답 5~25ms 관측 → 100ms 는 4~20배 마진(정상 응답에 헛발동 없음). 멱등(ADD/MODIFY/REMOVE 모두
  session_id 기준)이라 재전송 안전.
- 효과: 4cps 동거 기준 500 17→9(재전송이 다수 복구). 송신측 유실(RCVBUF 로 못 잡는 부분)의 유일한 복구 수단.
- (초기 검증은 400ms×5 로 진행, 이후 100ms×3 으로 조정 — CMP 응답이 일관되게 <25ms 라 더 타이트하게.)

---

## (검토 후 불채택) 다중 수신 스레드

검증 중 일시적으로 `RecvLoop` 를 4개 스레드로 늘렸으나(v6) **불필요로 판단해 되돌림**.
- 이유: CmpClient 는 **단일 소켓**으로 양 CMP 응답을 받고 **커널 수신 큐는 하나**라 다중 스레드가 실질
  병렬 수신 이득이 없다. 버그1(비동기 로깅) 수정 후 수신 처리는 `recvfrom`→parse→enqueue→notify 의
  **µs 급**이라 단일 스레드로 초당 수만 건 처리 가능(실부하는 초당 수백 건).
- v6 의 p50 개선(3224→1167ms)은 **동거 환경 노이즈**로 해석(같은 v6 에서 500 은 9→12 로 증가, 일관성 없음).
  진짜 병목은 v7 에서 드러난 **동거(cspsim) 포화**였고 수신 스레드 수와 무관.
- "CMP 당 1개" 도 현 구조엔 부적합 — 응답은 source 가 아니라 `trans_id` 로 demux 하므로 endpoint 별
  소켓/스레드가 불필요. **단일 수신 스레드 유지가 정답.**

---

## 검증 진행 (4cps/HT20, 동거 vs 분리)

| run | csp 변경 | 500 | setup p50 | csp CPU | 비고 |
|---|---|---|---|---|---|
| v1 | 기준(0.0.21) | 10 | 4859ms | 3.6% | 동기 NFS 로깅 |
| v2 | +비동기 로깅 | 12 | 5614ms | 4.9% | lost-wakeup 노출 |
| v3 | +csp Debug/Net 로그 OFF | 19 | 9196ms | 3.5% | 로깅 주범 아님 확인 |
| v4 | +lost-wakeup fix | 17 | 4552ms | 3.5% | UDP 유실 잔존 |
| v5 | +SO_RCVBUF+재전송 | 9 | 3224ms | 3.6% | 재전송 153회 복구 |
| v6 | +다중 recv스레드(후에 불채택) | 12 | 1167ms | — | p50 변화는 동거 노이즈로 판단 |
| **v7** | **동거 분리(csp@ctrl02)** | — | **1073ms** | **1.9%** | ⭐ **tail 소멸** |
| v8 | 분리 10cps/HT5/60s | 0(500) | 1109ms | 5.2% | csp 여유, 누수 0 |

### ⭐ 결정적 결론
- **CSP 는 4cps·10cps 의 병목이 아니다.** 동거를 제거(cspsim@ctrl01 → csp@ctrl02)하니 csp 는
  **CPU 1.9~5.2% / setup p50 ~1.1초 / 누수 0** 로 여유롭게 처리. v1~v6 의 4cps 실패는
  **동거 cspsim 이 공유 4코어 box 를 포화**시킨 것이 근본(csp 결함 아님).
- 4개 fix 는 모두 **실제 결함/견고화**라 유지·커밋 가치. (1·2 는 정합성 버그, 3·4 는 부하 견고화)
  다중 수신 스레드(v6)는 검토 후 **불채택**(단일 소켓 구조상 이득 없음).
- 분리 시험의 잔존 408/410(v7 37건·v8 49건)은 **csp 가 아니라 cspsim(단일 프로세스 부하생성기)** 한계:
  csp 유휴(1.9~5%)인데 cspsim 은 100~125%(1코어 초과)로 포화 — 200 UA + 착신 leg 응답 + RTP 동시처리 한계.

---

## 추가 발견 (2026-06-06 후속) — UDP 드롭의 실제 위치 + 408 의 진짜 근본

"로컬 내부망인데 UDP 재전송이 발생하는 건 이상하다" 는 지적에서 출발해 실제 커널 드롭 카운터를 조사한 결과,
재전송이 잡던 손실보다 **훨씬 큰 두 가지 문제**를 발견.

### (a) SIP 수신소켓(5060) 버퍼가 OS 기본 212KB — psip 가 SO_RCVBUF 미설정
- `ss -uanm` 으로 소켓별 드롭 확인: **CMP 제어소켓(9001)=8MB·드롭 0**(버그3 fix 효과), 그러나
  **SIP 소켓(5060)=212KB(OS 기본)·드롭 d11909**. `/proc/net/snmp` 의 `RcvbufErrors` 거의 전부가 5060.
- 원인: `ext/psip/SipPlatform/SipUdp.cpp::UdpListen` 가 소켓 생성 후 SO_RCVBUF 미설정 → OS `net.core.rmem_default`(212KB) 사용.
- 조치: **OS `net.core.rmem_default=4MB, rmem_max=8MB` 상향(sysctl, 4서버)** → 재기동 후 SIP 소켓 rb 212KB→4MB 자동 확대(코드 패치 불필요). (psip 에 SO_RCVBUF 명시 설정하는 패치도 대안.)

### (b) ⭐ 진짜 408 근본 = 모든 VoLTE INVITE 가 그룹 조회 cache miss → 매 호 `LoadAllGroups`(DB 폭풍)
- `EventIncomingCall` 이 착신번호를 **그룹인지 먼저 조회**하는데, VoLTE 1:1 호의 착신(일반 사용자 MSISDN)은
  그룹 캐시에서 **항상 miss → `LoadAllGroups`(SelectGroup ×5, 각 40멤버) 전체 재로드**. 600콜 시험에서 LoadAllGroups 3183회.
- 이 **동기 DB 호출이 SIP 스레드를 블록** → 5060 소켓이 (4MB 로 키워도) 가득 차 드롭 → INVITE/응답 유실 → 408.
- **이전 v7/v8 의 408 을 "cspsim 한계" 로 본 것은 오진** — 실제로는 csp 의 이 per-INVITE DB 재로드가 주범.
- ⚠️**`UdpThreadCount` 를 8 로 올리자 오히려 전면 붕괴**(v8 2스레드 215성공/49실패 → v9 8스레드 0성공/584×408):
  스레드가 늘면 동시 LoadAllGroups DB 폭풍이 병렬화되어 DB/처리 정체가 심화. 즉 **버퍼·스레드 증설은 대증요법이고,
  근본은 라우팅이 매 호 전체 그룹을 재로드하지 않도록 고치는 것**.
- 조치 후 UdpThreadCount 는 2 로 원복.

### ✅ (b) 수정 — `EventIncomingCall` 그룹 lazy-load 게이트 + 단건 조회 (csp 0.0.28)
PTT-AS lazy-load 를 두 조건으로 게이트:
1. **착신이 등록된 가입자(`gclsCspUserMap.isAlive`) 면 DB 조회 자체 생략** — 1:1 VoLTE 호는 그룹이 아니므로 폭풍 원천 차단.
2. 미등록 타겟(신규 그룹 가능성)만 **`CGroupMap::LoadOneFromDb(pszTo)` — 전체(LoadAllGroups) 대신 해당 id 단건만 조회·로드**
   (`SelectGroup` 재사용, 맵 Clear/재구축 없음). 그룹이면 Insert 후 ProcessGroupCall, 아니면 1회 cheap SELECT 로 끝.
   - (초기엔 3초 throttle+LoadFromDb 로 구현했으나, 사용자 제안대로 **단건 조회**가 더 정확·효율적이라 그것으로 대체.)
- 검증 v11(10cps/ht5, ctrl01 동거): **per-call 그룹 DB 조회 0**(VoLTE 착신=등록가입자라 isAlive 에서 skip;
  LoadAllGroups 5=startup only, single-group 0), **408 584→0, 200 성공 408, setup p50 1122ms**.
  남은 500×60 은 **동거 포화**(CMP 타임아웃·재전송; off-box v8 500=0) — csp 결함 아닌 테스트환경 한계.
  **즉 408/DB폭풍 완전 해결.**

## 후속/주의 사항

1. **CMP·CSC·OAM 비동기 로깅 미적용**: 버그1 과 동일한 open-per-write 패턴. 같은 방식 적용 후보.
2. **100cps 검증 불가(현 환경)**: 100cps×HT20 = 동시 2000호. cspsim 근접 실행으로는 측정 불가 →
   **분산 부하생성기(다중 호스트 cspsim) + 전용 HW + 다노드 csp/cmp** 필요.
3. **`Network` 로그 OFF 부작용**: psip `NETWORK` 레벨 콜백이 SipMessageLogger 의 SIP 메시지/flow 기록을 트리거 →
   `Setup.Log.Level.Network=false` 면 **SIP flow 로깅도 비활성화**된다(부하 시험엔 OFF 권장하나 SIP 추적 불가).
4. **운영 상태 변경분**(이번 세션, 커밋 전 정리 필요):
   - csp 0.0.26 양 노드 배포(바이너리 in-place swap, **OAM 미경유·비공식** — 정식 재배포 권장).
   - ctrl01/ctrl02 `csp.json` 의 `Log.Level.Debug/Network=false` 로 변경됨.
   - 검증 위해 VIP 121.161.164.47 을 ctrl02 로 failover(ctrl01 keepalived stop) — **`sudo systemctl start keepalived` 로 원복 필요**.

## 교훈 및 진단 플레이북 (다음에 더 빠르고 확실하게)

이번 세션은 같은 증상(부하 시 500/408·setup 폭증)을 두고 **원인을 5번 갈아탔다**(NFS 로깅 → lost-wakeup →
UDP 버퍼 → SIP 소켓 버퍼 → 라우팅 LoadAllGroups). 매번 "고쳤는데 또 실패" 가 반복된 이유와 교훈을 정리한다.

### 핵심 원리 — "수신 스레드를 동기 I/O 로 막지 마라"
이번에 발견된 결함들은 **표면 증상은 다르지만 뿌리가 하나**다:
**제한된 수신 스레드(SIP UdpThreadCount=2, CmpClient RecvLoop 1개)가 처리 중 동기 I/O 로 블록되면,
그동안 소켓을 못 비워 커널 버퍼가 차고 패킷이 드롭된다.** 블록 원인이 매번 달랐을 뿐:
- NFS 로깅(open-per-write) — 버그1
- `cv.wait_for` lost-wakeup 으로 2초 헛대기 — 버그2
- 작은 소켓 버퍼라 잠깐의 지연에도 overflow — 버그3·(b)
- CMP 왕복 동기 대기 — 버그4 가 완화
- **매 INVITE 의 동기 DB `LoadAllGroups`** — (b), 가장 큰 throughput 킬러
→ 설계 원칙: **수신/디스패치 스레드의 임계경로에서 NFS·DB·동기 RPC·락보유-I/O 를 제거**(비동기 큐, 캐시,
단건 조회, notify 선행). 이번 fix 들은 모두 이 원칙의 적용이다.

### 진단 플레이북 — 부하 시 호 실패(500/408/setup 폭증) 나오면 이 순서로
1. **CPU 가 원인인지 먼저 배제**: per-process CPU 샘플(`/proc/<pid>/stat`). **csp CPU 가 낮은데(한 자릿수%) 실패면
   CPU 포화가 아니라 "블록/유실"** 이다 — 락·동기 I/O·버퍼 드롭을 의심(이번 모든 케이스가 이랬다).
2. **커널 UDP 드롭 카운터 확인(로컬망이면 "전송손실" 아닌 버퍼 드롭)**:
   - `cat /proc/net/snmp | grep ^Udp` → `RcvbufErrors`/`SndbufErrors`.
   - `ss -uanmp` → 소켓별 `skmem(... d<N>)` 의 **d=드롭, rb=rcvbuf**. **어느 포트(5060 SIP? 9001 CMP?)가 드롭하는지 반드시 소켓 단위로 본다** (이번에 9001=0, 5060=11909 로 범인이 갈렸다).
3. **양단 시각 대조로 "누가 느린가" 격리**: CSP 로그 TX 시각 ↔ 상대(CMP) flow 로그 RX→TX 시각.
   이번엔 **CMP 가 5~25ms 응답했는데 CSP 가 2초 타임아웃** → 범인이 CSP 측임을 즉시 확정.
4. **동기 I/O 빈도 grep**: `grep -c LoadAllGroups`·`SelectGroup`·`Timeout`·`retransmit` 를 **시험 윈도우로 한정**해서
   센다(누적 로그 주의). per-call 로 수천 번 찍히면 그게 병목.
5. **OS UDP 버퍼 한도 확인**: `net.core.rmem_default`(소켓 미튜닝 시 이 값 사용)·`rmem_max`(SO_RCVBUF 캡).
   앱이 SO_RCVBUF 미설정이면 `rmem_default` 가 그대로 적용된다(psip SIP 소켓이 그랬다 → rmem_default 상향으로 해결).

### 이번 세션이 남긴 함정/오진 (반복 금지)
- ⚠️ **co-location confound**: cspsim(부하생성기)을 csp 와 같은 box 에 두면 cspsim 이 코어를 먹어 csp 가
  멀쩡한데도 실패한다. **부하시험은 부하생성기를 반드시 off-box(다른 노드)에서** — 이걸 안 해서 v1~v6
  내내 "csp 병목" 으로 오인했다(실제 csp 는 1.9%). HA standby 노드를 active 로 돌려 분리하는 방법 유효.
- ⚠️ **대증요법이 악화시키기도**: SIP 버퍼/스레드 증설(UdpThreadCount 8)은 근본(매 호 DB 폭풍)을 두면
  오히려 DB 폭풍을 병렬화해 **전면 붕괴**시켰다(v8 215성공 → v9 0성공). 근본부터 고치고 튜닝은 그 다음.
- ⚠️ **로그 레벨 변경의 부작용**: `Setup.Log.Level.Network=false` 가 SIP flow 로깅까지 끈다(psip NETWORK 콜백 커플링) — 부하시험엔 OFF 가 맞지만 그러면 SIP 추적 불가, 트레이드오프 인지.
- ⚠️ **fix 가 잠재버그를 드러냄**: 비동기 로깅(빨라진 RecvLoop)이 lost-wakeup 을 노출해 500 이 늘었다. "고쳤는데 악화" 는 새 버그가 아니라 가려졌던 버그일 수 있다.
- ⚠️ **누적 로그 카운트 착시**: `grep -c` 가 이전 run 까지 세서 오판. 항상 시험 시각 윈도우로 한정.
- ⚠️ **불필요한 fix 경계**: 다중 recv 스레드(#5)는 단일 소켓 구조상 무의미했고 p50 개선은 노이즈였다.
  "측정으로 효과가 재현되는가" 로 채택/기각(재전송#4 는 17→9 재현되어 채택, #5 는 기각).

### 검증 자산(재사용)
- per-process CPU 샘플러: `/tmp/volte_mon/sampler.py`(외부의존 無, /proc 기반).
- 부하: `build/bin/cspsim -server_ip <VIP> -mode volte -scenario call -domain ims.mnc033… -cps N -ht H -calls C -count 200 -no_video -media_dir tests/media -db <csp.json>` (착신 등록 위해 `-count` 充, 음성만 `-no_video`).
- HA failover 로 부하생성기 분리: ctrl01 `sudo systemctl stop keepalived`(VIP→ctrl02), 원복 `start`.

## 변경 파일 (커밋 대상)
- `csp/SipMessageLogger.{h,cpp}` — 버그1(비동기 배치 writer)
- `csp/CmpClient.{h,cpp}` — 버그2(predicate)·3(SO_RCVBUF)·4(재전송 100ms×3). (다중 recv스레드는 시도 후 단일로 환원)
- `csp/ModuleDispatcher.cpp` + `csp/GroupMap.{h,cpp}` — (b) EventIncomingCall 그룹 lazy-load 게이트 +
  `CGroupMap::LoadOneFromDb`(전체 재로드 대신 단건 조회·로드)
- `csp/pkg.json` — 0.0.21 → 0.0.28
- OS(4서버): `net.core.rmem_default=4MB, rmem_max=8MB` (sysctl; `/etc/sysctl.d` 영속화 권장)

---

# 부록 (2026-06-07) — PTT 소크 후속: csc/cmp 비동기 로깅 + csp DB 데드락

> PTT 그룹콜(g001 40명, floor 10s 순환) 오버나잇 소크 중 발견·수정. 관련 메모리:
> `project_session_2026_06_07_async_logging_overnight`.

## 버그 6 — csp DbManager: 단일 DB 연결을 락으로 직렬화 + 쿼리 타임아웃 부재 → 데드락

### 근본원인
`csp/DbManager` 는 **단일 `MYSQL*` 연결**을 `recursive_mutex m_mutex` 로 직렬화하고,
`Connect()` 가 `MYSQL_OPT_RECONNECT` 만 설정할 뿐 **read/write/connect 타임아웃을 전혀 설정하지 않았다**.
모든 DB 접근(REGISTER 인증·그룹/세션 조회·CDR 등)이 이 락 하에서 `mysql_query` 를 호출한다.

연결이 half-open(상대 무응답·네트워크 hiccup)으로 멈추면 `mysql_query` 가 **무한 블록**(스레드가
`poll_schedule_timeout` 에 정지) → **락을 영구 보유** → REGISTER/group 을 처리하는 SIP 스레드가 전부
`m_mutex` 에서 wedge → SIP UDP 5060 소켓이 드레인되지 않아 가득 참 → **csp 전체가 데드락**(재기동 외 복구 불가).
`MYSQL_OPT_RECONNECT` 는 쿼리가 에러를 *반환*할 때만 동작하므로 무한 블록은 막지 못한다.

### 증상 / 증거
- PTT 통화 정상 ~20분 후 **40명 동시 REGISTER 갱신 버스트** → 전원 `REGISTER FAILED 408`. floor 정체.
- `ss -uanm` 5060 소켓: `r` 가 수신버퍼(4MB)까지 차고 드롭(`d`) 증가, **부하(cspsim) 제거 후에도 안 빠짐** = wedge.
- `ps -L -o wchan`: csp 스레드 **5개가 `futex_do_wait`**(뮤텍스 락 대기), 락보유 후보가 `poll_schedule_timeout`(=DB 쿼리 네트워크 대기). **CmpClient(별도 스레드)만 정상**(heartbeat OK).
- 트리거 = 40 동시 REGISTER 버스트(DB 경합 급증). sysctl rmem 은 이미 4MB(버퍼가 아니라 **드레인** 문제).

### 수정 (csp 0.0.30)
`DbManager::Connect()` 에 `mysql_options` 로 타임아웃 추가:
`MYSQL_OPT_CONNECT_TIMEOUT/READ_TIMEOUT/WRITE_TIMEOUT = 5초`.
멈춘 쿼리가 **무한 대신 유한 시간 후 실패** → 락 해제 → 회복(다음 쿼리에서 RECONNECT). 영구 wedge 제거.
(근본 아키텍처 개선=DB 연결 풀이나 lock 밖 쿼리이나, 단일연결+락 구조상 타임아웃이 최소·고위험낮은 핵심 fix.)

### 검증
- csp 0.0.30 배포 후 **무패치 csp 가 데드락 났던 ~3.5h 시점을 넘겨 3h41m+ 무재발**, REGISTER FAILED 동결, SIP `r0`.
- gdb(ptrace_scope=0) 백트레이스로 락 확정 예정이었으나 fix 후 미재발(=좋은 결과). 감시기
  `scripts/csp_deadlock_watch.sh` 가 SIP 5060 wedge 감지 시 ps wchan + gdb 백트레이스 자동 캡처 + csp restart.

## csc/cmp 비동기 배치 로깅 (버그 1 패턴 이식)
csp `SipMessageLogger` 의 비동기 배치 writer(생산자=포맷+enqueue만, 단일 writer 스레드가 경로별 open-per-batch)를
- **cmp** `PCmpServer`(0.0.12): writeMsgLine/logFlow/logBody/writeLeakReclaim → `enqueueLine`+`logWriterLoop`.
  단일 control 스레드가 매 패킷 NFS open-per-write(특히 `sendResponse` 가 `sendto` **전에** 기록)로 막히던 HOL 제거.
- **csc** `logger.py`(0.0.14): `log_flow` open-per-write → deque + writer 스레드 + `_flush_batch`.
양쪽 4서버 배포, 10h+ 안정. (OAM 은 관리플레인이라 미적용.)

## 테스트 인프라 교훈 (소크 운영)
- ⚠️ **부하생성기 로그를 tmpfs(/tmp)+usrquota 에 두지 마라**: cspsim RTP STATS 폭증이 /tmp(RAM) 를 채워
  EDQUOT → 셸 전체 마비. 로그는 **디스크**에, RTP STATS 등 고빈도 노이즈는 grep 필터, 크기 cap 안전장치.
- ⚠️ **cspsim `-floor_loop` busy-spin**: 멤버 BYE 후 떠난 멤버에 floor 무한 시도("not in call" 스팸). 도구 결함 →
  `-floor_rounds N`(정상 종료·재수립 반복) + CPU watchdog 으로 우회.
- ⚠️ **co-location 의 또 다른 형태 = OOM**: ctrl01(7.4GB)에 IDE(antigravity node, RES1.5GB+swap1.8GB)+claude+소크+mariadbd
  동거 → 스왑 thrashing(`ksoftirqd` 100% = BLOCK softirq=스왑 페이징 I/O 완료) + **OOM killer 가 csp 주기 kill**(watchdog 복구).
  진단: `vmstat`(si/wa/st), `/proc/softirqs`(BLOCK vs NET_RX), `journalctl ... 'OOM killer killed'`. csp 재기동 ≠ 데드락일 수 있음.

## 변경 파일 (2026-06-07, 커밋 대상)
- `csp/DbManager.cpp` — 버그6(DB connect/read/write 타임아웃 5s). `csp/pkg.json` 0.0.28→0.0.30.
- `cmp/PCmpServer.{h,cpp}` + `cmp/pkg.json`(0.0.10→0.0.12) — 비동기 배치 로깅.
- `csc/src/services/logger.py` + `csc/pkg.json`(0.0.12→0.0.14) — 비동기 배치 로깅.
- `scripts/overnight_ptt_0608.sh`·`scripts/csp_deadlock_watch.sh` — 소크 러너·데드락 감시기(신규).
