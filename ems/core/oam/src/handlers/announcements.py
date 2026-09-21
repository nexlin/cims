"""서비스 안내음성 라이브러리 REST — /api/v1/announcements (base OAM 소유, announcements.md §7.3).

  GET    /announcements                 목록(동봉 sys: + 운영자 op:) [+ ?nodes=1 → CMP 노드별 보유 상태]
  POST   /announcements?id=&kind=&description=&loop=&normalize=&replace=   WAV 등록 (본문 application/octet-stream)
  GET    /announcements/nodes           CMP 노드별 운영자 음원 보유 상태(sha256 대조)
  POST   /announcements/deploy          {ids?} — CMP 노드 전부에 파일·카탈로그 배포 + SIGUSR1
  GET    /announcements/{id}            상세
  GET    /announcements/{id}/master.wav 16 kHz PCM 마스터(콘솔 청취 — fetch+Blob, 인증 헤더)
  GET    /announcements/{id}/files/{codec}   운영자 음원 코덱 파일(pcmu|pcma|g722|amr-wb)
  DELETE /announcements/{id}[?undeploy=1]    운영자 음원 삭제(동봉은 409) — undeploy=1 이면 노드 파일도 걷는다

권한: GET=monitor, POST=operator, DELETE=manager. 배포 자산 분배(패키지·컬렉션과 같은 평면)라 base 가 소유하고 agent sync REST 로 내린다.
"""
from __future__ import annotations

from urllib.parse import unquote

from httpsrv.handler import HandlerArgs, HandlerResult
from services import announcements as ann

_BASE = '/api/v1/announcements'
_MOD = 'oam'


def _json(status: int, body) -> HandlerResult:
    return HandlerResult(status=status, body=body, media_type='application/json')


def _parts(full_path: str):
    p = full_path.split('?', 1)[0]
    if p.startswith(_BASE):
        p = p[len(_BASE):]
    return tuple(unquote(x) for x in p.strip('/').split('/') if x)


def _rbac(handler_args, method: str):
    from services.admin_auth import require_role
    need = 'monitor' if method == 'GET' else ('manager' if method == 'DELETE' else 'operator')
    payload, err = require_role(handler_args, need)
    return payload, err


async def handle_announcements(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {}) or {}
    method = handler_args.method.upper()
    payload, deny = _rbac(handler_args, method)
    if deny:
        return deny
    parts = _parts(handler_args.full_path)
    q = handler_args.query_params or {}
    try:
        if not parts:
            if method == 'GET':
                out = {'media': ann.list_media(), 'converter': ann.converter_path() is not None,
                       'store_dir': ann.store_dir(), 'bundled_catalog': ann.bundled_catalog_path()}
                if (q.get('nodes') or '') in ('1', 'true', 'yes'):
                    out['nodes'] = ann.node_status(config)
                return _json(200, out)
            if method == 'POST':
                b = getattr(handler_args, 'body', None)
                data = bytes(b) if isinstance(b, (bytes, bytearray)) else (
                    b.get('file') if isinstance(b, dict) and isinstance(b.get('file'), (bytes, bytearray)) else None)
                if not data:
                    return _json(400, {'error': 'wav_body_required', 'detail': 'WAV 파일을 application/octet-stream 본문으로, id 등은 query 로'})
                norm = q.get('normalize')
                actor = f"console:{(payload or {}).get('login_id') or (payload or {}).get('sub') or ''}"
                row = ann.register(q.get('id') or '', bytes(data), kind=q.get('kind') or 'announcement',
                                   description=q.get('description') or '', loop=(q.get('loop') or '') in ('1', 'true', 'yes'),
                                   normalize=(float(norm) if norm not in (None, '') else None),
                                   replace=(q.get('replace') or '') in ('1', 'true', 'yes'), actor=actor)
                return _json(201, row)
        elif parts[0] == 'nodes' and len(parts) == 1 and method == 'GET':
            return _json(200, {'nodes': ann.node_status(config)})
        elif parts[0] == 'deploy' and len(parts) == 1 and method == 'POST':
            body = handler_args.body if isinstance(getattr(handler_args, 'body', None), dict) else {}
            ids = body.get('ids') if isinstance(body.get('ids'), list) else None
            return _json(200, {'result': ann.deploy(config, ids)})
        else:
            mid = parts[0]
            row = ann.get_media(mid)
            if row is None:
                return _json(404, {'error': 'not_found', 'id': mid})
            if len(parts) == 1 and method == 'GET':
                return _json(200, row)
            if len(parts) == 1 and method == 'DELETE':
                ann.delete(mid)
                out = {'deleted': True, 'id': mid}
                if (q.get('undeploy') or '') in ('1', 'true', 'yes'):
                    out['undeploy'] = ann.undeploy_file(config, mid)
                return _json(200, out)
            if method == 'GET' and len(parts) == 2 and parts[1] == 'master.wav':
                path = ann.master_path(mid)
                if not path:
                    return _json(404, {'error': 'master_not_found', 'id': mid})
                return HandlerResult(status=200, body='', headers={'X-File-Path': path, 'Content-Type': 'audio/wav',
                                                                    'Content-Disposition': f'inline; filename="{row["name"]}.wav"'})
            if method == 'GET' and len(parts) == 3 and parts[1] == 'files':
                path = ann.codec_file_path(mid, parts[2])
                if not path:
                    return _json(404, {'error': 'file_not_found', 'id': mid, 'codec': parts[2]})
                return HandlerResult(status=200, body='', headers={'X-File-Path': path, 'Content-Type': 'application/octet-stream'})
        return _json(404, {'error': 'not_found', 'path': '/'.join(parts)})
    except ann.AnnError as e:
        return _json(e.status, {'error': e.code, 'detail': e.detail})


