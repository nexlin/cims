"""/api/v1/provision/* REST — 콘솔 [자동 배포] 탭의 백엔드 (auto_deployment.md §6).

  GET  POST         /blueprints                블루프린트 목록 / 업로드
  GET  PUT  DELETE  /blueprints/{id}           1건(구조+원문) / 수정 / 삭제
  GET               /blueprints/{id}/raw       YAML 원문 (다운로드)
  POST              /blueprints/validate       스키마 + 인벤토리 참조 검증
  GET  POST         /inventories               목록 / 업로드   (항상 마스킹 응답)
  GET  PUT  DELETE  /inventories/{id}          1건 / 수정 / 삭제
  POST              /inventories/{id}/preflight  SSH·sudo 접속만 확인 (변경 없음)
  GET  POST         /runs                      run 목록 / 시작 (?dry_run=true 는 계획만)
  GET               /runs/{id}                 진행 상태
  POST              /runs/{id}/resume|abort|rollback

base OAM 내장 핸들러다 — 별도 프로세스·포트·게이트웨이 라우트가 없다. OAM 이 떠 있으면
그 순간부터 동작한다(엔진을 쓰려면 엔진을 먼저 수동 배포해야 하는 닭-달걀 제거).

전 경로 admin 필수. 요청의 Authorization 토큰을 **그대로 OAM REST 호출에 재사용**한다 —
provisioner 는 자격증명을 발급하지 않는다. 긴 run 중 토큰이 만료되면 auth_expired 로 멈추고,
재로그인 후 [재개]하면 이어진다.

run 은 백그라운드 스레드에서 돈다 — POST 는 run_id 만 즉시 반환하고 콘솔이 폴링한다.
(수 분짜리 작업을 HTTP 요청으로 붙잡지 않는다.)
"""

from __future__ import annotations

import json
import threading
from pathlib import PurePath
from urllib.parse import urlparse, unquote

from httpsrv.handler import HandlerArgs, HandlerResult
from handlers import auth

from services.provision import schema, engine, phases
from services.provision import ssh as sshmod
from services.provision.oam_client import OamClient, OamError
from services.provision.store import Store

_BASE = '/api/v1/provision'

# run_id → {'engine': Engine, 'thread': Thread} — abort 와 중복 실행 차단에 쓴다.
_ACTIVE: dict = {}
_LOCK = threading.Lock()

_store: Store | None = None
_config: dict = {}


def init(config: dict, store: Store):
    global _store, _config
    _config = config
    _store = store


# ── 공통 ────────────────────────────────────────────────────────

def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _body(handler_args: HandlerArgs):
    b = getattr(handler_args, 'body', None)
    if isinstance(b, dict):
        return b
    if isinstance(b, (bytes, bytearray)):
        b = b.decode('utf-8', 'replace')
    if isinstance(b, str):
        try:
            return json.loads(b)
        except ValueError:
            return None
    return None


def _err(status, code, message=None, **extra):
    d = {'error': code}
    if message:
        d['message'] = message
    d.update(extra)
    return HandlerResult(status=status, body=d, media_type='application/json')


def _ok(body, status=200):
    return HandlerResult(status=status, body=body, media_type='application/json')


def _int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _bearer(handler_args: HandlerArgs) -> str:
    for k, v in (handler_args.headers or {}).items():
        if k.lower() == 'authorization':
            return v.split(None, 1)[1] if ' ' in v else v
    return ''


def _self_url() -> str:
    """OAM 자기 자신을 호출할 주소 — 같은 프로세스이므로 loopback + 자기 bind 포트.

    별도 키를 새로 만들지 않는다: oam.json 의 Server.Port 가 정본이다.
    (0.0.0.0 bind 여도 loopback 으로 접속 가능.)
    """
    override = (_config.get('OamUrl') or '').strip()
    if override:
        return override.rstrip('/')
    port = ((_config.get('Server') or {}).get('Port')) or 4419
    return f'https://127.0.0.1:{port}'


