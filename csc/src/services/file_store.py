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


# runtime store v2 P2 — OAM 자기 데이터 카테고리화.
#   control/ = OAM 관리평면, console/ = OAM 콘솔. (서비스 모듈 컬렉션은 ha_lookup
#   .collection_dir 로 modules/<owner>/runtime/collections 에 별도 — 여기 미포함.)
#   효과: runtime 루트 평면에 성격이 다른 도메인이 섞이는 문제 해소(백업/권한/정책 분리).
_OAM_CATEGORY = {
    'agents': 'control', 'deployments': 'control', 'jobs': 'control',
    'metrics': 'control', 'packages': 'control', 'ha_groups': 'control',
    'csp_sync_txn': 'control',
    'console_accounts': 'console', 'console_layouts': 'console', 'console_menu': 'console',
}


def _domain_rel(domain: str) -> str:
    cat = _OAM_CATEGORY.get(domain)
    return os.path.join(cat, domain) if cat else domain


def domain_dir(config: dict, domain: str) -> str:
    """도메인 디렉토리. 없으면 생성. OAM 자기 도메인은 control/·console/ 로 카테고리화."""
    path = os.path.join(runtime_root(config), _domain_rel(domain))
    os.makedirs(path, exist_ok=True)
    return path


def migrate_oam_categories(config: dict) -> int:
    """1회 이행 — 구 평면 {runtime}/<domain> 의 OAM 자기 데이터를 control/·console/ 로 이동.
    디렉터리 엔트리(파일/하위트리)를 rename 으로 옮기므로 nested(jobs/metrics) 보존·원자적.
    이미 카테고리화된 dst 가 있으면 merge(없는 엔트리만 이동). 이동한 도메인 수 반환."""
    rt = runtime_root(config)
    moved = 0
    for domain, cat in _OAM_CATEGORY.items():
        flat = os.path.join(rt, domain)
        if not os.path.isdir(flat):
            continue
        newp = os.path.join(rt, cat, domain)
        if os.path.abspath(flat) == os.path.abspath(newp):
            continue
        entries = os.listdir(flat)
        if entries:
            os.makedirs(newp, exist_ok=True)
            for e in entries:
                src, dst = os.path.join(flat, e), os.path.join(newp, e)
                if not os.path.exists(dst):
                    os.rename(src, dst)
            moved += 1
        try:
            os.rmdir(flat)
        except OSError:
            pass
    return moved


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


# ──────────────────────────────────────────────────────────────────────────
#  시계열 JSONL (jobs / metrics 등 append-mostly)
# ──────────────────────────────────────────────────────────────────────────

def jsonl_path(domain_path: str, key: str, dt: datetime) -> str:
    """일별 jsonl 경로 — {domain_path}/<key>/YYYY/MM/DD.jsonl"""
    return os.path.join(
        domain_path, str(key),
        f"{dt.year:04d}", f"{dt.month:02d}", f"{dt.day:02d}.jsonl"
    )


def jsonl_append(domain_path: str, key: str, record: dict, dt: datetime = None) -> str:
    """JSONL 한 줄 append. dt 미지정 시 now. POSIX append-atomic 한 단일 write 보장."""
    dt = dt or datetime.now()
    path = jsonl_path(domain_path, key, dt)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    line = json.dumps(record, ensure_ascii=False) + '\n'
    with open(path, 'a', encoding='utf-8') as f:
        f.write(line)
    return path


