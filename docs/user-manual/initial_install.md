# 초도 설치 절차 (맨바닥 → 서비스 기동)

새 장비에 CIMS 를 처음 올리는 순서. 부트스트랩(base 운영평면) → DB → 콘솔 배포 →
CSP 접속점 시드까지 다룬다. 콘솔에서의 모듈 배포 상세는
[deployment_workflow.md](deployment_workflow.md) 가 정본이고, 설치 레이아웃·설계 근거는
[design/02_deployment.md](../design/02_deployment.md) §2 를 본다.

## 0. 전제

| 항목 | 요구 |
|---|---|
| OS | Linux + systemd (user session/linger 사용) |
| 서비스 계정 | 일반 계정 1개. **로그인 셸 + home 필요** (`install.sh` 가 `su - <계정>` 으로 전환) |
| 실행 계정 | 서비스 계정과 별개여도 된다. `sudo` 가능한 일반 계정에서 실행 |
| DB | MariaDB (같은 장비 또는 별도). 관리자 접근 필요 |
| 산출물 | `cims-bootstrap-<oam버전>.tar.gz` + 서비스 모듈 tarball (`./cims.sh pkg` → `build/dist/packages/`) |

- **root 직접 로그인으로 실행하면 거부된다.** `install.sh` 는 `EUID≠0` 이거나 `SUDO_USER`
  가 비어 있으면(= root 직접 로그인) 즉시 종료한다 — sudoers/linger 만 누락된 반쪽 설치를
  막기 위한 가드다. 서비스 계정이 root 인 것도 거부한다.
- 서비스 계정과 실행 계정을 분리하는 것이 권장 구성이다. 서비스 계정은 프로세스 소유자일
  뿐이고, 사람은 자기 계정으로 붙어 필요할 때 `sudo` / `sudo -u <서비스계정>` 을 쓴다.
  분리하면 로그·시크릿 읽기에 `sudo` 가 필요해지는 대신, 레포·빌드 트리·SSH 키를 서비스
  계정에 노출하지 않는다.

## 1. DB 부트스트랩

DB·앱 계정·스키마를 만든다. `install.sh` 는 **DB 를 건드리지 않으므로** 이 단계가 선행이다.

```bash
python3 deployment/db-bootstrap/db_bootstrap.py
```

대화식으로 호스트/포트/관리자 계정/DB명/앱 계정을 묻는다. 비대화식:

```bash
python3 deployment/db-bootstrap/db_bootstrap.py --yes \
    --host 127.0.0.1 --port 3306 --admin-user root --admin-pass '****' \
    --db cims --app-user cims --app-pass '****' --grant-host '%'
```

만드는 것 — DB `cims`(utf8mb4) · 앱 계정 + 해당 DB 전권 GRANT · 통합 스키마
`sql/cims_schema.sql`. 상세는 `deployment/db-bootstrap/README.md`.

### DB 관리자 접근이 없을 때

`db_bootstrap.py` 는 `pymysql.connect(host=..., port=...)` 로 **TCP 접속만** 한다.
MariaDB 를 배포판 기본값으로 깔면 `root@localhost` 가 `unix_socket` 인증이라 이 경로로는
붙을 수 없다. 둘 중 하나를 쓴다.

**(a) 소켓으로 들어가 TCP 관리 계정을 임시 발급** — 정석.

```bash
sudo mysql -e "CREATE USER 'cimsadm'@'127.0.0.1' IDENTIFIED BY '<임시비번>'; GRANT ALL PRIVILEGES ON *.* TO 'cimsadm'@'127.0.0.1' WITH GRANT OPTION; FLUSH PRIVILEGES;"
```

이후 `--admin-user cimsadm --admin-pass '<임시비번>'` 으로 실행하고, 끝나면
`DROP USER 'cimsadm'@'127.0.0.1';` 로 회수한다.

**(b) 앱 계정만으로 DB·스키마만 재생성** — 앱 계정이 이미 있고 `ALL PRIVILEGES ON <db>.*`
를 가진 재설치 상황에 쓴다. 환경변수로 앱 계정명을 비우면 `CREATE USER`/`GRANT` 단계를
건너뛴다(`db_bootstrap.py` 의 `if appusr:` 분기).

```bash
CIMS_DB_APP_USER= python3 deployment/db-bootstrap/db_bootstrap.py --yes --host 127.0.0.1 --port 3306 --admin-user cims --admin-pass '****' --db cims
```

확인:

```bash
mysql -u<앱계정> -p<비번> <db> -e "SHOW TABLES;"
```

## 2. 부트스트랩 설치 (base 운영평면)

OAM + 콘솔 + agent 를 올린다. 서비스 모듈은 포함되지 않는다.

```bash
mkdir -p ~/bootstrap && tar xzf build/dist/packages/cims-bootstrap-<oam버전>.tar.gz -C ~/bootstrap
```

```bash
sudo ~/bootstrap/cims-bootstrap/install.sh --mgmt-ip <관리IP> --user <서비스계정>
```

