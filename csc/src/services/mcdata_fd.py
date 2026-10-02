"""MCData FD(File Distribution) 콘텐츠 서버 — TS 23.282 media storage function (TS 24.282 §10.2.2·§10.2.3·§6.7.3).

FD via HTTP (docs/design/features/mcdata_messaging.md): 발신 단말이 파일을 업로드하고
URL 을 FD SIGNALLING PAYLOAD(SIP MESSAGE) 로 전파, 수신 단말이 URL 로 다운로드한다.
MCPTT 서버(4430)에 동봉 — 단말은 이미 이 포트로 IdMS/GMS 를 쓴다(Bearer 토큰 동일).

  POST /mcdata/fd                 업로드 → 201 Created + Location(파일 URL) + {"id","url","size","name"}
        규격형(§10.2.2.1)  Content-Type: multipart/mixed — application/vnd.3gpp.mcdata-info+xml(request-type
                           one-to-one-fd | group-fd · mcdata-request-uri(그룹) · mcdata-calling-user-id) + application/octet-stream
        간이형             ?name=<fname>&group=<gid>&type=<mime> + 본문 octet-stream (또는 multipart/form-data "file")
  GET  /mcdata/fd/{id}            파일 (§10.2.3 — 수신 제어: 그룹 파일은 그 그룹 멤버·올린 사람만)
  HEAD /mcdata/fd/{id}            파일 존재 확인 (§6.7.3 — 200 / 404, 본문 없음. 제어 기능(CSP)은 내부 토큰으로 부른다)

저장: {FdDir}/{YYYY}/{MM}/{DD}/{id}.bin + index/{id}.json (메타: name/size/type/group/uploader/ts)
FdDir = McDataFd.Dir(명시) > 서비스 콘텐츠 영역 `{Content.Dir}/mcdata_fd` (site_paths — CMDP 와 같은 경로, NAS 공유).
"""
import os
import hmac
import json
import re
import uuid
import datetime
import xml.etree.ElementTree as ET

from httpsrv.handler import HandlerArgs, HandlerResult
from util.log_util import Logger

logger = Logger("mcdata_fd")

_FD_DIR = ""
_MAX_BYTES = 52428800  # 50MB (McDataFd.MaxBytes)
_ID_RE = re.compile(r"^[0-9a-f]{32}$")

MCDATA_INFO_TYPE = "application/vnd.3gpp.mcdata-info+xml"
REQ_ONE_TO_ONE_FD = "one-to-one-fd"
REQ_GROUP_FD = "group-fd"


def init(config: dict):
    """csc_app 기동 시 호출 — 저장 루트/상한 결정."""
    global _FD_DIR, _MAX_BYTES
    from services import site_paths
    fd_conf = config.get('McDataFd', {}) or {}
    _FD_DIR = site_paths.mcdata_fd_dir(config)
    try:
        _MAX_BYTES = int(fd_conf.get('MaxBytes', _MAX_BYTES))
    except (TypeError, ValueError):
        pass
    os.makedirs(_FD_DIR, exist_ok=True)
    logger.log_info(f"MCData FD store: {_FD_DIR} (max {_MAX_BYTES} bytes)")


def max_bytes() -> int:
    """FD 한 건의 상한(octet). 그룹 문서 `<mcdata-on-network-max-data-size-for-FD>`(TS 24.481 §7.2.2 l))가 싣는 값이자 업로드 413 의
    기준(TS 24.282 §10.2.2.2 1)b)) — 그룹 FD 와 1:1 FD(service configuration `<max-data-size-fd-bytes>` 자리)에 같은 값을 쓴다."""
    return _MAX_BYTES


def _err(status, msg, headers=None):
    return HandlerResult(status=status, body={'error': msg}, headers=headers or {})


def _q(args: HandlerArgs, name, default=''):
    v = (args.query_params or {}).get(name)
    if isinstance(v, list):
        v = v[0] if v else None
    return v if v else default


def _meta_path(fid: str):
    """id → (meta.json, bin) 경로. 날짜 디렉토리는 메타에 기록되므로 인덱스 파일로 탐색."""
    # 업로드 시 {FdDir}/index/{id}.json 에 실경로 기록 (날짜 무관 O(1) 조회)
    return os.path.join(_FD_DIR, 'index', f'{fid}.json')


