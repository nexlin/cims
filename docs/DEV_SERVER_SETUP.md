# 개발 서버 수동 구축 가이드 (Dev Server Setup)

새 리눅스 머신에 CIMS 개발 서버를 **처음부터 수동으로** 빌드·구동하는 절차입니다.
편의 래퍼(`cims.sh`)를 쓰지 않고 각 컴포넌트를 직접 빌드/실행하는 경로를 본문으로,
래퍼로 한 번에 처리하는 방법을 각 절 끝에 함께 적었습니다.

> 검증 기준: 이 문서는 2026-06-09 기준 리포지터리 실태(`cims.sh`, `configure.sh`,
> `CMakeLists.txt`, `cims-console/vite.config.ts`, `sql/`)를 직접 확인하여 작성했습니다.

---

## 0. 구성요소 한눈에 보기

| 컴포넌트 | 역할 | 빌드 결과물 | 실행 방식 | 기본 포트 |
|---|---|---|---|---|
| **CMP** | RTP 미디어 릴레이 + PTT 발언권 | `build/bin/cmp` (C++) | 바이너리 | UDP 9000 (control) |
| **CSP** | SIP 호 처리 (등록/발착신/PTT) | `build/bin/csp` (C++) | 바이너리 | 5060/5061/25061, 9001 |
| **cwrtc** | WebRTC ↔ SIP/RTP 게이트웨이 | `build/bin/cwrtc` (C++) | 바이너리 | WS 8080 / WSS 8443, SIP 5062 |
| **cspsim** | SIP 단말 시뮬레이터 (테스트) | `build/bin/cspsim` (C++) | 바이너리(CLI) | — |
| **OAM** | 운영·관리 REST API (콘솔 백엔드) | Python (vendored) | `python3 oam/src/oam_app.py` | HTTPS 4419 |
| **CSC** | 가입자/그룹/MCPTT REST API | Python (vendored) | `python3 csc/src/csc_app.py` | HTTPS 4420/4421, MCPTT 4430 |
| **Console** | 관리자 Web UI (React/Vite) | `cims-console/dist/` 또는 dev 서버 | `npm run dev` (개발) | HTTP 3000 |
| **MariaDB** | 가입자/그룹/조직/RBAC 등 영속 저장 | — | 시스템 서비스 | 3306 |

**의존 순서**: MariaDB → (C++/Python 빌드) → **CMP → CSP** → cwrtc → OAM → CSC → Console.
CMP 는 CSP 의 제어 명령(UDP 9000)을 받으므로 **반드시 CSP 보다 먼저** 떠 있어야 합니다.

---

## 1. 사전 준비 (시스템 패키지)

### 1.1 빌드/런타임 도구
```bash
sudo apt-get update
sudo apt-get install -y \
  cmake build-essential libssl-dev git make \
  clang-format \
  python3 python3-dev \
  nodejs npm \
  mariadb-server mariadb-client
```

| 도구 | 최소 버전 | 비고 |
|---|---|---|
| CMake | 3.10+ | `CMakeLists.txt` `cmake_minimum_required(VERSION 3.10)` |
| GCC/G++ | C++17 지원 | `set(CMAKE_CXX_STANDARD 17)` |
| Python | 3.x | csc/oam 실행. 의존성은 vendored(아래 §5) |
| Node/npm | 18+ 권장 | Vite 8.x 사용 (`cims-console/package.json`) |
| MariaDB | 10.x | 가입자/그룹 영속 저장소 |
| clang-format | 임의 | 검증 S1 정적검사용. 없으면 SKIP |

> **첫 C++ 빌드는 네트워크가 필요합니다.** CMake `ExternalProject_Add` 가
> googletest·oneTBB 를 GitHub 에서 내려받습니다(psip/pasf/opencore-amr/vo-amrwbenc 는 `ext/` 동봉).
> air-gapped 환경이면 §3.3 을 참고하세요.

---

## 2. 소스 가져오기

```bash
git clone <repository_url> cims
cd cims
```
이하 모든 경로는 이 리포지터리 루트(예: `/home/cims/work/cims`) 기준입니다.

---

## 3. C++ 빌드 (CMP / CSP / cwrtc / cspsim)