- `--mgmt-ip` 는 **agent↔OAM 통신 기준**이다. `Server.AgentOamUrl` 과 `Mgmt.Cidr`(/24),
  TLS 인증서 SAN 에 반영된다. 생략하면 첫 global IP 가 기본값이 되므로, 관리망과 외부망이
  나뉜 장비에서는 반드시 명시한다.
- `--user` 생략 시 서비스 계정 = `sudo` 호출자.
- `--admin-pass` 는 명령행에 쓰면 shell history 에 남는다. 생략하면 아래 `[5/7]` 에서
  가려진 입력으로 두 번 묻는다.
- `--batch` 를 주면 문답을 생략하고 옵션·기본값만 쓴다(자동화용).

### 대화식 문답 7개

| | 항목 | 기본값 / 권장 |
|---|---|---|
| 1/7 | 설치 경로 | `/opt/cims-agent` |
| 2/7 | OAM bind 포트 | `4419` (비root 프로세스라 1024~65535) |
| 3/7 | 서버 명 | hostname |
| 4/7 | 관리(mgmt) IP | 후보 IP 목록 제시. `--mgmt-ip` 를 줬으면 건너뜀 |
| 5/7 | admin 비밀번호 | 4자 이상, 확인까지 2회 (필수) |
| 6/7 | 공유 스토리지 | Enter = **노드 로컬**. NAS 를 쓰면 `host:/export` 형식으로 입력 |
| 7/7 | 이 서버 agent 자동 설치 | `Y` |

**6/7 을 Enter 로 두면** 관리 store 가 `<prefix>/modules/oam/runtime`, 서비스 로그가 그
하위 `service_log` 로 잡힌다. 관리평면 이중화를 계획하면 여기서 공유 마운트를 지정해야
나중에 store 이관이 불필요하다 — 로컬로 두고 나중에 옮기는 정규 경로는 콘솔의
`[패키지 설정] > oam > 관리 store` 이며, **마운트 지점 하위로만** 옮길 수 있다
(로컬→로컬 이관 경로는 없다).

### install.sh 가 하는 일

```
① <prefix>/modules/oam/<ver> 전개 + current 심볼릭
② modules/oam/runtime/cert — self-signed TLS (SAN: DNS:<host>, 127.0.0.1, <mgmt-ip>)
③ modules/oam/runtime/_secrets/jwt_secret 생성 (0600) + 배포 overlay 에도 주입
④ 동봉 tarball 을 seed_packages/ 에 배치 → OAM 첫 부팅 시 패키지 저장소 자동 등록
⑤ OAM 1회 부트스트랩 기동 (su - <서비스계정>)
⑥ https://127.0.0.1:<port>/install-agent.sh 를 내려받아 root 로 실행
   → agent 전개 + sudoers + linger + enroll + systemd --user enable
⑦ OAM 을 cims-svc 감독으로 인계 (OAM_ROLE=base drop-in + pidfile)
```

root systemd unit(`cims-oam.service`)은 만들지 않는다 — OAM 도 다른 모듈과 같이 agent 의
`cims-svc` + watchdog 이 감독한다. ⑥은 자기 자신의 OAM 에 HTTP 로 붙는 단계이므로, 실패
시 `<prefix>/modules/oam/current/log/agent_install.log` 와 `oam_handover.log` 를 본다.

### 설치 검증

```bash
curl -sk -o /dev/null -w 'OAM %{http_code}\n' https://<관리IP>:4419/
```

```bash
ps -eo user,pid,args | grep -E 'oam_app|cims_agent' | grep -v grep
```

서비스 계정 소유로 `oam_app.py --role base` 와 `cims_agent.py` 두 프로세스가 떠야 한다.
추가로 확인할 것:

```bash
ls <prefix>/modules/oam/runtime/          # control/ pkg_files/ cert/ _secrets/ service_log/
cat <prefix>/modules/oam/current/oam/config.json   # CimsRuntimeDir · Packages.Dir · JwtSecret
ls /var/lib/systemd/linger/               # 서비스 계정
ls /etc/sudoers.d/                        # cims-priv
```

배포 overlay(`config.json`)의 `CimsRuntimeDir` 은 `<prefix>/modules/oam/runtime`,
`Packages.Dir` 은 그 하위 `pkg_files` 여야 한다. 이 값이 **배포 레코드에 명시되어 있어야**
이후 설치되는 csc/oam-svc 가 store 위치를 유도해 받는다.

### 인증서

OAM 은 기동 시 SAN 이 부족하면 그룹 CA 로 인증서를 재발급하며, 이때 호스트의 모든 IP 가
SAN 에 들어간다. 외부망 IP 로 브라우저 접속해도 SAN 불일치는 나지 않는다(self-signed
경고는 남는다). 상용 인증서는 `modules/oam/runtime/cert` 의 `server.crt`/`server.key` 를
교체한다. 특정 주소(VIP 등)를 강제로 넣으려면 `[패키지 설정] > oam > Server.CertSans`.

## 3. 콘솔 배포 — 패키지 등록 → 설치 → 설정

