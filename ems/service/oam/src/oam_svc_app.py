"""CIMS oam-svc — 서비스 관측/관리 독립 모듈 엔트리포인트 (oam_base_service_split P3, D5).

base OAM(게이트웨이) 뒤의 독립 서비스 모듈. csc(가입자/PTT)와 동격으로, 서비스 KPI 관측·
녹취·SIP flow·검증(S1~S6)을 담당한다. loopback 비공개 포트(기본 4480)에 bind 하고 외부 노출은
오직 base 게이트웨이(4419) 경유(I1). base 에는 의존하지 않으며(I3), 자기 토큰을 공유 JwtSecret
(common.json §5)으로 독립 검증한다.

귀속 핸들러(§4):
  - stats(service KPI)   /api/v1/stats/service/*   (CSP/CMP VoIP·PTT 관측)
  - recording            /api/v1/recordings        (녹취 조회/스트리밍)
  - flow_logger          /api/v1/flow, /api/v1/call/logs, /api/v1/ptt/history,
                         /api/v1/security/abnormal-sessions
  - verification         /api/v1/verification      (S1~S6 검증 파이프라인)

D5 — 독립 아티팩트: 본 엔트리포인트는 oam_app.py 의 role flag 재사용이 아니라 자기 프로세스/
버전을 갖는 독립 모듈이다. 공통 코드(httpsrv·util·flow_logger·logger·핸들러)는 **oam/src 에서만**
import 한다(csc_standalone_module.md P4 — csc/src 마운트 폐지). csc 와는 코드 비공유 — 계약
(HTTP/JWT/DB)으로만 결합. (oam-svc↔oam 간 oam/src 공유는 OAM 패밀리 내부로, 별도 고려.)
"""

import argparse
import os
import sys
import time
import traceback
import glob as _glob

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))   # = oam-svc/
_CONFIG_PATH = os.environ.get('CIMS_OAMSVC_CONFIG') \
    or os.path.join(_COMPONENT_ROOT, 'config', 'oam-svc.json')


def _first_dir(cands):
    for c in cands:
        if c and os.path.isdir(c):
            return c
    return None


# ── 공유 라이브러리/핸들러 경로 mount (csc_standalone_module.md P4) ──
#   vendor + oam/src(services: flow_logger·logger / handlers: stats·recording·verification /
#   httpsrv · util) 를 sys.path 에 올린다. **csc/src 마운트 폐지** — oam-svc 가 쓰는 것은 전부
#   oam/src 에 있다(P3b 에서 oam 자체 보유). csc 와 코드 비공유 — base/oam-svc 는 csc 를 안 본다.
#   dev = repo sibling 레이아웃, production = 버전단위 install glob 둘 다 지원.
_repo_root = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..'))