### 3.1 수동 빌드 (out-of-source)
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```
- 첫 빌드는 외부 의존성 다운로드/컴파일로 수 분~수십 분 소요됩니다.
- 산출물: `build/bin/{cmp,csp,cwrtc,cspsim}`

### 3.2 배포 디렉터리 생성 (`make dist`)
설정/스크립트와 함께 한 곳(`build/dist/`)에 모으려면:
```bash
cd build && make dist
```
- `build/dist/{cmp,csp,cwrtc}/{bin,config}/...` 구조로 바이너리+설정 템플릿+기동 스크립트(`csp.sh` 등)가 모입니다.
- 운영/실행은 보통 `build/dist/` 에서 합니다(설정도 여기에 생성됨 — §7).

### 3.3 외부 의존성
| 의존성 | 출처 | 위치 |
|---|---|---|
| psip / pasf | 동봉(로컬) | `ext/` |
| opencore-amr / vo-amrwbenc | 동봉(로컬) | `ext/` → `pkg/` 설치 |
| oneTBB | GitHub 다운로드 | `ext/oneTBB_down/` → `pkg/` |
| googletest | GitHub 다운로드 (release-1.10.0) | `ext/googletest/` |
| ffmpeg(정적) | OAM 녹취 재생용 | `ffmpeg-prefix/`, OAM vendor |

### 3.4 편의 래퍼
```bash
./cims.sh build -j$(nproc)
```
→ C++ 빌드 + `make dist` + **Console(prod) 빌드** + cims-phone 빌드 까지 한 번에. (소스 트리에서만 실행)

---

## 4. Console (cims-console, React/Vite)

### 4.1 개발 모드 (권장 — HMR, 배포 불필요)
```bash
cd cims-console
npm install
npm run dev -- --port 3000 --host
```
- `http://<host>:3000/` 접속.
- `/api/*` 요청을 백엔드로 프록시합니다. 프록시 대상 = `VITE_ADMIN_TARGET`.
  - `vite.config.ts` 기본값: `https://127.0.0.1:4420`
  - `.env.local` / `.env.tb.local`: `VITE_ADMIN_TARGET=https://127.0.0.1:4419` (**OAM**)
  - 즉 개발 콘솔은 **OAM(4419)** 을 백엔드로 사용 → OAM 이 떠 있어야 데이터가 채워집니다.
- 자체서명 인증서 허용을 위해 프록시는 `secure:false`. HTTPS 로 띄우려면 `VITE_DEV_HTTPS=1`.

> **세션 분리 실행 팁**: 터미널/SSH 세션이 끊겨도 유지하려면
> `setsid nohup npm run dev -- --port 3000 --host >/tmp/console.log 2>&1 &`

### 4.2 운영(정적) 빌드
```bash
cd cims-console
npm install
VITE_CONSOLE_TARGET=prod npm run build   # 산출물: cims-console/dist/
```
- prod 타깃은 packaging 메뉴를 숨깁니다. 정적 파일을 웹서버로 서빙(또는 `make dist` 시 `build/dist/console/` 로 복사).

---

## 5. Python 서비스 (OAM / CSC)

두 서비스 모두 의존성을 **vendored** 로 동봉합니다(`oam/vendor/`, `csc/vendor/`).
**상용=air-gapped 전제 → 런타임 pip/apt 설치 금지.** 시스템 `python3` 만 있으면 실행됩니다.

### 5.1 OAM (콘솔 백엔드, :4419)
```bash
cd oam/src
python3 -u oam_app.py
# 설정: oam/config/oam.json (CIMS_OAM_CONFIG 로 경로 override 가능)
```
- 기동 로그에 `Uvicorn running on https://0.0.0.0:4419` 가 보이면 정상.
- `oam_app.py` 가 `oam/vendor/` 와 `csc/src/`(공유 서비스) 를 sys.path 에 자동 추가.

### 5.2 CSC (가입자/그룹/MCPTT API, :4421/:4420, MCPTT :4430)
```bash
cd csc/src
python3 -u csc_app.py
# 설정: csc/config/csc.json (템플릿: csc/config/config_template.json, CIMS_CSC_CONFIG override)
```

### 5.3 (의존성 갱신이 필요할 때만) vendor 재생성
일반적으로는 불필요. 패키지를 바꿔야 할 때만, 빌드 머신에서:
```bash
pip install --target=oam/vendor -r oam/requirements.txt
pip install --target=csc/vendor -r csc/requirements.txt
```
- 주요 의존성: fastapi, uvicorn, starlette, pymysql, PyJWT, loguru, requests, readerwriterlock (+OAM: aiohttp, netifaces, asyncstdlib, strenum)

---

## 6. MariaDB 스키마

### 6.1 신규 설치 = 통합 스키마 1개
`sql/cims_schema.sql` 은 **현행 통합 스키마**(users 의 RBAC role enum, organizations, ptt_groups v3, ptt_affiliations 등 포함)입니다. 신규 DB 는 이것 하나만 적용하면 됩니다.
```bash
sudo mysql -u root < sql/cims_schema.sql      # DB 'cims' 생성 + 전체 테이블
```