def jsonl_iter_recent(domain_path: str, key: str, days: int = 7):
    """최근 N일 jsonl 파일을 시간 역순(최근 일자 우선) 으로 yield. 각 파일 안에서는 순서 그대로."""
    from datetime import timedelta
    today = datetime.now().date()
    for i in range(days):
        d = today - timedelta(days=i)
        path = jsonl_path(domain_path, key, datetime(d.year, d.month, d.day))
        if not os.path.exists(path):
            continue
        try:
            with open(path, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        yield json.loads(line)
                    except Exception:
                        continue
        except Exception:
            continue


def jsonl_last(domain_path: str, key: str, days: int = 2):
    """최근 일자 jsonl 의 마지막(=최신) 유효 레코드 1건 반환, 없으면 None.
    파일 끝에서 일부만 seek-read(tail) 하므로 2s 케이던스 대용량 파일에도 저렴.
    파일 내 레코드는 append 순서(시간순)라 마지막 줄이 최신."""
    from datetime import timedelta
    today = datetime.now().date()
    for i in range(max(1, days)):
        d = today - timedelta(days=i)
        path = jsonl_path(domain_path, key, datetime(d.year, d.month, d.day))
        if not os.path.exists(path):
            continue
        try:
            with open(path, 'rb') as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 65536))
                data = f.read().decode('utf-8', 'ignore')
        except Exception:
            continue
        for line in reversed(data.splitlines()):
            line = line.strip()
            if not line:
                continue
            try:
                return json.loads(line)
            except Exception:
                continue
    return None


def _tail_records(path: str, need: int) -> list:
    """파일 끝에서부터 최신 레코드를 need 개 모을 때까지 tail-read (필요 시 윈도 확장).
    반환은 최신 우선(newest-first). 파일 내 레코드는 append 순서(시간순)라 마지막 줄이 최신."""
    try:
        with open(path, 'rb') as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            window = 65536
            while True:
                start = max(0, size - window)
                f.seek(start)
                data = f.read().decode('utf-8', 'ignore')
                lines = data.splitlines()
                # 윈도가 파일 중간에서 시작하면 첫 줄이 잘렸을 수 있어 버림
                if start > 0 and lines:
                    lines = lines[1:]
                recs = []
                for line in reversed(lines):
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        recs.append(json.loads(line))
                    except Exception:
                        continue
                    if len(recs) >= need:
                        break
                if len(recs) >= need or start == 0:
                    return recs
                window *= 4   # 아직 부족 — 더 거슬러 읽기
    except Exception:
        return []


def jsonl_tail_recent(domain_path: str, key: str, limit: int = 120, days: int = 7):
    """최근 일자 jsonl 에서 최신 limit 개 레코드를 최신 우선(newest-first) 으로 반환.
    각 파일을 끝에서부터 tail-read 하고 limit 를 채우면 조기 종료 — 7일 전체를
    파싱하던 jsonl_iter_recent + list() 대비 훨씬 저렴 (2s 케이던스 대용량 파일 대응)."""
    from datetime import timedelta
    out: list = []
    today = datetime.now().date()
    for i in range(max(1, days)):
        if len(out) >= limit:
            break
        d = today - timedelta(days=i)
        path = jsonl_path(domain_path, key, datetime(d.year, d.month, d.day))
        if not os.path.exists(path):
            continue
        out.extend(_tail_records(path, limit - len(out)))
    return out[:limit]


def jsonl_purge_old(domain_path: str, retain_days: int) -> int:
    """domain 의 모든 key 에 대해 retain_days 보다 오래된 일별 jsonl 파일 삭제.
    레이아웃 {domain_path}/<key>/YYYY/MM/DD.jsonl 의 날짜를 경로에서 파싱.
    삭제한 파일 수 반환. 빈 디렉토리는 그대로 둠 (다음 append 가 재사용)."""
    from datetime import timedelta
    if retain_days < 1 or not os.path.isdir(domain_path):
        return 0
    cutoff = datetime.now().date() - timedelta(days=retain_days)
    removed = 0
    # {domain}/<key>/YYYY/MM/DD.jsonl — glob 으로 날짜 깊이만 매칭 (다른 파일 보호)
    pattern = os.path.join(domain_path, '*', '[0-9]' * 4, '[0-9]' * 2, '[0-9]' * 2 + '.jsonl')
    for path in glob.glob(pattern):
        try:
            dd = os.path.basename(path)[:2]
            mm = os.path.basename(os.path.dirname(path))
            yyyy = os.path.basename(os.path.dirname(os.path.dirname(path)))
            fdate = datetime(int(yyyy), int(mm), int(dd)).date()
        except (ValueError, IndexError):
            continue
        if fdate < cutoff:
            try:
                os.remove(path)
                removed += 1
            except OSError:
                continue
    return removed
