# .48 정식 배치 전환 — build/dist 실행 → 부트스트랩 설치(/opt/cims-agent) + NAS `/mnt/cims/test48`

개발 서버 .48(media01)을 S5 검증 때 만든 **소스 트리 배치**(dev OAM 4419 = `build/dist/oam/src`, 소스 agent,
모듈은 `build/dist/mgmt-server/`)에서 **정식 배포 절차**([initial_install.md](../user-manual/initial_install.md) —
부트스트랩 `install.sh` → agent → 패키지 설치)로 옮기는 절차. 이후 .48 의 모든 시험은 배포본 위에서 돈다.

## 0. 결정 (2026-09-23, 사용자)

| 항목 | 결정 |
|---|---|
| (a) 관리 store | NAS. 다른 서버와 섞이지 않게 `/mnt/cims/test48` 를 **.48 전용 마운트 지점**으로 두고 그 아래 데이터 종류별 디렉터리 |
| (b) 기존 배포 | dep29~36 전부 폐기, 모듈 전부 재패키징·재설치 |

**최종 레이아웃** (`install.sh --runtime-mount /mnt/cims/test48` 의 유도 규칙 = `services/paths.py`):

```
/mnt/cims/test48/                 ← NFS 마운트 지점 (121.161.164.105:/home/cbm/NAS/cims/test48, fstab)
  runtime/                        ← 관리 store (CimsRuntimeMount 유도: control/ collections/ pkg_files/ …)
    pkg_files/                    ← 패키지 저장소
  service_log/                    ← 서비스 로그·녹취·알람·mcdata_fd  (옛 /mnt/cims/log48 를 통째로 이동)
  tester/                         ← 계측기 DataDir (옛 /mnt/cims/tester/data 를 이동 — topologies·creds·runs)
  _migration/                     ← 이 전환의 이식 자료(overlay·컬렉션·인증서) — 비밀 포함, git 밖
/opt/cims-agent/                  ← 설치 루트: agent + modules/<모듈>/<버전>, modules/oam/runtime(노드 로컬 비밀·인증서)
```

`/mnt/cims/runtime`·`/mnt/cims/service_log`(다른 서버 것)·`/mnt/cims/tester45` 는 건드리지 않는다.
DB(.45 `cims`)·가입자·PTT 그룹·역할은 DB 에 있어 그대로다.

OAM 의 mount guard(`oam_app._assert_runtime_mount`)는 `CimsRuntimeMount` 가 `/proc/mounts` 에 **그 경로로** 있어야
기동하므로 `/mnt/cims` 의 하위 디렉터리를 그냥 쓰면 안 되고, export 의 하위 경로를 `/mnt/cims/test48` 에 **따로 마운트**한다
(`install.sh --mount … --mount-src …`, cims-priv mount-add 가 fstab `_netdev,nofail` 로 영속화).

## 1. 준비 (전환 전, 서비스 살아 있는 상태에서 — 완료)

1. 이식 자료 추출(4419 API): `deployments.json`, 배포별 overlay `config_dep{29..36}.json`, CSP 컬렉션 8종
   (`local_nodes access_services routes remote_nodes route_sets rules rule_sets routing_policies`), 단말 대면 인증서
   (`csp/cert/csp.pem`, `csc/cert/server.crt|key`). → `/mnt/cims/test48/_migration/`
2. 새 사이트용 overlay 생성 `new_<모듈>.json` — 경로 치환 `/mnt/cims/log48 → /mnt/cims/test48/service_log`,
   `/mnt/cims/tester/data → /mnt/cims/test48/tester`, 주입·유도 키(`CimsAuth.JwtSecret`·`CimsRuntimeDir`·`Packages.Dir`) 제거.
3. `cd build && make && make dist` → `./cims.sh pkg csp cmp cmdp csc oam-svc oam-cims-tester cims-tester-worker cspsim agent oam`
   → `build/dist/packages/*.tar.gz` + `cims-bootstrap-<oam>.tar.gz`(oam + console + agent 동봉).