여기부터는 [deployment_workflow.md](deployment_workflow.md) 가 정본이다. 요약하면
`https://<관리IP>:4419/` 로 로그인(admin) 후, **관리 > 시스템 > 시스템/인프라**
(`/deploy/servers`) 한 화면의 상단 탭 4개로 진행한다.

```
[시스템/서버 구성] → [패키지 설치] → [패키지 설정] → [패키지 제어]
```

- **패키지 등록**은 `관리 > 시스템 > 패키지`(`/deploy/packages`). 서비스 모듈
  (csp/cmp/csc/oam-svc, 필요 시 cmdp/cspsim) tarball 을 올린다.
- **`oam-svc` 는 사실상 필수다.** csp·cmp·csc 의 `Fm.OamIp`/`Fm.OamPort` 가 가리키는 알람
  수집기(FM ingest, 기본 9010)가 oam-svc 이고, 콘솔의 서비스/성능/기록 메뉴도 설치된 서비스
  모듈로 게이팅된다. 없으면 모듈 자기보고 알람이 어디에도 도달하지 않는다.
- **`oam` 은 다시 설치하지 않는다.** 부트스트랩이 `status=running` deployment 로 자기등록해
  둔다.
- 게이트웨이 라우트는 서비스 모듈이 설치 시 **self-register** 한다 — 수동 시드 불필요.
- `CimsAuth.JwtSecret` 은 **입력하지 않는다** — 설정 실체화가 게이트웨이 서비스 모듈
  (csc·oam-svc)에 그룹 공통 신원으로 주입한다.
- `CimsRuntimeDir` 은 다르다. oam-svc 는 oam 배포설정에서 유도(`_store_source`)받지만
  **csc 는 받지 못한다** — 비워 두면 컬렉션·IdMS 토큰 경로가 엉뚱하게 잡힌다.
  §4.2 ① 에서 반드시 설정한다.
- 설정 항목은 대부분 재기동이 필요하다. 값을 다 넣고 패널 하단 **`저장 + 재기동`** 을 쓴다
  (`저장` 만 누르면 파일에만 반영되고 프로세스는 옛 값으로 계속 돈다).
- 템플릿에 선언되지 않은 키는 저장 시 버려지고 `미저장(템플릿에 없는 키)` 로 보고된다.
- `Infrastructure (내부 전용)` 섹션은 기본 접혀 있다. 헤더를 클릭해 펼친다.

### 3.1 oam-svc 설치 후 — **oam 재기동** (놓치기 쉬움)

부트스트랩 동봉 콘솔은 **base 프로파일**이라 `관리>시스템`·`관리>릴리스` 만 있다. 가입자
프로비저닝(`관리>구성`)·서비스 현황·통계 메뉴는 **oam-svc 패키지에 동봉된 풀 콘솔**로
오는데, OAM 은 정적 디렉토리를 **기동 시 1회만** 해석하고 그 결과를 리로드에도 보존한다.
oam-svc 설치는 oam-svc 만 재기동하므로 **oam 재기동 없이는 풀 콘솔이 서빙되지 않는다.**

`[패키지 제어]` 탭에서 **oam** 을 재기동한다. 재기동 중 콘솔이 잠시 끊기고, agent 감독이라
자동 복귀한다. 승격 대기 상태면 콘솔 상단에 **"콘솔 업데이트 대기"** 배너가 뜬다.

승격됐는지는 서빙 중인 번들로 확인한다 — oam 동봉본과 다른 해시가 나와야 한다.

```bash
curl -sk https://<관리IP>:4419/ | grep -o 'assets/index-[A-Za-z0-9_-]*\.js'
```

```bash
ls <prefix>/modules/oam-svc/current/oam-svc/console/dist/assets/*.js
```

재기동 후 나타나는 메뉴:

```
관리 > 구성   조직 · 사용자 · PTT 그룹 · MCPTT 정책 · 서비스 정의
운용 > 서비스  서비스 현황 · VoLTE 호 이력 · PTT 세션 이력 · 메세지 이력
운용 > 성능    VoLTE/PTT 통계 · 인터페이스 통계
```

> 독립 `console` 패키지를 설치해도 서빙 경로는 바뀌지 않는다 — 해석 후보에
> `<modules>/console/...` 이 없다. 그 dist 를 쓰려면 `Console.StaticDir` 로 명시해야 한다.

## 4. CSP 접속점 시드 — **없으면 CSP 가 기동하지 않는다**

CSP 의 실효 접속점(bind IP/포트/프로토콜) 정본은 `local_nodes` 컬렉션이다.
`Setup.Sip.LocalIp`/`UdpPort` 는 **identity fallback 일 뿐 bind 에 쓰이지 않는다.**
primary local_node 가 없으면 CSP 는 기동을 중단한다:

```
[ERROR] no primary local_node — start aborted. Set is_primary=true on a
        local_nodes.jsonl record (or use UI 'local_nodes' collection).
```

콘솔 배포 경로에는 이 컬렉션의 초기 시드가 없으므로 **손으로 넣어야 한다.**
(시나리오 YAML 기반 선언적 배포에서는 `deployment/bin/render.py` 가 primary 1개를 보장한다.)

