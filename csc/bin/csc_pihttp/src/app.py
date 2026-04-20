import argparse
import os
import time
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))
_CONFIG_PATH = os.path.join(_COMPONENT_ROOT, 'config', 'csc.json')

from util.pi_http.http_server import HttpServer
from util.log_util import Logger


# for test
from util.pi_http.http_handler import HandlerArgs, HandlerResult
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
    def load_config():
        # Configuration file location resolved via _CONFIG_PATH (absolute)
        try:
            with open(_CONFIG_PATH, 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            logger.log_error(f"Config file not found at {_CONFIG_PATH}")
            return {}

    from csc_service import load_shared_data, CSC_HANDLER_LIST, notify_csp
    from cims_admin import CIMS_ADMIN_HANDLER_LIST
    import cims_auth
    from cims_auth import CIMS_AUTH_HANDLER_LIST
    import csc_flow
    from csc_flow import FLOW_HANDLER_LIST
    import cims_recording
    from cims_recording import CIMS_RECORDING_HANDLER_LIST
    from cims_stats import CIMS_STATS_HANDLER_LIST
    from cims_org import CIMS_ORG_HANDLER_LIST
    from cims_verification import CIMS_VERIFICATION_HANDLER_LIST, init as ver_init
    import csc_config_cache
    import csc_internal
    from csc_internal import CSC_INTERNAL_HANDLER_LIST

    admin_server = None
    mcptt_server = None
    try:
        logger.log_info(f'==================== start ====================')

        config = load_config()
        cims_auth.init(config)

        # ServiceLogging 설정 (신규 통합)
        sl = config.get("ServiceLogging", {})
        _service_log_dir = sl.get("Dir", "")
        # 레거시 호환
        if not _service_log_dir:
            _service_log_dir = config.get("ServiceLogDir", config.get("MsgLogDir", ""))
        _system_id = config.get("SystemId", "csp_01")

        csc_flow.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
        )

        import csc_logger
        tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'tests'))
        if not os.path.isdir(tests_dir):
            tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        ver_init(tests_dir)

        csc_logger.init(
            service_log_dir=_service_log_dir,
        )

        cims_recording.init(service_log_dir=_service_log_dir)

        # ── pi_http 요청 로깅 훅 등록 (admin/mcptt 자동 로깅) ──
        from util.pi_http.http_server_controller import DynamicRouteProc

        # base_path 접두어 → service 매핑 (긴 prefix 우선 매칭)
        _BASE_PATH_SERVICE = [
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
                payload = cims_auth.extract_token(handler_args)
                if payload:
                    return payload.get("login_id") or str(payload.get("sub", "")) or ""
            except Exception:
                pass
            # query string 'user_name' 또는 body 'login_id' (IdMS authreq 등)
            qp = handler_args.query_params or {}
            if qp.get("user_name"): return qp["user_name"]
            body = handler_args.body or {}
            if isinstance(body, dict):
                if body.get("login_id"): return body["login_id"]
                if body.get("user_name"): return body["user_name"]
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

        # Adjust relative data paths
        if 'Data' in config:
            for key in ['User', 'Group']:
                val = config['Data'].get(key, '')
                if val and not os.path.isabs(val):
                    config['Data'][key] = os.path.normpath(os.path.join(_COMPONENT_ROOT, val))
            load_shared_data(config)

        # CSP 런타임 설정 캐시 (DB→mem→file). DB 장애 시 파일 캐시로 read-only 모드 작동.
        _cache_path = config.get('ConfigCacheDir')
        if _cache_path and not os.path.isabs(_cache_path):
            _cache_path = os.path.normpath(os.path.join(_COMPONENT_ROOT, _cache_path))
        if not _cache_path:
            _cache_path = os.path.normpath(os.path.join(_COMPONENT_ROOT, 'cache'))
        config['ConfigCacheDir'] = _cache_path
        try:
            _cc = csc_config_cache.init_config_cache(config)
            logger.log_info(
                f"ConfigCache ready (read_only={_cc.is_read_only()}) dir={_cache_path} "
                f"listeners={len(_cc.get_all('listener'))} trunks={len(_cc.get_all('trunk'))} "
                f"routes={len(_cc.get_all('route'))} access={len(_cc.get_all('access'))}"
            )
        except Exception as _e:
            logger.log_error(f"ConfigCache init failed: {_e}")

        # CSP 전용 내부 API 초기화 (shared secret + loopback only)
        csc_internal.init(config)

        # [Test Support] Inject dummy data if empty so tests pass without real JSON files
        from csc_service import USERS, GROUPS
        if not USERS:
            logger.log_info("No users loaded. Injecting dummy user for test.")
            USERS["tel:+1000"] = {"password": "password123", "name": "Test User", "profile_etag": "etag_1000"}
        if not GROUPS:
            logger.log_info("No groups loaded. Injecting dummy group for test.")
            GROUPS["tel:+2000"] = {"display_name": "Test Group", "etag": "etag_2000", "members": []}

        # SSL certificates (shared by both servers)
        _cert_dir = os.path.join(_COMPONENT_ROOT, 'cert')
        ssl_keyfile  = os.path.join(_cert_dir, 'server.key')  if os.path.exists(os.path.join(_cert_dir, 'server.key'))  else None
        ssl_certfile = os.path.join(_cert_dir, 'server.crt') if os.path.exists(os.path.join(_cert_dir, 'server.crt')) else None
        if ssl_keyfile and ssl_certfile:
            logger.log_info(f"SSL Enabled. Key: {ssl_keyfile}, Cert: {ssl_certfile}")
        else:
            logger.log_info("SSL Disabled (server.key / server.crt not found)")

        # ── Admin server (CIMS Web API) ──────────────────────────────────────
        admin_conf = config.get('Server', {'Ip': '0.0.0.0', 'Port': 4420})
        cims_kwargs = {'config': config}
        admin_server = HttpServer(
            admin_conf.get('Ip', '0.0.0.0'),
            admin_conf.get('Port', 4420),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_AUTH_HANDLER_LIST + CIMS_ADMIN_HANDLER_LIST
        ])
        admin_server.add_dynamic_rules(FLOW_HANDLER_LIST)
        admin_server.add_dynamic_rules(CIMS_RECORDING_HANDLER_LIST)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_STATS_HANDLER_LIST + CIMS_ORG_HANDLER_LIST + CIMS_VERIFICATION_HANDLER_LIST
        ])
        admin_server.start()
        logger.log_info(f"Admin server started on port {admin_conf.get('Port', 4420)}")

        # ── MCPTT server (IdMS / GMS / CMS / KMS) ───────────────────────────
        mcptt_conf = config.get('McpttServer', {'Ip': '0.0.0.0', 'Port': 4430})
        mcptt_server = HttpServer(
            mcptt_conf.get('Ip', '0.0.0.0'),
            mcptt_conf.get('Port', 4430),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        mcptt_server.add_dynamic_rules(CSC_HANDLER_LIST)
        mcptt_server.start()
        logger.log_info(f"MCPTT server started on port {mcptt_conf.get('Port', 4430)}")

        # ── CSP 전용 내부 API — loopback 전용 plain HTTP ─────────────────
        _internal_conf = config.get('InternalServer', {'Ip': '127.0.0.1', 'Port': 4422})
        internal_server = HttpServer(
            _internal_conf.get('Ip', '127.0.0.1'),
            _internal_conf.get('Port', 4422),
            ssl_keyfile=None,   # plain HTTP
            ssl_certfile=None,
        )
        internal_server.add_dynamic_rules(CSC_INTERNAL_HANDLER_LIST)
        internal_server.start()
        logger.log_info(f"Internal server started on {_internal_conf.get('Ip')}:{_internal_conf.get('Port', 4422)} (plain HTTP, loopback+token)")

        # Notify CSP that CSC has (re)started so it resyncs all state from DB
        try:
            notify_csp("CSC_RESTART", "", "START")
            logger.log_info("CSC_RESTART notification sent to CSP")
        except Exception as e:
            logger.log_error(f"CSC_RESTART notification failed: {e}")

        while True:
            time.sleep(1)

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)