def _enroll_url(handler_args) -> str:
    """대상 노드의 agent 가 접속할 OAM 주소.

    콘솔의 install-command 와 **같은 값**을 쓴다 (handlers.agents._oam_public_url):
    Server.AgentOamUrl → Host 헤더 → Server.Ip:Port. loopback 을 원격 노드에 알려주는
    사고를 막기 위해 자기호출 URL 과 분리한다.
    """
    from handlers.agents import _oam_public_url
    return _oam_public_url(handler_args, _config)


def _oam(handler_args) -> OamClient:
    return OamClient(_self_url(), _bearer(handler_args))


# ── blueprints ──────────────────────────────────────────────────

def _load_blueprint_doc(text: str):
    """YAML 원문 → (Blueprint, issues). 문법 오류는 HandlerResult 로 변환해 올린다."""
    try:
        return schema.parse_blueprint(text)
    except schema.ParseError as e:
        raise _HttpFail(_err(400, 'yaml_parse_error', e.message, line=e.line)) from None


class _HttpFail(Exception):
    def __init__(self, result):
        self.result = result


def _blueprints(method, parts, handler_args):
    if not parts:
        if method == 'GET':
            return _ok({'blueprints': _store.list_blueprints()})
        if method == 'POST':
            b = _body(handler_args) or {}
            raw = b.get('raw') or b.get('yaml') or ''
            if not raw.strip():
                return _err(400, 'raw_required', 'YAML 원문(raw) 필요')
            bp, issues = _load_blueprint_doc(raw)
            if bp is None:
                return _err(400, 'invalid_blueprint',
                            issues=[i.as_dict() for i in issues])
            rec = _store.save_blueprint(bp.as_dict(), raw, name=bp.name,
                                        description=bp.description)
            return _ok({'id': rec['id'], 'name': rec['name'],
                        'issues': [i.as_dict() for i in issues]}, 201)
        return _err(405, 'method_not_allowed')

    if parts[0] == 'validate' and method == 'POST':
        b = _body(handler_args) or {}
        bp_raw, inv_raw = b.get('blueprint') or '', b.get('inventory') or ''
        if b.get('blueprint_id') and not bp_raw:
            rec = _store.get_blueprint(_int(b['blueprint_id'])) or {}
            bp_raw = rec.get('raw') or ''
        if b.get('inventory_id') and not inv_raw:
            rec = _store.get_inventory(_int(b['inventory_id'])) or {}
            inv_raw = rec.get('raw') or ''
        return _ok(schema.validate(bp_raw, inv_raw))

    bid = _int(parts[0])
    if bid is None:
        return _err(404, 'not_found')
    rec = _store.get_blueprint(bid)
    if not rec:
        return _err(404, 'not_found', f'블루프린트 #{bid} 없음')

    if len(parts) == 2 and parts[1] == 'raw' and method == 'GET':
        return HandlerResult(status=200, body=rec.get('raw') or '',
                             media_type='text/yaml',
                             headers={'Content-Disposition':
                                      f'attachment; filename="{rec.get("name")}.yaml"'})
    if len(parts) != 1:
        return _err(404, 'not_found')

    if method == 'GET':
        return _ok(rec)
    if method == 'PUT':
        b = _body(handler_args) or {}
        raw = b.get('raw')
        if raw is None and isinstance(b.get('doc'), dict):
            # 구성 뷰 편집 — 구조에서 YAML 재생성. 원본 주석은 여기서 소실된다(§3.0).
            raw = schema.dump_yaml(b['doc'])
        if not raw:
            return _err(400, 'raw_or_doc_required')
        bp, issues = _load_blueprint_doc(raw)
        if bp is None:
            return _err(400, 'invalid_blueprint', issues=[i.as_dict() for i in issues])
        saved = _store.save_blueprint(bp.as_dict(), raw, bid=bid, name=bp.name,
                                      description=bp.description)
        return _ok({'id': saved['id'], 'issues': [i.as_dict() for i in issues]})
    if method == 'DELETE':
        return _ok({'deleted': _store.delete_blueprint(bid)})
    return _err(405, 'method_not_allowed')


