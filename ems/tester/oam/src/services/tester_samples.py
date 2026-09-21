"""계측기 미디어 샘플 라이브러리 — test_instrument.md §4 미디어 평면.

원음은 전부 **16-bit linear PCM 16 kHz mono WAV 마스터**(`<id>.wav`)로 두고, 코덱별 파일(pcmu/pcma/g722/amrwb/`_dtx.amrwb`)은
변환기 `cims-sample-conv`(패키지 `native/`, tester/sampleconv)가 마스터에서 뽑는다 — 동봉 샘플 생성기(gen_samples.py)와 같은 경로.
두 뿌리(시나리오 규약과 같다):
  - 패키지 동봉 `<component_root>/samples/` — 읽기 전용(source=bundled), 목록 = 그 안의 samples.json
  - 운영자 등록 `Tester.DataDir/samples/` — 콘솔 [샘플 등록] 이 만든다(source=user), 목록 = 그 안의 samples.json
같은 id 는 운영자본이 이긴다. 워커의 샘플 디렉터리(Media.SampleDir)는 이 라이브러리의 **사본** — run 시작 전 시나리오가 참조한 파일을
이름+크기로 대조해 없는 것만 `PUT /samples/{file}` 로 밀어 넣고(sync_for_run), 콘솔 [워커 동기화] 는 토폴로지 워커 전부에 전체를 맞춘다.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from typing import Dict, List, Optional, Tuple

from services import tester_store as store

_ID_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$')
SAMPLE_KINDS = ('tone', 'announcement', 'music', 'speech', 'other')
CATALOG = 'samples.json'
MAX_WAV_BYTES = 64 * 1024 * 1024


class SampleError(Exception):
    def __init__(self, code: str, detail: str = '', status: int = 400):
        super().__init__(detail or code)
        self.code, self.detail, self.status = code, detail, status


# ── 자리 ────────────────────────────────────────────────────────────────────

def bundled_dir() -> str:
    """패키지 `samples/`(dist — 마스터가 평평하게). 소스 트리 실행이면 생성기 산출물 자리 `tester/worker/samples/`(마스터는 `pcm/`)."""
    d = os.path.join(store._component_root, 'samples')
    if os.path.isdir(d):
        return d
    return os.path.normpath(os.path.join(store._component_root, '..', '..', '..', 'tester', 'worker', 'samples'))


def user_dir() -> str:
    return os.path.join(store.data_dir(), 'samples')


def _read_catalog(root: str) -> Dict[str, dict]:
    p = os.path.join(root, CATALOG)
    try:
        with open(p, 'r', encoding='utf-8') as f:
            doc = json.load(f)
        return doc if isinstance(doc, dict) else {}
    except (OSError, ValueError):
        return {}


def _write_user_catalog(cat: Dict[str, dict]) -> None:
    os.makedirs(user_dir(), exist_ok=True)
    p = os.path.join(user_dir(), CATALOG)
    tmp = p + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(cat, f, ensure_ascii=False, indent=1)
    os.replace(tmp, p)


def _row(sid: str, rec: dict, source: str) -> dict:
    files = dict(rec.get('files') or {})
    out = {
        'id': sid, 'source': source, 'kind': rec.get('kind') or 'other', 'description': rec.get('description') or '',
        'master': os.path.basename(rec.get('master') or f'{sid}.wav'),
        'duration_s': rec.get('duration_s'), 'p56_active_level_dbov': rec.get('p56_active_level_dbov'),
        'p56_activity_pct': rec.get('p56_activity_pct'), 'rms_dbov': rec.get('rms_dbov'),
        'files': files, 'pattern': rec.get('pattern'), 'created': rec.get('created'),
    }
    for k in ('amr-wb-dtx', 'dtx_frames', 'dtx_channel_activity_pct', 'pair', 'target_activity_pct', 'double_talk_pct', 'mutual_silence_pct'):
        if k in rec:
            out[k] = rec[k]
    return out


def list_samples() -> List[dict]:
    rows: Dict[str, dict] = {}
    for source, root in (('bundled', bundled_dir()), ('user', user_dir())):
        for sid, rec in _read_catalog(root).items():
            if isinstance(rec, dict):
                rows[sid] = _row(sid, rec, source)
    return sorted(rows.values(), key=lambda r: (r['kind'], r['id']))


def get_sample(sid: str) -> Optional[dict]:
    return next((r for r in list_samples() if r['id'] == sid), None)


def _root_of(row: dict) -> str:
    return user_dir() if row['source'] == 'user' else bundled_dir()


def sample_file_names(row: dict) -> List[str]:
    """이 샘플의 배포 대상 파일(코덱 파일 + DTX) — 마스터는 워커에 안 보낸다."""
    names = [v for v in (row.get('files') or {}).values() if v and v != 'synthetic']
    if row.get('amr-wb-dtx'):
        names.append(row['amr-wb-dtx'])
    return names


def file_path(sid: str, name: str) -> Optional[str]:
    """라이브러리 파일의 절대 경로 — 그 샘플의 마스터·코덱 파일만(이름 검증)."""
    row = get_sample(sid)
    if not row or not name or '/' in name or name.startswith('.') or '..' in name:
        return None
    if name != row['master'] and name not in sample_file_names(row):
        return None
    root = _root_of(row)
    cands = [os.path.join(root, name)]
    if name == row['master']:
        cands.insert(0, os.path.join(root, 'pcm', name))   # 소스 트리 동봉본 — 마스터는 pcm/ 아래
    return next((p for p in cands if os.path.isfile(p)), None)


def files_by_name() -> Dict[str, str]:
    """워커 동기화용 색인 — 파일 이름 → 절대 경로(운영자본이 동봉본을 가린다)."""
    idx: Dict[str, str] = {}
    for row in list_samples():
        root = _root_of(row)
        for n in sample_file_names(row):
            idx[n] = os.path.join(root, n)
    return idx


# ── 변환기 ──────────────────────────────────────────────────────────────────

def converter_path() -> Optional[str]:
    """cims-sample-conv 위치 — 설정 `Tester.SampleConv` > 패키지 `native/` > 개발 트리 `build/bin` > PATH."""
    cfg = str(((store._config.get('Tester') or {}).get('SampleConv') or '')).strip()
    cands = [cfg] if cfg else []
    cands.append(os.path.join(store._component_root, 'native', 'cims-sample-conv'))
    cands.append(os.path.normpath(os.path.join(store._component_root, '..', '..', '..', 'build', 'bin', 'cims-sample-conv')))
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return shutil.which('cims-sample-conv')


def _wav_header_ok(data: bytes) -> Tuple[bool, str]:
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return False, 'RIFF/WAVE 파일이 아니다'
    return True, ''


def run_converter(wav_bytes: bytes, sid: str, out_dir: str, dtx: bool = True, normalize: Optional[float] = None,
                  codecs: Optional[List[str]] = None) -> dict:
    conv = converter_path()
    if not conv:
        raise SampleError('converter_missing', 'cims-sample-conv 가 없다(패키지 native/ 또는 Tester.SampleConv)', 503)
    with tempfile.NamedTemporaryFile(prefix='sample-', suffix='.wav', delete=False) as tf:
        tf.write(wav_bytes)
        src = tf.name
    try:
        cmd = [conv, '--in', src, '--out-dir', out_dir, '--id', sid]
        if codecs:
            cmd += ['--codecs', ','.join(codecs)]
        if not dtx:
            cmd.append('--no-dtx')
        if normalize is not None:
            cmd += ['--normalize', str(normalize)]
        try:
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300, text=True)
        except subprocess.TimeoutExpired:
            raise SampleError('convert_failed', '변환 시간 초과(300 s)', 500)
        try:
            out = json.loads((proc.stdout or '').strip().splitlines()[-1]) if proc.stdout.strip() else {}
        except ValueError:
            out = {}
        if proc.returncode != 0 or not isinstance(out, dict) or 'error' in out:
            raise SampleError('convert_failed', (out or {}).get('error') or (proc.stderr or '').strip()[-300:] or f'rc={proc.returncode}', 400)
        return out
    finally:
        try:
            os.unlink(src)
        except OSError:
            pass


# ── 등록·삭제 ────────────────────────────────────────────────────────────────

def register(sid: str, wav_bytes: bytes, kind: str = 'other', description: str = '', dtx: bool = True,
             normalize: Optional[float] = None, replace: bool = False) -> dict:
    if not _ID_RE.match(sid or ''):
        raise SampleError('bad_id', 'id 는 영숫자로 시작, 영숫자·_.- 64자 이내')
    if kind not in SAMPLE_KINDS:
        raise SampleError('bad_kind', f'kind 는 {list(SAMPLE_KINDS)} 중 하나')
    if not wav_bytes:
        raise SampleError('empty_body', 'WAV 본문이 비었다')
    if len(wav_bytes) > MAX_WAV_BYTES:
        raise SampleError('too_large', f'WAV 는 {MAX_WAV_BYTES // (1024 * 1024)} MB 이하', 413)
    ok, why = _wav_header_ok(wav_bytes)
    if not ok:
        raise SampleError('bad_wav', why)
    if sid in _read_catalog(bundled_dir()):
        raise SampleError('bundled_read_only', '패키지 동봉 샘플 id 는 다시 등록할 수 없다 — 다른 id 로', 409)
    cat = _read_catalog(user_dir())
    if sid in cat and not replace:
        raise SampleError('exists', f'샘플 {sid!r} 가 이미 있다(replace=1 로 교체)', 409)
    os.makedirs(user_dir(), exist_ok=True)
    tmp_dir = tempfile.mkdtemp(prefix='sample-conv-', dir=user_dir())
    try:
        out = run_converter(wav_bytes, sid, tmp_dir, dtx=dtx, normalize=normalize)
        names = [out.get('master') or f'{sid}.wav'] + [v for v in (out.get('files') or {}).values()]
        if out.get('amr-wb-dtx'):
            names.append(out['amr-wb-dtx'])
        for n in names:
            os.replace(os.path.join(tmp_dir, n), os.path.join(user_dir(), n))
    finally:
        shutil.rmtree(tmp_dir, ignore_errors=True)
    rec = {k: out[k] for k in ('duration_s', 'p56_active_level_dbov', 'p56_activity_pct', 'rms_dbov', 'pattern', 'files',
                               'amr-wb-dtx', 'dtx_frames', 'dtx_channel_activity_pct') if k in out}
    rec.update({'kind': kind, 'description': description or '', 'master': out.get('master') or f'{sid}.wav',
                'created': time.strftime('%Y-%m-%dT%H:%M:%S'), 'input': out.get('input'),
                'normalized_dbov': normalize})
    cat[sid] = rec
    _write_user_catalog(cat)
    return _row(sid, rec, 'user')


def delete(sid: str) -> None:
    if sid in _read_catalog(bundled_dir()):
        raise SampleError('bundled_read_only', '패키지 동봉 샘플은 지울 수 없다', 409)
    cat = _read_catalog(user_dir())
    rec = cat.pop(sid, None)
    if rec is None:
        raise SampleError('sample_not_found', sid, 404)
    row = _row(sid, rec, 'user')
    for n in [row['master']] + sample_file_names(row):
        try:
            os.unlink(os.path.join(user_dir(), n))
        except OSError:
            pass
    _write_user_catalog(cat)


# ── 워커 동기화 ──────────────────────────────────────────────────────────────

def _worker_files(health: Optional[dict]) -> Dict[str, int]:
    media = (health or {}).get('media') or {}
    out: Dict[str, int] = {}
    for f in media.get('files_detail') or []:
        if isinstance(f, dict) and f.get('name'):
            out[str(f['name'])] = int(f.get('size') or 0)
    for n in media.get('files') or []:   # 옛 워커 — 크기 없이 이름만
        out.setdefault(str(n), -1)
    return out


def presence(workers, rows: Optional[List[dict]] = None) -> Dict[str, Dict[str, str]]:
    """샘플 id → {워커 이름: ok|partial|missing|unreachable} — 워커 health(files_detail)와 라이브러리 파일(이름+크기) 대조."""
    rows = rows if rows is not None else list_samples()
    idx = files_by_name()
    out: Dict[str, Dict[str, str]] = {r['id']: {} for r in rows}
    for w in workers:
        have = _worker_files(w.health) if w.health else None
        for r in rows:
            if have is None:
                out[r['id']][w.name] = 'unreachable'
                continue
            names = sample_file_names(r)
            ok = 0
            for n in names:
                sz = have.get(n)
                if sz is None:
                    continue
                if sz < 0 or not os.path.isfile(idx.get(n, '')) or sz == os.path.getsize(idx[n]):
                    ok += 1
            out[r['id']][w.name] = 'ok' if names and ok == len(names) else 'partial' if ok else 'missing'
    return out


def _push_missing(w, names: List[str], idx: Dict[str, str]) -> Tuple[List[str], List[str]]:
    """워커에 없거나 크기가 다른 파일만 PUT. 반환 = (pushed, errors)."""
    have = _worker_files(w.health)
    pushed, errors = [], []
    for n in sorted(set(names)):
        path = idx.get(n)
        if not path or not os.path.isfile(path):
            continue   # 라이브러리 밖 파일(운영자가 워커에 직접 둔 것) — 워커의 run 시작 검사가 판정
        size = os.path.getsize(path)
        if have.get(n) == size:
            continue
        try:
            with open(path, 'rb') as f:
                w.put_file(n, f.read())
            pushed.append(n)
        except Exception as e:  # noqa: BLE001 — 워커 하나의 실패가 다른 워커 동기화를 막지 않는다
            errors.append(f'{n}: {e}')
    return pushed, errors


def sync_for_run(workers, samples: Dict[str, Dict[str, str]], notes: Optional[list] = None) -> None:
    """run 시작 전 — 계획이 참조한 샘플 파일(RunStart.samples 의 파일 이름)을 워커마다 대조해 없는 것만 밀어 넣는다."""
    names = [v for m in (samples or {}).values() for v in (m or {}).values() if v and v != 'synthetic']
    if not names:
        return
    idx = files_by_name()
    for w in workers:
        if w.health is None:
            w.probe()
        if w.health is None:
            continue
        pushed, errors = _push_missing(w, names, idx)
        if pushed and notes is not None:
            notes.append(f'samples → {w.name}: ' + ', '.join(pushed))
        if errors:
            raise RuntimeError(f'{w.name} 샘플 배포 실패: ' + '; '.join(errors))


def sync_workers(workers, ids: Optional[List[str]] = None) -> Dict[str, dict]:
    """콘솔 [워커 동기화] — 라이브러리(전부 또는 ids)의 코덱 파일을 토폴로지 워커 전부에 맞춘다."""
    rows = [r for r in list_samples() if not ids or r['id'] in ids]
    names = [n for r in rows for n in sample_file_names(r)]
    idx = files_by_name()
    result: Dict[str, dict] = {}
    for w in workers:
        w.probe()
        if w.health is None:
            result[w.name] = {'error': w.health_error or 'unreachable', 'pushed': []}
            continue
        pushed, errors = _push_missing(w, names, idx)
        result[w.name] = {'pushed': pushed, 'errors': errors}
    return result
