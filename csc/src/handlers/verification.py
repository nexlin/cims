"""검증 실행 및 리포트 API — verify.lib (tests/verify.lib/) 기반.

엔드포인트:
  /phases/<N> (POST)                      — cims.sh verify phase<N> 실행 (N=1/2/3)
                                            body 의 async=true 지정 시 job_id 즉시 반환 (비동기)
                                            기본은 sync — subprocess 종료까지 블록 (CLI/curl 호환)
                                            body 의 items/preset/only_children 으로 부분 실행 지원
  /phases/<N>/latest-report (GET)         — verify_reports/*_phase<N>.md 최신 내용
  /phases/<N>/reports (GET)               — verify_reports/*_phase<N>.md 목록
  /jobs/<job_id> (GET)                    — 비동기 job 상태 + stdout tail + items_progress (폴링용)
  /items?phase=N (GET)                    — verify.lib registry 등록 항목 트리 (UI 동적 체크박스)
  /presets (GET)                          — verify.lib 프리셋 목록
"""
import os
import re
import json
import glob
import time
import uuid
import asyncio
import subprocess
from typing import Optional
from urllib.parse import urlparse, parse_qs
from httpsrv.handler import HandlerArgs, HandlerResult

_VER_BASE = '/api/v1/verification'
_TESTS_DIR = ''
_SCRIPT_DIR = ''          # cims.sh 가 있는 디렉토리 (소스 루트)
_REPORT_DIR = ''          # verify_reports/ 경로

# cims.sh verify phase<N> 의 합리적 timeout (초)
_PHASE_TIMEOUT = {
    1: 900,   # reset + build + configure + start + 회귀 시나리오
    2: 360,   # install + start/health/stop
    3: 600,   # install + start/health/stop + 4시나리오
}

def init(tests_dir: str):
    global _TESTS_DIR, _SCRIPT_DIR, _REPORT_DIR
    _TESTS_DIR = tests_dir
    # repo root 탐색: tests_dir 의 상위를 올라가며 cims.sh + CMakeLists.txt 가 있는 곳.
    # tests_dir 자체가 존재하지 않더라도 경로 문자열 기반으로 상위 탐색.
    cur = os.path.dirname(os.path.abspath(tests_dir))
    _SCRIPT_DIR = ''
    for _ in range(6):
        if os.path.isfile(os.path.join(cur, 'cims.sh')) and \
           os.path.isfile(os.path.join(cur, 'CMakeLists.txt')):
            _SCRIPT_DIR = cur
            break
        nxt = os.path.dirname(cur)
        if nxt == cur:
            break
        cur = nxt
    if not _SCRIPT_DIR:
        # fallback: env 또는 tests_dir 의 바로 위
        _SCRIPT_DIR = os.environ.get('CIMS_REPO_ROOT') or os.path.normpath(os.path.join(tests_dir, '..'))
    _REPORT_DIR = os.path.join(_SCRIPT_DIR, 'verify_reports')


