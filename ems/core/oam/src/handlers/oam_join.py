"""관리평면 노드 합류(join) — 두 번째 OAM 노드에 **그룹 공통 신원**을 전달한다.

Routes:
  POST /api/v1/ha/join-token   1회용 합류 토큰 발급 (admin)
  GET  /api/v1/ha/join-token   미사용 토큰 상태 조회 (admin)
  POST /api/v1/ha/join         토큰으로 신원 수령 (인증=토큰. 새 노드는 admin 계정이 없다)

왜 필요한가 (oam_ha.md §9): 콘솔 배포 경로로 두 번째 노드에 `oam` 을 설치하는 것은
성립하지 않는다 — 시크릿·CA·런타임 경로(`_infra`)는 부트스트랩이 1번 노드의 deployment
overlay 에만 넣고 템플릿 default 가 비어 있어 **반드시 잘못된 설정으로 뜬다**. 두 노드가
같은 신원을 갖는 것은 이중화의 전제이므로(다르면 절체 후 전 세션 무효 + 모듈 401),
합류 절차가 신원을 명시적으로 전달한다.

보안:
  - 토큰은 **1회용 + TTL**(기본 15분). 저장은 sha256 해시만 — 평문은 발급 응답에 1회.
  - 응답에 **개인키(그룹 CA·mTLS CA)** 가 들어간다. HTTPS 전용이고, 발급·사용을 감사
    로그로 남긴다(누가 발급했고 어느 peer 가 수령했는지).
  - 개인키를 공유 볼륨에 두지 않는 원칙(§5)의 배포 수단이 이 API 다 — 복제가 아니라
    **1회 복사**.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import secrets
from datetime import datetime, timedelta

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store
from util.log_util import Logger
from handlers import auth

logger = Logger()

_BASE = '/api/v1/ha'
_DOMAIN = 'join_tokens'
_DEFAULT_TTL_SEC = 900


def _dir(config):
    return file_store.domain_dir(config, _DOMAIN)


def _hash(tok: str) -> str:
    return hashlib.sha256(tok.encode()).hexdigest()


def _parse_body(handler_args: HandlerArgs) -> dict:
    b = handler_args.body
    if isinstance(b, dict):
        return b
    try:
        return json.loads(b) if b else {}
    except Exception:
        return {}


def _read_pem(path: str) -> "str | None":
    try:
        with open(path) as f:
            return f.read()
    except Exception:
        return None


def _secrets_dir(config) -> str:
    # 노드 로컬 — 개인키는 공유 store 에 두지 않는다(oam_ha.md §5).
    from services import paths as _paths
    return _paths.secrets_dir(config, create=False)


def _identity_bundle(config: dict) -> dict:
    """합류 노드에 넘기는 그룹 공통 신원 — 노드 로컬 0600 자산의 사본.

    서버 인증서는 넘기지 않는다: 합류 노드는 **같은 CA 로 자기 인증서를 발급**한다
    (개인키는 노드를 떠나지 않는다). 브라우저는 CA 하나만 신뢰하면 절체 후에도 경고가 없다."""
    sd = _secrets_dir(config)
    ca_dir = os.path.join(sd, 'ca')
    mtls_dir = os.path.join(sd, 'agent_mtls')
    srv = config.get('Server') or {}
    ca_auth = config.get('CimsAuth') or {}
    out = {
        'auth': {
            'JwtSecret': ca_auth.get('JwtSecret') or '',
            'BuiltinAccounts': ca_auth.get('BuiltinAccounts') or [],
        },
        'server': {
            'Port': srv.get('Port') or 4419,
            'Role': srv.get('Role') or 'base',
            'AgentOamUrl': srv.get('AgentOamUrl') or '',
            'CertSans': srv.get('CertSans') or [],
        },
        'mgmt': {'Cidr': (config.get('Mgmt') or {}).get('Cidr') or ''},
        'runtime': {
            'CimsRuntimeDir': config.get('CimsRuntimeDir') or '',
            'CimsRuntimeMount': config.get('CimsRuntimeMount') or '',
        },
        'logging': {'Dir': (config.get('ServiceLogging') or {}).get('Dir') or ''},
        'ca': {},
        'agent_mtls': {},
    }
    for k, fn in (('crt', 'ca.crt'), ('key', 'ca.key')):
        pem = _read_pem(os.path.join(ca_dir, fn))
        if pem:
            out['ca'][k] = pem
    for k, fn in (('ca_crt', 'ca.crt'), ('ca_key', 'ca.key'),
                  ('client_crt', 'csc_client.crt'), ('client_key', 'csc_client.key')):
        pem = _read_pem(os.path.join(mtls_dir, fn))
        if pem:
            out['agent_mtls'][k] = pem
    return out


async def _issue_token(handler_args: HandlerArgs, config: dict) -> HandlerResult:
    payload, err = auth.require_admin(handler_args)
    if err:
        return err
    body = _parse_body(handler_args)
    try:
        ttl = int(body.get('ttl_sec') or _DEFAULT_TTL_SEC)
    except (TypeError, ValueError):
        ttl = _DEFAULT_TTL_SEC
    ttl = max(60, min(ttl, 86400))
    tok = base64.urlsafe_b64encode(secrets.token_bytes(32)).decode().rstrip('=')
    exp = datetime.now() + timedelta(seconds=ttl)
    rec = {
        'id': _hash(tok)[:16],
        'token_sha256': _hash(tok),
        'expires_at': exp.isoformat(timespec='seconds'),
        'issued_by': (payload or {}).get('login_id') or '',
        'used_at': None, 'used_by_peer': None,
    }
    file_store.save(_dir(config), rec['id'], rec)
    logger.log_info(f"[ha-join] 합류 토큰 발급 — by={rec['issued_by']} ttl={ttl}s id={rec['id']}")
    return HandlerResult(status=201, body={
        'token': tok, 'expires_at': rec['expires_at'], 'ttl_sec': ttl,
        'hint': '이 토큰은 1회용이며 응답에만 표시된다. 합류 노드에서 '
                'install.sh --join --peer-url <this> --join-token <token> 로 사용.',
    }, media_type='application/json')


async def _list_tokens(handler_args: HandlerArgs, config: dict) -> HandlerResult:
    payload, err = auth.require_admin(handler_args)
    if err:
        return err
    now = datetime.now()
    items = []
    for r in file_store.load_all(_dir(config)):
        try:
            expired = datetime.fromisoformat(str(r.get('expires_at'))) < now
        except Exception:
            expired = True
        items.append({'id': r.get('id'), 'expires_at': r.get('expires_at'),
                      'issued_by': r.get('issued_by'), 'used_at': r.get('used_at'),
                      'used_by_peer': r.get('used_by_peer'), 'expired': expired})
    return HandlerResult(status=200, body={'items': items}, media_type='application/json')


async def _join(handler_args: HandlerArgs, config: dict) -> HandlerResult:
    """토큰 검증 → 신원 반환 + 토큰 소모(1회용). admin 로그인 불요(새 노드엔 계정이 없다)."""
    body = _parse_body(handler_args)
    tok = str(body.get('token') or '').strip()
    peer = f"{getattr(handler_args, 'client_ip', '')}"
    if not tok:
        return HandlerResult(status=400, body={'error': 'token_required'},
                             media_type='application/json')
    tid = _hash(tok)[:16]
    rec = file_store.load(_dir(config), tid)
    if not rec or rec.get('token_sha256') != _hash(tok):
        logger.log_warning(f"[ha-join] 잘못된 합류 토큰 시도 — peer={peer}")
        return HandlerResult(status=401, body={'error': 'invalid_token'},
                             media_type='application/json')
    if rec.get('used_at'):
        logger.log_warning(f"[ha-join] 이미 사용된 토큰 재시도 — peer={peer} id={tid}")
        return HandlerResult(status=409, body={'error': 'token_already_used',
                                              'used_at': rec.get('used_at'),
                                              'used_by_peer': rec.get('used_by_peer')},
                             media_type='application/json')
    try:
        if datetime.fromisoformat(str(rec.get('expires_at'))) < datetime.now():
            logger.log_warning(f"[ha-join] 만료 토큰 시도 — peer={peer} id={tid}")
            return HandlerResult(status=401, body={'error': 'token_expired'},
                                 media_type='application/json')
    except Exception:
        return HandlerResult(status=401, body={'error': 'token_expired'},
                             media_type='application/json')

    bundle = _identity_bundle(config)
    if not bundle['auth'].get('JwtSecret'):
        # 신원이 없으면 합류가 무의미하다(두 노드가 다른 신원을 갖게 된다) → 명시 실패.
        return HandlerResult(status=409, body={
            'error': 'identity_not_ready',
            'detail': 'JwtSecret 이 비어 있다 — 이 노드의 신원이 아직 확정되지 않았다.',
        }, media_type='application/json')

    # agent enrollment — 합류 노드는 admin 계정이 없어 스스로 agent 를 등록할 수 없다.
    # 신원 전달과 같은 트랜잭션에서 agent 레코드 + 1회용 enrollment token 을 함께 준다
    # (없으면 운영자가 콘솔에서 수동으로 서버를 추가해야 해 절차가 두 갈래가 된다).
    node_name = str(body.get('node_name') or '').strip()
    if node_name:
        try:
            from handlers import agents as _ag
            existing = _ag._agent_load(config, None, node_name)
            if existing:
                bundle['agent'] = {'id': existing.get('id'), 'name': node_name,
                                   'enrollment_token': None,
                                   'note': '같은 이름의 agent 가 이미 있음 — 콘솔에서 토큰 재발급'}
            else:
                import secrets as _sec
                from datetime import datetime as _dt, timedelta as _td
                _tok = _sec.token_hex(24)
                _ttl = _ag._enrollment_token_ttl_sec(config)
                _now = _dt.now()
                _row = {
                    'id': file_store.next_id(_ag._agent_dir(config)),
                    'name': node_name,
                    'enrollment_token': _tok,
                    'enrollment_token_issued_at': _now.isoformat(timespec='seconds'),
                    'enrollment_token_expires_at': (_now + _td(seconds=_ttl)).isoformat(timespec='seconds'),
                    'agent_token': _sec.token_hex(32),
                    'status': 'pending',
                    'note': 'ha-join',
                }
                _ag._agent_save(config, _row)
                bundle['agent'] = {'id': _row['id'], 'name': node_name,
                                   'enrollment_token': _tok,
                                   'enrollment_token_ttl_sec': _ttl}
                logger.log_info(f"[ha-join] agent 등록 — name={node_name} id={_row['id']}")
        except Exception as e:
            logger.log_warning(f"[ha-join] agent 등록 실패({e}) — 콘솔에서 수동 추가 필요")
            bundle['agent'] = {'error': str(e)}

    rec['used_at'] = datetime.now().isoformat(timespec='seconds')
    rec['used_by_peer'] = peer
    file_store.save(_dir(config), tid, rec)
    logger.log_info(f"[ha-join] 신원 전달 완료 — peer={peer} id={tid} "
                    f"ca={'yes' if bundle['ca'].get('key') else 'no'} "
                    f"mtls={'yes' if bundle['agent_mtls'].get('ca_key') else 'no'}")
    return HandlerResult(status=200, body={'identity': bundle}, media_type='application/json')


async def handle_ha_join(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    path = (handler_args.full_path or '').split('?')[0]
    tail = path[len(_BASE):].strip('/')
    method = handler_args.method.upper()

    if tail == 'join-token':
        if method == 'POST':
            return await _issue_token(handler_args, config)
        if method == 'GET':
            return await _list_tokens(handler_args, config)
        return HandlerResult(status=405, body={'error': 'method_not_allowed'},
                             media_type='application/json')
    if tail == 'join' and method == 'POST':
        return await _join(handler_args, config)
    return HandlerResult(status=404, body={'error': 'not_found'}, media_type='application/json')


CIMS_OAM_JOIN_HANDLER_LIST = [
    (_BASE, handle_ha_join, {}),
]
