"""CIMS OAM — Operation & Management 프로세스 엔트리포인트.

Phase 3 (분리): 별도 프로세스 + 별도 포트 (default 4419).

OAM 책임:
- Agent 레지스트리 / heartbeat / metric
- HA 그룹 lifecycle
- 패키지 / 배포 / 빌드 / 검증
- 모니터링 통계 / 알람 / 녹취 조회
- Admin JWT 발급 + 사용자 본인 정보

같이 들어있지 않은 책임 (CSC 가 담당):
- 가입자 (VoLTE/PTT) CRUD
- 조직 (organizations)
- MCPTT IdMS/GMS/CMS/KMS (UE 통신)
- CSP 가입자 데이터 notify_csp

공유 라이브러리는 csc/src/services 에 그대로 (admin_auth, mcptt.notify_csp 등) — sys.path 에 csc/src 도 mount.
"""

import argparse
import os
import sys
import time
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))  # = oam/
_CONFIG_PATH = os.environ.get('CIMS_OAM_CONFIG') or os.path.join(_COMPONENT_ROOT, 'config', 'oam.json')

# ── Phase 4 vendor: private 환경 (인터넷 없음) 대응 ──
# oam/vendor/ 에 사전 다운로드된 fastapi/uvicorn/pymysql/PyJWT/loguru/requests/
# readerwriterlock + OAM 전용 aiohttp/netifaces/strenum/asyncstdlib 등.
# 빌드 시점: 'pip3 install --target=oam/vendor -r oam/requirements.txt --no-compile'
_VENDOR = os.path.normpath(os.path.join(_COMPONENT_ROOT, 'vendor'))
if os.path.isdir(_VENDOR) and _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)

# CSC 공유 라이브러리 (services.mcptt / services.admin_auth / services.flow_logger /
# services.config_cache / services.drift_sweeper / services.sync_txn / services.alert_log /
# httpsrv / util) 를 import 하기 위해 sys.path 에 csc/src mount.
# Phase 4 vendor: agent install 환경 (install_path/csc/<ver>/csc/src) 도 glob 검색.
_CSC_SRC = None
_csc_candidates = [os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'csc', 'src'))]
import glob as _glob
_csc_glob = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'csc', '*', 'csc', 'src'))
_csc_candidates += sorted(_glob.glob(_csc_glob), reverse=True)
# 버전 단위 설치 레이아웃: oam=<root>/oam/<ver>/oam → csc=<root>/csc/<ver>/csc/src 는 3-up.
_csc_glob3 = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', '..', 'csc', '*', 'csc', 'src'))
_csc_candidates += sorted(_glob.glob(_csc_glob3), reverse=True)
for _c in _csc_candidates:
    if os.path.isdir(_c):
        _CSC_SRC = _c
        break
if _CSC_SRC and _CSC_SRC not in sys.path:
    sys.path.insert(0, _CSC_SRC)

