"""계측기 API — /api/v1/tester (세그먼트 하나, D2). base 게이트웨이가 여기로 프록시한다.

  GET  /health                      모듈 상태(버전·데이터 디렉터리·색인 수·스트림 구독자)
  GET  /schema                      계약 스키마 이름 목록
  GET  /schema/<name>               JSON 스키마(tester_models 에서 생성 — 워커 C++ 와 같은 계약)
  POST /validate                    {kind, doc|yaml} → {ok, errors[], doc}(파싱된 문서 — 편집기의 YAML→캔버스)
  GET  /scenarios/vocab             단계 어휘·지표·kind 게이트·워커 지원 셋·Q.850 목록 — 시나리오 편집기 팔레트/폼의 정본
  POST /scenarios/compile-check     {scenario_id|doc|yaml, topology_id|topology, profile?, bindings?, instances?, rate_saps?} → compile_run 드라이런(tester_plan)
  GET  /scenarios[/<id>]            패키지 동봉 + 운영자 추가 시나리오 (검증 오류 포함 목록). 상세는 doc+yaml 원문
  PUT  /scenarios/<id>              {yaml} → 운영자본 저장(Tester.DataDir/scenarios/, 검증 통과분만, id 는 경로와 일치)
  DELETE /scenarios/<id>            운영자본만(동봉본 409 bundled)
  GET  /profiles[/<name>]           부하 프로파일 — PUT/DELETE 규약은 시나리오와 같다(profiles/ 아래)
  GET|POST /topologies              토폴로지 목록·생성(검증 통과분만 저장)
  GET|PUT|DELETE /topologies/<id>
  POST /topologies/<id>/check       연결 검사 — CSP OPTIONS/TCP/TLS·CSC·대상 OAM 토큰·워커 health (tester_check)
  GET  /runs[/<id>]                 run 색인 (본체는 Tester.DataDir/runs/<id>/) — 진행 중이면 라이브 누계 포함.
                                    색인 필터 ?scenario=&build=&verdict=a,b&since=ISO|7d&profile=&label=&topology=&load=1
  POST /runs                        run 시작 (RunRequest) → 202 {id}. 동시에 하나만
  POST /runs/plan                   계획 미리보기 — compile-check 와 같은 함수(run 시작 창)
  POST /runs/<id>/stop              중단(drain 뒤 verdict=aborted)
  POST /runs/<id>/rate              {rate_saps} 율 변경(진행 중)
  POST /runs/<id>/hold              {hold: bool} 단계 고정/재개 — 프로파일 시계 정지, 율 유지
  GET  /runs/<id>/hist?timer=       지연 지표 버킷 분포(로그 상한) + p50/p95/p99 — 행 펼침 히스토그램
  GET  /workers/discovered          자기 base OAM 배포 목록의 cims-tester-worker (Tester.BaseOamUrl 필요)
  GET  /runs/<id>/target-series     대상 자원 시계열(호스트 cpu_pct·mem_pct + SSH 프로세스별 cpu_pct·rss_mb — 대상 관측 §5)
  GET  /runs/<id>/sip               SIP 덤프 목록(call_id·bytes·messages)
  GET  /runs/<id>/sip/<call_id>     그 Call-ID 의 실패 이벤트 + SIP 덤프(runs/<id>/sip/<call_id>.log 가 있을 때)
  GET  /runs/<id>/target-alerts     대상 OAM 알람/이벤트를 run 창(started~ended)으로 잘라 — 대상 oam 노드 필요
  GET  /runs/<id>/report            run.json 전체(RFC 6076 표·expect 판정·단계 로그) + markdown
  GET  /runs/<id>/events            실패 개별 건(events.jsonl 꼬리, ?limit=)
  GET  /runs/<id>/series            1초 시계열(metrics.sqlite → 열 형태: t[], counters{}, gauges{}, timers{p95[],count[]})
  GET  /runs/<id>/stream            SSE — 이 run 의 agg/events/runs 프레임만
  DELETE /runs/<id>                 색인 + 본체 제거(진행 중 409)
  GET  /runs/compare?ids=a,b[,c]    run 나열 비교 — 첫 id 가 기준, 지표별 delta·회귀 판정(target_build 병기). &format=md|csv 는 텍스트
  GET  /api/v1/api-docs             이 모듈의 API 자기기술(base /api/v1/api-docs 가 모듈별로 수집 — api_docs.py)
  GET  /workers[?topology=<id>]     토폴로지 워커 + GET /health 결과
  GET  /events                      SSE(text/event-stream) — run/워커 상태 변화 라이브

권한: 조회 monitor, 토폴로지 쓰기·run 시작/중단 operator, 삭제 manager (test_instrument.md §6.2).
"""
from __future__ import annotations

import asyncio
import json
import os
from datetime import datetime
from typing import List, Optional
from pathlib import PurePath
from urllib.parse import unquote, urlparse

import yaml

from httpsrv.handler import HandlerArgs, HandlerResult
from services.admin_auth import require_role
from services import tester_store as store
from services.tester_bus import TESTER_BUS
from services.tester_models import (SCHEMAS, schema_json, validate, RunRequest, STEP_VOCAB, STEP_GROUPS, METRIC_NAMES, REAL_UE_STEPS,
                                    METRIC_LABELS, RATIO_METRICS, WORKER_STEPS, DURING_STEPS, Q850_CAUSES, AUDIO_CODECS,
                                    VIDEO_CODECS, RTP_MODES, SAMPLE_CODECS)
from services.tester_run import RUNS
from services import tester_workers
from services import tester_check
from services import tester_plan
from services import tester_observe
from services import tester_target
from services.tester_run import run_series, run_hist, run_call_events, run_sip_dumps
from starlette.responses import PlainTextResponse

_BASE = '/api/v1/tester'
_VERSION = '0.1.0'
_BASE_OAM_URL = ''      # Tester.BaseOamUrl — 워커 발견(GET /workers/discovered)이 부르는 자기 base OAM


def init(component_root: str, config: dict) -> None:
    global _VERSION, _BASE_OAM_URL
    _BASE_OAM_URL = str(((config or {}).get('Tester') or {}).get('BaseOamUrl') or '').strip()
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


def _bearer(handler_args) -> Optional[str]:
    """요청의 Bearer 토큰 — 동거 형태에서 대상 OAM(자기 base)을 요청자 권한으로 부를 때 쓴다(tester_target.resolve_token)."""
    auth = (getattr(handler_args, 'headers', None) or {}).get('authorization', '')
    return auth[7:].strip() if auth.startswith('Bearer ') else None


