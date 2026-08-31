"""csc_logger.py — CSC 서비스 로그 + 메시지 로그 유틸리티 (통합 포맷)

포맷 (5분 버킷 — CSP/CMP 와 동일; mm5=00/05/.../55):
  Flow: {ServiceLogDir}/YYYY/MM/DD/HH/csc_01.flow.{mm5}.jsonl
  Msg : {ServiceLogDir}/YYYY/MM/DD/HH/csc_01_{iface}.msg.{mm5}.jsonl

공통 키: ts, node, service, from, to, proto, method, detail, mid, sesid, subid, seq, iface
sesid 포맷: {caller}::{module}::{yyyymmddHHMMSSuuuuuu}::{counter}

저장 경로 무의존(non-blocking) 계약 — flow_logging.md §2 (CSP/CMP/CMDP 와 동일 2단 writer):
  - 생산자(log_flow 등)는 파일시스템 호출 없이 포맷+seq 부여+큐 적재만 한다.
  - dispatch 스레드가 큐를 소비해 목적지를 정한다. 저장소가 건강하면 NAS flusher 큐로,
    아니면 로컬 스풀(SpoolDir)에 기록한다 — dispatch 도 저장 경로를 만지지 않는다.
  - NAS flusher 스레드만 저장 경로 I/O 를 수행한다. 쓰기 실패(fail-fast)·in-flight 정체
    (StallSec 초과, NFS hard mount 행)·flusher 큐 포화 시 폴백이 걸리고, 회복되면 스풀을
    경로별 줄 순서대로 재생(replay)한 뒤 직행으로 복귀한다 (seq↔줄번호 정합 보존).
  - 폴백 진입/회복은 A-PRC-006 storage_failure 알람으로 자기보고한다 (fm_reporter 경유).
    공유 마운트를 아직 붙이지 않은 부트스트랩 직후에도 요청마다 에러를 찍지 않는다 —
    전이 시 1회 알리고 스풀에 남겼다가 마운트가 붙으면 재기동 없이 재생한다.
"""

import os
import time as _time
import json
import atexit
import threading
import collections
from datetime import datetime
from glob import glob

_service_log_dir: str = ""
_system_id: str = "csc_01"
_lock = threading.Lock()

# iface:msg_path → 누적 seq 카운터 (프로세스 재시작 시 flusher 시딩 결과로 복원)
_seq_map: dict = {}

# ── 2단 비동기 writer (dispatch + NAS flusher + 로컬 스풀 폴백) ────────────────
_LOG_FLUSH_INTERVAL = 0.1     # dispatch 주기 flush
_LOG_NOTIFY_THRESHOLD = 128   # 큐가 이만큼 쌓이면 즉시 flush 깨움
_LOG_MAX_QUEUE = 200000       # 큐 상한 (스풀까지 막힌 극단 상황의 메모리 폭주 방지)
_NAS_QUEUE_MAX = 8            # dispatch→flusher 대기 배치 상한 (포화 = 저장 경로 지연 신호)
_REPLAY_RETRY_SEC = 2.0       # 스풀 재생 실패 후 재시도 간격
_SPOOL_TRIM_INTERVAL = 5.0    # 스풀 용량 정리 최소 간격
_STOP_FLUSHER_WAIT = 2.0      # 정지 시 flusher 종료 대기 상한 (초과 시 방치 — daemon 스레드)

_write_queue: "collections.deque" = collections.deque()
_q_cond = threading.Condition()
_writer_thread = None
_flusher_thread = None
_writer_running = False
_dropped_logs = 0

