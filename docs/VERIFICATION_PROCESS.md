# CIMS 검증 절차

> **6단계 (S1~S6) 파이프라인**.
>
> **SSOT**: 이 문서. 실행 가이드는 `docs/VERIFICATION_MANUAL.md`.

---

## 0. 개요

### 0.1 6단계 한눈에

| Stage | 이름 | scope | gate | 평균 |
|---|---|---|---|---|
| **S1** | 정적 검사 | py_compile / eslint / tsc / clang-format / unit test | 코드 위생 | <30s |
| **S2** | 빌드 | preflight + cmake build | 컴파일 통과 | 1~5m |
| **S3** | 스모크 | configure → start dev → 1콜 VoIP/PTT | 빠른 sanity | 1~2m |
| **S4** | 패키지화 | tarball 12종 + manifest.json (SHA-256) | immutability gate | <30s |
| **S5** | 로컬 배포 | TB-OAM → Test-agent → mgmt(oam) → 배포본 OAM 경유 csc + 4 service-server | 배포 절차 회귀 | 3~5m |
| **S6** | 통합 검증 | VoLTE/PTT 음성·영상 + summary | 상용 진입 | 2~3m |

**원칙**:
- **stage gate**: stage N FAIL → stage>N 항목 자동 BLOCKED. `runner._topo_sort` + V2Page 노란 배너.
- **immutability gate**: S4 가 산출한 `packages/manifest.json` 의 SHA-256 을 S5 가 `.deployed-manifest.json` 에 기록 → S6 가 매칭 검증. 배포 후 재패키지화 시 mismatch 로 즉시 차단.
- **registry execution_order**: stage 안의 순서는 `@verify_item(execution_order=N)` 명시. depends_on 이 있으면 그것이 우선. 미명시는 alphabetical fallback.
- **mock 도구화**: `--inject-fail ITEM_ID` 로 강제 FAIL 주입 (gate / 회귀 점검용).

### 0.2 진입점

| 형태 | 명령 / URL |
|---|---|
| CLI | `./cims-verify stage<N>` (1~6) |
| CLI 메타 | `./cims-verify list [--stage N]` / `list-presets` / `describe <ID>` |
| CLI 직접 | `python3 -m tests.cims_verify run --stage N \| --items ID,... \| --preset NAME` |
| Console UI | `http://<ens160>:3000/release/verify` (LIVE 1.5s 폴링) |
| 이력 | `http://<ens160>:3000/release/verify-history` (회차 + 통계 + DetailModal PDF) |
| 빌드/패키징 | `http://<ens160>:3000/release/package` (카드 그리드 — 빌드 & 패키징 통합) |

### 0.3 6단계 디렉토리 / 코드 SOT

```
verify/lib/
├── registry.py       # @verify_item, ItemMeta(execution_order), validate_registry()
├── runner.py         # group/leaf 펼침, BLOCKED, stage_gate, --inject-fail
├── presets.py        # stage1-full ~ stage6-full + pipeline-full + post-deploy
├── context.py / reporting.py / shell.py
├── common/           # csc_http (urllib+TLS skip), db (pymysql), pkg_manifest, ...
└── items/
    ├── stage1/  (5)  # PY-SYNTAX, FRONTEND-LINT/TYPECHECK, CPP-FORMAT, UNIT-VERIFY-LIB
    ├── stage2/  (2)  # PREFLIGHT, BUILD
    ├── stage3/  (7)  # RESET, CONFIGURE, START, SEED, HEALTH, SCN-VOIP-SMOKE, SCN-PTT-SMOKE
    ├── stage4/  (2)  # PKG-BUILD (12 tarball), PKG-MANIFEST (sha256 → packages/manifest.json)
    ├── stage5/  (7부모+13자식)
    │   ├── reset.py / finalize.py (평면)
    │   ├── csc_deploy.py    (group + AGENT-ENROLL/PKG-UPLOAD/INSTALL)
    │   ├── csc_verify.py    (group + FILES/OVERLAY)
    │   ├── csc_run.py       (group + CSC-START/CSC-HEALTH/CONSOLE-START)
    │   ├── modules_deploy.py (group + AUTH/PKG-UPLOAD/AGENT-ENROLL/INSTALL)
    │   ├── modules_run.py   (group + START)
    │   └── _native_steps.py # 22 step (Python 포팅 본체)
    └── stage6/  (7)  # ENTRY-CHECK, SEED, SCN-VOLTE-VOICE/VIDEO, SCN-PTT-VOICE/VIDEO, SUMMARY

tests/
├── cims_verify.py        # CLI (--stage / --items / --preset / --inject-fail)
└── test_verify_lib.py    # 103 unit tests (registry/runner/native steps/parser)

csc/src/handlers/verification.py   # Backend (TB-CSC 4419)
   GET  /stages, /stages/<N>/report, /stages/<N>/reports
   POST /stages/<N>, /run
   GET  /jobs/<id>, /runs, /runs/<id>, /runs/stats, /env
   GET  /items, /presets

ems/core/console/src/
├── api/verification.ts                # API client
├── pages/VerificationV2Page.tsx       # 6단계 LIVE
├── pages/VerificationHistoryPage.tsx  # 이력 list + stats + DetailModal PDF
└── components/VerificationPrintReport.tsx
```