def _json(status: int, body) -> HandlerResult:
    return HandlerResult(status=status, body=body)


def _sse(run_id: str = '') -> HandlerResult:
    """SSE — alerts._sse_stream 과 같은 규약(20 초 `: ping`, 절단 시 구독 해제).
    게이트웨이는 text/event-stream 응답을 청크 그대로 통과시킨다(gateway.py). run_id 를 주면 그 run 프레임만."""
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
                    if run_id and (rec.get('record') or {}).get('run_id') != run_id:
                        continue
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
            'active_runs': [d.run_id for d in RUNS.active()],
            'stream_subscribers': TESTER_BUS.subscriber_count(),
            'worker_stream': ({'ip': RUNS.stream.ip, 'port': RUNS.stream.port, 'connections': RUNS.stream.connections}
                              if RUNS.stream else None),
            'phase': 'E2',  # 이행 단계 — 토폴로지 v2 + 계획 미리보기(plan/compile-check)·hold·hist·sip·색인 필터
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
        # doc 을 되돌려 준다 — 콘솔 편집기는 YAML 파서를 따로 두지 않고(단일 파서 = 서버) 여기서 받은 문서로 캔버스를 그린다
        return _json(200, {'ok': model is not None, 'errors': errs, 'doc': doc})

    if head == 'scenarios' and len(parts) == 2 and parts[1] == 'vocab' and method == 'GET':
        return _json(200, scenario_vocab())

    if (head == 'scenarios' and len(parts) == 2 and parts[1] == 'compile-check' and method == 'POST') or \
            (head == 'runs' and len(parts) == 2 and parts[1] == 'plan' and method == 'POST'):
        body = _body(handler_args)
        if not isinstance(body, dict):
            return _json(400, {'error': 'body 는 {scenario_id|doc|yaml, topology_id|topology, profile?, bindings?, instances?, rate_saps?}'})
        status, out = await _plan(body, config)
        return _json(status, out)

    if head in ('scenarios', 'profiles'):
        is_sc = head == 'scenarios'
        key = 'id' if is_sc else 'name'
        if len(parts) == 1 and method == 'GET':
            return _json(200, {'scenarios': store.list_scenarios()} if is_sc else {'profiles': store.list_profiles()})
        if len(parts) == 2:
            name = parts[1]
            if method == 'GET':
                model, doc, errs = (store.get_scenario(name) if is_sc else store.get_profile(name))
                if doc is None:
                    return _json(404, {'error': f'{head[:-1]}_not_found', key: name})
                rows = store.list_scenarios() if is_sc else store.list_profiles()
                row = next((r for r in rows if r[key] == name), None)
                text = store.read_yaml_text(row['path']) if row else None
                return _json(200, {key: name, 'doc': doc, 'yaml': text, 'source': (row or {}).get('source'),
                                   'errors': errs, 'valid': model is not None})
            if method == 'PUT':
                body = _body(handler_args) or {}
                text = body.get('yaml')
                if not isinstance(text, str) or not text.strip():
                    return _json(400, {'error': 'yaml(문자열) 이 필요하다'})
                row, errs = (store.save_scenario_yaml(name, text) if is_sc else store.save_profile_yaml(name, text))
                if errs:
                    return _json(400, {'error': f'invalid_{head[:-1]}', 'errors': errs})
                return _json(200, row)
            if method == 'DELETE':
                ok, why = (store.delete_scenario(name) if is_sc else store.delete_profile(name))
                if ok:
                    return _json(200, {'deleted': True, key: name})
                if why == 'bundled':
                    return _json(409, {'error': 'bundled_read_only', key: name,
                                       'detail': '패키지 동봉본은 지울 수 없다 — 운영자본(override)만 삭제 대상'})
                return _json(404, {'error': f'{head[:-1]}_not_found', key: name})

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
        if len(parts) == 3 and parts[2] == 'check' and method == 'POST':
            try:
                tid = int(parts[1])
            except ValueError:
                return _json(400, {'error': 'id 는 정수'})
            rec = store.get_topology(tid)
            if rec is None:
                return _json(404, {'error': 'topology_not_found', 'id': tid})
            model = store.topology_model(rec)
            if model is None:
                return _json(400, {'error': 'invalid_topology', 'id': tid})
            items = await asyncio.get_running_loop().run_in_executor(None, tester_check.check_topology, model, rec.get('doc') or {}, _bearer(handler_args))
            return _json(200, {'id': tid, 'ok': all(i['ok'] or i.get('info') for i in items), 'items': items})

    if head == 'runs':
        if len(parts) == 1 and method == 'GET':
            q = handler_args.query_params or {}
            try:
                limit = int(q.get('limit', 100))
            except ValueError:
                limit = 100
            filters = {k: q.get(k) for k in ('scenario', 'build', 'verdict', 'since', 'profile', 'label', 'topology', 'load') if q.get(k)}
            rows = store.list_runs(limit=limit, filters=filters)
            live = {d.run_id: d for d in RUNS.active()}
            for r in rows:
                if r.get('id') in live:
                    r['live'] = live[r['id']].live()
            return _json(200, {'runs': rows})
        if len(parts) == 1 and method == 'POST':
            body = _body(handler_args)
            if not isinstance(body, dict):
                return _json(400, {'error': 'body 는 RunRequest(JSON) 여야 한다'})
            req, errs = validate('run_request', body)
            if req is None:
                return _json(400, {'error': 'invalid_run_request', 'errors': errs})
            try:
                d = RUNS.start(req, _bearer(handler_args))
            except RuntimeError as e:
                return _json(409, {'error': str(e)})
            except ValueError as e:
                return _json(404 if 'not_found' in str(e) else 400, {'error': str(e)})
            return _json(202, {'id': d.run_id, 'state': d.state, 'scenario_id': d.scenario.id,
                               'topology': d.topology_name, 'profile': d.profile_name})
        if len(parts) == 2 and parts[1] == 'compare' and method == 'GET':
            q = handler_args.query_params or {}
            ids = [x for x in str(q.get('ids', '')).split(',') if x.strip()]
            if len(ids) < 2:
                return _json(400, {'error': 'ids 는 run id 둘 이상(콤마 구분)'})
            cmp = compare_runs(ids)
            fmt = str(q.get('format') or '').lower()
            if fmt in ('md', 'csv'):
                text = compare_markdown(cmp) if fmt == 'md' else compare_csv(cmp)
                return HandlerResult(response=PlainTextResponse(text, media_type=('text/markdown' if fmt == 'md' else 'text/csv') + '; charset=utf-8'))
            return _json(200, cmp)
        if len(parts) >= 2:
            rid = parts[1]
            d = RUNS.get(rid)
            if len(parts) == 2 and method == 'DELETE':
                if d is not None and d.state != 'stopped':
                    return _json(409, {'error': 'run_active', 'id': rid})
                ok = store.delete_run(rid)
                return _json(200 if ok else 404, {'deleted': ok, 'id': rid})
            if len(parts) == 3 and parts[2] == 'series' and method == 'GET':
                if store.get_run(rid) is None and d is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                try:
                    limit = int((handler_args.query_params or {}).get('limit', 7200))
                except ValueError:
                    limit = 7200
                return _json(200, {'id': rid, **run_series(rid, limit)})
            if len(parts) == 2 and method == 'GET':
                rec = store.get_run(rid)
                if rec is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                if d is not None and d.state != 'stopped':
                    rec['live'] = d.live()
                return _json(200, rec)
            if len(parts) == 3 and parts[2] == 'stop' and method == 'POST':
                if not RUNS.stop(rid):
                    return _json(404 if d is None else 409, {'error': 'run_not_running', 'id': rid})
                return _json(202, {'id': rid, 'state': 'stopping'})
            if len(parts) == 3 and parts[2] == 'rate' and method == 'POST':
                body = _body(handler_args) or {}
                try:
                    rate = float(body.get('rate_saps'))
                except (TypeError, ValueError):
                    return _json(400, {'error': 'rate_saps 필요'})
                if rate < 0:
                    return _json(400, {'error': 'rate_saps ≥ 0'})
                if not RUNS.rate(rid, rate):
                    return _json(404 if d is None else 409, {'error': 'run_not_running', 'id': rid})
                return _json(200, {'id': rid, 'rate_saps': rate})
            if len(parts) == 3 and parts[2] == 'hold' and method == 'POST':
                body = _body(handler_args) or {}
                on = bool(body.get('hold', True))
                if not RUNS.hold(rid, on):
                    return _json(404 if d is None else 409, {'error': 'run_not_running', 'id': rid})
                return _json(200, {'id': rid, 'hold': on})
            if len(parts) == 3 and parts[2] == 'hist' and method == 'GET':
                timer = str((handler_args.query_params or {}).get('timer') or 'srd_ms')
                if store.get_run(rid) is None and d is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                h = run_hist(rid, timer)
                if h is None:
                    return _json(200, {'id': rid, 'timer': timer, 'count': 0, 'buckets': []})
                return _json(200, {'id': rid, **h})
            if len(parts) == 3 and parts[2] == 'sip' and method == 'GET':
                if store.get_run(rid) is None and d is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                return _json(200, {'id': rid, 'dumps': run_sip_dumps(rid)})
            if len(parts) == 4 and parts[2] == 'sip' and method == 'GET':
                call_id = parts[3]
                if store.get_run(rid) is None and d is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                evs = run_call_events(rid, call_id)
                dump = None
                p = os.path.join(store.run_dir(rid), 'sip', store._safe_name(call_id) + '.log')
                if os.path.isfile(p):
                    with open(p, 'r', encoding='utf-8', errors='replace') as f:
                        dump = f.read(512 * 1024)
                if not evs and dump is None:
                    return _json(404, {'error': 'call_not_found', 'id': rid, 'call_id': call_id})
                return _json(200, {'id': rid, 'call_id': call_id, 'events': evs, 'dump': dump,
                                   'note': None if dump is not None else '이 호의 SIP 덤프가 없다 — 워커 Sip.Capture 가 failed 면 실패한 인스턴스의 호만 올린다'})
            if len(parts) == 3 and parts[2] == 'target-series' and method == 'GET':
                if store.get_run(rid) is None and d is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                return _json(200, {'id': rid, **tester_observe.target_series(os.path.join(store.run_dir(rid), 'metrics.sqlite'))})
            if len(parts) == 3 and parts[2] == 'target-alerts' and method == 'GET':
                rec = _load_run_doc(rid)
                if rec is None:
                    return _json(404, {'error': 'run_not_found', 'id': rid})
                out = await asyncio.get_running_loop().run_in_executor(None, target_alerts, rec, _bearer(handler_args))
                return _json(200, {'id': rid, **out})
            if len(parts) == 3 and parts[2] == 'stream' and method == 'GET':
                return _sse(rid)
            if len(parts) == 3 and parts[2] == 'report' and method == 'GET':
                p = os.path.join(store.run_dir(rid), 'run.json')
                if not os.path.isfile(p):
                    if d is not None:
                        return _json(200, {'run': d.live(), 'markdown': None, 'final': False})
                    return _json(404, {'error': 'report_not_found', 'id': rid})
                with open(p, 'r', encoding='utf-8') as f:
                    doc = json.load(f)
                return _json(200, {'run': doc, 'markdown': report_markdown(doc), 'final': True})
            if len(parts) == 3 and parts[2] == 'events' and method == 'GET':
                try:
                    limit = int((handler_args.query_params or {}).get('limit', 200))
                except ValueError:
                    limit = 200
                p = os.path.join(store.run_dir(rid), 'events.jsonl')
                rows = []
                if os.path.isfile(p):
                    with open(p, 'r', encoding='utf-8') as f:
                        lines = f.readlines()[-max(1, limit):]
                    for ln in lines:
                        try:
                            rows.append(json.loads(ln))
                        except Exception:
                            pass
                return _json(200, {'events': rows})

    if head == 'workers' and len(parts) == 2 and parts[1] == 'discovered' and method == 'GET':
        if not _BASE_OAM_URL:
            return _json(200, {'items': [], 'note': 'Tester.BaseOamUrl 이 비어 있다 — 자기 base OAM 주소(예: https://127.0.0.1:4445)를 배포 설정에 준다'})
        tok = _bearer(handler_args)
        try:
            items = await asyncio.get_running_loop().run_in_executor(
                None, tester_workers.discover_deployed, _BASE_OAM_URL, tok or '')
        except Exception as e:
            return _json(200, {'items': [], 'note': f'base OAM 조회 실패 — {e}'})
        return _json(200, {'items': items, 'note': None})
    if head == 'workers' and method == 'GET':
        q = handler_args.query_params or {}
        topos = store.list_topologies()
        if q.get('topology'):
            topos = [t for t in topos if str(t.get('id')) == str(q.get('topology'))]
        out = []
        for t in topos:
            ws = tester_workers.discover(t.get('doc') or {})
            await asyncio.get_running_loop().run_in_executor(None, tester_workers.probe_all, ws)
            for w in ws:
                row = w.to_dict()
                row['topology_id'] = t.get('id')
                out.append(row)
        return _json(200, {'workers': out})

    return _json(404, {'error': 'not_found', 'path': handler_args.full_path})


