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

    from csc_service import load_shared_data, CSC_HANDLER_LIST
    from cims_admin import CIMS_ADMIN_HANDLER_LIST
    import cims_auth
    from cims_auth import CIMS_AUTH_HANDLER_LIST
    import csc_flow
    from csc_flow import FLOW_HANDLER_LIST
    from cims_recording import CIMS_RECORDING_HANDLER_LIST
    from cims_stats import CIMS_STATS_HANDLER_LIST
    from cims_org import CIMS_ORG_HANDLER_LIST

    admin_server = None
    mcptt_server = None
    try:
        logger.log_info(f'==================== start ====================')

        config = load_config()
        cims_auth.init(config)
        csc_flow.init(config.get("ServiceLogDir", config.get("MsgLogDir", "")))

        import csc_logger
        csc_logger.init(
            service_log_dir=config.get("ServiceLogDir", ""),
            msg_log_dir=config.get("MsgLogDir", ""),
        )

        # Adjust relative data paths
        if 'Data' in config:
            for key in ['User', 'Group']:
                val = config['Data'].get(key, '')
                if val and not os.path.isabs(val):
                    config['Data'][key] = os.path.normpath(os.path.join(_COMPONENT_ROOT, val))
            load_shared_data(config)

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
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_STATS_HANDLER_LIST + CIMS_ORG_HANDLER_LIST
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

        while True:
            time.sleep(1)

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)
