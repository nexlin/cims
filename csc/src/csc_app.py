import argparse
import os
import sys
import time
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))
_CONFIG_PATH = os.environ.get('CIMS_CSC_CONFIG') or os.path.join(_COMPONENT_ROOT, 'config', 'csc.json')

# ── Phase 4 vendor: private 환경 (인터넷 없음) 대응 ──
# csc/vendor/ 에 사전 다운로드된 fastapi/uvicorn/pymysql/PyJWT/loguru/requests/
# readerwriterlock 등 site-packages. system pip 없이도 동작.
# 빌드 시점: 'pip3 install --target=csc/vendor -r csc/requirements.txt --no-compile'
_VENDOR = os.path.normpath(os.path.join(_COMPONENT_ROOT, 'vendor'))
if os.path.isdir(_VENDOR) and _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)

# ── CSC 완전 독립 모듈 (csc_standalone_module.md P1) ──
# csc 는 oam/src 를 마운트하지 않는다. base ↔ csc 결합은 계약(stable contract)으로만:
# 게이트웨이 HTTP 프록시 + 공유 JwtSecret 기반 JWT verify + DB 스키마. csc 는 자기
# handlers(admin/org)·services(mcptt/idms/config_cache + 인프라 유틸)·httpsrv·util 만으로
# 기동한다(별도 프로세스 = sys.path 독립 = services 충돌 원천 소멸).
# 로그인/토큰발급(auth)·본인프로파일(/users/me)은 base(oam) 책임 — csc 는 미서빙.

from httpsrv.server import HttpServer
from util.log_util import Logger


