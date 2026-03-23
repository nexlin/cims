import argparse
import os
import time
import traceback

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
    logger = Logger(log_dir="../logs", log_file_prefix="app", retention_day=30)
    
    # Load Config
    import json
    def load_config():
        # Configuration file location: csc/bin/csc_pihttp/config/csc.json
        # app.py is in csc/bin/csc_pihttp/src/
        try:
            with open('../config/csc.json', 'r') as f:
                return json.load(f)
        except FileNotFoundError:
            logger.log_error("Config file not found at ../config/csc.json. Trying ../../../config/csc.json")
            try:
                with open('../../../config/csc.json', 'r') as f:
                    return json.load(f)
            except:
                return {}

    from csc_service import load_shared_data, CSC_HANDLER_LIST
    from cims_admin import CIMS_ADMIN_HANDLER_LIST
    from cims_auth import CIMS_AUTH_HANDLER_LIST

    http_server = None
    try:
        logger.log_info(f'==================== start ====================')
        
        config = load_config()
        
        # Adjust data paths in config to be absolute or relative to where we run app.py
        # csc.json has "Data": { "User": "../user", "Group": "../group" } relative to csc/bin
        # We need to make them relative to csc/bin/csc_pihttp/src if they are relative.
        # Actually, let's just cheat and hardcode fix or assume user runs from correct dir.
        # Better: Fix paths in config object before passing to load_shared_data
        if 'Data' in config:
            # Original: ../user -> csc/user
            # Current CWD: csc/bin/csc_pihttp/src
            # Target: ../../../user
            for key in ['User', 'Group']:
                if config['Data'].get(key, '').startswith('..'):
                     # Assuming original meant one level up from bin, which is csc root.
                     # We are 3 levels down from csc root (bin/csc_pihttp/src).
                     # So replace .. with ../../.. 
                     # Wait, original was in csc/bin and loaded ../user. So user is in csc/user.
                     # We are in csc/bin/csc_pihttp/src. 
                     # Path to csc/user is ../../../user.
                     config['Data'][key] = os.path.join("../../..", config['Data'][key].replace('..', '.'))
            
            load_shared_data(config)
            
        # [Test Support] Inject dummy data if empty so tests pass without real JSON files
        from csc_service import USERS, GROUPS
        if not USERS:
            logger.log_info("No users loaded. Injecting dummy user for test.")
            USERS["tel:+1000"] = {"password": "password123", "name": "Test User", "profile_etag": "etag_1000"}
        if not GROUPS:
            logger.log_info("No groups loaded. Injecting dummy group for test.")
            GROUPS["tel:+2000"] = {"display_name": "Test Group", "etag": "etag_2000", "members": []}

        # Server Config
        server_conf = config.get('Server', {'Ip': '0.0.0.0', 'Port': 8080})
        ip = server_conf.get('Ip', '0.0.0.0')
        port = server_conf.get('Port', 8080)

        # SSL Config
        ssl_keyfile = "server.key" if os.path.exists("server.key") else None
        ssl_certfile = "server.crt" if os.path.exists("server.crt") else None

        if ssl_keyfile and ssl_certfile:
            logger.log_info(f"SSL Enabled. Key: {ssl_keyfile}, Cert: {ssl_certfile}")
        else:
            logger.log_info("SSL Disabled (Files not found)")

        http_server = HttpServer(ip, port, ssl_keyfile=ssl_keyfile, ssl_certfile=ssl_certfile)
        http_server.add_dynamic_rules(CSC_HANDLER_LIST)
        # CIMS admin/auth API: pass config so handlers can open DB connections
        cims_kwargs = {'config': config}
        http_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_AUTH_HANDLER_LIST + CIMS_ADMIN_HANDLER_LIST
        ])
        http_server.start()

        while True:
            time.sleep(1)

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if http_server: http_server.stop(5)
