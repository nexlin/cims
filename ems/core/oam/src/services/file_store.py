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

# 소유권 리스 — write 진입점 가드. 관리 store 는 단일 writer 다(oam_ha.md §4.4).
# 미획득 상태에서는 조회는 되고 write 만 LeaseLostError 로 막힌다(read-only 강등).
from services import lease


# ──────────────────────────────────────────────────────────────────────────
#  경로 유틸
# ──────────────────────────────────────────────────────────────────────────

_SHARED_FSTYPES = {'nfs', 'nfs4', 'cifs', 'smb3', 'fuse.sshfs', 'glusterfs', 'ceph', 'lustre'}


def _is_shared_mount(path: str) -> bool:
    """path 를 담고 있는 마운트의 fstype 이 공유 파일시스템인가 (/proc/mounts 최장일치).
    판정 불가(비 Linux·읽기 실패)면 False — 폴백을 막지 않는다."""
    try:
        best, fstype = '', ''
        target = os.path.abspath(path)
        with open('/proc/mounts') as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                mp = parts[1]
                if (target == mp or target.startswith(mp.rstrip('/') + '/')) and len(mp) > len(best):
                    best, fstype = mp, parts[2]
        return fstype in _SHARED_FSTYPES
    except Exception:
        return False


def _writable_dir(path: str) -> bool:
    """이 경로에 store 를 만들 수 있는가 — 존재하면 쓰기 가능, 없으면 생성 가능."""
    try:
        os.makedirs(path, exist_ok=True)
        return os.access(path, os.W_OK)
    except Exception:
        return False


def runtime_root(config: dict) -> str:
    """runtime store 의 base 디렉토리.

    우선순위:
      1. config['CimsRuntimeDir']
      2. ServiceLogging.Dir 의 sibling '../runtime'
      3. ServiceLogDir / MsgLogDir 의 sibling
      4. 현재 CWD 의 'runtime' (개발 fallback)

    2·3 은 로그 디렉터리에서 유도하는 폴백인데, 로그를 공유 스토리지(NAS)에 두는
    구성에서는 **관리 데이터(배포/그룹/에이전트)까지 공유 스토리지로 끌려간다**. 그러면
    노드를 밀어도 관리 데이터가 남고, 무엇보다 **펜싱 없이** 두 OAM 이 같은 store 를
    동시에 write 할 수 있다.

    금지의 대상은 공유 스토리지 자체가 아니라 **펜싱 없는 다중 writer** 다(oam_ha.md §4).
    이중화 구성에서는 (a) mount guard, (b) 소유권 리스(`services.lease`) 2층으로 단일
    writer 를 보장하고, 그때는 운영자가 `CimsRuntimeDir` 을 그 공유 경로로 **명시**한다. 따라서 명시 경로는 존중하고(리스가
    보호), **유도된 폴백이 공유 마운트로 끌려가는 사고**만 기동 실패로 막는다.
    """
    explicit = config.get('CimsRuntimeDir')
    if explicit:
        # **쓸 수 있는 경로인지 확인한다.** 패키지 기본값이나 옛 배포 overlay 에 다른 머신의
        # 절대경로가 들어 있으면(실측: 빌드 머신 경로 `/home/<user>/work/...` 가 패키지
        # oam.json 에 커밋돼 배포됨) OAM 은 그 경로에 makedirs 하다 PermissionError 로 죽고,
        # 콘솔이 사라져 설정을 고칠 통로까지 없어진다. 접근 불가면 **노드 로컬로 폴백**한다.
        #
        # 단, 공유 마운트 구성(`CimsRuntimeMount` 설정)에서는 폴백하지 않는다 — 마운트가
        # 잠깐 없다고 로컬로 갈아타면 store 가 갈라진다. 그 판정은 mount guard 의 몫이다.
        if _writable_dir(explicit) or config.get('CimsRuntimeMount'):
            return explicit
        from services import paths as _p
        fallback = _p.local_runtime_dir(config)
        print(f'[store] ⚠ CimsRuntimeDir={explicit} 를 쓸 수 없습니다(권한/경로). '
              f'노드 로컬 {fallback} 로 폴백합니다 — 관리평면이 기동하지 못하는 것보다 안전합니다. '
              f'콘솔에서 경로를 고치세요.', flush=True)
        return fallback
    # ── 폴백은 **버전무관 노드 로컬 경로**로 고정한다 ──────────────────────────
    # 옛 폴백(로그 디렉터리 sibling → cwd/runtime)은 OAM 의 cwd 가 **버전 디렉터리**
    # (modules/oam/<ver>/oam/src)라서 store 를 `.../current/ext_mnt/runtime` 같은 위치에
    # 만들었다(실서버 실측). 그 위치는 **oam 업그레이드 시 통째로 사라진다** — 관리
    # 데이터(에이전트·배포·그룹·패키지)를 잃는 경로다. 설정 누락은 흔한 일이므로
    # (배포 overlay 에 CimsRuntimeDir 이 주입되지 않은 노드가 실제로 있었다) 폴백 자체가
    # 안전해야 한다: services.paths 가 계산하는 `modules/oam/runtime` 로 고정한다.
    from services import paths as _paths
    cand = _paths.local_runtime_dir(config)
    if _is_shared_mount(cand):
        raise RuntimeError(
            f"runtime store 폴백이 공유 스토리지로 유도됨: {cand}\n"
            f"  펜싱 없이 두 OAM 이 같은 store 를 write 하면 손상된다.\n"
            f"  해결: 모듈 설정 CimsRuntimeDir 을 명시하라 — 단일 노드는 로컬 경로,"
            f" 이중화는 공유 store 마운트 하위 경로(+ CimsRuntimeMount).")
    return cand