# for test
from httpsrv.handler import HandlerArgs, HandlerResult
async def _rcv_msg(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if handler_args.method != 'GET':
        return HandlerResult(status=405, body=f'invalid method')
    return HandlerResult(status=200, body='OK')
TEST_HANDLER_LIST = [("/test", _rcv_msg, {})]


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    # parser.add_argument('--agent-id', type=str, required=True, help='Agent ID')
    args_dict = vars(parser.parse_args())

    # set arguments

    # init logger
    logger = Logger(log_dir=os.path.join(_COMPONENT_ROOT, "log"), log_file_prefix="app", retention_day=30)
    
    # Load Config
    import json
    def _apply_overlay(root: dict, flat: dict) -> int:
        """Flat dot-path config ({"Server.Port": 5420}) 를 root 에 재귀 merge."""
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

    def load_config():
        # Configuration file location resolved via _CONFIG_PATH (absolute)
        # base(config/csc.json)는 configure 단계(apply_config_template)에서만 생성되므로
        # 상용 배포본(build→pkg, configure 생략)에는 없는 게 정상이다. 이때 실제 설정은
        # agent 가 쓴 deployment overlay(config.json)가 SoT 이므로, base 부재 시에도
        # overlay 머지를 계속 진행한다 (배포 계약: overlay=primary, base=optional —
        # lifecycle.sh / SipServerSetup 과 동일).
        try:
            with open(_CONFIG_PATH, 'r') as f:
                c = json.load(f)
        except FileNotFoundError:
            logger.log_info(f"base config not found at {_CONFIG_PATH} — overlay(config.json) 만으로 기동")
            c = {}
        # Deployment overlay: cims_agent 가 멀티-변종 install 지원으로 변종 디렉토리
        # 안에 config.json 을 쓰므로 (install_path/csc/config.json), 거기를 먼저 본다.
        # 후방 호환으로 legacy 위치 (install_path/config.json) 도 fallback.
        # _CONFIG_PATH = install_path/csc/config/csc.json → _COMPONENT_ROOT = install_path/csc.
        try:
            for overlay in (
                os.path.join(_COMPONENT_ROOT, 'config.json'),                  # scoped
                os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'config.json')),  # legacy
            ):
                if not os.path.isfile(overlay):
                    continue
                with open(overlay, 'r') as f:
                    flat = json.load(f)
                if isinstance(flat, dict) and flat:
                    n = _apply_overlay(c, flat)
                    logger.log_info(f"CSC overlay applied: {overlay} ({n} keys)")
                    break
        except Exception as e:
            logger.log_error(f"CSC overlay failed: {e}")
        return c

    # CSC 완전 독립 (csc_standalone_module.md P1) — csc 자기 handlers/services 만 import.
    #   가입자 CRUD(admin) + 조직(org) + MCPTT(IdMS/GMS/CMS/KMS). 로그인/토큰발급(auth)·
    #   본인프로파일(users /me)은 base(oam) 책임 → csc 미서빙·미import.
    #   JWT 검증은 자체 services.admin_auth (공유 JwtSecret = 계약).
    from services.mcptt import load_shared_data, apply_config, CSC_HANDLER_LIST, notify_csp
    from services       import logger as csc_logger
    from services       import admin_auth
    from handlers.admin          import CIMS_ADMIN_HANDLER_LIST
    from handlers.org            import CIMS_ORG_HANDLER_LIST
    # 자기 API 문서 — 분리 배포에서 OAM 이 import 로 읽을 수 없으므로 직접 서비스한다.
    from handlers.api_docs       import CSC_API_DOCS_HANDLER_LIST
    # AuC — IMS AKA 인증 벡터 발급자 (sip_access_security.md §8.2). 내부 AV API 는 admin 서버(4421)에
    #   붙지만 /api/v1 밖이라 OAM 게이트웨이가 프록시하지 않는다(CSP 직접 호출 전용).
    from handlers.auc_api        import CSC_AUC_HANDLER_LIST
    # 내부 토폴로지 API — CSP 가 단말용 XCAP root 를 조회한다(정본=McpttServer.PublicUrl).
    from handlers.internal_api   import CSC_INTERNAL_HANDLER_LIST
    from services.auc            import auc as _auc

    admin_server = None
    mcptt_server = None
    try:
        logger.log_info(f'==================== start (CSC) ====================')

        config = load_config()
        admin_auth.init(config)
        _auc.init(config)

        # ── SIGUSR1 = 배포 config reload (agent job_update_config 규약) ──
        # 핸들러가 없으면 파이썬 기본 동작(프로세스 종료)이라 update_config 가 CSC 를
        # 죽인다. config_template 의 restart:false (런타임 리로드 가능) 필드 —
        # CspNotify/PspNotify, IdMs TTL, Provisioning 등 — 가 실제로 재기동 없이
        # 반영되는 경로. bind 계열(Server.Port/McpttServer.Port)은 재기동 필요.
        import signal as _signal

        def _on_usr1(_sig, _frm):
            try:
                newc = load_config()
                if newc:
                    from services import config_reload as _cr
                    kept = _cr.apply_reload(config, newc)
                    admin_auth.init(config)
                    _auc.init(config)
                    apply_config(config)
                    logger.log_info(f'[reload] SIGUSR1 — config 재적용 ({kept}건 런타임 보존) '
                                    '(bind/기동 캡처 항목은 재기동 필요)')
                else:
                    logger.log_warning('[reload] SIGUSR1 — 재로드 실패(빈 설정), 기존 유지')
            except Exception as e:
                logger.log_error(f'[reload] SIGUSR1 처리 실패: {e}')

        _signal.signal(_signal.SIGUSR1, _on_usr1)

        # ServiceLogging 설정 (신규 통합)
        #   비어 있으면 **노드 로컬**로 해석한다 — 공유 마운트를 붙이기 전(부트스트랩
        #   직후)에는 그 경로가 없는 것이 정상이라, 없는 공유 경로를 붙들고 기록 실패를
        #   반복하는 대신 로컬에라도 남긴다. 공유 경로는 배포 overlay 가 정한다.
        sl = config.get("ServiceLogging", {})
        _service_log_dir = str(sl.get("Dir", "") or "").strip()
        # 레거시 호환
        if not _service_log_dir:
            _service_log_dir = str(config.get("ServiceLogDir",
                                              config.get("MsgLogDir", "")) or "").strip()
        if not _service_log_dir:
            _service_log_dir = os.path.normpath(
                os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime', 'service_log'))
        _system_id = config.get("SystemId", "csc_01")

        # flow_logger(통화이력/flow API)는 oam-svc 책임 — csc 는 미서빙(FLOW_HANDLER_LIST
        # 미등록)이므로 init 불요. csc 자기 flow 로깅은 csc_logger(logger.py) 가 담당.
        csc_logger.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
            spool_dir=str(sl.get("SpoolDir", "") or "spool"),
            stall_sec=int(sl.get("StallSec", 5) or 5),
            spool_max_mb=int(sl.get("SpoolMaxMb", 1024) or 1024),
        )

        # ── pi_http 요청 로깅 훅 등록 (admin/mcptt 자동 로깅) ──
        from httpsrv.controller import DynamicRouteProc

        # base_path 접두어 → service 매핑 (긴 prefix 우선 매칭)
        _BASE_PATH_SERVICE = [
            ("/.well-known/openid-configuration",      "mcptt"),
            ("/provisioning/",                         "mcptt"),
            ("/idms/",                                 "mcptt"),
            ("/org.openmobilealliance.groups",         "mcptt"),
            ("/org.3gpp.mcptt",                        "mcptt"),
            ("/keymanagement/",                        "mcptt"),
            ("/api/v1/",                               "console"),
        ]

        def _route_to_service(full_path: str) -> str:
            for prefix, svc in _BASE_PATH_SERVICE:
                if full_path.startswith(prefix):
                    return svc
            return ""

        def _extract_caller(handler_args) -> str:
            """Authorization Bearer JWT → login_id 추출. 없으면 body/query 에서 user 후보."""
            try:
                payload = admin_auth.extract_admin_jwt(handler_args.headers)
                if payload:
                    return payload.get("login_id") or str(payload.get("sub", "")) or ""
            except Exception:
                pass
            # query string 'user_name' 또는 body 'login_id' (IdMS authreq 등).
            #   규격 폼 POST 는 입력칸 이름이 설정값(IdMs.FormLoginField) — mcptt 모듈의 현재값을 본다.
            qp = handler_args.query_params or {}
            if qp.get("user_name"): return qp["user_name"]
            body = handler_args.body or {}
            if isinstance(body, dict):
                if body.get("login_id"): return body["login_id"]
                if body.get("user_name"): return body["user_name"]
                try:
                    from services import mcptt as _mcptt
                    if body.get(_mcptt.IDMS_FORM_LOGIN_FIELD): return str(body[_mcptt.IDMS_FORM_LOGIN_FIELD])
                except Exception:
                    pass
            return ""

        def _post_hook(handler_args, base_path, handler_result):
            """모든 dynamic 요청 완료 후 flow/msg 로그 기록."""
            try:
                # 로그 제외: health check, WebSocket 업그레이드, 빈 경로
                if not handler_args.full_path or handler_args.full_path == "/health":
                    return
                service = _route_to_service(handler_args.full_path)
                if not service:
                    service = "console"  # 기본: admin UI 추정

                caller = _extract_caller(handler_args)
                peer = f"{getattr(handler_args,'client_ip','')}:{getattr(handler_args,'client_port','')}"
                status = getattr(handler_result, "status", 0)

                # IdMS/GMS/CMS sub-function prefix 로 method 구성
                path = handler_args.full_path
                sub = ""
                if path.startswith("/idms/"):                    sub = "IdMS"
                elif path.startswith("/org.openmobilealliance"): sub = "GMS"
                elif path.startswith("/org.3gpp.mcptt"):         sub = "CMS"
                elif path.startswith("/keymanagement"):          sub = "KMS"

                mname = f"{handler_args.method} {path}" if not sub else f"{sub}/{handler_args.method} {path}"
                detail = f"status={status}"

                csc_logger.log_flow(
                    service=service,
                    from_actor="ue", to_actor="csc",
                    proto="HTTPS", method=mname,
                    detail=detail,
                    iface="ue", peer=peer,
                    caller=caller,
                )
            except Exception as e:
                logger.log_error(f"post_hook error: {e}")

        DynamicRouteProc.set_request_hooks(pre=None, post=_post_hook)

        # ── IdMS file_store 루트 (CimsRuntimeDir) — **버전 무관** 경로 보증 ──
        #   refresh 토큰·auth code 가 여기 산다. 버전 디렉터리/개발 트리 경로면 업그레이드마다
        #   저장소가 갈려 단말 refresh 가 "not found" 로 죽고 전 단말 재로그인이 필요해진다
        #   (08-20 실측). 미설정이면 인증서(runtime/cert)와 같은 규칙으로 모듈 루트
        #   (modules/csc/runtime)를 유도한다 — oam/oam-svc 와 동일.
        #   ⚠ 유도값은 반드시 runtime_set 으로 — 평대입하면 SIGUSR1 리로드(apply_reload 의
        #   clear+update)가 파일에 없는 이 키를 지우고, file_store.runtime_root() 가 폴백
        #   (ServiceLogging.Dir sibling '../runtime' = NAS /mnt/cims/runtime)으로 표류한다.
        #   실측 사고(08-26): 리로드 한 번에 IdMS 스토어가 관리평면 공유 스토어로 옮겨가
        #   기존 refresh 토큰 전멸("Refresh token not found") + 펜싱 없는 두 번째 writer.
        from services import config_reload as _cfg_rt
        if not config.get('CimsRuntimeDir'):
            _cfg_rt.runtime_set(config, 'CimsRuntimeDir', os.path.normpath(
                os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime')))
            logger.log_info(f"CimsRuntimeDir 미설정 — 모듈 runtime 유도: {config['CimsRuntimeDir']}")

        # Adjust relative data paths
        if 'Data' in config:
            for key in ['User', 'Group']:
                val = config['Data'].get(key, '')
                if val and not os.path.isabs(val):
                    config['Data'][key] = os.path.normpath(os.path.join(_COMPONENT_ROOT, val))
            # 절대화된 Data 도 리로드가 파일의 상대경로로 되돌리지 않게 보존 (위 CimsRuntimeDir 와 동일 계기)
            _cfg_rt.runtime_set(config, 'Data', config['Data'])
            load_shared_data(config)

        # [Test Support] Inject dummy data if empty so tests pass without real JSON files
        from services.mcptt import USERS, GROUPS
        if not USERS:
            logger.log_info("No users loaded. Injecting dummy user for test.")
            USERS["tel:+1000"] = {"password": "password123", "name": "Test User", "profile_etag": "etag_1000"}
        if not GROUPS:
            logger.log_info("No groups loaded. Injecting dummy group for test.")
            GROUPS["tel:+2000"] = {"display_name": "Test Group", "etag": "etag_2000", "members": []}

        # SSL certificates (shared by both servers)
        #   **버전무관 runtime/cert 우선** — lifecycle 엔진(cims-svc)이 기동 전에 노드
        #   인증서를 보증해 두는 자리다(agent/lib/cert.sh). 버전 디렉터리 cert 만 보면
        #   csc 업그레이드마다 새 디렉터리엔 인증서가 없어 평문으로 뜨고, 게이트웨이가
        #   https 로 프록시하므로 가입자 API 가 통째로 끊긴다. oam/oam-svc 와 같은 규칙.
        _cert_dir = None
        for _cand in (os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime', 'cert')),
                      os.path.join(_COMPONENT_ROOT, 'cert')):
            if os.path.exists(os.path.join(_cand, 'server.key')) and \
               os.path.exists(os.path.join(_cand, 'server.crt')):
                _cert_dir = _cand
                break
        _cert_dir = _cert_dir or os.path.join(_COMPONENT_ROOT, 'cert')
        ssl_keyfile  = os.path.join(_cert_dir, 'server.key')  if os.path.exists(os.path.join(_cert_dir, 'server.key'))  else None
        ssl_certfile = os.path.join(_cert_dir, 'server.crt') if os.path.exists(os.path.join(_cert_dir, 'server.crt')) else None
        if ssl_keyfile and ssl_certfile:
            logger.log_info(f"SSL Enabled. Key: {ssl_keyfile}, Cert: {ssl_certfile}")
        else:
            logger.log_info("SSL Disabled (server.key / server.crt not found)")

        # ── CSC Admin server (가입자 CRUD + 조직) ─────────────────────────────
        # CSC 완전 독립 (csc_standalone_module.md P1) — 가입자(admin.py) + 조직(org.py)만.
        # 로그인/토큰발급(auth)·본인프로파일(users /me)은 base(oam) 책임. port 는
        # csc.json Server.Port (default 4421, OAM 4419 와 충돌 회피).
        admin_conf = config.get('Server', {'Ip': '0.0.0.0', 'Port': 4421})
        cims_kwargs = {'config': config}
        admin_server = HttpServer(
            admin_conf.get('Ip', '0.0.0.0'),
            admin_conf.get('Port', 4421),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in (CIMS_ADMIN_HANDLER_LIST + CIMS_ORG_HANDLER_LIST
                                     + CSC_API_DOCS_HANDLER_LIST + CSC_AUC_HANDLER_LIST
                                     + CSC_INTERNAL_HANDLER_LIST)
        ])
        admin_server.start()
        logger.log_info(f"CSC Admin server started on port {admin_conf.get('Port', 4421)}")

        # ── MCPTT server (IdMS / GMS / CMS / KMS) ───────────────────────────
        mcptt_conf = config.get('McpttServer', {'Ip': '0.0.0.0', 'Port': 4430})
        mcptt_server = HttpServer(
            mcptt_conf.get('Ip', '0.0.0.0'),
            mcptt_conf.get('Port', 4430),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        mcptt_server.add_dynamic_rules(CSC_HANDLER_LIST)
        # MCData FD 콘텐츠 서버 — 파일 업로드/다운로드 (mcdata_messaging.md, 토큰 인증 동일)
        from services import mcdata_fd
        mcdata_fd.init(config)
        mcptt_server.add_dynamic_rules(mcdata_fd.MCDATA_FD_HANDLER_LIST)
        mcptt_server.start()
        logger.log_info(f"MCPTT server started on port {mcptt_conf.get('Port', 4430)}")

        # Notify CSP that CSC has (re)started so it resyncs all state from DB
        try:
            notify_csp("CSC_RESTART", "", "START")
            logger.log_info("CSC_RESTART notification sent to CSP")
        except Exception as e:
            logger.log_error(f"CSC_RESTART notification failed: {e}")

        # ── FM 자기보고 (alarm_self_reporting.md) — OAM 으로 알람/이벤트 push ──
        # DB 연결 알람은 요청 경로와 분리된 전용 probe 연결(10s 주기, 3연속 실패 전이).
        from services import fm_reporter
        _fm = fm_reporter.init(
            config, node=_system_id,
            catalog_file=os.path.join(_COMPONENT_ROOT, 'config', 'fm_catalog.json'),
            log=logger)
        _fm_db_probe = None
        if _fm:
            # mo 는 서버명 루트 <node>/<module>[/<component>] (표준화 §3.4(b)).
            _fm.send_event('process_started', mo=f'{_system_id}/csc')
            _db_mo = f'{_system_id}/csc/db'
            _db_cfg = config.get('CimsDatabase') or {}
            _db_params = {'host': _db_cfg.get('Host', ''), 'port': _db_cfg.get('Port', ''),
                          'db': _db_cfg.get('Database', '')}
            _fm_db_probe = fm_reporter.DbHealthProbe(
                lambda: config.get('CimsDatabase') or {},
                lambda up: (_fm.alarm_close('A-COM-001', _db_mo) if up
                            else _fm.alarm_open('A-COM-001', _db_mo, params=_db_params)),
                log=logger)
            _fm_db_probe.start()

        # ── SIGTERM = graceful stop — process_stopping 이벤트 후 서버 정리 ──
        # (종전: 핸들러 부재 → 파이썬 기본 동작으로 즉사, 종료 통지 불가)
        import threading as _threading
        _stop_evt = _threading.Event()
        _signal.signal(_signal.SIGTERM, lambda _s, _f: _stop_evt.set())

        # OAM 분리 Phase 3b — sweeper (agent/cert/alert/sync_txn/drift) 는 oam_app.py
        # 책임. csc 는 가입자 CRUD + MCPTT signaling 만 — 백그라운드 작업은 IdMS 토큰
        # 위생(만료/회수분 삭제)뿐이다. 정리 부재로 refresh_tokens/ 가 수천 파일로
        # 누적되던 것을 회수(기동 직후 1회 + 6시간 주기).
        from services.mcptt import storage as _idms_storage
        _IDMS_CLEAN_SEC = 6 * 3600
        _idms_clean_at = time.time() + 60          # 기동 60초 후 첫 정리
        while not _stop_evt.wait(1):
            if time.time() >= _idms_clean_at:
                _idms_clean_at = time.time() + _IDMS_CLEAN_SEC
                try:
                    _idms_storage.cleanup_expired_tokens()
                    _idms_storage.cleanup_expired_codes()
                except Exception as _ce:
                    logger.log_error(f"IdMS token cleanup: {_ce}")

        logger.log_info('==================== stop (SIGTERM) ====================')
        if _fm:
            _fm.send_event('process_stopping', mo=f'{_system_id}/csc')
            time.sleep(1)          # ack/재전송 1회 여유 (best-effort)
            if _fm_db_probe:
                _fm_db_probe.stop()
            _fm.stop()
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)