### 0.4 신규 검증 항목 추가 규약

검증 항목은 **명시적 import 없이 자동 노출**된다. `verify/lib/items/__init__.py` 가 `pkgutil.iter_modules` 로 `items/` 하위 패키지를 **재귀 import** 하며, 각 모듈이 import 되는 순간 `@verify_item` 데코레이터가 항목을 전역 `_REGISTRY` 에 적재한다. import 가 끝나면 `validate_registry()` 로 group/parent 무결성을 검사하고 issue 가 있으면 `ImportError` 로 즉시 차단한다 (조용한 실패 방지).

규칙:
- `__` 로 시작하는 파일(`__init__.py`)은 스캔 제외. `_` 로 시작하는 파일(`_helpers.py`·`_native_steps.py` 등)은 helper 로 import 되지만 통상 `@verify_item` 을 갖지 않는다.
- 디렉토리는 `__init__.py` 가 있어야 패키지로 인식되어 재귀 스캔 대상이 된다.
- `id` 는 registry 전역에서 유일해야 한다 (중복 시 `ValueError`). 자식 항목은 `parent="<부모 ID>"`, 부모와 같은 `stage` 여야 한다.

**추가 절차** — 적합한 `verify/lib/items/stage{N}/<cat>/` (또는 `stage{N}/`) 아래에 파일 1개를 만들고 `@verify_item(...)` 데코레이터 + 함수를 작성하면 끝. 별도 등록·import 코드 불필요 → CLI(`cims-verify list`)·Console UI 에 자동 노출된다.

```python
# verify/lib/items/stage3/my_check.py
from ...registry import verify_item, ItemResult, ItemStatus
from ...context import VerifyContext

@verify_item(
    id="S3-MY-CHECK",
    stage=3,
    category="시나리오",
    name="내 신규 점검",
    depends_on=["S3-HEALTH"],
    side_effects=["read-only"],
    timeout_s=30,
    description="한 줄 설명 (미지정 시 함수 docstring 첫 줄)",
)
def my_check(ctx: VerifyContext) -> ItemResult:
    ok = True  # ... 실제 검사 ...
    return ItemResult(
        id="S3-MY-CHECK", name="내 신규 점검",
        status=ItemStatus.PASS if ok else ItemStatus.FAIL,
        detail="...", stage=3,
    )
```

함수는 `ItemResult` 또는 `bool` 을 반환한다. `is_group=True` 부모 항목은 runner 가 자식 실행을 자동 처리하므로 본체는 placeholder 여도 무방하다.

#### `side_effects` 표준 값

`@verify_item(side_effects=[...])` 는 항목이 환경에 끼치는 영향을 선언한다 (UI 배지·실행 위험 표시용). 코드에서 실제 사용 중인 값:

| 값 | 의미 |
|---|---|
| `read-only` | 부작용 없음 — 조회/검사만 (lint, health, 매니페스트 매칭 등) |
| `fs-write` | 파일 시스템 쓰기 (configure, build, 패키지 산출, seed) |
| `db-write` | DB row INSERT/UPDATE |
| `db-truncate` | DB 테이블 비우기 (reset) |
| `network` | 네트워크 호출 (API/배포/원격 enroll) |
| `process-start` | 새 프로세스 기동 (Test-agent 등) |
| `process-kill` | 프로세스 종료 (reset) |
| `process-state` | 프로세스 상태 전환 (failover 시나리오의 stop/start) |
| `service-start` | 서비스 모듈 기동 (csc/csp/cmp/console) |
| `service-state` | 서비스 상태 전환 (finalize stop list 등) |
| `service-signal` | 기동 중 서비스에 시그널 (SIGUSR1 reload) |
| `sim-call` | cspsim 호 발생 (VoIP/PTT 스모크·통합 시나리오) |

---

## 1. Stage 별 상세

### S1 — 정적 검사 (8 항목)

| ID | 검사 | 도구 |
|---|---|---|
| S1-PY-SYNTAX | Python 문법 | `py_compile` (verify/+tests/+csc/+scripts/) |
| S1-CONFIG-PORTABILITY | 배포 설정 이식성 | 빌드 머신 절대경로 금지 |
| S1-FRONTEND-LINT | TS/JS lint | `npm run lint` (ems/core/console — @svc alias 로 서비스 팩 포함) |
| S1-FRONTEND-TYPECHECK | TS 타입 | `npx tsc -b --noEmit` (ems/core/console) |
| S1-CPP-FORMAT | C++ 포맷 | `clang-format --dry-run -Werror` |
| S1-UNIT-VERIFY-LIB | verify.lib 단위 | `python3 -m unittest tests.test_verify_lib` |
| S1-UNIT-HA-INTENT | HA 무장/해제 의도·소유 경계 | `python3 -m unittest tests.test_ha_intent` |
| S1-UNIT-CONSOLE-LAYOUT | 콘솔 레이아웃 영속 계약 | `python3 -m unittest tests.test_console_layouts` |

S1 FAIL → S2~S6 자동 BLOCKED (stage gate).

### S2 — 빌드 (2 항목)

| ID | 작업 |
|---|---|
| S2-PREFLIGHT | `cims.sh preflight` (ens160 IP, 충돌 포트, mariadb up) |
| S2-BUILD | `cmake --build build -j$(nproc)` (depends_on=S2-PREFLIGHT) |

S2 FAIL → S3~S6 BLOCKED.

### S3 — 스모크 (환경 4 + 시나리오 12 + 검증 1, depends_on chain)

`S3-CONFIGURE → S3-START → S3-SEED → {S3-SCN-*} → S3-HEALTH` (S3-RESET 은 `prep-reset` 프리셋으로 분리 — 사용자가 명시 실행).

- **S3-RESET**: `cims.sh reset --all` (가입자 보존, 로그/DB/녹취/배포본 wipe)
- **S3-CONFIGURE**: `configure --local-ip <ens160>`
- **S3-START**: cmp/cmdp/csp/oam/csc/console 순서 기동 (cwrtc/phone 은 재설계 예정 — 제외). **dev 스택은
  build/dist 기본 포트(csp 5060/5061, cmp 9000/9001, oam 4419, csc 4421/4430)를 잡으므로 배포본(라이브)과
  동거하는 박스에서는 라이브를 내린 창에서만 돈다.**