4. `scripts/oam-deploy.py` 에 `install`(생성→install job→overlay→start)·`config`(overlay PUT) 서브커맨드 추가.

## 2. 철거 (Claude 실행 — sudo 불필요)

순서가 중요하다. 모듈 → base → dev OAM → 소스 agent. 옛 트리는 지우지 않는다(검증 뒤 §6).

```bash
export OAM_URL=https://127.0.0.1:4419 OAM_LOGIN=admin OAM_PASSWORD=…
for d in 36 35 34 33 32 31 30; do scripts/oam-deploy.py … stop $d; done   # POST /deployments/{id}/job {stop} → live down
# dep29(4445 base oam) stop → 4445 내려감
./cims.sh tb stop                       # dev OAM 4419 (+ vite 3000)
kill <소스 agent pid>                   # build/dist/agent/cims_agent.py --name mgmt-server (ppid 1, nohup)
pgrep -af 'bin/csp|bin/cmp|bin/cmdp|cims-tester-worker|oam_app.py|oam_svc_app.py|csc_pihttp' # 잔존 확인 → kill
ss -ltn | grep -E ':(4419|4445|4480|4490|4430|4421|7100|7110|9000|9001|15060|15061|2855|16000|9900) '   # 전부 비어야 한다
```

데이터 이동(서비스가 다 내려간 뒤, **설치 전에** — `install.sh` 가 `<마운트>/service_log` 를 만들기 전에 옮겨 둬야 mv 가 된다.
같은 NFS export 안이라 mv 는 즉시 끝난다):

```bash
mv /mnt/cims/log48        /mnt/cims/test48/service_log
mv /mnt/cims/tester/data/* /mnt/cims/test48/tester/     # topologies/ scenarios/ runs/ samples/
```

## 3. 부트스트랩 설치 (**사용자 실행 — sudo**)

`install.sh` 는 sudo 가 필요하고 이 세션의 Claude 는 sudo 가 없다(`sudo: interactive authentication is required`).
프롬프트에 `! ` 를 앞에 붙여 실행하면 출력이 세션에 들어온다.

```bash
mkdir -p ~/bootstrap && rm -rf ~/bootstrap/cims-bootstrap && tar xzf build/dist/packages/cims-bootstrap-<oam버전>.tar.gz -C ~/bootstrap
sudo ~/bootstrap/cims-bootstrap/install.sh --batch --user cims --port 4419 --server-name media01 \
     --mgmt-ip 121.161.164.48 --admin-pass 1234 \
     --mount /mnt/cims/test48 --mount-src 121.161.164.105:/home/cbm/NAS/cims/test48 --runtime-mount /mnt/cims/test48
```

- `--mount` + `--mount-src` = export 하위 `cims/test48` 를 `/mnt/cims/test48` 에 nfs4 로 붙이고 fstab 에 남긴다.
  `--runtime-mount` = 관리 store 를 그 마운트 아래 `runtime/` 으로. 서비스 로그는 `MNT_TARGET/service_log` 로 유도된다.
- `--batch` 라 문답 없음. admin 비밀번호는 지금과 같은 값(개발 서버).
- 옛 잔재 `~/.config/systemd/user/cims-agent.service`(`/opt/cims-agent`·`Media-Server-01`·10.0.2.45 를 가리키는 activating 상태 unit)는
  설치가 덮어쓴다. `/etc/sudoers.d/cims`·linger 도 설치가 다시 맞춘다.
- 하위 경로 nfs4 마운트가 거부되면(서버 export 옵션) 대안 = `sudo mount --bind /mnt/cims/test48 /mnt/cims/test48` + fstab 한 줄,
  그 뒤 `--mount /mnt/cims/test48 --runtime-mount /mnt/cims/test48` 만으로 재실행(이미 마운트됨 → 그대로 사용).

확인:

