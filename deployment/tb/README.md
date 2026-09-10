# deployment/tb — TB(폐쇄망) 단계별 자동 설치

맨바닥 서버에 CIMS 를 올린다. **TB 는 1대 구성**(서버 1대에 모듈 1벌씩, HA 미사용,
서비스 계정 `cims`, DB 동거)이다.

단계마다 스크립트가 따로 있고 **역할 하나 = 단계 하나**로 개별 실행된다 — DB 는 DB 대로,
설정은 설정대로 따로 돌린다. 설정을 고쳐 다시 넣는 일이 설치보다 훨씬 자주 생기기 때문이다.

새 설치 로직을 만들지 않는다 — 이미 있는 엔진을 **정해진 순서로 호출**하고 TB 값을 채워
넣는 껍데기다. 그래서 상용 경로와 갈라지지 않는다.

| 단계 | 호출하는 것 |
|---|---|
| DB 서버 설치 | 반입 `.deb` (로컬 apt 저장소) |
| DB·계정·스키마 | `deployment/db-bootstrap/db_bootstrap.py` |
| 초기 데이터 입력 | `data/*.csv` → DB 직접 입력 (`lib/tb_seed.py`) |
| 관리 서버(OAM) | `cims-bootstrap-<ver>.tar.gz` 의 `install.sh` |
| 타 서버 모듈 | OAM 내장 자동 배포 엔진 (`scripts/prov` = 콘솔 `관리>릴리스>자동 배포` 와 같은 엔진) |
| 가입자 | oam-svc 일괄 등록 API (`POST /api/v1/users/import`) |
| 호시험 | `cspsim` |

절차의 정본은 [docs/user-manual/initial_install.md](../../docs/user-manual/initial_install.md) 다.
이 디렉토리는 그 절차의 자동화이며, 매뉴얼이 경고하는 함정(csc `SIP 포트` 기본값 15060,
`CimsRuntimeDir` 미설정, `local_nodes` primary 누락)을 값으로 고정해 재현 가능하게 한다.

## 원칙

- **원본 불변** — 반입한 tarball·`.deb` 와 레포 스크립트는 고치지 않고 호출만 한다.
  모듈 설정은 OAM 배포 오버레이/컬렉션 경로로만 넣는다(패키지 안 json 직접 편집 금지).
  MariaDB 설정도 배포판 conffile 대신 drop-in(`99-cims-tb.cnf`)으로 넣는다.
- **멱등** — 모든 단계는 여러 번 돌려도 안전하다. 실패하면 `--from <번호>` 로 이어서 돌린다.
- **사이트 값은 한 파일** — `tb-site.conf`. 없으면 물어서 만들고, 두 번째부터 묻지 않는다.
- **네트워크 미사용** — 설치 중 어떤 것도 인터넷에서 받지 않는다.

## 기준 환경

| 항목 | 값 | 왜 고정인가 |
|---|---|---|
| OS | **Ubuntu 26.04 LTS** / x86_64 | `csp`·`cmp` 가 이 배포판의 glibc·`libssl.so.3`·`libmariadb.so.3` 에 링크됨 |
| Python | **3.14** | OAM·CSC 동봉 확장이 `cpython-314` ABI 전용. `netifaces` 는 조건 없이 import 되므로 버전이 다르면 OAM 이 기동조차 못 한다 |
| OS 패키지 | `.deb` | agent 가 OS 의존을 dpkg 로 설치한다 (`agent/bin/cims-priv`) |

다른 배포판(Rocky 등)은 모듈 재빌드 + agent 의 rpm 분기 + 동봉물 재수집이 필요하다 —
설정 변경으로 되는 일이 아니며 별도 과제다. 스크립트는 OS 를 확인하고 다르면 이유를 말하고
멈춘다(`--force-os` 로 무시 가능하지만 권장하지 않는다).

## 쓰는 순서

### ① 빌드 장비(인터넷 됨)에서 반입본 조립

```bash
deployment/tb/tools/tb-fetch-debs.sh mariadb        # MariaDB + 의존 .deb 수집
deployment/tb/tools/tb-pack.sh --with-packages      # db-bootstrap + 모듈 tarball + 시험 음성 모으기
```

`deployment/tb/` 전체(=`offline/` 포함)를 USB 로 옮긴다. `--tar` 를 주면 하나로 묶는다.

