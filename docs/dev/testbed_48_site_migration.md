# .48 정식 배치 전환 — build/dist 실행 → 부트스트랩 설치(/opt/cims-agent) + NAS `/mnt/cims/test48/`

개발 서버 .48(media01)을 S5 검증 때 만든 **소스 트리 배치**(dev OAM 4419 = `build/dist/oam/src`, 소스 agent,
모듈은 `build/dist/mgmt-server/`)에서 **정식 배포 절차**([initial_install.md](../user-manual/initial_install.md) —
부트스트랩 `install.sh` → agent → 패키지 설치)로 옮기는 절차. 이후 .48 의 모든 시험은 배포본 위에서 돈다.

## 0. 결정 (2026-09-23, 사용자)

| 항목 | 결정 |
|---|---|
| (a) 관리 store | NAS. 마운트는 지금처럼 `/mnt/cims` 하나이고, 다른 서버와 섞이지 않게 그 안에 `test48/` 디렉터리를 만들어 데이터 종류별로 나눈다. store·로그 위치는 **경로 설정**(NAS 든 로컬 디스크든 운영 상황이 정한다) — OAM 이 마운트 여부를 따로 판단하지 않는다 |
| (b) 기존 배포 | dep29~36 전부 폐기, 모듈 전부 재패키징·재설치 |

**최종 레이아웃** (`install.sh --runtime-dir … --log-dir …` — 경로 설정, `services/paths.py`):

```
/mnt/cims/                        ← NFS 마운트 (121.161.164.105:/home/cbm/NAS/cims, 기존 fstab) — 마운트는 이것 하나
  test48/                         ← .48 사이트 디렉터리 (다른 서버의 runtime/·service_log/·tester45/ 와 분리)
    runtime/                      ← 관리 store  = CimsRuntimeDir (control/ collections/ pkg_files/ …)
      pkg_files/                  ← 패키지 저장소 (유도)
    service_log/                  ← 서비스 로그·녹취·알람·mcdata_fd = ServiceLogging.Dir (옛 /mnt/cims/log48 를 통째로 이동)
    tester/                       ← 계측기 DataDir = Tester.DataDir (옛 /mnt/cims/tester/data 를 이동)
    _migration/                   ← 이 전환의 이식 자료(overlay·컬렉션·인증서) — 비밀 포함, git 밖
/opt/cims-agent/                  ← 설치 루트: agent + modules/<모듈>/<버전>, modules/oam/runtime(노드 로컬 비밀·인증서)
```

`/mnt/cims/runtime`·`/mnt/cims/service_log`(다른 서버 것)·`/mnt/cims/tester45` 는 건드리지 않는다.
DB(.45 `cims`)·가입자·PTT 그룹·역할은 DB 에 있어 그대로다.

이 전환에서 제품 규칙을 바꿨다([oam_ha.md](../design/features/oam_ha.md) §4.1): store 위치는 `CimsRuntimeDir`(템플릿 선언,
콘솔 편집 가능)·로그는 `ServiceLogging.Dir` 의 **경로 설정**이 정본이고, `CimsRuntimeMount` 는 공유 스토리지 이중화 사이트가
mount guard 를 켜는 선택값이다(비우면 검사 없음). 부트스트랩은 `--runtime-dir`/`--log-dir` 로 받는다. .48 은 마운트 지점을
적지 않는다.

## 1. 준비 (전환 전, 서비스 살아 있는 상태에서 — 완료)

1. 이식 자료 추출(4419 API): `deployments.json`, 배포별 overlay `config_dep{29..36}.json`, CSP 컬렉션 8종
   (`local_nodes access_services routes remote_nodes route_sets rules rule_sets routing_policies`), 단말 대면 인증서
   (`csp/cert/csp.pem`, `csc/cert/server.crt|key`). → `/mnt/cims/test48/_migration/`
2. 새 사이트용 overlay 생성 `new_<모듈>.json` — 경로 치환 `/mnt/cims/log48 → /mnt/cims/test48/service_log`,
   `/mnt/cims/tester/data → /mnt/cims/test48/tester`, 주입·유도 키(`CimsAuth.JwtSecret`·`CimsRuntimeDir`·`Packages.Dir`) 제거.
3. `cd build && make && make dist` → `./cims.sh pkg csp cmp cmdp csc oam-svc oam-cims-tester cims-tester-worker cspsim agent oam`
   → `build/dist/packages/*.tar.gz` + `cims-bootstrap-<oam>.tar.gz`(oam + console + agent 동봉).