# runtime store v2 P2 — OAM 자기 데이터 카테고리화.
#   control/ = OAM 관리평면, console/ = OAM 콘솔. (서비스 모듈 컬렉션은 ha_lookup
#   .collection_dir 로 modules/<owner>/runtime/collections 에 별도 — 여기 미포함.)
#   효과: runtime 루트 평면에 성격이 다른 도메인이 섞이는 문제 해소(백업/권한/정책 분리).
_OAM_CATEGORY = {
    'agents': 'control', 'deployments': 'control', 'jobs': 'control',
    'metrics': 'control', 'packages': 'control', 'ha_groups': 'control',
    'csp_sync_txn': 'control', 'gateway_routes': 'control',
    # 계획 절체 operation — 관리평면 이중화에서 **신 Active 가 이어받아야 하는** 상태다
    # (source 가 자기 자신을 정지시키므로). control/ 로 묶어 백업·복제 범위를 통일한다.
    'ha_operations': 'control',
    'console_accounts': 'console', 'console_layouts': 'console', 'console_menu': 'console',
    'console_user_layouts': 'console',
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
    """JSON 1건 atomic write. obj['update_time'] 자동 갱신.

    소유권 리스가 없으면 `LeaseLostError` — 관리 store 는 단일 writer 다(oam_ha.md §4.4)."""
    lease.assert_writable()
    obj = dict(obj)
    now = datetime.now().isoformat(timespec='seconds')
    obj.setdefault('create_time', now)
    obj['update_time'] = now
    path = os.path.join(dir_path, _safe_key(key) + '.json')
    _atomic_write_json(path, obj)


def delete(dir_path: str, key) -> bool:
    """삭제. 존재했으면 True, 없으면 False. (리스 필요 — save 와 동일)"""
    lease.assert_writable()
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
    """단조 증가 다음 ID. .seq 미존재 시 디렉토리 스캔으로 max(id)+1 시드.

    ID 발급도 write 다(.seq 갱신) — 리스 없이 발급하면 두 writer 가 같은 ID 를 준다."""
    lease.assert_writable()
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
    """id 로 1건 조회.

    **파일명 우선(`<id>.json`) → 없을 때만 전체 스캔.** 우리 도메인은 `save(dir, id, obj)`
    로 id 를 파일명에 쓰므로 거의 항상 직접 히트한다. 옛 구현은 무조건 `find_by` 로 디렉터리
    전체를 읽었고, store 가 공유 스토리지(NFS, 파일당 ~5ms)로 옮겨간 뒤 이것이 최대 비용이
    됐다 — 예: job 120건 store 에서 단건 조회 1회가 120파일 읽기. 그것이 heartbeat·조회마다
    반복돼 콘솔 전체가 느려졌다(실측).

    파일명이 자연키(id 아님)인 도메인은 스캔 폴백이 그대로 커버한다.
    """
    direct = load(dir_path, target_id)
    if isinstance(direct, dict) and direct.get('id') == target_id:
        return direct
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
    """JSONL 한 줄 append. dt 미지정 시 now. POSIX append-atomic 한 단일 write 보장.

    시계열 append 도 store write 이므로 리스가 필요하다(oam_ha.md §4.4)."""
    lease.assert_writable()
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