**위치**: `[패키지 설정]` → 좌측에서 서버 선택 → `csp` 모듈 탭 → 그 안의 컬렉션 탭.
HA 그룹에 속하지 않은 서버라면 컬렉션 탭 9종이 모두 보인다.

### 4.1 `Local Node (수신 엔드포인트)`

UE 수신용 UDP 리스너 1개가 최소 구성이다.

| 필드 | 값 |
|---|---|
| 이름 | `access-udp` |
| 활성 | ✔ |
| **Primary (CSP identity)** | **✔** — `enabled=true` 중 정확히 1개 |
| Edge 분류 | `access` (UE 수신) |
| 바인딩 IP | 단말이 붙는 실주소 |
| 포트 | `5060` |
| 프로토콜 | `UDP` |

TCP/TLS 단말을 받으려면 같은 컬렉션에 `protocol=TCP`(관례 25061) / `TLS`(관례 5061) 행을
추가한다. `is_primary` 는 **UDP 행에만** 둔다 — TCP/TLS primary 는
`enabled && edge=access && protocol 일치` 로 자동 선택된다.

**TLS 행은 `tls_cert_path` 를 반드시 채운다.** 비워 두면 리스너가 열리지 않는다:

```
[ERROR] AddTlsListener: no certificate — per-listener cert 미지정이고
        stack-global cert(<none>) 로도 SSL 기동 실패.
```

`Setup.Sip.CertFile` 은 SIP bind 계열 키와 함께 **csp.json 에서 읽지 않으므로**
(`csp/SipServerSetup.cpp` — `local_nodes.jsonl` 가 SoT) stack-global 인증서가 항상 비어
있고, 폴백이 성립하지 않는다. 인증서 경로가 채워지는 유일한 통로는 primary TLS local_node
의 `tls_cert_path` 다.

| 필드 | 값 |
|---|---|
| `tls_cert_path` | 인증서 절대경로. 패키지 동봉본은 `<prefix>/modules/csp/current/csp/cert/csp.pem` (버전 무관 `current` 경유) |
| `tls_key_path` | cert+key 결합 PEM 이면 **비운다** (스택이 인증서 파일에서 키를 읽는다). 분리 배치면 키 경로를 넣는다 |
| `tls_ca_path` | 체인 검증이 필요할 때만 |

동봉 `csp.pem` 은 개발·시험용 self-signed 다. 상용은 이 경로의 파일을 교체한다.

저장 시 CSP 는 SIGUSR1 로 rebind 하므로 재기동이 필요 없다(기동 전이면 이후 start 에 반영).
리스너 개설이 실패하면 프로세스는 죽지 않고 **`A-PRC-012` 알람으로 격리 보고**된다 —
`open` 알람에 `listener/<protocol>:<port>` 가 뜨면 그 접속점만 안 열린 것이다. 실제로 열렸는지는
포트로 확인한다.

```bash
ss -lntu | grep -E '<bind_ip>:(5060|5061|25061)'
```

```bash
openssl s_client -connect <bind_ip>:5061 </dev/null 2>/dev/null | openssl x509 -noout -subject -dates
```

### 4.2 `Access Service (UE 직접 서비스)`

가입자가 직접 붙는 서비스를 종류별로 정의한다. UE 의 IMPU/IMPI 조립(`imsi@<domain>`)과
Digest realm 의 근거다.

| 이름 | 종류 | 도메인 |
|---|---|---|
| `volte` | `volte` | VoLTE 도메인 |
| `mcptt` | `ptt` | PTT 도메인 |

`Auth realm` 을 비우면 도메인을 상속한다. `Inbound 정책` `any` 면 `허용 Local Node` 는
비워 둔다(`restricted` 일 때만 필수).

#### ⚠ 알려진 결함 — `sip_service` 컬렉션 수동 seed 가 필요하다

**이 단계까지만 하면 가입자 프로비저닝이 전면 실패한다.** PTT/VoLTE 번호를 추가할 때
`400 service_ref required to derive ha1 (unknown service)` 가 나온다.

CSP·콘솔은 `access_services` 를 쓰지만, **CSC 는 구 컬렉션 `sip_service` 만 읽는다**
(`csc/src/services/config_cache.py` — `_DOMAIN_BY_ENTITY["service"] = "sip_service"`).
가입자의 SIP 자격 H(A1) 은 `imsi@<domain>:<realm>:<passwd>` 로 만들어지는데, 그 domain/realm
을 이 컬렉션에서 찾기 때문에 비어 있으면 유도가 불가능하다. **`sip_service` 를 쓰는 주체는
어디에도 없다** — 콘솔에도 그 컬렉션 편집 화면이 없다.

해소에는 **두 단계가 다 필요하다.** 하나만 하면 증상이 그대로다.

##### ① csc `CimsRuntimeDir` 설정 — 이것부터