# dispatch ↔ flusher 공유 상태 (_store['lock'] 보호 항목은 주석 표기)
_store = {
    'lock': threading.Lock(),
    'cv': None,                # threading.Condition(lock) — init 에서 생성
    'nas_queue': collections.deque(),  # 직행 대기 배치 (lock)
    'inflight': [],            # flusher 가 기록 중인 배치 (lock — 정지 시 스풀 회수용)
    'run': True,
    'exited': False,
    'nas_healthy': True,       # 저장 경로 판정 (dispatch 라우팅 기준)
    'spool_pending': False,    # 스풀 잔량 존재 — 드레인 전 직행 금지 (순서 보존)
    'op_start': 0.0,           # 저장 경로 op 시작 시각 (0=idle, monotonic) — 정체 감지
    'last_op_ok': True,
    'spool_bytes': 0,
    'spooled_lines': 0,
    'replayed_lines': 0,
    'dropped_lines': 0,
    'last_error': '',          # (lock)
    'seed': {},                # msg_path → 기동 시점 줄 수 (flusher 가 채움)
    'seed_done': False,
    # 불변 설정 (init 에서 확정)
    'spool_dir': 'spool',
    'stall_sec': 5,
    'spool_max_bytes': 1024 * 1024 * 1024,
    'base_dir': '',
    'seed_hour_dir': '',
    'seed_mm5': '',
}
_degraded = False          # 폴백 상태 전이 추적 (dispatch 단독 접근)
_last_spool_trim = 0.0


def _enqueue(path: str, line: str):
    """포맷 완료된 한 줄을 파일경로와 함께 큐에 적재 — 파일 I/O 없이 즉시 반환."""
    if not path or not line:
        return
    global _dropped_logs
    with _q_cond:
        if len(_write_queue) >= _LOG_MAX_QUEUE:
            _write_queue.popleft()
            _dropped_logs += 1
        _write_queue.append((path, line))
        if len(_write_queue) >= _LOG_NOTIFY_THRESHOLD:
            _q_cond.notify()


def _group_batch(batch):
    """파일경로별 병합 — 같은 경로의 줄은 batch 순서(=enqueue/seq 순서)대로 누적."""
    groups: dict = {}
    for path, line in batch:
        g = groups.setdefault(path, ["", 0])
        g[0] += line
        g[1] += 1
    return groups


# ── 스풀 (로컬 디스크 — 실패는 즉시 반환, 행 없음) ────────────────────────────

def _spool_path_for(target: str) -> str:
    """목적 경로 → 스풀 미러 경로 ({spool}/abs{path} | {spool}/rel/{path})"""
    if target.startswith('/'):
        return _store['spool_dir'] + '/abs' + target
    return _store['spool_dir'] + '/rel/' + target


def _target_path_for(spool_file: str) -> str:
    """스풀 미러 경로 → 목적 경로 (역변환. 실패 시 빈 문자열)"""
    abs_prefix = _store['spool_dir'] + '/abs/'
    rel_prefix = _store['spool_dir'] + '/rel/'
    if spool_file.startswith(abs_prefix):
        return spool_file[len(abs_prefix) - 1:]   # 선행 '/' 유지
    if spool_file.startswith(rel_prefix):
        return spool_file[len(rel_prefix):]
    return ''


def _scan_spool(d: str, out: list):
    """스풀 트리 재귀 스캔 — (경로, mtime, size) 수집."""
    try:
        for name in os.listdir(d):
            p = os.path.join(d, name)
            try:
                st = os.stat(p)
            except OSError:
                continue
            if os.path.isdir(p):
                _scan_spool(p, out)
            else:
                out.append((p, st.st_mtime, st.st_size))
    except OSError:
        pass


def _count_lines(path: str) -> int:
    try:
        with open(path, 'rb') as f:
            return sum(chunk.count(b'\n') for chunk in iter(lambda: f.read(65536), b''))
    except OSError:
        return 0


def _ensure_dir(path: str):
    # 공유 NAS(NFS)에서 여러 프로세스가 같은 시각 버킷 디렉터리를 동시 생성하면
    # 속성 캐시 레이스로 FileExistsError 가 새어나온다. 이미 존재하면 성공으로 간주.
    try:
        os.makedirs(path, exist_ok=True)
    except FileExistsError:
        pass


