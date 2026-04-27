"""검증 실행 및 리포트 API

엔드포인트:
  /run                                    — 기존 run_all.py (Phase 1 테스트 세밀)
  /report                                 — 기존 verification_report.md

  /phases/<N> (POST)                      — cims.sh verify phase<N> 실행 (N=1/2/3)
                                            body 의 async=true 지정 시 job_id 즉시 반환 (비동기)
                                            기본은 sync — subprocess 종료까지 블록 (CLI/curl 호환)
  /phases/<N>/latest-report (GET)         — verify_reports/*_phase<N>.md 최신 내용
  /phases/<N>/reports (GET)               — verify_reports/*_phase<N>.md 목록
  /jobs/<job_id> (GET)                    — 비동기 job 상태 + stdout tail (폴링용)
"""
import os
import re
import json
import glob
import time
import uuid
import asyncio
import subprocess
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

    if after == 'run' and method == 'POST':
        return await _run_verification()
    if after == 'report' and method == 'GET':
        return await _get_report()

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

    return HandlerResult(status=404, body={'error': 'Not Found'})


async def _run_verification() -> HandlerResult:
    if not _TESTS_DIR:
        return HandlerResult(status=500, body={'error': 'tests_dir not configured'})

    run_all = os.path.join(_TESTS_DIR, 'run_all.py')
    if not os.path.exists(run_all):
        return HandlerResult(status=500, body={'error': f'run_all.py not found: {run_all}'})

    try:
        def _run_sync():
            return subprocess.run(
                ['python3', run_all],
                capture_output=True, text=True, timeout=600,
                cwd=_TESTS_DIR,
            )
        result = await asyncio.to_thread(_run_sync)
        output = result.stdout + result.stderr

        # 결과 파싱
        report_path = os.path.join(_TESTS_DIR, 'verification_report.md')

        # run_all.py의 출력에서 JSON 형태 결과 추출 시도
        # 또는 리포트 파일에서 파싱
        modules = []
        total = pass_count = fail_count = 0

        if os.path.exists(report_path):
            with open(report_path, 'r') as f:
                content = f.read()
            # 간단 파싱: "총 N건: PASS=M FAIL=F" 패턴
            import re
            m = re.search(r'총 (\d+)건.*PASS=(\d+).*FAIL=(\d+)', output)
            if m:
                total = int(m.group(1))
                pass_count = int(m.group(2))
                fail_count = int(m.group(3))

            # 소요 시간 파싱
            elapsed = 0.0
            m2 = re.search(r'소요 시간: ([\d.]+)초', output)
            if m2:
                elapsed = float(m2.group(1))

            # 모듈별 결과 파싱 (ANSI 코드 제거)
            clean = re.sub(r'\033\[[0-9;]*m', '', output)
            current_module = None
            for line in clean.split('\n'):
                # 모듈 헤더: "  [1/7] CMP 모듈 검증" 또는 "── PTT-MCPTT: ..."
                m_hdr = re.search(r'\[(\d+)/\d+\]\s+(.+?)$', line)
                if m_hdr:
                    current_module = {'module': m_hdr.group(2).strip(), 'total': 0, 'pass': 0, 'fail': 0, 'skip': 0, 'results': []}
                    modules.append(current_module)
                    continue

                # 테스트 결과: "  [PASS] CSP-IF-01 stats 요청 (0ms)"
                m_res = re.match(r'\s*\[(PASS|FAIL|SKIP)\]\s+(\S+)\s+(.+?)\s+\((\d+)ms\)', line)
                if m_res and current_module is not None:
                    status = m_res.group(1)
                    tid = m_res.group(2)
                    name = m_res.group(3)
                    ms = int(m_res.group(4))
                    current_module['total'] += 1
                    if status == 'PASS': current_module['pass'] += 1
                    elif status == 'FAIL': current_module['fail'] += 1
                    else: current_module['skip'] += 1
                    current_module['results'].append({'id': tid, 'name': name, 'status': status, 'detail': '', 'elapsed_ms': ms})

                # FAIL 상세: "         detail..."
                if current_module and current_module['results'] and line.startswith('         '):
                    current_module['results'][-1]['detail'] = line.strip()

        return HandlerResult(status=200, body={
            'total': total, 'pass': pass_count, 'fail': fail_count, 'skip': 0,
            'elapsed': elapsed,
            'modules': modules,
        })

    except subprocess.TimeoutExpired:
        return HandlerResult(status=500, body={'error': 'Verification timeout (600s)'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


async def _get_report() -> HandlerResult:
    report_path = os.path.join(_TESTS_DIR, 'verification_report.md')
    if not os.path.exists(report_path):
        return HandlerResult(status=404, body={'error': 'Report not found'})

    with open(report_path, 'r') as f:
        content = f.read()

    return HandlerResult(status=200, body={'content': content})


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
    })


def _build_phase_argv(phase: int, opts: dict) -> list:
    """opts → cims.sh argv. Phase 별 옵션 호환성 적용."""
    skip_build = bool(opts.get('skip_build', True))
    skip_pkg   = bool(opts.get('skip_pkg', True))
    skip_reset = bool(opts.get('skip_reset', False))
    keep_agent = bool(opts.get('keep_agent', False))
    argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'verify', f'phase{phase}']
    if skip_build: argv.append('--skip-build')
    if phase == 1:
        if skip_reset: argv.append('--skip-reset')
    else:
        if skip_pkg:   argv.append('--skip-pkg')
        if keep_agent: argv.append('--keep-agent')
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
            cwd=_SCRIPT_DIR,
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


CIMS_VERIFICATION_HANDLER_LIST = [
    (_VER_BASE, handle_verification, {}),
]
