# CSP Linux 빌드 및 실행 가이드

이 문서는 Windows Subsystem for Linux (WSL) 환경에서 `CSP` (csp)를 빌드하고 실행하는 방법을 설명합니다.

## 1. 필수 패키지 설치

빌드에 필요한 도구와 라이브러리를 설치해야 합니다. (Ubuntu 24.04 기준)

```bash
sudo apt-get update
sudo apt-get install build-essential libssl-dev cmake net-tools dos2unix
```

## 2. 빌드 및 배포 방법 (CMake & Install)

**참고**: `csp` 프로젝트는 `ext/psip` 하위의 라이브러리들(`SipStack`, `SipUserAgent` 등)을 의존성으로 가지며, 빌드 시 자동으로 함께 컴파일됩니다.

프로젝트 루트(`csp` 디렉토리)에서 다음 명령어를 실행하여 빌드하고 배포 패키지를 생성합니다.

```bash
# 1. 빌드 준비 (Makefiles 생성)
# 기본적으로 프로젝트 루트의 'dist' 폴더에 배포되도록 설정되어 있습니다.
wsl cmake -S . -B build

# 2. 컴파일 및 배포 실행
# 이 명령은 컴파일 후 'dist' 폴더로 실행 파일과 설정 파일들을 자동 복사합니다.
wsl bash -c "cd build && make install -j4"
# or 배포 디렉토리 지정
wsl bash -c "cd build && cmake -DCMAKE_INSTALL_PREFIX=../dist .. && make install"
```

### 배포 디렉토리 (`dist`) 구조
빌드가 완료되면 `dist` 폴더에 다음과 같이 파일이 배치됩니다.

- `dist/bin/`: 실행 파일 (`csp`, `csp.sh`)
- `dist/config/`: 설정 파일 (`csp.xml`, `SipServerXml/` 등)
- `dist/cert/`: 인증서 (`csp.pem`)
- `dist/log/`: 로그 저장소

## 3. 서버 실행 (배포 버전)

배포된 서버를 실행하려면 `dist/bin` 디렉토리의 `csp.sh`를 사용합니다.

```bash
# dist/bin 디렉토리로 이동
ws cd dist/bin

# 서버 시작
wsl ./csp.sh start

# 상태 확인
wsl ./csp.sh status

# 서버 중지
wsl ./csp.sh stop
```

**설정 수정**: 서버 설정은 `dist/config/csp.xml` 파일을 수정하여 반영합니다. (재빌드 불필요)

### 주요 설정 항목 (csp.xml):
- **LocalIp**: 본인 PC IP 또는 `0.0.0.0`
- **CertFile**: `../cert/csp.pem` (상대 경로 사용 시 주의, 실행 위치 기준)
- **Log/Cdr**: `../log`, `../config/cdr` 등으로 설정 권장

**인증서 재생성 (필요 시)**:
```bash
wsl openssl req -x509 -newkey rsa:2048 -nodes -keyout csp.pem -out csp.pem -days 3650 -subj "/C=KR/ST=Seoul/L=Gangnam/O=CIMS/CN=csp"
# 이후 다시 make install 실행하여 배포
```

## 4. 서버 실행 및 관리 (csp.sh)

서버 프로세스를 쉽게 관리하기 위해 `csp.sh` 스크립트를 제공합니다.

### 명령어
- **시작**: 서버를 백그라운드에서 실행하고 Watchdog을 켭니다.
  ```bash
  wsl ./csp.sh start
  ```
- **상태 확인**: 현재 실행 중인 프로세스(PID)를 확인합니다.
  ```bash
  wsl ./csp.sh status
  ```
- **중지**: 서버와 Watchdog을 종료합니다.
  ```bash
  wsl ./csp.sh stop
  ```

## 5. 포트 정보
서버가 정상적으로 실행되면 다음 포트가 리스닝 상태여야 합니다. (`netstat -tulnp` 로 확인 가능)

- **TCP 6000**: 모니터링 포트
- **TCP 5061**: SIP TLS
- **TCP 25061**: SIP TCP
- **UDP 5060**: SIP UDP

### 방화벽 설정 (외부 접속 시 필수)
Mirrored Networking 모드를 사용하더라도 Windows 방화벽이 포트를 차단할 수 있습니다.
외부에서 접속하려면 **관리자 권한의 PowerShell**에서 다음 명령어를 실행하여 포트를 열어주세요.

```powershell
New-NetFirewallRule -DisplayName "CSP SIP Server" -Direction Inbound -LocalPort 5061,25061,6000 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "CSP SIP Server UDP" -Direction Inbound -LocalPort 5060 -Protocol UDP -Action Allow
```

## 6. 문제 해결

### 권한 오류
스크립트 실행 시 `Permission denied` 등의 오류가 발생하면 실행 권한을 부여하세요.
```bash
wsl chmod +x csp.sh
```

### 윈도우 줄바꿈 오류
스크립트 실행 시 `command not found` 또는 `\r` 관련 오류가 발생하면 줄바꿈 형식을 변환하세요.
```bash
wsl dos2unix csp.sh
```

### SSL 오류
로그에 `ee key too small` 오류가 발생하면 `openssl` 명령어로 2048-bit 인증서를 재생성해야 합니다.
```bash
# build 디렉토리 내에서 실행
wsl openssl req -newkey rsa:2048 -nodes -keyout build/csp.key -x509 -days 365 -out build/csp.crt -subj "/CN=CspServer"
wsl cat build/csp.key build/csp.crt > build/csp.pem
```
