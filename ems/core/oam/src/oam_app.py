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

인프라(admin_auth/file_store/ha_lookup/service_registry/sync_txn/drift_sweeper/alert_log/
collection_schema/config_cache/flow_logger/logger) + httpsrv + util 는 oam/src 자체 보유
(csc_standalone_module.md P3b — csc/src 마운트 폐지, 코드 비공유). base 는 mcptt(notify_csp/
audit)를 쓰지 않는다 — MCPTT→CSP notify 는 csc 전용. base↔csc 결합은 계약(HTTP/JWT/DB)만.
"""

import argparse
import glob as _glob
import os
import shutil as _shutil
import sys
import time
import traceback

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..'))  # = oam/
_CONFIG_PATH = os.environ.get('CIMS_OAM_CONFIG') or os.path.join(_COMPONENT_ROOT, 'config', 'oam.json')


def _runtime_dir(config=None):
    """버전무관 runtime 루트 — `CimsRuntimeDir` 우선, 없으면 modules/oam/runtime 추정."""
    d = (config or {}).get('CimsRuntimeDir')
    if d:
        return d
    return os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime'))


def _secrets_dir(config=None):
    """시크릿 격리 디렉토리(0700) — **노드 로컬**(services.paths).

    `CimsRuntimeDir` 에서 유도하면 안 된다: 이중화에서 그 값은 **공유 store**를 가리키고,
    개인키(그룹 CA·mTLS CA)를 볼륨에 올리는 것은 설계 위반이다(oam_ha.md §5 — 복제가 아니라
    join 1회 복사)."""
    from services import paths as _paths
    return _paths.secrets_dir(config)


def _resolve_jwt_secret(config):
    """콘솔/게이트웨이 토큰 서명 시크릿 해석 — config → `_secrets/jwt_secret` → 생성.

    패키지에는 시크릿을 동봉하지 않는다(하드코딩 상수는 예측 가능 = 토큰 위조). 해석 순서:
      1) 설정값(배포 overlay 또는 oam.json) — **이중화의 정본**. 두 노드에 같은 값이
         주입되므로 절체 후에도 세션·모듈 토큰 검증이 유지된다(oam_ha.md §5).
      2) `<runtime>/_secrets/jwt_secret` — 부트스트랩이 만든 노드 로컬 파일.
      3) 없으면 **노드 로컬로 1회 생성**(0600) + 경고. 관리평면이 부팅 불가가 되는 것보다
         안전하되, 이 노드 토큰은 피어와 호환되지 않으므로 경고를 남긴다.
    config 를 제자리에서 갱신한다."""
    import base64 as _b64
    ca = config.setdefault('CimsAuth', {})
    if (ca.get('JwtSecret') or '').strip():
        return ca['JwtSecret']
    path = os.path.join(_secrets_dir(config), 'jwt_secret')
    try:
        with open(path) as f:
            s = f.read().strip()
        if s:
            ca['JwtSecret'] = s
            print(f'[oam-auth] JwtSecret 로드: {path}', flush=True)
            return s
    except Exception:
        pass
    s = _b64.b64encode(os.urandom(32)).decode()
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(s + '\n')
        print(f'[oam-auth] ⚠ JwtSecret 미설정 — 이 노드용으로 새로 생성했습니다: {path}\n'
              f'[oam-auth]   HA(이중화) 구성에서는 배포 설정으로 두 노드에 **같은 값**이 '
              f'주입되어야 합니다. 지금 발급되는 토큰은 피어 노드에서 검증되지 않습니다.',
              flush=True)
    except Exception as e:
        print(f'[oam-auth] ⚠ JwtSecret 파일 기록 실패({e}) — 메모리 값으로만 기동 '
              f'(재기동 시 전 세션 무효)', flush=True)
    ca['JwtSecret'] = s
    return s


def _assert_runtime_mount(config):
    """mount guard — `CimsRuntimeMount` 가 설정돼 있으면 그 경로가 **실제 마운트**인지 확인.

    관리평면 store 가 공유 스토리지(NAS)에 있는 구성에서, 마운트가 안 된 상태로 OAM 이 뜨면
    마운트 포인트 **하부 로컬 디스크**에 두 번째 store 를 만든다. 절체마다 서로 다른 데이터를
    보게 되는 최악의 divergence 이고 조용히 진행되므로, 여기서 **기동을 거부**한다
    (oam_ha.md §4.3). 미설정이면 검사하지 않는다(단일 노드·개발).

    반환 없음. 위반 시 프로세스 종료(비0) — agent 의 health-gate 가 실패로 잡아 롤백/재시도.
    """
    mp = (config or {}).get('CimsRuntimeMount')
    if not mp:
        return
    mp = str(mp).rstrip('/')
    rt = os.path.abspath((config or {}).get('CimsRuntimeDir') or '')
    mounted = False
    try:
        with open('/proc/mounts') as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[1].rstrip('/') == mp:
                    mounted = True
                    break
    except Exception as e:
        print(f'[oam-mount] /proc/mounts 확인 실패({e}) — guard 통과시킴', flush=True)
        return

    if not mounted:
        # 마운트가 없다. 막아야 하는 사고는 "**마운트 지점 하부 로컬 디스크에 두 번째
        # store 를 새로 만드는 것**" 이다. 그런데 무조건 기동을 거부하면 관리평면은
        # **되돌릴 통로 없이 영구 정지**한다 — 설정을 고칠 콘솔이 사라지기 때문이다(실측).
        # 그래서 위험한 경우만 거부하고, 안전한 경우는 **직전 로컬 store 로 기동**한다.
        target_has_store = os.path.isdir(os.path.join(rt, 'control')) if rt else False
        local = ''
        try:
            from services import paths as _paths
            local = _paths.local_runtime_dir(config)
        except Exception:
            local = ''
        local_has_store = bool(local) and os.path.isdir(os.path.join(local, 'control'))

        if target_has_store:
            # 그 경로에 이미 store 가 있는데 마운트가 빠졌다 = 공유 store 를 보던 노드가
            # 스토리지를 잃은 상태. 로컬로 갈아타면 분기(divergence)가 되므로 거부한다.
            print(f'OAM_MOUNT_GUARD_FAIL: CimsRuntimeMount={mp} 가 마운트되지 않았습니다. '
                  f'그 경로({rt})에 이미 store 가 있어 로컬로 대체하면 데이터가 분기됩니다 — '
                  f'마운트를 복구한 뒤 기동하세요.', flush=True)
            sys.exit(3)
        if local_has_store:
            # 대상은 비었고 로컬에 기존 store 가 있다 = 아직 이관 전이거나 설정이 잘못 들어갔다.
            # 새 빈 store 를 만들지 않고 **기존 로컬 store 로 뜬다** → 콘솔이 살아 있어
            # 운영자가 설정을 고치거나 이관을 실행할 수 있다. 이 노드는 공유 store 를 쓸 수
            # 없으므로 agent preflight 가 승격 자격에서 제외한다(정상).
            config['CimsRuntimeDir'] = local
            config.pop('CimsRuntimeMount', None)
            print(f'[oam-mount] ⚠ CimsRuntimeMount={mp} 미마운트 — 대상 store 는 비어 있고 '
                  f'로컬 store 가 있으므로 **로컬로 기동**합니다: {local}\n'
                  f'  마운트를 붙인 뒤 콘솔 HA > 공유 store 에서 "이 경로로 이관" 을 실행하세요. '
                  f'(현 상태에서는 관리평면이 이중화되지 않습니다)', flush=True)
            return
        # 양쪽 다 store 가 없다 = 신규 설치인데 마운트가 없다. 여기서 뜨면 마운트 지점 하부
        # 로컬 디스크에 store 가 생기고, 나중에 마운트가 붙으면 그 데이터가 가려진다.
        print(f'OAM_MOUNT_GUARD_FAIL: CimsRuntimeMount={mp} 가 마운트되지 않았습니다. '
              f'지금 기동하면 마운트 지점 하부 로컬 디스크에 store 가 만들어져, 마운트 후 '
              f'그 데이터가 가려집니다 — 마운트를 먼저 붙이세요.', flush=True)
        sys.exit(3)

    if rt and not (rt == mp or rt.startswith(mp + '/')):
        print(f'OAM_MOUNT_GUARD_FAIL: CimsRuntimeDir={rt} 가 마운트 {mp} 하위가 아닙니다 '
              f'— 설정 불일치로 기동을 거부합니다.', flush=True)
        sys.exit(3)
    print(f'[oam-mount] runtime store 마운트 확인: {mp}', flush=True)


def _install_signal_guards(logger) -> None:
    """SIGUSR1/SIGHUP 을 **치명적이지 않게** 만든다.

    agent 의 `update_config` job 은 설정 파일을 쓴 뒤 모듈에 SIGUSR1 을 보낸다(CSP 처럼
    즉시 재적용하는 모듈용). 파이썬의 SIGUSR1 **기본 동작은 프로세스 종료**이므로, 핸들러가
    없으면 **oam 설정을 저장하는 것만으로 OAM 이 죽는다** — 콘솔이 사라지고, 그 상태에서
    되돌릴 통로도 없다(실측 사고).

    OAM 은 bind 포트·store 경로·시크릿을 **기동 시점에** 읽으므로 부분 reload 가 안전하지
    않다. 그래서 신호는 **기록만 하고 무시**하고, 반영은 명시적 재기동(콘솔 restart)으로
    한다 — 조용히 죽는 것보다 낫다.
    """
    import signal as _signal

    def _noop(signum, _frame):
        name = {getattr(_signal, 'SIGUSR1', -1): 'SIGUSR1',
                getattr(_signal, 'SIGHUP', -2): 'SIGHUP'}.get(signum, str(signum))
        logger.log_warning(
            f'{name} 수신 — OAM 은 설정을 기동 시점에 읽으므로 무시합니다. '
            f'변경 반영이 필요하면 콘솔에서 oam restart 를 실행하세요.')

    for sig in ('SIGUSR1', 'SIGHUP'):
        s_ = getattr(_signal, sig, None)
        if s_ is None:
            continue
        try:
            _signal.signal(s_, _noop)
        except Exception as e:                      # 스레드 컨텍스트 등 — 치명적 아님
            logger.log_warning(f'{sig} 핸들러 설치 실패: {e}')


# 인증서 발급(그룹 CA 생성·SAN 재발급)은 **lifecycle 엔진**이 모듈 기동 전에 수행한다
# (agent/lib/cert.sh, oam_ha.md §5.2). oam 이 자기 기동 중에 만들면 그 사이 뜬 oam-svc 가
# cert 를 못 찾고 평문으로 bind 한다 — 발급자와 소비자를 분리해 순환을 없앴다.
# 아래 _generate_self_signed_cert 는 엔진을 거치지 않은 기동(수동 실행 등)을 위한
# 최후 폴백으로만 남는다.


def _generate_self_signed_cert(dest_dir):
    """cert 가 어디에도 없을 때 runtime/cert 에 self-signed 생성 — 부트스트랩 없이 패키지
    배포로 올라온 노드도 항상 HTTPS 로 기동해 에이전트 health-gate(HTTPS 전용)가 성립한다.
    install.sh 의 부트스트랩 생성과 동형 (CN=hostname/O=CIMS + SAN).
    실패 시 (None, None) — 기존 평문 fallback 유지 (호출부가 SSL Disabled 로그)."""
    import socket as _socket
    import subprocess as _subprocess
    try:
        os.makedirs(dest_dir, exist_ok=True)
        host = _socket.gethostname() or 'cims-oam'
        key = os.path.join(dest_dir, 'server.key')
        crt = os.path.join(dest_dir, 'server.crt')
        _subprocess.run(
            ['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '3650',
             '-subj', f'/CN={host}/O=CIMS',
             '-addext', f'subjectAltName=DNS:{host},IP:127.0.0.1',
             '-keyout', key, '-out', crt],
            check=True, capture_output=True, timeout=60)
        os.chmod(key, 0o600)
        print(f'[oam-cert] self-signed TLS cert 생성: {dest_dir} (CN={host})', flush=True)
        return key, crt
    except Exception as e:
        print(f'[oam-cert] self-signed 생성 실패 — 평문 기동: {e}', flush=True)
        return None, None


def _resolve_oam_cert():
    """OAM TLS cert (server.key, server.crt) 경로 결정 — 버전 업그레이드 생존이 핵심.

    cert 가 버전 디렉터리(modules/oam/<ver>/oam/cert)에만 있으면 oam 버전업마다 새 디렉터리엔
    cert 가 없어 평문 기동 → self-upgrade health-gate(HTTPS 프로브) 실패 → 롤백. oam-svc 도
    동일 사유로 평문→게이트웨이 502. 그래서 **버전무관 위치(modules/oam/runtime/cert)** 를 SoT 로 한다.

    우선순위: (1) runtime/cert (업그레이드 생존)  (2) 자기 버전 cert (dev/repo·구 레이아웃)
    (3) 형제 버전 cert  (4) 어디에도 없으면 self-signed 를 runtime 에 생성 (패키지 배포 노드).
    self-heal: runtime 이 비어있고 자기/형제 버전에 cert 가 있으면 runtime 으로 복사 → 이후
    버전업이 자동 상속(부트스트랩 재실행 불요).
    """
    def _ok(d):
        return bool(d) and os.path.exists(os.path.join(d, 'server.key')) \
            and os.path.exists(os.path.join(d, 'server.crt'))

    def _pair(d):
        return os.path.join(d, 'server.key'), os.path.join(d, 'server.crt')

    runtime_cert = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', 'runtime', 'cert'))
    if _ok(runtime_cert):
        return _pair(runtime_cert)

    # migration 소스: 자기 버전 → 형제 버전(최신 우선)
    src = None
    if _ok(os.path.join(_COMPONENT_ROOT, 'cert')):
        src = os.path.join(_COMPONENT_ROOT, 'cert')
    else:
        for cand in sorted(_glob.glob(os.path.join(_COMPONENT_ROOT, '..', '..', '*', 'oam', 'cert')),
                           reverse=True):
            if _ok(cand):
                src = cand
                break
    if not src:
        return _generate_self_signed_cert(runtime_cert)
    # self-heal: runtime 으로 복사(가능하면). 실패해도 src 직접 사용.
    try:
        os.makedirs(runtime_cert, exist_ok=True)
        for fn in ('server.key', 'server.crt'):
            dst = os.path.join(runtime_cert, fn)
            if not os.path.exists(dst):
                _shutil.copy2(os.path.join(src, fn), dst)
        if _ok(runtime_cert):
            return _pair(runtime_cert)
    except Exception:
        pass
    return _pair(src)

# ── Phase 4 vendor: private 환경 (인터넷 없음) 대응 ──
# oam/vendor/ 에 사전 다운로드된 fastapi/uvicorn/pymysql/PyJWT/loguru/requests/
# readerwriterlock + OAM 전용 aiohttp/netifaces/strenum/asyncstdlib 등.
# 빌드 시점: 'pip3 install --target=oam/vendor -r oam/requirements.txt --no-compile'
_VENDOR = os.path.normpath(os.path.join(_COMPONENT_ROOT, 'vendor'))
if os.path.isdir(_VENDOR) and _VENDOR not in sys.path:
    sys.path.insert(0, _VENDOR)

# OAM 자체 인프라 — csc/src 마운트 폐지 (csc_standalone_module.md P3b).
#   oam/src/services(file_store·ha_lookup·admin_auth·sync_txn·drift_sweeper·service_registry·
#   alert_log·collection_schema·config_cache·flow_logger·logger) + httpsrv + util 를 oam 자체
#   복사본으로 보유. base↔csc 결합은 계약(게이트웨이 HTTP + JWT verify + DB 스키마)만 —
#   코드 비공유(독립 업그레이드 가능). oam/src 가 sys.path[0](스크립트 디렉토리)라 별도 mount 불요.

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

    from services       import flow_logger, ptt_index, logger as csc_logger
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
    from handlers.events         import CIMS_EVENTS_HANDLER_LIST
    from handlers.console        import CIMS_CONSOLE_HANDLER_LIST
    from handlers.console_accounts import CIMS_CONSOLE_ACCOUNTS_HANDLER_LIST
    from handlers.console_layouts  import CIMS_CONSOLE_LAYOUTS_HANDLER_LIST
    from handlers.service_descriptors import CIMS_SERVICE_DESCRIPTORS_HANDLER_LIST
    from handlers.api_docs             import CIMS_API_DOCS_HANDLER_LIST
    from handlers.provision            import (CIMS_PROVISION_HANDLER_LIST,
                                               init as provision_init)
    from handlers.external_systems     import CIMS_EXTERNAL_SYSTEMS_HANDLER_LIST
    from handlers.gateway              import CIMS_GATEWAY_HANDLER_LIST, register_gateway
    from handlers.oam_join             import CIMS_OAM_JOIN_HANDLER_LIST
    from services import service_registry
    from services.flow_logger    import FLOW_HANDLER_LIST

    admin_server = None
    try:
        logger.log_info(f'==================== start (OAM) ====================')

        # 설정 저장(update_config)이 보내는 SIGUSR1 로 죽지 않게 — config 로드보다 먼저.
        _install_signal_guards(logger)

        config = load_config()
        # mount guard — store 가 공유 스토리지에 있는 구성에서 마운트 없이 뜨면 로컬 디스크에
        # 두 번째 store 를 만든다. store 접근(마이그레이션·seed) 전에 먼저 확인한다.
        _assert_runtime_mount(config)
        _resolve_jwt_secret(config)     # 패키지 하드코딩 제거 → _secrets 파일/생성으로 해석
        auth.init(config)

        # ── SIGUSR1 = 배포 config reload (agent job_update_config 규약) ──
        # 종전: 핸들러 부재 → 파이썬 기본 동작(종료)으로 update_config 가 OAM 을 죽였다.
        # config dict in-place 갱신으로 dict 참조 지점(sweeper 등)에 전파 — bind 계열
        # (Port)과 기동 시 캡처된 로컬 값은 재기동 필요.
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

        # D3: --preflight 모드 — 여기까지 왔으면 핸들러 import(107~140) + config 로드 OK.
        # bind/마이그레이션/sweeper 없이 즉시 종료(0). agent 가 구 OAM kill 전에 호출.
        if args_dict.get('preflight'):
            if not config:
                print('OAM_PREFLIGHT_FAIL: empty config', flush=True)
                sys.exit(2)
            logger.log_info('[preflight] handler imports + config OK — exit 0 (no bind)')
            print('OAM_PREFLIGHT_OK', flush=True)
            sys.exit(0)

        # ── 관리 store 소유권 리스 (oam_ha.md §4.4) ─────────────────────────
        # store 에 처음 write 하기 전(마이그레이션·seed 포함) 반드시 획득한다. 실패하면
        # **죽지 않고 read-only 로 강등** — 조회는 되어야 운영자가 원인을 볼 수 있다.
        # preflight 는 위에서 이미 종료했다: 검증 실행이 살아있는 OAM 의 잠금·epoch 를
        # 건드리면 안 되므로 리스 획득은 preflight 경로에 없어야 한다.
        from services import file_store as _fs0, lease as _lease
        try:
            _lst = _lease.acquire(_fs0.runtime_root(config))
            if _lst.get('active'):
                logger.log_info(f"[lease] 관리 store 소유권 획득 — node={_lst['node_id']} "
                                f"epoch={_lst['epoch']} path={_lst['path']}")
            else:
                logger.log_error(
                    f"[lease] 소유권 획득 실패({_lst.get('reason')}) — **read-only 모드**로 기동합니다. "
                    f"다른 OAM 프로세스가 같은 store 를 쓰고 있거나(같은 노드 이중 기동), "
                    f"강제 dual-primary 로 두 노드가 같은 볼륨을 열었을 수 있습니다. "
                    f"조회는 가능하고 변경(API/스위퍼)은 409 로 거부됩니다.")
        except Exception as _e:
            logger.log_error(f"[lease] 획득 예외({_e}) — read-only 모드")

        # ── 잘못된 위치의 store 1회 회수 (버전 디렉터리 → 버전무관) ──────────
        # 배포 overlay 에 CimsRuntimeDir 이 없던 노드는 옛 폴백 때문에 store 가 버전
        # 디렉터리 안(`.../current/ext_mnt/runtime`, `cwd/runtime`)에 생겼다 — oam 업그레이드
        # 시 사라지는 위치다(실서버 실측). 폴백은 고쳤지만 **이미 생긴 데이터**는 옮겨줘야
        # 잃지 않는다. 목표 위치에 control/ 이 없을 때만 복사한다(멱등, 덮어쓰기 없음).
        try:
            _rt_now = _fs0.runtime_root(config)
            if not os.path.isdir(os.path.join(_rt_now, 'control')):
                import shutil as _sh
                for _legacy in (os.path.join(_COMPONENT_ROOT, 'ext_mnt', 'runtime'),
                                os.path.join(_COMPONENT_ROOT, '..', 'ext_mnt', 'runtime'),
                                os.path.abspath('runtime')):
                    _legacy = os.path.normpath(_legacy)
                    if os.path.abspath(_legacy) == os.path.abspath(_rt_now):
                        continue
                    if not os.path.isdir(os.path.join(_legacy, 'control')):
                        continue
                    os.makedirs(_rt_now, exist_ok=True)
                    for _ent in os.listdir(_legacy):
                        _src, _dst = os.path.join(_legacy, _ent), os.path.join(_rt_now, _ent)
                        if os.path.exists(_dst):
                            continue
                        if os.path.isdir(_src):
                            _sh.copytree(_src, _dst)
                        else:
                            _sh.copy2(_src, _dst)
                    logger.log_warning(
                        f"[store] 잘못된 위치의 관리 store 회수: {_legacy} → {_rt_now} "
                        f"(버전 디렉터리 안이라 업그레이드 시 소실되는 위치였다. "
                        f"배포 설정 CimsRuntimeDir 를 명시해 두는 것을 권장)")
                    break
        except Exception as _e:
            logger.log_warning(f"[store] 위치 회수 skip: {_e}")

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

        # 비어 있으면 **노드 로컬**로 해석 — 부트스트랩 직후엔 공유 마운트가 없는 것이
        # 정상이다(붙이는 수단이 이 OAM 이 서빙하는 콘솔이다). services/paths 참조.
        from services import paths as _paths
        _service_log_dir = _paths.service_log_dir(config)
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

        # PTT 세션 읽기 모델 — 녹취가 정본이고 인덱스는 파생물이다. Enabled=false 로 두면
        #   조회가 종전처럼 녹취를 직접 스캔한다(되돌리기 경로).
        _ptt_idx_cfg = config.get('PttIndex') or {}
        _ptt_index_enabled = bool(_ptt_idx_cfg.get('Enabled', True))
        PTT_INDEX_INTERVAL = int(_ptt_idx_cfg.get('Interval', 30))
        ptt_index.init(_service_log_dir, enabled=_ptt_index_enabled)

        tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        if not os.path.isdir(tests_dir):
            tests_dir = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', 'tests'))
        ver_init(tests_dir, config)
        build_init(os.path.dirname(tests_dir))

        # ── 자동 배포 (auto_deployment.md) ──────────────────────
        # 블루프린트/인벤토리/run 기록은 runtime store 아래 'provision/' 에 둔다.
        # runtime 은 버전 디렉토리 밖(modules/oam/runtime)이라 업그레이드·롤백에 생존한다.
        try:
            from services import file_store as _prov_fs
            from services.provision.store import Store as _ProvStore
            _prov_dir = os.path.join(_prov_fs.runtime_root(config), 'provision')
            provision_init(config, _ProvStore(_prov_dir))
            logger.log_info(f'[provision] runtime dir: {_prov_dir}')
        except Exception as _pe:      # 배포 엔진 부재가 OAM 기동을 막지 않게
            logger.log_error(f'[provision] 초기화 실패 — /api/v1/provision 비활성: {_pe}')

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

        # SSL certificates — 버전무관 runtime cert(modules/oam/runtime/cert) 우선 + self-heal.
        #   (버전 디렉터리 cert 만 있으면 버전업 시 평문→health-gate 롤백. _resolve_oam_cert 참조.)
        ssl_keyfile, ssl_certfile = _resolve_oam_cert()
        # SAN 점검·그룹 CA 재발급은 여기서 하지 않는다 — lifecycle 엔진이 **기동 전**에
        # 끝낸다(agent/lib/cert.sh, oam_ha.md §5.2). 기동 중에 재발급하면 이미 뜬
        # oam-svc·csc 가 옛 인증서를 계속 서빙해 노드 안에서 인증서가 갈린다.
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
        else:
            # 이미 운용 중인 store — seed 에 새로 생긴 **모듈만** 병합(운영자 편집 보존).
            # 이게 없으면 신규 모듈(관리평면 oam/oam-svc 등)이 기존 노드에서 영구히
            # descriptor 밖에 남아 HA 의 daemon/cold/relevant/헬스 대상이 되지 못한다.
            try:
                _merged = service_registry.merge_seed_modules(config)
                if _merged:
                    logger.log_info(f'[service-registry] merged {_merged} new module(s) from seed dir')
            except Exception as _e:
                logger.log_warning(f'[service-registry] seed module merge skip: {_e}')

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
        base_rules = _bind(CIMS_AUTH_HANDLER_LIST)
        base_rules += _bind(CIMS_BUILD_HANDLER_LIST)
        base_rules += _bind(CIMS_SERVICE_CONTROL_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_ADMIN_HANDLER_LIST)
        base_rules += _bind(CIMS_MODULES_HANDLER_LIST)
        base_rules += _bind(CIMS_HA_GROUPS_HANDLER_LIST)
        if _console_dir:
            base_rules += _bind(CIMS_CONSOLE_STATIC_HANDLER_LIST)
        base_rules += _bind(CIMS_ALERTS_HANDLER_LIST)
        base_rules += _bind(CIMS_EVENTS_HANDLER_LIST)
        base_rules += _bind(CIMS_CONSOLE_HANDLER_LIST)
        # D1 사용자 레이아웃/카탈로그/프로파일 — console.py 보다 구체 경로(최장 일치 우선).
        base_rules += _bind(CIMS_CONSOLE_LAYOUTS_HANDLER_LIST)
        base_rules += _bind(CIMS_CONSOLE_ACCOUNTS_HANDLER_LIST)
        base_rules += _bind(CIMS_SERVICE_DESCRIPTORS_HANDLER_LIST)
        # /api/v1/api-docs — 각 모듈이 코드 옆에 선언한 API 문서 수집(개발자 모드). 메타데이터만
        # 읽으므로 base 상주. 모듈 미설치/미가용이면 그 모듈 API 는 응답에서 빠진다.
        base_rules += _bind(CIMS_API_DOCS_HANDLER_LIST)
        base_rules += _bind(CIMS_PROVISION_HANDLER_LIST)   # /api/v1/provision/* — 자동 배포(내장). 별도 모듈 아님
        base_rules += _bind(CIMS_EXTERNAL_SYSTEMS_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_API_HANDLER_LIST)
        base_rules += _bind(CIMS_AGENT_PUBLIC_HANDLER_LIST)
        base_rules += _bind(CIMS_GATEWAY_HANDLER_LIST)   # /api/v1/gateway/* 제어면(base 소유)
        # /api/v1/ha/join* — 관리평면 2번째 노드 합류(그룹 공통 신원 전달). base 소유.
        base_rules += _bind(CIMS_OAM_JOIN_HANDLER_LIST)

        # D8 — /users/me(identity-plane)는 base 가 소유, 가입자 CRUD(/users/*)는 csc(resource).
        #   all  : ME 를 /api/v1/users 에 mount → 뒤의 SERVICE admin superset 이 overwrite(현행 동작).
        #   base : ME 를 /api/v1/users/me 에 mount → 나머지 /api/v1/users/* 는 게이트웨이가 csc 로
        #          프록시(controller 최장 일치: /me* 는 base, 그 외는 게이트웨이). slim 핸들러는
        #          parts 를 고정 _USERS_BASE 로 파싱하므로 mount 경로와 무관하게 me 를 처리.
        _me_fn = CIMS_ME_HANDLER_LIST[0][1]
        # csc 핸들러(admin/org)가 in-process 로 로드됐을 때만 ME 를 /users 전체에 mount
        # (superset 이 덮어씀). 미로드(P3b 이후 csc 별도 프로세스) 시 base 와 동일하게
        # /users/me 만 잡고 나머지 /users/* 는 아래 게이트웨이 프록시가 csc 로 넘긴다.
        _csc_inproc = bool(CIMS_ADMIN_HANDLER_LIST)
        base_rules.append(('/api/v1/users' if (role == 'all' and _csc_inproc) else '/api/v1/users/me',
                           _me_fn, cims_kwargs))
        admin_server.add_dynamic_rules(base_rules)

        # ── SERVICE (in-process; role=all 에서만; P2+ 게이트웨이 프록시로 이관) ──
        if role == 'all':
            if _csc_inproc:
                # 가입자/조직 CRUD(csc 귀속) — admin superset 이 base slim /me 를 덮어씀.
                admin_server.add_dynamic_rules(_bind(CIMS_ADMIN_HANDLER_LIST + CIMS_ORG_HANDLER_LIST))
            else:
                # P3b(csc/src 마운트 폐지) 이후 csc 핸들러는 OAM 에 동봉되지 않음 —
                # 가입자/조직(csc 귀속) 세그먼트는 role=all 에서도 게이트웨이 프록시로 커버.
                # module='csc' 라우트만 mount: stats/recordings 등 in-process 소유 세그먼트
                # (oam-svc 계열 라우트가 테이블에 있어도) 와의 충돌 방지.
                try:
                    _gw_n = register_gateway(admin_server, config, modules={'csc'})
                    logger.log_info(f"[gateway] role=all hybrid — csc proxy {_gw_n} route(s) mounted")
                except Exception as _e:
                    logger.log_error(f"[gateway] role=all csc proxy mount failed: {_e}")
            # 녹취·flow(oam-svc 귀속) — 자기 init() 상태 사용(raw kwargs). FLOW→RECORDING
            # 순서로 /api/v1/recordings 충돌 시 RECORDING 우선(현행 보존).
            admin_server.add_dynamic_rules(FLOW_HANDLER_LIST)
            admin_server.add_dynamic_rules(CIMS_RECORDING_HANDLER_LIST)
            # 검증·stats 전체(/api/v1/stats — health/subscribers/messages/leak + service KPI,
            # oam-svc 귀속. 서비스 관측 데이터라 base 미등록 — role=base 는 게이트웨이 프록시).
            admin_server.add_dynamic_rules(
                _bind(CIMS_VERIFICATION_HANDLER_LIST + CIMS_STATS_HANDLER_LIST
                      + CIMS_STATS_SERVICE_HANDLER_LIST))
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

        # ── overlay 스키마 정리 (1회, 멱등) ──────────────────────────────
        # deployment overlay 는 config_template 에 선언된 키만 담는다(스키마가 계약).
        # 옛 레코드에 남은 템플릿 밖 키를 치우되, **렌더 결과가 동일함이 증명된 것만**
        # 지운다 — 살아있는 설정이면 경고만 남겨 템플릿 선언 누락을 드러낸다.
        try:
            from handlers.agents import sweep_overlay_schema
            _sw = sweep_overlay_schema(config)
            if _sw["cleaned"] or _sw["kept_keys"]:
                logger.log_info(f"[config-sweep] scanned={_sw['scanned']} "
                                f"cleaned={_sw['cleaned']} kept={len(_sw['kept_keys'])}")
        except Exception as _e:
            logger.log_warning(f"[config-sweep] skip: {_e}")

        # ── Agent stale sweeper ─────────────────────────────────────────
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
        # 코어(emit/transition/서비스 규칙 평가)는 services.alarm_sweeper 공용.
        # 서비스 계열(service_unresponsive/db_down/rtp_pct_gte)은 oam-svc 소유
        # (oam_base_service_split §4) — base 는 role=all(단일 프로세스)에서만 대행 평가하고,
        # role=base 는 agent 계열(disk_high/module_down/config_drift/ha_flap)만 평가한다
        # (CSP/CMP probe·DB 미접속).
        from services import alarm_sweeper
        ALERT_SWEEP_INTERVAL = int(config.get('AlertSweepSec', 30))
        ALERT_RTP_THRESHOLD = int(config.get('AlertRtpThresholdPct', 80))
        from services import paths as _paths2
        _service_log = _paths2.service_log_dir(config)
        # _alert_open: { akey(code@mo_instance) : alarm_id } — 자기 소유 계열만 추적.
        _alert_open: dict = alarm_sweeper.restore_open_state(
            _service_log, scope=('all' if role == 'all' else 'agent'), log=logger)

        # ── FM ingest (모듈 자기보고 — alarm_self_reporting.md) ─────────
        # 소유는 oam-svc — role=all 단일 프로세스에서만 base 가 대행 (sweeper 소유
        # 규약과 동일). 자기보고 계열(detected_by=self)의 활성 상태는 FmIngest 가
        # 자체 복원·추적한다 (_alert_open 과 분리).
        if role == 'all' and _service_log:
            try:
                from services.fm_ingest import FmIngest
                FmIngest(config, _service_log, log=logger).start()
            except Exception as e:
                logger.log_error(f"[fm] ingest start failed: {e}")

        _fmt = alarm_sweeper.fmt

        def _transition(rule, mo_instance, detected_by, is_open, msg_open, msg_close,
                        threshold_info=None):
            alarm_sweeper.transition(_alert_open, _service_log, rule, mo_instance,
                                     detected_by, is_open, msg_open, msg_close,
                                     threshold_info=threshold_info, log=logger)

        def _cold_module_ha_sets() -> tuple:
            """AS 그룹 cold 모듈의 HA 상태별 module_down 평가 보정 집합 2개.
            반환: (skip, must_run) — 각각 {(agent_id, module)}.
            - skip: VIP 미보유 확정(standby) 멤버의 cold 모듈 — 정지가 정상 상태라
              평가 제외 (오탐 방지).
            - must_run: VIP 보유 확정(active) 멤버의 cold 모듈 — 배포기록 status 가
              stopped 여도 실제로는 떠 있어야 정상 (절체 시 notify 가 기동하는 모듈이라
              기록은 stopped 인 채 남는다). 기록 기준 평가에서 빠져 죽어도 무알람이던
              것을 잡는다.
            VIP 관측 None(판정 불가) 멤버는 종전대로 기록 기준 평가."""
            skip, must_run = set(), set()
            try:
                from handlers.ha_groups import _agent_daemon_modules, _group_started_modules
                from services import ha_lookup
                for g in ha_lookup.ha_groups_all(config):
                    if g.get('mode') != 'active_standby':
                        continue
                    fo = g.get('failover_options') if isinstance(g.get('failover_options'), dict) else {}
                    modes = fo.get('module_modes') if isinstance(fo.get('module_modes'), dict) else {}
                    members = ha_lookup.members_of(g)
                    # 서비스 개시 게이트와 동일 기준 — 개시된 모듈만 HA 평가 대상
                    # (설치만 된 모듈을 must-run 으로 오판하지 않게).
                    started = _group_started_modules(members, config)
                    observed = ha_lookup.vip_observation(config, g)['observed']
                    for m in members:
                        aid = m.get('agent_id')
                        if observed.get(aid) is None:
                            continue
                        for mod in _agent_daemon_modules(aid, config):
                            if mod in started and modes.get(mod, 'cold') != 'hot':
                                (must_run if observed.get(aid) else skip).add((aid, mod))
            except Exception as e:
                logger.log_error(f"[alarm-sweep] cold 모듈 HA 집합 계산 실패: {e}")
            return skip, must_run

        def _eval_agent_rule(rule: dict, agent: dict, metric: dict,
                             deps: list,
                             cold_skip: set, cold_must_run: set = frozenset(),
                             expected_cfg: dict | None = None) -> list:
            """scope='agent' 규칙을 한 agent 최신 metric 으로 평가.
            반환: (mo_instance, is_open, msg_open, msg_close, threshold_info, severity) 목록 —
            threshold_info 는 임계 계열의 {observed, threshold, unit}, severity 는 단계
            임계(thresholds) 도달 단계 (단일 임계/비임계 규칙은 None = rule 기본값)."""
            chk = rule.get('check')
            host = agent.get('name') or str(agent.get('id'))
            res = []
            if chk == 'disk_high':
                disk = metric.get('disk_pct')
                if disk is None:
                    return res
                disk = round(float(disk), 1)
                thr = int(rule.get('threshold', 90))
                sev = None
                if isinstance(rule.get('thresholds'), dict):
                    sev, sthr = alarm_sweeper.staged_severity(rule, disk)
                    if sthr is not None:
                        thr = int(sthr)
                    is_open = sev is not None
                else:
                    is_open = disk >= thr
                mo = f"{host}/disk"
                kw = dict(mo=mo, host=host, pct=disk, threshold=thr)
                tinfo = {'observed': disk, 'threshold': thr, 'unit': rule.get('unit') or '%'}
                res.append((mo, is_open, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw), tinfo, sev))
            elif chk == 'module_down':
                running = {(m.get('name') or '').lower()
                           for m in (metric.get('modules') or []) if m.get('name')}
                for dep in deps:
                    if dep.get('agent_id') != agent.get('id'):
                        continue
                    proc = (dep.get('process_name') or dep.get('package_name') or '').lower()
                    # 기록 running 이 평가 기본. 예외: VIP 보유 멤버의 cold 모듈은 절체
                    # notify 가 기동해 기록이 stopped 인 채 실행되는 게 정상 상태라,
                    # 기록 stopped 여도 must-run 으로 평가 (죽어 있으면 알람).
                    must = (agent.get('id'), proc) in cold_must_run
                    if dep.get('status') != 'running' and not (must and dep.get('status') == 'stopped'):
                        continue
                    # 프로세스 생존은 agent 관측이 전 모듈 정본(감지 L1 — 표준화 §3.4(b)).
                    # 원격 probe 무응답은 별개 조건(service_unresponsive)이라 여기서 제외하지 않는다.
                    if not proc:
                        continue
                    # 비데몬(별도 프로세스 없음) 모듈은 module_down 대상 아님 — agent 가 metric.modules
                    # 에 보고하지 않으므로(_NON_DAEMON_MODULES) 'running' 이어도 항상 down 으로 오탐.
                    #   console = OAM 이 정적 서빙(별도 프로세스 없음), agent = 자기 자신.
                    if proc in ('console', 'agent'):
                        continue
                    # cold-spare standby 의 cold 모듈 — 정지가 desired state, 오탐 방지.
                    if (agent.get('id'), proc) in cold_skip:
                        continue
                    mo = f"{host}/{proc}"
                    kw = dict(mo=mo, host=host, module=proc)
                    res.append((mo, proc not in running, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw), None, None))
            elif chk == 'config_drift':
                # 노드 실파일 hash (agent 보고) vs 배포기록 실체화본 hash — 불일치 = 드리프트.
                # 구 agent(cfg_hashes 미보고)는 평가 자체를 건너뜀 (오알람 없음).
                reported = metric.get('cfg_hashes')
                if not isinstance(reported, dict) or not reported:
                    return res
                for dep in deps:
                    if dep.get('agent_id') != agent.get('id'):
                        continue
                    if dep.get('status') not in ('running', 'stopped'):
                        continue
                    proc = (dep.get('process_name') or dep.get('package_name') or '').lower()
                    got = reported.get(proc)
                    exp = (expected_cfg or {}).get((agent.get('id'), proc))
                    if not proc or not got or not exp:
                        continue        # 미보고 모듈/기대값 산출 실패 — 판정 보류
                    mo = f"{host}/{proc}/config"
                    kw = dict(mo=mo, host=host, module=proc, expected=exp, actual=got)
                    res.append((mo, got != exp, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw), None, None))
            elif chk == 'ha_flap':
                # 최근 10분 keepalived 전이 수 (agent 가 notify 로그 tail 로 집계).
                # flap 정지 → 윈도 밖으로 밀려 미보고 → 미평가 close 경로로 자동 해제.
                trans = metric.get('ha_transitions')
                if not isinstance(trans, dict) or not trans:
                    return res
                thr = int(rule.get('threshold', 6))
                for svc, cnt in trans.items():
                    try:
                        cnt = int(cnt)
                    except (TypeError, ValueError):
                        continue
                    sev = None
                    thr_svc = thr
                    if isinstance(rule.get('thresholds'), dict):
                        sev, sthr = alarm_sweeper.staged_severity(rule, cnt)
                        if sthr is not None:
                            thr_svc = int(sthr)
                        is_open = sev is not None
                    else:
                        is_open = cnt >= thr
                    mo = f"{host}/ha/{svc}"
                    kw = dict(mo=mo, host=host, svc=svc, count=cnt, threshold=thr_svc)
                    tinfo = {'observed': cnt, 'threshold': thr_svc, 'unit': rule.get('unit') or '회/10분'}
                    res.append((mo, is_open, _fmt(rule.get('msg_open'), **kw), _fmt(rule.get('msg_close'), **kw), tinfo, sev))
            return res

        def _sweep_agent_alerts(agent_rules: list):
            """per-agent 규칙(disk/module)을 online agent 별로 평가.

            관측 두절 규율(파이프라인 §9): 관측이 끊긴 노드의 알람을 정상 해소로
            위장하지 않는다 — 두절 노드의 열린 agent 알람은 "판정 불가"로 종결하고
            노드 두절 알람(A-COM-015, check=agent_lost)을 연다. 관측 대상이 0건
            (agent 스토어 공백 — 절체 직후 standby 등)이면 아무 판정도 하지 않는다."""
            from handlers.agents import _agent_load_all, _deploy_load_all, _metric_root
            from services import file_store
            agents = [a for a in _agent_load_all(config) if a.get('status') != 'revoked']
            if not agents:
                return
            deps = _deploy_load_all(config)
            mroot = _metric_root(config)
            cold_skip, cold_must_run = _cold_module_ha_sets()
            # 코드 개정 이행 종결 — 옛 code(CIMS-CFG-001 등)로 열린 agent 계열 알람은
            # 현행 code 평가/자동 close 어느 쪽에도 안 잡힌다(akey 의 code 가 다름).
            # 여기서 종결하고, 지속 조건은 아래 평가가 현행 code 로 재발행.
            for r in agent_rules:
                for old in service_registry.legacy_codes(r.get('code')):
                    alarm_sweeper.close_legacy_code(_alert_open, _service_log, r, old,
                                                    'oam', log=logger)
            # config_drift 기대 hash — 배포별 실체화본을 스윕당 1회만 계산 (pkg 캐시).
            expected_cfg: dict = {}
            if any(r.get('check') == 'config_drift' for r in agent_rules):
                from handlers.agents import _pkg_load, deploy_config_hash
                _pkgs: dict = {}
                for dep in deps:
                    if dep.get('status') not in ('running', 'stopped'):
                        continue
                    proc = (dep.get('process_name') or dep.get('package_name') or '').lower()
                    pid = dep.get('package_id')
                    if not proc or pid is None:
                        continue
                    if pid not in _pkgs:
                        try:
                            _pkgs[pid] = _pkg_load(config, pid)
                        except Exception:
                            _pkgs[pid] = None
                    try:
                        expected_cfg[(dep.get('agent_id'), proc)] = \
                            deploy_config_hash(config, _pkgs[pid], dep.get('config'))
                    except Exception:
                        pass
            active = set()
            observed_hosts = set()   # 이번 스윕에서 관측(metric)이 있었던 호스트
            for ag in agents:
                host = ag.get('name') or str(ag.get('id'))
                if ag.get('status') != 'online':
                    continue
                metric = file_store.jsonl_last(mroot, str(ag['id']))
                if not metric:
                    continue
                observed_hosts.add(host)
                for r in agent_rules:
                    if r.get('check') == 'agent_lost':
                        continue    # 두절 규칙은 아래 별도 판정
                    for mo, is_open, msg_open, msg_close, tinfo, sev in _eval_agent_rule(r, ag, metric, deps, cold_skip, cold_must_run, expected_cfg):
                        active.add(f"{r.get('code')}@{mo}")
                        rr = {**r, 'perceived_severity': sev} if sev else r
                        # detected_by 는 주체 클래스만(표준화 §3.4(b)) — 호스트는 mo 가 보유.
                        _transition(rr, mo, 'agent', is_open, msg_open, msg_close, tinfo)
            agent_rule_by_code = {r.get('code'): r for r in agent_rules}
            # 관측 두절 판정 (agent_lost) — 노드 두절 알람 open/close + 두절 노드의
            # 잔여 알람 판정 불가 종결.
            lost_rule = next((r for r in agent_rules if r.get('check') == 'agent_lost'), None)
            lost_hosts = set()
            if lost_rule is not None:
                for ag in agents:
                    host = ag.get('name') or str(ag.get('id'))
                    mo = f"{host}/agent"
                    akey = f"{lost_rule.get('code')}@{mo}"
                    lost = host not in observed_hosts
                    if lost:
                        lost_hosts.add(host)
                        active.add(akey)   # 아래 미평가 close 에서 제외
                    _transition(lost_rule, mo, 'agent', lost,
                                _fmt(lost_rule.get('msg_open'), mo=mo, host=host),
                                _fmt(lost_rule.get('msg_close'), mo=mo, host=host))
                for akey, ent in list(_alert_open.items()):
                    if alarm_sweeper.partition_of(
                            alarm_sweeper._entry_detected_by(ent), akey) != 'agent':
                        continue
                    mo_part = akey.split('@', 1)[1] if '@' in akey else ''
                    if mo_part.split('/')[0] not in lost_hosts or akey in active:
                        continue
                    r = agent_rule_by_code.get(akey.split('@', 1)[0])
                    if r:
                        _transition(r, mo_part, 'agent', False, '',
                                    f"{mo_part} 판정 불가 종결 — agent 관측 두절 "
                                    f"(노드 두절 알람 {lost_rule.get('code')} 참조)")
            # agent 파티션 알람 중 이번에 평가 안 된 것 = 관측 불가 → close.
            # 자기 파티션(detected_by=agent)만 정리한다 (파이프라인 §4.3) — 서비스/drift
            # 계열(oam-svc/oam)은 mo 루트가 같은 서버명/그룹명 어휘라 mo 로는 구분 불가.
            for akey, ent in list(_alert_open.items()):
                if alarm_sweeper.partition_of(alarm_sweeper._entry_detected_by(ent), akey) \
                        != 'agent':
                    continue
                mo_part = akey.split('@', 1)[1] if '@' in akey else ''
                if akey in active:
                    continue
                r = agent_rule_by_code.get(akey.split('@', 1)[0])
                if r:
                    _transition(r, mo_part, 'agent', False, '',
                                _fmt(r.get('msg_close'), mo=mo_part))

        def _sweep_alerts():
            try:
                rules = service_registry.alert_rules(config)   # 표준 정규화됨
                svc_rules   = [r for r in rules if r.get('scope') != 'agent']
                agent_rules = [r for r in rules if r.get('scope') == 'agent']
                # 서비스 계열 — 분리 배포(role=base)에서는 oam-svc sweeper 가 발화
                # (detected_by='oam-svc'). 여기서는 role=all 대행만.
                if role == 'all' and svc_rules:
                    alarm_sweeper.sweep_service_rules(
                        config, _alert_open, _service_log,
                        detected_by='oam', rtp_threshold=ALERT_RTP_THRESHOLD, log=logger)
                if agent_rules:
                    _sweep_agent_alerts(agent_rules)
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
        # 기동 시 open drift 알람을 alert_log 리플레이로 복원 — 빈 dict 로 시작하면
        # 재시작 이전에 열린 알람의 close 를 영원히 발행하지 못한다 (좀비).
        _drift_open: dict = {}
        if _service_log:
            try:
                from services import alert_log as _alert_log
                from services import drift_sweeper as _ds
                # 창을 활성 알람 뷰(90일)와 맞춘다 — 기본 30일이면 30~90일 구간의
                # 열린 알람을 서버가 잊어 중복 open 을 낸다. (스윕 중 재도출은
                # drift_sweeper._reseed_if_empty 가 담당 — 여기 실패해도 자가복구)
                # 현행 akey(A-PRC-003@<그룹>/config/<coll>)와 구 포맷(cims/ha/…·
                # CIMS-CFG-001·config_drift::…, 이행 종결 대상) 둘 다 복원한다.
                _meta = _alert_log.compute_open_state(_service_log, days=90, with_meta=True)
                _drift_open = {k: {'alarm_id': _meta[k]['alarm_id'],
                                   'severity': _meta[k].get('perceived_severity'),
                                   'detected_by': _meta[k].get('detected_by')}
                               for k in _ds.drift_open_keys(_meta)}
                if _drift_open:
                    logger.log_info(f"[drift-sweep] restored {len(_drift_open)} open drift alarm(s)")
            except Exception as e:
                logger.log_error(f"[drift-sweep] open-state restore failed: {e}")

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

        # ── HA 자동 동기화 스위퍼 (R4) ──────────────────────────────────
        #  AS 그룹 × 스위치 ON 패키지의 STANDBY 를 실측 ACTIVE(heartbeat VIP 관측)
        #  기준으로 자동 교정. 판정 불가·버전 불일치는 reconcile 내부에서 skip/보류.
        #  컬렉션 정합은 agent proxy GET 비용이 있어 매 N 라운드마다만 포함.
        AUTO_SYNC_SWEEP_INTERVAL = int(config.get('AutoSyncSweepSec', 60))
        AUTO_SYNC_COLL_EVERY     = max(1, int(config.get('AutoSyncCollectionEvery', 5)))
        _auto_sync_round = {'n': 0}
        _observed_active: dict = {}   # gid → 마지막 확정 active_agent_id (절체 감지)

        def _sweep_auto_sync():
            try:
                from services import ha_lookup
                from handlers.agents import reconcile_group_package
                _auto_sync_round['n'] += 1
                include_colls = (_auto_sync_round['n'] % AUTO_SYNC_COLL_EVERY) == 0
                for g in ha_lookup.ha_groups_all(config):
                    if g.get('mode') != 'active_standby':
                        continue
                    gid = g.get('id')
                    # 절체 감지 — 확정 판정 간 변화만 기록 (판정 불가(None)는 미갱신)
                    obs = ha_lookup.vip_observation(config, g)
                    active = obs['active_agent_id']
                    prev = _observed_active.get(gid)
                    if active is not None:
                        if prev is not None and active != prev:
                            logger.log_info(
                                f"[auto-sync] HA 절체 감지 — group#{gid}"
                                f"({g.get('name')}) agent#{prev} → agent#{active}")
                        _observed_active[gid] = active
                    for pkg_name in sorted(ha_lookup.packages_in_group(config, g)):
                        if not ha_lookup.auto_sync_enabled(g, pkg_name):
                            continue
                        r = reconcile_group_package(
                            config, g, pkg_name,
                            include_collections=include_colls, actor='sweeper')
                        if r['status'] == 'synced':
                            logger.log_info(
                                f"[auto-sync] group#{gid} pkg={pkg_name} 교정 — "
                                f"keys={len(r['synced_keys']) + len(r['removed_keys'])} "
                                f"colls={len(r['collections'])} "
                                f"active=agent#{r['active_agent_id']} sync#{r['sync_id']}")
                        elif r['deferred']:
                            logger.log_info(
                                f"[auto-sync] group#{gid} pkg={pkg_name} 보류 — "
                                f"버전 불일치 {r['deferred']}")
            except Exception as e:
                logger.log_error(f"[auto-sync] error: {e}")

        # ── Metric JSONL retention purge sweeper ────────────────────────
        # heartbeat 2s × 다수 host → metrics/<id>/YYYY/MM/DD.jsonl 무한 누적.
        # retain_days 보다 오래된 일별 파일 삭제 (B 트랙 Phase 1 의 24h purge 설계 구현).
        METRIC_RETAIN_DAYS    = int(config.get('MetricRetentionDays', 3))
        # 완료 job 보존 — 무한 누적이 store(특히 NFS) 상시 비용이 된다. 미완 job 은 대상 아님.
        JOB_RETAIN_DAYS       = int(config.get('JobRetentionDays', 2))
        JOB_RETAIN_COUNT      = int(config.get('JobRetentionCount', 200))
        JOB_PURGE_INTERVAL    = 600
        METRIC_PURGE_INTERVAL = int(config.get('MetricPurgeSweepSec', 3600))

        _ptt_index_last_n = [-1]

        def _sweep_ptt_index():
            # 오늘 인덱스만 다시 만든다 — 지난 날짜 파일은 불변이라 손대지 않는다.
            #   30초마다 도는 스위퍼라 세션 수가 바뀔 때만 남긴다(로그 오염 방지).
            try:
                if not ptt_index.enabled():
                    return
                n = ptt_index.sweep()
                if n != _ptt_index_last_n[0]:
                    logger.log_info(f"[ptt-index] today: {n} session(s)")
                    _ptt_index_last_n[0] = n
            except Exception as e:
                logger.log_error(f"[ptt-index] error: {e}")

        def _sweep_job_purge():
            try:
                from handlers.agents import purge_old_jobs
                n = purge_old_jobs(config, JOB_RETAIN_DAYS, JOB_RETAIN_COUNT)
                if n > 0:
                    logger.log_info(f"[job-purge] 완료 job {n}건 정리 "
                                    f"(보존 {JOB_RETAIN_DAYS}d / 최대 {JOB_RETAIN_COUNT}건)")
            except Exception as e:
                logger.log_error(f"[job-purge] error: {e}")

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

        # ── 알람/이벤트 스트림 보존 (파이프라인 §6.2) — 일 1회 파일 날짜 기준 purge.
        # 알람 보존은 open-state replay 윈도(90일)보다 작아지면 시드가 깨진다 — 90일
        # 하한 클램프 (0 = 무제한).
        _sl_cfg = config.get('ServiceLogging', {}) or {}
        ALERT_RETAIN_DAYS = int(_sl_cfg.get('AlertRetainDays', 180))
        EVENT_RETAIN_DAYS = int(_sl_cfg.get('EventRetainDays', 365))
        if 0 < ALERT_RETAIN_DAYS < 90:
            logger.log_warning(f"[retention] AlertRetainDays={ALERT_RETAIN_DAYS} < 90 — "
                               f"open-state replay 윈도 보호를 위해 90 으로 클램프")
            ALERT_RETAIN_DAYS = 90
        RETENTION_SWEEP_INTERVAL = 86400

        def _sweep_retention():
            try:
                from services import daily_jsonl
                if not _service_log:
                    return
                na = daily_jsonl.purge_old(_service_log, 'alerts', ALERT_RETAIN_DAYS)
                ne = daily_jsonl.purge_old(_service_log, 'events', EVENT_RETAIN_DAYS)
                if na or ne:
                    logger.log_info(f"[retention] purged alerts={na} (>{ALERT_RETAIN_DAYS}d) "
                                    f"events={ne} (>{EVENT_RETAIN_DAYS}d)")
            except Exception as e:
                logger.log_error(f"[retention] error: {e}")

        logger.log_info(f"[agent-sweep] stale threshold={STALE_SEC}s, interval={SWEEP_INTERVAL}s")
        logger.log_info(f"[cert-sweep] rotate threshold={_AGENT_CERT_ROTATE_THRESHOLD_DAYS}d, "
                        f"interval={CERT_SWEEP_INTERVAL}s")
        logger.log_info(f"[alert-sweep] interval={ALERT_SWEEP_INTERVAL}s, "
                        f"rtp_threshold={ALERT_RTP_THRESHOLD}%, "
                        f"dir={_service_log or '(disabled — no ServiceLogDir)'}")
        logger.log_info(f"[sync-txn-sweep] interval={SYNC_TXN_SWEEP_INTERVAL}s")
        logger.log_info(f"[drift-sweep] interval={DRIFT_SWEEP_INTERVAL}s "
                        f"auto_resync={DRIFT_AUTO_RESYNC}")
        logger.log_info(f"[auto-sync] interval={AUTO_SYNC_SWEEP_INTERVAL}s, "
                        f"collections_every={AUTO_SYNC_COLL_EVERY} round(s)")
        logger.log_info(f"[metric-purge] retain={METRIC_RETAIN_DAYS}d, interval={METRIC_PURGE_INTERVAL}s")
        logger.log_info(f"[ptt-index] enabled={_ptt_index_enabled}, interval={PTT_INDEX_INTERVAL}s")
        # 계획 절체 operation 구동 — 짧은 주기(VIP 이동 관측이 필요하므로).
        HA_OP_SWEEP_INTERVAL = int(config.get('HaOpSweepSec', 2))
        def _sweep_ha_ops():
            try:
                from handlers.ha_groups import sweep_ha_operations
                sweep_ha_operations(config)
            except Exception as e:
                logger.log_warning(f"[ha-op] sweep 실패: {e}")

        _last_sweep = 0
        _last_cert_sweep = 0
        _last_alert_sweep = 0
        _last_sync_txn_sweep = 0
        _last_drift_sweep = 0
        _last_auto_sync_sweep = 0
        _last_metric_purge = 0
        _last_ptt_index = 0
        _last_job_purge = 0
        _last_retention = 0
        _last_ha_op_sweep = 0
        _last_ro_log = 0
        # 리스 재획득 주기 — 절체 직후 구 Active 가 물러나는 데 수 초 걸리므로 짧게.
        LEASE_RETRY_INTERVAL = 5
        _last_lease_retry = 0
        while True:
            time.sleep(1)
            _now = time.time()
            # ── 리스 게이트 (oam_ha.md §4.5) ────────────────────────────────
            # 8개 스위퍼는 전부 store 에 쓴다 — API 만 막고 스위퍼를 두면 background
            # writer 가 그대로 남아 이중 write 가 된다. 소유권이 없으면 전부 건너뛴다.
            # verify() 는 epoch fence 도 겸한다(다른 writer 가 epoch 를 올렸으면 강등).
            if not _lease.verify():
                # **재획득 시도** — 리스는 기동 시 1회만 잡았고 재시도가 없었다. 그래서 절체
                # 직후 신 Active 가 (구 Active 가 아직 놓지 않아) 획득에 실패하면, 구 Active 가
                # 물러난 뒤에도 **영원히 read-only** 로 남았다(실측: VIP 는 넘어갔는데 콘솔이
                # locked_by_other_writer). 주기적으로 다시 잡아 절체를 완결시킨다.
                if _now - _last_lease_retry >= LEASE_RETRY_INTERVAL:
                    _last_lease_retry = _now
                    try:
                        _st = _lease.acquire(_fs0.runtime_root(config))
                        if _st.get('active'):
                            logger.log_info(f"[lease] 소유권 재획득 — epoch={_st['epoch']} "
                                            f"(절체 완결: 이제 변경 가능)")
                    except Exception as _e:
                        logger.log_warning(f"[lease] 재획득 예외: {_e}")
                if not _lease.is_active():
                    if _now - _last_ro_log >= 60:
                        _last_ro_log = _now
                        logger.log_warning(f"[lease] read-only — 스위퍼 중단 "
                                           f"({_lease.state().get('reason')})")
                    continue
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
            if _now - _last_auto_sync_sweep >= AUTO_SYNC_SWEEP_INTERVAL:
                _sweep_auto_sync()
                _last_auto_sync_sweep = _now
            if _now - _last_ha_op_sweep >= HA_OP_SWEEP_INTERVAL:
                _sweep_ha_ops()
                _last_ha_op_sweep = _now
            if _now - _last_job_purge >= JOB_PURGE_INTERVAL:
                _sweep_job_purge()
                _last_job_purge = _now
            if _now - _last_metric_purge >= METRIC_PURGE_INTERVAL:
                _sweep_metric_purge()
                _last_metric_purge = _now
            if _now - _last_retention >= RETENTION_SWEEP_INTERVAL:
                _sweep_retention()
                _last_retention = _now
            if _now - _last_ptt_index >= PTT_INDEX_INTERVAL:
                _sweep_ptt_index()
                _last_ptt_index = _now

    except Exception as e:
        tb_str = traceback.format_exc()
        logger.log_error(f'==================== stop : {e} : {tb_str} ====================')
        if admin_server:  admin_server.stop(5)
        # D3: preflight 모드에서 import/config 예외는 "신 패키지 기동 불가" 신호 → 비0 종료.
        if args_dict.get('preflight'):
            print(f'OAM_PREFLIGHT_FAIL: {e}', flush=True)
            sys.exit(2)