# ── inventories ─────────────────────────────────────────────────

def _inventories(method, parts, handler_args):
    if not parts:
        if method == 'GET':
            return _ok({'inventories': _store.list_inventories()})
        if method == 'POST':
            b = _body(handler_args) or {}
            raw = b.get('raw') or b.get('yaml') or ''
            if not raw.strip():
                return _err(400, 'raw_required', 'YAML 원문(raw) 필요')
            try:
                inv, issues = schema.parse_inventory(raw)
            except schema.ParseError as e:
                return _err(400, 'yaml_parse_error', e.message, line=e.line)
            if inv is None:
                return _err(400, 'invalid_inventory', issues=[i.as_dict() for i in issues])
            rec = _store.save_inventory(inv.as_dict(mask=False), raw,
                                        name=b.get('name') or '')
            return _ok({'id': rec['id'], 'name': rec['name'],
                        'inventory': inv.as_dict(mask=True),
                        'issues': [i.as_dict() for i in issues]}, 201)
        return _err(405, 'method_not_allowed')

    iid = _int(parts[0])
    if iid is None:
        return _err(404, 'not_found')
    rec = _store.get_inventory(iid)
    if not rec:
        return _err(404, 'not_found', f'인벤토리 #{iid} 없음')

    # 인벤토리에는 /raw 가 없다 — 원문에 비밀번호가 있으므로 (§8).
    if len(parts) == 2 and parts[1] == 'preflight' and method == 'POST':
        return _preflight(rec, handler_args)
    if len(parts) != 1:
        return _err(404, 'not_found')

    if method == 'GET':
        inv, _ = schema.parse_inventory(rec['raw'])
        return _ok({'id': iid, 'name': rec.get('name'),
                    'inventory': inv.as_dict(mask=True) if inv else None})
    if method == 'PUT':
        b = _body(handler_args) or {}
        raw = b.get('raw')
        if raw is None and isinstance(b.get('doc'), dict):
            merged = _merge_secrets(b['doc'], rec)
            raw = schema.dump_yaml(schema.normalize_inventory_doc(merged))
        if not raw:
            return _err(400, 'raw_or_doc_required')
        try:
            inv, issues = schema.parse_inventory(raw)
        except schema.ParseError as e:
            return _err(400, 'yaml_parse_error', e.message, line=e.line)
        if inv is None:
            return _err(400, 'invalid_inventory', issues=[i.as_dict() for i in issues])
        _store.save_inventory(inv.as_dict(mask=False), raw, iid=iid,
                              name=b.get('name') or rec.get('name') or '')
        return _ok({'id': iid, 'inventory': inv.as_dict(mask=True),
                    'issues': [i.as_dict() for i in issues]})
    if method == 'DELETE':
        return _ok({'deleted': _store.delete_inventory(iid)})
    return _err(405, 'method_not_allowed')


def _merge_secrets(doc: dict, stored: dict) -> dict:
    """구성 뷰 편집 저장 — 비밀 필드가 비어 오면 저장값을 유지한다.

    마스킹 문자열이 그대로 되돌아와 실제 비밀번호를 덮어쓰는 사고를 구조적으로 막는다(§8).
    """
    prev = {s.get('name'): s for s in ((stored.get('doc') or {}).get('servers') or [])}
    for srv in doc.get('servers') or []:
        old = prev.get(srv.get('name')) or {}
        for sect, key in schema.SECRET_FIELDS:
            new_val = (srv.get(sect) or {}).get(key)
            if new_val in (None, '', '••••', '***'):
                old_val = (old.get(sect) or {}).get(key)
                if old_val:
                    srv.setdefault(sect, {})[key] = old_val
                elif sect in srv and key in srv[sect]:
                    del srv[sect][key]
    return doc