# ── multipart/mixed (RFC 2046 §5.1) ──
def parse_multipart(content_type: str, raw: bytes) -> list:
    """multipart 본문 → [(헤더 dict(소문자 이름), 본문 bytes)]. 형식이 아니면 ValueError.

    경계선은 줄 머리의 `--boundary` 이고 그 앞 CRLF 는 경계선의 일부다(본문에 넣지 않는다). 파일 본문은 바이너리라 줄 단위로
    쪼개지 않고 경계선 위치만 찾는다."""
    m = re.search(r'boundary\s*=\s*(?:"([^"]+)"|([^;\s]+))', content_type or '', re.I)
    if not m:
        raise ValueError('multipart boundary missing')
    delim = b'--' + (m.group(1) or m.group(2)).encode('latin-1')
    if raw.startswith(delim):
        pos = 0
    else:
        pos = raw.find(b'\n' + delim)
        if pos < 0:
            raise ValueError('multipart boundary not found')
        pos += 1
    parts = []
    while True:
        cur = pos + len(delim)
        if raw[cur:cur + 2] == b'--':                       # 닫는 경계선
            break
        eol = raw.find(b'\n', cur)
        if eol < 0:
            raise ValueError('multipart part not terminated')
        start = eol + 1
        nxt = raw.find(b'\n' + delim, start - 1)
        if nxt < 0:
            raise ValueError('multipart closing boundary missing')
        end = nxt - 1 if nxt > 0 and raw[nxt - 1:nxt] == b'\r' else nxt
        chunk = raw[start:max(start, end)]
        if chunk[:2] == b'\r\n':
            head, body = b'', chunk[2:]
        elif chunk[:1] == b'\n':
            head, body = b'', chunk[1:]
        else:
            sep = chunk.find(b'\r\n\r\n')
            skip = 4
            alt = chunk.find(b'\n\n')
            if sep < 0 or (0 <= alt < sep):
                sep, skip = alt, 2
            if sep < 0:
                raise ValueError('multipart part header not terminated')
            head, body = chunk[:sep], chunk[sep + skip:]
        headers = {}
        for line in head.decode('utf-8', 'replace').splitlines():
            name, _, value = line.partition(':')
            if _:
                headers[name.strip().lower()] = value.strip()
        parts.append((headers, body))
        pos = nxt + 1
    return parts


def _media_type(value: str) -> str:
    return (value or '').split(';', 1)[0].strip().lower()


def parse_mcdata_info(xml_bytes: bytes) -> dict:
    """mcdata-info(TS 24.282 Annex D.1) → {request_type, group, calling_user}. 형식이 아니면 ValueError."""
    text = xml_bytes.decode('utf-8', 'replace')
    if '<!DOCTYPE' in text or '<!ENTITY' in text:
        raise ValueError('DTD not allowed')
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        raise ValueError(f'mcdata-info not well-formed: {e}')

    def local(el):
        return el.tag.rsplit('}', 1)[-1]

    out = {'request_type': '', 'group': '', 'calling_user': ''}
    for el in root.iter():
        name = local(el)
        if name == 'request-type':
            out['request_type'] = (el.text or '').strip()
        elif name in ('mcdata-request-uri', 'mcdata-calling-user-id'):
            # 값은 <mcdataURI> 자식(mcdataInfo contentType). type="Encrypted" 는 풀지 않는다(XML 보호 미지원).
            val = next(((c.text or '').strip() for c in el if local(c) == 'mcdataURI'), '') or (el.text or '').strip()
            out['group' if name == 'mcdata-request-uri' else 'calling_user'] = val
    return out


def _find_group(identity: str):
    """그룹 식별자(sip:·tel:·맨 id) → (GROUPS 키, 그룹). 없으면 (None, None)."""
    from services.mcptt import GROUPS, _group_uri, _norm_mcptt_uri
    for key in (identity, _group_uri(identity)):
        if key in GROUPS:
            return key, GROUPS[key]
    want = _norm_mcptt_uri(identity)
    for key, grp in GROUPS.items():
        if want and _norm_mcptt_uri(key) == want:
            return key, grp
    return None, None


def _content_disposition(name: str) -> str:
    """파일 이름 헤더(RFC 6266) — HTTP 헤더는 latin-1 이라 ASCII 대체 이름 + `filename*`(UTF-8 percent-encoding)을 함께 싣는다."""
    from urllib.parse import quote
    ascii_name = ''.join(c if 32 <= ord(c) < 127 and c not in '"\\' else '_' for c in name) or 'file.bin'
    value = f'attachment; filename="{ascii_name}"'
    if ascii_name != name:
        value += f"; filename*=UTF-8''{quote(name, safe='')}"
    return value


def _is_internal(auth_header: str) -> bool:
    """제어 기능(CSP)의 내부 토큰 — `/internal/*` 과 같은 `Authorization: Bearer <InternalApi.Token>`."""
    from services.auc import auc
    token = auc.internal_token()
    parts = (auth_header or '').split(None, 1)
    return bool(token) and len(parts) == 2 and parts[0].lower() == 'bearer' and hmac.compare_digest(parts[1].strip(), token)


