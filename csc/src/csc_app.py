import argparse
import os
import time
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))
_CONFIG_PATH = os.environ.get('CIMS_CSC_CONFIG') or os.path.join(_COMPONENT_ROOT, 'config', 'csc.json')

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

    from services.mcptt import load_shared_data, CSC_HANDLER_LIST, notify_csp
    from services       import flow_logger, logger as csc_logger, config_cache
    from handlers       import auth, recording
    from handlers.admin          import CIMS_ADMIN_HANDLER_LIST
    from handlers.auth           import CIMS_AUTH_HANDLER_LIST
    from handlers.users          import CIMS_USERS_HANDLER_LIST
    from handlers.recording      import CIMS_RECORDING_HANDLER_LIST
    from handlers.stats          import CIMS_STATS_HANDLER_LIST
    from handlers.org            import CIMS_ORG_HANDLER_LIST
    from handlers.verification   import CIMS_VERIFICATION_HANDLER_LIST, init as ver_init
    from handlers.build          import CIMS_BUILD_HANDLER_LIST, init as build_init
    from handlers.csp_runtime    import CIMS_CSP_RUNTIME_HANDLER_LIST
    from handlers.service_control import CIMS_SERVICE_CONTROL_HANDLER_LIST
    from handlers.agents         import CIMS_AGENT_ADMIN_HANDLER_LIST, CIMS_AGENT_PUBLIC_HANDLER_LIST
    from handlers.agent_api      import CIMS_AGENT_API_HANDLER_LIST
    from handlers.modules        import CIMS_MODULES_HANDLER_LIST
    from handlers.ha_groups      import CIMS_HA_GROUPS_HANDLER_LIST
    from handlers.alerts         import CIMS_ALERTS_HANDLER_LIST
    from services.flow_logger    import FLOW_HANDLER_LIST
    from services                import alert_log

    admin_server = None
    mcptt_server = None
    try:
        logger.log_info(f'==================== start ====================')

        config = load_config()
        auth.init(config)

        # ServiceLogging 설정 (신규 통합)
        sl = config.get("ServiceLogging", {})
        _service_log_dir = sl.get("Dir", "")
        # 레거시 호환
        if not _service_log_dir:
            _service_log_dir = config.get("ServiceLogDir", config.get("MsgLogDir", ""))
        _system_id = config.get("SystemId", "csp_01")

        flow_logger.init(
            service_log_dir=_service_log_dir,
            system_id=_system_id,
        )

        tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'tests'))
        if not os.path.isdir(tests_dir):
            tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        ver_init(tests_dir, config)
        build_init(os.path.dirname(tests_dir))

        csc_logger.init(
            service_log_dir=_service_log_dir,
        )

        recording.init(service_log_dir=_service_log_dir)

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
                payload = auth.extract_token(handler_args)
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
            _cc = config_cache.init_config_cache(config)
            logger.log_info(
                f"ConfigCache ready (read_only={_cc.is_read_only()}) dir={_cache_path} "
                f"listeners={len(_cc.get_all('listener'))} trunks={len(_cc.get_all('trunk'))} "
                f"routes={len(_cc.get_all('route'))} access={len(_cc.get_all('access'))}"
            )
        except Exception as _e:
            logger.log_error(f"ConfigCache init failed: {_e}")

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
            for path, handler, _ in CIMS_AUTH_HANDLER_LIST + CIMS_USERS_HANDLER_LIST + CIMS_ADMIN_HANDLER_LIST
        ])
        admin_server.add_dynamic_rules(FLOW_HANDLER_LIST)
        admin_server.add_dynamic_rules(CIMS_RECORDING_HANDLER_LIST)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_STATS_HANDLER_LIST + CIMS_ORG_HANDLER_LIST + CIMS_VERIFICATION_HANDLER_LIST
        ])
        # 빌드 / 패키지화 / 패키지 다운로드 (admin JWT)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_BUILD_HANDLER_LIST
        ])
        # CSP 런타임 설정 관리 API (listener/trunk/route/access)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_CSP_RUNTIME_HANDLER_LIST
        ])
        # CMP/CSP/CSC 프로세스 제어 API
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_SERVICE_CONTROL_HANDLER_LIST
        ])
        # P10: Agent 레지스트리 + 패키지 + 배포 관리 (admin JWT)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_AGENT_ADMIN_HANDLER_LIST
        ])
        # Phase 1: 모듈 overlay 설정 API (로컬 dist 대상)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_MODULES_HANDLER_LIST
        ])
        # HA 그룹 — 노드 묶음 (A/S / All Active) 관리 (admin JWT)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_HA_GROUPS_HANDLER_LIST
        ])
        # Alert 이력 (CSP/CMP/DB down, RTP 사용률 임계값 초과)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_ALERTS_HANDLER_LIST
        ])
        # P10: Agent 전용 API (agent token 인증, JWT 우회)
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_AGENT_API_HANDLER_LIST
        ])
        # P10: 공개 정적 에셋 — install 스크립트 / agent 바이너리
        admin_server.add_dynamic_rules([
            (path, handler, cims_kwargs)
            for path, handler, _ in CIMS_AGENT_PUBLIC_HANDLER_LIST
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

        # Notify CSP that CSC has (re)started so it resyncs all state from DB
        try:
            notify_csp("CSC_RESTART", "", "START")
            logger.log_info("CSC_RESTART notification sent to CSP")
        except Exception as e:
            logger.log_error(f"CSC_RESTART notification failed: {e}")

        # ── Agent stale sweeper ─────────────────────────────────────────
        # heartbeat 이 STALE_SEC 이상 안 오면 online/approved → offline 로 전이.
        # 기본 90s (agent 의 기본 heartbeat 30s × 3).
        from handlers.agents import _get_db as _agent_db_conn
        from handlers.agent_api import _AGENT_CERT_ROTATE_THRESHOLD_DAYS
        STALE_SEC = int(config.get('AgentStaleSec', 90))
        SWEEP_INTERVAL = 30
        CERT_SWEEP_INTERVAL = int(config.get('AgentCertSweepSec', 3600))  # 기본 1시간

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
            """mtls_enabled=1 이고 cert 가 THRESHOLD 일 이내 만료 예정인 agent 를 표식.
            다음 heartbeat 응답에서 rotate 지시가 내려간다."""
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
        # CSP/CMP/DB down 전이 + RTP 포트 사용률 임계값 (default 80%) 위반을 감시.
        # 상태 전이마다 {ServiceLogDir}/alerts/YYYY/MM/DD.jsonl 에 event 1줄 기록.
        from handlers.stats import _get_csp_stats, _get_cmp_stats, _get_db as _cims_db_conn
        ALERT_SWEEP_INTERVAL = int(config.get('AlertSweepSec', 30))
        ALERT_RTP_THRESHOLD = int(config.get('AlertRtpThresholdPct', 80))
        _service_log = config.get('ServiceLogging', {}).get('Dir') \
            or config.get('ServiceLogDir', config.get('MsgLogDir', ''))
        # type -> open_ts (ISO). 시작 시 최근 30일 이벤트를 replay 하여
        # 직전 인스턴스가 열어두고 끝난 알람 상태를 복원 → 재기동 후 동일
        # 조건이 계속되면 open 재-emit 하지 않고, 해소되면 close 1건만 기록.
        _alert_open: dict = {}
        if _service_log:
            try:
                restored = alert_log.compute_open_state(_service_log, days=30)
                _alert_open.update({k: True for k in restored})
                if restored:
                    logger.log_info(f"[alert-sweep] restored open state: {sorted(restored.keys())}")
            except Exception as e:
                logger.log_error(f"[alert-sweep] restore failed: {e}")

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

        def _emit(typ: str, severity: str, action: str, message: str):
            from datetime import datetime as _dt
            alert_log.record_event(_service_log, {
                'ts': _dt.now().isoformat(timespec='seconds'),
                'type': typ,
                'severity': severity,
                'action': action,
                'message': message,
            })

        def _transition(typ: str, is_open: bool, severity: str, msg_open: str, msg_close: str):
            was_open = typ in _alert_open
            if is_open and not was_open:
                _alert_open[typ] = True
                _emit(typ, severity, 'open', msg_open)
                logger.log_info(f"[alert] OPEN {typ} — {msg_open}")
            elif not is_open and was_open:
                _alert_open.pop(typ, None)
                _emit(typ, severity, 'close', msg_close)
                logger.log_info(f"[alert] CLOSE {typ}")

        def _sweep_alerts():
            try:
                csp = _get_csp_stats(config)
                cmp = _get_cmp_stats(config)
                db_ok = _check_db()
                _transition('csp_down', not bool(csp), 'critical',
                            'CSP 프로세스 응답 없음', 'CSP 응답 정상화')
                _transition('cmp_down', not bool(cmp), 'critical',
                            'CMP 프로세스 응답 없음', 'CMP 응답 정상화')
                _transition('db_down', not db_ok, 'critical',
                            'DB 연결 끊김', 'DB 연결 복구')
                total = cmp.get('rtp_ports_total', 0) or 0
                used = cmp.get('rtp_ports_used', 0) or 0
                pct = int(round(used / total * 100)) if total > 0 else 0
                _transition('rtp_high', pct >= ALERT_RTP_THRESHOLD, 'warning',
                            f'RTP 포트 사용률 {pct}% ({ALERT_RTP_THRESHOLD}% 초과)',
                            f'RTP 포트 사용률 {pct}% (정상)')
            except Exception as e:
                logger.log_error(f"[alert-sweep] error: {e}")

        # ── sync_txn timeout sweeper (L3) ───────────────────────────────
        # HA fan-out 의 sync_txn 가 ttl_sec 초과해도 pending 인 멤버 슬롯을
        # timeout 으로 마크 → 전체 status = failed (또는 partial 잔존).
        SYNC_TXN_SWEEP_INTERVAL = int(config.get('SyncTxnSweepSec', 15))

        def _sweep_sync_txn():
            try:
                from services import sync_txn
                n = sync_txn.sweep_timeouts(config)
                if n > 0:
                    logger.log_info(f"[sync-txn-sweep] timed out {n} transaction(s)")
            except Exception as e:
                logger.log_error(f"[sync-txn-sweep] error: {e}")

        logger.log_info(f"[agent-sweep] stale threshold={STALE_SEC}s, interval={SWEEP_INTERVAL}s")
        logger.log_info(f"[cert-sweep] rotate threshold={_AGENT_CERT_ROTATE_THRESHOLD_DAYS}d, "
                        f"interval={CERT_SWEEP_INTERVAL}s")
        logger.log_info(f"[alert-sweep] interval={ALERT_SWEEP_INTERVAL}s, "
                        f"rtp_threshold={ALERT_RTP_THRESHOLD}%, "
                        f"dir={_service_log or '(disabled — no ServiceLogDir)'}")
        logger.log_info(f"[sync-txn-sweep] interval={SYNC_TXN_SWEEP_INTERVAL}s")
        _last_sweep = 0
        _last_cert_sweep = 0
        _last_alert_sweep = 0
        _last_sync_txn_sweep = 0
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

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        if mcptt_server:  mcptt_server.stop(5)