```bash
curl -sk -o /dev/null -w 'OAM %{http_code}\n' https://127.0.0.1:4419/
ps -eo user,pid,args | grep -E 'oam_app|cims_agent' | grep -v grep     # cims 소유 oam_app.py --role base · /opt/cims-agent/agent/cims_agent.py
findmnt /mnt/cims/test48 ; ls /mnt/cims/test48/runtime
```

## 4. 모듈 재설치 (Claude 실행 — API)

```bash
export OAM_URL=https://127.0.0.1:4419 OAM_LOGIN=admin OAM_PASSWORD=1234 OAM_PACKAGE_STORE=/mnt/cims/test48/runtime/pkg_files
scripts/oam-deploy.py packages build/dist/packages/{oam-svc,csc,cmp,cmdp,csp,oam-cims-tester,cims-tester-worker,cspsim}-*.tar.gz
M=/mnt/cims/test48/_migration
scripts/oam-deploy.py install oam-svc csc --agent media01 --config oam-svc=$M/new_oam-svc.json --config csc=$M/new_csc.json
scripts/oam-deploy.py install cmp cmdp  --agent media01 --config cmp=$M/new_cmp.json --config cmdp=$M/new_cmdp.json
scripts/oam-deploy.py install csp       --agent media01 --config csp=$M/new_csp.json --no-start       # local_nodes 없이는 기동 거부
scripts/oam-deploy.py collection <csp dep> local_nodes --file $M/collections/local_nodes.json   # 8종 순서대로 …
cp $M/files/csp/cert/csp.pem /opt/cims-agent/modules/csp/current/csp/cert/ ; cp $M/files/csc/cert/server.* …/csc/current/csc/cert/
# csp start · csc restart(인증서)
scripts/oam-deploy.py install oam-cims-tester cims-tester-worker --agent media01 --config oam-cims-tester=$M/new_oam-cims-tester.json --config cims-tester-worker=$M/new_cims-tester-worker.json
```

- 단말 대면 TLS 인증서(csp `cert/csp.pem` = local_nodes `access-tls.tls_cert_path`, csc `cert/server.crt|key` 4430)는 패키지에 없다 —
  옛 트리에서 복사한 뒤 기동. 단말 앵커(APK)는 .45 CSC 것이라 .48 은 어차피 계측기·콘솔 전용.
- `oam` 은 재설치하지 않는다(부트스트랩이 자기등록). 게이트웨이 라우트는 모듈이 self-register.
- 계측기 `Tester.Secrets`(PBX_HA1)·`Tester.BaseOamUrl`·`Tester.DataDir=/mnt/cims/test48/tester` 는 overlay 로 들어간다.
  토폴로지·creds·run 색인은 DataDir 이동으로 그대로.

## 5. 검증

```bash
scripts/oam-deploy.py status                                   # 전부 running/up
curl -sk https://127.0.0.1:4419/api/v1/tester/health -H "Authorization: Bearer $TOK"   # data_dir=/mnt/cims/test48/tester · topologies 2
cims-tester run VOLTE-CALL-BASIC --topology 2 --ht 3 --instances 1 ; cims-tester run PTT-GROUP-CALL-BASIC --topology 2 …
```

콘솔: `/deploy/servers` 서버 1·모듈 8, `/service/history/ptt` 이력·녹취 재생(service_log 이동 확인), `/test/topologies` tb48.

## 6. 정리 (검증 pass 뒤)

- `build/dist/mgmt-server/`·`build/dist/ext_mnt/`·`build/dist/oam/log` 는 하루 두고 삭제. `cims.sh up/start/tb` 는 .48 에서 쓰지 않는다
  (S3/S5/S6 dist 스택 단계는 포트 충돌 — 계측기 시나리오로 대체).
- 이후 사이클 = `make` + `./cims.sh pkg <모듈>` → `scripts/oam-deploy.py packages/upgrade` → 계측기 시나리오.
- `docs/dev/oam_api_deploy_runbook.md` 의 전제(패키지 store 경로·dep 번호)와 메모리(`cims-dev-server-env`) 갱신.