### 6.2 DB 계정 권한 부여
`configure.sh`(§7) 가 `build/dist/sql/grant_db_access.sql` 을 생성합니다. 또는 수동으로:
```sql
GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO 'cims'@'127.0.0.1' IDENTIFIED BY '<DB_PASSWORD>';
GRANT SELECT, INSERT, UPDATE, DELETE ON cims.* TO 'cims'@'localhost'  IDENTIFIED BY '<DB_PASSWORD>';
FLUSH PRIVILEGES;
```
- DB 이름 `cims`, 기본 사용자 `cims`, 포트 3306. 자격증명은 각 컴포넌트 json 의 `CimsDatabase` 블록과 **일치**해야 합니다.

### 6.3 `migrate_*.sql` 의 용도
`sql/migrate_*.sql`(31개) 은 **기존 DB 를 올릴 때 쓰는 이력성 마이그레이션**입니다.
신규 설치에는 §6.1 통합 스키마만으로 충분하며, 마이그레이션을 순차 적용할 필요는 없습니다.
(기존 운영 DB 를 최신으로 끌어올리는 경우에만 날짜 순으로 적용.)

---

## 7. 설정 생성 (`configure.sh`)

각 컴포넌트 json(IP/도메인/DB/로그경로)을 로컬 환경에 맞게 한 번에 생성합니다.
```bash
./configure.sh \
  --local-ip 192.168.1.10 \
  --db-host 127.0.0.1 --db-user cims --db-password '<DB_PASSWORD>' \
  --volte-domain ims.mnc033.mcc450.3gppnetwork.org
```
- 생성 대상: `build/dist/{cmp,csp,csc,cwrtc}/config/*.json` + `cims-console/.env.local`/`.env.tb.local`(`VITE_ADMIN_TARGET`) + `build/dist/sql/grant_db_access.sql`
- 주요 플래그: `--csp-ip/--cmp-ip/--cwrtc-ip/--csc-host`, `--db-host/--db-user/--db-password`,
  `--volte-domain/--ptt-domain`, `--service-log-dir/--msg-log-dir/--record-dir`, `--cims-secret/--idms-secret`
- 재실행 멱등성: `--local-ip`/`--db-password` 는 `.cims/server.local.json` 에 저장되어 다음 실행 시 기본값으로 사용.
- 편의 래퍼: `./cims.sh configure [동일 옵션]`

> 도메인 주의: cspsim/단말의 Digest username 은 `imsi@domain` 이라 도메인이 정확해야 인증됩니다.
> VoLTE=`ims.mnc033.mcc450.3gppnetwork.org`, PTT=`ptt.mnc033...`.

---

## 8. 실행 (구동 순서 · 포트)

### 8.1 구동 순서
바이너리는 `build/dist/` 에서 실행(설정이 그곳에 생성됨):
```bash
# 1) CMP (CSP 보다 먼저)
./build/dist/cmp/bin/cmp ./build/dist/cmp/config/cmp.json

# 2) CSP (foreground 는 -n)
./build/dist/csp/bin/csp ./build/dist/csp/config/csp.json -n
# (또는: ./build/dist/csp/bin/csp.sh start)

# 3) cwrtc (WebRTC 가 필요할 때)
./build/dist/cwrtc/bin/cwrtc ./build/dist/cwrtc/config/cwrtc.json

# 4) OAM → 5) CSC → 6) Console (§5, §4)
cd oam/src && python3 -u oam_app.py        # :4419
cd csc/src && python3 -u csc_app.py        # :4421/4420, 4430
cd cims-console && npm run dev -- --port 3000 --host
```

### 8.2 포트 맵 (USAGE.md §7 기준)
| 포트 | 프로토콜 | 컴포넌트 | 용도 |
|---|---|---|---|
| 3000 | HTTP/WS | Console | Web UI (Vite dev) |
| 4419 | HTTPS | OAM | 운영·관리 API (콘솔 백엔드) |
| 4420 | HTTPS | CSC | Admin REST API (운영) |
| 4421 | HTTPS | CSC | Admin REST API (테스트) |
| 4430 | HTTPS | CSC | MCPTT API (UE) |
| 5060 | UDP/TCP | CSP | SIP 시그널링 |
| 5061 | UDP | CSP | SIP TLS |
| 5062 | UDP | cwrtc→CSP | SIP |
| 8080 / 8443 | WS / WSS | cwrtc | WebRTC |
| 9000 | UDP | CMP | 제어 채널 (CSP→CMP) |
| 9001 | UDP | CSP | 제어 응답 (CMP→CSP) |
| 25061 | TCP | CSP | SIP TCP |
| 50000–50039 | UDP | CMP | VoIP RTP |
| 52000–52009 | UDP | CMP | PTT RTP 오디오 |
| 54000–54009 | UDP | CMP | PTT Floor 제어 |
| 56000–56009 | UDP | CMP | PTT 영상 |

---

## 9. 동작 확인

