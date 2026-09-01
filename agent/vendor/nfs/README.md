# vendor/nfs — NFS 클라이언트 오프라인(air-gapped) 설치용 deb

private 환경엔 apt repo 가 없으므로, `cims-priv` 가 nfs 마운트 시 `mount.nfs`
부재를 감지하면 이 디렉터리의 `*.deb` 를 `dpkg -i` 로 설치한다(keepalived 패턴 동일).

## 포함해야 할 패키지 (Ubuntu 26.04 / amd64 기준, nfs-common 의존 폐쇄집합)
- nfs-common
- libnfsidmap1
- rpcbind
- keyutils
- libevent-core-2.1-7t64

나머지 의존(`libtirpc3t64` `libwrap0` `libgssapi-krb5-2` `libcom-err2` `ucf` 등)은 베이스
이미지에 이미 있어 제외한다(실측: 대상 노드 설치 확인).

## deb 확보 방법 (이미 nfs 동작하는 media02 등에서)
    # 설치본을 deb 로 재포장
    sudo apt-get install -y dpkg-repack 2>/dev/null   # repo 있으면
    for p in nfs-common libnfsidmap1 rpcbind keyutils libevent-core-2.1-7t64 libtirpc3; do
        sudo dpkg-repack "$p"
    done
    # 또는 /var/cache/apt/archives/ 에 캐시된 .deb 복사

생성된 *.deb 를 이 디렉터리에 두고 agent 패키지 재빌드.
