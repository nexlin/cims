"""
CIMS 조직 관리 REST API
GET/POST/PUT/DELETE /api/v1/organizations
POST /api/v1/organizations/import (Excel)
GET  /api/v1/organizations/import/template
DELETE /api/v1/organizations/batch
"""

import json
import io
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult
from services import admin_auth

_ORG_BASE = '/api/v1/organizations'


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


async def handle_organizations(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts = _path_parts(handler_args.full_path, _ORG_BASE)
    method = handler_args.method.upper()

    # RBAC — 조회 monitor+, 변경 manager+ (계획서 §3 가입자/조직).
    payload, err = admin_auth.require_role(handler_args, 'monitor' if method == 'GET' else 'manager')
    if err:
        return err

    try:
        if len(parts) == 0:
            if method == 'GET':
                return await _list_orgs(config)
            elif method == 'POST':
                return await _create_org(handler_args.body, config)
            return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

        if parts[0] == 'batch' and method == 'DELETE':
            return await _batch_delete_orgs(handler_args.body, config)


        org_id = parts[0]
        if len(parts) == 1:
            if method == 'GET':
                return await _get_org(org_id, config)
            elif method == 'PUT':
                return await _update_org(org_id, handler_args.body, config)
            elif method == 'DELETE':
                return await _delete_org(org_id, config)

        if len(parts) == 2 and parts[1] == 'users' and method == 'GET':
            return await _list_org_users(org_id, config)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except pymysql.Error as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _list_orgs(config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, code, code_path, name, parent_id, sort_order FROM organizations ORDER BY code_path, sort_order, name"
            )
            rows = cur.fetchall()
    return HandlerResult(status=200, body={'organizations': rows})


async def _get_org(org_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM organizations WHERE id=%s", (org_id,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={'error': 'Not found'})
    return HandlerResult(status=200, body=row)


def _build_code_path(cur, parent_id, code):
    """parent_id로부터 code_path 계산"""
    if not parent_id:
        return code
    cur.execute("SELECT code_path FROM organizations WHERE id=%s", (parent_id,))
    row = cur.fetchone()
    parent_path = row['code_path'] if row and row.get('code_path') else ''
    return (parent_path + '/' + code) if parent_path else code


def _rebuild_children_paths(cur, parent_id, parent_path):
    """부모 변경 시 모든 하위 조직의 code_path 재계산"""
    cur.execute("SELECT id, code FROM organizations WHERE parent_id=%s", (parent_id,))
    for child in cur.fetchall():
        child_path = parent_path + '/' + child['code']
        cur.execute("UPDATE organizations SET code_path=%s WHERE id=%s", (child_path, child['id']))
        _rebuild_children_paths(cur, child['id'], child_path)


async def _create_org(body, config):
    if not body:
        return HandlerResult(status=400, body={'error': 'Body required'})
    code = body.get('code', '').strip()
    name = body.get('name', '').strip()
    parent_id = body.get('parent_id')
    sort_order = body.get('sort_order', 0)
    if not code or not name:
        return HandlerResult(status=400, body={'error': 'code, name 필수'})

    with _get_db(config) as conn:
        with conn.cursor() as cur:
            code_path = _build_code_path(cur, parent_id, code)
            cur.execute(
                "INSERT INTO organizations (code, code_path, name, parent_id, sort_order) "
                "VALUES (%s,%s,%s,%s,%s)",
                (code, code_path, name, parent_id, sort_order)
            )
            new_id = cur.lastrowid
    return HandlerResult(status=201, body={'id': new_id, 'code': code, 'code_path': code_path})


async def _update_org(org_id, body, config):
    if not body:
        return HandlerResult(status=400, body={'error': 'Body required'})
    sets, params = [], []
    for field in ('code', 'name', 'parent_id', 'sort_order'):
        if field in body:
            sets.append(f"{field}=%s")
            params.append(body[field])
    if not sets:
        return HandlerResult(status=400, body={'error': '변경할 필드가 없습니다'})
    params.append(org_id)
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            cur.execute(f"UPDATE organizations SET {','.join(sets)} WHERE id=%s", params)
            # code 또는 parent_id 변경 시 code_path 재계산
            if 'parent_id' in body or 'code' in body:
                cur.execute("SELECT code, parent_id FROM organizations WHERE id=%s", (org_id,))
                row = cur.fetchone()
                if row:
                    new_path = _build_code_path(cur, row['parent_id'], row['code'])
                    cur.execute("UPDATE organizations SET code_path=%s WHERE id=%s", (new_path, org_id))
                    _rebuild_children_paths(cur, int(org_id), new_path)
    return HandlerResult(status=200, body={'id': org_id})


async def _delete_org(org_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # 하위 조직의 parent를 NULL로 변경 (상위로 이동)
            cur.execute("UPDATE organizations SET parent_id=NULL WHERE parent_id=%s", (org_id,))
            cur.execute("DELETE FROM organizations WHERE id=%s", (org_id,))
            if cur.rowcount == 0:
                return HandlerResult(status=404, body={'error': 'Not found'})
    return HandlerResult(status=200, body={'id': org_id})


async def _batch_delete_orgs(body, config):
    ids = body.get('ids', []) if body else []
    if not ids:
        return HandlerResult(status=400, body={'error': 'ids 필요'})
    deleted = 0
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            for oid in ids:
                cur.execute("UPDATE organizations SET parent_id=NULL WHERE parent_id=%s", (oid,))
                cur.execute("DELETE FROM organizations WHERE id=%s", (oid,))
                if cur.rowcount > 0:
                    deleted += 1
    return HandlerResult(status=200, body={'deleted': deleted})


async def _list_org_users(org_id, config):
    with _get_db(config) as conn:
        with conn.cursor() as cur:
            # org_id로 code 조회
            cur.execute("SELECT code FROM organizations WHERE id=%s", (org_id,))
            row = cur.fetchone()
            if not row:
                return HandlerResult(status=404, body={'error': 'Organization not found'})
            code = row['code']
            cur.execute(
                "SELECT id, name, org_id FROM users WHERE org_id=%s ORDER BY name",
                (code,)
            )
            users = cur.fetchall()
    return HandlerResult(status=200, body={'org_id': org_id, 'org_code': code, 'users': users})




CIMS_ORG_HANDLER_LIST = [
    (_ORG_BASE, handle_organizations, {}),
]


# ── API 문서 (개발자 모드) ──────────────────────────────────────────────────
#  이 모듈이 제공하는 엔드포인트의 자기기술. OAM 의 handlers/api_docs.py 가 수집한다.
#  csc 미설치 환경에서는 이 파일이 없으므로 수집에서 자연히 빠진다.
_AUTH_MONITOR = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_MANAGER = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login'}

_ERR_COMMON = [
    {'status': 401, 'when': 'Authorization 헤더 없음 / 토큰 만료', 'body': {'error': 'unauthorized'}},
    {'status': 403, 'when': '권한 등급 미달', 'body': {'error': 'forbidden'}},
]

_ORG_FIELDS = [
    {'name': 'id', 'type': 'integer', 'desc': '조직 surrogate id (batch 삭제의 ids)'},
    {'name': 'code', 'type': 'string', 'desc': '조직 코드 — 가입자 org_id 가 참조하는 값'},
    {'name': 'code_path', 'type': 'string', 'desc': '루트부터의 코드 경로 (정렬 기준)'},
    {'name': 'name', 'type': 'string', 'desc': '조직명'},
    {'name': 'parent_id', 'type': 'integer', 'desc': '상위 조직 id (루트는 null)'},
    {'name': '(그 외)', 'type': 'object',
     'desc': '상세 조회(csc.orgs.get)는 SELECT * 라 스키마에 추가된 컬럼도 함께 온다'},
    {'name': 'sort_order', 'type': 'integer', 'desc': '같은 depth 내 표시 순서'},
]

_ORG_EXAMPLE = {'id': 3, 'code': 'D110', 'code_path': 'D100/D110', 'name': '운영팀',
                'parent_id': 1, 'sort_order': 10}

CIMS_ORG_API_DOCS = [
    {'id': 'csc.orgs.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/organizations',
     'summary': '조직 목록 (계층 코드 체계, code_path 순 정렬)',
     'params': [],
     'response': '{organizations[]}',
     'response_fields': [{'name': 'organizations[].' + f['name'],
                          **{k: v for k, v in f.items() if k != 'name'}} for f in _ORG_FIELDS],
     'example': {'organizations': [{'id': 1, 'code': 'D100', 'code_path': 'D100', 'name': '본사',
                                    'parent_id': None, 'sort_order': 0}, _ORG_EXAMPLE]},
     'errors': list(_ERR_COMMON),
     'notes': ['정렬은 code_path → sort_order → name 이라 배열 그대로 트리 순서로 그릴 수 있다.',
               '트리 구조는 parent_id 로 재구성한다 (code_path 로도 가능).'],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.orgs.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/organizations/{org_id}',
     'summary': '조직 1건 상세',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True,
                 'desc': '조직 id (숫자). **코드가 아니다**'}],
     'response': '조직 객체 (organizations[] 항목과 동일)',
     'response_fields': list(_ORG_FIELDS),
     'example': dict(_ORG_EXAMPLE),
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 조직', 'body': {'error': 'Not found'}}],
     'notes': [],
     'auth': dict(_AUTH_MONITOR)},

    {'id': 'csc.orgs.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/organizations',
     'summary': '조직 생성 (code_path 자동 계산)',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{code(필수), name(필수), parent_id?, sort_order?}'}],
     'response': '{id, code, code_path}',
     'response_fields': [
         {'name': 'id', 'type': 'integer', 'desc': '생성된 조직 id'},
         {'name': 'code', 'type': 'string', 'desc': '조직 코드'},
         {'name': 'code_path', 'type': 'string', 'desc': '계산된 코드 경로'},
     ],
     'example': {'id': 7, 'code': 'D120', 'code_path': 'D100/D120'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'Body required'}},
         {'status': 400, 'when': 'code 또는 name 누락', 'body': {'error': 'code, name 필수'}},
     ],
     'notes': ['성공 시 **201** 이다.', 'code_path 는 서버가 parent 를 따라 계산한다 — 직접 보내지 않는다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.orgs.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/organizations/{org_id}',
     'summary': '조직 수정 (전달한 필드만 변경)',
     'params': [
         {'name': 'org_id', 'in': 'path', 'type': 'integer', 'required': True, 'desc': '조직 id (숫자)'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경할 필드만'},
     ],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '수정된 조직 식별자'}],
     'example': {'id': '3'},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'JSON 본문 없음', 'body': {'error': 'Body required'}},
         {'status': 400, 'when': '변경할 필드 없음', 'body': {'error': '변경할 필드가 없습니다'}},
     ],
     'notes': ['parent_id 를 바꾸면 하위 조직의 code_path 도 함께 갱신된다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.orgs.delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/organizations/{org_id}',
     'summary': '조직 삭제',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True,
                 'desc': '조직 id (숫자)'}],
     'response': '{id}',
     'response_fields': [{'name': 'id', 'type': 'string', 'desc': '삭제된 조직 식별자'}],
     'example': {'id': '3'},
     'errors': _ERR_COMMON + [{'status': 404, 'when': '없는 조직', 'body': {'error': 'Not found'}}],
     'notes': ['**하위 조직은 함께 삭제되지 않고 parent_id 가 NULL 이 되어 루트로 올라간다.**',
               '소속 가입자가 있는 조직을 지우면 그 가입자의 org_id 는 참조가 끊긴다 — 먼저 이동시킬 것.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.orgs.batch-delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/organizations/batch',
     'summary': '조직 일괄 삭제',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{ids: [정수]}'}],
     'response': '{deleted}',
     'response_fields': [{'name': 'deleted', 'type': 'integer', 'unit': '건', 'desc': '삭제 건수'}],
     'example': {'deleted': 2},
     'errors': _ERR_COMMON + [
         {'status': 400, 'when': 'ids 누락', 'body': {'error': 'ids 필요'}},
     ],
     'notes': ['가입자 일괄 삭제와 달리 errors 배열이 없다 — 삭제된 개수만 돌려준다.',
               '단건 삭제와 같이 하위 조직은 루트로 올라간다.',
               '경로가 /organizations/{org_id} 와 겹치므로 batch 라는 예약어를 쓴다.'],
     'auth': dict(_AUTH_MANAGER)},

    {'id': 'csc.orgs.users', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/organizations/{org_id}/users',
     'summary': '조직 소속 가입자 목록',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True,
                 'desc': '조직 id (숫자) — 서버가 코드로 변환한다'}],
     'response': '{org_id, org_code, users[]}',
     'response_fields': [
         {'name': 'org_id', 'type': 'string', 'desc': '요청한 조직 식별자'},
         {'name': 'org_code', 'type': 'string', 'desc': '해석된 조직 코드'},
         {'name': 'users[].id', 'type': 'integer', 'desc': '가입자 id'},
         {'name': 'users[].name', 'type': 'string', 'desc': '이름'},
         {'name': 'users[].login_id', 'type': 'string', 'desc': '단말 로그인 ID'},
     ],
     'example': {'org_id': '3', 'org_code': 'D110',
                 'users': [{'id': 11, 'name': '홍길동', 'login_id': 'test001'}]},
     'errors': _ERR_COMMON + [
         {'status': 404, 'when': '없는 조직', 'body': {'error': 'Organization not found'}},
     ],
     'notes': ['**직속 소속만** 반환한다 — 하위 조직은 포함되지 않는다.',
               '번호(가입) 정보까지 필요하면 csc.users.list 를 쓴다.'],
     'auth': dict(_AUTH_MONITOR)},
]