async def handle_verification(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    path = urlparse(handler_args.full_path).path
    after = path[len(_VER_BASE):].lstrip('/')
    method = handler_args.method.upper()

    # /phases/<N> — cims.sh verify phase<N> 실행
    m = re.fullmatch(r'phases/(\d+)', after)
    if m and method == 'POST':
        return await _run_phase(int(m.group(1)), handler_args)

    # /phases/<N>/latest-report
    m = re.fullmatch(r'phases/(\d+)/latest-report', after)
    if m and method == 'GET':
        return await _get_latest_phase_report(int(m.group(1)))

    # /phases/<N>/reports
    m = re.fullmatch(r'phases/(\d+)/reports', after)
    if m and method == 'GET':
        return await _list_phase_reports(int(m.group(1)))

    # /jobs/<job_id> — 비동기 job 상태 폴링
    m = re.fullmatch(r'jobs/([0-9a-f]+)', after)
    if m and method == 'GET':
        return await _get_job_status(m.group(1))

    # /items — verify.lib registry 항목 트리 (Console UI 동적 체크박스용)
    if after == 'items' and method == 'GET':
        # query_params 는 HandlerArgs 에 dict 로 별도 노출됨
        phase_str = (handler_args.query_params or {}).get('phase')
        phase = int(phase_str) if phase_str and str(phase_str).isdigit() else None
        return await _get_verify_items(phase)

    # /presets — verify.lib 프리셋 목록
    if after == 'presets' and method == 'GET':
        return await _get_verify_presets()

    return HandlerResult(status=404, body={'error': 'Not Found'})


# ─────────────────────────────────────────────────────────────
# Phase 1/2/3 — cims.sh verify phase<N> 래퍼
# ─────────────────────────────────────────────────────────────

def _find_latest_phase_report(phase: int):
    """verify_reports/ 에서 *_phase<N>.md 중 가장 최근 파일 경로 반환."""
    if not os.path.isdir(_REPORT_DIR):
        return None
    pat = os.path.join(_REPORT_DIR, f'*_phase{phase}.md')
    files = glob.glob(pat)
    if not files:
        return None
    files.sort()
    return files[-1]


# ─────────────────────────────────────────────────────────────
# 비동기 job 관리 — 진행 중 stdout tail 폴링 + 완료 시 결과 조회용
# ─────────────────────────────────────────────────────────────
_JOBS: dict = {}              # job_id → job dict
_JOBS_TTL_SEC = 3600          # 완료된 job 의 보관 기간 (1시간)
_JOB_LOG_DIR = '/tmp/cims_verify_jobs'

# TB-CSC 가 csc-tb.json (4419/4431) 으로 떠있는 상태에서 subprocess 가 환경을 그대로 상속하면
# 자식 cims.sh → csc_app.py 가 csc-tb.json 을 읽어 TB-CSC 와 같은 포트 bind 시도 → 충돌.
# Test-CSC / 배포본 csc 는 base csc.json (4421/4445/4420) 을 써야 하므로 TB 전용 env 차단.
_BLOCKED_ENV_KEYS = {"CIMS_CSC_CONFIG", "CIMS_AGENT_SYNC_PORT"}


def _sanitized_env() -> dict:
    return {k: v for k, v in os.environ.items() if k not in _BLOCKED_ENV_KEYS}


def _resolve_verdict(phase: int) -> tuple:
    """최신 리포트에서 verdict 파싱. (verdict, report_path, report_ts) 반환."""
    rp = _find_latest_phase_report(phase)
    if not rp:
        return ('UNKNOWN', None, '')
    ts = os.path.basename(rp).split('_phase')[0]
    verdict = 'UNKNOWN'
    try:
        with open(rp) as fp:
            content = fp.read()
        m = re.search(r'^##\s*판정[:：]\s*(\w+)', content, re.MULTILINE)
        if m: verdict = m.group(1).upper()
    except Exception:
        pass
    return (verdict, rp, ts)


def _gc_jobs():
    """오래된 완료 job 메모리 회수."""
    now = time.time()
    stale = [jid for jid, j in _JOBS.items()
             if j.get('done') and (now - (j.get('ended_at') or now)) > _JOBS_TTL_SEC]
    for jid in stale:
        j = _JOBS.pop(jid, None)
        if j and j.get('log_path'):
            try: os.remove(j['log_path'])
            except Exception: pass


async def _start_phase_job(phase: int, argv: list, timeout: int) -> str:
    """Spawn subprocess in background. Returns job_id immediately."""
    _gc_jobs()
    os.makedirs(_JOB_LOG_DIR, exist_ok=True)
    job_id = uuid.uuid4().hex[:12]
    log_path = os.path.join(_JOB_LOG_DIR, f'phase{phase}_{job_id}.log')
    log_file = open(log_path, 'wb')
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=log_file,
        stderr=asyncio.subprocess.STDOUT,
        cwd=_SCRIPT_DIR,
        env=_sanitized_env(),
    )
    job = {
        'job_id': job_id,
        'phase': phase,
        'argv': argv,
        'started_at': time.time(),
        'ended_at': None,
        'log_path': log_path,
        'returncode': None,
        'done': False,
        'verdict': None,
        'report_path': None,
        'report_ts': '',
        '_proc': proc,
        '_log_file': log_file,
        '_timeout': timeout,
    }
    _JOBS[job_id] = job
    asyncio.create_task(_watch_phase_job(job_id))
    return job_id