def _preflight(rec, handler_args):
    inv, _ = schema.parse_inventory(rec['raw'])
    if inv is None:
        return _err(400, 'invalid_inventory')
    ssh_cfg = _config.get('Ssh') or {}
    results = sshmod.preflight_all(
        inv.servers,
        max_parallel=int((_config.get('Run') or {}).get('MaxParallel', 8) or 8),
        connect_timeout=int(ssh_cfg.get('ConnectTimeout', 15) or 15),
        command_timeout=int(ssh_cfg.get('CommandTimeout', 900) or 900),
        strict_host_key=ssh_cfg.get('StrictHostKeyChecking') or 'accept-new',
        known_hosts=None)
    return _ok({'results': results,
                'ok': all(r.get('ok') for r in results)})


# ── runs ────────────────────────────────────────────────────────

def _build_ctx(run: dict, handler_args):
    bp_rec = _store.get_blueprint(run['blueprint_id'])
    inv_rec = _store.get_inventory(run['inventory_id'])
    if not bp_rec or not inv_rec:
        raise OamError('input_missing', '블루프린트 또는 인벤토리 레코드가 없음')
    bp, bp_iss = schema.parse_blueprint(bp_rec['raw'])
    inv, inv_iss = schema.parse_inventory(inv_rec['raw'])
    if bp is None or inv is None:
        raise OamError('input_invalid', '저장된 문서가 더 이상 유효하지 않음 — 다시 업로드')
    cross = schema.cross_validate(bp, inv)
    if any(i.level == 'error' for i in cross):
        raise OamError('input_invalid',
                       '; '.join(i.message for i in cross if i.level == 'error'))

    cfg = dict(_config)
    cfg['_runtime_dir'] = _store.root
    cfg['OamUrl'] = _self_url()
    # enroll 주소는 run 생성 시점 값을 고정한다 — resume 요청의 Host 헤더가 달라도
    # 이미 설치된 agent 와 같은 OAM 을 가리키게.
    if not run.get('enroll_url'):
        run['enroll_url'] = _enroll_url(handler_args)
    cfg['AgentEnrollUrl'] = run['enroll_url']

    def log(msg):
        run.setdefault('log', []).append(msg)
        del run['log'][:-500]                 # 로그 무한 성장 방지

    return engine.Context(blueprint=bp, inventory=inv,
                          oam=OamClient(cfg['OamUrl'], _bearer(handler_args)),
                          config=cfg, run=run, log=log)


def _spawn(run: dict, handler_args, on_error: str):
    ctx = _build_ctx(run, handler_args)
    eng = engine.Engine(_store, ctx, phases.ORDER, on_error=on_error)

    def body():
        try:
            eng.execute()
        finally:
            with _LOCK:
                _ACTIVE.pop(run['id'], None)

    th = threading.Thread(target=body, name=f"prov-run-{run['id']}", daemon=True)
    with _LOCK:
        _ACTIVE[run['id']] = {'engine': eng, 'thread': th}
    th.start()