4. `scripts/oam-deploy.py` 에 `install`(생성→install job→overlay→start)·`config`(overlay PUT) 서브커맨드 추가.

## 2. 철거 (Claude 실행 — sudo 불필요, 완료)

순서가 중요하다. 모듈 → base → dev OAM → 소스 agent. 옛 트리는 지우지 않는다(검증 뒤 §6).

```bash
# POST /deployments/{id}/job {stop} 를 36→29 순으로 → live down 확인
./cims.sh tb stop                       # dev OAM 4419 (+ vite 3000)
kill <소스 agent pid>                   # build/dist/agent/cims_agent.py --name mgmt-server (ppid 1, nohup) — pgrep -f 는 자기 bash 도 잡는다
ss -ltn | grep -E ':(4419|4445|4480|4490|4430|4421|7100|7110|9000|9001|15060|15061|2855|16000|9900) '   # 전부 비어야 한다
```

데이터 이동(서비스가 다 내려간 뒤, 설치 전 — 같은 NFS export 안이라 mv 는 즉시 끝난다):

```bash
mv /mnt/cims/log48        /mnt/cims/test48/service_log
mv /mnt/cims/tester/data/* /mnt/cims/test48/tester/     # topologies/ scenarios/ runs/ samples/
```

## 3. 부트스트랩 설치 (**사용자 실행 — sudo**)

`install.sh` 는 sudo 가 필요하고 이 세션의 Claude 는 sudo 가 없다. `! sudo …` 는 비밀번호 프롬프트를 못 열므로 실제 터미널에서
실행한다(또는 터미널에서 `sudo -v` 로 자격을 캐시한 직후 `! sudo …`).

첫 시도(2026-09-23 오전)는 `/mnt/cims/test48` 를 따로 nfs4 마운트하는 방식으로 설치됐다 — 되돌린 뒤 다시 한다:

```bash
sudo /opt/cims-agent/uninstall-base.sh --yes                    # agent·OAM·/opt/cims-agent 제거 (공유 store·마운트는 안 건드림)
sudo umount /mnt/cims/test48 && sudo sed -i '\#/mnt/cims/test48 nfs4#d' /etc/fstab   # 하위 마운트·fstab 줄 제거
rm -rf /mnt/cims/test48/runtime                                 # 첫 설치가 만든 빈 store (cims 소유 — sudo 불필요)
```

그 다음 새 부트스트랩(oam 0.2.165 — `--runtime-dir`/`--log-dir` 지원)으로:

```bash
mkdir -p ~/bootstrap && rm -rf ~/bootstrap/cims-bootstrap && tar xzf build/dist/packages/cims-bootstrap-<oam버전>.tar.gz -C ~/bootstrap
sudo ~/bootstrap/cims-bootstrap/install.sh --batch --user cims --port 4419 --server-name media01 \
     --mgmt-ip 121.161.164.48 --admin-pass 1234 \
     --runtime-dir /mnt/cims/test48/runtime --log-dir /mnt/cims/test48/service_log
```

- 마운트 옵션 없음 — `/mnt/cims` 는 시스템 fstab 으로 이미 붙어 있다. store 는 경로 설정으로 `/mnt/cims/test48/runtime`,
  서비스 로그는 `/mnt/cims/test48/service_log`(이미 이동해 둔 데이터 위에 그대로 이어 쓴다).
- `--batch` 라 문답 없음. admin 비밀번호는 지금과 같은 값(개발 서버).
- `~/.config/systemd/user/cims-agent.service`·`/etc/sudoers.d/cims`·linger 는 설치가 다시 맞춘다.

확인:

```bash
curl -sk -o /dev/null -w 'OAM %{http_code}\n' https://127.0.0.1:4419/
ps -eo user,pid,args | grep -E 'oam_app|cims_agent' | grep -v grep     # cims 소유 oam_app.py --role base · /opt/cims-agent/agent/current/cims_agent.py
ls /mnt/cims/test48/runtime          # control/ pkg_files/ … (마운트는 /mnt/cims 하나)
cat /opt/cims-agent/modules/oam/current/oam/config.json | grep -E 'CimsRuntimeDir|Packages.Dir|"Dir"'
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

### 4.1 실측에서 드러난 함정 (2026-09-23)

- **API 로 뽑은 overlay 의 password 는 마스크다** — `GET /deployments/{id}/config` 는 `type=password` 필드를 `••••••••` 로 돌려준다.
  그 파일을 `POST /deployments {config}` 에 그대로 넣으면 마스크 문자열이 **비밀번호로 저장**돼 CSP `DB Connect failed`·REGISTER 403(unknown user),
  CSC/oam-svc DB 실패가 난다. 실값은 소스 설정(`build/dist/csp/config/csp.json` `Setup.Database.Password`, 옛 `csc.json` `IdMs.JwtSecret`)에서
  채워 넣는다(`_migration/new_*.json` 은 채워진 상태).
- **overlay PUT 만으로는 노드 파일이 안 바뀐다** — `PUT /deployments/{id}/config` 는 `queue_update: true` 로 update_config job 을 큐잉해야
  실체화가 `config.json` 을 다시 쓴다. restart job 은 파일을 안 쓴다. `oam-deploy.py config` 가 그렇게 한다(update → 8 초 → restart).
- **oam-svc `FmIngest.Port` 는 9010** — 옛 overlay 의 9011 은 dev OAM 이 9010 을 쓰던 시절 값. 모듈(csp/cmp/cmdp) `Fm.OamPort` 기본 9010 과
  맞춰야 FM_REGISTER ack 가 온다.
- **capability 바이너리의 live 판정** — csp(`cap_net_admin`)·cims-tester-worker(`cap_sys_admin`)는 같은 uid 라도 `/proc/<pid>/exe` 를 못 읽어
  agent 의 설치 트리 소유 검사가 실패 → live=down + watchdog 재시작 반복. agent 0.2.107 이 `cims-priv proc-exe`(sudo)로 폴백한다.
- csc 4430 인증서는 OAM-CA 자동발급본을 쓴다 — 옛 `cert/server.*` 복사는 효과 없음(.48 은 실단말 대상이 아니라 무관). csp `cert/csp.pem` 은 복사가 필요하다.
- 새 사이트의 dep 번호: oam 1 · oam-svc 2 · csc 3 · cmp 4 · cmdp 5 · csp 6 · oam-cims-tester 7 · cims-tester-worker 8, agent 1(media01).
  계측기 토폴로지 tb48 = id **1**(`csp_deployment_id: 6`, `nodes.csp.logs` = `/opt/cims-agent/modules/csp/current/csp/log/csp_*.log`).
- 계측기 creds 는 DB 에서 다시 뽑았다(`/mnt/cims/test48/tester/scenarios/creds/` — volte 40 · volte_tls 20 · voip 3(`--transport TLS`) · ptt 11 ·
  ptt_video 5). 옛 run 색인·서비스 로그는 새 설치라 이어받지 않았다(사용자 결정).

## 5. 검증 — 2026-09-23 pass

`VOLTE-CALL-BASIC`(등록 30/30, RRD p95 5 ms)·`PTT-GROUP-CALL-BASIC`(11/11) pass. 전 모듈 running/up, 4419 콘솔·게이트웨이 정상.

## 5a. 검증 절차

```bash
scripts/oam-deploy.py status                                   # 전부 running/up
curl -sk https://127.0.0.1:4419/api/v1/tester/health -H "Authorization: Bearer $TOK"   # data_dir=/mnt/cims/test48/tester · topologies 2
cims-tester run VOLTE-CALL-BASIC --topology 1 --ht 3 --instances 1 ; cims-tester run PTT-GROUP-CALL-BASIC --topology 1 …
```

콘솔: `/deploy/servers` 서버 1·모듈 8, `/service/history/ptt` 이력·녹취 재생(service_log 이동 확인), `/test/topologies` tb48.

## 6. 정리 (검증 pass 뒤)

- `build/dist/mgmt-server/`·`build/dist/ext_mnt/`·`build/dist/oam/log` 는 하루 두고 삭제. `cims.sh up/start/tb` 는 .48 에서 쓰지 않는다
  (S3/S5/S6 dist 스택 단계는 포트 충돌 — 계측기 시나리오로 대체).
- 이후 사이클 = `make` + `./cims.sh pkg <모듈>` → `scripts/oam-deploy.py packages/upgrade` → 계측기 시나리오.
- `docs/dev/oam_api_deploy_runbook.md` 의 전제(패키지 store 경로·dep 번호)와 메모리(`cims-dev-server-env`) 갱신.