def _spool_append(target: str, data: str, n_lines: int) -> bool:
    """한 목적 경로 분량을 스풀 미러 파일에 append (로컬). 실패 시 폐기 계수."""
    s = _store
    sp = _spool_path_for(target)
    try:
        _ensure_dir(os.path.dirname(sp))
        with open(sp, 'a', encoding='utf-8') as f:
            f.write(data)
        s['spool_bytes'] += len(data.encode('utf-8', 'replace'))
        s['spooled_lines'] += n_lines
        s['spool_pending'] = True
        return True
    except OSError:
        s['dropped_lines'] += n_lines   # 로컬 스풀마저 실패 (디스크 풀 등)
        return False


def _spool_batch(batch):
    for target, (data, n) in _group_batch(batch).items():
        _spool_append(target, data, n)
    _trim_spool_if_needed()


def _trim_spool_if_needed():
    """스풀 용량 상한 초과 시 오래된 스풀 파일부터 폐기 (dispatch 전용, 주기 제한)."""
    global _last_spool_trim
    s = _store
    if s['spool_bytes'] <= s['spool_max_bytes']:
        return
    now = _time.monotonic()
    if now - _last_spool_trim < _SPOOL_TRIM_INTERVAL:
        return
    _last_spool_trim = now
    files: list = []
    _scan_spool(s['spool_dir'], files)
    files = [f for f in files if not f[0].endswith('.replay')]   # 재생 중 파일은 제외
    files.sort(key=lambda f: f[1])
    target = s['spool_max_bytes'] * 9 // 10
    for path, _mt, size in files:
        if s['spool_bytes'] <= target:
            break
        n = _count_lines(path)
        try:
            os.unlink(path)
            s['spool_bytes'] -= size
            s['dropped_lines'] += n
            print(f"[service-log] 스풀 용량 초과 — 폐기 {path} ({n}줄)", flush=True)
        except OSError:
            pass


# ── dispatch 스레드 (저장 경로 무접촉 — 항상 join 가능) ───────────────────────

def _reclaim_nas_queue_to_spool():
    """폴백 진입 시 flusher 큐 잔량(스풀 내용보다 오래된 분)을 FIFO 로 먼저 스풀에 회수."""
    s = _store
    with s['lock']:
        pending = list(s['nas_queue'])
        s['nas_queue'].clear()
    for b in pending:
        _spool_batch(b)


def _route_batch(batch, force_spool_on_backpressure: bool) -> bool:
    """배치 라우팅: 직행(nas_queue) | 폴백(회수 후 스풀). False = 큐 포화 역압
    (배치를 _write_queue 앞으로 되돌림 — 호출자는 한 tick 쉬고 재시도)."""
    if not batch:
        return True
    s = _store
    fallback = (not s['nas_healthy']) or s['spool_pending']
    if not fallback:
        queued = False
        with s['lock']:
            if len(s['nas_queue']) < _NAS_QUEUE_MAX:
                s['nas_queue'].append(batch)
                queued = True
            if queued:
                s['cv'].notify()
        if queued:
            return True
        if not force_spool_on_backpressure:
            with _q_cond:
                _write_queue.extendleft(reversed(batch))
            return False
    _reclaim_nas_queue_to_spool()
    _spool_batch(batch)
    return True


def _reconcile_degrade():
    """폴백 상태 전이 정리 — 전이 시에만 로그 + A-PRC-006 open/close (dispatch 단독)."""
    global _degraded
    s = _store
    degraded = (not s['nas_healthy']) or s['spool_pending']
    if degraded == _degraded:
        return
    _degraded = degraded
    with s['lock']:
        reason = s['last_error'] or 'spool backlog'
    if degraded:
        print(f"[service-log] 저장 경로 폴백 전환 — 로컬 스풀로 우회 ({reason}, "
              f"spool={s['spool_dir']}). 회복되면 자동 재생됩니다.", flush=True)
    else:
        print(f"[service-log] 저장 경로 회복 — 스풀 재생 완료 (spooled={s['spooled_lines']}, "
              f"replayed={s['replayed_lines']}, dropped={s['dropped_lines']})", flush=True)
    try:
        from services import fm_reporter
        fm = fm_reporter.get()
        if fm:
            mo = f"{_system_id}/csc/service_log"
            if degraded:
                fm.alarm_open('A-PRC-006', mo, params={
                    'path': _service_log_dir, 'reason': reason,
                    'spooled': s['spooled_lines'], 'dropped': s['dropped_lines']})
            else:
                fm.alarm_close('A-PRC-006', mo)
    except Exception:
        pass