def _runs(method, parts, handler_args, payload):
    if not parts:
        if method == 'GET':
            return _ok({'runs': _store.list_runs()})
        if method == 'POST':
            b = _body(handler_args) or {}
            bid, iid = _int(b.get('blueprint_id')), _int(b.get('inventory_id'))
            if bid is None or iid is None:
                return _err(400, 'ids_required', 'blueprint_id 와 inventory_id 필요')
            dry = str((handler_args.query_params or {}).get('dry_run', '')).lower() \
                in ('1', 'true', 'yes') or bool(b.get('dry_run'))
            on_error = b.get('on_error') or 'stop'

            bp_rec = _store.get_blueprint(bid)
            run = engine.new_run(_store, blueprint_id=bid, inventory_id=iid,
                                 blueprint_name=(bp_rec or {}).get('name') or '',
                                 actor=(payload or {}).get('login_id') or '',
                                 on_error=on_error)
            try:
                if dry:
                    ctx = _build_ctx(run, handler_args)
                    plan = engine.Engine(_store, ctx, phases.ORDER).plan()
                    return _ok({'dry_run': True, 'blueprint': ctx.blueprint.name,
                                'phases': plan})
                _store.save_run(run)
                _spawn(run, handler_args, on_error)
                return _ok({'run_id': run['id'], 'status': 'running'}, 202)
            except OamError as e:
                return _err(400, e.code, e.message)
        return _err(405, 'method_not_allowed')

    rid = _int(parts[0])
    if rid is None:
        return _err(404, 'not_found')
    run = _store.get_run(rid)
    if not run:
        return _err(404, 'not_found', f'run #{rid} 없음')

    if len(parts) == 1:
        if method == 'GET':
            with _LOCK:
                run['live'] = rid in _ACTIVE
            return _ok(run)
        return _err(405, 'method_not_allowed')

    if len(parts) != 2 or method != 'POST':
        return _err(404, 'not_found')
    action = parts[1]

    with _LOCK:
        active = _ACTIVE.get(rid)

    if action == 'abort':
        if not active:
            return _err(409, 'not_running', 'run 이 실행 중이 아님')
        active['engine'].abort()
        return _ok({'run_id': rid, 'aborting': True})

    if action == 'resume':
        if active:
            return _err(409, 'already_running', 'run 이 이미 실행 중')
        if run.get('status') == 'succeeded':
            return _err(409, 'already_succeeded', '이미 완료된 run')
        try:
            _spawn(run, handler_args, run.get('on_error') or 'stop')
        except OamError as e:
            return _err(400, e.code, e.message)
        return _ok({'run_id': rid, 'status': 'running'}, 202)

    if action == 'rollback':
        if active:
            return _err(409, 'already_running', '실행 중에는 롤백할 수 없음 — 먼저 [중단]')
        return _rollback(run, handler_args)

    return _err(404, 'not_found')


def _rollback(run: dict, handler_args):
    """이 run 이 **생성한 것만** 역순으로 되돌린다 (§4).

    agent 설치는 대상 노드의 uninstall.sh 로만 제거되므로 롤백 범위 밖이다.
    """
    oam = _oam(handler_args)
    created = list(run.get('created') or [])
    undone, failed = [], []

    for entry in reversed(created):
        kind, ident, label = entry['kind'], entry['id'], entry.get('label', '')
        try:
            if kind == 'deployment':
                dep = oam.get_deployment(ident) or {}
                if dep.get('status') == 'running':
                    oam.queue_job(ident, 'stop')
                oam.delete(f'/api/v1/deployments/{ident}')
                undone.append(f'배포#{ident} {label}')
            elif kind == 'ha_group':
                oam.delete(f'/api/v1/ha-groups/{ident}')
                undone.append(f'그룹#{ident} {label}')
        except OamError as e:
            failed.append(f'{kind}#{ident} {label}: {e.message}')

    run['status'] = 'rolled_back' if not failed else 'rollback_partial'
    run['rollback'] = {'undone': undone, 'failed': failed}
    if not failed:
        run['created'] = []
    _store.save_run(run)
    return _ok({'run_id': run['id'], 'status': run['status'],
                'undone': undone, 'failed': failed})


# ── 진입점 ──────────────────────────────────────────────────────

async def handle_provision(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    payload, err = auth.require_admin(handler_args)
    if err:
        return err
    if _store is None:
        return _err(503, 'not_initialized', 'provisioner 스토어가 초기화되지 않음')

    method = handler_args.method.upper()
    parts = _parts(handler_args.full_path)
    if not parts:
        return _ok({'service': 'provisioner',
                    'endpoints': ['blueprints', 'inventories', 'runs']})
    try:
        if parts[0] == 'blueprints':
            return _blueprints(method, parts[1:], handler_args)
        if parts[0] == 'inventories':
            return _inventories(method, parts[1:], handler_args)
        if parts[0] == 'runs':
            return _runs(method, parts[1:], handler_args, payload)
        return _err(404, 'not_found')
    except _HttpFail as e:
        return e.result
    except OamError as e:
        return _err(502, e.code, e.message)
    except Exception as e:                                     # noqa: BLE001
        import traceback
        return _err(500, 'internal_error', str(e), trace=traceback.format_exc()[-1200:])


CIMS_PROVISION_HANDLER_LIST = [
    (_BASE, handle_provision, {}),
]
