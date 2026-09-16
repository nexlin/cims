"""계측기 API — /api/v1/tester (세그먼트 하나, D2). base 게이트웨이가 여기로 프록시한다.

  GET  /health                      모듈 상태(버전·데이터 디렉터리·색인 수·스트림 구독자)
  GET  /schema                      계약 스키마 이름 목록
  GET  /schema/<name>               JSON 스키마(tester_models 에서 생성 — 워커 C++ 와 같은 계약)
  POST /validate                    {kind, doc|yaml} → {ok, errors[]}
  GET  /scenarios[/<id>]            패키지 동봉 + 운영자 추가 시나리오 (검증 오류 포함 목록)
  GET  /profiles[/<name>]           부하 프로파일
  GET|POST /topologies              토폴로지 목록·생성(검증 통과분만 저장)
  GET|PUT|DELETE /topologies/<id>
  GET  /runs[/<id>]                 run 색인 (본체는 Tester.DataDir/runs/<id>/)
  POST /runs                        run 시작 — B 단계(워커·오케스트레이터) 전까지 501
  GET  /workers                     발견된 워커 — B 단계 전까지 빈 목록
  GET  /events                      SSE(text/event-stream) — run/워커 상태 변화 라이브

권한: 조회 monitor, 토폴로지 쓰기·run 시작/중단 operator, 삭제 manager (test_instrument.md §6.2).
"""
from __future__ import annotations

import asyncio
import json
import os
from pathlib import PurePath
from urllib.parse import unquote, urlparse

import yaml

from httpsrv.handler import HandlerArgs, HandlerResult
from services.admin_auth import require_role
from services import tester_store as store
from services.tester_bus import TESTER_BUS
from services.tester_models import SCHEMAS, schema_json, validate

_BASE = '/api/v1/tester'
_VERSION = '0.1.0'


def init(component_root: str, config: dict) -> None:
    global _VERSION
    store.init(component_root, config)
    try:
        with open(os.path.join(component_root, 'pkg.json'), 'r', encoding='utf-8') as f:
            _VERSION = str(json.load(f).get('version') or _VERSION)
    except Exception:
        pass


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
        try: return json.loads(b.decode('utf-8'))
        except Exception: return None
    if isinstance(b, str):
        try: return json.loads(b)
        except Exception: return None
    return None


def _json(status: int, body) -> HandlerResult:
    return HandlerResult(status=status, body=body)


def _sse() -> HandlerResult:
    """SSE — alerts._sse_stream 과 같은 규약(20 초 `: ping`, 절단 시 구독 해제).
    게이트웨이는 text/event-stream 응답을 청크 그대로 통과시킨다(gateway.py)."""
    from starlette.responses import StreamingResponse

    loop = asyncio.get_running_loop()
    queue: asyncio.Queue = asyncio.Queue(maxsize=1000)
    sid = TESTER_BUS.subscribe(loop, queue)

    async def gen():
        try:
            yield b': connected\n\n'
            yield f'data: {json.dumps({"stream": "hello", "record": {"module": "oam-cims-tester", "version": _VERSION}})}\n\n'.encode('utf-8')
            while True:
                try:
                    rec = await asyncio.wait_for(queue.get(), timeout=20)
                    yield f'data: {json.dumps(rec, ensure_ascii=False)}\n\n'.encode('utf-8')
                except asyncio.TimeoutError:
                    yield b': ping\n\n'
        finally:
            TESTER_BUS.unsubscribe(sid)

    resp = StreamingResponse(gen(), media_type='text/event-stream', headers={
        'Cache-Control': 'no-cache',
        'X-Accel-Buffering': 'no',
        'Connection': 'keep-alive',
    })
    return HandlerResult(response=resp)