def _writer_loop():
    global _write_queue
    s = _store
    while _writer_running:
        batch = None
        with _q_cond:
            if not _write_queue:
                _q_cond.wait(_LOG_FLUSH_INTERVAL)
            if _write_queue:
                batch = list(_write_queue)
                _write_queue.clear()

        # 정체 감지: flusher 가 저장 경로 op 에 StallSec 이상 갇혀 있으면 무응답 판정.
        op_start = s['op_start']
        if op_start and _time.monotonic() - op_start > s['stall_sec'] and s['nas_healthy']:
            s['nas_healthy'] = False
            s['last_op_ok'] = False
            with s['lock']:
                s['last_error'] = f"stall: store op in-flight > {s['stall_sec']}s"

        if batch:
            if not _route_batch(batch, False):
                _time.sleep(_LOG_FLUSH_INTERVAL)   # 역압 — 한 tick 쉬고 재시도
        _reconcile_degrade()
    # 종료 — 잔여 큐 회수. 역압이면 스풀로 강제 회수해 어떤 경우에도 막히지 않는다.
    while True:
        with _q_cond:
            batch = list(_write_queue)
            _write_queue.clear()
        if not batch:
            break
        _route_batch(batch, True)


# ── NAS flusher 스레드 (저장 경로 I/O 전담) ───────────────────────────────────

def _flush_batch_to_store(batch):
    """배치를 저장 경로에 기록. 실패 그룹부터는 스풀로 우회 적재."""
    s = _store
    failed = False
    for target, (data, n) in _group_batch(batch).items():
        if failed:
            _spool_append(target, data, n)
            continue
        s['op_start'] = _time.monotonic()
        try:
            _ensure_dir(os.path.dirname(target))
            with open(target, 'a', encoding='utf-8') as f:
                f.write(data)
            s['op_start'] = 0.0
            s['last_op_ok'] = True
        except OSError as e:
            s['op_start'] = 0.0
            failed = True
            s['last_op_ok'] = False
            s['nas_healthy'] = False
            with s['lock']:
                s['last_error'] = f"write failed: {e}"
            _spool_append(target, data, n)


def _replay_spool_one():
    """스풀에서 가장 오래된 파일 하나를 저장 경로로 재생. 스풀이 비면 pending 해제."""
    s = _store
    files: list = []
    _scan_spool(s['spool_dir'], files)
    replays = [f for f in files if f[0].endswith('.replay')]
    cands = replays if replays else files
    if not cands:
        # 스풀 드레인 완료 — 직행 복귀 (전이 통지는 dispatch 가 수행)
        s['spool_pending'] = False
        s['spool_bytes'] = 0
        if s['last_op_ok']:
            s['nas_healthy'] = True
        return False
    pick = min(cands, key=lambda f: f[1])[0]
    if not pick.endswith('.replay'):
        try:
            os.rename(pick, pick + '.replay')
        except OSError:
            return True
        pick = pick + '.replay'
    try:
        with open(pick, 'r', encoding='utf-8', errors='replace') as f:
            data = f.read()
    except OSError:
        return True   # 경쟁 삭제 등 — 다음 tick 재평가
    size = len(data.encode('utf-8', 'replace'))
    n_lines = data.count('\n')
    target = _target_path_for(pick[:-len('.replay')])
    if not target:
        try:
            os.unlink(pick)
            s['spool_bytes'] -= size
            s['dropped_lines'] += n_lines
        except OSError:
            pass
        return True
    s['op_start'] = _time.monotonic()
    try:
        _ensure_dir(os.path.dirname(target))
        with open(target, 'a', encoding='utf-8') as f:
            f.write(data)
        s['op_start'] = 0.0
        os.unlink(pick)
        s['spool_bytes'] -= size
        s['replayed_lines'] += n_lines
        s['last_op_ok'] = True
    except OSError as e:
        s['op_start'] = 0.0
        s['last_op_ok'] = False
        s['nas_healthy'] = False
        with s['lock']:
            s['last_error'] = f"replay failed: {e}"
    return True