> 모듈 tarball 은 반드시 빌드 장비에서 미리 만든다 — 첫 빌드가 외부 의존
> (oneTBB·opencore-amr 등)을 인터넷에서 받는다. C++ 변경분은 `make dist` 를 빼먹으면
> 옛 바이너리가 실린다.

### ② TB 장비에서 설치

```bash
sudo ./tb-install.sh                       # 메뉴에서 이 서버의 역할 선택
sudo ./tb-install.sh --role db             # 비대화식으로 역할 지정
sudo ./tb-install.sh --role db --from 10   # 10 단계부터 이어서
sudo ./tb-install.sh --show                # 저장된 사이트 값 보기 (비밀값 가림)
```

## 역할과 단계

| 역할 | 단계 | 하는 일 |
|---|---|---|
| `all-in-one` | 05→10→15→18→20→30→40→45→50→60 | 처음부터 끝까지 |
| `db` | 05 → 10 → 15 → 18 | DB 설치 + 설정 + 스키마 + 초기데이터 |
| `db-server` | 05 → 10 | DB 서버 설치·설정만 |
| `db-schema` | 15 | 스키마/테이블 생성만 |
| `db-data` | 18 | 초기 데이터 입력만 |
| `oam` | 20 | 관리 서버 부트스트랩 (OAM+콘솔+agent) |
| `packages` | 30 | 패키지 등록 |
| `install` | 40 | 모듈 설치 |
| `config` | 45 | 패키지 설정 (overlay + CSP 컬렉션) |
| `console` | 50 | 풀 콘솔 승격 |
| `start` | 60 | 순서 기동 + 상태 확인 |
| `callcheck` | 70 | 호시험 (cspsim PTT 그룹호) |

### DB 단계 상세

| 단계 | 파일 | 하는 일 |
|---|---|---|
| 05 | `steps/05-os-prereq.sh` | OS·아키텍처·필수 명령 확인 → 반입 `.deb` 설치 |
| 10 | `steps/10-db-setup.sh` | 서비스 기동·자동기동, `bind-address`/포트 drop-in, **임시 TCP 관리 계정 발급** |
| 15 | `steps/15-db-schema.sh` | `db_bootstrap.py` 로 DB·앱계정·스키마 → 앱 계정 접속 검증 → **임시 계정 회수** |
| 18 | `steps/18-db-seed.sh` | `data/*.csv` → 조직·가입자·가입번호·PTT 그룹 직접 입력 |

15 단계에는 모드가 둘 있다. **`create`**(기본)는 DB·앱계정을 만든다. **`reuse`** 는 이미
돌고 있는 DB 를 그대로 쓰고 스키마만 보정하며 **계정을 건드리지 않는다** — `db_bootstrap.py`
가 앱 계정에 `ALTER USER` 로 비밀번호를 다시 심기 때문에(코드 159행) 운영 중 DB 에 그대로
돌리면 모듈들이 일제히 DB 인증 실패한다. `TB_DB_MODE` 로 고른다.

> ⚠ **`CREATE TABLE IF NOT EXISTS` 는 기존 테이블에 컬럼을 추가하지 않는다.** 그래서 15 단계는
> 없는 **테이블**은 만들지만 **컬럼** 드리프트는 못 메운다. 기존 DB 를 이어받는 경우
> `sql/migrate_*.sql` 적용 여부를 따로 확인해야 한다 (신규 설치는 해당 없음 — 최신 스키마에
> 컬럼이 다 들어 있다).

### 서비스 단계 상세

| 단계 | 파일 | 하는 일 |
|---|---|---|
| 20 | `steps/20-oam.sh` | 반입 `cims-bootstrap-*.tar.gz` 의 `install.sh --batch` 호출 → OAM+콘솔+로컬 agent |
| 30 | `steps/30-packages.sh` | `offline/packages/*.tar.gz` → `POST /api/v1/packages` (oam·agent 는 제외 — 부트스트랩이 소유) |
| 40 | `steps/40-install.sh` | 배포 레코드 생성 + `install` job (oam-svc·cmp·csc·csp) |
| 45 | `steps/45-config.sh` | overlay(`update_config`) + CSP 컬렉션(`local_nodes`·`access_services`) |
| 50 | `steps/50-console.sh` | 서빙 번들 ↔ oam-svc 번들 비교 후 **다를 때만** oam 재기동 |
| 60 | `steps/60-start.sh` | `oam-svc → cmp → csc → csp` 순서 기동 + `running/up` 확인 |
| 70 | `steps/70-callcheck.sh` | cspsim PTT 그룹호 + `turn_count` 까지 판정 |