- **S3-SEED**: dev 환경을 라이브 토폴로지와 정합시킨다 — ① `access_services.jsonl` 시드 ② **TLS 접속점
  (5061, dist csc 자가서명) 시드** — TLS 전제 시나리오(sec-agree·AKA over TLS·TLS 재바인드)가 임시 리스너
  없이 돈다 ③ DB 가입자 선택 = **UDP Digest 창**(채널 정책 `sip_transport` TLS/TCP 강제·`auth_scheme=aka`
  가입자 제외 — 그 정책은 전용 시나리오가 본다) + 단말별 자격(`ha1` + IdMS 로그인 `login/loginPw`) ④ csp
  SIGUSR1 리로드 ⑤ dev CSC HTTPS(4421) 워밍업(기동 직후 첫 TLS 요청이 CSP AV 클라이언트 2s 타임아웃을 넘겨
  AKA 제안이 504 로 떨어지는 콜드스타트 흡수)
- **S3-SCN-VOIP-SMOKE**: cspsim VoIP 1콜 (B2BUA, RTP relay, seg_*.rtp +1)
- **S3-SCN-PTT-SMOKE**: cspsim PTT 그룹콜 1회 (5인, multipart INVITE, floor) — `-media_dir tests/media`
  로 AMR-WB 를 offer 한다(없으면 PTT-AS 488), `-no-db -creds -users_from_creds` 로 시드 창을 그대로 전개
- **S3-SCN-SRTP**: 미디어 SRTP 회귀 ([media_security.md §9](design/features/media_security.md)) —
  접속서비스 `media_srtp` 를 required/optional/off 로 플립(SIGUSR1)하며 cspsim `-srtp` 군을 돌린다.
  R1 required 협상+CMP 종단+평문 녹취, R2 평문 offer 488 게이트, R3 optional best-effort 수용,
  R4 off 대조군(자기복원)
- **S3-SCN-CHANNEL-POLICY / REALM-MISMATCH / SEC-AGREE / AKA / IPSEC(-LIVE) / TLS-REBIND / MIXED-TRANSPORT /
  AKA-MIGRATE-IDEMPOTENT / PROVISIONING-HA1**: SIP 접속 보안 회귀(V1~V26) — 항목 정의는
  [sip_access_security.md](design/features/sip_access_security.md) 검증표. IPSEC-LIVE 는 dev 에 IPSEC
  Local Node 가 없으면 SKIP.
- **S3-HEALTH**: csp/cmp `[E]/[F]` + csc `ERROR:/CRITICAL:` 로그 스캔 (파일별 최근 2000행) 0건

### S4 — 패키지화 (2 항목)

- **S4-PKG-BUILD**: `cims.sh pkg --no-bump` → `build/dist/packages/<m>-<ver>.tar.gz` 12종 (base 8: cmp/cmdp/csp/csc/oam/oam-svc/cspsim/agent + 변종 4: psp/isp/pmp/imp)
- **S4-PKG-MANIFEST**: 12 tarball 의 SHA-256 → `packages/manifest.json` (자동 생성). `_self_sha256` 가 S5/S6 immutability gate 의 SoT.

패키지 포맷·변종 staging·빌드 흐름은 [`design/features/build_and_packaging.md`](design/features/build_and_packaging.md) + [`design/features/package_and_template.md`](design/features/package_and_template.md) 참조.

### S5 — 로컬 배포 (7 부모 + 13 자식, 22 native step, mgmt + 5 service-server topology)

#### 토폴로지 (server 단위 분리) — SSOT

mgmt 체인은 **standalone OAM**(02_deployment §2.1 — 콘솔 SPA 정적 동봉, 관리 API 단일
오리진)을 배포하고, 이후 모든 모듈(csc 서비스 모듈 + service-server)은 **배포본 OAM 경유**로
배포한다 (oam_csc_split — auth/agents/packages/deployments 는 OAM 소유, csc 는 게이트웨이 뒤
서비스 모듈). `_INSTANCES` (`verify/lib/items/stage5/_native_steps.py`) 가 모듈 인스턴스의 SoT:

| display_name | agent_name | dist 디렉토리 | 역할 / 변종 |
|---|---|---|---|
| CIMS 관리 서버 | `mgmt-server` | `<dist>/mgmt-server/` | oam (관리평면 4445, console 동봉) + cspsim (install-only) |
| Mgmt Service (CSC) | `mgmt-svc-server` | `<dist>/mgmt-svc-server/` | csc (variant=CSC, 4446 — 게이트웨이 뒤 서비스 모듈) |
| VoLTE SIP Server | `volte-sip-server` | `<dist>/volte-sip-server/` | csp (variant=CSP) |
| VoLTE Media Server | `volte-media-server` | `<dist>/volte-media-server/` | cmp (variant=CMP) |
| PTT SIP Server | `ptt-sip-server` | `<dist>/ptt-sip-server/` | psp (variant=PSP) |
| PTT Media Server | `ptt-media-server` | `<dist>/ptt-media-server/` | pmp (variant=PMP) |

각 service-server 는 **base 바이너리 동일 + Roles 토글 + LocalIp/Port overlay** 로 인스턴스화 (deployment overlay = `install_path/<모듈>/config.json` → 모듈 config 시작 직전 머지). 설치 레이아웃은 버전형 (`<agent>/modules/<모듈>/<ver>/` + `current` 심볼릭). csp 변종의 DB 접속·`Setup.Csc.*`(배포본 csc admin), csc 의 `CimsDatabase.*`·`McpttServer.{Port,PublicUrl}`(4431)·`InternalApi.Token`·notify 대상은 step 19 가 overlay 로 주입 — 운영에서 블루프린트/콘솔 설정 단계가 담당하는 값을 verify 체인에서는 하네스가 담당한다. csp 변종의 SIP 리스너 SoT(`local_nodes.jsonl` primary) 는 step 21 이 시작 전 non-clobber 시드. loopback alias (127.0.0.2 / 127.0.0.3 등) 는 `verify/lib/common/loopback.py` 가 관리.

verify Test-agent 는 `CIMS_AGENT_NO_SUPERVISE=1` 로 spawn 한다 — `_PREFIX`(build/dist)/run 의 dev pid 파일이 감독 집합에 seed 되어 dev 스택(oam 등)과 재기동 경쟁하는 사고 방지. `cims.sh reset` 이 `<dist>/run/supervised.json` 잔재도 정리한다.

#### 항목 트리 (execution_order 순)

```
S5-RESET                      (10) — step 01: cleanup
S5-CSC-DEPLOY                 (20) ─ group  (mgmt-server: oam + sim 부트스트랩)
  ├─ S5-CSC-DEPLOY-AGENT-ENROLL (21) — step 05+06+07: TB-OAM admin login + agent + Test-agent (mgmt-server, sync 9903)
  ├─ S5-CSC-DEPLOY-PKG-UPLOAD   (22) — step 08: oam/cspsim 2 tarball multipart upload
  └─ S5-CSC-DEPLOY-INSTALL      (23) — step 09+10: 2 deployment 생성 (overlay: Server.Port=4445 + runtime/packages/log 격리) + install poll 60s
S5-CSC-VERIFY                 (30) ─ group
  ├─ S5-CSC-VERIFY-FILES        (31) — step 11: meta.json + config/ 존재 (current 통로 해석)
  └─ S5-CSC-VERIFY-OVERLAY      (32) — step 12: oam/config.json Server.Port=4445
S5-CSC-RUN                    (40) ─ group
  ├─ S5-CSC-RUN-CSC-START       (41) — step 13: oam Start + 4445 LISTEN (25s)
  ├─ S5-CSC-RUN-CSC-HEALTH      (42) — step 14: health_check job + job 상태 폴링 (15s)
  └─ S5-CSC-RUN-CONSOLE-START   (43) — step 15: console 정적 서빙 확인 (GET / → SPA HTML)
S5-MODULES-DEPLOY             (50) ─ group  (배포본 OAM 4445 경유, csc + 4 service-server)
  ├─ S5-MODULES-DEPLOY-AUTH       (51) — step 16: 배포본 OAM admin login → tok2
  ├─ S5-MODULES-DEPLOY-PKG-UPLOAD (52) — step 17: csc/csp/cmp/psp/pmp tarball upload (_INSTANCES iteration)
  ├─ S5-MODULES-DEPLOY-AGENT-ENROLL (53) — step 18: agent + Test-agent (sync 9904~9909)
  └─ S5-MODULES-DEPLOY-INSTALL    (54) — step 19+20: deployment (overlay 주입) + install poll
S5-MODULES-RUN                (60) ─ group
  └─ S5-MODULES-RUN-START         (61) — step 21: csc(4446) + 시그널링↔미디어 두 pair (csp↔cmp + psp↔pmp)
                                          local_nodes 시드 + OnCmpStatusChanged Connected wait + .deployed-manifest.json marker
S5-FINALIZE                   (70) — step 22: 기본 기동 유지 / --stop-after 시 모듈(배포본 OAM 경유) → mgmt 순 stop + agent kill
```

