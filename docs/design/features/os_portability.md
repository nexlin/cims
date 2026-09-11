# OS 이식성 — 이 배포본이 어떤 리눅스에서 도는가

CIMS 배포본이 **어떤 조건의 리눅스에서 도는지**, 그 조건이 **왜 그 숫자인지**, 그리고
**OS 에 의존하는 지점이 정확히 어디인지**를 정하는 문서다. 배포판이 하나 늘 때 무엇을
확인하고 무엇을 고쳐야 하는지가 여기서 나온다.

빌드 기준 배포판은 Ubuntu 26.04 LTS 지만, **그것이 실행 조건은 아니다.**

---

## 1. 원칙 — 분기가 아니라 축

배포판 대응을 **fork 로 만들지 않는다.** 로키용 코드 갈래를 따로 두면 상류가 움직일
때마다 두 벌을 맞춰야 하고, 결국 한쪽이 조용히 뒤처진다.

대신 **축(axis)** 으로 다룬다 — 같은 산출물이 여러 배포판에서 돌고, 갈라지는 곳은
**설치·기동 경계 몇 군데로 한정**한다.

세 가지를 지킨다.

1. **모듈 소스에는 OS 분기를 넣지 않는다.** `csp/`·`cmp/`·`csc/`·`ems/` 안에 배포판
   이름이 등장하면 그것은 설계 실패다. OS 의존은 전부 설치기(`install.sh`·
   `install-agent.sh`)와 노드 에이전트(`agent/`), TB 키트(`deployment/tb/`)에 있다.
2. **이름이 아니라 능력으로 판정한다.** `ID=ubuntu` 를 보지 않고 glibc 버전·인터프리터
   버전·패키지 관리자 존재를 본다. 이름으로 판정하면 파생 배포판마다 목록이 길어지고,
   정작 진짜 전제는 검사하지 않게 된다.
3. **판정 규칙은 하나다.** 같은 질문("쓸 수 있는 파이썬이 어디 있나")에 대한 답이 여러
   곳에서 달라지면 안 된다. 구현이 여러 파일에 복제돼 있다면(§4) 그 사실을 주석으로
   명시하고 함께 고친다.

---

## 2. 실행 조건 — 능력 셋

배포판 이름이 아니라 아래 셋이 조건이다. 셋을 만족하면 배포판을 묻지 않는다.

| | 조건 | 왜 그 값인가 | 확인 |
|---|---|---|---|
| ① | **x86_64** | 동봉 바이너리(`csp`·`cmp`·`cspsim`) 아키텍처 | `uname -m` |
| ② | **glibc >= 2.38**<br>**GLIBCXX >= 3.4.32** | `csp` 가 링크한 최고 심볼. 낮으면 실행 즉시 죽고 설정으로 풀 수 없다 | `ldd --version`<br>`objdump -T bin/csp \| grep GLIBC_` |
| ③ | **CPython 3.14** | OAM·CSC 의 동봉 확장이 `*.cpython-314-*.so` — ABI 전용 바이너리 | `python3 -V` / `python3.14 -V` |

### ②에 관한 사실 — 파이썬 확장은 제약이 아니다

vendor 에 든 네이티브 확장(17개)이 요구하는 glibc 는 **전부 2.14 이하**다:

```
GLIBC_2.14   pydantic_core · aiohttp · multidict · yarl · propcache · mypyc
GLIBC_2.4    netifaces
GLIBC_2.2.5  charset_normalizer · frozenlist
```

대부분이 manylinux 휠이고, 소스에서 빌드되는 `netifaces` 도 쓰는 심볼이 오래된 것뿐이라
2.4 에 머문다. 즉 **glibc 하한을 정하는 것은 C++ 모듈(csp)이지 파이썬이 아니다.**
③만 채우면 vendor 트리는 배포판을 옮겨도 그대로 쓸 수 있다 — 배포판별 vendor 재수집은
필요 없다.

### ③에 관한 사실 — 자리는 배포판마다 다르다

