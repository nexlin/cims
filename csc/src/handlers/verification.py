"""검증 실행 및 리포트 API — verify.lib (verify/lib/) 기반.

엔드포인트:
  /stages (GET)                            — 6 stage 메타 + 각 stage 의 항목 트리
  /stages/<N> (POST)                       — cims.sh verify stage<N> 실행 (N=1~6)
                                              body:
                                                - async=true 지정 시 job_id 즉시 반환
                                                - items/preset/only_children 으로 부분 실행
                                                - skip_build/skip_pkg/skip_reset/keep_agent 옵션
  /stages/<N>/latest-report (GET)          — verify_reports/*_stage<N>.md 최신
  /stages/<N>/reports (GET)                — verify_reports/*_stage<N>.md 목록
  /run (POST)                              — items / preset 으로 임의 실행 (multi-stage 가능)
  /jobs/<job_id> (GET)                     — 비동기 job 상태 + stdout tail + items_progress
  /items?stage=N (GET)                     — verify.lib registry 항목 트리 (UI 동적 체크박스)
  /presets (GET)                           — verify.lib 프리셋 목록
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

# cims.sh verify stage<N> 의 합리적 timeout (초)
_STAGE_TIMEOUT = {
    1:  300,   # 정적 검사 (lint/typecheck/syntax/unit)
    2:  900,   # 빌드
    3:  900,   # 스모크 (configure → start → 1콜)
    4:  300,   # 패키지화 (tarball + manifest)
    5: 1200,   # 로컬 배포 (TB-CSC → 배포본 csp/cmp 체인)
    6:  600,   # 통합 검증 (4시나리오)
}

_VALID_STAGES = (1, 2, 3, 4, 5, 6)


def init(tests_dir: str):
    global _TESTS_DIR, _SCRIPT_DIR, _REPORT_DIR
    _TESTS_DIR = tests_dir
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
        _SCRIPT_DIR = os.environ.get('CIMS_REPO_ROOT') or os.path.normpath(os.path.join(tests_dir, '..'))
    _REPORT_DIR = os.path.join(_SCRIPT_DIR, 'verify_reports')


async def handle_verification(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    path = urlparse(handler_args.full_path).path
    after = path[len(_VER_BASE):].lstrip('/')
    method = handler_args.method.upper()

    # /stages — 6 stage 메타 + 각 stage 항목 트리
    if after == 'stages' and method == 'GET':
        return await _get_stages_overview()

    # /stages/<N> — cims.sh verify stage<N> 실행
    m = re.fullmatch(r'stages/(\d+)', after)
    if m and method == 'POST':
        return await _run_stage(int(m.group(1)), handler_args)

    # /stages/<N>/latest-report
    m = re.fullmatch(r'stages/(\d+)/latest-report', after)
    if m and method == 'GET':
        return await _get_latest_stage_report(int(m.group(1)))

    # /stages/<N>/reports
    m = re.fullmatch(r'stages/(\d+)/reports', after)
    if m and method == 'GET':
        return await _list_stage_reports(int(m.group(1)))

    # /run — 임의 항목/프리셋 실행 (multi-stage 가능)
    if after == 'run' and method == 'POST':
        return await _run_arbitrary(handler_args)

    # /jobs/<job_id> — 비동기 job 상태 폴링
    m = re.fullmatch(r'jobs/([0-9a-f]+)', after)
    if m and method == 'GET':
        return await _get_job_status(m.group(1))

    # /items — verify.lib registry 항목 트리
    if after == 'items' and method == 'GET':
        stage_str = (handler_args.query_params or {}).get('stage')
        stage = int(stage_str) if stage_str and str(stage_str).isdigit() else None
        return await _get_verify_items(stage)

    # /presets — verify.lib 프리셋 목록
    if after == 'presets' and method == 'GET':
        return await _get_verify_presets()

    return HandlerResult(status=404, body={'error': 'Not Found'})


# ─────────────────────────────────────────────────────────────
# 보조: 리포트 / sanitized env
# ─────────────────────────────────────────────────────────────

# TB-CSC 가 csc-tb.json 으로 떠있는 상태에서 subprocess 가 환경을 그대로 상속하면
# 자식 cims.sh → csc_app.py 가 csc-tb.json 을 읽어 TB-CSC 와 같은 포트 bind 시도 → 충돌.
# Test-CSC / 배포본 csc 는 base csc.json 을 써야 하므로 TB 전용 env 차단.
_BLOCKED_ENV_KEYS = {"CIMS_CSC_CONFIG", "CIMS_AGENT_SYNC_PORT"}


def _sanitized_env() -> dict:
    return {k: v for k, v in os.environ.items() if k not in _BLOCKED_ENV_KEYS}


def _find_latest_stage_report(stage: int):
    """verify_reports/ 에서 *_stage<N>.md 중 가장 최근 파일 경로 반환."""
    if not os.path.isdir(_REPORT_DIR):
        return None
    pat = os.path.join(_REPORT_DIR, f'*_stage{stage}.md')
    files = glob.glob(pat)
    if not files:
        return None
    files.sort()
    return files[-1]


def _resolve_verdict(stage: int) -> tuple:
    """최신 리포트에서 verdict 파싱. (verdict, report_path, report_ts)."""
    rp = _find_latest_stage_report(stage)
    if not rp:
        return ('UNKNOWN', None, '')
    ts = os.path.basename(rp).split('_stage')[0]
    verdict = 'UNKNOWN'
    try:
        with open(rp) as fp:
            content = fp.read()
        m = re.search(r'^##\s*판정[:：]\s*(\w+)', content, re.MULTILINE)
        if m: verdict = m.group(1).upper()
    except Exception:
        pass
    return (verdict, rp, ts)


# ─────────────────────────────────────────────────────────────
# 비동기 job 관리
# ─────────────────────────────────────────────────────────────
_JOBS: dict = {}              # job_id → job dict
_JOBS_TTL_SEC = 3600
_JOB_LOG_DIR = '/tmp/cims_verify_jobs'


def _gc_jobs():
    now = time.time()
    stale = [jid for jid, j in _JOBS.items()
             if j.get('done') and (now - (j.get('ended_at') or now)) > _JOBS_TTL_SEC]
    for jid in stale:
        j = _JOBS.pop(jid, None)
        if j and j.get('log_path'):
            try: os.remove(j['log_path'])
            except Exception: pass


async def _start_job(stage: int, argv: list, timeout: int,
                     label: str = '') -> str:
    """Spawn subprocess in background. Returns job_id immediately."""
    _gc_jobs()
    os.makedirs(_JOB_LOG_DIR, exist_ok=True)
    job_id = uuid.uuid4().hex[:12]
    log_path = os.path.join(_JOB_LOG_DIR, f'stage{stage}_{job_id}.log')
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
        'stage': stage,
        'label': label,
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
    asyncio.create_task(_watch_job(job_id))
    return job_id


async def _watch_job(job_id: str):
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
    if job['stage'] in _VALID_STAGES:
        verdict, rp, ts = _resolve_verdict(job['stage'])
        job['verdict'] = verdict
        job['report_path'] = rp
        job['report_ts'] = ts
    job['done'] = True


# ─────────────────────────────────────────────────────────────
# stdout 마커 파서 — UI 진행 폴링용
# ─────────────────────────────────────────────────────────────
_ANSI_RE         = re.compile(r'\x1b\[[0-9;]*m')
_RE_RUN_START    = re.compile(r'^\[VERIFY\] run-start: total=(\d+) ids=(.+)$')
_RE_ITEM_START   = re.compile(r'^\[VERIFY\] item-start: (\S+) stage=(\d+) idx=(\d+)/(\d+) name=(.+)$')
_RE_ITEM_END     = re.compile(r'^\[VERIFY\] item-end: (\S+) status=(\S+) elapsed_ms=(\d+)$')
_RE_CHILD        = re.compile(r'^\[VERIFY\] child-result: (\S+)\.(\S+) status=(\S+) elapsed_ms=(\d+) name=(.+)$')
_RE_GROUP_END    = re.compile(r'^\[VERIFY\] group-end: (\S+) status=(\S+) child_count=(\d+)$')
_RE_RUN_END      = re.compile(r'^\[VERIFY\] run-end: total=(\d+) pass=(\d+) fail=(\d+) skip=(\d+)(?: blocked=(\d+))?$')


def _parse_items_progress(log_path: str) -> dict:
    """log 의 누적 stdout 에서 [VERIFY] 마커를 파싱 → 진행 dict.

    반환 형식:
      {
        "selected": [id, ...],
        "total": int, "completed": int, "current": str|None,
        "items": [
          {"id", "name", "stage", "status": "RUNNING|PASS|FAIL|SKIP|BLOCKED",
           "elapsed_ms", "idx", "children": [{"id", "name", "status", "elapsed_ms"}]
          }, ...
        ],
        "summary": {"pass":..., "fail":..., "skip":..., "blocked":...}|None,
      }
    """
    selected: list = []
    total: int = 0
    items: list = []
    by_id: dict = {}
    current: Optional[str] = None
    completed: int = 0
    summary: Optional[dict] = None

    if not log_path or not os.path.isfile(log_path):
        return {"selected": selected, "total": 0, "completed": 0,
                "current": None, "items": items, "summary": None}
    try:
        with open(log_path, 'rb') as f:
            data = f.read().decode('utf-8', errors='replace')
    except Exception:
        return {"selected": selected, "total": 0, "completed": 0,
                "current": None, "items": items, "summary": None}

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
            iid, stage, idx, n, name = (
                m.group(1), int(m.group(2)),
                int(m.group(3)), int(m.group(4)), m.group(5),
            )
            if total < n:
                total = n
            entry = by_id.get(iid)
            if entry is None:
                entry = {"id": iid, "name": name, "stage": stage,
                         "status": "RUNNING", "elapsed_ms": 0, "idx": idx,
                         "children": []}
                items.append(entry); by_id[iid] = entry
                if iid not in selected:
                    selected.append(iid)
            else:
                entry["status"] = "RUNNING"
                entry["name"] = name
                entry["stage"] = stage
                entry["idx"] = idx
            current = iid
            continue
        m = _RE_ITEM_END.match(line)
        if m:
            iid, status, elapsed = m.group(1), m.group(2), int(m.group(3))
            entry = by_id.get(iid)
            if entry is None:
                entry = {"id": iid, "name": iid, "stage": 0,
                         "status": status, "elapsed_ms": elapsed,
                         "idx": len(items) + 1, "children": []}
                items.append(entry); by_id[iid] = entry
            else:
                entry["status"] = status
                entry["elapsed_ms"] = elapsed
            if status in ("PASS", "FAIL", "SKIP", "BLOCKED"):
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
                parent = {"id": parent_id, "name": parent_id, "stage": 0,
                          "status": "RUNNING", "elapsed_ms": 0,
                          "idx": len(items) + 1, "children": []}
                items.append(parent); by_id[parent_id] = parent
            existing = next((c for c in parent["children"] if c["id"] == child_id), None)
            if existing:
                existing["status"] = status
                existing["elapsed_ms"] = elapsed
                existing["name"] = name
            else:
                parent["children"].append({
                    "id": child_id, "name": name,
                    "status": status, "elapsed_ms": elapsed,
                })
            continue
        m = _RE_GROUP_END.match(line)
        if m:
            parent_id, status, _ = m.group(1), m.group(2), int(m.group(3))
            entry = by_id.get(parent_id)
            if entry is not None:
                entry["status"] = status
            continue
        m = _RE_RUN_END.match(line)
        if m:
            summary = {
                "pass":    int(m.group(2)),
                "fail":    int(m.group(3)),
                "skip":    int(m.group(4)),
                "blocked": int(m.group(5) or 0),
            }
            continue

    if not total:
        total = len(items)
    return {
        "selected": selected,
        "total": total,
        "completed": completed,
        "current": current,
        "items": items,
        "summary": summary,
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
        'stage': job['stage'],
        'label': job.get('label', ''),
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


# ─────────────────────────────────────────────────────────────
# Stage 실행 (cims.sh verify stage<N>)
# ─────────────────────────────────────────────────────────────

def _build_stage_argv(stage: int, opts: dict) -> list:
    """opts → cims.sh argv. stage 별 옵션 호환성 적용."""
    skip_build = bool(opts.get('skip_build', True))
    skip_pkg   = bool(opts.get('skip_pkg', True))
    skip_reset = bool(opts.get('skip_reset', False))
    keep_agent = bool(opts.get('keep_agent', False))
    items      = opts.get('items') or []
    only_children = opts.get('only_children') or {}

    argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'verify', f'stage{stage}']
    # 옵션 — 모든 stage 가 cims_verify CLI 위임이므로 동일하게 통과
    if skip_build: argv.append('--skip-build')
    if skip_pkg:   argv.append('--skip-pkg')
    if skip_reset: argv.append('--skip-reset')
    if keep_agent: argv.append('--keep-agent')
    if items:
        argv += ['--items', ','.join(items)]
    if isinstance(only_children, dict) and only_children:
        argv += ['--only-children', json.dumps(only_children, ensure_ascii=False)]
    return argv


async def _run_stage(stage: int, handler_args: HandlerArgs) -> HandlerResult:
    if stage not in _VALID_STAGES:
        return HandlerResult(status=400, body={'error': f'stage must be 1~6 (got {stage})'})
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    is_async = bool(opts.get('async', False))
    argv = _build_stage_argv(stage, opts)
    timeout = _STAGE_TIMEOUT.get(stage, 600)
    label = f'stage{stage}'

    if is_async:
        job_id = await _start_job(stage, argv, timeout, label=label)
        return HandlerResult(status=202, body={
            'job_id': job_id,
            'stage': stage,
            'argv': argv,
            'started_at': _JOBS[job_id]['started_at'],
            'message': 'started',
        })

    # 동기 모드 — to_thread 로 blocking subprocess
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
            'stage': stage, 'error': f'verify stage{stage} timeout ({timeout}s)',
        })
    except Exception as e:
        return HandlerResult(status=500, body={'stage': stage, 'error': str(e)})

    verdict, report_path, report_ts = _resolve_verdict(stage)
    return HandlerResult(status=200, body={
        'stage': stage,
        'verdict': verdict,
        'returncode': result.returncode,
        'report_path': report_path,
        'report_ts': report_ts,
        'stdout_tail': stdout_tail,
        'argv': argv,
    })


async def _run_arbitrary(handler_args: HandlerArgs) -> HandlerResult:
    """POST /run — body 의 items/preset 으로 임의 항목 실행 (async 만)."""
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    items = opts.get('items') or []
    preset = opts.get('preset') or ''
    if not items and not preset:
        return HandlerResult(status=400, body={
            'error': 'items / preset 중 하나 지정 필요',
        })

    # cims_verify CLI 직접 호출 — multi-stage 가능
    argv = ['python3', '-m', 'tests.cims_verify', 'run']
    if items:  argv += ['--items', ','.join(items)]
    if preset: argv += ['--preset', preset]
    if opts.get('skip_build'): argv.append('--skip-build')
    if opts.get('skip_pkg'):   argv.append('--skip-pkg')
    if opts.get('skip_reset'): argv.append('--skip-reset')
    if opts.get('keep_agent'): argv.append('--keep-agent')
    only_children = opts.get('only_children') or {}
    if isinstance(only_children, dict) and only_children:
        argv += ['--only-children', json.dumps(only_children, ensure_ascii=False)]

    timeout = int(opts.get('timeout') or 1800)
    job_id = await _start_job(stage=0, argv=argv, timeout=timeout,
                              label=f"items={len(items)} preset={preset or '-'}")
    return HandlerResult(status=202, body={
        'job_id': job_id,
        'stage': 0,
        'argv': argv,
        'started_at': _JOBS[job_id]['started_at'],
        'message': 'started',
    })


async def _get_latest_stage_report(stage: int) -> HandlerResult:
    if stage not in _VALID_STAGES:
        return HandlerResult(status=400, body={'error': f'stage must be 1~6 (got {stage})'})
    path = _find_latest_stage_report(stage)
    if not path:
        return HandlerResult(status=404, body={'error': f'No stage{stage} report found'})
    with open(path) as f:
        content = f.read()
    return HandlerResult(status=200, body={
        'stage': stage,
        'path': path,
        'ts': os.path.basename(path).split('_stage')[0],
        'content': content,
    })


async def _list_stage_reports(stage: int) -> HandlerResult:
    if stage not in _VALID_STAGES:
        return HandlerResult(status=400, body={'error': f'stage must be 1~6 (got {stage})'})
    if not os.path.isdir(_REPORT_DIR):
        return HandlerResult(status=200, body={'stage': stage, 'reports': []})
    pat = os.path.join(_REPORT_DIR, f'*_stage{stage}.md')
    files = sorted(glob.glob(pat), reverse=True)
    items = []
    for p in files[:50]:
        name = os.path.basename(p)
        ts = name.split('_stage')[0]
        size = os.path.getsize(p)
        items.append({'ts': ts, 'name': name, 'size': size})
    return HandlerResult(status=200, body={'stage': stage, 'reports': items})


# ─────────────────────────────────────────────────────────────
# verify.lib 메타 API
# ─────────────────────────────────────────────────────────────
_ITEMS_CACHE: dict = {'data': None, 'expires_at': 0}
_ITEMS_TTL_SEC = 60


def _run_verify_cli(args: list, timeout: int = 20) -> tuple:
    """python3 -m tests.cims_verify <args> 실행. (rc, stdout, stderr)."""
    cmd = ['python3', '-m', 'tests.cims_verify'] + args
    try:
        proc = subprocess.run(
            cmd, cwd=_SCRIPT_DIR, env={k: v for k, v in os.environ.items()
                                       if k not in _BLOCKED_ENV_KEYS},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout, text=True,
        )
        return (proc.returncode, proc.stdout, proc.stderr)
    except Exception as e:
        return (-1, '', str(e))


async def _get_verify_items(stage: Optional[int]) -> HandlerResult:
    """verify.lib 의 등록 항목 트리 + 프리셋. 60s 캐시."""
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

    if stage is not None:
        filtered = [s for s in data.get('stages', []) if s.get('stage') == stage]
        return HandlerResult(status=200, body={
            'stage': stage, 'stages': filtered, 'presets': data.get('presets', []),
        })
    return HandlerResult(status=200, body=data)


async def _get_verify_presets() -> HandlerResult:
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


# ─────────────────────────────────────────────────────────────
# /stages — 6 stage 메타 + 항목 트리 (UI 초기 로드)
# ─────────────────────────────────────────────────────────────
_STAGE_TITLES = {
    1: ("정적 검사",   "lint / format / unit test"),
    2: ("빌드",        "preflight + cmake build"),
    3: ("스모크",      "configure → start dev → 1콜 VoIP/PTT"),
    4: ("패키지화",    "tarball + manifest hash"),
    5: ("로컬 배포",   "TB-CSC → Test-agent → csc-server → csp/cmp 체인"),
    6: ("통합 검증",   "VoLTE/PTT 음성·영상 (배포본 대상)"),
}


async def _get_stages_overview() -> HandlerResult:
    """6 stage 메타 + 각 stage 항목 트리 + 프리셋."""
    items_resp = await _get_verify_items(None)
    if items_resp.status != 200:
        return items_resp
    data = items_resp.body
    by_stage = {s['stage']: s.get('items', []) for s in data.get('stages', [])}
    stages = []
    for n in _VALID_STAGES:
        title, desc = _STAGE_TITLES[n]
        stages.append({
            'stage':       n,
            'title':       title,
            'description': desc,
            'timeout_s':   _STAGE_TIMEOUT.get(n, 600),
            'items':       by_stage.get(n, []),
        })
    return HandlerResult(status=200, body={
        'stages':  stages,
        'presets': data.get('presets', []),
    })


CIMS_VERIFICATION_HANDLER_LIST = [
    (_VER_BASE, handle_verification, {}),
]