def _nas_flusher_loop():
    s = _store
    # 기동 작업 ①: base 디렉터리 보장 (저장 경로 최초 접촉 — 여기서 행이면 여기만 갇힌다)
    s['op_start'] = _time.monotonic()
    if s['base_dir']:
        try:
            _ensure_dir(s['base_dir'])
        except OSError:
            pass
    # 기동 작업 ②: 시딩 — 기동 시점 버킷 msg 파일들의 기존 줄 수(+스풀 잔량) 계수.
    #   생산자(_next_seq)가 첫 write 에서 합류해 재기동 seq 연속성을 잇는다.
    seed = {}
    if s['seed_hour_dir']:
        pat = os.path.join(s['seed_hour_dir'], f"{_system_id}_*.msg.{s['seed_mm5']}.jsonl")
        try:
            for p in glob(pat):
                seed[p] = _count_lines(p)
        except OSError:
            pass
        mirror_pat = _spool_path_for(os.path.join(
            s['seed_hour_dir'], f"{_system_id}_*.msg.{s['seed_mm5']}.jsonl"))
        try:
            for sp in glob(mirror_pat) + glob(mirror_pat + '.replay'):
                t = _target_path_for(sp[:-len('.replay')] if sp.endswith('.replay') else sp)
                if t:
                    seed[t] = seed.get(t, 0) + _count_lines(sp)
        except OSError:
            pass
    s['seed'] = seed
    s['op_start'] = 0.0
    s['seed_done'] = True

    next_replay = 0.0
    while s['run']:
        batch = None
        with s['lock']:
            if not s['nas_queue']:
                s['cv'].wait(0.2)
            if not s['run']:
                break
            if s['nas_queue']:
                batch = s['nas_queue'].popleft()
                s['inflight'] = batch   # 정지 시 회수용
        if batch:
            if s['spool_pending']:
                # dispatch 회수(reclaim)와 pop 이 겹친 드문 경쟁 — 스풀로 우회해 순서 일원화.
                _spool_batch_flusher(batch)
            else:
                _flush_batch_to_store(batch)
            with s['lock']:
                s['inflight'] = []
            continue
        # idle — 스풀 재생 (실패 시 백오프)
        if s['spool_pending'] and _time.monotonic() >= next_replay:
            _replay_spool_one()
            if not s['last_op_ok']:
                next_replay = _time.monotonic() + _REPLAY_RETRY_SEC
        # 정체로 unhealthy 가 됐지만 스풀 유입이 없었던 경우 — 직전 op 성공 확인 시 복귀
        if (not s['nas_healthy']) and (not s['spool_pending']) and s['last_op_ok']:
            s['nas_healthy'] = True
    s['exited'] = True


def _spool_batch_flusher(batch):
    """flusher 전용 스풀 우회 (trim 은 dispatch 몫 — 여기선 append 만)."""
    for target, (data, n) in _group_batch(batch).items():
        _spool_append(target, data, n)


def _start_writer():
    global _writer_thread, _flusher_thread, _writer_running
    if _writer_thread is not None:
        return
    _writer_running = True
    _store['cv'] = threading.Condition(_store['lock'])
    # 이전 run 스풀 잔량 스캔 (로컬) — 잔량이 있으면 드레인 전 직행 금지 (순서 보존)
    _ensure_dir(_store['spool_dir'])
    files: list = []
    _scan_spool(_store['spool_dir'], files)
    _store['spool_bytes'] = sum(f[2] for f in files)
    _store['spool_pending'] = bool(files)
    _writer_thread = threading.Thread(target=_writer_loop, daemon=True, name="csc-log-writer")
    _writer_thread.start()
    _flusher_thread = threading.Thread(target=_nas_flusher_loop, daemon=True,
                                       name="csc-log-flusher")
    _flusher_thread.start()
    atexit.register(_stop_writer)