비워 두면 CSC 가 컬렉션을 **엉뚱한 경로에서** 찾는다. `file_store.runtime_root()` 의 폴백이
`ServiceLogging.Dir` 의 형제 `../runtime` 인데, 부트스트랩 레이아웃은 `service_log` 가
`runtime` **안에** 있어서 경로가 겹쳐 나온다.

```
CimsRuntimeDir 미설정
  ServiceLogging.Dir = <prefix>/modules/oam/runtime/service_log
        ../runtime   = <prefix>/modules/oam/runtime/runtime      ← 존재하지 않는 경로
  → 컬렉션 = .../oam/runtime/runtime/collections/csp/sip_service
```

값을 명시하면 `ha_lookup._collections_base()` 의 판정(`basename=runtime` ∧ `parent=oam` ∧
`grandparent=modules`)이 성립해 **소유 모듈 네임스페이스**로 유도된다.

```
CimsRuntimeDir = <prefix>/modules/oam/runtime
  → 컬렉션 = <prefix>/modules/csp/runtime/collections/sip_service
```

**콘솔 위치** — `[패키지 설정]` → `csc` 모듈 탭 → **`서비스 로그`** 섹션 →
필드 **`IdMS 토큰 store`**. `restart` 필드이므로 하단 **`저장 + 재기동`** 을 쓴다.

> 라벨이 `IdMS 토큰 store` 라 찾기 어렵다 — 이 키는 IdMS refresh 토큰/auth code 저장소와
> **컬렉션 읽기 경로를 함께** 정한다. 같은 섹션의 `서비스 로그 루트`와 혼동하지 말 것:
> 넣을 값은 그 한 단계 위(`/service_log` 없이)다.
>
> 이 값은 결함 우회와 **무관하게 반드시 설정해야 한다.** 비우면 IdMS 토큰도 없는 경로에
> 쌓으려 하고, 버전 디렉터리로 유도되면 업그레이드마다 전 단말 재로그인이 된다.

##### ② `sip_service` 컬렉션 seed

`access_services` 를 미러링해 손으로 넣는다. `name`·`domain` 이 §4.2 에서 만든 접속 서비스와
**정확히 같아야** 한다.

```bash
sudo install -o <서비스계정> -g <서비스계정> -d <prefix>/modules/csp/runtime/collections/sip_service
```

서비스마다 `<n>.json` 을 하나씩 둔다 (`file_store` 가 디렉터리의 `*.json` 을 전부 읽는다).

```json
{ "id": 1, "name": "mcptt", "kind": "ptt",
  "domain": "ptt.mnc033.mcc450.3gppnetwork.org", "auth_realm": "",
  "inbound_policy": "any", "priority": 100, "enabled": true, "note": "",
  "listeners": ["access-udp"] }
```

소유자는 서비스 계정, 모드 `660`. **넣은 뒤 `csc` 를 재기동한다** — 설정 캐시는 기동 시
`file_store` 에서 1회 로드되고, `refresh_entity()` 는 CSC 자기 write 경로에서만 호출되므로
(`csc/src/services/mcptt.py`) 외부가 넣은 파일은 재기동으로만 반영된다.

##### 확인

번호 추가가 `201` 로 통하고 `ha1` 이 채워지면 된다.

```bash
mysql -u<앱계정> -p<비번> <db> -e "SELECT id, imsi, service_ref, LEFT(ha1,10) FROM ptt_subscriptions;"
```

접속 서비스를 나중에 바꾸면 **두 곳을 함께 고쳐야 한다** — 이 미러가 어긋나면 이미 발급된
H(A1) 과 CSP 가 계산하는 realm 이 달라져 등록이 401 로 실패한다.

### 4.3 그 밖의 CSP 설정

`설정` 탭에서 채운다.

| 섹션 | 항목 |
|---|---|
| 미디어서버 연동 | `미디어서버(CMP) 엔드포인트`(CMP 주소:9000) · `Local 수신 IP` |
| FM 자기보고 | `OAM FM ingest 주소`(oam-svc 주소) |
| 데이터베이스 | `DB User` / `DB Password` — 기본값이 빈 비번이라 반드시 채운다 |
| 서비스 로그 | `서비스 로그 루트` |
| MCData 미디어평면 | cmdp 미설치면 `CMDP 사용` = false |

`SDP 코덱 테이블`은 기본값을 쓴다. 임의로 좁히면 코덱 협상과 녹취 재생이 함께 깨진다.

### 4.4 CSC 자동 프로비저닝 — CSP 리스너와 반드시 맞춘다

`local_nodes` 는 CSP 가 **어디서 듣는지**를 정하고, CSC 의 자동 프로비저닝은 단말에
**어디로 붙으라고 알려주는지**를 정한다. 이 둘이 어긋나면 CSP 는 정상 기동하고 리스너도
열려 있는데 **단말만 못 붙는다** — 증상이 CSP 쪽으로 보이지 않아 찾기 어렵다.

**위치**: `csc` 탭 → **자동 프로비저닝 (단말 접속 정보)** 섹션. 서비스 종류(volte/ptt)별로
같은 항목이 있다.