def scenario_vocab() -> dict:
    """GET /scenarios/vocab — 편집기 팔레트·속성 폼·kind 게이트의 정본(tester_models 어휘 표)."""
    return {
        'steps': {k: {**v, 'supported': k in WORKER_STEPS} for k, v in STEP_VOCAB.items()},
        'real_ue_steps': sorted(REAL_UE_STEPS),
        'groups': STEP_GROUPS,
        'worker_steps': sorted(WORKER_STEPS),
        'during_steps': list(DURING_STEPS),
        'metrics': {m: METRIC_LABELS.get(m, m) for m in METRIC_NAMES},
        'pct_metrics': [m for m in METRIC_NAMES if m.endswith('_pct')],
        'ratio_metrics': {k: list(v) for k, v in RATIO_METRICS.items()},
        'thresholds': ['p50', 'p95', 'p99', 'max', 'min'],
        'q850': {str(k): v for k, v in Q850_CAUSES.items()},
        'audio': list(AUDIO_CODECS), 'video': list(VIDEO_CODECS),
        'rtp_modes': list(RTP_MODES), 'sample_codecs': list(SAMPLE_CODECS),
        'evidence_kinds': ['recording_created', 'log_errors', 'alarm_raised', 'event_logged', 'rss_growth_mb', 'fd_growth'],
        'profile_models': ['constant', 'step', 'ramp', 'soak', 'burst'],
        'pool_kinds': ['ue', 'peer', 'real-ue'], 'peer_profiles': ['ibcf', 'pbx', 'mgcf'],
        'transports': ['udp', 'tcp', 'tls'], 'srtp': ['off', 'optional', 'required'],
        'node_roles': ['sip', 'tas', 'media', 'subscriber', 'oam', 'db'], 'target_kinds': ['cims', 'ims', 'pbx'],
        'phases': {'prelude': '앞쪽 register/wait — run 시작 때 역할 단말 전부 등록', 'body': '시나리오 인스턴스 단위',
                   'epilogue': '끝의 deregister — run 종료 시'},
    }


