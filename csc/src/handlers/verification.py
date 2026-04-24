"""검증 실행 및 리포트 API

엔드포인트:
  /run                                    — 기존 run_all.py (Phase 1 테스트 세밀)
  /report                                 — 기존 verification_report.md

  /phases/<N> (POST)                      — cims.sh verify phase<N> 실행 (N=1/2/3)
  /phases/<N>/latest-report (GET)         — verify_reports/*_phase<N>.md 최신 내용
  /phases/<N>/reports (GET)               — verify_reports/*_phase<N>.md 목록
"""
import os
import re
import json
import glob
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


async def _run_phase(phase: int, handler_args: HandlerArgs) -> HandlerResult:
    if phase not in (1, 2, 3):
        return HandlerResult(status=400, body={'error': 'phase must be 1, 2, or 3'})
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    skip_build  = bool(opts.get('skip_build', True))
    skip_pkg    = bool(opts.get('skip_pkg', True))
    keep_agent  = bool(opts.get('keep_agent', False))

    argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'verify', f'phase{phase}']
    if skip_build: argv.append('--skip-build')
    if skip_pkg:   argv.append('--skip-pkg')
    if keep_agent: argv.append('--keep-agent')

    timeout = _PHASE_TIMEOUT.get(phase, 600)
    # async handler 에서 blocking subprocess.run 은 uvicorn 이벤트 루프를 block 하여
    # 자기 자신 (TB-CSC) 에 대한 self-call (curl) 이 실패. to_thread 로 worker 분리.
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

    # 판정: 최신 리포트 파일의 "## 판정: PASS|FAIL" 라인 파싱
    report_path = _find_latest_phase_report(phase)
    verdict = 'UNKNOWN'
    report_ts = ''
    if report_path:
        report_ts = os.path.basename(report_path).split('_phase')[0]
        try:
            with open(report_path) as f:
                content = f.read()
            m = re.search(r'^##\s*판정[:：]\s*(\w+)', content, re.MULTILINE)
            if m:
                verdict = m.group(1).upper()
        except Exception:
            pass

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
