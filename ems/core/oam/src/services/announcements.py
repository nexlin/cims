"""서비스 안내음성 라이브러리 — 등록·삭제·CMP 노드 배포 (docs/design/features/announcements.md §7).

계측기 샘플 라이브러리(ems/tester — `oam-cims-tester`)와는 **형식·변환기만 같고 실체는 별개**다: 카탈로그·저장소·API·화면·삭제가
서로 독립이다. 여기 음원은 CSP 정책(Setup.Announcement.Rules)이 참조하고 CMP 재생기가 낸다.

  저장소(관리 store — file_store 도메인 이웃, oam_ha 공유 마운트 규칙 그대로)
    <store>/announcements/catalog.jsonl        운영자 등록 카탈로그(행 = 음원 하나, id = op:<name>)
    <store>/announcements/op/<name>.wav        16 kHz PCM 마스터(청취·재변환)
    <store>/announcements/op/<name>.{pcmu,pcma,g722,amrwb}   코덱 파일(DTX 끔 — CMP 가 그대로 낸다)
  동봉(패키지)
    <oam>/announcements/sys_catalog.jsonl      기본 세트(sys:) 카탈로그 — 표시·프로파일 선택기용(파일은 CMP 패키지가 가진다)
    <oam>/announcements/sys/<name>.wav         기본 세트 마스터 — 콘솔 청취
    <oam>/native/cims-sample-conv              변환기(계측기와 같은 바이너리)

  배포 = OAM → agent sync REST → CMP install_path
    PUT /module-file?install_path&path=announcements/op/<file>   (바이너리, atomic)
    PUT /collection?install_path&name=announcements               (config/announcements.jsonl = op 행) + SIGUSR1 → CMP 재적재
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import wave
from typing import Dict, List, Optional, Tuple

from . import paths

_HERE = os.path.dirname(os.path.abspath(__file__))
_COMPONENT_ROOT = os.path.normpath(os.path.join(_HERE, '..', '..'))          # <oam>
_REPO_ROOT = os.path.normpath(os.path.join(_COMPONENT_ROOT, '..', '..', '..'))  # dev 트리(ems/core/oam → 레포)

CODECS = ('pcmu', 'pcma', 'g722', 'amr-wb')
FILE_EXT = {'pcmu': 'pcmu', 'pcma': 'pcma', 'g722': 'g722', 'amr-wb': 'amrwb'}
KINDS = ('tone', 'announcement', 'music')
MAX_WAV_BYTES = 64 * 1024 * 1024
_NAME_RE = re.compile(r'^[a-z0-9_]{1,40}$')
_CATALOG_COLLECTION = 'announcements'
_DEPLOY_SUBDIR = 'announcements/op'

_config: dict = {}


class AnnError(Exception):
    def __init__(self, code: str, detail: str = '', status: int = 400):
        super().__init__(detail or code)
        self.code, self.detail, self.status = code, detail, status


def init(config: dict) -> None:
    global _config
    _config = config or {}


# ── 경로 ─────────────────────────────────────────────────────────────────────

def store_dir() -> str:
    d = os.path.join(paths.runtime_store_dir(_config), 'announcements')
    os.makedirs(os.path.join(d, 'op'), exist_ok=True)
    return d


def _catalog_path() -> str:
    return os.path.join(store_dir(), 'catalog.jsonl')


def bundled_catalog_path() -> Optional[str]:
    for c in (os.path.join(_COMPONENT_ROOT, 'announcements', 'sys_catalog.jsonl'),
              os.path.join(_REPO_ROOT, 'media', 'announcements', 'sys', 'catalog.jsonl')):
        if os.path.isfile(c):
            return c
    return None


def bundled_master_dir() -> Optional[str]:
    for c in (os.path.join(_COMPONENT_ROOT, 'announcements', 'sys'),
              os.path.join(_REPO_ROOT, 'media', 'announcements', 'pcm')):
        if os.path.isdir(c):
            return c
    return None


def converter_path() -> Optional[str]:
    cfg = str(((_config.get('Announcements') or {}).get('SampleConv') or '')).strip()
    cands = [cfg] if cfg else []
    cands.append(os.path.join(_COMPONENT_ROOT, 'native', 'cims-sample-conv'))
    cands.append(os.path.join(_REPO_ROOT, 'build', 'bin', 'cims-sample-conv'))
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return shutil.which('cims-sample-conv')


# ── 카탈로그 ─────────────────────────────────────────────────────────────────

def _read_jsonl(path: Optional[str]) -> List[dict]:
    out: List[dict] = []
    if not path or not os.path.isfile(path):
        return out
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if isinstance(row, dict) and row.get('id'):
                out.append(row)
    return out


def _write_catalog(rows: List[dict]) -> None:
    path = _catalog_path()
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')
    os.replace(tmp, path)


def operator_rows() -> List[dict]:
    return _read_jsonl(_catalog_path())


def bundled_rows() -> List[dict]:
    return _read_jsonl(bundled_catalog_path())


def _row_out(r: dict, source: str) -> dict:
    o = dict(r)
    o['source'] = source
    o['name'] = r['id'].split(':', 1)[1] if ':' in r['id'] else r['id']
    o['has_master'] = master_path(r['id']) is not None
    return o


def list_media() -> List[dict]:
    return [_row_out(r, 'bundled') for r in bundled_rows()] + [_row_out(r, 'operator') for r in operator_rows()]


def get_media(mid: str) -> Optional[dict]:
    for r in operator_rows():
        if r['id'] == mid:
            return _row_out(r, 'operator')
    for r in bundled_rows():
        if r['id'] == mid:
            return _row_out(r, 'bundled')
    return None


def master_path(mid: str) -> Optional[str]:
    scope, _, name = mid.partition(':')
    if scope == 'op':
        p = os.path.join(store_dir(), 'op', f'{name}.wav')
        return p if os.path.isfile(p) else None
    if scope == 'sys':
        d = bundled_master_dir()
        if d:
            p = os.path.join(d, f'{name}.wav')
            return p if os.path.isfile(p) else None
    return None


def codec_file_path(mid: str, codec: str) -> Optional[str]:
    """운영자 음원의 코덱 파일 절대 경로(동봉 세트의 파일은 CMP 패키지에 있어 OAM 이 갖지 않는다)."""
    row = get_media(mid)
    if not row or row['source'] != 'operator':
        return None
    rel = (row.get('files') or {}).get(codec)
    if not rel:
        return None
    p = os.path.join(store_dir(), rel)
    return p if os.path.isfile(p) else None


# ── 등록·삭제 ────────────────────────────────────────────────────────────────

def _sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 16), b''):
            h.update(chunk)
    return h.hexdigest()


def _duration_ms(wav: str) -> int:
    try:
        with wave.open(wav, 'rb') as w:
            return int(round(w.getnframes() * 1000.0 / w.getframerate()))
    except Exception:
        return 0


def _run_converter(wav_bytes: bytes, name: str, out_dir: str, normalize: Optional[float]) -> dict:
    conv = converter_path()
    if not conv:
        raise AnnError('converter_missing', 'cims-sample-conv 가 없다(패키지 native/ 또는 Announcements.SampleConv)', 503)
    with tempfile.NamedTemporaryFile(prefix='ann-', suffix='.wav', delete=False) as tf:
        tf.write(wav_bytes)
        src = tf.name
    try:
        cmd = [conv, '--in', src, '--out-dir', out_dir, '--id', name, '--codecs', ','.join(CODECS), '--no-dtx']
        if normalize is not None:
            cmd += ['--normalize', str(normalize)]
        try:
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300, text=True)
        except subprocess.TimeoutExpired:
            raise AnnError('convert_failed', '변환 시간 초과(300 s)', 500)
        try:
            out = json.loads((proc.stdout or '').strip().splitlines()[-1]) if (proc.stdout or '').strip() else {}
        except ValueError:
            out = {}
        if proc.returncode != 0 or not isinstance(out, dict) or 'error' in out:
            raise AnnError('convert_failed', (out or {}).get('error') or (proc.stderr or '').strip()[-300:] or f'rc={proc.returncode}', 400)
        return out
    finally:
        try:
            os.unlink(src)
        except OSError:
            pass


def register(name: str, wav_bytes: bytes, kind: str = 'announcement', description: str = '', loop: bool = False,
             normalize: Optional[float] = None, replace: bool = False, actor: str = '') -> dict:
    if not _NAME_RE.match(name or ''):
        raise AnnError('bad_id', 'id 는 소문자·숫자·_ 40자 이내 (op:<id> 로 저장)')
    if kind not in KINDS:
        raise AnnError('bad_kind', f'kind 는 {list(KINDS)} 중 하나')
    if not wav_bytes:
        raise AnnError('empty_body', 'WAV 본문이 비었다')
    if len(wav_bytes) > MAX_WAV_BYTES:
        raise AnnError('too_large', f'WAV 는 {MAX_WAV_BYTES // (1024 * 1024)} MB 이하', 413)
    if len(wav_bytes) < 44 or wav_bytes[:4] != b'RIFF' or wav_bytes[8:12] != b'WAVE':
        raise AnnError('bad_wav', 'RIFF/WAVE 파일이 아니다')
    mid = f'op:{name}'
    rows = operator_rows()
    if any(r['id'] == mid for r in rows) and not replace:
        raise AnnError('exists', f'{mid} 가 이미 있다(replace=1 로 교체)', 409)
    op_dir = os.path.join(store_dir(), 'op')
    tmp_dir = tempfile.mkdtemp(prefix='ann-conv-', dir=store_dir())
    try:
        out = _run_converter(wav_bytes, name, tmp_dir, normalize)
        files: Dict[str, str] = {}
        sha: Dict[str, str] = {}
        master = out.get('master') or f'{name}.wav'
        for codec in CODECS:
            fn = f'{name}.{FILE_EXT[codec]}'
            src = os.path.join(tmp_dir, fn)
            if not os.path.isfile(src):
                raise AnnError('convert_failed', f'변환 결과에 {fn} 이 없다', 500)
            os.replace(src, os.path.join(op_dir, fn))
            files[codec] = f'op/{fn}'
            sha[codec] = _sha256(os.path.join(op_dir, fn))
        if os.path.isfile(os.path.join(tmp_dir, master)):
            os.replace(os.path.join(tmp_dir, master), os.path.join(op_dir, f'{name}.wav'))
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    row = {
        'id': mid, 'kind': kind, 'description': description or '',
        'duration_ms': _duration_ms(os.path.join(op_dir, f'{name}.wav')) or int(round(float(out.get('duration_s') or 0) * 1000)),
        'loop': bool(loop), 'files': files, 'sha256': sha,
        'level_dbov': out.get('p56_active_level_dbov'),
        'registered_at': time.strftime('%Y-%m-%dT%H:%M:%S'), 'registered_by': actor or '',
    }
    rows = [r for r in rows if r['id'] != mid] + [row]
    _write_catalog(rows)
    return _row_out(row, 'operator')


def delete(mid: str) -> None:
    if mid.startswith('sys:'):
        raise AnnError('bundled_read_only', '동봉 세트(sys:)는 지울 수 없다', 409)
    rows = operator_rows()
    row = next((r for r in rows if r['id'] == mid), None)
    if row is None:
        raise AnnError('not_found', mid, 404)
    name = mid.split(':', 1)[1]
    for fn in [f'{name}.wav'] + [f'{name}.{FILE_EXT[c]}' for c in CODECS]:
        try:
            os.unlink(os.path.join(store_dir(), 'op', fn))
        except OSError:
            pass
    _write_catalog([r for r in rows if r['id'] != mid])


# ── CMP 노드 배포 ─────────────────────────────────────────────────────────────

def _cmp_deployments(config: dict) -> List[dict]:
    """CMP 배포(package name cmp — 없으면 process_name/설치 경로로 추정) → proxy 용 레코드(agent 주소·토큰 포함)."""
    from handlers.agents import _deploy_load_all, _fetch_deployment_for_proxy, _pkg_load
    out = []
    for dep in _deploy_load_all(config):
        name = (dep.get('package_name') or '')
        if not name and dep.get('package_id') is not None:
            pkg = _pkg_load(config, pid=dep.get('package_id')) or {}
            name = pkg.get('name') or ''
        is_cmp = (name.lower() == 'cmp') or (str(dep.get('process_name') or '').upper() == 'CMP') or \
                 ('/cmp/' in str(dep.get('install_path') or ''))
        if not is_cmp or not dep.get('install_path'):
            continue
        full = _fetch_deployment_for_proxy(dep['id'], config)
        if full:
            full['status'] = dep.get('status')
            out.append(full)
    return out


def _node_label(dep: dict) -> str:
    return f"{dep.get('agent_name') or dep.get('ip_address') or '?'}/cmp#{dep.get('id')}"


def _expected_files() -> Dict[str, str]:
    """운영자 파일 → sha256 (배포 대조 기준)."""
    exp: Dict[str, str] = {}
    for r in operator_rows():
        for codec, rel in (r.get('files') or {}).items():
            fn = os.path.basename(rel)
            exp[fn] = (r.get('sha256') or {}).get(codec, '')
    return exp


def node_status(config: dict) -> List[dict]:
    """CMP 노드별 운영자 음원 보유 상태 — GET /module-files 로 sha256 대조."""
    from handlers.agents import _agent_proxy_call
    exp = _expected_files()
    nodes = []
    for dep in _cmp_deployments(config):
        st, resp = _agent_proxy_call('GET', dep, '/module-files',
                                     {'install_path': dep['install_path'], 'dir': _DEPLOY_SUBDIR}, None, 10, config)
        n = {'deployment_id': dep.get('id'), 'node': _node_label(dep), 'agent_id': dep.get('agent_id'),
             'install_path': dep.get('install_path'), 'status': dep.get('status')}
        if st != 200 or not isinstance(resp, dict):
            n.update({'presence': 'unreachable', 'error': (resp or {}).get('error') if isinstance(resp, dict) else str(resp)})
            nodes.append(n)
            continue
        have = {f.get('name'): f.get('sha256') for f in (resp.get('files') or []) if isinstance(f, dict)}
        missing = [fn for fn, sha in exp.items() if have.get(fn) != sha]
        n.update({'presence': 'ok' if not missing else ('missing' if len(missing) == len(exp) and exp else 'partial'),
                  'missing': missing, 'have': len(have), 'expected': len(exp)})
        nodes.append(n)
    return nodes


def deploy(config: dict, ids: Optional[List[str]] = None) -> dict:
    """운영자 음원을 CMP 노드 전부에 맞춘다 — 없거나 지문이 다른 파일만 PUT, 카탈로그(collection) 갱신, SIGUSR1."""
    from handlers.agents import _agent_proxy_call
    rows = operator_rows()
    if ids:
        want = set(ids)
        rows_sel = [r for r in rows if r['id'] in want]
    else:
        rows_sel = rows
    result: Dict[str, dict] = {}
    for dep in _cmp_deployments(config):
        label = _node_label(dep)
        pushed: List[str] = []
        errors: List[str] = []
        st, resp = _agent_proxy_call('GET', dep, '/module-files',
                                     {'install_path': dep['install_path'], 'dir': _DEPLOY_SUBDIR}, None, 10, config)
        if st != 200 or not isinstance(resp, dict):
            result[label] = {'error': (resp or {}).get('error') if isinstance(resp, dict) else f'HTTP {st}', 'pushed': [], 'errors': []}
            continue
        have = {f.get('name'): f.get('sha256') for f in (resp.get('files') or []) if isinstance(f, dict)}
        for r in rows_sel:
            for codec, rel in (r.get('files') or {}).items():
                fn = os.path.basename(rel)
                sha = (r.get('sha256') or {}).get(codec, '')
                if have.get(fn) == sha:
                    continue
                path = os.path.join(store_dir(), rel)
                try:
                    with open(path, 'rb') as f:
                        data = f.read()
                except OSError as e:
                    errors.append(f'{fn}: {e}')
                    continue
                st2, resp2 = _agent_proxy_call('PUT', dep, '/module-file',
                                               {'install_path': dep['install_path'], 'path': f'{_DEPLOY_SUBDIR}/{fn}'},
                                               None, 60, config, raw=data, content_type='application/octet-stream')
                if st2 == 200:
                    pushed.append(fn)
                else:
                    errors.append(f"{fn}: {(resp2 or {}).get('error') if isinstance(resp2, dict) else f'HTTP {st2}'}")
        # 카탈로그(collection) — op 행 전부(선택 배포여도 목록은 전체가 정본) + SIGUSR1 → CMP 재적재
        st3, resp3 = _agent_proxy_call('PUT', dep, '/collection',
                                       {'install_path': dep['install_path'], 'name': _CATALOG_COLLECTION},
                                       {'records': rows, 'signal': True}, 15, config)
        if st3 != 200:
            errors.append(f"catalog: {(resp3 or {}).get('error') if isinstance(resp3, dict) else f'HTTP {st3}'}")
        result[label] = {'pushed': pushed, 'errors': errors, 'signaled': (resp3 or {}).get('signaled') if isinstance(resp3, dict) else None}
    return result


def undeploy_file(config: dict, mid: str) -> dict:
    """삭제된 운영자 음원의 파일을 노드에서도 걷고 카탈로그를 다시 내린다(선택 — 남겨도 무해)."""
    from handlers.agents import _agent_proxy_call
    name = mid.split(':', 1)[1] if ':' in mid else mid
    rows = operator_rows()
    result: Dict[str, dict] = {}
    for dep in _cmp_deployments(config):
        errs = []
        for codec in CODECS:
            fn = f'{name}.{FILE_EXT[codec]}'
            st, resp = _agent_proxy_call('DELETE', dep, '/module-file',
                                         {'install_path': dep['install_path'], 'path': f'{_DEPLOY_SUBDIR}/{fn}'}, None, 10, config)
            if st not in (200, 404):
                errs.append(f'{fn}: HTTP {st}')
        st3, _ = _agent_proxy_call('PUT', dep, '/collection', {'install_path': dep['install_path'], 'name': _CATALOG_COLLECTION},
                                   {'records': rows, 'signal': True}, 15, config)
        if st3 != 200:
            errs.append(f'catalog: HTTP {st3}')
        result[_node_label(dep)] = {'errors': errs}
    return result