20 단계만 부트스트랩 경로이고(엔진이 자기 자신을 띄울 수 없다), 30~70 은 **OAM 공개 REST**
로만 움직인다(`lib/tb_oam.py`) — 콘솔이 사람 손으로 하는 것과 같은 경로여서 OAM 이 소유한
부수효과(설치 이력·게이트웨이 라우트 재등록·JwtSecret 주입)를 우회하지 않는다.

45 단계가 매뉴얼의 함정 셋을 값으로 고정한다:

| 함정 | 고정하는 값 |
|---|---|
| csc `Provisioning.Services.*.port` 기본값 15060 (§4.4) | CSP UDP 리스너 포트와 같게 |
| csc `CimsRuntimeDir` 미설정 (§4.2 ①) | `<prefix>/modules/oam/runtime` |
| `local_nodes` primary 누락 → CSP 기동 중단 (§4.1) | UDP 행에 `is_primary=true` |
| TLS 행 `tls_cert_path` 누락 → 리스너 미개설 | 동봉 `csp.pem` 경로 |

**`CimsAuth.JwtSecret` 은 넣지 않는다** — OAM 이 그룹 공통 신원으로 주입한다.

## 18 단계 — 데이터 직접 입력

`data/` 의 CSV 5개를 읽어 DB 에 바로 넣는다. **OAM·CSC 가 떠 있지 않아도 되고 콘솔 조작도
필요 없다.** 콘솔·CSC 가 만드는 행과 규약을 똑같이 맞췄다(실 DB 행으로 대조 확인).

| 파일 | 유일키 | 비고 |
|---|---|---|
| `organizations.csv` | `code` | `parent_code` 로 계층. `code_path` 는 `/` 로 자동 조립 |
| `users.csv` | `login_id` | `login_id`/`passwd` = 단말(IdMS) 로그인 자격. **passwd 는 평문 저장**(CSC 가 그대로 비교). `org_code` = 조직 code |
| `subscriptions.csv` | `number` | 가입 번호. `kind`=ptt\|volte · `service_ref` = access_services 의 서비스 이름 |
| `ptt_groups.csv` | `mcptt_group_id` | 빈 열은 DB 기본값 |
| `ptt_group_members.csv` | (그룹, 번호) | `number` 는 **PTT** 가입 번호여야 한다 |

**`ha1` 은 스크립트가 계산한다** — `MD5("<imsi>@<domain>:<realm>:<passwd>")`. 평문 비밀번호는
DB 에 저장되지 않는다(컬럼이 없다). `domain`/`realm` 은 `tb-site.conf` 의 `TB_PTT_DOMAIN`·
`TB_VOLTE_DOMAIN`(+ `_REALM`, 비우면 도메인)에서 오고, **CSP `access_services` 의 그 서비스
행과 반드시 같아야 한다** — 어긋나면 CSP 가 계산하는 realm 과 달라 등록이 401 로 실패한다.

실행하면 먼저 **계획(dry-run)** 을 보여주고 확인을 받은 뒤 넣는다. dry-run 은 트랜잭션으로
실제로 넣어보고 되돌리는 방식이라(전 테이블 InnoDB) 조직 → 사용자 → 번호 → 그룹멤버 연쇄
참조까지 신규 설치 상태에서 그대로 검증된다. 입력은 한 트랜잭션이라 중간에 실패하면 아무것도
반영되지 않는다.

빈 칸의 뜻은 "DB 기본값 / 현재값 유지" 다 — 값을 적은 열만 반영한다.

10 단계의 임시 관리 계정이 필요한 이유: 배포판 기본 MariaDB 는 `root@localhost` 가
`unix_socket` 인증인데 `db_bootstrap.py` 는 TCP 전용이다
(매뉴얼 §1 의 (a) 경로를 자동화한 것). 자격은 `state/db-admin.env`(0600)에 두고 15 단계가
쓰고 지운다.