_VENDOR = _first_dir(
    # dev (ems 트리): ems/service/oam → core oam = ../core/oam
    [os.path.join(_repo_root, '..', 'core', 'oam', 'vendor')]
    # dev (구 sibling 레이아웃 호환)
    + [os.path.join(_repo_root, 'oam', 'vendor')]
    + sorted(_glob.glob(os.path.join(_repo_root, '..', 'oam', '*', 'oam', 'vendor')), reverse=True)
    # production: 버전단위 install (modules/oam-svc/<ver> → modules/oam/*)
    + sorted(_glob.glob(os.path.join(_repo_root, '..', '..', 'oam', '*', 'oam', 'vendor')), reverse=True)
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

from httpsrv.server import HttpServer
from util.log_util import Logger


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
        """§7 설정 분리: common.json 존재 시 common.json + services/oam-svc.json,
        부재 시 _CONFIG_PATH(oam-svc.json) 단독. base oam.json 상속(fallback)은 없다 —
        정규 경로는 콘솔 배포설정: OAM 이 job 디스패치 시 config_template default +
        base 공유값(JwtSecret/CimsRuntimeDir/Mgmt.Cidr)을 병합한 완전한 유효설정을
        config.json 으로 실체화한다(agents._materialize_deploy_config). 설정이 비면
        조용히 코드 기본값으로 동작하는 대신 여기서 명시적으로 실패한다."""
        cfg_dir = os.path.dirname(_CONFIG_PATH)
        common_p = os.path.join(cfg_dir, 'common.json')
        merged: dict = {}
        src = None
        try:
            if os.path.isfile(common_p):
                with open(common_p, 'r') as f:
                    _deep_merge(merged, json.load(f))
                svc_p = os.path.join(cfg_dir, 'services', 'oam-svc.json')
                if os.path.isfile(svc_p):
                    with open(svc_p, 'r') as f:
                        _deep_merge(merged, json.load(f))
                src = f'common.json+services/oam-svc.json ({cfg_dir})'
            elif os.path.isfile(_CONFIG_PATH):
                with open(_CONFIG_PATH, 'r') as f:
                    merged = json.load(f)
                src = _CONFIG_PATH
        except Exception as e:
            logger.log_error(f"oam-svc config load error: {e}")
            return {}
        # 배포 overlay (cims_agent 변종 디렉토리) — 실체화된 완전 설정
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
                    logger.log_info(f"oam-svc overlay applied: {overlay} ({n} keys)")
                    if not src:
                        src = overlay
                    break
        except Exception as e:
            logger.log_error(f"oam-svc overlay failed: {e}")
        if src:
            logger.log_info(f"oam-svc config source: {src}")
        else:
            logger.log_error("oam-svc config 없음 — oam-svc.json/common.json/배포 config.json "
                             "중 어느 것도 발견 못함. 콘솔 배포설정으로 배포했는지 확인 필요.")
        return merged

    # 귀속 핸들러 import (공유 모듈) — preflight 가 여기 import 성공을 검증.
    from services import flow_logger, ptt_index, logger as csc_logger
    from services.flow_logger    import FLOW_HANDLER_LIST
    from handlers                import recording, auth
    from handlers.recording      import CIMS_RECORDING_HANDLER_LIST
    from handlers.stats          import CIMS_STATS_HANDLER_LIST, CIMS_STATS_SERVICE_HANDLER_LIST
    from handlers.verification   import CIMS_VERIFICATION_HANDLER_LIST, init as ver_init
    from handlers.subscriber_import import CIMS_SUBSCRIBER_IMPORT_HANDLER_LIST

    admin_server = None
    try:
        logger.log_info('==================== start (oam-svc) ====================')

        config = load_config()
        # 관측 설정 무결성 — 빠지면 관측이 코드 기본값으로 조용히 오동작(엉뚱한 probe
        #   대상·DB 계정)하는 대신 기동 로그에 드러낸다. MediaServer 비움=CMP 관측
        #   비활성(허용)이라 제외.
        for _k in ('CimsDatabase', 'CspNotify', 'CimsRuntimeDir'):
            if not config.get(_k):
                logger.log_warning(f"[config] '{_k}' 미설정 — 관련 관측/기능이 비활성 또는 "
                                   "오동작. 콘솔 배포설정(oam-svc) 확인 필요.")
        auth.init(config)   # 공유 JwtSecret 로 토큰 독립 검증(§5)

        # ── SIGUSR1 = 배포 config reload (agent job_update_config 규약) ──
        # 종전: 핸들러 부재 → 파이썬 기본 동작(종료)으로 update_config 가 oam-svc 를
        # 죽였다. CSP 와 동일 규약으로 config 를 재적용한다 — config dict 를 in-place
        # 로 갱신해 sweeper 등 dict 참조 지점에 전파. bind 계열(Server.Port)과 기동 시
        # 캡처된 값(ServiceLogging.Dir 등 로컬 변수)은 재기동 필요.
        import signal as _signal

        def _on_usr1(_sig, _frm):
            try:
                newc = load_config()
                if newc:
                    config.clear()
                    config.update(newc)
                    auth.init(config)
                    logger.log_info('[reload] SIGUSR1 — config 재적용 '
                                    '(bind/기동 캡처 항목은 재기동 필요)')
                else:
                    logger.log_warning('[reload] SIGUSR1 — 재로드 실패(빈 설정), 기존 유지')
            except Exception as e:
                logger.log_error(f'[reload] SIGUSR1 처리 실패: {e}')

        _signal.signal(_signal.SIGUSR1, _on_usr1)

        if args_dict.get('preflight'):
            if not config:
                print('OAMSVC_PREFLIGHT_FAIL: empty config', flush=True)
                sys.exit(2)
            logger.log_info('[preflight] handler imports + config OK — exit 0 (no bind)')
            print('OAMSVC_PREFLIGHT_OK', flush=True)
            sys.exit(0)

        sl = config.get("ServiceLogging", {})
        _service_log_dir = sl.get("Dir", "") or config.get("ServiceLogDir", config.get("MsgLogDir", ""))
        _system_id = config.get("SystemId", "oam_svc_01")

        # 신뢰망(비정상 세션 탐지에서 '외부' 제외) — mgmt CIDR + config 공인 IP /24 + override.
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
        flow_logger.init(service_log_dir=_service_log_dir, system_id=_system_id,
                         db_config=config.get('CimsDatabase'), trusted_nets=_trusted)
        csc_logger.init(service_log_dir=_service_log_dir, system_id=_system_id)
        # PTT 세션 읽기 모델 — flow_logger 의 /ptt/history 가 ptt_index 를 소비하므로
        # oam-svc 도 base(oam_app)와 동일하게 초기화해야 한다. 미초기화면 빈 목록이 나온다.
        _ptt_idx_cfg = config.get('PttIndex') or {}
        ptt_index.init(_service_log_dir, enabled=bool(_ptt_idx_cfg.get('Enabled', True)))

        # verification (S1~S6) — tests 디렉토리 탐색
        tests_dir = _first_dir([os.path.join(_repo_root, 'tests')]) \
            or os.path.normpath(os.path.join(_repo_root, 'tests'))
        ver_init(tests_dir, config)

        # 녹취 변환툴(ffmpeg) — vendor 우선(air-gapped), 없으면 config.FFmpegBin → PATH.
        _ffmpeg_bin = ''
        for _cand in (os.path.join(_VENDOR or '', 'bin', 'ffmpeg'),
                      os.path.join(_VENDOR or '', 'ffmpeg'),
                      config.get('FFmpegBin', '')):
            if _cand and os.path.exists(_cand):
                _ffmpeg_bin = _cand
                break
        try:
            _tx_workers = max(1, int(config.get('RecordingTranscodeWorkers', 2) or 2))
        except (TypeError, ValueError):
            _tx_workers = 2
        recording.init(service_log_dir=_service_log_dir, ffmpeg_bin=_ffmpeg_bin,
                       transcode_workers=_tx_workers)

        # SSL — 버전무관 runtime cert(modules/oam/runtime/cert) 우선(업그레이드 생존; oam_app
        #   _resolve_oam_cert 가 SoT 로 seed) → oam-svc/cert → oam/cert(dev/동거). loopback 평문 허용.
        #   과거: oam 버전업 시 oam/<ver>/oam/cert 만 보고 못 찾아 평문→게이트웨이 https 업스트림 502.
        ssl_keyfile = ssl_certfile = None
        _cert_cands = []
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

        # bind — 기본 loopback 4480 (I1). Server 섹션 우선.
        admin_conf = config.get('Server', {'Ip': '127.0.0.1', 'Port': 4480})
        cims_kwargs = {'config': config}

        admin_server = HttpServer(
            admin_conf.get('Ip', '127.0.0.1'),
            admin_conf.get('Port', 4480),
            ssl_keyfile=ssl_keyfile,
            ssl_certfile=ssl_certfile,
        )

        def _bind(L):
            return [(path, handler, cims_kwargs) for path, handler, _ in L]

        # FLOW → RECORDING 순서로 /api/v1/recordings 충돌 시 RECORDING 우선(oam_app 과 동일).
        admin_server.add_dynamic_rules(FLOW_HANDLER_LIST)
        admin_server.add_dynamic_rules(CIMS_RECORDING_HANDLER_LIST)
        # stats 전체(/api/v1/stats — health/subscribers/messages/leak + service KPI) 귀속:
        # 서비스 관측 데이터(CSP/CMP probe·DB·서비스 로그)라 base 가 아닌 여기서 서빙.
        # 게이트웨이(base)가 /api/v1/stats 세그먼트를 이리로 프록시(콘솔 URL 불변).
        admin_server.add_dynamic_rules(
            _bind(CIMS_STATS_HANDLER_LIST + CIMS_STATS_SERVICE_HANDLER_LIST
                  + CIMS_VERIFICATION_HANDLER_LIST + CIMS_SUBSCRIBER_IMPORT_HANDLER_LIST))
        admin_server.start()
        logger.log_info(f"oam-svc server started on {admin_conf.get('Ip','127.0.0.1')}:{admin_conf.get('Port', 4480)}")

        # ── 서비스 알람 sweeper (alarm_standardization) ──────────────────
        # 서비스 계열 규칙(csp_down/cmp_down/db_down/rtp_high)의 평가·발화는 oam-svc 소유
        # (oam_base_service_split §4) — probe 대상·DB 가 이 모듈 설정이므로. base 는
        # agent 계열(disk/module)만 평가. 저장(alert_log→ServiceLogging.Dir)·조회 API 는
        # base 유지 — 같은 노드 동거 전제라 동일 디렉토리에 기록한다.
        from services import alarm_sweeper
        ALERT_SWEEP_INTERVAL = int(config.get('AlertSweepSec', 30))
        ALERT_RTP_THRESHOLD = int(config.get('AlertRtpThresholdPct', 80))
        _alert_open = alarm_sweeper.restore_open_state(
            _service_log_dir, scope='service', log=logger)
        if _service_log_dir:
            logger.log_info(f"[alert-sweep] interval={ALERT_SWEEP_INTERVAL}s, "
                            f"rtp_threshold={ALERT_RTP_THRESHOLD}%, dir={_service_log_dir}")
        else:
            logger.log_info("[alert-sweep] disabled — no ServiceLogging.Dir")

        _last_alert_sweep = 0.0
        while True:
            time.sleep(1)
            if _service_log_dir and time.time() - _last_alert_sweep >= ALERT_SWEEP_INTERVAL:
                try:
                    alarm_sweeper.sweep_service_rules(
                        config, _alert_open, _service_log_dir,
                        detected_by='oam-svc', rtp_threshold=ALERT_RTP_THRESHOLD, log=logger)
                except Exception as e:
                    logger.log_error(f"[alarm-sweep] error: {e}")
                _last_alert_sweep = time.time()

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:
            admin_server.stop(5)
        if args_dict.get('preflight'):
            print(f'OAMSVC_PREFLIGHT_FAIL: {e}', flush=True)
            sys.exit(2)