| 배포판 | CPython 3.14 의 자리 |
|---|---|
| Ubuntu 26.04 | `/usr/bin/python3` (기본 파이썬이 3.14) |
| Rocky Linux 10.2 | `/usr/bin/python3.14` (AppStream 별도 패키지. 기본 `python3` 은 3.12) |
| Rocky Linux 10.0 / 10.1 | **없다** — 저장소에 `python3.<버전>` 패키지 자체가 없다 |
| 그 외 | 반입본 동봉 인터프리터 `<prefix>/runtime/python/bin/python3` |

그래서 **이름으로 찾지 않고 버전을 물어서** 고른다(§4).

---

## 3. 확인된 조합

| 배포판 | glibc | 기본 python3 | 상태 |
|---|---|---|---|
| Ubuntu 26.04 LTS | 2.43 | 3.14 | 빌드 기준 · 전 구간 실측 통과 |
| Rocky Linux 10.2 | 2.39 | 3.12 (+ `python3.14` 패키지) | 조건 ①②③ 충족 확인 |
| Rocky Linux 10.0 / 10.1 | 2.39 | 3.12 | ③ **불충족** — 아래 참조 |

### rhel 계열은 마이너 버전까지 따진다

`python3.14` 는 **Rocky 10.2 에서 처음 들어온 패키지**다. 10.0·10.1 저장소에는
`python3.<버전>` 형태의 패키지가 아예 없다(AppStream 목록 직접 확인). 그래서 rhel 계열은
"메이저 10 이면 된다" 가 성립하지 않는다.

10.2 미만 노드에서는 **§4 의 후보 ②(인터프리터 동봉)가 유일한 길**이다 — 폐쇄망에서는
`dnf update` 로 마이너를 올리는 것도 자유롭지 않다.

또 하나: 반입 rpm 은 수집 장비의 마이너 버전 빌드(`el10_2` 등)다. 대상이 더 낮은 마이너면
`dnf` 가 glibc·systemd 를 포함한 **기반 패키지를 그 마이너로 끌어올린다**(§5). 수집 장비와
대상 장비의 마이너를 맞추는 것이 원칙이고, 못 맞추면 그 영향을 미리 합의해야 한다.

---

## 4. 인터프리터 선택 — 단일 규칙

agent 가 어떤 파이썬으로 기동됐는지가 **파이썬 모듈 전부의 런타임을 결정한다.**
전파 경로는 이렇다.

```
agent (systemd --user: ExecStart=<선택된 파이썬> cims_agent.py)
   │  sys.executable
   ▼
CIMS_PYTHON  ──►  cims-svc  ──►  lifecycle.sh: PYBIN="${CIMS_PYTHON:-…}"
                                      │
                                      ▼
                          "$PYBIN" oam_app.py / csc_app.py
```

선택 순서는 네 단계이고 **어디서나 같다**.

```
① 명시 지정         install-agent.sh --python <경로> · TB_PYTHON=<경로>
② 동봉 인터프리터    <prefix>/runtime/python/bin/python3
③ 배포판 별도 패키지  PATH 의 python3.14
④ 기본 파이썬        PATH 의 python3 이 3.14 인 경우
```

각 후보는 **버전을 직접 물어서** 판정한다:

```bash
"$c" -c 'import sys; raise SystemExit(0 if sys.version_info[:2] == (3, 14) else 1)'
```

### 동봉 인터프리터가 배포판에 안 묶이는 이유

`python-build-standalone` 의 `install_only` 빌드는 **glibc 외에 아무것도 요구하지 않는다**
(2026-09-11 실측):

```
python3.14 본체   libc · libm · libpthread · libdl · libutil · librt   (전부 glibc)
확장 모듈 전체    glibc 외 없음 (tcl/tk 는 동봉물 안에 함께 들어 있다)
OpenSSL 3.5.8     내장 — 시스템 libssl 을 쓰지 않는다
SQLite 3.53.1     내장
```

