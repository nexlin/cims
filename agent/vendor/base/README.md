# vendor/base — OS base 공유 의존성 (air-gapped)

모든 노드가 **역할(HA/NFS 사용 여부)과 무관하게** 필요로 하는 OS base 라이브러리.
private(air-gapped) 환경에서 apt repo 없이 `dpkg -i` 로 설치된다.

## 왜 별도 디렉터리인가

`libmnl0` 는 keepalived 뿐 아니라 **`iproute2`(`ip` 명령)의 의존성**이기도 하다.
과거 `vendor/keepalived/` 에 함께 두었더니, HA 미사용 노드에서
`cims-ha uninstall` 이 keepalived vendor deb 전체를 `dpkg -P --force-all` 로
강제 purge 하면서 `libmnl0` 까지 제거 → `ip` 가
`error while loading shared libraries: libmnl.so.0` 로 깨지고, agent 의
`collect_interfaces`/`collect_routes` 가 빈 배열을 보고 → 콘솔에 네트워크
인터페이스·라우팅이 표시되지 않는 버그가 발생했다 (bootstrap 0.0.4 uninstall
→ 0.0.5 reinstall 시 재현).

→ keepalived 와 공유되더라도 base 공유 의존성은 이 디렉터리에 두고,
   `cims-ha uninstall` 의 purge 대상에서 제외한다.

## 설치/제거 주체

- 설치: `cims-priv ensure-base-deps` (agent 부팅 시 1회, `vendor/*/*.deb` 전체) +
  `cims-ha install` (HA 노드에서 keepalived 와 함께).
- 제거: **하지 않는다.** base 공유 의존성이므로 HA/NFS uninstall 이 건드리지 않는다.

## deb 갱신

대상 OS 의 apt 캐시(`/var/cache/apt/archives/`)에서 복사하거나
`apt-get download libmnl0` 로 받아 이 디렉터리에 두고 agent 패키지를 재빌드.
