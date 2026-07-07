"""
External System Registry — 외부 시스템(외부 DB / 모니터링 / 스토리지 등) 등록 CRUD + 라이브니스 probe.

시스템/인프라에 등록한 외부 시스템을 대시보드 시스템 형상 위젯에 표시하기 위한 메타데이터 저장소.
agent/HA 모델과 분리 — 신규 DB 테이블 없이 file_store 컬렉션(domain 'external_systems')에 1레코드=1json.

Routes (mounted at /api/v1/external-systems):
  GET    /api/v1/external-systems            목록 {systems:[...]}
  POST   /api/v1/external-systems            생성 (201 {id})
  GET    /api/v1/external-systems/status     enabled 전체 동시 probe {items:[{id,status,latency_ms}]}
  GET    /api/v1/external-systems/{id}       1건
  PUT    /api/v1/external-systems/{id}       수정
  DELETE /api/v1/external-systems/{id}       삭제
  POST   /api/v1/external-systems/{id}/probe 즉시 probe {id,status,latency_ms,checked_at}

probe.mode: tcp(구현) / http,icmp(예약→unknown) / none. TCP probe 는 socket.create_connection.
"""
from urllib.parse import urlparse, unquote
from pathlib import PurePath
from datetime import datetime
import asyncio
import socket
import time
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store


_BASE = '/api/v1/external-systems'
_DOMAIN = 'external_systems'

_VALID_TYPES = {'db', 'monitoring', 'storage', 'auth', 'other'}
_VALID_PROBE_MODES = {'none', 'tcp', 'http', 'icmp'}


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _body_dict(handler_args: HandlerArgs):
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


def _dir(config):
    return file_store.domain_dir(config, _DOMAIN)


def _coerce_port(v):
    try:
        p = int(v)
        return p if 1 <= p <= 65535 else None
    except (TypeError, ValueError):
        return None


def _normalize_endpoints(raw) -> list:
    """[{host,port,label?}] 검증. port 1~65535, host 비어있지 않음."""
    out = []
    if not isinstance(raw, list):
        return out
    for e in raw:
        if not isinstance(e, dict):
            continue
        host = str(e.get('host', '')).strip()
        port = _coerce_port(e.get('port'))
        if not host or port is None:
            continue
        ep = {'host': host, 'port': port}
        label = str(e.get('label', '')).strip()
        if label:
            ep['label'] = label
        out.append(ep)
    return out


def _normalize_probe(raw, endpoints) -> dict:
    """probe 설정 검증·기본값. host/port 미지정 시 endpoints[0] fallback."""
    if not isinstance(raw, dict):
        raw = {}
    mode = str(raw.get('mode', 'none')).strip().lower()
    if mode not in _VALID_PROBE_MODES:
        mode = 'none'
    out = {'mode': mode}
    host = str(raw.get('host', '')).strip()
    port = _coerce_port(raw.get('port'))
    if (not host or port is None) and endpoints:
        host = host or endpoints[0].get('host', '')
        if port is None:
            port = endpoints[0].get('port')
    if host:
        out['host'] = host
    if port is not None:
        out['port'] = port
    url = str(raw.get('url', '')).strip()
    if url:
        out['url'] = url
    try:
        to = float(raw.get('timeout', 2))
        out['timeout'] = min(max(to, 1), 10)
    except (TypeError, ValueError):
        out['timeout'] = 2
    return out


def _normalize_record(body: dict, *, sid=None) -> dict:
    """입력 → 검증된 external_system 레코드. 잘못된 값은 기본값."""
    endpoints = _normalize_endpoints(body.get('endpoints'))
    typ = str(body.get('type', 'other')).strip().lower()
    if typ not in _VALID_TYPES:
        typ = 'other'
    tags = body.get('tags')
    if not isinstance(tags, list):
        tags = []
    tags = [str(t).strip() for t in tags if str(t).strip()]
    rec = {
        'name': str(body.get('name', '')).strip(),
        'type': typ,
        'endpoints': endpoints,
        'description': str(body.get('description', '')).strip(),
        'probe': _normalize_probe(body.get('probe'), endpoints),
        'tags': tags,
        'enabled': bool(body.get('enabled', True)),
    }
    if sid is not None:
        rec['id'] = sid
    return rec