async def _plan(body: dict, config: dict):
    """compile-check / runs/plan 공용 — 시나리오는 id 또는 문서(doc|yaml, 미저장 편집본), 토폴로지는 id 또는 name."""
    scenario = None
    if body.get('scenario_id'):
        scenario, sdoc, errs = store.get_scenario(str(body['scenario_id']))
        if sdoc is None:
            return 404, {'error': 'scenario_not_found', 'scenario_id': body['scenario_id']}
        if scenario is None:
            return 400, {'error': 'invalid_scenario', 'errors': errs}
    else:
        doc = body.get('doc')
        if doc is None and isinstance(body.get('yaml'), str):
            try:
                doc = yaml.safe_load(body['yaml'])
            except Exception as e:
                return 200, {'ok': False, 'errors': [f'YAML 파싱 실패: {e}'], 'warnings': [], 'notes': []}
        if not isinstance(doc, dict):
            return 400, {'error': 'scenario_id 또는 doc|yaml 이 필요하다'}
        scenario, errs = validate('scenario', doc)
        if scenario is None:
            return 200, {'ok': False, 'errors': errs, 'warnings': [], 'notes': []}
    topo_rec = store.find_topology(body.get('topology_id') if body.get('topology_id') is not None else body.get('topology'))
    if topo_rec is None:
        return 404, {'error': 'topology_not_found'}
    topology = store.topology_model(topo_rec)
    if topology is None:
        return 400, {'error': 'invalid_topology', 'id': topo_rec.get('id')}
    profile = None
    if body.get('profile'):
        profile, pdoc, perrs = store.get_profile(str(body['profile']))
        if pdoc is None:
            return 404, {'error': 'profile_not_found', 'profile': body['profile']}
        if profile is None:
            return 400, {'error': 'invalid_profile', 'errors': perrs}
    bindings = body.get('bindings') if isinstance(body.get('bindings'), dict) else {}
    try:
        instances = int(body['instances']) if body.get('instances') is not None else None
        rate = float(body['rate_saps']) if body.get('rate_saps') is not None else None
    except (TypeError, ValueError):
        return 400, {'error': 'instances 는 정수, rate_saps 는 수'}
    probe = body.get('probe', True) not in (False, 0, '0', 'false')
    stream_port = int(((config.get('Tester') or {}).get('WorkerStreamPort')) or 7110)
    out = await asyncio.get_running_loop().run_in_executor(
        None, lambda: tester_plan.build_plan(scenario, topology, topo_rec.get('doc') or {}, profile, bindings, instances, rate, probe, stream_port))
    out['topology_id'] = topo_rec.get('id')
    out['active_runs'] = [d.run_id for d in RUNS.active()]
    return 200, out


def target_alerts(run_doc: dict, requester_token: Optional[str] = None) -> dict:
    """대상 OAM `/api/v1/alerts` 를 run 창(started_at~ended_at)으로 잘라 — 시간축 알람 레인. 대상 oam 노드가 없으면 빈 목록 + note."""
    topo_rec = store.find_topology(run_doc.get('topology'))
    topology = store.topology_model(topo_rec) if topo_rec else None
    oam = topology.oam_ref() if topology else None
    if oam is None:
        return {'alerts': [], 'note': '대상 oam 노드가 없다(또는 토폴로지 삭제됨) — 알람 타임라인 없음'}
    try:
        client = tester_target.OamClient(oam.url, tester_target.resolve_token(oam, requester_token))
        st, out = client._req('GET', '/api/v1/alerts?days=7&limit=2000')
        if st != 200:
            return {'alerts': [], 'note': f'대상 OAM alerts {st}'}
    except tester_target.TargetError as e:
        return {'alerts': [], 'note': str(e)}
    a, b = str(run_doc.get('started_at') or ''), str(run_doc.get('ended_at') or '9999')
    rows = []
    for ev in (out.get('events') if isinstance(out, dict) else out) or []:
        ts = str(ev.get('ts') or ev.get('time') or '')
        if a[:19] <= ts[:19] <= b[:19]:
            rows.append(ev)
    return {'alerts': rows, 'window': [a, b], 'oam': oam.url}