요구 glibc 는 **2.17** 이라 §2 ②의 2.38(csp) 보다 낮다 — 인터프리터가 하한을 정하는 일은
없다. 그래서 배포판·마이너가 바뀌어도 그대로 동작한다.

**대비 방향이 거꾸로라는 점에 주의한다**: 버전에 묶여 있는 쪽은 배포판 rpm(`el10_2` 전용)이고,
안 묶인 쪽이 동봉본이다. "OS 가 바뀔 때를 대비해 rpm 도 함께 담는다" 는 그래서 성립하지
않는다 — 바뀌면 그 rpm 이 먼저 못 쓰게 된다.

동봉본을 쓰면 **배포판의 보안 갱신 경로(`dnf update`)를 잃는다.** 폐쇄망에서는 그 경로가
애초에 없으므로 실질 손실이 없지만, "배포판 패키지만 쓴다" 는 고객 정책이 있으면 §5 의
`tb-fetch-rpms.sh python` 으로 rpm 경로를 택할 수 있다(05 의 ③).

### 구현 위치 — 세 곳, 같은 규칙

| 파일 | 역할 | 못 찾으면 |
|---|---|---|
| `deployment/bootstrap/install.sh` | base 부트스트랩(OAM+콘솔+agent) | **중단** — OAM 이 여기서 뜬다 |
| `agent/install-agent.sh` | 노드 agent 설치 | **경고 후 계속** — csp/cmp 만 올리는 노드는 3.14 가 필요 없다 |
| `deployment/tb/lib/tb-common.sh` (`tb_find_python314`) | TB 키트 20 단계 전 검사 | 중단(`--force-os` 로 우회) |

앞의 둘은 `curl | bash` 로 단독 배포되므로 공통 파일을 `source` 할 수 없다 — **규칙이
복제돼 있다.** 세 곳 모두 그 사실을 주석에 적어 두었다. **고칠 때는 세 곳을 함께 고친다.**

---

## 5. 패키지 계열 축 — debian / rhel

OS 의존 설치가 갈라지는 **유일한 축**이다. 판정은 명령의 존재로 한다
(`tb_pkg_family`): `apt-get` 이 있으면 debian, `dnf`/`rpm` 이 있으면 rhel.

갈라지는 지점은 아래뿐이다.

| # | 지점 | debian | rhel | 상태 |
|---|---|---|---|---|
| 1 | TB 05 단계 — MariaDB 설치 | `offline/debs/` 를 로컬 apt 저장소로 | `offline/rpms/mariadb/` 를 `dnf install` | 구현됨 |
| 2 | TB 05 단계 — CPython 3.14 | 불필요(기본 파이썬이 3.14) | **동봉 인터프리터**(`offline/runtime/`)가 기본. rpm 경로(`offline/rpms/python/`)는 선택 | 구현됨 |
| 3 | 모듈 동봉 OS 의존 — `libmariadb.so.3` | `csp/vendor/*.deb` (agent 가 설치) | OS 패키지 `mariadb-connector-c` (05 가 함께 설치) | rpm 동봉물 없음 |
| 4 | 철거 — `tb-teardown.sh` | `dpkg -s` / `apt-get purge` | 미구현 | **남은 과제** |
| 5 | agent HA/NFS 의존 — keepalived·nfs | `agent/vendor/*/*.deb` | 미구현 | **남은 과제** (HA·NAS 사용 시) |
| 6 | 방화벽 | ufw — 기본 꺼짐, 할 일 없음 | **firewalld 기본 켜짐** — 서비스 포트를 열어야 한다 | 구현됨(`tb_firewall_allow`) |
| 7 | MariaDB 시스템 DB 생성 시점 | 패키지 postinst | **첫 기동**(`ExecStartPre=mariadb-prepare-db-dir`) | 구현됨 |

### 방화벽 — rhel 에서만 생기는 관문