| 필드 | 템플릿 기본값 | 넣을 값 |
|---|---|---|
| `VoLTE/PTT SIP 포트` | **15060** | `local_nodes` 의 **UDP primary `bind_port`** (관례 5060) |
| `VoLTE/PTT SIP TCP 포트` | 0 | TCP 리스너를 **다른 포트로 분리**했으면 그 포트. UDP 와 같은 포트를 공용하면 0 |
| `VoLTE/PTT SIP TLS 포트` | 0 | TLS 리스너의 `bind_port`. **0 이면 단말에 TLS 를 광고하지 않는다** |
| `VoLTE/PTT SIP transport` | UDP | 단말의 기본 transport (UDP / TCP / TLS) |

**`SIP 포트` 기본값 15060 은 CSP 관례 포트(5060)와 다르다.** 손대지 않으면 프로비저닝이
단말에 15060 을 알려주고, 그 포트에는 아무것도 없다. 템플릿 설명도 *"CSP 의 수신 local_node
`bind_port` 와 일치해야 한다"* 고 못박고 있으니 **초도 설치에서 반드시 확인한다.**

`TCP 포트` 의 `0` 은 "없음"이 아니라 **"평문 포트를 UDP 와 공용"** 이라는 뜻이다. TCP 를
`local_nodes` 에서 다른 포트(관례 25061)로 분리해 두고 여기를 0 으로 남기면, 단말은 UDP
포트로 TCP 접속을 시도해 실패한다.

**도메인**은 `access_services`(§4.2)의 `domain` 과 같아야 한다 — 단말의 IMPU/IMPI
(`imsi@<domain>`)와 Digest realm 의 근거다. 템플릿 기본값이 이미 맞으면 손대지 않는다.

`MCPTT 서비스 공개 URL`(`McpttServer.PublicUrl`)은 **올인원 단일 노드면 비워 둔다** —
단말/CSP 가 접속해 온 주소에서 유도한다. VIP·NAT·리버스 프록시 뒤라면 반드시 지정한다.

## 5. 기동 및 확인

`[패키지 제어]` 탭에서 모듈을 시작한 뒤 확인한다.

```bash
ps -eo user,pid,etime,args | grep -E 'oam_app|csc_app|oam_svc|bin/csp|bin/cmp' | grep -v grep
```

```bash
ss -lntup | grep -E ':4419|:4421|:4430|:4480|:5060|:9000|:9001|:9010'
```

콘솔 배포 목록에서 모든 모듈이 `status=running` + `live_state=up` 이어야 한다. 추가 확인:

- **게이트웨이 경유 조회** — `/api/v1/users` · `/api/v1/organizations` · `/api/v1/ptt/groups`
  가 200 이면 OAM↔CSC 프록시가 정상이다.
- **FM 경로** — `<store>/service_log/fm_catalog` 와 `state` 의 mtime 이 갱신되면
  모듈 → oam-svc(FM ingest) → store 경로가 돌고 있다는 뜻이다.
- **활성 알람 0건**.
- CSP 는 기동 직후 pid 가 유지되는지 한 번 더 본다 (fail-fast 항목이 남아 있으면
  watchdog 재기동 루프로 보인다).

## 6. 가입자 프로비저닝

DB 를 새로 만들었으면 비어 있다. `관리 > 구성` 에서 **조직 → 사용자 → PTT 그룹** 순으로
넣는다.

- 번호(구독)의 `service_ref` 는 §4.2 에서 만든 **접속 서비스 이름**과 같아야 한다. 이 값으로
  domain/realm 을 찾아 H(A1) 을 만든다.
- `imsi` 는 번호 추가 시 필수다 — 인증 username 의 user 파트(`imsi@<domain>`)가 된다.
- PTT 그룹 ID(`mcptt_group_id`)는 자유 문자열이고 `adhoc-`/`priv-` 접두사만 예약이다.
  cspsim 의 `-group` 기본값이 `1000` 이므로, 시험 편의상 숫자 ID 로 두면 인자를 생략할 수 있다.

### 6.1 호 시험 (cspsim)

절차·시나리오는 [VERIFICATION_MANUAL.md](../VERIFICATION_MANUAL.md) 가 정본이다. 초도 설치
직후 최소 확인은 PTT 그룹호 한 번이다.

```bash
build/bin/cspsim -server_ip <primary_bind_ip> -local_ip <primary_bind_ip> -count 4 -mode ptt -scenario group_call -group <mcptt_group_id> -call_duration 10 -media_dir tests/media -db <db설정.json> -domain <ptt 도메인>
```

**`-local_ip` 를 명시한다 (멀티홈 호스트 필수).** 생략하면 auto-detect 가 첫 global IP 를
고르는데, 그것이 서비스망 인터페이스가 아니면 **SDP 광고 주소와 실제 패킷 출발지가 달라진다.**
등록·호 설정은 정상인데 floor 만 실패하고, CMP 로그에 이렇게 남는다.

```
addMember … ip=<SDP 주소> floor=33827      ← 이 주소로 멤버 등록
Floor from unknown <실제 출발지>:33827      ← 포트는 같고 IP 만 다름 → 매칭 실패
```

