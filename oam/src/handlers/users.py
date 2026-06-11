"""
CIMS Users API (v3, 2026-04-22) — 로그인한 본인 리소스 조회

Routes:
  GET /api/v1/users/me              - 본인 프로파일 (role, org_id 등; Console admin 용)
  GET /api/v1/users/me/subscriptions - 본인 VoIP/PTT 가입자 배열 (Phone UE 가 SIP REGISTER 전에 호출)

분리 원칙 (v3):
  - /auth/login 은 인증 전용 (토큰 + 최소 user 만 반환)
  - 프로파일/가입자 정보는 별도 리소스 엔드포인트로 분리
  - Phone UE 는 /users/me + /users/me/subscriptions 만 호출하면 REGISTER 가능
  - Console admin 은 /users/me 만 필요 (subscription 은 관리자 본인에게 없어도 됨)
"""

from pathlib import PurePath
import json
import os

from httpsrv.handler import HandlerArgs, HandlerResult
import pymysql

from . import auth as _auth


_USERS_BASE = '/api/v1/users'


def _access_service_domain_map(config):
    """access_services.jsonl 을 읽어 service_ref(name) → domain 매핑 반환.

    탐색 경로:
      1) config['AccessServicesFile'] (절대경로 지정 시)
      2) <csc config dir>/../../config/access_services.jsonl (install_path/config)
      3) build/dist/config/access_services.jsonl (개발 환경)
    """
    candidates = []
    if config.get('AccessServicesFile'):
        candidates.append(config['AccessServicesFile'])
    # config 파일이 있는 디렉토리 → ../../config/access_services.jsonl
    cfg_path = config.get('__config_path__')
    if cfg_path:
        base = os.path.dirname(os.path.abspath(cfg_path))
        candidates.append(os.path.normpath(os.path.join(base, '..', '..', 'config', 'access_services.jsonl')))
    # 실행 경로 기반 fallback — handlers/ 에서 3 단계 up = dist_root
    here = os.path.dirname(os.path.abspath(__file__))
    candidates.append(os.path.normpath(os.path.join(here, '..', '..', '..', 'config', 'access_services.jsonl')))
    # 소스 트리 개발환경 — build/dist/config 시도
    candidates.append(os.path.normpath(os.path.join(here, '..', '..', '..', 'build', 'dist', 'config', 'access_services.jsonl')))

    for path in candidates:
        try:
            if not os.path.isfile(path):
                continue
            out = {}
            with open(path, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        rec = json.loads(line)
                    except Exception:
                        continue
                    name = rec.get('name')
                    domain = rec.get('domain')
                    if name and domain:
                        out[name] = domain
            if out:
                return out
        except Exception:
            continue
    return {}


def _dt(val):
    return val.isoformat() if val else None


def _parts(full_path: str):
    try:
        rel = PurePath(full_path).relative_to(PurePath(_USERS_BASE))
        return [p for p in rel.parts if p and p != '.']
    except Exception:
        return []


async def handle_users(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """
    경로 분기:
      GET /users/me
      GET /users/me/subscriptions
    """
    config = kwargs.get('config', {})
    parts  = _parts(handler_args.full_path)
    method = handler_args.method.upper()

    # 현재는 본인 조회만 지원 (/me). 향후 /users/{id} 관리자용은 admin.py 가 담당.
    if len(parts) == 1 and parts[0] == 'me' and method == 'GET':
        return await _get_me(handler_args, config)
    if len(parts) == 2 and parts[0] == 'me' and parts[1] == 'subscriptions' and method == 'GET':
        return await _get_me_subscriptions(handler_args, config)

    return HandlerResult(status=404, body={'error': 'Not Found'})


async def _get_me(handler_args, config):
    """본인 프로파일 — role, org_id 등. subscription 없음."""
    payload, err = _auth.require_auth(handler_args)
    if err:
        return err

    # 패키지 내장 계정(admin/developer) — DB 미구축 부트스트랩에서도 동작해야
    # 하므로 토큰 클레임만으로 프로파일 합성 (DB 조회 없음).
    if payload.get('builtin'):
        return HandlerResult(status=200, body={
            'id': int(payload.get('sub') or -1000),
            'name': payload.get('name') or payload.get('login_id'),
            'login_id': payload.get('login_id'),
            'role': payload.get('role'),
            'org_id': None,
            'builtin': True,
            'create_time': None,
            'update_time': None,
        })

    try:
        with _auth._get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT id, name, login_id, role, org_id, create_time, update_time "
                    "FROM users WHERE id=%s",
                    (int(payload['sub']),)
                )
                user = cur.fetchone()
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})

    if user is None:
        return HandlerResult(status=404, body={'error': '사용자를 찾을 수 없습니다'})

    user['create_time'] = _dt(user.get('create_time'))
    user['update_time'] = _dt(user.get('update_time'))
    return HandlerResult(status=200, body=user)


async def _get_me_subscriptions(handler_args, config):
    """본인 VoIP/PTT 가입자 배열 — Phone UE 가 SIP REGISTER 전에 호출.

    응답 각 subscription 에는 다음이 포함됨:
      id           — MSISDN (E.164)
      service_ref  — access_services.name
      imsi         — IMSI (user part)
      passwd       — SIP Digest password
      domain       — service_ref 가 가리키는 access_services.domain
      auth_id      — imsi@domain (Digest username) — Phone 은 이 값을 그대로 사용
    """
    payload, err = _auth.require_auth(handler_args)
    if err:
        return err

    # 내장 계정은 가입자(전화) 정보가 없음 — DB 없이 빈 배열
    if payload.get('builtin'):
        return HandlerResult(status=200, body={'call_subscriptions': [], 'ptt_subscriptions': []})
    uid = int(payload['sub'])

    domain_map = _access_service_domain_map(config)

    def _fill(s):
        s['dnd'] = bool(s['dnd'])
        s['register_time'] = _dt(s['register_time'])
        s['logout_time']   = _dt(s['logout_time'])
        domain = domain_map.get(s.get('service_ref') or '', '')
        s['domain']  = domain
        s['auth_id'] = f"{s.get('imsi','')}@{domain}" if (s.get('imsi') and domain) else ''
        return s

    try:
        with _auth._get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT id, service_ref, imsi, passwd, dnd, forward_id, "
                    "       register_time, logout_time "
                    "FROM volte_subscriptions WHERE user_id=%s ORDER BY id",
                    (uid,)
                )
                call_subs = [_fill(s) for s in cur.fetchall()]

                cur.execute(
                    "SELECT id, service_ref, imsi, passwd, dnd, forward_id, "
                    "       register_time, logout_time "
                    "FROM ptt_subscriptions WHERE user_id=%s ORDER BY id",
                    (uid,)
                )
                ptt_subs = [_fill(s) for s in cur.fetchall()]
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})

    return HandlerResult(status=200, body={
        'call_subscriptions': call_subs,
        'ptt_subscriptions':  ptt_subs,
    })


# ── 핸들러 목록 ────────────────────────────────────────────────
CIMS_USERS_HANDLER_LIST = [
    (_USERS_BASE, handle_users, {}),
]