def compare_markdown(cmp: dict) -> str:
    runs = cmp['runs']
    lines = [f"# run 비교 — 기준 {cmp['baseline']}", '',
             '| run | 판정 | 대상 빌드 | 시나리오 | 프로파일 | 시작 |', '|---|---|---|---|---|---|']
    for r in runs:
        lines.append(f"| {r['id']} | {r.get('verdict') or '-'} | {r.get('target_build') or '-'} | {r.get('scenario_id') or '-'} | "
                     f"{r.get('profile') or '(단발)'} | {r.get('started_at') or '-'} |")
    lines += ['', '| 지표 | 방향 | ' + ' | '.join(r['id'] for r in runs) + ' |', '|---|---|' + '---|' * len(runs)]
    for m in cmp['metrics']:
        cells = []
        for v, d, reg in zip(m['values'], m['delta'], m['regression']):
            if v is None:
                cells.append('-')
            else:
                cells.append(f"{v:.2f}" + (f" ({d:+.2f}{' ⚠' if reg else ''})" if d is not None and d != 0 else ''))
        lines.append(f"| {m['metric']} | {'↑' if m['direction'] == 'up' else '↓'} | " + ' | '.join(cells) + ' |')
    lines += ['', f"회귀 {cmp['regressions']} 건" + ('' if cmp['same_scenario'] else ' · 시나리오가 다르다(비교 주의)')]
    return '\n'.join(lines) + '\n'


def compare_csv(cmp: dict) -> str:
    runs = cmp['runs']
    lines = ['metric,direction,' + ','.join(r['id'] for r in runs) + ',' + ','.join(f"delta_{r['id']}" for r in runs[1:])]
    for m in cmp['metrics']:
        vals = ['' if v is None else f'{v:.4f}' for v in m['values']]
        deltas = ['' if d is None else f'{d:+.4f}' for d in m['delta'][1:]]
        lines.append(f"{m['metric']},{m['direction']}," + ','.join(vals) + ',' + ','.join(deltas))
    return '\n'.join(lines) + '\n'


def report_markdown(doc: dict) -> str:
    """run.json → Markdown 보고서 (RFC 6076 표·expect 판정·단계 로그). 콘솔 인쇄·CLI 출력 공용."""
    s = doc.get('summary') or {}
    t = doc.get('timers') or {}
    lines = [f"# 계측기 run {doc.get('id')} — {doc.get('scenario_id')}",
             '',
             f"- 판정: **{doc.get('verdict')}**  · 토폴로지 {doc.get('topology')} · 프로파일 {doc.get('profile') or '(단발)'}",
             f"- 시작 {doc.get('started_at')} · 종료 {doc.get('ended_at')} · 워커 {', '.join(doc.get('workers') or [])}",
             '']
    if doc.get('stop_reason'):
        lines.append(f"- 중단 사유: {doc['stop_reason']}")
    if doc.get('notes'):
        lines += [f'- 참고: {n}' for n in doc['notes']]
    lines += ['', '## RFC 6076 지표', '', '| 지표 | 값 |', '|---|---|']

    def fmt(v):
        if v is None:
            return '-'
        if isinstance(v, float):
            return f'{v:.2f}'
        return str(v)
    for k, label in (('attempts', '호 시도(attempt)'), ('sessions', '세션(성립)'), ('completed', '완료(정상 BYE)'),
                     ('failed', '실패'), ('skipped', '단말 부족으로 건너뜀'), ('ser_pct', 'SER %'), ('scr_pct', 'SCR %'),
                     ('registered_ok', '등록 성공'), ('registered_fail', '등록 실패'), ('doc_saps', 'DOC (SApS)'),
                     ('rtp_rx', 'RTP 수신'), ('rtp_lost', 'RTP 손실'), ('rtp_loss_pct', 'RTP 손실 %'), ('codes', '응답 코드')):
        lines.append(f'| {label} | {fmt(s.get(k))} |')
    lines += ['', '| 지연 | n | p50 | p95 | p99 | max |', '|---|---|---|---|---|---|']
    for name, label in (('rrd_ms', 'RRD ms'), ('srd_ms', 'SRD ms'), ('sdd_ms', 'SDD ms'), ('sdt_s', 'SDT s'), ('jitter_ms', '지터 ms'),
                        ('rtp_loss_pct', 'RTP 손실 %(호별)'), ('affiliate_ms', 'Affiliation ms'), ('group_fanout_ms', '그룹 fan-out ms'),
                        ('floor_grant_ms', 'Floor grant ms'), ('floor_taken_ms', 'Floor taken ms'), ('floor_queue_ms', 'Floor 큐 대기 ms'),
                        ('floor_idle_ms', 'Floor idle ms'), ('real_srd_ms', '실단말 SRD ms'), ('real_jitter_ms', '실단말 지터 ms'),
                        ('real_rtp_loss_pct', '실단말 RTP 손실 %(호별)'), ('real_mos', '실단말 MOS 추정')):
        h = t.get(name)
        if h:
            lines.append(f"| {label} | {h.get('count')} | {fmt(h.get('p50'))} | {fmt(h.get('p95'))} | {fmt(h.get('p99'))} | {fmt(h.get('max'))} |")
    er = doc.get('expect_results') or []
    if er:
        lines += ['', '## 기대치 판정', '', '| 단계 | 지표 | 기대 | 관측 | 판정 |', '|---|---|---|---|---|']
        for r in er:
            lines.append(f"| {r.get('step')} {r.get('kind')} | {r.get('metric')} | {json.dumps(r.get('expect'), ensure_ascii=False)} | "
                         f"{json.dumps(r.get('observed'), ensure_ascii=False)} | {'PASS' if r.get('ok') else 'FAIL'} |")
    sl = doc.get('step_log') or []
    if len(sl) > 1:
        lines += ['', '## 단계 로그', '', '| 시각 | 율(SApS) | 시도 | IHS % |', '|---|---|---|---|']
        for r in sl:
            lines.append(f"| {datetime.fromtimestamp(r['t']).strftime('%H:%M:%S')} | {fmt(r.get('rate'))} | {fmt(r.get('attempts'))} | {fmt(r.get('ihs_pct'))} |")
    return '\n'.join(lines) + '\n'


# 비교 축 — (지표, 방향) : 'up' = 클수록 좋다, 'down' = 작을수록 좋다. 허용 오차 = 비율 지표 0.5 포인트, 나머지 5 %.
COMPARE_METRICS = (('ser_pct', 'up'), ('scr_pct', 'up'), ('doc_saps', 'up'),
                   ('rrd_ms_p95', 'down'), ('srd_ms_p95', 'down'), ('sdd_ms_p95', 'down'),
                   ('jitter_ms_p95', 'down'), ('rtp_loss_pct', 'down'),
                   ('early_media_pct', 'up'), ('prack_pct', 'up'), ('hold_resume_pct', 'up'), ('refer_pct', 'up'),
                   ('floor_grant_pct', 'up'), ('floor_grant_ms_p95', 'down'), ('floor_taken_ms_p95', 'down'), ('group_fanout_ms_p95', 'down'))