```bash
# OAM 헬스 (자체서명이라 -k)
curl -sk -o /dev/null -w "%{http_code}\n" https://127.0.0.1:4419/api/v1/agents     # 200 기대

# 콘솔 프록시 경유 (dev 서버가 4419 로 프록시)
curl -s  -o /dev/null -w "%{http_code}\n" http://127.0.0.1:3000/api/v1/agents       # 200 기대

# 콘솔 접속: 브라우저로 http://<host>:3000

# 1콜 스모크 (VoLTE / PTT) — cspsim 은 가급적 별도 머신에서
./build/bin/cspsim -server_ip 127.0.0.1 -count 2 -user 1001 -domain ims.mnc033.mcc450.3gppnetwork.org \
  -password 1234 -mode volte -scenario call -call_duration 5
```

---

## 10. 빠른 전체 시퀀스 (요약)

```bash
# 0) 패키지
sudo apt-get update && sudo apt-get install -y \
  cmake build-essential libssl-dev git make clang-format \
  python3 python3-dev nodejs npm mariadb-server mariadb-client

# 1) 소스
git clone <repo> cims && cd cims

# 2) 빌드 (C++ + dist + console + phone)
./cims.sh build -j$(nproc)
#   (수동: cd build && cmake .. && make -j$(nproc) && make dist
#          cd ../cims-console && npm install && npm run dev ...)

# 3) DB
sudo mysql -u root < sql/cims_schema.sql

# 4) 설정
./configure.sh --local-ip <IP> --db-host 127.0.0.1 --db-user cims --db-password '<PW>' \
               --volte-domain ims.mnc033.mcc450.3gppnetwork.org
sudo mysql -u root < build/dist/sql/grant_db_access.sql

# 5) 실행 (CMP → CSP → cwrtc → OAM → CSC → Console)
./build/dist/cmp/bin/cmp ./build/dist/cmp/config/cmp.json &
./build/dist/csp/bin/csp ./build/dist/csp/config/csp.json -n &
( cd oam/src && setsid nohup python3 -u oam_app.py >/tmp/oam.log 2>&1 & )
( cd csc/src && setsid nohup python3 -u csc_app.py >/tmp/csc.log 2>&1 & )
( cd cims-console && setsid nohup npm run dev -- --port 3000 --host >/tmp/console.log 2>&1 & )
```

---

## 부록 A. `cims.sh` 주요 서브커맨드

| 명령 | 용도 |
|---|---|
| `./cims.sh init` | `.cims/server.local.json` 초기화(로컬 IP/DB 등) |
| `./cims.sh build [-j N] [-v X.Y.Z]` | C++ + dist + Console + phone 빌드 |
| `./cims.sh configure [opts]` | `configure.sh` 래퍼(설정 json 생성) |
| `./cims.sh clean [all\|cpp\|py]` | 빌드 산출물 정리 |
| `./cims.sh reset [--files\|--db]` | 하드 리셋 |
| `./cims.sh preflight` | 사전 점검(python3/node 등) |
| `./cims.sh sim [opts]` | cspsim 시뮬레이터 실행 |
| `./cims.sh pkg [opts]` | 배포 tarball 패키징 |
| `./cims.sh verify stage<N> \| run --preset <name>` | 검증 파이프라인 S1~S6 |
| `./cims.sh tb start\|stop\|status [oam\|csc\|console\|all]` | 테스트베드(OAM 4419 / Console 3000) 라이프사이클 |

> 참고: 운영 환경의 서비스 라이프사이클(start/stop/upgrade/HA)은 `agent/`(cims-agent)와 OAM API 가 담당합니다.
> 개발 단계 단일 머신에서는 위 수동 실행 또는 `cims.sh tb` 로 충분합니다.

## 부록 B. 자주 겪는 함정

- **콘솔은 떴는데 데이터가 비어 있음 / `ECONNREFUSED 127.0.0.1:4419`**: OAM 미기동. §5.1 로 OAM 먼저 기동.
- **단말 REGISTER 403**: Digest username `imsi@domain` 불일치. cspsim `-domain` 을 설정한 도메인과 일치시킬 것.
- **녹취 미생성**: 미디어 디렉터리/NFS 마운트 또는 ffmpeg(재생 변환) 문제 — `--record-dir` 와 마운트 상태 확인.
- **첫 빌드 실패(네트워크)**: oneTBB/googletest 다운로드 실패. 사내망/프록시 또는 사전 캐시 필요(§3.3).
- **`./cims.sh build` 가 "소스 트리에서만" 오류**: dist 디렉터리에서 실행하면 안 됨. 리포지터리 루트에서 실행.
- **DB 인증 실패**: 각 컴포넌트 json 의 `CimsDatabase`(host/user/password/db) 가 실제 MariaDB 계정과 일치하는지 확인. `configure.sh` 로 일괄 주입 권장.
```