Build/Configure/Pkg 는 S2/S3/S4 가 담당하므로 S5 step 에서 제외.

#### 공유 상태 (`ctx.state["_s5_native"]`)

```
{
  "results": {step_no: ItemResult},                 # cache (idempotent)
  # mgmt 체인 (TB-OAM 4419 → oam + sim 부트스트랩)
  "tok", "aid_csc", "enroll_tok_csc", "ta_pid_csc",      # 05~07 (mgmt agent)
  "pkg_id_oam", "pkg_id_sim",                             # 08
  "dep_id_oam", "dep_id_sim",                             # 09
  "all_install_done_csc",                                 # 10
  "mgmt_start_ok", "mgmt_health_ok", "console_static_ok", # 13~15
  # modules 체인 (배포본 OAM 4445 — csc + service-server)
  "tok2",
  "aid2_<id>", "enroll_tok2_<id>", "ta_pid2_<id>",        # 18
  "pkg2_id_<id>",                                         # 17
  "dep2_id_<id>",                                         # 19
  "all_install_done_modules", "modules_start_ok",         # 20, 21
}
```

#### 인프라 헬퍼 (`verify/lib/common/`)

- `csc_http.py` — TB-CSC API client. urllib + TLS skip. `admin_login` / `get_json` / `post_json` / `delete` / `post_multipart` / `list_agents` / `find_agent_id_by_name`.
- `db.py` — `csp_db_config(dist_dir)` + `connect(cfg)` (pymysql).
- `service_log.py` — `service_log_roots(dist_dir)`. 녹취·flow 카운터의 탐색 루트. dist 설정
  (`cmp.json`/`csp.json` 의 `ServiceLogging.Dir`)을 읽어 **설정된 경로**를 쓰고 기본
  `<dist>/ext_mnt/service_log` 는 폴백으로 둔다 — `configure --service-log-dir` 로 공유 NAS
  경로를 쓰는 환경에서 기본 경로만 보면 서비스가 정상인데도 "녹취 파일 없음" 으로 오판한다.
- `recordings.py` — `count_recordings` / `count_ptt_events`. 위 루트 기준 delta 카운트.
- `subscribers.py` — `select_subscribers(db_cfg, voip_count, ptt_count)`. **cspsim 은 시작
  가입자의 비밀번호 하나로 `-count` 명을 시뮬레이션**하므로(번호만 +1, auth_id 는 계정별 파생)
  "번호 연속 + 비밀번호 동일" 구간의 첫 가입자를 고른다(`pick_start_subscriber`). 일부 계정만
  비밀번호가 다르면 2번째 단말부터 REGISTER 403(digest 불일치)이 나 제품 결함처럼 보인다.
  조건을 만족하는 구간이 없으면 첫 가입자로 폴백.
- `pkg_manifest.py` — `write_marker(dist_dir)` (S5 immutability) / `immutability_check(dist_dir)` (S6).

#### 옵션

- `--stop-after`: step 22 가 stop jobs + Test-agent 4개 SIGTERM. 기본은 4 ports (4445/8081/5060/9000) 기동 유지.
- `--inject-fail S5-...`: 디버그용 강제 FAIL.

### S6 — 통합 검증 (7 항목)

