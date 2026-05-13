"""
File-based runtime store — 가입자 외 도메인 (배포/HA/런타임 설정 등) 의 SoT.

설계: docs/design/runtime_store_design.md

기본 사용:

    from services import file_store

    pkgs_dir = file_store.domain_dir(config, 'packages')
    file_store.save(pkgs_dir, 'csp__0.1.4', {'id': 7, 'name': 'csp', 'version': '0.1.4', ...})
    pkg = file_store.load(pkgs_dir, 'csp__0.1.4')
    all_pkgs = file_store.load_all(pkgs_dir)
    nxt = file_store.next_id(pkgs_dir)
    file_store.delete(pkgs_dir, 'csp__0.1.4')

각 도메인 디렉토리에 `.seq` 파일이 단조 증가 ID 시퀀스를 보관 (flock 으로 보호).
모든 write 는 atomic (tmp + rename).
"""
from __future__ import annotations

import json
import os
import glob
import fcntl
import tempfile
from datetime import datetime
from typing import Optional


# ──────────────────────────────────────────────────────────────────────────
#  경로 유틸
# ──────────────────────────────────────────────────────────────────────────

def runtime_root(config: dict) -> str:
    """runtime store 의 base 디렉토리.

    우선순위:
      1. config['CimsRuntimeDir']
      2. ServiceLogging.Dir 의 sibling '../runtime'
      3. ServiceLogDir / MsgLogDir 의 sibling
      4. 현재 CWD 의 'runtime' (개발 fallback)
    """
    explicit = config.get('CimsRuntimeDir')
    if explicit:
        return explicit
    sl = config.get('ServiceLogging', {}).get('Dir') or config.get('ServiceLogDir') \
        or config.get('MsgLogDir', '')
    if sl:
        return os.path.normpath(os.path.join(sl, '..', 'runtime'))
    return os.path.abspath('runtime')


def domain_dir(config: dict, domain: str) -> str:
    """도메인 디렉토리. 없으면 생성."""
    path = os.path.join(runtime_root(config), domain)
    os.makedirs(path, exist_ok=True)
    return path


# ──────────────────────────────────────────────────────────────────────────
#  Atomic write / read
# ──────────────────────────────────────────────────────────────────────────

def _atomic_write_json(path: str, obj: dict) -> None:
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix='.tmp_', dir=d)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(obj, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


def _read_json(path: str) -> Optional[dict]:
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None


# ──────────────────────────────────────────────────────────────────────────
#  CRUD
# ──────────────────────────────────────────────────────────────────────────

def _safe_key(key) -> str:
    """파일명으로 안전하지 않은 문자를 _로 치환. 절대경로/.. 차단."""
    s = str(key)
    for c in '/\\:*?"<>|':
        s = s.replace(c, '_')
    if s.startswith('.'):
        s = '_' + s[1:]
    return s


def load(dir_path: str, key) -> Optional[dict]:
    """key 에 해당하는 JSON 1건 로드 (없으면 None)."""
    path = os.path.join(dir_path, _safe_key(key) + '.json')
    return _read_json(path)


def load_all(dir_path: str) -> list:
    """디렉토리 내 모든 *.json 로드. .seq / .tmp_* 등은 제외."""
    if not os.path.isdir(dir_path):
        return []
    results = []
    for path in sorted(glob.glob(os.path.join(dir_path, '*.json'))):
        if os.path.basename(path).startswith('.'):
            continue
        obj = _read_json(path)
        if obj is not None:
            results.append(obj)
    return results


def save(dir_path: str, key, obj: dict) -> None:
    """JSON 1건 atomic write. obj['update_time'] 자동 갱신."""
    obj = dict(obj)
    now = datetime.now().isoformat(timespec='seconds')
    obj.setdefault('create_time', now)
    obj['update_time'] = now
    path = os.path.join(dir_path, _safe_key(key) + '.json')
    _atomic_write_json(path, obj)


def delete(dir_path: str, key) -> bool:
    """삭제. 존재했으면 True, 없으면 False."""
    path = os.path.join(dir_path, _safe_key(key) + '.json')
    try:
        os.unlink(path)
        return True
    except FileNotFoundError:
        return False


def exists(dir_path: str, key) -> bool:
    return os.path.exists(os.path.join(dir_path, _safe_key(key) + '.json'))


# ──────────────────────────────────────────────────────────────────────────
#  ID 시퀀스 (.seq, flock 보호)
# ──────────────────────────────────────────────────────────────────────────

def next_id(dir_path: str) -> int:
    """단조 증가 다음 ID. .seq 미존재 시 디렉토리 스캔으로 max(id)+1 시드."""
    os.makedirs(dir_path, exist_ok=True)
    seq_path = os.path.join(dir_path, '.seq')
    with open(seq_path, 'a+') as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        try:
            f.seek(0)
            content = f.read().strip()
            if content.isdigit():
                cur = int(content)
            else:
                cur = _scan_max_id(dir_path)
            nxt = cur + 1
            f.seek(0)
            f.truncate()
            f.write(str(nxt))
            f.flush()
            os.fsync(f.fileno())
            return nxt
        finally:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)


def seed_seq(dir_path: str, value: int) -> None:
    """이미 마이그레이션 데이터가 있어 seq 를 특정 값으로 강제 시드. atomic."""
    os.makedirs(dir_path, exist_ok=True)
    seq_path = os.path.join(dir_path, '.seq')
    with open(seq_path, 'w') as f:
        fcntl.flock(f.fileno(), fcntl.LOCK_EX)
        try:
            f.write(str(int(value)))
            f.flush()
            os.fsync(f.fileno())
        finally:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)


def _scan_max_id(dir_path: str) -> int:
    """디렉토리 내 *.json 의 'id' 필드 최대값. 없으면 0."""
    mx = 0
    for path in glob.glob(os.path.join(dir_path, '*.json')):
        if os.path.basename(path).startswith('.'):
            continue
        obj = _read_json(path)
        if obj and isinstance(obj.get('id'), int):
            if obj['id'] > mx:
                mx = obj['id']
    return mx


# ──────────────────────────────────────────────────────────────────────────
#  편의 함수
# ──────────────────────────────────────────────────────────────────────────

def find_by(dir_path: str, predicate) -> Optional[dict]:
    """predicate(obj) → True 첫 매칭. 없으면 None."""
    for obj in load_all(dir_path):
        if predicate(obj):
            return obj
    return None


def find_all_by(dir_path: str, predicate) -> list:
    return [obj for obj in load_all(dir_path) if predicate(obj)]


def by_id(dir_path: str, target_id: int) -> Optional[dict]:
    """id 필드로 검색 (파일명이 자연키일 때 사용)."""
    return find_by(dir_path, lambda o: o.get('id') == target_id)