증상은 cspsim 쪽 `GRANT timeout — skipping (DENY/QUEUE?)` 다. `media_nat_mode=auto` 로도
흡수되지만 그건 진짜 NAT 를 위한 것이라, 시험 도구의 주소 선택 오류를 그걸로 덮지 않는다.

**`-group` 은 `mcptt_group_id` 다** (surrogate `ptt_groups.id` 가 아니다). PTT 모드는 **그룹
멤버만** 로드하므로 값이 틀리면 `[DB] 0명 가입자 로드 → 중단` 이다.

**`-domain` 은 접속 서비스의 `domain` 과 같아야 한다** — `-db` 는 DB 에서 `id/imsi/ha1` 만
읽고 `authId` 를 `imsi@<domain>` 으로 조립한다.

`-db` 는 `Setup.Database.{Host,Port,User,Password,DbName}` 만 본다. 운영 `csp.json` 은 서비스
계정 소유(`0600`/`0660`)라 읽히지 않을 수 있으므로, 그 다섯 키만 담은 파일을 따로 두면 된다.

```json
{ "Setup": { "Database": { "Host": "127.0.0.1", "Port": 3306,
    "User": "<앱계정>", "Password": "<비번>", "DbName": "<db>" } } }
```

### 통과 판정

stdout 의 `Registered N/N`·`Call OK` 만으로는 부족하다 — **floor 가 돌았는지**를 봐야 한다.

| 확인 | 통과 기준 |
|---|---|
| cspsim stdout | `Registered N/N (fail=0)` · `Call OK/End N/0` · 멤버별 `GRANT received, speaking` |
| 코덱 | `=> RTP(...) codec(96)` = AMR-WB. **`codec(0)`(PCMU) 이면 미디어 미주입** |
| PTT 세션 (`/api/v1/ptt/sessions`) | `turn_count`·`speaker_count` > 0, `speakers` 에 참여자 |
| 녹취 (`/api/v1/recordings`) | 세션당 1건, `segment_count` = 발언 턴 수 |
| 알람 | 시험 후 `open` 0건 |

`turn_count: 0` 인데 `Call OK` 인 세션은 **floor 실패**다 — 위 `-local_ip` 항목을 먼저 본다.

> **XCAP Token 0 (fail=N, 401) 은 알려진 미해결 항목이다** — IdMS 는 `login_id` 로 키를 잡는데
> cspsim 은 `tel:<msisdn>` 을 보낸다. 등록·호·floor 와 무관하므로 초도 설치 판정에서 제외한다.

## 7. 철거 / 재설치

부트스트랩으로 설치한 노드는 `<prefix>/uninstall-base.sh` 가 대칭 제거 경로다
(agent 자체 `uninstall.sh` 에 위임 후 base 잔여를 정리). 콘솔 배포로만 세운 노드에는
`uninstall-base.sh` 가 없으므로 `<prefix>/uninstall.sh` 를 쓰고 base 잔여는 수동 정리한다.

```bash
sudo env -u SUDO_USER bash <prefix>/uninstall.sh --yes
```

- **`env -u SUDO_USER` 를 반드시 붙인다.** `uninstall.sh` 는 서비스 계정을 `SUDO_USER`
  → (없으면) 설치 디렉터리 소유자 순으로 정한다. 다른 계정에서 `sudo` 로 실행하면 호출자
  계정의 systemd unit 을 지우고 정작 대상 계정의 agent 는 살려 둔다. 한 장비에 여러 prefix
  가 있는 구성에서 특히 위험하다.
- **C++ 모듈(csp/cmp/cmdp)은 자동 정리되지 않는다.** 모듈 프로세스 탐색이
  `pgrep -af <install_path>` 인데 `cims-svc` 는 cwd 를 모듈 디렉터리로 잡고 상대 경로
  (`bin/csp config/csp.json -n`)로 exec 하므로 cmdline 에 절대경로가 없다. 철거 후에도
  포트를 물고 살아남으므로 직접 확인·종료한다. `csp` 는 SIGTERM 핸들러가 있어 `pkill` 로
  안 죽는 경우가 있어 `kill -9` 가 필요할 수 있다.

```bash
pgrep -af 'cims_agent.py|bin/csp|bin/cmp|bin/cmdp|oam_app.py|csc_app.py'
```

수동 정리 대상 — 설치 루트 · `linger`(`loginctl disable-linger <계정>`) ·
`/etc/sudoers.d/cims-priv` · 서비스 계정의
`~/.config/systemd/user/cims-agent.service.d/` drop-in.

> drop-in 에 운영자가 손으로 넣은 값이 있으면(한 장비에 agent 여럿을 띄울 때의
> `CIMS_AGENT_SYNC_PORT` 등) 지우기 전에 기록해 둔다 — 재설치 시 다시 필요하다.

재설치 시 **배포 overlay 와 컬렉션은 관리 store 와 함께 사라진다.** 서버별 bind 주소,
DB 비번, 접속 서비스 도메인, `local_nodes` 를 전부 다시 넣어야 하므로, 철거 전에 배포
설정을 받아 두면 복구가 짧아진다.