- **S6-ENTRY-CHECK** — mgmt OAM(4445) + _INSTANCES 포트 LISTEN + immutability gate 매칭 (S5-MODULES-RUN-START 가 기록한 `.deployed-manifest.json` ↔ 현재 `packages/manifest.json` SHA-256).
- **S6-SEED** — 배포본 csp jsonlDir 에 access_services.jsonl 시드 + SIGUSR1.
- **S6-SCN-VOLTE-VOICE / VOLTE-VIDEO / PTT-VOICE / PTT-VIDEO** — cspsim 4시나리오. 각 `seg_*.rtp` 녹취 +1 이상 → PASS.
- **S6-SUMMARY** — 4 시나리오 결과 합산 + 로그 ERROR/FATAL 카운트.

S6 PASS → ✅ 상용 배포 가능.

---

## 2. 합격 기준 / Gate

### 2.1 stage_gate (자동 차단)

`runner.run_items` 에 `stage_gate=True` (기본). stage N 의 항목 1개라도 FAIL → stage>N 의 모든 leaf 가 함수 호출 없이 `BLOCKED` 처리. `[VERIFY] stage-blocked: stage=M reason=stageN-FAIL count=K` stdout 마커 emit. backend `_parse_items_progress.stage_gate` 가 파싱 → V2Page 노란 배너.

비활성: `stage_gate=False` (단일 stage 실행 시는 무의미).

### 2.2 immutability_gate (S6 진입)

S4-PKG-MANIFEST 가 `packages/manifest.json` 작성 → S5-MODULES-RUN-START PASS 시 sha256 을 `.deployed-manifest.json` 에 기록 → S6-ENTRY-CHECK 가 두 sha 매칭 검증. 불일치 시 FAIL + 복구 절차 안내.

### 2.3 합격 기준 (공통)

- 빌드: warning/error 0
- 런타임 로그: ERROR/FATAL 0
- Stage 별 항목 전부 PASS
- Flow/Msg 로그 무결성 (sesid 일관, body seq 매칭)

---

## 3. 진행 상태 + 이력

### 3.1 stdout 마커 (LIVE)

```
[VERIFY] run-start: total=N ids=...
[VERIFY] item-start: <id> stage=<N> idx=i/N name=<...>
[VERIFY] item-end:   <id> status=PASS|FAIL|SKIP|BLOCKED elapsed_ms=<n>
[VERIFY] child-result: <parent>.<child> status=... elapsed_ms=... name=...
[VERIFY] group-end:  <parent> status=... child_count=...
[VERIFY] stage-blocked: stage=M reason=stageN-FAIL count=K
[VERIFY] run-end: total=N pass=p fail=f skip=s blocked=b
```

backend `_parse_items_progress(log_path)` 가 폴링으로 파싱.

### 3.2 회차 이력

각 job 종료 시 회차 record 를 파일로 저장 — `verify.lib.run_store` 가 `verify_runs/YYYY/MM/<id>.json` 1 파일로 기록(record/list/get/stats 모두 파일시스템 스캔). `pkg_manifest_hash` 필드가 S6 immutability gate 의 SoT. API:

- `GET /api/v1/verification/runs?days=N&scope=...&verdict=...&limit=...`
- `GET /api/v1/verification/runs/<id>` — 회차 + 항목 결과 + manifest hash
- `GET /api/v1/verification/runs/stats?days=N` — overall + by_scope + timeline

UI: `/release/verify-history` — KpiGrid + ScopeTable + inline-SVG Sparkline + DetailModal PDF.

---

## 4. 실패 / 디버깅

### 4.1 stage gate 차단

V2Page 노란 배너 + history page row 에 BLOCKED. 차단 원인 stage 의 FAIL 항목 detail 확인 → 해당 stage 만 재실행 후 잔여 자동 진행.

### 4.2 immutability gate 불일치

S6-ENTRY-CHECK FAIL 시 detail 에 두 sha 출력. 복구:
1. `cims-verify stage4` — manifest 재생성
2. `cims-verify stage5` — 재배포 + marker 갱신
3. `cims-verify stage6` — 재진입

### 4.3 디버그용 강제 FAIL 주입

