# csp/vendor — CSP OS 의존 라이브러리 오프라인(air-gapped) 설치용 deb

CSP 는 가입자·그룹 데이터를 MariaDB 에서 읽는 **C++ 바이너리**라 네이티브 클라이언트
라이브러리(`libmariadb.so.3`)가 실행 전제다. private 환경엔 apt repo 가 없으므로 deb 를
패키지에 싣고, agent 가 **모듈 설치 시점에** `dpkg -i` 로 설치한다
(`cims_agent._install_module_deps` → `cims-priv module-deps-install`).

> agent 의 `vendor/`(keepalived·nfs·base)는 **전 노드 균일 설치**다 — 역할과 무관하게 필요한
> OS base 의존이라 그렇다. 이쪽은 반대로 **그 모듈이 설치되는 노드에만** 들어간다. CSP 가 없는
> 노드(CMP 전용 등)에 MariaDB 클라이언트를 깔 이유가 없다.
>
> CSC/OAM 은 순수 파이썬 드라이버(`pymysql`, `<모듈>/vendor/`)를 쓰므로 해당 없음 — 같은 DB 를
> 보지만 네이티브 라이브러리가 필요 없다.

## 포함해야 할 패키지 (Ubuntu 26.04 / amd64 기준, libmariadb3 의존 폐쇄집합)
- libmariadb3
- mariadb-common   (libmariadb3 의존)
- mysql-common     (mariadb-common 의존)

`libssl3t64` · `zlib1g` · `libc6` 도 의존이지만 베이스 이미지에 이미 있어 제외한다
(실측: 대상 노드 설치 확인).

## 왜 `.so` 번들이 아니라 deb 인가

옛 방식은 빌드 장비의 `libmariadb.so.3` 를 패키지 안(`csp/lib/`)에 복사하고 실행 파일 RPATH
`$ORIGIN/../lib` 로 참조했다. 대상 노드에서 **설치가 일어나지 않아** `dpkg`·`ldconfig` 가 그
존재를 모르고, 실행 파일의 상대 경로 규칙 하나에만 의존하는 구조였다.

그 규칙은 파일 capability 가 붙는 순간 무효가 된다 — 리눅스는 특권 바이너리를 보안 실행
모드(`AT_SECURE`)로 취급해 `$ORIGIN` RPATH 와 `LD_LIBRARY_PATH` 를 무시한다(특권 프로그램이
자기 옆 디렉터리에서 라이브러리를 끌어오면 그 디렉터리 쓰기 권한만으로 특권을 뺏을 수 있다).
IPsec(P4) 용 `cims-priv setcap-net-admin` 이 csp 바이너리에 `CAP_NET_ADMIN` 을 걸면서
`libmariadb.so.3: cannot open shared object file` 로 CSP 가 즉시 죽었다(실측 사고).

표준 경로(`/usr/lib/x86_64-linux-gnu/`)에 정식 설치하면 capability 유무와 무관하다.

## deb 갱신

대상 OS 의 apt 캐시(`/var/cache/apt/archives/`)에서 복사하거나
`apt-get download libmariadb3 mariadb-common mysql-common` 으로 받아 이 디렉터리에 둔다.