rhel 계열은 firewalld 가 기본으로 켜져 있고 ssh 외에는 전부 거부한다. **증상이 고약하다** —
서버 자신에게서는 `curl` 이 200 인데 브라우저만 붙지 않고, 바깥에서 보면 `No route to host`
(ICMP host-prohibited)가 돌아온다. "연결이 안 된다" 가 아니라 "거부 응답이 온다" 는 것이
네트워크 차단(보통 조용한 타임아웃)과 구분되는 표시다.

`tb_firewall_allow "<포트>/<proto>"…` 가 이것을 다룬다 — firewalld 가 없거나 꺼져 있으면
아무 일도 하지 않으므로 debian 경로는 그대로다. **여는 것은 우리가 설치한 서비스의 포트뿐**
이고 zone 정책은 건드리지 않는다. `TB_SKIP_FIREWALL=1` 로 끄면 안내만 나온다.

열어야 하는 것: OAM/콘솔(20 단계가 자동) · SIP(UDP 5060·TCP 25061·TLS 5061) ·
CSC 단말 대면(TCP 4430) · RTP 범위(UDP). **게이트웨이 뒤 내부 API 는 loopback 이라 열지
않는다** — 무엇을 여느냐가 곧 노출면이므로 "듣고 있으니 연다" 로 하지 않는다.

### 반입 패키지 수집

폐쇄망 원칙은 두 계열이 같다 — **반입한 패키지로만 설치한다.**

```
tools/tb-fetch-debs.sh mariadb      빌드 장비(우분투)에서
tools/tb-fetch-rpms.sh mariadb      대상과 같은 배포판 장비에서
tools/tb-fetch-python.sh            빌드 장비에서 — 동봉 인터프리터(배포판 무관)
tools/tb-fetch-rpms.sh python       (선택) 배포판 패키지로 3.14 를 깔아야 할 때만
```

**반입본은 `tb-fetch-rpms.sh python` 산출물을 담지 않는다.** 05 가 동봉본을 먼저 보므로
쓰이지 않는 예비품인데, 그 rpm 집합에는 `glibc`·`systemd`·`selinux-policy-targeted` 가
딸려 있어 언젠가 쓰이면 기반 패키지를 건드린다. 필요해지면 그때 한 줄로 다시 받는다.

`dnf download --resolve` 는 **조건부(rich) 의존을 따라가지 않는다** — `mariadb-server` 의
`(mysql-selinux >= 1.0.10 if selinux-policy-targeted)` 가 그래서 빠졌고 대상 장비에서야
드러났다. 그래서 수집기는 목록에 그 이름을 명시하고, 받은 뒤 **dnf 에게 실제로 풀어 보게**
한다(`dnf install --assumeno`). 빠진 것은 네트워크가 있는 수집 장비에서 잡는다.

rpm 은 **빌드 장비에서 받을 수 없다.** 배포판 빌드에 맞춰져 있어 Rocky 10 용을 RHEL 9 에
깔 수 없다. deb 쪽은 빌드 장비가 곧 기준 배포판이라 이 문제가 없었다.

네트워크 저장소는 `TB_ALLOW_ONLINE=1` 을 명시할 때만 쓴다. 반입본이 조용히 인터넷에서
받아 오면, 현장에서 "여기선 되는데 거기선 안 되는" 차이의 원인을 찾을 수 없게 된다.

### rpm 은 색인을 만들지 않는다

deb 경로는 `Packages` 색인을 만들어 로컬 apt 저장소로 붙인다(그래야 apt 가 순서와 의존을
푼다). dnf 는 rpm 파일 목록을 통째로 받으면 그 안에서 의존을 스스로 풀기 때문에 색인이
필요 없다 — rhel 경로가 debian 경로보다 단순한 이유다.

---

## 6. OS 의존 지점 목록

상류가 새 OS 의존을 만들었을 때 **이 목록과 대조**한다. 여기 없는 곳에 배포판 의존이
생겼다면 §1 의 원칙이 깨진 것이다.