CIMS_ANNOUNCEMENTS_HANDLER_LIST = [
    (_BASE, handle_announcements, {}),
]

_AUTH_MON = {'scheme': 'bearer', 'role': 'monitor', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_OP = {'scheme': 'bearer', 'role': 'operator', 'token_from': 'POST /api/v1/auth/login'}
_AUTH_MGR = {'scheme': 'bearer', 'role': 'manager', 'token_from': 'POST /api/v1/auth/login'}

CIMS_ANNOUNCEMENTS_API_DOCS = [
    {'id': 'announcements.list', 'module': _MOD, 'method': 'GET', 'path': _BASE,
     'summary': '안내음성 라이브러리 — 동봉 세트(sys:, CMP 패키지가 파일을 가진다) + 운영자 등록(op:). ?nodes=1 이면 CMP 노드별 보유 상태',
     'params': [{'name': 'nodes', 'in': 'query', 'type': 'bool', 'desc': 'CMP 노드 대조(sha256) 포함'}],
     'response': '{media[]: {id, name, source(bundled|operator), kind, description, duration_ms, loop, files{codec: path}, sha256{}, level_dbov, has_master}, converter, nodes?[]}',
     'auth': _AUTH_MON},
    {'id': 'announcements.register', 'module': _MOD, 'method': 'POST', 'path': _BASE,
     'summary': '음원 등록 — WAV(PCM 8~48 kHz mono/stereo) 본문. 16 kHz 마스터 + pcmu/pcma/g722/amrwb(DTX 끔) 를 만든다. id 는 op:<id> 로 저장',
     'params': [{'name': 'body', 'in': 'body', 'type': 'bytes', 'required': True, 'desc': 'WAV 바이트(Content-Type: application/octet-stream)'},
                {'name': 'id', 'in': 'query', 'type': 'string', 'required': True, 'desc': '[a-z0-9_]{1,40}'},
                {'name': 'kind', 'in': 'query', 'type': 'string', 'enum': list(ann.KINDS)},
                {'name': 'description', 'in': 'query', 'type': 'string'}, {'name': 'loop', 'in': 'query', 'type': 'bool', 'desc': '반복 힌트(보류 음악)'},
                {'name': 'normalize', 'in': 'query', 'type': 'number', 'desc': 'P.56 활성 레벨을 이 dBov 로(안내 -26 · 신호음 -16 · 음악 -20 권장)'},
                {'name': 'replace', 'in': 'query', 'type': 'bool'}],
     'errors': [{'status': 400, 'when': 'id·WAV·변환 실패', 'body': {'error': 'bad_id|bad_wav|convert_failed'}},
                {'status': 409, 'when': '같은 id', 'body': {'error': 'exists'}}, {'status': 503, 'when': '변환기 없음', 'body': {'error': 'converter_missing'}}],
     'auth': _AUTH_OP},
    {'id': 'announcements.nodes', 'module': _MOD, 'method': 'GET', 'path': f'{_BASE}/nodes',
     'summary': 'CMP 노드별 운영자 음원 보유 상태(agent GET /module-files 의 sha256 대조) — ok|partial|missing|unreachable', 'auth': _AUTH_MON},
    {'id': 'announcements.deploy', 'module': _MOD, 'method': 'POST', 'path': f'{_BASE}/deploy',
     'summary': 'CMP 노드 전부에 운영자 음원을 맞춘다 — 없거나 지문이 다른 파일만 PUT /module-file, 카탈로그 collection PUT, SIGUSR1(재적재)',
     'params': [{'name': 'ids', 'in': 'body', 'type': 'string[]', 'desc': '비면 전부'}],
     'response': '{result{node: {pushed[], errors[], signaled}}}', 'auth': _AUTH_OP},
    {'id': 'announcements.get', 'module': _MOD, 'method': 'GET', 'path': f'{_BASE}/{{id}}',
     'summary': '음원 상세. `/master.wav` = 마스터(청취), `/files/{codec}` = 운영자 코덱 파일', 'auth': _AUTH_MON},
    {'id': 'announcements.delete', 'module': _MOD, 'method': 'DELETE', 'path': f'{_BASE}/{{id}}',
     'summary': '운영자 음원 삭제(동봉 sys: 는 409). ?undeploy=1 이면 CMP 노드 파일도 걷고 카탈로그를 다시 내린다. CSP 프로파일이 참조 중이면 그 안내는 MEDIA_NOT_FOUND 폴백(응답 코드만)',
     'auth': _AUTH_MGR},
]
