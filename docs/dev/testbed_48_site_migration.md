# .48 정식 배치 — 부트스트랩 설치(/opt/cims-agent) + 사이트 디렉터리 `/mnt/cims/test48`

개발 서버 .48(media01)의 배치와 (재)설치 절차. .48 의 모든 시험은 배포본 위에서 돈다 — 모듈은 부트스트랩
`install.sh` → agent → OAM API 패키지 설치([initial_install.md](../user-manual/initial_install.md),
[oam_api_deploy_runbook.md](oam_api_deploy_runbook.md))로 올리고, `build/dist` 직접 실행·`cims.sh up/start/tb` 는 쓰지 않는다.

## 0. 결정 (사용자)

| 항목 | 결정 |
|---|---|
| 사이트 데이터 | NAS. 마운트는 `/mnt/cims` 하나이고, 다른 서버와 섞이지 않게 그 안의 `test48/` 를 **사이트 디렉터리**로 쓴다. 위치는 경로 설정이다(OAM 이 마운트 여부를 판단하지 않는다) |
| 레이아웃 | [site_directory_layout.md](../design/features/site_directory_layout.md) — `CimsSiteDir=/mnt/cims/test48` 하나에서 영역을 유도한다 |
| 재설치 | 레이아웃이 바뀌는 변경은 데이터 이동 없이 철거 → 부트스트랩부터 재설치한다(새 설치라 옛 데이터를 이어받지 않는다). `_migration/`·`tester/` 만 남긴다 |

**레이아웃**:

```
/mnt/cims/                        ← NFS 마운트 (121.161.164.105:/home/cbm/NAS/cims, 시스템 fstab) — 마운트는 이것 하나
  test48/                         ← .48 사이트 디렉터리 = CimsSiteDir (다른 서버의 runtime/·service_log/·tester45/ 와 분리)
    runtime/  packages/  content/  recordings/  log/  stats/  state/     ← 영역 (OAM 이 유도)
    tester/                       ← 계측기 DataDir (topologies/·scenarios/creds/·runs/) — 재설치해도 그대로 쓴다
    _migration/                   ← 재설치 자료(비밀 포함, 0700, git 밖) — 아래 §1
/opt/cims-agent/                  ← 설치 루트: agent + modules/<모듈>/<버전>, modules/oam/runtime(노드 로컬 비밀·인증서)
```

`/mnt/cims/runtime`·`/mnt/cims/service_log`(다른 서버 것)·`/mnt/cims/tester45` 는 건드리지 않는다.
DB(.45 `cims`)·가입자·PTT 그룹·역할은 DB 에 있어 재설치와 무관하다.

## 1. 재설치 자료 — `/mnt/cims/test48/_migration/`

| 파일 | 내용 |
|---|---|
| `site_<모듈>.json` | 모듈 배포 overlay — **경로 키 없음**(`ServiceLogging.Dir`·`McDataFd.Dir`·`Tester.DataDir` 등은 사이트에서 유도되므로 넣지 않는다). 비밀번호는 실값 |
| `now/collections/<이름>.json` | CSP 컬렉션 8종(`local_nodes access_services routes remote_nodes route_sets rules rule_sets routing_policies`) |
| `now/config_dep<N>.json` | 철거 직전 배포 설정 스냅샷(대조용 — password 는 마스크) |
| `files/csp/cert/csp.pem` | 단말 대면 TLS 인증서(`local_nodes access-tls.tls_cert_path = cert/csp.pem`) |

overlay 를 API 로 다시 뽑을 때(`GET /deployments/{id}/config`)는 `type=password` 가 `••••••••` 로 마스크된다 — 그대로 되돌려
넣으면 마스크 문자열이 비밀번호로 저장된다(CSP `DB Connect failed`·REGISTER 403). 실값은 `site_*.json` 을 쓴다.

## 2. 철거 (**사용자 실행 — sudo**)

```bash
sudo /opt/cims-agent/uninstall-base.sh --yes            # agent·OAM·모듈·/opt/cims-agent 제거 (사이트 디렉터리·마운트는 안 건드림)
ps -eo pid,args | grep -E "bin/csp|bin/cmp|bin/cmdp|cims-tester-worker" | grep -v grep   # 남은 C++ 모듈 확인
rm -rf /mnt/cims/test48/{runtime,packages,content,recordings,log,stats,state}   # cims 소유 — sudo 불필요. _migration/·tester/ 는 남긴다
```

**uninstall-base.sh 는 C++ 모듈 프로세스(csp·cmp·cmdp·계측기 워커)를 끝내지 않는다** — 설치 트리가 지워진 채
(`/proc/<pid>/exe … (deleted)`) 옛 설정으로 계속 돌며 옛 경로에 쓰고 포트를 잡는다. cims 소유이므로 `kill`(안 끝나면 `kill -KILL`)로
정리하고 `ss -lunp` 로 5060·15060·9000·9001·9100·2855·7100·7110 이 비었는지 본다. 그 뒤에 영역을 지운다(돌고 있으면 다시 만든다).

## 3. 부트스트랩 설치 (**사용자 실행 — sudo**)

`install.sh` 는 sudo 가 필요하다(Claude 세션은 비밀번호 프롬프트를 못 연다 — 실제 터미널에서 실행).

```bash
mkdir -p ~/bootstrap && rm -rf ~/bootstrap/cims-bootstrap
tar xzf build/dist/packages/cims-bootstrap-<oam버전>.tar.gz -C ~/bootstrap
sudo ~/bootstrap/cims-bootstrap/install.sh --batch --user cims --port 4419 --server-name media01 \
     --mgmt-ip 121.161.164.48 --admin-pass 1234 --site-dir /mnt/cims/test48
```