| 영역 | 파일 | 무엇이 OS 에 묶이나 |
|---|---|---|
| 부트스트랩 | `deployment/bootstrap/install.sh` | 인터프리터 선택 |
| agent 설치 | `agent/install-agent.sh` | 인터프리터 선택, systemd --user + linger |
| agent 런타임 | `agent/cims_agent.py` (`_install_module_deps`) | 모듈 동봉 `.deb` 설치 (실패는 로그만) |
| agent HA | `agent/lib/ha.sh`, `agent/lib/pkgstate.sh` | dpkg 직렬화·락, keepalived `.deb` |
| 모듈 기동 | `agent/lib/lifecycle.sh` | `PYBIN` (= `CIMS_PYTHON`) |
| TB 전제 검사 | `deployment/tb/lib/tb-common.sh` | 아키텍처·glibc·계열·인터프리터·`mariadbd` 자리·datadir |
| TB OS 패키지 | `deployment/tb/steps/05-os-prereq.sh` | apt/dpkg ↔ dnf/rpm |
| TB 철거 | `deployment/tb/tools/tb-teardown.sh` | dpkg/apt (rhel 미구현) |
| 모듈 동봉 의존 | `csp/vendor/*.deb` | libmariadb 런타임 |

### 배포판마다 다른 자리

| 무엇 | Ubuntu 26.04 | Rocky 10.2 |
|---|---|---|
| `mariadbd` | `/usr/sbin/mariadbd` (PATH 안) | `/usr/libexec/mariadbd` (**PATH 밖**) |
| MariaDB datadir | `/var/lib/mariadb` | `/var/lib/mysql` |
| 기본 `python3` | 3.14 | 3.12 |
| 강제 접근제어 | AppArmor | SELinux |

`mariadbd` 는 `tb_db_serverbin` 이, datadir 은 `tb_db_datadir` 이 양쪽을 본다.
`command -v mariadbd` 로 판정하면 로키에서 "설치 안 됨"으로 오판한다.

---

## 7. 남은 과제

| | 내용 |
|---|---|
| 철거 | `tb-teardown.sh` 의 rhel 갈래 — 지금은 조용히 건너뛰어 MariaDB rpm 이 남는다 |
| HA·NAS | agent 의 keepalived·nfs 동봉물이 `.deb` 뿐 — rhel 노드에서 HA·공유 store 를 쓰려면 rpm 동봉이 필요하다 |
| SELinux | enforcing 상태로 05~20 단계는 통과했다. 모듈이 `/opt/cims-agent` 에서 돌고 파일 capability(`CAP_NET_ADMIN`)를 쓰므로 뒤 단계(45·60·호시험)에서 다시 본다 — `ausearch -m AVC -ts recent` |
| 방화벽 자동 개방 범위 | 지금은 20 단계의 OAM 포트만 자동이다. SIP·CSC·RTP 는 45/60 단계에서 값이 정해지므로 그 자리에 붙여야 한다 |
| 인터프리터 동봉 | §4 의 후보 ② — 재배치 가능 CPython 동봉. 선택 규칙은 이미 그 자리를 먼저 본다. **Rocky 10.2 미만이 대상이면 선택이 아니라 필수다**(§3) |
| rpm 동봉물 | `csp/vendor` 의 rpm 판 — 지금은 OS 패키지(`mariadb-connector-c`)에 기댄다 |

---

## 8. 새 배포판을 추가할 때

1. **조건 셋(§2)을 먼저 잰다** — `uname -m`, `ldd --version`, 쓸 수 있는 CPython 3.14.
   ②를 못 넘기면 그 배포판은 재빌드 대상이지 설정 대상이 아니다.
2. **패키지 계열(§5)을 확인한다.** debian·rhel 중 하나면 새로 만들 갈래가 없다.
3. **§6 목록을 훑어 자리가 다른 것을 찾는다** (`mariadbd`·datadir 류).
4. **반입 패키지를 그 배포판 장비에서 수집한다** (§5).
5. 05 → 20 순으로 뚫고, 막힌 지점을 §6 표에 더한다.