```bash
./cims-verify stage1 --inject-fail S1-CPP-FORMAT
# stdout: "강제 FAIL 주입 (--inject-fail)" detail
```

backend 도 옵션 통과: `POST /stages/1` body `{"inject_fail": ["S1-CPP-FORMAT"]}`.

### 4.4 native step 단위 재실행

ctx.state cache 덕분에 idempotent. CLI 로 자식만 실행:
```bash
python3 -m tests.cims_verify run --items S5-CSC-DEPLOY-PKG-UPLOAD
```

---

## 부록 A. 디렉토리 레이아웃

```
build/dist/
├── csc/ csp/ cmp/ cmdp/ oam/ oam-svc/ console/ cspsim/   # S2/S3 직접 기동 대상
├── packages/                                       # S4 산출물 (12 tarball + manifest.json)
├── mgmt-server/                                    # S5 mgmt 체인 (csc + console + sim 모두 흡수)
│   └── {agent, csc, console, sim, config/}
├── volte-sip-server/   {agent, csp, config/}      # S5 service 체인 (4)
├── volte-media-server/ {agent, cmp, config/}
├── ptt-sip-server/     {agent, psp, config/}
└── ptt-media-server/   {agent, pmp, config/}
```

각 `<server>/` 내부: `agent/` (cims_agent + state + 발급 cert) · `<모듈>/` (pkg.json + config.json overlay + modules/**) · `config/` (collection jsonl).

## 부록 B. 포트 매핑

| 구분 | 서비스 | 포트 |
|---|---|---|
| **TB** (상시) | TB-OAM (구 TB-CSC — deprecated) | 4419 |
| | TB-Console | 3000 |
| **S2/S3 직접 기동 (Test-\*)** | Test-OAM (dev 는 role=all 로 4419 겸함) | 4419 |
| | Test-CSC | 4421 |
| | Test-Console (dist) | 8080 |
| | Test-CSP | 5060/udp + 5061/tls + 25061/tcp |
| | Test-CMP | 9000/udp + RTP 풀 |
| **S5 배포본 (verify)** | mgmt oam (관리평면 + console 정적) | **4445** |
| | mgmt csc (서비스 모듈, 게이트웨이 뒤) | **4446** (MCPTT 4431) |
| | volte-sip (csp) | 5060/udp · LocalIp 127.0.0.1 |
| | volte-media (cmp) | 9000/udp · 127.0.0.1 |
| | ptt-sip (psp) | 5060/udp · LocalIp 127.0.0.2 (loopback alias) |
| | ptt-media (pmp) | 9000/udp · 127.0.0.3 |
| | Test-agent | sync 9903~9909 (mgmt 9903, csc 9909, service 9904~9908) |
| **운영 배포본 (참고)** | csc | 4421 |
| | console | 80 |

## 부록 C. 알려진 함정

- 빌드 시점에 `-v X.Y.Z` 로 모든 base `pkg.json` 동기 → `pkg --no-bump` 가 권장 흐름. 옛 `cims.sh pkg` patch +1 auto-bump 는 변종 drift 위험.
- `make dist` 이후 반드시 `configure.sh` 재실행 (IP 반영).
- localhost 로는 외부 접근 불가 — 반드시 `ens160` IP.
- cspsim REGISTER: `-auth_id "IMSI@domain"` 필수 (verify CLI 는 DB 자동 조회).
- TB-CSC mTLS 모드: `Agent.MtlsEnabled: true` overlay 필수.
- 같은 호스트 다중 agent: `CIMS_AGENT_SYNC_PORT` env 주입 (CLI 미지원).
- TB-Console (3000) 만 vite dev 모드. dist 정적 서빙 (8080/8081) 은 `/api` proxy 없음 → 로그인 불가.

## 부록 D. 문서 관리

- 본 문서는 검증 절차의 SSOT. 변경 이력은 git 관리.
- 진행 중 보완은 검증 리포트에 먼저 기록 후 본 문서에 반영.
- 사용자 용어 정의 변경 시 §0.1 표 + 부록 B 포트 매핑 동기화 필수.