def _load_run_doc(rid: str) -> Optional[dict]:
    p = os.path.join(store.run_dir(rid), 'run.json')
    if os.path.isfile(p):
        with open(p, 'r', encoding='utf-8') as f:
            return json.load(f)
    rec = store.get_run(rid)
    return dict(rec) if rec else None


def compare_runs(ids: List[str]) -> dict:
    """첫 id 가 기준(baseline). 지표별 {metric, direction, base, values[], delta[], regression[]} 와 run 요약을 돌려준다.
    회귀 = 방향 기준으로 나빠진 폭이 허용 오차를 넘음. 없는 지표는 비교에서 빠진다(None)."""
    docs = []
    for rid in ids:
        d = _load_run_doc(rid)
        docs.append(d if d is not None else {'id': rid, 'missing': True})
    runs = [{'id': d.get('id'), 'missing': d.get('missing', False), 'scenario_id': d.get('scenario_id'),
             'topology': d.get('topology'), 'profile': d.get('profile'), 'started_at': d.get('started_at'),
             'ended_at': d.get('ended_at'), 'verdict': d.get('verdict'), 'target_build': d.get('target_build'),
             'label': d.get('label'), 'summary': d.get('summary') or {}, 'timers': d.get('timers') or {},
             'expect_results': d.get('expect_results') or [], 'stop_reason': d.get('stop_reason')} for d in docs]
    metrics = []
    base = runs[0]['summary']
    for name, direction in COMPARE_METRICS:
        vals = [r['summary'].get(name) for r in runs]
        if all(v is None for v in vals):
            continue
        b = base.get(name)
        deltas, regs = [], []
        for v in vals:
            if b is None or v is None or not isinstance(v, (int, float)) or not isinstance(b, (int, float)):
                deltas.append(None); regs.append(None); continue
            delta = float(v) - float(b)
            deltas.append(delta)
            worse = delta < 0 if direction == 'up' else delta > 0
            tol = 0.5 if name.endswith('_pct') else abs(float(b)) * 0.05
            regs.append(bool(worse and abs(delta) > tol))
        metrics.append({'metric': name, 'direction': direction, 'base': b, 'values': vals, 'delta': deltas, 'regression': regs})
    same_scenario = len({r['scenario_id'] for r in runs if not r['missing']}) <= 1
    return {'baseline': ids[0], 'runs': runs, 'metrics': metrics, 'same_scenario': same_scenario,
            'regressions': sum(1 for m in metrics for x in m['regression'][1:] if x)}


# ── API 자기기술 (api_docs.md) — base /api/v1/api-docs 가 게이트웨이 라우트의 업스트림에서 수집한다 ─────────
_AUTH_MON = {'scheme': 'Bearer JWT', 'role': 'monitor', 'token_from': 'base OAM 로그인 — 공유 CimsAuth.JwtSecret 로 모듈이 독립 검증'}
_AUTH_OP = {**_AUTH_MON, 'role': 'operator'}
_AUTH_MGR = {**_AUTH_MON, 'role': 'manager'}
_MOD = 'oam-cims-tester'
_P = _BASE

