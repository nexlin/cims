"""검증 실행 및 리포트 API"""
import os
import json
import subprocess
from urllib.parse import urlparse
from httpsrv.handler import HandlerArgs, HandlerResult

_VER_BASE = '/api/v1/verification'
_TESTS_DIR = ''

def init(tests_dir: str):
    global _TESTS_DIR
    _TESTS_DIR = tests_dir


async def handle_verification(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    path = urlparse(handler_args.full_path).path
    after = path[len(_VER_BASE):].lstrip('/')
    method = handler_args.method.upper()

    if after == 'run' and method == 'POST':
        return await _run_verification()
    if after == 'report' and method == 'GET':
        return await _get_report()

    return HandlerResult(status=404, body={'error': 'Not Found'})


async def _run_verification() -> HandlerResult:
    if not _TESTS_DIR:
        return HandlerResult(status=500, body={'error': 'tests_dir not configured'})

    run_all = os.path.join(_TESTS_DIR, 'run_all.py')
    if not os.path.exists(run_all):
        return HandlerResult(status=500, body={'error': f'run_all.py not found: {run_all}'})

    try:
        result = subprocess.run(
            ['python3', run_all],
            capture_output=True, text=True, timeout=600,
            cwd=_TESTS_DIR,
        )
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


CIMS_VERIFICATION_HANDLER_LIST = [
    (_VER_BASE, handle_verification, {}),
]
