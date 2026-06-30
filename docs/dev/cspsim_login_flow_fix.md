# cspsim 로그인 순서 수정 계획

## 현재 cspsim 순서 (잘못된 순서)

```
1. SIP REGISTER         (CSP, Digest auth)
2. SIP SUBSCRIBE GMS    (CSP → xcap-diff)
3. SIP SUBSCRIBE CMS    (CSP → xcap-diff)
4. SIP PUBLISH          (affiliation, CSP)
   ↕ (비동기 — NOTIFY 수신 시점에 트리거됨)
5. NOTIFY 수신 → IdMS auth (HTTP PKCE → CSC)
                → XCAP doc fetch (HTTP → CSC)
6. PTT GROUP INVITE     (SIP → CSP)
7. FLOOR CONTROL
```

**문제:** IdMS 인증이 REGISTER 이후, NOTIFY 수신이라는 비동기 이벤트에 의해 트리거됨.  
시스템 설계상 IdMS 인증은 REGISTER 전에 완료되어야 함.

---

## 올바른 순서 (CSP 설계 기준)

```
1. IdMS auth            (HTTP PKCE → CSC)  ← REGISTER 전에 먼저
2. SIP REGISTER         (CSP, Digest auth)
3. SIP SUBSCRIBE GMS    (CSP → xcap-diff)
4. SIP SUBSCRIBE CMS    (CSP → xcap-diff)
5. NOTIFY 수신 → XCAP doc fetch (HTTP → CSC, 이미 취득한 토큰 재사용)
6. SIP PUBLISH          (affiliation, CSP)
7. PTT GROUP INVITE     (SIP → CSP)
8. FLOOR CONTROL
```

---

## 수정 범위

### 1. `cspsim/CspsimMain.cpp` — CLI 파라미터 추가

```
-csc_ip <IP>     CSC 서버 IP (IdMS/XCAP 서버)
-csc_port <N>    CSC McpttServer 포트 (기본: 4530)
-csc_tls         CSC TLS 사용 여부 (기본: false, 테스트 환경)
```

### 2. `cspsim/SimSession.h` — 멤버 추가

```cpp
std::string m_strCscHost;   // CSC IP
int         m_iCscPort;     // CSC port (default 4530)
bool        m_bCscTls;      // TLS 여부
```

`SetCscHost(host, port, tls)` 메서드 추가.

### 3. `cspsim/SimSession.cpp` — `Start()` 수정

SIP 스택 `m_clsUserAgent.Start()` 호출 **전**에 IdMS 인증 삽입:

```cpp
bool SimSession::Start() {
    // ★ 추가: REGISTER 전 IdMS 인증
    if (!m_strCscHost.empty()) {
        if (!AcquireXcapToken(m_strCscHost, m_iCscPort, m_bCscTls)) {
            printf("[%d] IdMS auth failed — abort\n", m_iId);
            return false;
        }
        printf("[%d] IdMS auth OK\n", m_iId);
    }

    // 기존: SIP 스택 시작 (REGISTER 포함)
    if (!m_clsUserAgent.Start(m_clsSetup, m_pSipClient)) { ... }
    ...
}
```

### 4. `cspsim/SimSession.cpp` — `FetchXcapDoc()` 수정

NOTIFY 트리거 시 이미 토큰이 있으면 `AcquireXcapToken()` 재호출 스킵:

```cpp
void SimSession::FetchXcapDoc(...) {
    // 토큰이 없을 때만 취득 (Start()에서 이미 취득했으면 재사용)
    if (m_strXcapToken.empty()) {
        if (!AcquireXcapToken(strHost, iPort, bTls)) return;
    }
    // 이후 HTTP GET 동일
}
```

---

## 실행 커맨드 (수정 후)

```bash
./bin/cspsim \
  -server_ip 127.0.0.1 \
  -count 4 \
  -user 1001 \
  -domain csp \
  -password 1234 \
  -mode ptt \
  -group 1000 \
  -scenario group_call \
  -csc_ip 127.0.0.1 \
  -csc_port 4530 \
  -call_duration 10
```

---

## 확인 필요 사항 (별도 논의)

- cims-phone은 SIP SUBSCRIBE / SIP PUBLISH(affiliation)를 하지 않음
  → 실제 단말이 SUBSCRIBE/PUBLISH를 해야 하는지 관계자 확인 필요