async def handle_tester(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = (handler_args.method or 'GET').upper()
    parts = _parts(handler_args.full_path)
    head = parts[0] if parts else ''

    # 쓰기 권한 — 삭제 manager, 그 외 변경 operator, 조회 monitor.
    if method == 'DELETE':
        min_role = 'manager'
    elif method in ('POST', 'PUT', 'PATCH'):
        min_role = 'operator'
    else:
        min_role = 'monitor'
    _, err = require_role(handler_args, min_role)
    if err:
        return err

    if head == 'health' and method == 'GET':
        return _json(200, {
            'module': 'oam-cims-tester', 'version': _VERSION,
            'data_dir': store.data_dir(),
            'scenarios': len(store.list_scenarios()), 'profiles': len(store.list_profiles()),
            'topologies': len(store.list_topologies()), 'runs': len(store.list_runs(limit=100000)),
            'workers': 0,
            'stream_subscribers': TESTER_BUS.subscriber_count(),
            'phase': 'A',   # 이행 단계 — B 에서 run 실행 가능
        })

    if head == 'events' and method == 'GET':
        return _sse()

    if head == 'schema' and method == 'GET':
        if len(parts) == 1:
            return _json(200, {'schemas': sorted(SCHEMAS)})
        name = parts[1]
        if name not in SCHEMAS:
            return _json(404, {'error': 'schema_not_found', 'name': name})
        return _json(200, schema_json(name))

    if head == 'validate' and method == 'POST':
        body = _body(handler_args) or {}
        kind = body.get('kind')
        doc = body.get('doc')
        if doc is None and isinstance(body.get('yaml'), str):
            try:
                doc = yaml.safe_load(body['yaml'])
            except Exception as e:
                return _json(200, {'ok': False, 'errors': [f'YAML 파싱 실패: {e}']})
        if not isinstance(doc, dict):
            return _json(400, {'error': 'doc 또는 yaml 이 필요하다'})
        model, errs = validate(str(kind), doc)
        return _json(200, {'ok': model is not None, 'errors': errs})

    if head == 'scenarios' and method == 'GET':
        if len(parts) == 1:
            return _json(200, {'scenarios': store.list_scenarios()})
        model, doc, errs = store.get_scenario(parts[1])
        if doc is None:
            return _json(404, {'error': 'scenario_not_found', 'id': parts[1]})
        return _json(200, {'id': parts[1], 'doc': doc, 'errors': errs, 'valid': model is not None})

    if head == 'profiles' and method == 'GET':
        if len(parts) == 1:
            return _json(200, {'profiles': store.list_profiles()})
        model, doc, errs = store.get_profile(parts[1])
        if doc is None:
            return _json(404, {'error': 'profile_not_found', 'name': parts[1]})
        return _json(200, {'name': parts[1], 'doc': doc, 'errors': errs, 'valid': model is not None})

    if head == 'topologies':
        if len(parts) == 1 and method == 'GET':
            return _json(200, {'topologies': store.list_topologies()})
        if len(parts) == 1 and method == 'POST':
            body = _body(handler_args)
            if not isinstance(body, dict):
                return _json(400, {'error': 'body 는 토폴로지 문서(JSON) 여야 한다'})
            rec, errs = store.save_topology(body)
            if errs:
                return _json(400, {'error': 'invalid_topology', 'errors': errs})
            return _json(201, rec)
        if len(parts) == 2:
            try:
                tid = int(parts[1])
            except ValueError:
                return _json(400, {'error': 'id 는 정수'})
            if method == 'GET':
                rec = store.get_topology(tid)
                return _json(200, rec) if rec else _json(404, {'error': 'topology_not_found', 'id': tid})
            if method == 'PUT':
                body = _body(handler_args)
                if not isinstance(body, dict):
                    return _json(400, {'error': 'body 는 토폴로지 문서(JSON) 여야 한다'})
                rec, errs = store.save_topology(body, tid)
                if errs:
                    st = 404 if any('토폴로지 없음' in e for e in errs) else 400
                    return _json(st, {'error': 'invalid_topology', 'errors': errs})
                return _json(200, rec)
            if method == 'DELETE':
                ok = store.delete_topology(tid)
                return _json(200 if ok else 404, {'deleted': ok, 'id': tid})

    if head == 'runs':
        if len(parts) == 1 and method == 'GET':
            try:
                limit = int((handler_args.query_params or {}).get('limit', 100))
            except ValueError:
                limit = 100
            return _json(200, {'runs': store.list_runs(limit=limit)})
        if len(parts) == 1 and method == 'POST':
            # B 단계(워커·오케스트레이터) 전 — 계약만 확정된 상태를 정직하게 알린다.
            return _json(501, {'error': 'not_implemented',
                               'detail': 'run 실행은 B 단계(cims-tester-worker + 오케스트레이터)에서 열린다 — test_instrument.md §10'})
        if len(parts) == 2 and method == 'GET':
            rec = store.get_run(parts[1])
            return _json(200, rec) if rec else _json(404, {'error': 'run_not_found', 'id': parts[1]})

    if head == 'workers' and method == 'GET':
        return _json(200, {'workers': []})

    return _json(404, {'error': 'not_found', 'path': handler_args.full_path})


TESTER_HANDLER_LIST = [
    (_BASE, handle_tester, {}),
]