async def _watch_phase_job(job_id: str):
    job = _JOBS.get(job_id)
    if not job: return
    proc = job['_proc']
    try:
        rc = await asyncio.wait_for(proc.wait(), timeout=job['_timeout'])
    except asyncio.TimeoutError:
        try: proc.kill()
        except Exception: pass
        rc = -1
    finally:
        try: job['_log_file'].close()
        except Exception: pass
    job['returncode'] = rc
    job['ended_at'] = time.time()
    verdict, rp, ts = _resolve_verdict(job['phase'])
    job['verdict'] = verdict
    job['report_path'] = rp
    job['report_ts'] = ts
    job['done'] = True


_ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')
_RE_RUN_START   = re.compile(r'^\[VERIFY\] run-start: total=(\d+) ids=(.+)$')
_RE_ITEM_START  = re.compile(r'^\[VERIFY\] item-start: (\S+) idx=(\d+)/(\d+) name=(.+)$')
_RE_ITEM_END    = re.compile(r'^\[VERIFY\] item-end: (\S+) status=(\S+) elapsed_ms=(\d+)$')
_RE_CHILD       = re.compile(r'^\[VERIFY\] child-result: (\S+)\.(\S+) status=(\S+) elapsed_ms=(\d+) name=(.+)$')
_RE_STEP_START  = re.compile(r'^\[VERIFY\] step-start: (\S+) (.+)$')
_RE_STEP_END    = re.compile(r'^\[VERIFY\] step-end: (\S+) status=(\S+) elapsed_ms=(\d+)$')


