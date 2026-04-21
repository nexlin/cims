# MariaDB 설치 및 IDMS 연동 가이드

이 문서는 CSC IDMS의 데이터 저장소를 JSON에서 MariaDB로 전환하기 위한 설치, 설정 및 테이블 생성 절차를 설명합니다.

---

## 1. MariaDB 설치 및 보안 설정

### 1-1. 패키지 설치
Ubuntu 환경에서 MariaDB 서버를 설치합니다.
```bash
sudo apt update
sudo apt install mariadb-server -y
```

### 1-2. 기본 보안 설정
설치 직후 root 비밀번호 설정 및 불필요한 기본 데이터를 삭제합니다.
```bash
sudo mariadb-secure-installation
# - Switch to unix_socket authentication? [Y/n] n
# - Change the root password? [Y/n] y (원하는 비밀번호 입력)
# - Remove anonymous users? [Y/n] y
# - Disallow root login remotely? [Y/n] y
# - Remove test database and access to it? [Y/n] y
# - Reload privilege tables now? [Y/n] y
```

---

## 2. 데이터베이스 및 사용자 생성

### 2-1. 데이터베이스 생성
MariaDB 콘솔에 접속하여 IDMS 전용 데이터베이스를 생성합니다.
```sql
CREATE DATABASE csc_idms;
```

### 2-2. 사용자 생성 및 권한 부여
서비스용 계정을 생성하고 모든 IP(`%`)에서 접속할 수 있도록 권한을 설정합니다.
```sql
-- 사용자 생성 (비밀번호: !core0908)
CREATE USER 'agapeoom'@'%' IDENTIFIED BY '!core0908';

-- 권한 부여
GRANT ALL PRIVILEGES ON csc_idms.* TO 'agapeoom'@'%';

-- 설정 반영
FLUSH PRIVILEGES;
```

---

## 3. 외부 접속 허용 (선택 사항)
서버 외부에서 DB에 접속해야 하는 경우 환경 설정을 수정합니다.

1.  **설정 파일 수정**:
    ```bash
    sudo nano /etc/mysql/mariadb.conf.d/50-server.cnf
    ```
2.  **Bind Address 변경**:
    `bind-address = 127.0.0.1` 부분을 `0.0.0.0`으로 수정합니다.
3.  **서비스 재시작**:
    ```bash
    sudo systemctl restart mariadb
    ```

---

## 4. IDMS 테이블 스키마 생성

IDMS 인증 코드와 리프레시 토큰 관리를 위한 테이블 구조입니다.

### 4-1. Authorization Codes 테이블
```sql
USE csc_idms;

CREATE TABLE IF NOT EXISTS auth_codes (
    code VARCHAR(255) PRIMARY KEY,
    user_id VARCHAR(255) NOT NULL,
    client_id VARCHAR(255),
    redirect_uri TEXT,
    scope TEXT,
    state VARCHAR(255),
    issued_at BIGINT,
    expires_at BIGINT,
    used TINYINT(1) DEFAULT 0,
    code_challenge TEXT,
    code_challenge_method VARCHAR(50)
);
```

### 4-2. Refresh Tokens 테이블
```sql
CREATE TABLE IF NOT EXISTS refresh_tokens (
    token_id VARCHAR(255) PRIMARY KEY,
    user_id VARCHAR(255) NOT NULL,
    client_id VARCHAR(255),
    scope TEXT,
    issued_at BIGINT,
    expires_at BIGINT,
    revoked TINYINT(1) DEFAULT 0,
    rotated_to VARCHAR(255),
    FOREIGN KEY (rotated_to) REFERENCES refresh_tokens(token_id) ON DELETE SET NULL
);
```

---

## 5. Python 의존성 설치
서비스에서 MariaDB에 접속하기 위해 `PyMySQL` 패키지가 필요합니다.
```bash
python3 -m pip install PyMySQL
```

---
**최종 업데이트**: 2026-02-24