TESTER_API_DOCS = [
    {'id': 'tester.health', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/health',
     'summary': '계측기 컨트롤러 상태 — 버전·데이터 디렉터리·색인 수·진행 중 run·스트림 구독자·이행 단계',
     'response': '{module, version, data_dir, scenarios, profiles, topologies, runs, active_runs[], stream_subscribers, worker_stream, phase}',
     'auth': _AUTH_MON},
    {'id': 'tester.events', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/events',
     'summary': 'SSE(text/event-stream) — run 상태(runs)·1초 집계(agg)·실패 이벤트(events) 라이브. 게이트웨이가 청크 통과',
     'response': 'data: {stream: hello|runs|agg|events, record}',
     'notes': ['EventSource 는 Authorization 헤더를 못 붙이므로 콘솔은 fetch+ReadableStream 으로 읽는다', '20 초 무소식이면 `: ping`'],
     'auth': _AUTH_MON},
    {'id': 'tester.scenarios', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/scenarios',
     'summary': '시나리오 목록 — 패키지 동봉 + 운영자본(같은 id 면 운영자본이 이김). 검증 오류가 있는 파일도 errors 와 함께',
     'response': '{scenarios[]: {id, title, tags[], steps, source(bundled|user), path, errors[]}}', 'auth': _AUTH_MON},
    {'id': 'tester.scenario', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/scenarios/{{id}}',
     'summary': '시나리오 상세 — 파싱된 doc + YAML 원문(편집기)', 'response': '{id, doc, yaml, source, errors[], valid}', 'auth': _AUTH_MON},
    {'id': 'tester.scenario.put', 'module': _MOD, 'method': 'PUT', 'path': f'{_P}/scenarios/{{id}}',
     'summary': '운영자본 저장 — 검증 통과분만, 문서 id 는 경로 id 와 일치',
     'params': [{'name': 'yaml', 'in': 'body', 'type': 'string', 'required': True, 'desc': 'YAML 원문'}],
     'response': '목록 행 {id, title, tags[], steps, source:user, path, errors:[]}',
     'errors': [{'status': 400, 'when': '검증 실패', 'body': {'error': 'invalid_scenario', 'errors': ['flow.0.step: …']}}], 'auth': _AUTH_OP},
    {'id': 'tester.scenario.delete', 'module': _MOD, 'method': 'DELETE', 'path': f'{_P}/scenarios/{{id}}',
     'summary': '운영자본 삭제 — 동봉본은 409 bundled_read_only', 'auth': _AUTH_MGR},
    {'id': 'tester.profiles', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/profiles',
     'summary': '부하 프로파일 목록(ETSI TS 186 008 constant/step/ramp/soak/burst)', 'response': '{profiles[]: {name, model, source, path, errors[]}}', 'auth': _AUTH_MON},
    {'id': 'tester.profile', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/profiles/{{name}}',
     'summary': '프로파일 상세(doc + YAML 원문). PUT/DELETE 규약은 시나리오와 같다', 'response': '{name, doc, yaml, source, errors[], valid}', 'auth': _AUTH_MON},
    {'id': 'tester.validate', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/validate',
     'summary': '문서 검증만(저장 없음) — 편집기가 타이핑 중 호출',
     'params': [{'name': 'kind', 'in': 'body', 'type': 'string', 'required': True, 'enum': ['scenario', 'profile', 'topology', 'run_request']},
                {'name': 'yaml', 'in': 'body', 'type': 'string', 'desc': 'doc 대신 YAML 원문'},
                {'name': 'doc', 'in': 'body', 'type': 'object'}],
     'response': '{ok, errors[], doc}', 'auth': _AUTH_OP},
    {'id': 'tester.topologies', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/topologies',
     'summary': '토폴로지(호스트›워커·대상 노드›풀) 레코드 목록 — 런타임 store. 이전 꼴(target.csp/workers[].url) 레코드는 읽을 때 v2 로 승계', 'response': '{topologies[]: {id, name, created_at, updated_at, doc, migrated_from?}}', 'auth': _AUTH_MON},
    {'id': 'tester.topology.save', 'module': _MOD, 'method': 'PUT', 'path': f'{_P}/topologies/{{id}}',
     'summary': '토폴로지 저장(POST /topologies 는 생성) — 검증 통과분만',
     'errors': [{'status': 400, 'when': '검증 실패', 'body': {'error': 'invalid_topology', 'errors': ['…']}}], 'auth': _AUTH_OP},
    {'id': 'tester.topology.check', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/topologies/{{id}}/check',
     'summary': '연결 검사 — 노드별 수신점(`<노드>:<수신점 id>` SIP · `<노드>:api|db|oam|control`)·`<호스트>:ssh`·`worker_<이름>` health',
     'response': '{id, ok, items[]: {name, ok, detail, ms, info?, target{kind,id}}}',
     'notes': ['피어링 접속점은 run 중에만 열리므로 평상시 미도달은 info(참고) 로 표시'], 'auth': _AUTH_OP},
    {'id': 'tester.workers', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/workers',
     'summary': '토폴로지의 워커 + GET /health 결과(용량·cpu·진행 run·시계 오차)',
     'params': [{'name': 'topology', 'in': 'query', 'type': 'integer', 'desc': '토폴로지 id 로 한정'}],
     'response': '{workers[]: {name, url, host, cpus, media, up, health{max_endpoints,max_saps,cpu_pct,active_endpoints,active_run,clock_skew_ms,media{rtp_streams,max_rtp_streams}?,pools[]}, error, topology_id}}', 'auth': _AUTH_MON},
    {'id': 'tester.runs', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs',
     'summary': 'run 색인(최신순) — 진행 중이면 live 누계 포함',
     'params': [{'name': 'limit', 'in': 'query', 'type': 'integer', 'desc': '기본 100'},
                {'name': 'scenario', 'in': 'query', 'type': 'string'}, {'name': 'build', 'in': 'query', 'type': 'string', 'desc': 'target_build 정확 일치'},
                {'name': 'verdict', 'in': 'query', 'type': 'string', 'desc': 'pass,fail,aborted,error,running 콤마 구분'},
                {'name': 'since', 'in': 'query', 'type': 'string', 'desc': 'ISO 시각 또는 7d'}, {'name': 'profile', 'in': 'query', 'type': 'string'},
                {'name': 'label', 'in': 'query', 'type': 'string', 'desc': '라벨/id/시나리오 부분 일치'}, {'name': 'topology', 'in': 'query', 'type': 'string'},
                {'name': 'load', 'in': 'query', 'type': 'boolean', 'desc': '1 = 부하(프로파일) run 만'}],
     'response': '{runs[]: RunRecord{id, scenario_id, topology, profile, started_at, ended_at, verdict, workers[], summary{}, target_build, label, stop_reason, live?}}', 'auth': _AUTH_MON},
    {'id': 'tester.run.start', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/runs',
     'summary': 'run 시작(RunRequest) → 202. 동시에 하나만(409)',
     'params': [{'name': 'scenario_id', 'in': 'body', 'type': 'string', 'required': True},
                {'name': 'topology_id', 'in': 'body', 'type': 'integer', 'desc': 'topology(name) 와 둘 중 하나'},
                {'name': 'profile', 'in': 'body', 'type': 'string', 'desc': '없으면 단발(기능) 실행'},
                {'name': 'instances', 'in': 'body', 'type': 'integer'}, {'name': 'rate_saps', 'in': 'body', 'type': 'number'},
                {'name': 'bindings', 'in': 'body', 'type': 'object', 'desc': '${ht} 등'}, {'name': 'label', 'in': 'body', 'type': 'string'}],
     'response': '{id, state, scenario_id, topology, profile}', 'auth': _AUTH_OP},
    {'id': 'tester.run', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}',
     'summary': 'run 색인 1건(+ 진행 중 live)', 'auth': _AUTH_MON},
    {'id': 'tester.run.stop', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/runs/{{id}}/stop',
     'summary': '중단 — 워커 drain 뒤 verdict=aborted', 'auth': _AUTH_OP},
    {'id': 'tester.run.rate', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/runs/{{id}}/rate',
     'summary': '진행 중 시도율 변경', 'params': [{'name': 'rate_saps', 'in': 'body', 'type': 'number', 'required': True}], 'auth': _AUTH_OP},
    {'id': 'tester.run.report', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/report',
     'summary': 'run.json 전체(RFC 6076 요약·타이머 분포·기대치 판정·단계 로그·plan) + Markdown', 'response': '{run, markdown, final}', 'auth': _AUTH_MON},
    {'id': 'tester.run.events', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/events',
     'summary': '실패 개별 건(events.jsonl 꼬리)', 'params': [{'name': 'limit', 'in': 'query', 'type': 'integer', 'desc': '기본 200'}], 'auth': _AUTH_MON},
    {'id': 'tester.run.series', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/series',
     'summary': '1초 시계열(열 형태) — 결과 화면의 시간축 차트', 'response': '{id, t[], counters{k:[]}, gauges{k:[]}, timers{k:{p95[],count[]}}}', 'auth': _AUTH_MON},
    {'id': 'tester.run.stream', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/stream',
     'summary': 'SSE — 이 run 의 프레임만', 'auth': _AUTH_MON},
    {'id': 'tester.run.delete', 'module': _MOD, 'method': 'DELETE', 'path': f'{_P}/runs/{{id}}',
     'summary': 'run 색인·본체 삭제(진행 중 409)', 'auth': _AUTH_MGR},
    {'id': 'tester.runs.compare', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/compare',
     'summary': 'run 비교 — 첫 id 기준 지표별 delta·회귀 판정(비율 지표 0.5 pt / 그 외 5 % 허용), target_build 병기. format=md|csv 는 텍스트',
     'params': [{'name': 'ids', 'in': 'query', 'type': 'string', 'required': True, 'desc': 'run id 콤마 구분(2개 이상)'},
                {'name': 'format', 'in': 'query', 'type': 'string', 'enum': ['md', 'csv'], 'desc': '없으면 JSON'}],
     'response': '{baseline, runs[], metrics[]: {metric, direction, base, values[], delta[], regression[]}, same_scenario, regressions}', 'auth': _AUTH_MON},
    {'id': 'tester.scenarios.vocab', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/scenarios/vocab',
     'summary': '단계 어휘 표(STEP_VOCAB: group·actor·kind 게이트·제안 지표·워커 지원)·지표 라벨·Q.850·코덱 — 시나리오 편집기 팔레트/폼의 정본',
     'response': '{steps{}, groups[], worker_steps[], during_steps[], metrics{}, pct_metrics[], ratio_metrics{}, thresholds[], q850{}, audio[], video[], …}', 'auth': _AUTH_MON},
    {'id': 'tester.scenarios.compile_check', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/scenarios/compile-check',
     'summary': '토폴로지 적합성 — compile_run 드라이런(저장 없는 편집본도). runs/plan 과 같은 함수',
     'params': [{'name': 'scenario_id', 'in': 'body', 'type': 'string', 'desc': 'doc|yaml 과 둘 중 하나'},
                {'name': 'doc', 'in': 'body', 'type': 'object'}, {'name': 'yaml', 'in': 'body', 'type': 'string'},
                {'name': 'topology_id', 'in': 'body', 'type': 'integer', 'desc': 'topology(name) 와 둘 중 하나'},
                {'name': 'profile', 'in': 'body', 'type': 'string'}, {'name': 'bindings', 'in': 'body', 'type': 'object'},
                {'name': 'instances', 'in': 'body', 'type': 'integer'}, {'name': 'rate_saps', 'in': 'body', 'type': 'number'},
                {'name': 'probe', 'in': 'body', 'type': 'boolean', 'desc': '워커 health 조회(기본 true)'}],
     'response': '{ok, errors[], warnings[], notes[], roles{}, workers[], steps[], phases{}, procedure[], seed[], env[], little{}, estimate{}}', 'auth': _AUTH_OP},
    {'id': 'tester.runs.plan', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/runs/plan',
     'summary': 'run 시작 창의 계획 미리보기 — compile-check 와 같은 입력·출력(역할→풀→워커 창, 용량, 시드, Little 검산, 예상 소요)', 'auth': _AUTH_OP},
    {'id': 'tester.run.hold', 'module': _MOD, 'method': 'POST', 'path': f'{_P}/runs/{{id}}/hold',
     'summary': '단계 고정/재개 — 프로파일 시계를 멈추고 율은 유지(step 의 현 단계를 오래 본다)',
     'params': [{'name': 'hold', 'in': 'body', 'type': 'boolean', 'required': True}], 'auth': _AUTH_OP},
    {'id': 'tester.run.hist', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/hist',
     'summary': '지연 지표 버킷 분포(로그 상한 1·2·5·…·60000 ms) + p50/p95/p99 — 지연 분포 표의 행 펼침 히스토그램',
     'params': [{'name': 'timer', 'in': 'query', 'type': 'string', 'desc': 'rrd_ms|srd_ms|sdd_ms|jitter_ms|sdt_s|floor_grant_ms|floor_taken_ms|floor_idle_ms|group_fanout_ms … (기본 srd_ms)'}],
     'response': '{id, timer, count, mean, min, max, p50, p95, p99, buckets[]: {ub, count}}', 'auth': _AUTH_MON},
    {'id': 'tester.run.sips', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/sip',
     'summary': '워커가 올린 SIP 덤프 목록 — call_id·bytes·messages (워커 Sip.Capture: failed=실패한 인스턴스만 · all=전부)',
     'response': '{id, dumps: [{call_id, bytes, messages}]}', 'auth': _AUTH_MON},
    {'id': 'tester.run.sip', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/sip/{{call_id}}',
     'summary': 'Call-ID 하나의 실패 이벤트 + SIP 덤프(계측기 호스트 runs/<id>/sip/<call_id>.log 가 있을 때)',
     'response': '{id, call_id, events[], dump|null, note}', 'auth': _AUTH_MON},
    {'id': 'tester.workers.discovered', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/workers/discovered',
     'summary': '자기 base OAM 의 배포 목록에서 찾은 cims-tester-worker — 토폴로지 편집기의 "발견된 워커"(주소 = agent ip, 포트 = 배포 설정 Server.Port)',
     'response': '{items: [{name, agent_id, hostname, ip, port, cpus, version, live_state, deployment_id}], note}', 'auth': _AUTH_MON},
    {'id': 'tester.run.target_series', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/target-series',
     'summary': 'run 동안 모은 대상 자원 시계열 — 호스트(대상 OAM agent heartbeat 또는 hosts.*.ssh 의 /proc: cpu_pct·mem_pct) + 프로세스별(SSH: cpu_pct·rss_mb), stop_on.target_cpu_pct 의 원천',
     'response': '{id, agents: {이름: {t[], cpu_pct[], mem_pct[]}}, procs: {"호스트/프로세스": {t[], cpu_pct[], rss_mb[]}}}', 'auth': _AUTH_MON},
    {'id': 'tester.run.target_alerts', 'module': _MOD, 'method': 'GET', 'path': f'{_P}/runs/{{id}}/target-alerts',
     'summary': '대상 OAM 알람/이벤트를 run 창(started_at~ended_at)으로 잘라 — 시간축 차트의 알람 레인',
     'response': '{id, alerts[], window[], oam | note}', 'auth': _AUTH_MON},
]


async def handle_api_docs(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """모듈 자기기술 — base 의 api_docs._fetch_remote 가 `<upstream>/api/v1/api-docs` 로 가져간다(oam-svc 와 같은 규약)."""
    if (handler_args.method or 'GET').upper() != 'GET':
        return _json(405, {'error': 'method_not_allowed'})
    _, err = require_role(handler_args, 'monitor')
    if err:
        return err
    return _json(200, {'modules': [_MOD], 'count': len(TESTER_API_DOCS), 'apis': TESTER_API_DOCS})


TESTER_HANDLER_LIST = [
    (_BASE, handle_tester, {}),
    ('/api/v1/api-docs', handle_api_docs, {}),
]