05 단계는 반입 디렉토리를 **로컬 apt 저장소**로 붙여 설치한다. `dpkg -i *.deb` 는 대체
제공자가 함께 들어오면 충돌로 실패하고(예: `opensysusers` ↔ systemd 의 sysusers) 설치
순서도 스스로 풀지 못한다. 색인이 없으면 dpkg 직접 설치로 폴백한다.

## 파일

```
tb-install.sh          진입점 — 역할 선택 / 단계 실행
tb-site.conf           사이트 값 (스크립트가 생성, 0600, git 제외)
lib/tb-common.sh       공용 — 로그·질의·전제조건 검사 (자기완결, 레포 의존 없음)
steps/                 단계별 스크립트
tools/tb-fetch-debs.sh 반입 .deb 수집 (빌드 장비)
tools/tb-pack.sh       반입본 조립 (빌드 장비)
lib/tb_oam.py          OAM REST 클라이언트 (30~70 단계가 호출, 표준 라이브러리만)
lib/tb_seed.py         CSV → DB 직접 입력기 (18 단계가 호출)
data/*.csv             초기 데이터 (템플릿 동봉 — 사이트 값으로 고쳐 쓴다)
offline/               반입 자원 — debs/ db-bootstrap/ packages/  (git 제외)
state/                 임시 자격·진행 상태 (0700, git 제외)
log/                   단계별 로그 (git 제외)
```

## 사이트 값 (tb-site.conf)

물어보는 순서대로 채워지며, 직접 편집해도 된다. `--reconfigure` 로 다시 묻는다.

| 키 | 뜻 |
|---|---|
| `TB_DB_HOST` / `TB_DB_PORT` | 모듈이 붙을 DB 주소 |
| `TB_DB_NAME` | DB 이름 (기본 `cims`) |
| `TB_DB_APP_USER` / `TB_DB_APP_PASS` | 모듈이 쓸 DB 계정 |
| `TB_DB_GRANT_HOST` | 앱 계정 접속 허용 host (`%` = 모든 호스트) |
| `TB_DB_BIND_ADDRESS` | MariaDB 수신 주소 (`0.0.0.0` / `127.0.0.1`) |
| `TB_DB_ADMIN_USER` / `TB_DB_ADMIN_GRANT_HOST` | 임시 관리 계정 (스키마 적용용) |
| `TB_DB_MODE` | `create`(새로 만든다) / `reuse`(기존 DB·계정 유지, 스키마만) |
| `TB_PTT_DOMAIN` / `TB_PTT_REALM` | PTT ha1 결박 재료 — access_services 와 일치 필수 |
| `TB_VOLTE_DOMAIN` / `TB_VOLTE_REALM` | VoLTE ha1 결박 재료 — 동일 |
| `TB_MGMT_IP` | 관리 IP — agent↔OAM 기준·인증서 SAN |
| `TB_SIP_IP` / `TB_MEDIA_IP` | 단말이 붙는 SIP 주소 / RTP 주소 (1대 구성이면 같다) |
| `TB_OAM_PORT` / `TB_OAM_URL` | 콘솔 포트(4419) / 주소 |
| `TB_ADMIN_PASS` | 콘솔 admin 비밀번호 (부트스트랩에서 지정) |
| `TB_SERVICE_USER` / `TB_INSTALL_PREFIX` | 서비스 계정(`cims`) / 설치 경로 |
| `TB_SIP_UDP_PORT` / `_TCP_` / `_TLS_` | SIP 접속점 포트 (0=미사용) |
| `TB_PTT_SERVICE` / `TB_VOLTE_SERVICE` | 접속서비스 이름 = `subscriptions.csv` 의 `service_ref` |
| `TB_MODULES` / `TB_START_ORDER` | 설치·기동 대상과 순서 |
| `TB_PTT_GROUP` / `TB_CALL_COUNT` / `TB_CALL_DURATION` | 호시험 파라미터 |

관리 계정 비밀번호는 `tb-site.conf` 에 남기지 않는다 — 임시 자격이므로 `state/` 에만 둔다.

## 문제가 생기면

- 각 단계는 실패 지점과 이어서 돌리는 명령을 함께 출력한다.
- 장문 출력은 `log/<단계>.log` 로 간다.
- 증상별 원인은 매뉴얼
  [§8 문제 해결](../../docs/user-manual/initial_install.md) 표가 정본이다.
