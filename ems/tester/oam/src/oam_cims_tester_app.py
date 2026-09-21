"""CIMS oam-cims-tester — 계측기 컨트롤러 독립 모듈 엔트리포인트 (test_instrument.md).

base OAM(게이트웨이) 뒤의 독립 서비스 모듈 — oam-svc 와 동격. loopback 비공개 포트(기본 4490)에
bind 하고 외부 노출은 오직 base 게이트웨이(4419) 경유(I1). base 에 의존하지 않으며(I3) 공유
JwtSecret 으로 토큰을 독립 검증한다. 공통 코드(httpsrv·util·services.file_store·admin_auth·
live_bus)는 oam/src 에서만 import 한다(oam_svc_app 과 같은 규약).

귀속: /api/v1/tester (세그먼트 하나 — pkg.json gateway.routes).
"""

import argparse
import os
import sys
import time
import traceback
import glob as _glob

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))   # = oam-cims-tester/
_CONFIG_PATH = os.environ.get('CIMS_TESTER_CONFIG') \
    or os.path.join(_COMPONENT_ROOT, 'config', 'oam-cims-tester.json')


def _first_dir(cands):
    for c in cands:
        if c and os.path.isdir(c):
            return c
    return None


# ── 공유 라이브러리/핸들러 경로 mount — oam/src(+vendor). dev(ems 트리)·production(버전단위 install) 둘 다.
_repo_root = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..'))

_VENDOR = _first_dir(
    [os.path.join(_repo_root, '..', 'core', 'oam', 'vendor')]                                   # dev: ems/tester/oam → ems/core/oam
    + [os.path.join(_repo_root, 'oam', 'vendor')]                                               # dist 형제
    + sorted(_glob.glob(os.path.join(_repo_root, '..', 'oam', '*', 'oam', 'vendor')), reverse=True)
    + sorted(_glob.glob(os.path.join(_repo_root, '..', '..', 'oam', '*', 'oam', 'vendor')), reverse=True)  # modules/oam-cims-tester/<ver> → modules/oam/*
)
if _VENDOR and _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)

_OAM_SRC = _first_dir(
    [os.path.join(_repo_root, '..', 'core', 'oam', 'src')]
    + [os.path.join(_repo_root, 'oam', 'src')]
    + sorted(_glob.glob(os.path.join(_repo_root, '..', 'oam', '*', 'oam', 'src')), reverse=True)
    + sorted(_glob.glob(os.path.join(_repo_root, '..', '..', 'oam', '*', 'oam', 'src')), reverse=True)
)
if _OAM_SRC and _OAM_SRC not in sys.path:
    sys.path.insert(0, _OAM_SRC)
# 자기 src 는 oam/src 보다 앞 — services.tester_* / handlers.tester 는 여기에만 있다.
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from httpsrv.server import HttpServer   # noqa: E402
from util.log_util import Logger        # noqa: E402