def _stop_writer():
    """정지 — 잔여 큐 스풀 회수 후 dispatch 조인. flusher 는 저장 경로 op 에 갇혀 있으면
    방치한다 (daemon 스레드 — 프로세스 종료가 회수. 재기동 시 스풀이 재생된다)."""
    global _writer_running
    if not _writer_running:
        return
    _writer_running = False
    with _q_cond:
        _q_cond.notify_all()
    if _writer_thread is not None:
        _writer_thread.join(timeout=2.0)

    s = _store
    # 저장소가 건강하면 flusher 큐 잔량이 저장 경로로 나가도록 잠시 기다린다.
    deadline = _time.monotonic() + _STOP_FLUSHER_WAIT
    while _time.monotonic() < deadline and s['nas_healthy'] and not s['spool_pending']:
        with s['lock']:
            idle = not s['nas_queue'] and not s['inflight']
        if idle:
            break
        _time.sleep(0.05)

    s['run'] = False
    with s['lock']:
        s['cv'].notify_all()
    if _flusher_thread is not None:
        _flusher_thread.join(timeout=_STOP_FLUSHER_WAIT)

    # 미기록 잔량(nas_queue + inflight) 스풀 회수 — 다음 기동의 replay 가 밀어넣는다.
    with s['lock']:
        remains = list(s['nas_queue'])
        s['nas_queue'].clear()
        if s['inflight'] and not s['exited']:
            remains.insert(0, s['inflight'])
        s['inflight'] = []
    for b in remains:
        _spool_batch(b)


# sesid 카운터: 동일 us_ts가 연속될 때 1씩 증가
_sesid_lock = threading.Lock()
_sesid_last_ts: str = ""
_sesid_counter: int = 0

# key (call_id | group_id | user_id 등) → sesid 캐시
_sesid_cache: dict = {}


def init(service_log_dir: str = "", system_id: str = "csc_01",
         spool_dir: str = "spool", stall_sec: int = 5, spool_max_mb: int = 1024):
    """spool_dir 은 저장 경로 무응답 시 폴백 저장소 — 반드시 로컬 디스크 경로."""
    global _service_log_dir, _system_id, _seq_map
    _service_log_dir = service_log_dir or ""
    _system_id = system_id or "csc_01"
    _seq_map = {}
    _store['spool_dir'] = spool_dir or "spool"
    _store['stall_sec'] = stall_sec if stall_sec > 0 else 5
    _store['spool_max_bytes'] = (spool_max_mb if spool_max_mb > 0 else 1024) * 1024 * 1024
    _store['base_dir'] = _service_log_dir
    if _service_log_dir:
        yyyy, mm, dd, hh = _ymdh()
        _store['seed_hour_dir'] = os.path.join(_service_log_dir, yyyy, mm, dd, hh)
        _store['seed_mm5'] = _bucket()
    _start_writer()  # dispatch + NAS flusher 기동 (1회)


def issue_sesid(caller: str = "", module: str = "csc") -> str:
    """신규 sesid 발행: {caller}::{module}::{us_ts}::{counter}"""
    global _sesid_last_ts, _sesid_counter
    now = datetime.now()
    ts = now.strftime("%Y%m%d%H%M%S") + f"{now.microsecond:06d}"
    with _sesid_lock:
        if ts == _sesid_last_ts:
            _sesid_counter += 1
        else:
            _sesid_last_ts = ts
            _sesid_counter = 1
        counter = _sesid_counter
    return f"{caller}::{module}::{ts}::{counter}"