def _parse_items_progress(log_path: str) -> dict:
    """log 의 누적 stdout 에서 [VERIFY] 마커를 파싱하여 진행 dict 반환.

    반환 형식:
      {
        "selected": [id, ...],
        "total": int, "completed": int, "current": str|None,
        "items": [
          {"id", "name", "status": "RUNNING|PASS|FAIL|SKIP",
           "elapsed_ms", "started_at": <relative>,
           "children": [{"id", "name", "status", "elapsed_ms"}]
          }, ...
        ]
      }
    Phase 2 의 step-start/step-end 는 P2-RUN-ALL 의 children 으로 흡수.
    """
    selected: list = []
    total: int = 0
    items: list = []
    by_id: dict = {}
    current: str | None = None
    completed: int = 0

    if not log_path or not os.path.isfile(log_path):
        return {"selected": selected, "total": 0, "completed": 0,
                "current": None, "items": items}
    try:
        with open(log_path, 'rb') as f:
            data = f.read().decode('utf-8', errors='replace')
    except Exception:
        return {"selected": selected, "total": 0, "completed": 0,
                "current": None, "items": items}

    for raw in data.splitlines():
        line = _ANSI_RE.sub('', raw).rstrip()
        if not line.startswith('[VERIFY] '):
            continue
        m = _RE_RUN_START.match(line)
        if m:
            total = int(m.group(1))
            selected = m.group(2).split(',') if m.group(2) else []
            continue
        m = _RE_ITEM_START.match(line)
        if m:
            iid, idx, n, name = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            if total < n:
                total = n
            entry = by_id.get(iid)
            if entry is None:
                entry = {"id": iid, "name": name, "status": "RUNNING",
                         "elapsed_ms": 0, "idx": idx, "children": []}
                items.append(entry); by_id[iid] = entry
                if iid not in selected:
                    selected.append(iid)
            else:
                entry["status"] = "RUNNING"
                entry["name"] = name
                entry["idx"] = idx
            current = iid
            continue
        m = _RE_ITEM_END.match(line)
        if m:
            iid, status, elapsed = m.group(1), m.group(2), int(m.group(3))
            entry = by_id.get(iid)
            if entry is None:
                entry = {"id": iid, "name": iid, "status": status,
                         "elapsed_ms": elapsed, "idx": len(items) + 1, "children": []}
                items.append(entry); by_id[iid] = entry
            else:
                entry["status"] = status
                entry["elapsed_ms"] = elapsed
            if status in ("PASS", "FAIL", "SKIP"):
                completed += 1
            if current == iid:
                current = None
            continue
        m = _RE_CHILD.match(line)
        if m:
            parent_id, child_id, status, elapsed, name = (
                m.group(1), m.group(2), m.group(3), int(m.group(4)), m.group(5))
            parent = by_id.get(parent_id)
            if parent is None:
                parent = {"id": parent_id, "name": parent_id, "status": "RUNNING",
                          "elapsed_ms": 0, "idx": len(items) + 1, "children": []}
                items.append(parent); by_id[parent_id] = parent
            parent["children"].append({
                "id": child_id, "name": name, "status": status, "elapsed_ms": elapsed,
            })
            continue
        m = _RE_STEP_START.match(line)
        if m:
            step_no, step_name = m.group(1), m.group(2)
            # Phase 2 의 22단계 → P2-RUN-ALL 의 children 으로 흡수
            parent = by_id.get('P2-RUN-ALL')
            if parent is None:
                parent = {"id": "P2-RUN-ALL", "name": "Phase 2 22단계",
                          "status": "RUNNING", "elapsed_ms": 0,
                          "idx": len(items) + 1, "children": []}
                items.append(parent); by_id['P2-RUN-ALL'] = parent
                if 'P2-RUN-ALL' not in selected:
                    selected.append('P2-RUN-ALL')
            cid = f"P2-{step_no}"
            existing = next((c for c in parent["children"] if c["id"] == cid), None)
            if existing:
                existing["status"] = "RUNNING"; existing["name"] = step_name
            else:
                parent["children"].append({
                    "id": cid, "name": step_name, "status": "RUNNING", "elapsed_ms": 0,
                })
            continue
        m = _RE_STEP_END.match(line)
        if m:
            step_no, status, elapsed = m.group(1), m.group(2), int(m.group(3))
            parent = by_id.get('P2-RUN-ALL')
            if parent is None:
                continue
            cid = f"P2-{step_no}"
            existing = next((c for c in parent["children"] if c["id"] == cid), None)
            if existing:
                existing["status"] = status; existing["elapsed_ms"] = elapsed
            continue

    if not total:
        total = len(items)
    return {
        "selected": selected,
        "total": total,
        "completed": completed,
        "current": current,
        "items": items,
    }


async def _get_job_status(job_id: str) -> HandlerResult:
    job = _JOBS.get(job_id)
    if not job:
        return HandlerResult(status=404, body={'error': f'job not found: {job_id}'})
    tail = ''
    try:
        with open(job['log_path'], 'rb') as f:
            data = f.read().decode('utf-8', errors='replace')
            tail = '\n'.join(data.splitlines()[-50:])
    except Exception:
        pass
    progress = _parse_items_progress(job.get('log_path', ''))
    now = time.time()
    return HandlerResult(status=200, body={
        'job_id': job_id,
        'phase': job['phase'],
        'argv': job['argv'],
        'started_at': job['started_at'],
        'ended_at': job['ended_at'],
        'elapsed': (job['ended_at'] or now) - job['started_at'],
        'done': job['done'],
        'returncode': job['returncode'],
        'verdict': job['verdict'] if job['done'] else None,
        'report_path': job['report_path'] if job['done'] else None,
        'report_ts': job['report_ts'] if job['done'] else '',
        'stdout_tail': tail,
        'items_progress': progress,
    })


