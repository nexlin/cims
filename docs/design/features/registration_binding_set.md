# 등록 바인딩 집합 — flow 단위 도달 관리

CSP 가 "이 가입자에게 어떻게 도달하는가"를 관리하는 구조의 정본 문서다. 현재는 AoR(가입자)
당 **주소 칸 하나**를 두고 새 등록이 그것을 덮어쓴다. 이 문서는 그것을 **바인딩 집합**으로
바꾸는 설계를 정의한다.

관련 문서: [ue_nat_traversal.md](ue_nat_traversal.md) · [sip_tls_signaling.md](sip_tls_signaling.md) ·
[modules/csp.md](../modules/csp.md)

> **상태**: 설계 정본, 구현 착수 전. 범위는 **A안(flow 추적)** — 서버 내부 구조 교체이며
> 단말 변경이 없다. 멀티 디바이스(한 계정 여러 단말 동시 사용)는 이 문서의 범위가 아니다
> ([§7](#7-멀티-디바이스는-범위-밖-b안)).

## 1. 지금 구조와 그 대가

```
CUserMap:  AoR ──▶ CUserInfo{ m_strIp, m_iPort, m_eTransport, 만료, Contact, CSeq }
                   └─ 칸 하나. 새 등록이 덮어쓴다.
```

칸이 하나뿐이라 "지금 들어온 이 등록이 살아있는 도달 경로인가"를 서버가 **추측**해야 한다.
그 추측이 현재의 latch 갱신 규칙이다([sip_tls_signaling.md §4](sip_tls_signaling.md#4-latch-갱신-규칙)):

| 규칙 | 내용 | 성격 |
|---|---|---|
| ① | 수신 transport == 저장 transport 일 때만 갱신 | 승격 TCP 를 걸러내기 위한 **대리 판정** |
| ② | 인증된 REGISTER + 새 transport ≠ TCP 면 교체 | "TCP 만 우발 발생한다"는 **경험적 판별자** |

두 규칙은 실측으로 동작하지만, 근거가 "transport 종류"이지 "그 경로가 살아있는가"가 아니다.
저장값 `(IP, 포트)` 는 스트림 transport 에서 **연결의 이름표**일 뿐이라, 연결이 죽으면 같은
값의 의미가 "이 연결에 써라"에서 "이 주소로 새로 연결해라"로 조용히 뒤집힌다
([sip_tls_signaling.md §2.2](sip_tls_signaling.md#22-tcptls--저장값은-살아있는-연결을-찾는-열쇠)).

## 2. 목표 구조

```
CUserMap:  AoR ──▶ [ Binding{ ip, port, transport, contact, 만료, last-seen },
                     Binding{ … }, … ]
```

- **키 = (ip, port, transport)** — 이 3원소가 그대로 flow 키다. 스트림에서는 psip
  소켓맵(`CTcpSocketMap`)의 키와 **같은 값**이고, UDP 에서는 NAT 매핑을 가리킨다.
- 새 등록은 **덮어쓰기가 아니라 바인딩 추가/갱신**이다. 승격 TCP 는 별개 바인딩으로 들어오고,
  그 연결이 죽으면 생존 판정에서 탈락한다 — **규칙 ①②의 추측이 필요 없어진다.**
- 서버 발신은 **살아있는 바인딩**을 고른다.

### 2.1 생존 판정

| transport | 판정 | 근거 |
|---|---|---|
| TCP / TLS | psip 소켓맵에 그 (ip,port) 연결이 있는가 | 연결이 닫히면 맵에서 제거된다(`TcpSessionList`) |
| UDP | `last-seen` 이 keepalive 주기 안인가 | 연결 개념이 없으므로 최근 수신으로 판정 |

psip 은 소켓맵 조회 API(`CTcpSocketMap::Select`)를 이미 갖고 있다. 응용이 쓸 수 있게
**질의 함수 하나만 노출**하면 된다 — 연결 핸들을 응용까지 전달할 필요가 없다.

### 2.2 선택 정책 (서버 발신)

1. 살아있는 바인딩 중 **가장 최근에 갱신된 것** 하나
2. 살아있는 것이 없으면 마지막 바인딩(현 동작과 동일 — 도달 실패하지만 무해)

멀티 디바이스를 지원하지 않으므로 **병렬 포크는 하지 않는다**. 사람당 leg 하나가 유지된다.

## 3. 왜 소비자 26곳을 건드리지 않는가

`CUserMap` 의 공개 API 는 이미 "이 사용자에게 보낼 정보 하나를 달라"는 형태다.

```cpp
bool Select( const char *pszUserId, CUserInfo &clsInfo );   // 26곳이 이것을 쓴다
```

**시그니처를 유지하고 내부에서 최적 바인딩을 골라 반환**하면, fan-out INVITE·NOTIFY 2종·
세션 갱신(`EventGetLegDest`)·MSRP·라우팅 등 소비자 전부가 무변경이다. 구조 교체가
`UserMap` 안에 갇힌다.

| API | 변경 |
|---|---|
| `Select(id, info)` | 내부에서 살아있는 최적 바인딩 선택 (시그니처 불변) |
| `Select(id)` | 바인딩이 하나라도 있는가 (의미 불변) |
| `Insert(msg, user)` | 덮어쓰기 → **바인딩 추가/갱신**. 규칙 ①② 제거 |
| `SetIpPort(...)` | 그 flow 의 바인딩 갱신으로 축소 (또는 제거) |
| `TouchFlow(...)` | 해당 바인딩의 last-seen 갱신 |
| `DeleteTimeout(...)` | **바인딩 단위 만료** — 마지막 바인딩이 사라질 때 등록 해제로 통지 |
| `SendOptions()` | 살아있는 바인딩 대상 (UDP 바인딩 유지 목적) |
| `GetString()` | 바인딩 목록 표시 (운영 조회) |

## 4. reg-event(RFC 3680) 정합

reg-event NOTIFY 는 원래 **contact 목록** 모델이다. 지금은 바인딩이 하나뿐이라 단일 contact 로
내보내고 있는데, 바인딩 집합이 되면 각 바인딩이 `<contact>` 하나로 자연히 대응된다 —
구조가 규격에 가까워진다.

## 5. 이행 경로

각 단계는 그 단계 끝에서 검증 가능해야 한다.

| 단계 | 작업 | 통과 조건 |
|---|---|---|
| **1. flow 생존 질의** | psip 에 `(ip,port,transport)` 연결 생존 질의 노출 | 동작 무변 (조회만 추가) |
| **2. 바인딩 집합화** | `CUserMap` 내부를 집합으로. 선택 정책 = 살아있는 최근 1개 | **동작 무변** — 오늘과 같은 결과. 회귀 0 이 조건 |
| **3. 규칙 ①② 제거** | 생존 판정이 대신하므로 transport 추측 삭제 | 승격 TCP 오염 0 · 전환 정상 (실측 재현) |
| **4. 만료·통지 정합** | 바인딩 단위 만료, reg-event contact 목록화 | 등록 해제 통지 시점이 마지막 바인딩 소멸과 일치 |

## 6. 검증

`cspsim -transport {udp,tcp,tls}` 로 **한 계정을 여러 flow 로** 등록시켜 바인딩 집합을 만든다
(실기기 2대로는 만들기 어려운 조합).

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | UDP 등록 후 같은 계정 TLS 등록 | 바인딩 2개, 발신은 최근(TLS) |
| 2 | TLS 연결을 강제 종료 | 그 바인딩이 생존 판정에서 탈락, 발신이 UDP 로 폴백 |
| 3 | 대형 INVITE 로 TCP 승격 유발 | 승격 flow 가 별개 바인딩으로 들어오고 **등록 flow 를 덮지 않음** |
| 4 | 모든 바인딩 만료 | 등록 해제 통지 1회 |
| 5 | 실기기 회귀 (그룹콜·NOTIFY·세션 갱신) | 오늘과 동일 동작 |

## 7. 멀티 디바이스는 범위 밖 (B안)

한 계정으로 여러 단말을 동시에 쓰려면 서버가 **기기를 구분**해야 한다. RFC 5626 은 그 수단으로
`+sip.instance`(instance-id) + `reg-id` 를 규정하고, **우리 단말은 이미 그 파라미터를 보낸다**
— 그러나 값이 기기 고유가 아니다.

pjsip 은 instance-id 기본값을 **호스트명 해시**로 만든다
(`pjsua_acc.c: init_outbound_setting` — `pj_hash_calc(hostname)` 4바이트를 UUID 꼬리에 넣는다).
Android 기기의 호스트명은 관례적으로 `localhost` 이므로 **모든 단말이 같은 값**을 보낸다.

```
관측: urn:uuid:00000000-0000-0000-0000-0000e922f243   (양 단말 동일)
계산: pj_hash("localhost") = 43f222e9 → 리틀엔디안 기록 → e922f243   ✅ 일치
```

따라서 `(AoR, instance-id, reg-id)` 를 바인딩 키로 쓰면 **서로 다른 기기가 하나로 합쳐진다.**
멀티 디바이스를 지원하려면 앱이 `rfc5626_instance_id` 를 기기 고유값(ANDROID_ID·설치 UUID
등)으로 명시하는 것이 **선행 조건**이고, 그 위에 PTT fan-out 정책(기기당 leg 를 만들 것인가 —
floor 정원·녹취 슬롯·CMP 멤버 포트에 영향)을 정해야 한다. 별도 과제로 둔다.