def get_or_issue_sesid(key: str, caller: str = "", module: str = "csc") -> str:
    """key 기반으로 캐시된 sesid 조회. 없으면 신규 발행 후 저장."""
    if not key:
        return issue_sesid(caller, module)
    with _sesid_lock:
        sid = _sesid_cache.get(key)
        if sid:
            return sid
    sid = issue_sesid(caller, module)
    with _sesid_lock:
        _sesid_cache[key] = sid
        # 캐시 크기 제한
        if len(_sesid_cache) > 10000:
            # 가장 오래된 entry 10% 제거
            for k in list(_sesid_cache.keys())[:1000]:
                del _sesid_cache[k]
    return sid


def clear_sesid(key: str):
    """세션/콜 종료 후 캐시 정리"""
    if not key:
        return
    with _sesid_lock:
        _sesid_cache.pop(key, None)


def _ts_hms() -> str:
    now = datetime.now()
    return now.strftime("%H:%M:%S.") + f"{now.microsecond:06d}"


def _ymdh():
    now = datetime.now()
    return now.strftime("%Y"), now.strftime("%m"), now.strftime("%d"), now.strftime("%H")


def _hour_dir() -> str:
    """시간 디렉터리 경로 — 순수 문자열 조립 (파일시스템 무접촉, 생성은 flusher 몫)."""
    if not _service_log_dir:
        return ""
    yyyy, mm, dd, hh = _ymdh()
    return os.path.join(_service_log_dir, yyyy, mm, dd, hh)