def _build_phase_argv(phase: int, opts: dict) -> list:
    """opts → cims.sh argv. Phase 별 옵션 호환성 적용.

    items 가 있으면 cims.sh wrapper 가 cims_verify CLI 로 passthrough — 단,
    Phase 3 만 verify.lib 마이그레이션 완료된 상태이므로 그 외 phase 는
    items 옵션을 무시한다 (legacy 본체 호출).
    """
    skip_build = bool(opts.get('skip_build', True))
    skip_pkg   = bool(opts.get('skip_pkg', True))
    skip_reset = bool(opts.get('skip_reset', False))
    keep_agent = bool(opts.get('keep_agent', False))
    items      = opts.get('items') or []
    only_children = opts.get('only_children') or {}

    argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'verify', f'phase{phase}']
    if skip_build: argv.append('--skip-build')
    if phase == 1:
        if skip_reset: argv.append('--skip-reset')
    else:
        if skip_pkg:   argv.append('--skip-pkg')
        if keep_agent: argv.append('--keep-agent')
    # verify.lib 마이그레이션 완료된 phase 의 items 옵션 passthrough.
    # Step 1: Phase 3, Step 2: Phase 1, Step 3: Phase 2 (단일 P2-RUN-ALL 항목).
    if phase in (1, 2, 3) and items:
        argv += ['--items', ','.join(items)]
    # 모듈 자식 항목 부분 실행 — JSON 인코딩으로 단일 인자 전달
    if isinstance(only_children, dict) and only_children:
        argv += ['--only-children', json.dumps(only_children, ensure_ascii=False)]
    return argv


async def _run_phase(phase: int, handler_args: HandlerArgs) -> HandlerResult:
    if phase not in (1, 2, 3):
        return HandlerResult(status=400, body={'error': 'phase must be 1, 2, or 3'})
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    is_async = bool(opts.get('async', False))
    argv = _build_phase_argv(phase, opts)
    timeout = _PHASE_TIMEOUT.get(phase, 600)

    # 비동기 모드 — job 즉시 시작 + job_id 반환 (frontend 가 GET /jobs/<id> 폴링)
    if is_async:
        job_id = await _start_phase_job(phase, argv, timeout)
        return HandlerResult(status=202, body={
            'job_id': job_id,
            'phase': phase,
            'argv': argv,
            'started_at': _JOBS[job_id]['started_at'],
            'message': 'started',
        })

    # 동기 모드 (legacy) — CLI / curl 호환. async handler 에서 blocking subprocess.run 은
    # uvicorn 이벤트 루프를 block 하여 self-call 이 실패하므로 to_thread 사용.
    def _run_sync():
        return subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout,
            cwd=_SCRIPT_DIR, env=_sanitized_env(),
        )
    try:
        result = await asyncio.to_thread(_run_sync)
        stdout_tail = '\n'.join((result.stdout + result.stderr).splitlines()[-40:])
    except subprocess.TimeoutExpired:
        return HandlerResult(status=500, body={
            'phase': phase,
            'error': f'verify phase{phase} timeout ({timeout}s)',
        })
    except Exception as e:
        return HandlerResult(status=500, body={'phase': phase, 'error': str(e)})

    verdict, report_path, report_ts = _resolve_verdict(phase)
    return HandlerResult(status=200, body={
        'phase': phase,
        'verdict': verdict,
        'returncode': result.returncode,
        'report_path': report_path,
        'report_ts': report_ts,
        'stdout_tail': stdout_tail,
        'argv': argv,
    })


async def _get_latest_phase_report(phase: int) -> HandlerResult:
    if phase not in (1, 2, 3):
        return HandlerResult(status=400, body={'error': 'phase must be 1, 2, or 3'})
    path = _find_latest_phase_report(phase)
    if not path:
        return HandlerResult(status=404, body={'error': f'No phase{phase} report found'})
    with open(path) as f:
        content = f.read()
    return HandlerResult(status=200, body={
        'phase': phase,
        'path': path,
        'ts': os.path.basename(path).split('_phase')[0],
        'content': content,
    })


