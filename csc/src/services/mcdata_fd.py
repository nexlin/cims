"""MCData FD(File Distribution) 콘텐츠 서버 — TS 23.282 media storage function.

FD via HTTP (docs/design/features/mcdata_messaging.md): 발신 단말이 파일을 업로드하고
URL 을 FD SIGNALLING PAYLOAD(SIP MESSAGE) 로 전파, 수신 단말이 URL 로 다운로드한다.
MCPTT 서버(4430)에 동봉 — 단말은 이미 이 포트로 IdMS/GMS 를 쓴다(Bearer 토큰 동일).

  POST /mcdata/fd?name=<fname>&group=<gid>&type=<mime>   body=octet-stream → {"id","url","size"}
  GET  /mcdata/fd/{id}                                    → 파일 스트리밍

저장: {FdDir}/{YYYY}/{MM}/{DD}/{id}.bin + {id}.json (메타: name/size/type/group/uploader/ts)
FdDir 기본 = {ServiceLogging.Dir}/mcdata_fd (NAS 공유 — oam-svc 도 접근 가능).
"""
import os
import json
import re
import uuid
import datetime

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

logger = Logger("mcdata_fd")

_FD_DIR = ""
_MAX_BYTES = 52428800  # 50MB (McDataFd.MaxBytes)
_ID_RE = re.compile(r"^[0-9a-f]{32}$")


def init(config: dict):
    """csc_app 기동 시 호출 — 저장 루트/상한 결정."""
    global _FD_DIR, _MAX_BYTES
    fd_conf = config.get('McDataFd', {})
    base = fd_conf.get('Dir', '')
    if not base:
        sl = config.get('ServiceLogging', {})
        sl_dir = sl.get('Dir', '') or config.get('ServiceLogDir', config.get('MsgLogDir', ''))
        base = os.path.join(sl_dir or '.', 'mcdata_fd')
    _FD_DIR = base
    try:
        _MAX_BYTES = int(fd_conf.get('MaxBytes', _MAX_BYTES))
    except (TypeError, ValueError):
        pass
    os.makedirs(_FD_DIR, exist_ok=True)
    logger.log_info(f"MCData FD store: {_FD_DIR} (max {_MAX_BYTES} bytes)")


def _err(status, msg):
    return HandlerResult(status=status, body={'error': msg})


def _q(args: HandlerArgs, name, default=''):
    v = (args.query_params or {}).get(name)
    if isinstance(v, list):
        v = v[0] if v else None
    return v if v else default


def _meta_path(fid: str):
    """id → (meta.json, bin) 경로. 날짜 디렉토리는 메타에 기록되므로 인덱스 파일로 탐색."""
    # 업로드 시 {FdDir}/index/{id}.json 에 실경로 기록 (날짜 무관 O(1) 조회)
    return os.path.join(_FD_DIR, 'index', f'{fid}.json')


async def handle_mcdata_fd(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # 인증 — MCPTT access token (IdMS 발급, mcptt.validate_access_token)
    from services.mcptt import extract_token, GROUPS, _group_uri, _is_group_member
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")
    mcptt_id = token_payload.get('mcptt_id', '')

    if not _FD_DIR:
        return _err(503, 'FD store not configured')

    if args.method == 'POST':
        # 업로드 본문: octet-stream(bytes) 또는 multipart {"file": bytes}
        body = args.body
        if isinstance(body, dict):
            data = body.get('file')
            fname = body.get('file__filename') or _q(args, 'name', 'file.bin')
        else:
            data = body
            fname = _q(args, 'name', 'file.bin')
        if not isinstance(data, (bytes, bytearray)) or len(data) == 0:
            return _err(400, 'file body required (octet-stream or multipart "file")')
        if len(data) > _MAX_BYTES:
            return _err(413, f'file too large (max {_MAX_BYTES} bytes)')

        # 그룹 게이트 — allow_fd + 업로더 멤버십 (TS 24.481)
        gid = _q(args, 'group', '')
        if gid:
            group = GROUPS.get(_group_uri(gid)) or GROUPS.get(gid)
            if group is None:
                return _err(404, f'unknown group {gid}')
            if not group.get('allow_fd', False):
                return _err(403, 'file distribution disabled for this group')
            if not _is_group_member(group, mcptt_id):
                return _err(403, 'not a member of this group')

        mime = _q(args, 'type', 'application/octet-stream')
        fid = uuid.uuid4().hex
        now = datetime.datetime.now()
        rel_dir = now.strftime('%Y/%m/%d')
        data_dir = os.path.join(_FD_DIR, rel_dir)
        os.makedirs(data_dir, exist_ok=True)
        bin_path = os.path.join(data_dir, f'{fid}.bin')
        with open(bin_path, 'wb') as f:
            f.write(data)

        meta = {
            'id': fid, 'name': os.path.basename(fname), 'size': len(data), 'type': mime,
            'group': gid, 'uploader': mcptt_id, 'ts': now.isoformat(timespec='seconds'),
            'path': bin_path,
        }
        os.makedirs(os.path.join(_FD_DIR, 'index'), exist_ok=True)
        with open(_meta_path(fid), 'w', encoding='utf-8') as f:
            json.dump(meta, f, ensure_ascii=False)

        host = args.headers.get('host', '')
        url = f'https://{host}/mcdata/fd/{fid}' if host else f'/mcdata/fd/{fid}'
        logger.log_info(f"FD upload {fid} name={meta['name']} size={meta['size']} group={gid} by={mcptt_id}")
        return HandlerResult(status=201, body={'id': fid, 'url': url,
                                               'size': meta['size'], 'name': meta['name']})

    if args.method == 'GET':
        # /mcdata/fd/{id}
        fid = (args.full_path or '').split('?')[0].rstrip('/').rsplit('/', 1)[-1]
        if not _ID_RE.match(fid):
            return _err(400, 'invalid file id')
        try:
            with open(_meta_path(fid), encoding='utf-8') as f:
                meta = json.load(f)
        except OSError:
            return _err(404, 'file not found')
        path = meta.get('path', '')
        if not path or not os.path.isfile(path):
            return _err(404, 'file content missing')
        fname = (meta.get('name') or 'file.bin').replace('"', '')
        return HandlerResult(
            status=200, body='',  # str body + X-File-Path → FileResponse 스트리밍 (controller._http_response)
            headers={'X-File-Path': path,
                     'Content-Disposition': f'attachment; filename="{fname}"'},
            media_type=meta.get('type') or 'application/octet-stream')

    return HandlerResult(status=405, body="Method Not Allowed")


MCDATA_FD_HANDLER_LIST = [
    ("/mcdata/fd", handle_mcdata_fd, {}),
]