확인:

```bash
curl -sk -o /dev/null -w 'OAM %{http_code}\n' https://127.0.0.1:4419/
grep -E 'CimsSiteDir|CimsRuntimeDir|Packages.Dir|Recording.Dir' /opt/cims-agent/modules/oam/current/oam/config.json   # 전부 /mnt/cims/test48/…
ls /mnt/cims/test48/packages                                                                                # oam·agent 시드 패키지
```

## 4. 모듈 설치 (Claude 실행 — API)

```bash
export OAM_URL=https://127.0.0.1:4419 OAM_LOGIN=admin OAM_PASSWORD=1234 M=/mnt/cims/test48/_migration P=build/dist/packages
scripts/oam-deploy.py packages $P/{oam-svc,csc,cmp,cmdp,csp,oam-cims-tester,cims-tester-worker,cspsim}-<버전>.tar.gz   # 저장소는 OAM 이 알려 준다(packages/)
scripts/oam-deploy.py install oam-svc csc --agent media01 --config oam-svc=$M/site_oam-svc.json --config csc=$M/site_csc.json
scripts/oam-deploy.py install cmp cmdp  --agent media01 --config cmp=$M/site_cmp.json --config cmdp=$M/site_cmdp.json
scripts/oam-deploy.py install csp       --agent media01 --config csp=$M/site_csp.json --no-start       # local_nodes 없이는 기동 거부
for c in local_nodes access_services routes remote_nodes route_sets rules rule_sets routing_policies; do
    scripts/oam-deploy.py collection 6 $c --file $M/now/collections/$c.json
done
cp $M/files/csp/cert/csp.pem /opt/cims-agent/modules/csp/current/csp/cert/                         # 이어서 csp start job
scripts/oam-deploy.py install oam-cims-tester cims-tester-worker --agent media01 \
    --config oam-cims-tester=$M/site_oam-cims-tester.json --config cims-tester-worker=$M/site_cims-tester-worker.json
# base oam restart job — oam-svc 를 나중에 설치했으므로 콘솔 서비스 메뉴 승격(OAM 은 정적 디렉터리를 기동 때 해석)
```

- 설치 순서가 배포 id 를 정한다: oam 1 · oam-svc 2 · csc 3 · cmp 4 · cmdp 5 · csp 6 · oam-cims-tester 7 · cims-tester-worker 8, agent 1 = media01.
  계측기 토폴로지 tb48(id 1, `tester/topologies/1.json`)이 `csp_deployment_id: 6` 을 가정한다.
- 모듈 `config.json` 의 영역 경로는 실체화가 채운다(csp `Setup.Recording.Dir` = `/mnt/cims/test48/recordings` 등 — 템플릿 `site_area`).
  계측기 `Tester.DataDir` 는 비어 있으면 `/mnt/cims/test48/tester` 로 유도되므로 overlay 에 넣지 않는다.
- csc 4430 인증서는 OAM-CA 자동발급본을 쓴다(.48 은 실단말 대상이 아니다). csp `cert/csp.pem` 은 패키지 동봉본과 같은 인증서다.
- oam-svc `FmIngest.Port` 는 9010 — 모듈(csp/cmp/cmdp) `Fm.OamPort` 기본과 같다.
- capability 바이너리(csp `cap_net_admin`·워커 `cap_sys_admin`)의 live 판정은 agent 의 `cims-priv proc-exe`(sudo) 폴백으로 한다.

## 5. 검증

```bash
scripts/oam-deploy.py status                                                             # 8개 running/up
curl -sk https://127.0.0.1:4419/api/v1/tester/health -H "Authorization: Bearer $TOK"     # data_dir=/mnt/cims/test48/tester · topologies 1
cims-tester run VOLTE-CALL-BASIC --topology 1 --ht 3 --instances 1
cims-tester run PTT-GROUP-CALL-BASIC --topology 1 --ht 3 --instances 1                    # ${ht} 바인딩 필수
cims-tester run MCDATA-FD-GROUP --topology 1 --ht 3 --instances 1
```

영역에 기록이 쌓이는지 본다:

| 확인 | 기대 |
|---|---|
| `recordings/volte/…/S….d/` | call.json(CSP) + seg_*.rtp(CMP) 가 한 세션 디렉터리에 |
| `recordings/ptt/<그룹>/…/S…_N/` | session.json·events.jsonl·floor.jsonl·seg/ |
| `log/sip/<연/월/일/시>/` | csp·cmp·cmdp·csc·oam 의 msg·flow 5분 버킷 — `log/<연>/` 은 생기지 않는다 |
| `stats/` | `1m/ 1h/ 1d/ 1M/`·`ptt_index/`·`ptt_attempts/` |
| `state/` | `volte/ ptt/`·`stats_rollup.lock` |
| `content/mcdata_fd/` | FD 원본(CSC) |
| `content/announcements/` | 안내음성 라이브러리 — [배포] 뒤 CMP 로그 `announcements: … op /mnt/cims/test48/content/announcements` |
| API | `/api/v1/recordings`(id = 녹취 영역 상대) · `/api/v1/ptt/sessions` · `/api/v1/stats/calls` · 활성 알람 0 |

콘솔: `/deploy/servers` 서버 1·모듈 8, `/service/history/ptt` 이력·녹취 재생, `/test/topologies` tb48.