def _bucket() -> str:
    """5분 버킷 suffix "00".."55" (CSP/CMP 와 동일)."""
    return "%02d" % ((datetime.now().minute // 5) * 5)


def _next_seq(iface: str, msg_path: str) -> int:
    """저장 경로를 읽지 않는다 — 기동 첫 버킷은 flusher 시딩 결과에 합류하고
    (미도착 시 0 부터 — 어긋남은 리더의 sesid/내용 폴백이 흡수), 이후 버킷은
    새 파일경로라 0 부터."""
    key = f"{iface}:{msg_path}"
    cur = _seq_map.get(key)
    if cur is None:
        cur = _store['seed'].get(msg_path, 0) if _store['seed_done'] else 0
    cur += 1
    _seq_map[key] = cur
    return cur


# ── Flow + Msg 통합 로깅 ──────────────────────────────────

def log_flow(service: str,
              from_actor: str, to_actor: str,
              proto: str, method: str,
              detail: str = "", sesid: str = "", subid: str = "",
              iface: str = "ue",
              body: str = "", peer: str = "",
              mid: str = "",
              caller: str = "",
              callee: str = ""):
    """CSC의 flow + msg 이중 기록.

    - Flow: csc_01.flow.jsonl (경량, body 없음)
    - Msg : csc_01_{iface}.msg.jsonl (원문 포함) — seq로 flow와 상관
    - 모든 값이 빈 문자열이면 해당 key 생략
    - sesid 없으면 자동 발행 (caller 파라미터로 발신 MSISDN 전달 권장)
    """
    if not _service_log_dir:
        return

    hour_dir = _hour_dir()
    ts = _ts_hms()
    mm5 = _bucket()
    flow_path = os.path.join(hour_dir, f"{_system_id}.flow.{mm5}.jsonl")
    msg_path  = os.path.join(hour_dir, f"{_system_id}_{iface}.msg.{mm5}.jsonl")

    # sesid 미지정 시 자동 발행
    if not sesid:
        sesid = issue_sesid(caller, "csc")

    with _lock:
        seq = _next_seq(iface, msg_path)

        # msg: ts, dir, peer, caller, callee, proto, msg (빈값 key 생략)
        msg_entry: dict = {"ts": ts}
        direction = "RX" if to_actor == "csc" else "TX"
        msg_entry["dir"] = direction
        if peer:   msg_entry["peer"] = peer
        if caller: msg_entry["caller"] = caller
        if callee: msg_entry["callee"] = callee
        if proto:  msg_entry["proto"] = proto
        if body:   msg_entry["msg"] = body[:8000]

        # flow: ts, service, caller, callee, sesid, subid, node, from, to,
        #        proto, method, detail, mid, seq, iface
        flow_entry: dict = {"ts": ts}
        if service:    flow_entry["service"] = service
        if caller:     flow_entry["caller"] = caller
        if callee:     flow_entry["callee"] = callee
        if sesid:      flow_entry["sesid"] = sesid
        if subid:      flow_entry["subid"] = subid
        if _system_id: flow_entry["node"] = _system_id
        if from_actor: flow_entry["from"] = from_actor
        if to_actor:   flow_entry["to"] = to_actor
        if proto:      flow_entry["proto"] = proto
        if method:     flow_entry["method"] = method
        if detail:     flow_entry["detail"] = detail[:200]
        if mid:        flow_entry["mid"] = mid
        if seq > 0:    flow_entry["seq"] = seq
        if iface:      flow_entry["iface"] = iface

        # 비동기 writer 로 적재 (파일시스템 무접촉).
        # _lock 보유 중 enqueue 하여 seq 순서 = 파일 줄 순서 정합 유지.
        _enqueue(msg_path, json.dumps(msg_entry, ensure_ascii=False) + "\n")
        _enqueue(flow_path, json.dumps(flow_entry, ensure_ascii=False) + "\n")


# ── 레거시 호환 API ────────────────────────────────────────

def log_msg(interface: str, direction: str, proto: str, method: str, peer: str = "",
             caller: str = "", sesid: str = "", service: str = "mcptt"):
    """메시지 통계 로그. 신 포맷으로 redirect.
    service 기본값 "mcptt" (MCPTT 엔드포인트). admin/console 호출에선 "console" 명시.
    interface 는 method prefix 로 보존 (예: "IdMS/authreq").
    """
    from_actor = "ue" if direction == "in" else "csc"
    to_actor   = "csc" if direction == "in" else "ue"
    # interface 가 sub-function(idms/gms/cms)인 경우 method 앞에 prefix 로 붙임
    qualified_method = method
    if interface and interface not in ("mcptt", "console", "system"):
        qualified_method = f"{interface}/{method}"
    log_flow(service=service, from_actor=from_actor, to_actor=to_actor,
             proto=proto, method=qualified_method, detail=peer, iface="ue", peer=peer,
             sesid=sesid, caller=caller or peer)


def log_ptt_service(group_id: str, direction: str, proto: str, method: str, body: str = "",
                     caller: str = ""):
    """PTT 서비스 로그. group_id 기반 sesid 캐시 활용. service=mcptt 고정."""
    from_actor = "ue" if direction == "in" else "csc"
    to_actor   = "csc" if direction == "in" else "ue"
    sesid = get_or_issue_sesid(group_id, caller, "csc")
    log_flow(service="mcptt", from_actor=from_actor, to_actor=to_actor,
             proto=proto, method=method,
             sesid=sesid, iface="ue", body=body, caller=caller,
             detail=group_id)


def log_console(method: str, caller: str = "", peer: str = "",
                 proto: str = "HTTPS", body: str = "", detail: str = ""):
    """CSC admin/console API 호출 로그. service="console" 고정."""
    log_flow(service="console",
             from_actor="ue", to_actor="csc",
             proto=proto, method=method,
             detail=detail, peer=peer, iface="ue",
             caller=caller, body=body)


def log_ptt_participant(group_id: str, user_id: str, action: str):
    """레거시: PTT participants.jsonl 기록. 비동기 writer 경유 (생산자 파일시스템 무접촉)."""
    if not _service_log_dir or not group_id:
        return
    yyyy, mm, dd, hh = _ymdh()

    def _sanitize(s: str, max_len: int = 20) -> str:
        r = ''.join('_' if c in '/\\:*?"<>| ' else c for c in s)
        return r[:max_len]

    sg = _sanitize(group_id)
    dir_path = os.path.join(_service_log_dir, "ptt", yyyy, mm, dd, hh,
                             sg[:-2] if len(sg) > 2 else sg, sg + ".d")
    entry = {
        "msisdn": user_id,
        "action": action,
        "time": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
    }
    _enqueue(os.path.join(dir_path, "participants.jsonl"),
             json.dumps(entry, ensure_ascii=False) + "\n")