```bash
curl -sk https://<관리IP>:4419/api/v1/deployments -H "Authorization: Bearer <토큰>" > deployments.json
```

## 8. 문제 해결

| 증상 | 원인 / 확인 |
|---|---|
| `install.sh` 즉시 종료 | root 직접 로그인이거나 `sudo` 미경유. 일반 계정에서 `sudo ./install.sh`. 서비스 계정이 root 면 `--user` 로 일반 계정 지정 |
| `install.sh` 가 서비스 계정으로 전환 후 실패 | 서비스 계정에 로그인 셸/home 이 없거나, 참조 파일이 그 계정이 못 읽는 경로(다른 사용자 home 등)에 있음 |
| agent 설치 단계 실패 | `<prefix>/modules/oam/current/log/agent_install.log`. ⑥은 자기 OAM 에 HTTPS 로 붙는 단계라 OAM 기동 여부·포트를 먼저 본다 |
| `db_bootstrap.py` 관리자 접속 실패 | TCP 전용이다. `unix_socket` 환경이면 §1 의 (a) 또는 (b) |
| CSP `status=failed`, 로그에 `no primary local_node` | §4.1 — `local_nodes` 에 `is_primary=true` 행이 없음 |
| 번호 추가 시 `400 service_ref required to derive ha1 (unknown service)` | §4.2 — ① csc `CimsRuntimeDir`(콘솔 `서비스 로그 > IdMS 토큰 store`)이 비어 있으면 컬렉션을 `<store>/runtime/runtime/…` 에서 찾는다 ② `sip_service` 컬렉션 미러가 없다. **둘 다** 해야 한다 |
| csc 가 컬렉션·IdMS 토큰을 엉뚱한 경로에서 찾음 | `CimsRuntimeDir` 미설정 시 폴백이 `ServiceLogging.Dir` 의 형제 `../runtime` 인데, 부트스트랩 레이아웃은 `service_log` 가 `runtime` 안에 있어 `runtime/runtime` 이 된다. §4.2 ① |
| floor 만 실패 (`GRANT timeout`), 등록·호는 정상 | §6.1 — cspsim `-local_ip` 미지정으로 SDP 주소 ≠ 실제 출발지. CMP 로그에 `Floor from unknown` |
| CSP 는 떴고 리스너도 열렸는데 UE 가 등록되지 않음 | §4.4 — csc `SIP 포트` 가 기본값 **15060** 으로 남아 CSP 리스너(5060)와 어긋난 경우가 가장 흔하다. 다음으로 `access_services` 도메인 ↔ UE 프로비저닝 도메인 불일치, `local_nodes` bind IP 가 단말이 보는 주소인지 |
| 등록이 401 로 실패 | `sip_service` 미러의 `domain`/`auth_realm` 이 `access_services` 와 어긋나면 H(A1) 과 CSP 의 realm 계산이 달라진다. 두 곳을 맞춘 뒤 번호를 다시 저장(H(A1) 재발급) |
| TCP/TLS 로 붙는 UE 만 실패 | csc `SIP TCP/TLS 포트` 가 0 이면 단말에 그 transport 를 광고하지 않거나(TLS) UDP 포트로 시도한다(TCP). §4.4 |
| TLS 리스너가 안 열림 (`A-PRC-012`) | §4.1 — TLS 행의 `tls_cert_path` 미지정. 로그: `AddTlsListener: no certificate` |
| 설정을 저장했는데 반영되지 않음 | 모듈 설정 파일은 기동 시점에 써진다. `저장 + 재기동` 을 쓴다 |
| 저장 후 `미저장(템플릿에 없는 키)` | 그 키는 `config_template.json` 에 선언이 없어 overlay 에 저장되지 않는다. 템플릿 선언이 필요한 항목 |
| csc/oam-svc 가 관리 store 를 못 찾음 | base `oam` 배포설정에 `CimsRuntimeDir` 이 명시돼 있는지 (유도의 근거다) |
| 로그·시크릿을 읽을 수 없음 | 서비스 계정 소유(`0600`/`0660`)다. `sudo` 로 읽거나 실행 계정을 서비스 계정 그룹에 넣는다(재로그인 필요) |
| 철거 후에도 포트가 잡혀 있음 | §7 — C++ 모듈 잔존. `pgrep` 로 확인 후 종료 |

## 관련 문서

- [deployment_workflow.md](deployment_workflow.md) — 콘솔 배포 작업 순서 (3단계 이후 정본)
- [design/02_deployment.md](../design/02_deployment.md) — 설치 레이아웃·부트스트랩 설계 근거
- [design/features/sip_service_model.md](../design/features/sip_service_model.md) — `local_nodes`·`access_services` 모델
- [design/features/oam_ha.md](../design/features/oam_ha.md) — 관리 store 위치·공유 스토리지
- [VERIFICATION_MANUAL.md](../VERIFICATION_MANUAL.md) — cspsim 시험
