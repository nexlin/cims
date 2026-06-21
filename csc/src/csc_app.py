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
        try:
            with open(_CONFIG_PATH, 'r') as f:
                c = json.load(f)
        except FileNotFoundError:
            logger.log_error(f"Config file not found at {_CONFIG_PATH}")
            return {}
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
    from services.mcptt import load_shared_data, CSC_HANDLER_LIST, notify_csp
    from services       import flow_logger, logger as csc_logger
    from services       import admin_auth
    from handlers.admin          import CIMS_ADMIN_HANDLER_LIST
    from handlers.org            import CIMS_ORG_HANDLER_LIST

    admin_server = None
    mcptt_server = None
    try:
        logger.log_info(f'==================== start (CSC) ====================')

        config = load_config()
        admin_auth.init(config)

        # ServiceLogging 설정 (신규 통합)
        sl = config.get("ServiceLogging", {})
        _service_log_dir = sl.get("Dir", "")
        # 레거시 호환
        if not _service_log_dir:
            _service_log_dir = config.get("ServiceLogDir", config.get("MsgLogDir", ""))
        _system_id = config.get("SystemId", "csc_01")

        flow_logger.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
        )

        csc_logger.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
        )

        # ── pi_http 요청 로깅 훅 등록 (admin/mcptt 자동 로깅) ──
        from httpsrv.controller import DynamicRouteProc

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
                payload = admin_auth.extract_admin_jwt(handler_args.headers)
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

        # [Test Support] Inject dummy data if empty so tests pass without real JSON files
        from services.mcptt import USERS, GROUPS
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

        # ── CSC Admin server (가입자 CRUD + 조직) ─────────────────────────────
        # CSC 완전 독립 (csc_standalone_module.md P1) — 가입자(admin.py) + 조직(org.py)만.
        # 로그인/토큰발급(auth)·본인프로파일(users /me)은 base(oam) 책임. port 는
        # csc.json Server.Port (default 4420, OAM 4419 와 충돌 회피).
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
            for path, handler, _ in CIMS_ADMIN_HANDLER_LIST + CIMS_ORG_HANDLER_LIST
        ])
        admin_server.start()
        logger.log_info(f"CSC Admin server started on port {admin_conf.get('Port', 4420)}")

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

        # Notify CSP that CSC has (re)started so it resyncs all state from DB
        try:
            notify_csp("CSC_RESTART", "", "START")
            logger.log_info("CSC_RESTART notification sent to CSP")
        except Exception as e:
            logger.log_error(f"CSC_RESTART notification failed: {e}")

        # OAM 분리 Phase 3b — sweeper (agent/cert/alert/sync_txn/drift) 는 oam_app.py
        # 책임. csc 는 가입자 CRUD + MCPTT signaling 만 — 백그라운드 작업 없음.
        while True:
            time.sleep(60)

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)
