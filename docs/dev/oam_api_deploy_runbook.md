# OAM API 배포 절차 — 콘솔 없이 패키지 등록·업그레이드·설정 주입

콘솔([관리 > 릴리스], [docs/user-manual/deployment_workflow.md](../user-manual/deployment_workflow.md))이 부르는 **같은 OAM API** 를
CLI 로 부르는 절차다. 개발 테스트베드(.48, 관리평면 OAM 4419, agent#13 `mgmt-server`)에서 새 빌드를 올릴 때 쓴다.
스크립트 정본은 [`scripts/oam-deploy.py`](../../scripts/oam-deploy.py) — 아래 단계를 그대로 구현한다.

## 0. 전제

- 대상 OAM(4419)이 살아 있고 관리자 계정으로 로그인할 수 있다(`POST /api/v1/auth/login {login_id, password}` → `token`).
- 패키지 tarball 은 **OAM 프로세스가 읽을 수 있는 경로**에 있어야 한다(`POST /packages` 는 업로드가 아니라 `file_path` 등록이다).
  .48 정식 배치(2026-09-23, [testbed_48_site_migration.md](testbed_48_site_migration.md))의 저장소는 `/mnt/cims/test48/runtime/pkg_files` —
  `OAM_PACKAGE_STORE` 환경변수(또는 `packages --store`)로 준다. 배포 id = oam 1 · oam-svc 2 · csc 3 · cmp 4 · cmdp 5 · csp 6 · tester 7 · worker 8.
- 배포 모듈은 agent 가 감독한다. 같은 호스트에 dev OAM(4419)이 떠 있으면 **base oam 배포(dep29, 4445)는 `live_state` 가 늘 up** 이라
  stop 이 409 로 막힌다(agent heartbeat 가 프로세스 이름으로 판정) — 그 모듈은 이 절차로 올리지 않는다.
- `make dist` 뒤 `./cims.sh pkg <module…>` 로 tarball 을 만든다. pkg 는 patch 버전을 자동 bump 하고 source→dist 를 sync 한다
  (콘솔 번들이 바뀌었으면 `oam` 도 함께). 바이너리(csp/cmp/worker)는 `make` 가 최신이어야 한다 — pkg 는 mtime 만 경고한다.

## 1. 한 번에 (스크립트)

```bash
export OAM_URL=https://127.0.0.1:4419 OAM_LOGIN=admin OAM_PASSWORD=<pw>     # 또는 OAM_TOKEN=<login token>

cd build && make dist -j8 && cd ..                                          # 1) dist 갱신
./cims.sh pkg csp csc oam-cims-tester cims-tester-worker                    # 2) 패키징(auto-bump) → build/dist/packages/*.tar.gz

scripts/oam-deploy.py status                                                # 3) 현재 배포(dep id·모듈·버전·live)
scripts/oam-deploy.py packages build/dist/packages/csp-0.2.137.tar.gz \
                               build/dist/packages/csc-0.2.117.tar.gz       # 4) store 복사 + POST /packages → package id
scripts/oam-deploy.py upgrade 34=61 31=62                                   # 5) dep=pkg 순서대로 stop → upgrade → start
scripts/oam-deploy.py upgrade --latest oam-cims-tester cims-tester-worker   #    (모듈 이름으로 — 그 모듈의 최신 등록 패키지)

scripts/oam-deploy.py collection 34 access_services --set country_code=82 --signal   # 6) 설정 컬렉션 주입(선택) + SIGUSR1
scripts/oam-deploy.py install csp cmp --agent media01 --config csp=csp.json           # 새 배포(생성 → install job → overlay → start)
scripts/oam-deploy.py config 6 --file csp.json --restart                              # overlay 저장(update_config job 큐잉) → 재기동
```

`config` 는 `PUT /config` 에 `queue_update: true` 를 준다 — overlay 저장만으로는 노드 `config.json` 이 바뀌지 않고 update_config job 이
실체화해 쓴다(restart job 은 파일을 안 쓴다). `GET /config` 의 password 필드는 `••••••••` 로 마스크되므로 그대로 되돌려 넣지 않는다.

한 모듈 업그레이드는 stop(≈10 s) → upgrade job(설치·심볼릭 current 전환, 30~60 s) → start(≈10 s) 로 **1~2 분**이다. 네 모듈이면 5~8 분.
API 는 job 을 큐에 넣고 바로 돌아오므로 스크립트가 `GET /deployments/{id}` 의 `live_state`·`package_id` 를 3 초 간격으로 폴링한다.

## 2. 단계별 API (스크립트가 부르는 것)

| 단계 | 요청 | 확인 |
|---|---|---|
| 로그인 | `POST /api/v1/auth/login {login_id, password}` | `token` — 이후 `Authorization: Bearer` |
| 배포 목록 | `GET /api/v1/deployments` | `id`·`package_version`·`package_id`·`status`·`live_state`·`install_path` |
| 패키지 등록 | `POST /api/v1/packages {file_path, force?}` | `id`·`module`·`version`. 같은 모듈·같은 버전이 있으면 충돌 — 내용이 바뀌었으면 pkg.json 버전을 올려 다시 패키징(`force` 는 덮어쓰기) |
| 정지 | `POST /api/v1/deployments/{id}/job {job_type: stop}` | `job_id`. `GET /deployments/{id}` 의 `live_state == down` 까지 대기 |
| 업그레이드 | `POST /api/v1/deployments/{id}/upgrade {package_id}` | 패키지 전환(PUT) + upgrade job 큐잉을 서버가 한 번에. `package_id` 생략 = 같은 모듈의 최근 업로드. `GET /deployments/{id}` 의 `package_id` 가 바뀌고 `install_path` 가 새 버전 디렉터리로 |
| 시작 | `POST /api/v1/deployments/{id}/job {job_type: start}` | **upgrade job 은 설치·current 전환까지만 하고 프로세스를 띄우지 않는다**(실측 — live 는 down). `package_id` 가 바뀐 것을 본 뒤 바로 start, up 까지 대기 |
| job 상세 | `GET /api/v1/agents/{agent_id}/jobs/{job_id}` | `result_code`·`stdout`·`stderr` — 실패 원인. (`/jobs/{id}` 경로는 없다) |
| 설정 overlay | `PUT /api/v1/deployments/{id}/config {config: {"Setup.X.Y": v}}` → `POST …/job {job_type: restart}` | 병합·update_config job. 재기동이 필요한 키는 restart |
| 컬렉션 | `GET/PUT /api/v1/deployments/{id}/collection/{name} {records, signal}` | csp `local_nodes`·`access_services`·`routes`·`remote_nodes`·`route_sets`·`rules`·`rule_sets`·`routing_policies`·`acl_policies`. `signal: true` = SIGUSR1 리로드(재기동 없음). 참조 무결성은 검사하지 않는다 — CSP 로그 `references missing` 확인 |
| 롤백 | `POST /api/v1/deployments/{id}/rollback` | 직전 패키지로(upgrade 와 대칭) |

## 3. 배포 뒤 확인

```bash
scripts/oam-deploy.py status                                   # 전부 live=up · 버전
grep -h "version-\|started" build/dist/mgmt-server/csp/current/csp/log/csp_*.log | tail -1   # CSP 기동 줄
curl -sk https://127.0.0.1:4419/api/v1/tester/health -H "Authorization: Bearer $OAM_TOKEN"     # 계측기 컨트롤러 버전·data_dir
curl -s  http://127.0.0.1:7100/health                                                          # 워커 버전
ems/tester/oam/bin/cims-tester --url https://127.0.0.1:4419 --token $OAM_TOKEN run VOLTE-CALL-BASIC --topology 2 --ht 3 --instances 1   # 스모크
```

- 계측기 컨트롤러의 `Tester.DataDir` 은 버전 무관 `runtime/data`(또는 overlay 로 지정한 경로) 라 토폴로지(`topologies/`)·creds·runs 가 업그레이드에 살아남는다. 관리 store 에 남은 옛 토폴로지는 새 컨트롤러가 첫 기동 때 이어받는다.
  새 컨트롤러가 모르는 필드가 있는 토폴로지는 **컨트롤러 업그레이드 뒤** `PUT /api/v1/tester/topologies/{id}` 로 올린다.
- CSC 를 올렸으면 `GET /api/v1/users/{id}` 가 200 인지(csc 프록시 401 이면 JwtSecret 불일치 — csc overlay 의 `CimsAuth.JwtSecret` 이 base 와 같은지).
- csp/cmp 는 `PUT …/config` 로 바꾼 overlay 가 새 버전 디렉터리에 그대로 승계된다(`config.json` 은 버전 밖).

## 4. 함정

- `POST /packages` 는 같은 버전 재등록을 거절한다 — `cims.sh pkg` 의 auto-bump 를 쓰고, `--no-bump` 는 내용이 안 바뀐 재패키징에만.
- `cims.sh pkg` 도중 dev OAM·CSC 가 죽은 적이 있다 — pkg 뒤 `status` 로 4419 가 응답하는지 본다.
- 업그레이드 job 은 프로세스를 띄우지 않는다 — `package_id` 전환 확인 뒤 start job 이 항상 필요하다(스크립트가 처리). up 을 기다리며 시한을 태우지 않는다.
- 컬렉션 PUT 은 **레코드 전체 교체**다. 필드 하나를 바꿀 때도 GET 한 레코드를 수정해 전부 되돌려 놓는다(`--set` 이 그렇게 한다).
- 로그인 토큰 수명은 7 일(`handlers/auth.py` `_TTL_SEC`) — 401 이 나면 JwtSecret 이 바뀐 것(configure 재실행·base 재기동)이니 다시 로그인.