if __name__ == '__main__':

    parser = argparse.ArgumentParser()
    parser.add_argument('--preflight', action='store_true',
                        help='import + config 스모크만 수행하고 (bind 없이) 종료 — self-upgrade 검증용')
    args_dict = vars(parser.parse_args())

    logger = Logger(log_dir=os.path.join(_COMPONENT_ROOT, "log"), log_file_prefix="app", retention_day=30)

    import json

    def _deep_merge(dst: dict, src: dict) -> dict:
        for k, v in (src or {}).items():
            if isinstance(v, dict) and isinstance(dst.get(k), dict):
                _deep_merge(dst[k], v)
            else:
                dst[k] = v
        return dst

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

    def load_config():
        """oam_base_service_split §7: common.json 존재 시 common.json + services/oam-cims-tester.json,
        부재 시 _CONFIG_PATH 단독. 정규 경로는 콘솔 배포설정 → 실체화된 config.json overlay."""
        cfg_dir = os.path.dirname(_CONFIG_PATH)
        common_p = os.path.join(cfg_dir, 'common.json')
        merged: dict = {}
        src = None
        try:
            if os.path.isfile(common_p):
                with open(common_p, 'r') as f:
                    _deep_merge(merged, json.load(f))
                svc_p = os.path.join(cfg_dir, 'services', 'oam-cims-tester.json')
                if os.path.isfile(svc_p):
                    with open(svc_p, 'r') as f:
                        _deep_merge(merged, json.load(f))
                src = f'common.json+services/oam-cims-tester.json ({cfg_dir})'
            elif os.path.isfile(_CONFIG_PATH):
                with open(_CONFIG_PATH, 'r') as f:
                    merged = json.load(f)
                src = _CONFIG_PATH
        except Exception as e:
            logger.log_error(f"oam-cims-tester config load error: {e}")
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
                    n = _apply_overlay(merged, flat)
                    logger.log_info(f"oam-cims-tester overlay applied: {overlay} ({n} keys)")
                    if not src:
                        src = overlay
                    break
        except Exception as e:
            logger.log_error(f"oam-cims-tester overlay failed: {e}")
        if src:
            logger.log_info(f"oam-cims-tester config source: {src}")
        else:
            logger.log_error("oam-cims-tester config 없음 — oam-cims-tester.json/common.json/배포 config.json "
                             "중 어느 것도 발견 못함. 콘솔 배포설정으로 배포했는지 확인 필요.")
        return merged

    # 귀속 핸들러 import — preflight 가 import 성공을 검증.
    from handlers import auth
    from handlers.tester import TESTER_HANDLER_LIST, init as tester_init
    from services import tester_store

    admin_server = None
    try:
        logger.log_info('==================== start (oam-cims-tester) ====================')

        config = load_config()
        if not (config.get('CimsAuth') or {}).get('JwtSecret'):
            logger.log_warning("[config] 'CimsAuth.JwtSecret' 미설정 — 게이트웨이 경유 토큰을 검증할 수 없다. "
                               "콘솔 배포설정(base 주입) 확인 필요.")
        auth.init(config)   # 공유 JwtSecret 로 토큰 독립 검증

        import signal as _signal

        def _on_usr1(_sig, _frm):
            try:
                newc = load_config()
                if newc:
                    from services import config_reload as _cr
                    kept = _cr.apply_reload(config, newc)
                    auth.init(config)
                    logger.log_info(f'[reload] SIGUSR1 — config 재적용 ({kept}건 런타임 보존) '
                                    '(bind/기동 캡처 항목은 재기동 필요)')
                else:
                    logger.log_warning('[reload] SIGUSR1 — 재로드 실패(빈 설정), 기존 유지')
            except Exception as e:
                logger.log_error(f'[reload] SIGUSR1 처리 실패: {e}')

        _signal.signal(_signal.SIGUSR1, _on_usr1)

        if args_dict.get('preflight'):
            if not config:
                print('TESTER_PREFLIGHT_FAIL: empty config', flush=True)
                sys.exit(2)
            logger.log_info('[preflight] handler imports + config OK — exit 0 (no bind)')
            print('TESTER_PREFLIGHT_OK', flush=True)
            sys.exit(0)

        # 관리 store 의 자기 서브트리(modules/oam-cims-tester/runtime — I5 단일 소유)에 소유권 리스를 잡는다.
        #   base oam 은 store 루트를, 계측기는 자기 서브트리를 각각 flock 한다(oam_ha.md §4.4 단일 writer). 못 잡으면
        #   read-only 로 뜬다 — 토폴로지 저장·run 색인이 not_lease_owner 로 거절되고 이유가 응답에 나온다.
        from services import file_store as _fs, lease as _lease
        _lease_root = os.path.join(_fs.runtime_root(config), 'modules', 'oam-cims-tester', 'runtime')
        _lst = _lease.acquire(_lease_root)
        if _lst.get('active'):
            logger.log_info(f"[store] lease acquired: {_lease_root} (epoch {_lst.get('epoch')})")
        else:
            logger.log_warning(f"[store] lease NOT acquired: {_lease_root} — {_lst.get('reason')} (read-only)")

        tester_init(_COMPONENT_ROOT, config)
        logger.log_info(f"[tester] data_dir={tester_store.data_dir()} "
                        f"scenarios={len(tester_store.list_scenarios())} profiles={len(tester_store.list_profiles())} "
                        f"topologies={len(tester_store.list_topologies())}")
        # run 오케스트레이터 + 워커 관측 스트림 수신(TCP JSONL, Tester.WorkerStreamIp:Port)
        from services.tester_run import RUNS
        RUNS.init(config, logger)

        # SSL — 버전무관 runtime cert 우선(lifecycle 엔진 ensure_node_cert 가 기동 전에 보증) → 자기 cert → oam cert.
        ssl_keyfile = ssl_certfile = None
        _cert_cands = [os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime', 'cert'))]
        if _OAM_SRC:
            _cert_cands.append(os.path.normpath(os.path.join(_OAM_SRC, '..', '..', '..', 'runtime', 'cert')))
        _cert_cands.append(os.path.join(_COMPONENT_ROOT, 'cert'))
        if _OAM_SRC:
            _cert_cands.append(os.path.normpath(os.path.join(_OAM_SRC, '..', 'cert')))
        for _cert_dir in _cert_cands:
            if os.path.exists(os.path.join(_cert_dir, 'server.key')) and \
               os.path.exists(os.path.join(_cert_dir, 'server.crt')):
                ssl_keyfile = os.path.join(_cert_dir, 'server.key')
                ssl_certfile = os.path.join(_cert_dir, 'server.crt')
                break
        logger.log_info(f"SSL {'Enabled' if ssl_keyfile else 'Disabled'}")

        admin_conf = config.get('Server', {'Ip': '127.0.0.1', 'Port': 4490})
        cims_kwargs = {'config': config}
        admin_server = HttpServer(
            admin_conf.get('Ip', '127.0.0.1'),
            admin_conf.get('Port', 4490),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )
        admin_server.add_dynamic_rules([(path, handler, cims_kwargs) for path, handler, _ in TESTER_HANDLER_LIST])
        admin_server.start()   # bind 실패는 예외 → 종료 → watchdog 재기동 (§8)
        logger.log_info(f"oam-cims-tester server started on {admin_conf.get('Ip','127.0.0.1')}:{admin_conf.get('Port', 4490)}")

        # ── 일일 스윕: run 보존 ─────────────────────────────────────────
        _last_purge = 0.0
        while True:
            time.sleep(1)
            if time.time() - _last_purge >= 3600:
                _last_purge = time.time()
                try:
                    retain = int(((config.get('Tester') or {}).get('RunRetainDays')) or 0)
                    n = tester_store.purge_runs(retain)
                    if n:
                        logger.log_info(f"[run-purge] {n} run(s) removed (retain={retain}d)")
                except Exception as e:
                    logger.log_error(f"[run-purge] error: {e}")

    except KeyboardInterrupt:
        logger.log_info('interrupted')
    except Exception as e:
        logger.log_error(f"fatal: {e}\n{traceback.format_exc()}")
        sys.exit(1)
    finally:
        try:
            from services.tester_run import RUNS as _R
            _R.shutdown()
        except Exception:
            pass
        try:
            if admin_server is not None:
                admin_server.stop()
        except Exception:
            pass
        logger.log_info('==================== stop (oam-cims-tester) ====================')