from httpsrv.server import HttpServer
from util.log_util import Logger


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    # D3 (oam self-upgrade pre-flight): 핸들러 import + config 로드만 검증하고 bind 없이
    # 즉시 종료. agent 가 구 OAM 을 내리기 전에 신 OAM 패키지가 뜰 수 있는지 확인하는 용도.
    parser.add_argument('--preflight', action='store_true',
                        help='import + config 스모크만 수행하고 (bind 없이) 종료 — self-upgrade 검증용')
    # oam_base_service_split P0: base/service 핸들러 그룹 분기.
    #   all  = 현행 단일프로세스 (게이트웨이 + 공통 + in-process 서비스 핸들러 전부) — 기본·하위호환(I4)
    #   base = 게이트웨이 + 공통 관리만. 서비스(가입자/녹취/flow/검증/KPI)는 미등록 → 독립 모듈 귀속.
    parser.add_argument('--role', choices=['base', 'all'], default='all',
                        help='base = 게이트웨이+공통 관리만 / all = 현행 단일프로세스(기본)')
    args_dict = vars(parser.parse_args())

    logger = Logger(log_dir=os.path.join(_COMPONENT_ROOT, "log"), log_file_prefix="app", retention_day=30)

    import json
    def _apply_overlay(root: dict, flat: dict) -> int:
        applied = 0
        for key, val in flat.items():
            cur = root
            parts = str(key).split('.')
            for p in parts[:-1]:
                if p not in cur or not isinstance(cur[p], dict):
                    cur[p] = {}
                cur = cur[p]
            cur[parts[-1]] = val
            applied += 1
        return applied

    def _deep_merge(dst: dict, src: dict) -> dict:
        for k, v in (src or {}).items():
            if isinstance(v, dict) and isinstance(dst.get(k), dict):
                _deep_merge(dst[k], v)
            else:
                dst[k] = v
        return dst

    def _load_base_config():
        """oam_base_service_split §7 — 설정 분리(비파괴).
          common.json 존재 시 = common.json + base.json (공통+base 전용 병합).
          부재 시            = oam.json (현행 단일 설정, 하위호환).
        반환: (config_dict, source_label)."""
        cfg_dir = os.path.dirname(_CONFIG_PATH)
        common_p = os.path.join(cfg_dir, 'common.json')
        base_p = os.path.join(cfg_dir, 'base.json')
        if os.path.isfile(common_p):
            merged: dict = {}
            with open(common_p, 'r') as f:
                _deep_merge(merged, json.load(f))
            if os.path.isfile(base_p):
                with open(base_p, 'r') as f:
                    _deep_merge(merged, json.load(f))
            return merged, f'common.json+base.json ({cfg_dir})'
        with open(_CONFIG_PATH, 'r') as f:
            return json.load(f), _CONFIG_PATH

    def load_config():
        try:
            c, _src = _load_base_config()
            logger.log_info(f"OAM config source: {_src}")
        except FileNotFoundError:
            logger.log_error(f"Config file not found at {_CONFIG_PATH}")
            return {}
        try:
            for overlay in (
                os.path.join(_COMPONENT_ROOT, 'config.json'),
                os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'config.json')),
            ):
                if not os.path.isfile(overlay):
                    continue
                with open(overlay, 'r') as f:
                    flat = json.load(f)
                if isinstance(flat, dict) and flat:
                    n = _apply_overlay(c, flat)
                    logger.log_info(f"OAM overlay applied: {overlay} ({n} keys)")
                    break
        except Exception as e:
            logger.log_error(f"OAM overlay failed: {e}")
        return c

    from services       import flow_logger, logger as csc_logger, config_cache, alert_log
    from handlers       import auth, recording
    from handlers.auth           import CIMS_AUTH_HANDLER_LIST
    # /users/me (본인 프로파일) = identity-plane, base 귀속(oam_base_service_split D8).
    # 콘솔 로그인 부트스트랩의 필수 경로 → base 가 항상 직접 등록(slim 핸들러).
    from handlers.users          import CIMS_USERS_HANDLER_LIST as CIMS_ME_HANDLER_LIST
    # 가입자/PTT그룹 CRUD — admin.handle_users 는 /users/me 를 users.handle_users 로 위임하는
    # superset 이라, /me(인증) + 가입자 admin(관리) 를 한 핸들러로 커버. (OAM=콘솔 단일 게이트웨이)
    # admin(가입자/계정 CRUD)·org 는 csc(서비스 모듈) 측 핸들러 — 부트스트랩
    # standalone OAM 패키지에는 서비스 모듈이 없으므로 선택 로드. 서비스 설치
    # (3단계 이후, csc 모듈 배포) 후 OAM 재기동 시 자동 활성.
    # P0: 이 핸들러들은 SERVICE 그룹 → --role all 에서만 등록(--role base 는 제외).
    try:
        from handlers.admin      import CIMS_ADMIN_HANDLER_LIST
        from handlers.org        import CIMS_ORG_HANDLER_LIST
    except Exception as _e:
        print(f'[bootstrap] subscriber/org handlers unavailable (csc 미설치): {_e}', flush=True)
        # csc 미설치 시 admin superset 부재 → base 의 slim /me 핸들러가 /users/me 를 커버.
        CIMS_ADMIN_HANDLER_LIST = []
        CIMS_ORG_HANDLER_LIST = []
    from handlers.recording      import CIMS_RECORDING_HANDLER_LIST
    from handlers.stats          import CIMS_STATS_HANDLER_LIST, CIMS_STATS_SERVICE_HANDLER_LIST
    from handlers.verification   import CIMS_VERIFICATION_HANDLER_LIST, init as ver_init
    from handlers.build          import CIMS_BUILD_HANDLER_LIST, init as build_init
    from handlers.service_control import CIMS_SERVICE_CONTROL_HANDLER_LIST
    from handlers.agents         import CIMS_AGENT_ADMIN_HANDLER_LIST, CIMS_AGENT_PUBLIC_HANDLER_LIST
    from handlers.agent_api      import CIMS_AGENT_API_HANDLER_LIST
    from handlers.modules        import CIMS_MODULES_HANDLER_LIST
    from handlers.ha_groups      import CIMS_HA_GROUPS_HANDLER_LIST
    from handlers.alerts         import CIMS_ALERTS_HANDLER_LIST
    from handlers.console        import CIMS_CONSOLE_HANDLER_LIST
    from handlers.console_accounts import CIMS_CONSOLE_ACCOUNTS_HANDLER_LIST
    from handlers.service_descriptors import CIMS_SERVICE_DESCRIPTORS_HANDLER_LIST
    from handlers.external_systems     import CIMS_EXTERNAL_SYSTEMS_HANDLER_LIST
    from handlers.gateway              import CIMS_GATEWAY_HANDLER_LIST, register_gateway
    from services import service_registry
    from services.flow_logger    import FLOW_HANDLER_LIST

    admin_server = None
    try:
        logger.log_info(f'==================== start (OAM) ====================')

        config = load_config()
        auth.init(config)

        # D3: --preflight 모드 — 여기까지 왔으면 핸들러 import(107~140) + config 로드 OK.
        # bind/마이그레이션/sweeper 없이 즉시 종료(0). agent 가 구 OAM kill 전에 호출.
        if args_dict.get('preflight'):
            if not config:
                print('OAM_PREFLIGHT_FAIL: empty config', flush=True)
                sys.exit(2)
            logger.log_info('[preflight] handler imports + config OK — exit 0 (no bind)')
            print('OAM_PREFLIGHT_OK', flush=True)
            sys.exit(0)

        # runtime store v2 — 구 평면 도메인 1회 이행 (도메인 접근 전 선행).
        #   P2: OAM 자기 데이터 → control/·console/.   P3: 컬렉션 → modules/<owner>/runtime.
        try:
            from services import file_store as _fs
            _m2 = _fs.migrate_oam_categories(config)
            if _m2:
                logger.log_info(f"runtime store v2 P2: OAM 자기 {_m2} 도메인 control/·console/ 이행")
        except Exception as _e:
            logger.log_warning(f"runtime store v2 P2 이행 skip: {_e}")
        try:
            from services import ha_lookup as _ha_lookup
            _moved = _ha_lookup.migrate_flat_collections(config)
            if _moved:
                logger.log_info(f"runtime store v2 P3: 컬렉션 {_moved} 도메인 네임스페이스 이행")
        except Exception as _e:
            logger.log_warning(f"runtime store v2 collection 이행 skip: {_e}")

        # Phase 4d — Mgmt.Cidr 검증 + 명시 로깅.
        # mgmt 대역은 oam 운영의 기준선:
        #  - agent enroll/heartbeat 시 interface.role='mgmt' 자동 매핑 근거.
        #  - install_command (AgentOamUrl) 의 host IP 가 이 대역 안이어야 정합.
        import ipaddress as _ipaddress
        _mgmt = (config.get('Mgmt') or {})
        _mgmt_cidr_raw = (_mgmt.get('Cidr') or '').strip()
        _mgmt_net = None
        if _mgmt_cidr_raw:
            try:
                _mgmt_net = _ipaddress.ip_network(_mgmt_cidr_raw, strict=False)
                logger.log_info(f"Mgmt.Cidr = {_mgmt_cidr_raw} (oam 운영 mgmt 대역)")
                # AgentOamUrl 의 IP 가 mgmt 대역 안인지 검증.
                from urllib.parse import urlparse
                _aou = ((config.get('Server') or {}).get('AgentOamUrl') or '').strip()
                if _aou:
                    try:
                        _aou_ip = _ipaddress.ip_address(urlparse(_aou).hostname or '')
                        if _aou_ip in _mgmt_net:
                            logger.log_info(f"AgentOamUrl host {_aou_ip} ∈ {_mgmt_cidr_raw} OK")
                        else:
                            logger.log_error(
                                f"⚠ AgentOamUrl host {_aou_ip} ∉ Mgmt.Cidr {_mgmt_cidr_raw} — "
                                f"install_command 의 URL 이 mgmt 대역 밖. 정합 점검 필요.")
                    except (ValueError, TypeError):
                        pass
            except ValueError as _e:
                logger.log_error(f"Mgmt.Cidr 잘못된 형식: {_mgmt_cidr_raw} ({_e})")
        else:
            logger.log_info("Mgmt.Cidr 미설정 — agent 자율 mgmt 도출만. 명시 권장.")
        # config 에 정규화된 mgmt_net 캐시 (handlers 가 사용 가능).
        config['_mgmt_net'] = _mgmt_net

        sl = config.get("ServiceLogging", {})
        _service_log_dir = sl.get("Dir", "")
        if not _service_log_dir:
            _service_log_dir = config.get("ServiceLogDir", config.get("MsgLogDir", ""))
        _system_id = config.get("SystemId", "oam_01")

        # 신뢰망(trusted_nets): 비정상 세션 탐지에서 '외부' 제외 대상.
        #   mgmt CIDR + config 에 등장하는 우리 공인 IP(VIP 등)의 /24 + Security.TrustedNets override.
        #   (우리 노드/VIP 의 공인 IP 가 자기 트래픽으로 오탐되는 것을 방지)
        _trusted = []
        try:
            import re as _re2, ipaddress as _ip2
            _mc = (config.get('Mgmt') or {}).get('Cidr')
            if _mc:
                _trusted.append(_mc)
            _blob = json.dumps(config, default=str)
            for _ipstr in set(_re2.findall(r'\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}', _blob)):
                try:
                    _a = _ip2.ip_address(_ipstr)
                    if not (_a.is_private or _a.is_loopback or _a.is_unspecified):
                        _trusted.append(str(_ip2.ip_network(_ipstr + '/24', strict=False)))
                except Exception:
                    pass
            _trusted += list((config.get('Security') or {}).get('TrustedNets') or [])
        except Exception:
            pass
        flow_logger.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
            db_config=config.get('CimsDatabase'),
            trusted_nets=_trusted,
        )

        tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        if not os.path.isdir(tests_dir):
            tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        ver_init(tests_dir, config)
        build_init(os.path.dirname(tests_dir))

        # system_id 명시 — OAM 콘솔/admin flow 는 oam_01 로 기록(같은 호스트의 CSC xcap flow=csc_01 와 파일 분리).
        #   (미지정 시 둘 다 csc_01.flow 로 써서 seq·라인 충돌)
        csc_logger.init(service_log_dir=_service_log_dir, system_id=_system_id)

        # 녹취 변환툴(ffmpeg) — air-gapped(private) 환경 대응으로 OAM 패키지에 번들된
        # vendor 바이너리를 우선 사용. (패키지화 시 oam/vendor/bin/ffmpeg 또는
        # oam/vendor/ffmpeg 로 동봉할 것.) 없으면 config.FFmpegBin → 시스템 PATH 순.
        _ffmpeg_bin = ''
        for _cand in (os.path.join(_VENDOR, 'bin', 'ffmpeg'),
                      os.path.join(_VENDOR, 'ffmpeg'),
                      config.get('FFmpegBin', '')):
            if _cand and os.path.exists(_cand):
                _ffmpeg_bin = _cand
                break
        # 변환 워커 수 — 동시 ffmpeg 실행 상한(CPU 보호). config 로 조정(기본 2).
        try:
            _tx_workers = max(1, int(config.get('RecordingTranscodeWorkers', 2) or 2))
        except (TypeError, ValueError):
            _tx_workers = 2
        recording.init(service_log_dir=_service_log_dir, ffmpeg_bin=_ffmpeg_bin,
                       transcode_workers=_tx_workers)

        # ── pi_http 요청 로깅 훅 등록 (admin/console 자동 로깅) ──
        from httpsrv.controller import DynamicRouteProc

        def _extract_caller(handler_args) -> str:
            try:
                payload = auth.extract_token(handler_args)
                if payload:
                    return payload.get("login_id") or str(payload.get("sub", "")) or ""
            except Exception:
                pass
            qp = handler_args.query_params or {}
            if qp.get("user_name"): return qp["user_name"]
            body = handler_args.body or {}
            if isinstance(body, dict):
                if body.get("login_id"): return body["login_id"]
                if body.get("user_name"): return body["user_name"]
            return ""

        def _post_hook(handler_args, base_path, handler_result):
            try:
                if not handler_args.full_path or handler_args.full_path == "/health":
                    return
                # OAM 은 admin console / agent API 만 — service 는 "console" 로 통일
                service = "console"
                caller = _extract_caller(handler_args)
                peer = f"{getattr(handler_args,'client_ip','')}:{getattr(handler_args,'client_port','')}"
                status = getattr(handler_result, "status", 0)
                mname = f"{handler_args.method} {handler_args.full_path}"
                detail = f"status={status}"
                csc_logger.log_flow(
                    service=service,
                    from_actor="ue", to_actor="oam",
                    proto="HTTPS", method=mname,
                    detail=detail,
                    iface="ue", peer=peer,
                    caller=caller,
                )
            except Exception as e:
                logger.log_error(f"post_hook error: {e}")

        DynamicRouteProc.set_request_hooks(pre=None, post=_post_hook)

        # CSP 런타임 설정 캐시 (DB→mem→file). DB 장애 시 파일 캐시로 read-only 모드 작동.
        _cache_path = config.get('ConfigCacheDir')
        if _cache_path and not os.path.isabs(_cache_path):
            _cache_path = os.path.normpath(os.path.join(_COMPONENT_ROOT, _cache_path))
        if not _cache_path:
            _cache_path = os.path.normpath(os.path.join(_COMPONENT_ROOT, 'cache'))
        config['ConfigCacheDir'] = _cache_path
        try:
            _cc = config_cache.init_config_cache(config)
            logger.log_info(
                f"ConfigCache ready (read_only={_cc.is_read_only()}) dir={_cache_path} "
                f"listeners={len(_cc.get_all('listener'))} trunks={len(_cc.get_all('trunk'))} "
                f"routes={len(_cc.get_all('route'))} access={len(_cc.get_all('access'))}"
            )
        except Exception as _e:
            logger.log_error(f"ConfigCache init failed: {_e}")

        # SSL certificates — ① OAM 자체 cert(<oam>/cert, 부트스트랩 인스톨러가 생성)
        #                    ② csc/cert 공유(개발/동거 환경 fallback)
        ssl_keyfile = ssl_certfile = None
        _cert_cands = [os.path.join(_COMPONENT_ROOT, 'cert')]
        if _CSC_SRC:
            _cert_cands.append(os.path.normpath(os.path.join(_CSC_SRC, '..', 'cert')))
        for _cert_dir in _cert_cands:
            if os.path.exists(os.path.join(_cert_dir, 'server.key')) and                os.path.exists(os.path.join(_cert_dir, 'server.crt')):
                ssl_keyfile  = os.path.join(_cert_dir, 'server.key')
                ssl_certfile = os.path.join(_cert_dir, 'server.crt')
                break
        if ssl_keyfile and ssl_certfile:
            logger.log_info(f"SSL Enabled. Key: {ssl_keyfile}, Cert: {ssl_certfile}")
        else:
            logger.log_info("SSL Disabled (server.key / server.crt not found)")

        # ── OAM Admin server (4419) ──────────────────────────────────────
        admin_conf = config.get('Server', {'Ip': '0.0.0.0', 'Port': 4419})
        cims_kwargs = {'config': config}

        # ── 콘솔 정적 서빙 (상용/부트스트랩: vite dev 없이 OAM 단일 HTTPS 오리진) ──
        from handlers.console_static import (resolve_console_static_dir,
                                             CIMS_CONSOLE_STATIC_HANDLER_LIST)
        _console_dir = resolve_console_static_dir(config, _COMPONENT_ROOT)
        config['_ConsoleStaticDir'] = _console_dir
        if _console_dir:
            logger.log_info(f'[console-static] serving console SPA from {_console_dir}')
        else:
            logger.log_info('[console-static] console dist 미발견 — 정적 서빙 비활성 (dev 환경은 vite 사용)')

        # ── 시드 패키지 자동 등록 (부트스트랩 인스톨러가 떨군 oam/console/agent/csc
        #    tarball 을 file_store 에 멱등 등록 → /install-agent.sh, /agent-bundle.tar.gz
        #    및 콘솔 패키지 목록이 첫 부팅부터 동작) ──
        try:
            from handlers.agents import seed_packages_from_dir
            _seed_dir = (config.get('Packages') or {}).get('SeedDir') or 'seed_packages'
            if not os.path.isabs(_seed_dir):
                _seed_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, _seed_dir))
            _n = seed_packages_from_dir(config, _seed_dir)
            if _n:
                logger.log_info(f'[pkg-seed] registered {_n} package(s) from {_seed_dir}')
        except Exception as _e:
            logger.log_error(f'[pkg-seed] seed failed: {_e}')

        # Service Descriptor 레지스트리 — startup config 캐시 + store 비면 CIMS seed 주입.
        # ha_groups/build/service_control 이 descriptor 구동(하드코딩 fallback 보존).
        service_registry.init(config)
        _seeded = service_registry.seed_if_empty(config)
        if _seeded:
            logger.log_info(f'[service-registry] seeded {_seeded} service descriptor(s) from seed dir (store was empty)')

        admin_server = HttpServer(
            admin_conf.get('Ip', '0.0.0.0'),
            admin_conf.get('Port', 4419),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        # ── oam_base_service_split P0 — 핸들러 BASE/SERVICE 그룹 분기 (§4, §11) ──
        #   role=all  : BASE + SERVICE 전부 = 현행 단일프로세스(동작 무변경, 하위호환 I4).
        #   role=base : BASE(게이트웨이 + 공통 관리)만. 서비스(가입자/녹취/flow/검증/KPI)는
        #               미등록 → 독립 모듈/게이트웨이 프록시 귀속(P1+).
        # all 모드의 최종 라우트 테이블은 BASE→SERVICE 등록 순서로 구 interleaved 등록과
        # 동일하다(유일 충돌 = /api/v1/users[ME slim ← admin superset], /api/v1/recordings
        # [FLOW ← RECORDING] — 둘 다 후순위 등록이 우선이라 현행 핸들러 선택 보존).
        role = args_dict.get('role', 'all')
        logger.log_info(f"[role] OAM role = {role}")

        def _bind(L):
            return [(path, handler, cims_kwargs) for path, handler, _ in L]

        # ── BASE 공통 (모든 role) ──
        # /users/me = identity-plane(D8) → base slim 핸들러를 항상 등록.
        # all 모드에선 뒤의 SERVICE admin superset 이 /api/v1/users 를 덮어쓴다(현행 동작).
        base_rules = _bind(CIMS_AUTH_HANDLER_LIST + CIMS_ME_HANDLER_LIST)
        base_rules += _bind(CIMS_STATS_HANDLER_LIST)          # 노드 health/messages/leak
        base_rules += _bind(CIMS_BUILD_HANDLER_LIST)
        base_rules += _bind(CIMS_SERVICE_CONTROL_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_ADMIN_HANDLER_LIST)
        base_rules += _bind(CIMS_MODULES_HANDLER_LIST)
        base_rules += _bind(CIMS_HA_GROUPS_HANDLER_LIST)
        if _console_dir:
            base_rules += _bind(CIMS_CONSOLE_STATIC_HANDLER_LIST)
        base_rules += _bind(CIMS_ALERTS_HANDLER_LIST)
        base_rules += _bind(CIMS_CONSOLE_HANDLER_LIST)
        base_rules += _bind(CIMS_CONSOLE_ACCOUNTS_HANDLER_LIST)
        base_rules += _bind(CIMS_SERVICE_DESCRIPTORS_HANDLER_LIST)
        base_rules += _bind(CIMS_EXTERNAL_SYSTEMS_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_API_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_PUBLIC_HANDLER_LIST)
        base_rules += _bind(CIMS_GATEWAY_HANDLER_LIST)   # /api/v1/gateway/* 제어면(base 소유)
        admin_server.add_dynamic_rules(base_rules)

        # ── SERVICE (in-process; role=all 에서만; P2+ 게이트웨이 프록시로 이관) ──
        if role == 'all':
            # 가입자/조직 CRUD(csc 귀속) — admin superset 이 base slim /me 를 덮어씀.
            admin_server.add_dynamic_rules(_bind(CIMS_ADMIN_HANDLER_LIST + CIMS_ORG_HANDLER_LIST))
            # 녹취·flow(svc-mgmt 귀속) — 자기 init() 상태 사용(raw kwargs). FLOW→RECORDING
            # 순서로 /api/v1/recordings 충돌 시 RECORDING 우선(현행 보존).
            admin_server.add_dynamic_rules(FLOW_HANDLER_LIST)
            admin_server.add_dynamic_rules(CIMS_RECORDING_HANDLER_LIST)
            # 검증·service KPI(svc-mgmt 귀속).
            admin_server.add_dynamic_rules(
                _bind(CIMS_VERIFICATION_HANDLER_LIST + CIMS_STATS_SERVICE_HANDLER_LIST))
        else:
            logger.log_info('[role] base — 서비스 핸들러(가입자/녹취/flow/검증/KPI) 미등록 '
                            '(독립 모듈/게이트웨이 프록시 귀속, P1+)')
            # role=base: 서비스 세그먼트(/api/v1/<service>/*)를 라우트 테이블에 따라
            # loopback 업스트림으로 프록시 마운트. all 모드는 in-process 핸들러가 세그먼트를
            # 소유하므로 프록시를 마운트하지 않는다(충돌 방지).
            try:
                _gw_n = register_gateway(admin_server, config)
                logger.log_info(f"[gateway] mounted {_gw_n} proxy route(s)")
            except Exception as _e:
                logger.log_error(f"[gateway] mount failed: {_e}")

        admin_server.start()
        logger.log_info(f"OAM server started on port {admin_conf.get('Port', 4419)}")

        # ── D2: OAM self-upgrade self-reconcile ──────────────────────────────
        # 자기 self-upgrade 의 마지막 report(restart 완료)가 유실되면 해당 deployment 가
        # 'deploying' 에 stuck 된다(콘솔이 영원히 "배포 중"). 신 OAM(=나)이 이미 떠 있다는
        # 사실이 곧 그 업그레이드의 성공 증거이므로, 내가 실행 중인 install_path
        # (= _COMPONENT_ROOT 의 부모 = 버전 디렉터리)와 install_path 가 일치하는 oam
        # deployment 만 골라 running 으로 자가 정정한다.
        #   ⚠ install_path 일치로만 매칭 → 타 노드 oam deployment 오염 방지. HA 공유 store
        #     에서의 host 스코핑은 후속(docs/design/features/oam_self_upgrade.md §7).
        try:
            from handlers.agents import (_deploy_load_all, _deploy_update,
                                         _job_load, _job_update)
            from datetime import datetime as _rdt
            _my_install = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..'))
            _my_real = os.path.realpath(_my_install)
            _rec = 0
            for _d in _deploy_load_all(config):
                _proc = (_d.get('process_name') or _d.get('package_name') or '').lower()
                if _proc != 'oam' or _d.get('status') != 'deploying':
                    continue
                _dp = _d.get('install_path') or ''
                if not _dp or os.path.realpath(_dp) != _my_real:
                    continue
                _now_iso = _rdt.now().isoformat(timespec='seconds')
                _deploy_update(config, _d['id'], {
                    'status': 'running', 'deployed_at': _now_iso, 'reconciled': True,
                })
                _lj = _d.get('last_job_id')
                if _lj:
                    _j = _job_load(config, _lj)
                    if _j and _j.get('status') in ('queued', 'running'):
                        _job_update(config, _lj, {
                            'status': 'succeeded', 'result_code': 0,
                            'result_stdout': 'reconciled by oam self-upgrade startup',
                            'completed_at': _now_iso})
                _rec += 1
            if _rec:
                logger.log_info(f"[self-reconcile] oam deployment {_rec}건 "
                                f"deploying→running 정정 (install_path={_my_install})")
        except Exception as _e:
            logger.log_warning(f"[self-reconcile] skip: {_e}")

        # ── Agent stale sweeper ─────────────────────────────────────────
        from handlers.agents import _get_db as _agent_db_conn
        from handlers.agent_api import _AGENT_CERT_ROTATE_THRESHOLD_DAYS
        STALE_SEC = int(config.get('AgentStaleSec', 8))
        SWEEP_INTERVAL = int(config.get('AgentSweepIntervalSec', 2))
        CERT_SWEEP_INTERVAL = int(config.get('AgentCertSweepSec', 3600))

        def _sweep_stale_agents():
            try:
                from handlers.agents import _agent_load_all, _agent_update
                from datetime import datetime as _dt
                threshold = _dt.now().timestamp() - STALE_SEC
                n = 0
                for a in _agent_load_all(config):
                    if a.get('status') not in ('online', 'approved'):
                        continue
                    hb = a.get('last_heartbeat')
                    if not hb:
                        continue
                    try:
                        hb_ts = _dt.fromisoformat(hb).timestamp()
                    except Exception:
                        continue
                    if hb_ts < threshold:
                        _agent_update(config, a['id'], {'status': 'offline'})
                        n += 1
                if n > 0:
                    logger.log_info(f"[agent-sweep] marked {n} stale agent(s) offline "
                                    f"(threshold={STALE_SEC}s)")
            except Exception as e:
                logger.log_error(f"[agent-sweep] error: {e}")

        def _sweep_cert_rotate():
            try:
                from handlers.agents import _agent_load_all, _agent_update
                from datetime import datetime as _dt, timedelta as _td
                deadline = _dt.now() + _td(days=_AGENT_CERT_ROTATE_THRESHOLD_DAYS)
                n = 0
                for a in _agent_load_all(config):
                    if not a.get('mtls_enabled'):
                        continue
                    if a.get('cert_rotate_pending'):
                        continue
                    exp = a.get('cert_expires_at')
                    if not exp:
                        continue
                    try:
                        exp_dt = _dt.fromisoformat(exp)
                    except Exception:
                        continue
                    if exp_dt <= deadline:
                        _agent_update(config, a['id'], {'cert_rotate_pending': 1})
                        n += 1
                if n > 0:
                    logger.log_info(f"[cert-sweep] flagged {n} agent(s) for cert rotation "
                                    f"(threshold={_AGENT_CERT_ROTATE_THRESHOLD_DAYS}d)")
            except Exception as e:
                logger.log_error(f"[cert-sweep] error: {e}")

        # ── Alert sweeper ───────────────────────────────────────────────
        from handlers.stats import _get_csp_stats, _get_cmp_stats, _get_db as _cims_db_conn
        ALERT_SWEEP_INTERVAL = int(config.get('AlertSweepSec', 30))
        ALERT_RTP_THRESHOLD = int(config.get('AlertRtpThresholdPct', 80))
        _service_log = config.get('ServiceLogging', {}).get('Dir') \
            or config.get('ServiceLogDir', config.get('MsgLogDir', ''))
        # _alert_open: { akey(code@mo_instance) : alarm_id }  — 활성 알람 추적.
        _alert_open: dict = {}
        if _service_log:
            try:
                restored = alert_log.compute_open_state(_service_log, days=30)  # {akey: alarm_id}
                _alert_open.update(restored)
                if restored:
                    logger.log_info(f"[alarm] restored open state: {sorted(restored.keys())}")
            except Exception as e:
                logger.log_error(f"[alarm] restore failed: {e}")

        def _check_db():
            try:
                conn = _cims_db_conn(config)
                try:
                    with conn.cursor() as cur:
                        cur.execute("SELECT 1")
                    return True
                finally:
                    conn.close()
            except Exception:
                return False

        class _Safe(dict):
            def __missing__(self, k):  # 템플릿에 없는 키는 빈 문자열 (KeyError 방지)
                return ''

        def _fmt(tmpl: str, **kw) -> str:
            return (tmpl or '').format_map(_Safe(kw))

        # 표준 알람 이벤트 기록 (X.733/32.111 — code/severity/event_type/probable_cause/source/alarm_id).
        def _emit_alarm(action, rule, mo_instance, detected_by, message, alarm_id):
            from datetime import datetime as _dt
            sev = 'cleared' if action == 'close' else rule.get('perceived_severity', 'warning')
            rec = {
                'ts': _dt.now().isoformat(timespec='seconds'),
                'alarm_id': alarm_id,
                'type': rule.get('type'), 'code': rule.get('code'),
                'perceived_severity': sev, 'severity': sev,   # 'severity' 구 reader 호환
                'event_type': rule.get('event_type'), 'probable_cause': rule.get('probable_cause'),
                'source': {'mo_class': rule.get('mo_class'), 'mo_instance': mo_instance, 'detected_by': detected_by},
                'action': action, 'message': message,
            }
            if rule.get('effect'):
                rec['effect'] = rule['effect']
            if rule.get('recommended_action'):
                rec['recommended_action'] = rule['recommended_action']
            alert_log.record_event(_service_log, rec)

        # 활성식별 akey=(code@mo_instance). open 시 alarm_id 생성, close 가 동일 alarm_id 참조.
        def _transition(rule, mo_instance, detected_by, is_open, msg_open, msg_close):
            akey = f"{rule.get('code')}@{mo_instance}"
            was = akey in _alert_open
            if is_open and not was:
                alarm_id = f"{akey}@{int(time.time())}"
                _alert_open[akey] = alarm_id
                _emit_alarm('open', rule, mo_instance, detected_by, msg_open, alarm_id)
                logger.log_info(f"[alarm] OPEN {akey} sev={rule.get('perceived_severity')} — {msg_open}")
            elif not is_open and was:
                alarm_id = _alert_open.pop(akey)
                _emit_alarm('close', rule, mo_instance, detected_by, msg_close, alarm_id)
                logger.log_info(f"[alarm] CLEAR {akey}")

        def _eval_alert_rule(rule: dict, ctx: dict) -> bool:
            chk = rule.get('check')
            if chk == 'process_down':
                return not bool(ctx.get(rule.get('target')))
            if chk == 'db_down':
                return not ctx.get('db_ok')
            if chk == 'rtp_pct_gte':
                return ctx.get('rtp_pct', 0) >= int(rule.get('threshold', ALERT_RTP_THRESHOLD))
            return False

        def _eval_agent_rule(rule: dict, agent: dict, metric: dict,
                             deps: list, proc_down_targets: set) -> list:
            """scope='agent' 규칙을 한 agent 최신 metric 으로 평가.
            반환: (mo_instance, is_open, msg_open, msg_close) 목록."""
            chk = rule.get('check')
            host = agent.get('name') or str(agent.get('id'))
            res = []
            if chk == 'disk_high':
                disk = metric.get('disk_pct')
                if disk is None:
                    return res
                disk = round(float(disk), 1)
                thr = int(rule.get('threshold', 90))
                mo = f"{host}/disk"
                kw = dict(mo=mo, host=host, pct=disk, threshold=thr)
                res.append((mo, disk >= thr, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw)))
            elif chk == 'module_down':
                running = {(m.get('name') or '').lower()
                           for m in (metric.get('modules') or []) if m.get('name')}
                for dep in deps:
                    if dep.get('agent_id') != agent.get('id'):
                        continue
                    if dep.get('status') != 'running':
                        continue
                    proc = (dep.get('process_name') or dep.get('package_name') or '').lower()
                    # process_down 규칙으로 이미 평가되는 모듈(csp/cmp 등)은 제외 — 중복 alarm 방지.
                    if not proc or proc in proc_down_targets:
                        continue
                    # 비데몬(별도 프로세스 없음) 모듈은 module_down 대상 아님 — agent 가 metric.modules
                    # 에 보고하지 않으므로(_NON_DAEMON_MODULES) 'running' 이어도 항상 down 으로 오탐.
                    #   console = OAM 이 정적 서빙(별도 프로세스 없음), agent = 자기 자신.
                    if proc in ('console', 'agent'):
                        continue
                    mo = f"{host}/{proc}"
                    kw = dict(mo=mo, host=host, module=proc)
                    res.append((mo, proc not in running, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw)))
            return res

        def _sweep_agent_alerts(agent_rules: list, proc_down_targets: set):
            """per-agent 규칙(disk/module)을 online agent 별로 평가. 관측 불가 시 자동 close."""
            from handlers.agents import _agent_load_all, _deploy_load_all, _metric_root
            from services import file_store
            agents = _agent_load_all(config)
            deps = _deploy_load_all(config)
            mroot = _metric_root(config)
            active = set()
            for ag in agents:
                if ag.get('status') != 'online':
                    continue
                host = ag.get('name') or str(ag.get('id'))
                metric = file_store.jsonl_last(mroot, str(ag['id']))
                if not metric:
                    continue
                for r in agent_rules:
                    for mo, is_open, msg_open, msg_close in _eval_agent_rule(r, ag, metric, deps, proc_down_targets):
                        active.add(f"{r.get('code')}@{mo}")
                        _transition(r, mo, f"agent:{host}", is_open, msg_open, msg_close)
            # agent 알람(mo_instance 가 cims/ 가 아닌 host/…) 중 이번에 평가 안 된 것 = 관측 불가 → close.
            agent_rule_by_code = {r.get('code'): r for r in agent_rules}
            for akey in list(_alert_open.keys()):
                mo_part = akey.split('@', 1)[1] if '@' in akey else ''
                if mo_part.startswith('cims/') or akey in active:
                    continue
                r = agent_rule_by_code.get(akey.split('@', 1)[0])
                if r:
                    _transition(r, mo_part, f"agent:{mo_part.split('/', 1)[0]}", False, '',
                                _fmt(r.get('msg_close'), mo=mo_part))

        def _sweep_alerts():
            try:
                csp = _get_csp_stats(config)
                cmp = _get_cmp_stats(config)
                db_ok = _check_db()
                total = cmp.get('rtp_ports_total', 0) or 0
                used = cmp.get('rtp_ports_used', 0) or 0
                pct = int(round(used / total * 100)) if total > 0 else 0
                ctx = {'csp': csp, 'cmp': cmp, 'db_ok': db_ok, 'rtp_pct': pct}
                rules = service_registry.alert_rules(config)   # 표준 정규화됨
                svc_rules   = [r for r in rules if r.get('scope') != 'agent']
                agent_rules = [r for r in rules if r.get('scope') == 'agent']
                for r in svc_rules:
                    thr = r.get('threshold', ALERT_RTP_THRESHOLD)
                    mo = r.get('mo_instance') or f"cims/{r.get('target', '')}"
                    msg_open = _fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr)
                    msg_close = _fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr)
                    _transition(r, mo, 'oam', _eval_alert_rule(r, ctx), msg_open, msg_close)
                if agent_rules:
                    proc_down_targets = {(r.get('target') or '').lower()
                                         for r in svc_rules if r.get('check') == 'process_down'}
                    _sweep_agent_alerts(agent_rules, proc_down_targets)
            except Exception as e:
                logger.log_error(f"[alarm-sweep] error: {e}")

        # ── sync_txn timeout sweeper ────────────────────────────────────
        SYNC_TXN_SWEEP_INTERVAL = int(config.get('SyncTxnSweepSec', 15))

        def _sweep_sync_txn():
            try:
                from services import sync_txn
                n = sync_txn.sweep_timeouts(config)
                if n > 0:
                    logger.log_info(f"[sync-txn-sweep] timed out {n} transaction(s)")
            except Exception as e:
                logger.log_error(f"[sync-txn-sweep] error: {e}")

        # ── HA fan-out drift sweeper ────────────────────────────────────
        DRIFT_SWEEP_INTERVAL  = int(config.get('DriftSweepSec', 300))
        DRIFT_AUTO_RESYNC     = bool(config.get('AutoResyncDrift', False))
        _drift_open: dict = {}

        def _sweep_drift():
            try:
                from services import drift_sweeper
                results = drift_sweeper.scan_all(config)
                if not _service_log:
                    return
                counts = drift_sweeper.emit_drift_alerts(
                    config, results, _service_log, _drift_open)
                drift_rows = [r for r in results if r.get('drift')]
                if drift_rows:
                    logger.log_info(
                        f"[drift-sweep] scanned={len(results)} "
                        f"drift={len(drift_rows)} opened={counts['opened']} "
                        f"closed={counts['closed']}")
                if DRIFT_AUTO_RESYNC and drift_rows:
                    summary = drift_sweeper.auto_resync(config, drift_rows)
                    if summary['resynced'] or summary['errors']:
                        logger.log_info(
                            f"[drift-sweep] auto_resync — resynced="
                            f"{summary['resynced']} errors={len(summary['errors'])}")
            except Exception as e:
                logger.log_error(f"[drift-sweep] error: {e}")

        # ── Metric JSONL retention purge sweeper ────────────────────────
        # heartbeat 2s × 다수 host → metrics/<id>/YYYY/MM/DD.jsonl 무한 누적.
        # retain_days 보다 오래된 일별 파일 삭제 (B 트랙 Phase 1 의 24h purge 설계 구현).
        METRIC_RETAIN_DAYS    = int(config.get('MetricRetentionDays', 3))
        METRIC_PURGE_INTERVAL = int(config.get('MetricPurgeSweepSec', 3600))

        def _sweep_metric_purge():
            try:
                from handlers.agents import _metric_root
                from services import file_store
                n = file_store.jsonl_purge_old(_metric_root(config), METRIC_RETAIN_DAYS)
                if n > 0:
                    logger.log_info(f"[metric-purge] removed {n} metric jsonl file(s) "
                                    f"older than {METRIC_RETAIN_DAYS}d")
            except Exception as e:
                logger.log_error(f"[metric-purge] error: {e}")

        logger.log_info(f"[agent-sweep] stale threshold={STALE_SEC}s, interval={SWEEP_INTERVAL}s")
        logger.log_info(f"[cert-sweep] rotate threshold={_AGENT_CERT_ROTATE_THRESHOLD_DAYS}d, "
                        f"interval={CERT_SWEEP_INTERVAL}s")
        logger.log_info(f"[alert-sweep] interval={ALERT_SWEEP_INTERVAL}s, "
                        f"rtp_threshold={ALERT_RTP_THRESHOLD}%, "
                        f"dir={_service_log or '(disabled — no ServiceLogDir)'}")
        logger.log_info(f"[sync-txn-sweep] interval={SYNC_TXN_SWEEP_INTERVAL}s")
        logger.log_info(f"[drift-sweep] interval={DRIFT_SWEEP_INTERVAL}s "
                        f"auto_resync={DRIFT_AUTO_RESYNC}")
        logger.log_info(f"[metric-purge] retain={METRIC_RETAIN_DAYS}d, interval={METRIC_PURGE_INTERVAL}s")
        _last_sweep = 0
        _last_cert_sweep = 0
        _last_alert_sweep = 0
        _last_sync_txn_sweep = 0
        _last_drift_sweep = 0
        _last_metric_purge = 0
        while True:
            time.sleep(1)
            _now = time.time()
            if _now - _last_sweep >= SWEEP_INTERVAL:
                _sweep_stale_agents()
                _last_sweep = _now
            if _now - _last_cert_sweep >= CERT_SWEEP_INTERVAL:
                _sweep_cert_rotate()
                _last_cert_sweep = _now
            if _now - _last_alert_sweep >= ALERT_SWEEP_INTERVAL:
                _sweep_alerts()
                _last_alert_sweep = _now
            if _now - _last_sync_txn_sweep >= SYNC_TXN_SWEEP_INTERVAL:
                _sweep_sync_txn()
                _last_sync_txn_sweep = _now
            if _now - _last_drift_sweep >= DRIFT_SWEEP_INTERVAL:
                _sweep_drift()
                _last_drift_sweep = _now
            if _now - _last_metric_purge >= METRIC_PURGE_INTERVAL:
                _sweep_metric_purge()
                _last_metric_purge = _now

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        # D3: preflight 모드에서 import/config 예외는 "신 패키지 기동 불가" 신호 → 비0 종료.
        if args_dict.get('preflight'):
            print(f'OAM_PREFLIGHT_FAIL: {e}', flush=True)
            sys.exit(2)