async def _list_phase_reports(phase: int) -> HandlerResult:
    if phase not in (1, 2, 3):
        return HandlerResult(status=400, body={'error': 'phase must be 1, 2, or 3'})
    if not os.path.isdir(_REPORT_DIR):
        return HandlerResult(status=200, body={'phase': phase, 'reports': []})
    pat = os.path.join(_REPORT_DIR, f'*_phase{phase}.md')
    files = sorted(glob.glob(pat), reverse=True)
    items = []
    for p in files[:50]:
        name = os.path.basename(p)
        ts = name.split('_phase')[0]
        size = os.path.getsize(p)
        items.append({'ts': ts, 'name': name, 'size': size})
    return HandlerResult(status=200, body={'phase': phase, 'reports': items})


# ─────────────────────────────────────────────────────────────
# verify.lib 메타 API (UI 동적 체크박스용)
# ─────────────────────────────────────────────────────────────
_ITEMS_CACHE: dict = {'data': None, 'expires_at': 0}
_ITEMS_TTL_SEC = 60


def _run_verify_cli(args: list, timeout: int = 20) -> tuple:
    """python3 -m tests.cims_verify <args> 실행. (rc, stdout, stderr) 반환."""
    cmd = ['python3', '-m', 'tests.cims_verify'] + args
    try:
        proc = subprocess.run(
            cmd, cwd=_SCRIPT_DIR, env={k: v for k, v in os.environ.items()
                                       if k not in ('CIMS_CSC_CONFIG', 'CIMS_AGENT_SYNC_PORT')},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, text=True,
        )
        return (proc.returncode, proc.stdout, proc.stderr)
    except Exception as e:
        return (-1, '', str(e))


async def _get_verify_items(phase: Optional[int]) -> HandlerResult:
    """verify.lib 의 등록 항목 트리 + 프리셋. 60s 캐시.

    Phase 별 마이그레이션 진행도가 다르므로, registry 가 비어있는 phase 도 반환 (items: []).
    """
    now = time.time()
    cached = _ITEMS_CACHE
    if cached['data'] is not None and now < cached['expires_at']:
        data = cached['data']
    else:
        argv = ['list', '--json']
        rc, out, err = await asyncio.to_thread(_run_verify_cli, argv)
        if rc != 0:
            return HandlerResult(status=500, body={
                'error': 'cims_verify list failed', 'rc': rc, 'stderr': err[-2000:],
            })
        try:
            data = json.loads(out)
        except Exception as e:
            return HandlerResult(status=500, body={
                'error': f'invalid JSON from cims_verify: {e}', 'stdout': out[-1000:],
            })
        _ITEMS_CACHE['data'] = data
        _ITEMS_CACHE['expires_at'] = now + _ITEMS_TTL_SEC

    # phase 필터 (캐시는 전체. 응답에서만 필터링)
    if phase is not None:
        filtered = [p for p in data.get('phases', []) if p.get('phase') == phase]
        return HandlerResult(status=200, body={
            'phase': phase, 'phases': filtered, 'presets': data.get('presets', []),
        })
    return HandlerResult(status=200, body=data)


async def _get_verify_presets() -> HandlerResult:
    """프리셋 목록만."""
    argv = ['list-presets', '--json']
    rc, out, err = await asyncio.to_thread(_run_verify_cli, argv)
    if rc != 0:
        return HandlerResult(status=500, body={
            'error': 'cims_verify list-presets failed', 'rc': rc, 'stderr': err[-2000:],
        })
    try:
        data = json.loads(out)
    except Exception as e:
        return HandlerResult(status=500, body={
            'error': f'invalid JSON: {e}', 'stdout': out[-1000:],
        })
    return HandlerResult(status=200, body={'presets': data})


CIMS_VERIFICATION_HANDLER_LIST = [
    (_VER_BASE, handle_verification, {}),
]
