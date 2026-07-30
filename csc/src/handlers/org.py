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
CIMS_ORG_API_DOCS = [
    {'id': 'csc.orgs.list', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/organizations',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 목록 (계층 코드 체계)',
     'params': [], 'response': '{organizations[]}', 'auth': 'Bearer JWT (monitor)'},
    {'id': 'csc.orgs.create', 'module': 'csc', 'method': 'POST', 'path': '/api/v1/organizations',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 생성',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True,
                 'desc': '{code, name, parent_code?, ...}'}],
     'response': '{id, code}', 'auth': 'Bearer JWT (manager)'},
    {'id': 'csc.orgs.get', 'module': 'csc', 'method': 'GET', 'path': '/api/v1/organizations/{org_id}',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 1건 상세',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '조직 코드/ID'}],
     'response': '조직 객체', 'auth': 'Bearer JWT (monitor)'},
    {'id': 'csc.orgs.update', 'module': 'csc', 'method': 'PUT', 'path': '/api/v1/organizations/{org_id}',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 수정',
     'params': [
         {'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '조직 코드/ID'},
         {'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '변경 필드'},
     ],
     'response': '{id}', 'auth': 'Bearer JWT (manager)'},
    {'id': 'csc.orgs.delete', 'module': 'csc', 'method': 'DELETE', 'path': '/api/v1/organizations/{org_id}',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 삭제',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '조직 코드/ID'}],
     'response': '{id}', 'auth': 'Bearer JWT (manager)'},
    {'id': 'csc.orgs.batch-delete', 'module': 'csc', 'method': 'DELETE',
     'path': '/api/v1/organizations/batch',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 일괄 삭제',
     'params': [{'name': 'body', 'in': 'body', 'type': 'object', 'required': True, 'desc': '{ids: [int]}'}],
     'response': '{deleted}', 'auth': 'Bearer JWT (manager)'},
    {'id': 'csc.orgs.users', 'module': 'csc', 'method': 'GET',
     'path': '/api/v1/organizations/{org_id}/users',
     'screens': ['/subscribers/organizations'],
     'summary': '조직 소속 가입자 목록',
     'params': [{'name': 'org_id', 'in': 'path', 'type': 'string', 'required': True, 'desc': '조직 코드/ID'}],
     'response': '{users[]}', 'auth': 'Bearer JWT (monitor)'},
]
