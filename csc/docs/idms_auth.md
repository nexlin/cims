# CSC IDMS OAuth 2.0 인증 시스템 문서

**작성일**: 2026-02-10  
**버전**: 1.1  
**작성자**: 남광효

---

## 📋 목차

1. [인증 과정 이해하기](#인증-과정-이해하기)
2. [시스템 개요](#시스템-개요)
3. [아키텍처](#아키텍처)
4. [Config 파일 설명](#config-파일-설명)
5. [구현 상세](#구현-상세)
6. [테스트 가이드](#테스트-가이드)
7. [API 명세](#api-명세)
8. [보안 고려사항](#보안-고려사항)
9. [문제 해결](#문제-해결)

---

## 인증 과정 이해하기

### 전체 흐름도

```
┌─────────────────────────────────────────────────────────────────┐
│                    1. 인증 요청 (authreq)                        │
│  Client → CSC: PKCE 생성 + 사용자 인증 정보 전송                 │
│  응답: auth_code (10초 유효)                                     │
└────────────────────────┬────────────────────────────────────────┘
                         │ auth_code 재사용
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                    2. 토큰 교환 (tokenreq)                       │
│  Client → CSC: auth_code + code_verifier 전송                   │
│  응답: access_token (30초), refresh_token (60초), id_token      │
└────────────────────────┬────────────────────────────────────────┘
                         │ access_token 재사용
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                    3. API 호출 (GMS/CMS/KMS)                    │
│  Client → CSC: Authorization: Bearer <access_token>            │
│  - 그룹 조회/생성/삭제 (GMS)                                     │
│  - 사용자 프로필 조회 (CMS)                                      │
│  - 키 관리 (KMS)                                                │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
                    Access Token 만료?
                         │
                    ┌────┴────┐
                    │  YES    │  NO → 3번으로 돌아감
                    └────┬────┘
                         │ refresh_token 재사용
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│                4. 토큰 갱신 (tokenreq - refresh)                 │
│  Client → CSC: refresh_token 전송                               │
│  응답: 새 access_token, 새 refresh_token                        │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
                    Refresh Token 만료?
                         │
                    ┌────┴────┐
                    │  YES    │  NO → 3번으로 돌아감
                    └────┬────┘
                         │
                         ▼
                    1번부터 다시 시작
```

---

### 단계별 상세 설명

#### 단계 1: 인증 요청 (Authorization Request)

**목적**: 사용자 인증 및 임시 인증 코드 발급

**클라이언트 동작**:
```python
# 1. PKCE 생성
code_verifier = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode('utf-8').rstrip('=')
# 예: "Qb2sMUn7E6CfxvnBsd_4TzK8..."

code_challenge = base64.urlsafe_b64encode(
    hashlib.sha256(code_verifier.encode('utf-8')).digest()
).decode('utf-8').rstrip('=')
# 예: "oEtOOSr02_j5pNl4MTNM..."

# 2. 인증 요청
GET /idms/authreq?
    client_id=MCPTT_UE&
    user_name=tel:+2001&
    user_password=1234&
    redirect_uri=http://client/cb&
    state=mystate&
    scope=openid 3gpp:mcptt:ptt_server&
    code_challenge=oEtOOSr02_j5pNl4MTNM...&
    code_challenge_method=S256
```

**서버 응답**:
```json
{
  "Location": "http://client/cb",
  "code": "7b0d2986-6823-465e-8193-3158da5215f7",  // ← 이 값을 저장!
  "state": "mystate"
}
```

**중요**: 
- `code` (auth_code)를 **반드시 저장**하여 다음 단계에서 사용
- `code_verifier`도 **반드시 저장** (다음 단계에서 필요)
- **유효 시간**: 10초 (테스트용) / 60초 (프로덕션)
- **1회성**: 한 번 사용하면 삭제됨

---

#### 단계 2: 토큰 교환 (Token Exchange)

**목적**: 인증 코드를 실제 사용 가능한 토큰으로 교환

**클라이언트 동작**:
```python
# 1단계에서 받은 값 재사용
auth_code = "7b0d2986-6823-465e-8193-3158da5215f7"  # 1단계에서 저장한 값(서버로 부터 수신한 값)
code_verifier = "Qb2sMUn7E6CfxvnBsd_4TzK8..."      # 1단계에서 저장한 값(1단계에서 클라이언트가 생성했던 값)

# 토큰 요청
POST /idms/tokenreq
Content-Type: application/json

{
  "grant_type": "authorization_code",
  "code": "7b0d2986-6823-465e-8193-3158da5215f7",     // ← 1단계에서 받은 값
  "code_verifier": "Qb2sMUn7E6CfxvnBsd_4TzK8...",    // ← 1단계에서 저장한 값
  "redirect_uri": "http://client/cb",
  "client_id": "MCPTT_UE"
}
```

**서버 응답**:
```json
{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",  // ← API 호출에 사용
  "refresh_token": "76648ec2-6cb0-4a30-af36-9bd55c42c649",  // ← 갱신에 사용
  "id_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "token_type": "Bearer",
  "expires_in": 30  // 30초 후 만료
}
```

**중요**:
- `access_token`을 **저장**하여 모든 API 호출에 사용
- `refresh_token`을 **저장**하여 토큰 갱신에 사용
- **유효 시간**: 
  - access_token: 30초 (테스트용) / 1시간 (프로덕션)
  - refresh_token: 60초 (테스트용) / 7일 (프로덕션)

---

#### 단계 3: API 호출 (그룹 조회, 프로필 조회 등)

**목적**: 실제 서비스 API 사용

**클라이언트 동작**:
```python
# 2단계에서 받은 access_token 재사용
access_token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9..."  # 2단계에서 저장한 값

# 3-1. 그룹 조회 (GMS)
GET /org.openmobilealliance.groups/users/tel:+2001/tel:+2000
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...  // ← 2단계에서 받은 값

# 3-2. 그룹 생성 (GMS)
PUT /org.openmobilealliance.groups/users/tel:+2001/tel:+2000
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
Content-Type: application/json

{
  "uri": "tel:+2000",
  "display-name": "Test Group"
}

# 3-3. 사용자 프로필 조회 (CMS)
GET /org.3gpp.mcptt.user-profile/users/tel:+2001/user-profile
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...

# 3-4. 서비스 설정 조회 (CMS)
GET /org.3gpp.mcptt.service-config/users/tel:+2001/service-config
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...

# 3-5. KMS 초기화 (KMS)
POST /keymanagement/identity/v1/init
Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
Content-Type: application/json

{
  "UserUri": "tel:+2001@ps-lte.com",
  "ClientId": "MCPTT_UE"
}
```

**서버 응답**:
- **성공**: 200 OK + 요청한 데이터 (XML 또는 JSON)
- **토큰 만료**: 401 Unauthorized → **4단계로 이동**
- **권한 없음**: 403 Forbidden

**중요**:
- 모든 API 호출에 **동일한 access_token** 재사용
- 401 응답 받으면 → 4단계 (토큰 갱신)로 이동
- 403 응답 받으면 → 권한 문제 (재인증 필요 없음)

---

#### 단계 4: 토큰 갱신 (Token Refresh)

**목적**: 만료된 access_token을 새로 발급받기

**발생 시점**:
- API 호출 시 401 Unauthorized 응답 받음
- 또는 `expires_in` 시간 경과 확인

**클라이언트 동작**:
```python
# 2단계에서 받은 refresh_token 재사용
refresh_token = "76648ec2-6cb0-4a30-af36-9bd55c42c649"  # 2단계에서 저장한 값

# 토큰 갱신 요청
POST /idms/tokenreq
Content-Type: application/json

{
  "grant_type": "refresh_token",
  "refresh_token": "76648ec2-6cb0-4a30-af36-9bd55c42c649",  // ← 2단계에서 받은 값
  "client_id": "MCPTT_UE"
}
```

**서버 응답 (성공)**:
```json
{
  "access_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",  // ← 새 토큰 (저장!)
  "refresh_token": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",  // ← 새 토큰 (저장!)
  "id_token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "token_type": "Bearer",
  "expires_in": 30
}
```

**서버 응답 (실패 - refresh_token 만료)**:
```json
{
  "error": "invalid_grant"
}
```

**중요**:
- **성공 시**: 
  - 새 `access_token`과 `refresh_token`을 **저장** (기존 값 덮어쓰기)
  - **3단계로 돌아가서** API 호출 재시도
  - 기존 refresh_token은 **자동으로 무효화됨** (Rotation)
- **실패 시**: 
  - **1단계부터 다시 시작** (전체 재인증)

---

### 만료 시나리오 및 대응

#### 시나리오 1: Access Token 만료 (정상 흐름)

```
시간: 0초
  → 1단계: 인증 요청 → auth_code 발급
  → 2단계: 토큰 교환 → access_token, refresh_token 발급
  → 3단계: API 호출 (성공)

시간: 35초 (access_token 만료: 30초)
  → 3단계: API 호출 → 401 Unauthorized
  → 4단계: 토큰 갱신 (refresh_token 사용) → 새 access_token, refresh_token, id_token 발급
  → 3단계: API 호출 재시도 (성공)
```

**대응**: 자동으로 4단계 실행 → 3단계 재시도

---

#### 시나리오 2: Refresh Token 만료 (재인증 필요)

```
시간: 0초
  → 1단계: 인증 요청 → auth_code 발급
  → 2단계: 토큰 교환 → access_token, refresh_token 발급
  → 3단계: API 호출 (성공)

시간: 35초
  → 4단계: 토큰 갱신 → 새 access_token, refresh_token, id_token 발급
  → 3단계: API 호출 (성공)

시간: 65초 (refresh_token 만료: 60초)
  → 3단계: API 호출 → 401 Unauthorized
  → 4단계: 토큰 갱신 시도 → 400 Bad Request (invalid_grant)
  → 1단계부터 다시 시작 (전체 재인증)
```

**대응**: 1단계부터 전체 재인증

---

#### 시나리오 3: Authorization Code 만료

```
시간: 0초
  → 1단계: 인증 요청 → auth_code 발급

시간: 15초 (auth_code 만료: 10초)
  → 2단계: 토큰 교환 시도 → 400 Bad Request (invalid_grant)
  → 1단계부터 다시 시작
```

**대응**: 1단계부터 다시 시작 (auth_code는 10초 내에 사용해야 함)

---

### 데이터 재사용 요약표

| 단계 | 받는 데이터 | 저장 필요 | 다음 단계에서 사용 | 유효 시간 |
|------|------------|----------|------------------|----------|
| **1. 인증 요청** | `code` (auth_code) | ✅ | 2단계 tokenreq | 10초 |
| | `code_verifier` (생성) | ✅ | 2단계 tokenreq | - |
| **2. 토큰 교환** | `access_token` | ✅ | 3단계 API 호출 | 30초 |
| | `refresh_token` | ✅ | 4단계 토큰 갱신 | 60초 |
| | `id_token` | 선택 | (사용자 정보 확인용) | 30초 |
| **3. API 호출** | API 응답 데이터 | 필요시 | - | - |
| **4. 토큰 갱신** | 새 `access_token` | ✅ | 3단계 API 호출 | 30초 |
| | 새 `refresh_token` | ✅ | 다음 4단계 | 60초 |
| | 새 `id_token` | 선택 | (사용자 정보 확인용) | 30초 |

---

### 테스트 시나리오 예시

#### 예시 1: 정상 흐름 (만료 없음)

```python
# 1. 인증 요청
code, code_verifier = authenticate()  # code 저장

# 2. 토큰 교환
tokens = exchange_token(code, code_verifier)  # access_token, refresh_token 저장

# 3. API 호출 (10초 이내)
profile = get_user_profile(tokens['access_token'])  # 성공
groups = get_groups(tokens['access_token'])  # 성공
```

---

#### 예시 2: Access Token 만료 후 갱신

```python
# 1-2. 인증 및 토큰 교환
tokens = full_authentication()

# 3. API 호출
profile = get_user_profile(tokens['access_token'])  # 성공

# 35초 대기 (access_token 만료)
time.sleep(35)

# 3. API 호출 시도
try:
    groups = get_groups(tokens['access_token'])  # 401 Unauthorized
except Unauthorized:
    # 4. 토큰 갱신
    tokens = refresh_tokens(tokens['refresh_token'])  # 새 토큰 저장
    
    # 3. API 호출 재시도
    groups = get_groups(tokens['access_token'])  # 성공
```

---

#### 예시 3: Refresh Token 만료 후 재인증

```python
# 1-2. 인증 및 토큰 교환
tokens = full_authentication()

# 65초 대기 (refresh_token 만료)
time.sleep(65)

# 3. API 호출 시도
try:
    groups = get_groups(tokens['access_token'])  # 401 Unauthorized
except Unauthorized:
    # 4. 토큰 갱신 시도
    try:
        tokens = refresh_tokens(tokens['refresh_token'])  # 400 Bad Request
    except BadRequest:
        # 1-2. 전체 재인증
        tokens = full_authentication()
        
        # 3. API 호출 재시도
        groups = get_groups(tokens['access_token'])  # 성공
```

---

## 시스템 개요

### 목적

CSC (Common Service Core) IDMS (Identity Management System)는 MCPTT (Mission Critical Push To Talk) 시스템의 OAuth 2.0 기반 인증 및 권한 관리를 담당합니다.

### 주요 기능

- ✅ **OAuth 2.0 Authorization Code Flow** (PKCE 필수)
- ✅ **Refresh Token Rotation** (보안 강화)
- ✅ **토큰 영속성 관리** (파일 기반 저장)
- ✅ **토큰 만료 관리** (TTL 기반)
- ✅ **API 인증** (Bearer Token)
- ✅ **HTTPS 보안 통신** (TLS 1.2+ 강제)

### 기술 스택

- **언어**: Python 3.12
- **프레임워크**: aiohttp (비동기 HTTP)
- **인증**: JWT (JSON Web Token)
- **저장소**: JSON 파일 (파일 락 사용)

---

## 아키텍처

### 전체 구조

```
┌─────────────────────────────────────────────────────────────┐
│                        Client (UE)                          │
│                    (test_csc_http.py)                       │
└─────────────────────┬───────────────────────────────────────┘
                      │
                      │ HTTPS (4420)
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                     CSC Server                              │
│                                                             │
│  ┌────────────────────────────────────────────────────┐    │
│  │              app.py (진입점)                       │    │
│  │  - 서버 초기화                                      │    │
│  │  - SSL 설정                                        │    │
│  │  - 라우팅 설정                                      │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
│  ┌──────────────────▼─────────────────────────────────┐    │
│  │          csc_service.py (요청 처리)               │    │
│  │                                                    │    │
│  │  요청별 분기:                                       │    │
│  │  ┌──────────────────────────────────────────┐    │    │
│  │  │ /idms/authreq    → handle_auth_req()    │    │    │
│  │  │ /idms/tokenreq   → handle_token_req()   │    │    │
│  │  │ /org.openmobilealliance.groups/*        │    │    │
│  │  │                  → handle_group_*()     │    │    │
│  │  │ /org.3gpp.mcptt.user-profile/*         │    │    │
│  │  │                  → handle_user_profile()│    │    │
│  │  │ /keymanagement/* → handle_kms_*()      │    │    │
│  │  └──────────────────────────────────────────┘    │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
│  ┌──────────────────▼─────────────────────────────────┐    │
│  │          idms_storage.py (영속성 관리)            │    │
│  │  - 파일 락 (fcntl)                                │    │
│  │  - 원자적 저장                                     │    │
│  │  - 만료 데이터 정리                                │    │
│  └──────────────────┬─────────────────────────────────┘    │
│                     │                                       │
│  ┌──────────────────▼─────────────────────────────────┐    │
│  │              Persistent Storage                    │    │
│  │  ┌────────────────┐  ┌────────────────────────┐  │    │
│  │  │ auth_codes.json│  │ refresh_tokens.json    │  │    │
│  │  └────────────────┘  └────────────────────────┘  │    │
│  │  ┌────────────────┐                              │    │
│  │  │ .idms.lock     │  (파일 락)                   │    │
│  │  └────────────────┘                              │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 요청 처리 흐름

```
Client Request
      │
      ▼
   app.py (진입점)
      │
      ├─ SSL/TLS 검증
      ├─ 라우팅 매칭
      │
      ▼
csc_service.py (요청별 분기)
      │
      ├─ /idms/authreq      → handle_auth_req()
      ├─ /idms/tokenreq     → handle_token_req()
      ├─ /org.openmobilealliance.groups/* → handle_group_management()
      ├─ /org.3gpp.mcptt.user-profile/*   → handle_user_profile()
      └─ /keymanagement/*   → handle_kms_*()
      │
      ▼
  토큰 검증 (extract_token)
      │
      ├─ JWT 디코딩
      ├─ 만료 확인
      ├─ 서명 검증
      │
      ▼
  비즈니스 로직 처리
      │
      ├─ idms_storage.py (데이터 저장/조회)
      │   ├─ 파일 락 획득
      │   ├─ JSON 파일 읽기/쓰기
      │   └─ 파일 락 해제
      │
      ▼
   Response
```

### 파일 구조

```text
csc/bin/csc_pihttp/
├── config/
│   └── csc.json                  # 서비스 설정 (사용자, DB 등)
└── src/                          # (코드 실행 경로)
    ├── app.py                    # 서버 진입점 (라우팅, SSL 설정)
    ├── csc_service.py            # 메인 서비스 로직 (요청별 핸들러)
    ├── idms_storage.py           # MariaDB 연동 저장소 모듈
    ├── cleanup_idms.py           # 데이터 정리 스크립트
    ├── test_csc_http.py          # 통합 테스트 (토큰 캐싱 포함)
    ├── server.key                # SSL 개인키 (필수)
    └── server.crt                # SSL 인증서 (필수)
```

---

## Config 파일 설명

### 1. 서비스 설정 (`csc/bin/csc_pihttp/config/csc.json`)

MariaDB 연결 정보를 포함하도록 설정 파일을 관리합니다.

```json
{
    "Server": {
        "Ip": "0.0.0.0",
        "Port": 4420
    },
    "Database": {
        "Host": "localhost",
        "User": "agapeoom",
        "Password": "!core0908",
        "Db": "csc_idms"
    }
}
```

| 항목 | 설명 | 예시 값 | 비고 |
|------|------|---------|------|
| `Server.Ip` | 서버 바인딩 주소 | `"0.0.0.0"` | 모든 인터페이스에서 수신 |
| `Server.Port` | 서버 포트 | `4420` | HTTPS 포트 |
| `Database.Host` | MariaDB 호스트 | `"localhost"` | |
| `Database.User` | MariaDB 사용자명 | `"agapeoom"` | |
| `Database.Password` | MariaDB 비밀번호 | `"내비밀번호"` | |
| `Database.Db` | MariaDB 데이터베이스명 | `"csc_idms"` | |
| `idms.issuer` | JWT 발급자 | `"idms.mcptt.com"` | ID Token의 iss 필드 |
| `idms.secret_key` | JWT 서명 키 | `"your-secret-key"` | 256비트 이상 권장 |
| `users[].id` | 사용자 ID | `"tel:+2001"` | URI 형식 |
| `users[].password` | 사용자 비밀번호 | `"1234"` | 평문 (개발용) |
| `users[].name` | 사용자 이름 | `"User 2001"` | 표시용 |

---

### 2. 저장소 구조 (MariaDB)

기존 JSON 방식에서 MariaDB로 전환하여 무결성과 동시성을 보장합니다.

#### [auth_codes] 테이블
- `code` (PK): 인증 코드
- `user_id`: 사용자 URI
- `used`: 사용 여부 (0:미사용, 1:사용)
- `expires_at`: 만료 시간
- `client_id`: 클라이언트 ID
- `redirect_uri`: 리다이렉트 URI
- `scope`: 권한 범위
- `state`: CSRF 방지 토큰
- `issued_at`: 발급 시간
- `code_challenge`: PKCE 챌린지
- `code_challenge_method`: PKCE 방식

#### [refresh_tokens] 테이블
- `token_id` (PK): 리프레시 토큰 ID
- `user_id`: 사용자 URI
- `revoked`: 폐기 여부
- `rotated_to`: 갱신된 새로운 토큰 ID
- `client_id`: 발급받은 클라이언트
- `scope`: 권한 범위
- `issued_at`: 발급 시간
- `expires_at`: 만료 시간

> [!NOTE]
> MariaDB의 트랜잭션 기능을 사용하므로 별도의 파일락(`.idms.lock`)이 필요하지 않습니다.

### 3. TTL 설정 (`csc_service.py`)

| 항목 | 설명 | 테스트 값 | 프로덕션 권장 값 | 비고 |
|------|------|-----------|-----------------|------|
| `AUTH_CODE_TTL` | Authorization Code 유효 시간 | `10` (10초) | `60` (60초) | OAuth 2.0 권장: 10분 이하 |
| `ACCESS_TOKEN_TTL` | Access Token 유효 시간 | `30` (30초) | `3600` (1시간) | API 호출 빈도에 따라 조정 |
| `REFRESH_TOKEN_TTL` | Refresh Token 유효 시간 | `60` (60초) | `604800` (7일) | 보안 정책에 따라 조정 |

**설정 위치**: `csc_service.py` 라인 32-34

**변경 방법**:
```python
# 테스트용 (현재)
AUTH_CODE_TTL = 10
ACCESS_TOKEN_TTL = 30
REFRESH_TOKEN_TTL = 60

# 프로덕션용 (주석 해제)
# AUTH_CODE_TTL = 60
# ACCESS_TOKEN_TTL = 3600
# REFRESH_TOKEN_TTL = 7 * 24 * 3600
```

---

### 3. 영속성 저장 파일

#### 3-1. 토큰 캐시 (`data/ptt_token_config.json`)

| 필드 | 설명 | 예시 값 | 비고 |
|------|------|---------|------|
| `access_token` | Access Token (JWT) | `"eyJhbGci..."` | API 호출에 사용 |
| `refresh_token` | Refresh Token (UUID) | `"76648ec2-6cb0..."` | 갱신에 사용 |
| `id_token` | ID Token (JWT) | `"eyJhbGci..."` | 사용자 정보 확인용 |
| `expires_at` | 만료 시간 | `1770709065` | Unix timestamp |
| `user_id` | 사용자 ID | `"tel:+2001"` | 토큰 소유자 |
| `client_id` | 클라이언트 ID | `"MCPTT_UE"` | 발급받은 클라이언트 |
| `scope` | 권한 범위 | `"openid 3gpp:mcptt:ptt_server"` | 공백으로 구분 |

**용도**: 테스트 시 토큰 재사용 (빠른 테스트)  
**관리**: 만료 시 자동 갱신, 수동 삭제로 전체 재인증

**관리**: 만료 시 자동 갱신, 수동 삭제로 전체 재인증

---

#### 3-2. 그룹 정보 저장 (`csp/dist/Group/`)

**위치**: `/home/agapeoom/cims/csp/dist/Group/{group_id}.json`
**용도**: CSC와 CSP 간 그룹 정보 공유 (CSC: Write, CSP: Read)

| 필드 | 설명 | 예시 값 | 비고 |
|------|------|---------|------|
| `name` | 그룹 표시 이름 | `"Test Group"` | |
| `etag` | 버전 관리용 태그 | `"etag_170755..."` | 변경 감지용 |
| `created_by` | 생성자 URI | `"tel:+2001"` | |
| `created_at` | 생성 시간 | `"2026-02-10..."` | ISO 8601 |
| `users` | 멤버 목록 | `[...]` | 배열 |
| `users[].id` | 멤버 ID | `"2001"` | **tel:+ 접두사 없음** |
| `users[].priority` | 우선순위 | `5` | 0~7 (낮을수록 높음) |
| `users[].role` | 역할 | `"owner"` | owner, participant |
| `users[].joined_at` | 참여 시간 | `"2026-02-10..."` | |

**파일 예시**:
```json
{
    "name": "Test Group 2000",
    "created_by": "tel:+2001",
    "created_at": "2026-02-10T16:27:17",
    "etag": "etag_1707556037",
    "users": [
        {
            "id": "2001",
            "priority": 5,
            "role": "owner",
            "joined_at": "2026-02-10T16:27:17"
        }
    ]
}
```

---

## 구현 상세

### 1. OAuth 2.0 Authorization Code Flow (PKCE)

#### 1-1. 인증 요청 (`/idms/authreq`)

**처리 로직** (`csc_service.py:handle_auth_req`):

```python
# 1. PKCE 필수 검증
if not code_challenge:
    return 400 "code_challenge is required (PKCE mandatory)"

if code_challenge_method != 'S256':
    return 400 "only S256 is supported"

# 2. 사용자 인증
if user_name not in USERS or password != USERS[user_name]['password']:
    return 302 redirect with error

# 3. Authorization Code 생성
code = str(uuid.uuid4())
auth_data = {
    "user_id": user_name,
    "client_id": client_id,
    "redirect_uri": redirect_uri,
    "scope": scope,
    "state": state,
    "issued_at": now,
    "expires_at": now + AUTH_CODE_TTL,  # 10초 (테스트용)
    "code_challenge": code_challenge,
    "code_challenge_method": code_challenge_method
}

# 4. 영속성 저장
storage.save_auth_code(code, auth_data)

# 5. 응답
return {
    "Location": redirect_uri,
    "code": code,
    "state": state
}
```

---

#### 1-2. 토큰 요청 (`/idms/tokenreq`)

**처리 로직** (`csc_service.py:handle_token_req`):

```python
# 1. Authorization Code 조회
auth_data = storage.get_auth_code(code)
if not auth_data:
    return 400 "invalid_grant"

# 2. 만료 확인
if now > auth_data["expires_at"]:
    storage.delete_auth_code(code)
    return 400 "invalid_grant"

# 3. client_id 검증
if auth_data["client_id"] != client_id:
    return 400 "invalid_grant"

# 4. redirect_uri 검증
if auth_data["redirect_uri"] != redirect_uri:
    return 400 "invalid_grant"

# 5. PKCE 검증 (필수)
if "code_challenge" not in auth_data:
    return 400 "PKCE required"

if not code_verifier:
    return 400 "code_verifier required"

# SHA256 해시 검증
computed_challenge = base64.urlsafe_b64encode(
    hashlib.sha256(code_verifier.encode()).digest()
).decode().rstrip('=')

if computed_challenge != auth_data["code_challenge"]:
    return 400 "PKCE verification failed"

# 6. 토큰 발급
id_token, access_token, refresh_token = create_tokens(
    user_id, scope, client_id
)

# 7. Authorization Code 삭제 (1회성)
storage.delete_auth_code(code)

# 8. 응답
return {
    "access_token": access_token,
    "refresh_token": refresh_token,
    "id_token": id_token,
    "token_type": "Bearer",
    "expires_in": 30  # 30초 (테스트용)
}
```

---

### 2. Refresh Token Rotation

**처리 로직**:

```python
# 1. Refresh Token 조회
token_data = storage.get_refresh_token(refresh_token)
if not token_data:
    return 400 "invalid_grant"

# 2. revoked 확인
if token_data.get("revoked", False):
    return 400 "invalid_grant"

# 3. 만료 확인
if now > token_data.get("expires_at", 0):
    storage.revoke_refresh_token(refresh_token)
    return 400 "invalid_grant"

# 4. 새 토큰 발급
id_token, access_token, new_refresh_token = create_tokens(
    token_data["user_id"],
    token_data["scope"],
    client_id
)

# 5. 기존 Refresh Token 회전 (Rotation)
storage.rotate_refresh_token(refresh_token, new_refresh_token)

# 6. 응답
return {
    "access_token": access_token,
    "refresh_token": new_refresh_token,
    "id_token": id_token,
    "token_type": "Bearer",
    "expires_in": 30
}
```

---

### 3. API 인증

**검증 로직** (`csc_service.py:extract_token`):

```python
def extract_token(auth_header: str):
    if not auth_header:
        return None
    
    token = auth_header.replace('Bearer ', '')
    
    try:
        # JWT 검증 (만료 시간 포함)
        payload = jwt.decode(
            token,
            SECRET_KEY,
            algorithms=["HS256"],
            options={"verify_signature": True},
            audience="mcptt_client"
        )
        return payload
    except jwt.ExpiredSignatureError:
        logger.log_error("Token expired")
        return None
    except jwt.InvalidTokenError as e:
        logger.log_error(f"Invalid token: {e}")
        return None
```

---

## 테스트 가이드

### 1. 서버 시작

```bash
cd /home/agapeoom/cims/csc/bin/csc_pihttp/src
python3 app.py
```

---

### 2. 통합 테스트 실행

```bash
python3 test_csc_http.py
```

**테스트 항목**:
- ✅ PKCE 인증
- ✅ 토큰 발급
- ✅ GMS (그룹 관리) API
- ✅ CMS (설정 관리) API
- ✅ KMS (키 관리) API

---

### 3. 토큰 만료 테스트

#### 3-1. Access Token 만료 테스트

```bash
# 1. 첫 실행 (토큰 발급)
python3 test_csc_http.py

# 2. 35초 대기 (ACCESS_TOKEN_TTL: 30초)
sleep 35

# 3. 재실행 (Refresh Token으로 갱신)
python3 test_csc_http.py
```

**예상 결과**:
```
📁 Found cached token
✅ Token is valid (time-based), verifying...
⚠️  Token expired (401), refreshing...

============================================================
🔄 Refreshing Token
============================================================
✅ Token refreshed successfully
   Old refresh_token: f4fb80d8-b762-42d1-bc4d-426fdc855507
   New refresh_token: 76648ec2-6cb0-4a30-af36-9bd55c42c649
```

---

#### 3-2. Refresh Token 만료 테스트

```bash
# 1. 첫 실행 (토큰 발급)
python3 test_csc_http.py

# 2. 65초 대기 (REFRESH_TOKEN_TTL: 60초)
sleep 65

# 3. 재실행 (전체 재인증)
python3 test_csc_http.py
```

**예상 결과**:
```
📁 Found cached token
✅ Token is valid (time-based), verifying...
⚠️  Token expired (401), refreshing...

============================================================
🔄 Refreshing Token
============================================================
❌ Refresh failed: 400

============================================================
🔐 Full Authentication (PKCE)
============================================================
✅ Authentication successful
```

---

### 4. 토큰 캐시 초기화

```bash
rm data/ptt_token_config.json
python3 test_csc_http.py
```

---

### 5. 데이터 정리

```bash
python3 cleanup_idms.py
```

**정리 항목**:
- 만료된 Authorization Code
- 만료된 Refresh Token
- Revoked Refresh Token

---

## API 명세

### 1. IdMS API

#### 1-1. 인증 요청

```http
GET /idms/authreq?client_id=MCPTT_UE&user_name=tel:+2001&user_password=1234&redirect_uri=http://client/cb&state=mystate&scope=openid%203gpp:mcptt:ptt_server&code_challenge=oEtOOSr02_j5pNl4MTNM...&code_challenge_method=S256
```

**응답**:
```json
{
  "Location": "http://client/cb",
  "code": "7b0d2986-6823-465e-8193-3158da5215f7",
  "state": "mystate"
}
```

---

#### 1-2. 토큰 요청 (Authorization Code)

```http
POST /idms/tokenreq
Content-Type: application/json

{
  "grant_type": "authorization_code",
  "code": "7b0d2986-6823-465e-8193-3158da5215f7",
  "code_verifier": "Qb2sMUn7E6CfxvnBsd_4...",
  "redirect_uri": "http://client/cb",
  "client_id": "MCPTT_UE"
}
```

**응답**:
```json
{
  "access_token": "eyJhbGci...",
  "refresh_token": "76648ec2-6cb0-4a30-af36-9bd55c42c649",
  "id_token": "eyJhbGci...",
  "token_type": "Bearer",
  "expires_in": 30
}
```

---

#### 1-3. 토큰 요청 (Refresh Token)

```http
POST /idms/tokenreq
Content-Type: application/json

{
  "grant_type": "refresh_token",
  "refresh_token": "76648ec2-6cb0-4a30-af36-9bd55c42c649",
  "client_id": "MCPTT_UE"
}
```

**응답**: 동일

---

### 2. GMS API

#### 2-1. 그룹 조회

```http
GET /org.openmobilealliance.groups/users/tel:+2001/tel:+2000
Authorization: Bearer eyJhbGci...
```

---

#### 2-2. 그룹 생성

```http
PUT /org.openmobilealliance.groups/users/tel:+2001/tel:+2000
Authorization: Bearer eyJhbGci...
Content-Type: application/json

{
  "uri": "tel:+2000",
  "display-name": "Test Group 2000"
}
```

---

#### 2-3. 그룹 삭제

```http
DELETE /org.openmobilealliance.groups/users/tel:+2001/tel:+2000
Authorization: Bearer eyJhbGci...
```

---

### 3. CMS API

#### 3-1. 사용자 프로필 조회

```http
GET /org.3gpp.mcptt.user-profile/users/tel:+2001/user-profile
Authorization: Bearer eyJhbGci...
```

---

#### 3-2. 서비스 설정 조회

```http
GET /org.3gpp.mcptt.service-config/users/tel:+2001/service-config
Authorization: Bearer eyJhbGci...
```

---

### 4. KMS API

#### 4-1. KMS 초기화

```http
POST /keymanagement/identity/v1/init
Authorization: Bearer eyJhbGci...
Content-Type: application/json

{
  "UserUri": "tel:+2001@ps-lte.com",
  "ClientId": "MCPTT_UE"
}
```

---

#### 4-2. 키 프로비저닝

```http
POST /keymanagement/identity/v1/keyprov
Authorization: Bearer eyJhbGci...
Content-Type: application/json

{
  "UserUri": "tel:+2001@ptt.mnc031.mcc450.3gppnetwork.org",
  "ClientId": "MCPTT_UE"
}
```

---

## 보안 고려사항

### 1. PKCE (Proof Key for Code Exchange)

**목적**: Authorization Code Interception 공격 방지

**구현**:
- `code_verifier`: 43-128자 랜덤 문자열
- `code_challenge`: SHA256(code_verifier)의 Base64URL 인코딩
- `code_challenge_method`: S256 (SHA256)

**필수 적용**: 모든 Authorization Code 요청

---

### 2. Refresh Token Rotation

**목적**: Refresh Token 탈취 시 피해 최소화

**구현**:
- Refresh Token 사용 시 새 토큰 발급
- 기존 토큰 즉시 revoke
- `rotated_to` 필드로 추적

---

### 3. 토큰 만료 관리

| 토큰 | TTL (테스트) | TTL (프로덕션) |
|------|-------------|---------------|
| Authorization Code | 10초 | 60초 |
| Access Token | 30초 | 1시간 |
| Refresh Token | 60초 | 7일 |

---

### 4. HTTPS 필수

**설정**:
- TLS 1.2 이상
- 자체 서명 인증서 (개발용)
- 프로덕션: CA 발급 인증서 사용

---

## 보안 고려사항

### 1. HTTPS 암호화 통신 (TLS)

CSC 서버는 보안을 위해 **HTTPS 프로토콜을 강제**합니다. 서버 시작 시 SSL 인증서 파일이 없으면 HTTP 모드로 동작할 수 있으나, 프로덕션 환경에서는 반드시 인증서를 배치해야 합니다.

#### 1-1. HTTPS 설정 방법
`csc/bin/csc_pihttp/src/` 경로에 다음 두 파일을 배치하면 자동으로 HTTPS가 활성화됩니다.

- `server.key`: 개인키 (Private Key) - **절대 유출 금지**
- `server.crt`: 공개 인증서 (Public Certificate)

#### 1-2. TLS 핸드셰이크 과정
클라이언트가 CSC 서버에 접속할 때 다음과 같은 암호화 협상 과정을 거칩니다:

1.  **Client Hello**: 클라이언트가 지원하는 암호화 방식(Cipher Suite) 등을 보냄
2.  **Server Hello**: 서버가 암호화 방식을 선택하고 **인증서(server.crt)**를 보냄
3.  **Key Exchange**: 클라이언트가 서버의 인증서를 검증하고, 세션 키를 생성하여 서버의 공개키로 암호화해 보냄
4.  **Secure Connection**: 이후 모든 데이터는 세션 키로 암호화되어 전송됨

#### 1-3. 인증서 생성 (Self-Signed)
배포 환경에 인증서가 없는 경우, `openssl`을 사용하여 자가 서명 인증서를 생성할 수 있습니다.

```bash
# 개인키(server.key)와 인증서(server.crt) 생성 (유효기간 365일)
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt -days 365 -nodes
```

- `-nodes`: 개인키에 비밀번호를 걸지 않음 (서버 자동 시작을 위해 필요)
- `server.key`: 생성된 개인키 (보안 주의)
- `server.crt`: 생성된 인증서 (클라이언트에 배포 가능)

### 2. 키 관리 (KMS)
... (기존 내용)

## 문제 해결

### 1. 토큰 만료 오류

**증상**:
```
401 Unauthorized
Missing or Invalid Token
```

**해결**:
```bash
# 토큰 캐시 삭제 후 재인증
rm data/ptt_token_config.json
python3 test_csc_http.py
```

---

### 2. PKCE 검증 실패

**증상**:
```json
{
  "error": "invalid_grant",
  "error_description": "PKCE verification failed"
}
```

**원인**:
- `code_verifier`와 `code_challenge` 불일치
- `code_challenge_method`가 S256이 아님

**해결**:
- PKCE 생성 로직 확인
- `code_verifier` 저장 후 재사용

---

### 3. Refresh Token 실패

**증상**:
```json
{
  "error": "invalid_grant"
}
```

**원인**:
- Refresh Token 만료
- Refresh Token revoked
- Refresh Token 없음

**해결**:
```bash
# 전체 재인증
rm data/ptt_token_config.json
python3 test_csc_http.py
```

---

### 4. 서버 연결 실패

**증상**:
```
ConnectionRefusedError: [Errno 111] Connect call failed
```

**해결**:
```bash
# 서버 시작 확인
ps aux | grep app.py

# 서버 재시작
python3 app.py
```

---

## 부록

### A. 환경 변수

```bash
# 프로덕션 모드
export CSC_ENV=production

# 로그 레벨
export CSC_LOG_LEVEL=INFO

# 데이터 디렉토리
export CSC_DATA_DIR=/path/to/data
```

---

### B. 로그 확인

```bash
# 실시간 로그
tail -f /home/agapeoom/cims/csc/dist/log/$(date +%Y%m%d)_1.txt

# 에러 로그만
grep ERROR /home/agapeoom/cims/csc/dist/log/$(date +%Y%m%d)_1.txt
```

---

### C. 성능 최적화

**권장사항**:
- 프로덕션: Redis 사용 (파일 저장소 대체)
- Access Token TTL: 1시간 (API 호출 빈도에 따라 조정)
- Refresh Token TTL: 7-30일 (보안 정책에 따라 조정)

---

## 문서 이력

| 버전 | 날짜 | 작성자 | 변경 내용 |
|------|------|--------|----------|
| 1.0 | 2026-02-10 | 남광효 | 초안 작성 |
| 1.1 | 2026-02-11 | 남광효 | HTTPS 설정 및 인증서 생성 가이드 추가 |
| 1.2 | 2026-02-24 | 남광효 | 데이터 저장소 MariaDB 마이그레이션 (JSON 제거) |

---

**최종 업데이트**: 2026-02-24
```