def _load_meta(fid: str):
    try:
        with open(_meta_path(fid), encoding='utf-8') as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    path = meta.get('path', '')
    return meta if path and os.path.isfile(path) else None


def _may_receive(meta: dict, user: str) -> bool:
    """수신 제어(§10.2.3.2 1)) — 그룹 파일은 올린 사람과 지금 그 그룹 멤버만. 1:1 파일은 업로드에 수신자가 실리지 않으므로
    (§10.2.2.1 5) — calling-user-id 뿐) MCData 인가 토큰과 파일 URL(추측 불가 id)로 받는다."""
    from services.mcptt import _uri_eq, _is_group_member
    if _uri_eq(meta.get('uploader'), user):
        return True
    gid = meta.get('group') or ''
    if not gid:
        return True
    _, group = _find_group(gid)
    return bool(group) and _is_group_member(group, user)


async def handle_mcdata_fd(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # 인증 — IdMS access token(mcptt.validate_access_token) + MCData scope(3gpp:mc:data_service, TS 33.180 B.10).
    #   MCData 신원 = 토큰 mcdata_id (단일 MC service ID 구성이라 mcptt_id 와 같은 값 — 이행 전 토큰은 mcptt_id 폴백).
    #   HEAD 는 제어 기능의 내부 토큰도 받는다(§6.7.3.1 1)c) «valid access token» — CSP 는 사용자 토큰이 없다).
    from services.mcptt import (extract_token, unauthorized, require_scope, SCOPE_DATA_SERVICE, _is_group_member,
                                _uri_eq, public_base_url)
    auth = args.headers.get('authorization')
    internal = args.method == 'HEAD' and _is_internal(auth)
    mcdata_id = ''
    if not internal:
        token_payload = extract_token(auth)
        if not token_payload:
            return unauthorized(args)
        deny = require_scope(args, token_payload, 'MCDATA-FD', SCOPE_DATA_SERVICE)
        if deny:
            return deny
        mcdata_id = token_payload.get('mcdata_id') or token_payload.get('mcptt_id', '')

    if not _FD_DIR:
        return _err(503, 'FD store not configured')

    if args.method == 'POST':
        body = args.body
        ctype = args.headers.get('content-type', '')
        fname, mime, gid, req_type = _q(args, 'name', ''), _q(args, 'type', ''), _q(args, 'group', ''), ''
        if _media_type(ctype) == 'multipart/mixed':
            # 규격형 업로드(§10.2.2.1 4)~8)) — mcdata-info 로 그룹·발신자를, octet-stream 으로 파일을 받는다.
            if not isinstance(body, (bytes, bytearray)):
                return _err(400, 'multipart/mixed body required')
            try:
                parts = parse_multipart(ctype, bytes(body))
                info_part = next((b for h, b in parts if _media_type(h.get('content-type')) == MCDATA_INFO_TYPE), None)
                if info_part is None:
                    return _err(400, f'{MCDATA_INFO_TYPE} body required')
                info = parse_mcdata_info(info_part)
            except ValueError as e:
                return _err(400, str(e))
            if any(_media_type(h.get('content-type')) == 'message/external-body' for h, _ in parts):
                # network-stored file(MCData message store 의 파일을 가져와 저장) — message store 가 없다.
                return _err(501, 'message/external-body (network-stored file) not supported')
            file_part = next(((h, b) for h, b in parts if _media_type(h.get('content-type')) == 'application/octet-stream'), None)
            if file_part is None:
                return _err(400, 'application/octet-stream body required')
            fhead, data = file_part
            declared = fhead.get('content-length', '')
            if declared.isdigit() and int(declared) != len(data):
                return _err(400, 'Content-Length of the file body does not match')
            req_type = info['request_type']
            if req_type not in (REQ_ONE_TO_ONE_FD, REQ_GROUP_FD):
                return _err(400, f'request-type must be {REQ_ONE_TO_ONE_FD} or {REQ_GROUP_FD}')
            # 발신자 = 토큰의 MCData ID(TS 24.482 A.2 — 서버는 토큰의 신원으로 판정한다). 본문의 calling-user-id 가 다르면 거절.
            if info['calling_user'] and not _uri_eq(info['calling_user'], mcdata_id):
                return _err(403, 'mcdata-calling-user-id does not match the access token')
            gid = info['group'] if req_type == REQ_GROUP_FD else ''
            if req_type == REQ_GROUP_FD and not gid:
                return _err(400, 'mcdata-request-uri (group identity) required for group-fd')
            m = re.search(r'filename\s*=\s*"?([^";]+)"?', fhead.get('content-disposition', ''), re.I)
            fname = (m.group(1).strip() if m else '') or fname
        elif isinstance(body, dict):
            # 간이형 — multipart/form-data {"file": bytes}
            data = body.get('file')
            fname = body.get('file__filename') or fname
        else:
            data = body                                        # 간이형 — octet-stream
        if not isinstance(data, (bytes, bytearray)) or len(data) == 0:
            return _err(400, 'file body required (multipart/mixed, octet-stream or multipart/form-data "file")')

        # 전송 제어(§10.2.2.2 1)a)) — 그룹 FD 는 그 그룹이 FD 를 허용하고(allow_fd) 올리는 사람이 멤버여야 한다 (TS 24.481).
        group_key = ''
        if gid:
            group_key, group = _find_group(gid)
            if group is None:
                return _err(404, f'unknown group {gid}')
            if not group.get('allow_fd', False):
                return _err(403, 'file distribution disabled for this group')
            if not _is_group_member(group, mcdata_id):
                return _err(403, 'not a member of this group')
        # 크기(§10.2.2.2 1)b)) — 그룹 FD = 그룹 문서 <mcdata-on-network-max-data-size-for-FD>, 1:1 = <max-data-size-fd-bytes>. 둘 다 max_bytes().
        if len(data) > max_bytes():
            return _err(413, f'file too large (max {max_bytes()} bytes)')

        fid = uuid.uuid4().hex
        now = datetime.datetime.now()
        rel_dir = now.strftime('%Y/%m/%d')
        data_dir = os.path.join(_FD_DIR, rel_dir)
        os.makedirs(data_dir, exist_ok=True)
        bin_path = os.path.join(data_dir, f'{fid}.bin')
        with open(bin_path, 'wb') as f:
            f.write(data)

        meta = {
            'id': fid, 'name': os.path.basename(fname or 'file.bin'), 'size': len(data),
            'type': mime or 'application/octet-stream',
            'group': group_key or gid, 'uploader': mcdata_id, 'ts': now.isoformat(timespec='seconds'),
            'path': bin_path,
        }
        os.makedirs(os.path.join(_FD_DIR, 'index'), exist_ok=True)
        with open(_meta_path(fid), 'w', encoding='utf-8') as f:
            json.dump(meta, f, ensure_ascii=False)

        # 파일 URL = 공개 base URL(McpttServer.PublicUrl, 없으면 요청 Host) — 제어 기능이 FILEURL 을 이 base 와 대조한다
        #   (TS 24.282 §10.2.4.4.2 7)b) 403 212). 201 + Location(§10.2.2.2 2)b)) — 단말은 이 값을 FD 의 FILEURL 로 쓴다.
        url = f'{public_base_url(args)}/mcdata/fd/{fid}'
        logger.log_info(f"FD upload {fid} name={meta['name']} size={meta['size']} group={meta['group']} "
                        f"type={req_type or 'query'} by={mcdata_id}")
        return HandlerResult(status=201, headers={'Location': url},
                             body={'id': fid, 'url': url, 'size': meta['size'], 'name': meta['name']})

    if args.method in ('GET', 'HEAD'):
        # /mcdata/fd/{id}
        fid = (args.full_path or '').split('?')[0].rstrip('/').rsplit('/', 1)[-1]
        if not _ID_RE.match(fid):
            return _err(400, 'invalid file id')
        meta = _load_meta(fid)
        if meta is None:
            return _err(404, 'file not found')
        if not internal and not _may_receive(meta, mcdata_id):
            logger.log_error(f"FD {args.method} {fid} forbidden: {mcdata_id} is not a member of group {meta.get('group')}")
            return _err(403, 'not allowed to receive this file')
        headers = {'X-File-Path': meta['path'], 'Content-Disposition': _content_disposition(meta.get('name') or 'file.bin')}
        if internal:
            # 제어 기능용 — 그 파일이 어느 그룹에 올린 것인지(1:1 은 빈 값)와 올린 사람. FILEURL 을 다른 그룹에 다시 돌리는 것을 거를 때 쓴다.
            headers['X-Cims-Fd-Group'] = meta.get('group') or ''
            headers['X-Cims-Fd-Uploader'] = meta.get('uploader') or ''
        return HandlerResult(
            status=200, body='',  # str body + X-File-Path → FileResponse (HEAD 는 헤더만 — controller._http_response)
            headers=headers, media_type=meta.get('type') or 'application/octet-stream')

    return HandlerResult(status=405, body="Method Not Allowed")


MCDATA_FD_HANDLER_LIST = [
    ("/mcdata/fd", handle_mcdata_fd, {}),
]