def _probe_tcp(host: str, port, timeout: float):
    """TCP connect probe → (status, latency_ms). 블로킹 — to_thread 로 호출."""
    if not host or not port:
        return ('unknown', None)
    t0 = time.monotonic()
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            return ('up', round((time.monotonic() - t0) * 1000, 1))
    except Exception:
        return ('down', None)


def _probe_sync(rec: dict):
    """레코드의 probe 설정대로 1회 probe. tcp 만 구현, 나머지는 unknown."""
    probe = rec.get('probe') or {}
    mode = probe.get('mode', 'none')
    if mode == 'tcp':
        return _probe_tcp(probe.get('host'), probe.get('port'), float(probe.get('timeout', 2)))
    return ('unknown', None)


async def _probe_result(rec: dict) -> dict:
    status, latency = await asyncio.to_thread(_probe_sync, rec)
    out = {'id': rec.get('id'), 'status': status,
           'checked_at': datetime.now().isoformat(timespec='seconds')}
    if latency is not None:
        out['latency_ms'] = latency
    return out


async def handle_external_systems(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    method = handler_args.method.upper()
    parts = _parts(handler_args.full_path)
    sdir = _dir(config)

    try:
        # ── 컬렉션 루트 ──
        if not parts:
            if method == 'GET':
                systems = sorted(file_store.load_all(sdir), key=lambda r: r.get('id', 0))
                return HandlerResult(status=200, body={'systems': systems})
            if method == 'POST':
                body = _body_dict(handler_args)
                if not isinstance(body, dict):
                    return HandlerResult(status=400, body={'error': 'invalid_body'})
                rec = _normalize_record(body)
                if not rec['name']:
                    return HandlerResult(status=400, body={'error': 'name_required'})
                if not rec['endpoints']:
                    return HandlerResult(status=400, body={'error': 'endpoints_required'})
                sid = file_store.next_id(sdir)
                rec['id'] = sid
                file_store.save(sdir, sid, rec)
                return HandlerResult(status=201, body={'id': sid})
            return HandlerResult(status=405, body={'error': 'method_not_allowed'})

        # ── /status : enabled 전체 동시 probe ──
        if parts[0] == 'status':
            if method != 'GET':
                return HandlerResult(status=405, body={'error': 'method_not_allowed'})
            systems = [r for r in file_store.load_all(sdir) if r.get('enabled', True)]
            results = await asyncio.gather(*[_probe_result(r) for r in systems]) if systems else []
            return HandlerResult(status=200, body={'items': list(results)})

        sid = parts[0]
        # /{id}/probe
        if len(parts) >= 2 and parts[1] == 'probe':
            if method != 'POST':
                return HandlerResult(status=405, body={'error': 'method_not_allowed'})
            rec = file_store.load(sdir, sid)
            if rec is None:
                return HandlerResult(status=404, body={'error': 'not_found', 'id': sid})
            return HandlerResult(status=200, body=await _probe_result(rec))

        # /{id}
        if method == 'GET':
            rec = file_store.load(sdir, sid)
            if rec is None:
                return HandlerResult(status=404, body={'error': 'not_found', 'id': sid})
            return HandlerResult(status=200, body=rec)
        if method == 'PUT':
            existing = file_store.load(sdir, sid)
            if existing is None:
                return HandlerResult(status=404, body={'error': 'not_found', 'id': sid})
            body = _body_dict(handler_args)
            if not isinstance(body, dict):
                return HandlerResult(status=400, body={'error': 'invalid_body'})
            try:
                key_id = int(sid)
            except (TypeError, ValueError):
                key_id = existing.get('id', sid)
            rec = _normalize_record(body, sid=key_id)
            if not rec['name']:
                return HandlerResult(status=400, body={'error': 'name_required'})
            if not rec['endpoints']:
                return HandlerResult(status=400, body={'error': 'endpoints_required'})
            rec['create_time'] = existing.get('create_time')
            file_store.save(sdir, sid, rec)
            return HandlerResult(status=200, body={'id': key_id})
        if method == 'DELETE':
            ok = file_store.delete(sdir, sid)
            return HandlerResult(status=200, body={'id': sid, 'deleted': ok})
        return HandlerResult(status=405, body={'error': 'method_not_allowed'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


CIMS_EXTERNAL_SYSTEMS_HANDLER_LIST = [
    (_BASE, handle_external_systems, {}),
]
