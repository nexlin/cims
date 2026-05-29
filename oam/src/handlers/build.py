"""빌드 / 패키지화 / 패키지 다운로드 API.

콘솔 "빌드 · 검증 > 모듈관리" 페이지에서 사용. cims.sh build / pkg 를 비동기
실행하고, 진행 상태 폴링과 산출 tarball 다운로드를 제공한다. verification.py
와 동일한 _start_job → _watch_job → /jobs/<id> 패턴을 그대로 차용.

엔드포인트 (모두 admin JWT 필요):
  POST /run                     — cims.sh build 비동기 실행
                                   body: { async: true (기본) }
  POST /pkg                     — cims.sh pkg <module> [--no-bump] 비동기 실행
                                   body: { module: 'csp', no_bump: true (기본) }
  GET  /jobs/<job_id>           — 진행 상태 + stdout tail
  GET  /manifest                — build/dist/packages/manifest.json
  GET  /packages                — manifest.packages[] (없으면 디렉토리 스캔)
  GET  /packages/<module>       — tarball 바이너리 다운로드 (octet-stream)

동시 실행 가드: 빌드/패키지 모두 build/dist 트리를 만지므로 module-level
asyncio.Lock 으로 배타 처리. 진행 중 추가 요청은 409.
"""
import os
import re
import time
import json
import uuid
import asyncio
import hashlib
from typing import Optional
from urllib.parse import urlparse

from httpsrv.handler import HandlerArgs, HandlerResult
from . import auth as _auth
from services import service_registry


_BUILD_BASE = '/api/v1/build'

_SCRIPT_DIR = ''
_DIST_PKG_DIR = ''

# 12개 — base 8 + csp/cmp 변종 4 (psp/isp/pmp/imp). 변종은 cims.sh pkg 가 staging
# 으로 dist/csp 또는 dist/cmp 를 복사 + 바이너리/config rename 후 tar.
_VALID_MODULES = (
    'cmp', 'pmp', 'imp',
    'csp', 'psp', 'isp',
    'cwrtc', 'csc', 'console', 'phone', 'cspsim', 'agent',
)


def _valid_modules() -> set:
    """유효 패키지 모듈명 — service descriptor 구동 (startup config 캐시). 비면 하드코딩 fallback."""
    try:
        v = service_registry.valid_module_names()
        return v if v else set(_VALID_MODULES)
    except Exception:
        return set(_VALID_MODULES)

_BUILD_TIMEOUT = 1800   # 전체 cmake + make + npm
_PKG_TIMEOUT = 300

_JOBS: dict = {}
_JOBS_TTL_SEC = 3600
_JOB_LOG_DIR = '/tmp/cims_build_jobs'

_LOCK = asyncio.Lock()


def init(repo_root: str):
    """csc_app 부팅 시 호출. cims.sh + CMakeLists.txt 가 함께 있는 소스
    트리를 찾아 _SCRIPT_DIR 에 반영. dist 안에서 띄워진 csc 라도 부모로
    올라가 진짜 소스 루트를 잡아낸다 (cims.sh build 는 dist 에서 거부됨)."""
    global _SCRIPT_DIR, _DIST_PKG_DIR
    cur = os.path.abspath(repo_root)
    found = ''
    for _ in range(6):
        if (os.path.isfile(os.path.join(cur, 'cims.sh')) and
                os.path.isfile(os.path.join(cur, 'CMakeLists.txt'))):
            found = cur
            break
        nxt = os.path.dirname(cur)
        if nxt == cur:
            break
        cur = nxt
    _SCRIPT_DIR = found or os.environ.get('CIMS_REPO_ROOT') or repo_root
    _DIST_PKG_DIR = os.path.join(_SCRIPT_DIR, 'build', 'dist', 'packages')


def _gc_jobs():
    now = time.time()
    stale = [jid for jid, j in _JOBS.items()
             if j.get('done') and (now - (j.get('ended_at') or now)) > _JOBS_TTL_SEC]
    for jid in stale:
        j = _JOBS.pop(jid, None)
        if j and j.get('log_path'):
            try: os.remove(j['log_path'])
            except Exception: pass


async def _start_job(kind: str, argv: list, timeout: int, label: str = '') -> str:
    """Spawn subprocess in background, return job_id immediately."""
    _gc_jobs()
    os.makedirs(_JOB_LOG_DIR, exist_ok=True)
    job_id = uuid.uuid4().hex[:12]
    log_path = os.path.join(_JOB_LOG_DIR, f'{kind}_{job_id}.log')
    log_file = open(log_path, 'wb')
    proc = await asyncio.create_subprocess_exec(
        *argv,
        stdout=log_file,
        stderr=asyncio.subprocess.STDOUT,
        cwd=_SCRIPT_DIR,
    )
    job = {
        'job_id': job_id,
        'kind': kind,                  # 'build' | 'pkg'
        'label': label,
        'argv': argv,
        'started_at': time.time(),
        'ended_at': None,
        'log_path': log_path,
        'returncode': None,
        'done': False,
        'verdict': None,
        '_proc': proc,
        '_log_file': log_file,
        '_timeout': timeout,
    }
    _JOBS[job_id] = job
    asyncio.create_task(_watch_job(job_id))
    return job_id


async def _watch_job(job_id: str):
    job = _JOBS.get(job_id)
    if not job:
        return
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
    job['verdict'] = 'PASS' if rc == 0 else 'FAIL'
    job['done'] = True


def _read_manifest() -> Optional[dict]:
    p = os.path.join(_DIST_PKG_DIR, 'manifest.json')
    if not os.path.isfile(p):
        return None
    try:
        with open(p) as f:
            return json.load(f)
    except Exception:
        return None


def _scan_packages_fallback() -> list:
    """manifest.json 이 없을 때 디렉토리 직접 스캔."""
    if not os.path.isdir(_DIST_PKG_DIR):
        return []
    out = []
    for fn in sorted(os.listdir(_DIST_PKG_DIR)):
        if not fn.endswith('.tar.gz'):
            continue
        full = os.path.join(_DIST_PKG_DIR, fn)
        try:
            st = os.stat(full)
        except Exception:
            continue
        out.append({
            'name':  fn,
            'size':  st.st_size,
            'sha256': '',
            'mtime': '',
        })
    return out


def _find_tarball(module: str) -> Optional[str]:
    if module not in _valid_modules():
        return None
    if not os.path.isdir(_DIST_PKG_DIR):
        return None
    pat = re.compile(rf'^{re.escape(module)}-[\w.\-]+\.tar\.gz$')
    matches = [fn for fn in os.listdir(_DIST_PKG_DIR) if pat.match(fn)]
    if not matches:
        return None
    matches.sort(reverse=True)
    return os.path.join(_DIST_PKG_DIR, matches[0])


def _has_active_job() -> Optional[str]:
    """진행 중 job_id 가 있으면 그 id 반환. 없으면 None."""
    for jid, j in _JOBS.items():
        if not j.get('done'):
            return jid
    return None


# ──────────────────────────────────────────────────────────────
#  핸들러 라우팅
# ──────────────────────────────────────────────────────────────

async def handle_build(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    payload, err = _auth.require_admin(handler_args)
    if err:
        return err

    path = urlparse(handler_args.full_path).path
    after = path[len(_BUILD_BASE):].lstrip('/')
    method = handler_args.method.upper()

    if after == 'run' and method == 'POST':
        return await _start_build(handler_args)

    if after == 'pkg' and method == 'POST':
        return await _start_pkg(handler_args)

    if after == 'release' and method == 'POST':
        return await _start_release(handler_args)

    if after == 'clean' and method == 'POST':
        return await _clean_packages()

    m = re.fullmatch(r'jobs/([0-9a-f]+)', after)
    if m and method == 'GET':
        return await _get_job_status(m.group(1))

    if after == 'manifest' and method == 'GET':
        return await _get_manifest()

    if after == 'packages' and method == 'GET':
        return await _list_packages()

    m = re.fullmatch(r'packages/([a-z]+)', after)
    if m and method == 'GET':
        return await _download_package(m.group(1))

    return HandlerResult(status=404, body={'error': 'Not Found'})


async def _start_build(handler_args: HandlerArgs) -> HandlerResult:
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    # version 명시 시 cims.sh build -v <ver> 로 전달 — 이후 pkg --no-bump 가 그 버전 사용.
    raw_version = str(opts.get('version') or '').strip()
    if raw_version and not re.fullmatch(r'[0-9A-Za-z._+\-]{1,64}', raw_version):
        return HandlerResult(status=422, body={
            'error': f'invalid version format: {raw_version!r}',
        })

    async with _LOCK:
        active = _has_active_job()
        if active:
            j = _JOBS[active]
            return HandlerResult(status=409, body={
                'error':  '다른 빌드/패키지 작업 진행 중',
                'job_id': active, 'kind': j.get('kind'), 'label': j.get('label'),
            })
        argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'build']
        if raw_version:
            argv.extend(['-v', raw_version])
        label = f'cims.sh build{" -v " + raw_version if raw_version else ""}'
        job_id = await _start_job('build', argv, _BUILD_TIMEOUT, label=label)

    return HandlerResult(status=202, body={
        'job_id': job_id,
        'kind':   'build',
        'argv':   argv,
        'started_at': _JOBS[job_id]['started_at'],
        'message': 'started',
    })


async def _start_pkg(handler_args: HandlerArgs) -> HandlerResult:
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    # 단일 module 또는 modules 배열 (csp/psp/isp 같이 같은 dist 에서 나오는 변종을
    # 한 job 으로 묶어 패키징하기 위함). 둘 다 미지정이면 422.
    raw_modules = opts.get('modules')
    if isinstance(raw_modules, list) and raw_modules:
        modules = [str(m).strip() for m in raw_modules if str(m).strip()]
    else:
        single = str(opts.get('module') or '').strip()
        modules = [single] if single else []

    no_bump = opts.get('no_bump')
    if no_bump is None:
        no_bump = True
    no_bump = bool(no_bump)

    # version 명시 시 cims.sh pkg -v <ver> 로 전달 — 모든 대상 모듈이 그 버전으로 산출.
    # 명시 시에는 no_bump 무시 (cims.sh 가 -v 를 우선 적용).
    raw_version = str(opts.get('version') or '').strip()
    # 안전: 영숫자 + 점/대시/언더스코어/플러스 만 허용 (shell 인자 주입 방지)
    if raw_version and not re.fullmatch(r'[0-9A-Za-z._+\-]{1,64}', raw_version):
        return HandlerResult(status=422, body={
            'error': f'invalid version format: {raw_version!r}',
        })
    version = raw_version

    if not modules:
        return HandlerResult(status=422, body={
            'error': 'module or modules required',
            'allowed': sorted(_valid_modules()),
        })
    invalid = [m for m in modules if m not in _valid_modules()]
    if invalid:
        return HandlerResult(status=422, body={
            'error': f'invalid module(s): {invalid}',
            'allowed': sorted(_valid_modules()),
        })

    async with _LOCK:
        active = _has_active_job()
        if active:
            j = _JOBS[active]
            return HandlerResult(status=409, body={
                'error':  '다른 빌드/패키지 작업 진행 중',
                'job_id': active, 'kind': j.get('kind'), 'label': j.get('label'),
            })
        argv = [os.path.join(_SCRIPT_DIR, 'cims.sh'), 'pkg']
        if version:
            argv.extend(['-v', version])
        elif no_bump:
            argv.append('--no-bump')
        argv.extend(modules)
        label_module = ' '.join(modules)
        label_version = f' -v {version}' if version else ''
        job_id = await _start_job('pkg', argv, _PKG_TIMEOUT,
                                  label=f'cims.sh pkg{label_version} {label_module}')

    return HandlerResult(status=202, body={
        'job_id': job_id,
        'kind':   'pkg',
        'module': modules[0],          # 후방호환 — 단일 module 의 첫 항목
        'modules': modules,
        'argv':   argv,
        'started_at': _JOBS[job_id]['started_at'],
        'message': 'started',
    })


# 빌드 + 패키징 통합 — 한 job 으로 cims.sh build [-v X.Y.Z] && cims.sh pkg --no-bump 실행.
# 시각적으로 한 작업이고, 진행 표시도 하나의 stdout 으로 수렴.
async def _start_release(handler_args: HandlerArgs) -> HandlerResult:
    if not _SCRIPT_DIR or not os.path.isfile(os.path.join(_SCRIPT_DIR, 'cims.sh')):
        return HandlerResult(status=500, body={'error': f'cims.sh not found at {_SCRIPT_DIR}'})

    body = handler_args.body or {}
    opts = body if isinstance(body, dict) else {}
    raw_version = str(opts.get('version') or '').strip()
    if raw_version and not re.fullmatch(r'[0-9A-Za-z._+\-]{1,64}', raw_version):
        return HandlerResult(status=422, body={
            'error': f'invalid version format: {raw_version!r}',
        })

    async with _LOCK:
        active = _has_active_job()
        if active:
            j = _JOBS[active]
            return HandlerResult(status=409, body={
                'error':  '다른 빌드/패키지 작업 진행 중',
                'job_id': active, 'kind': j.get('kind'), 'label': j.get('label'),
            })
        cims_sh = os.path.join(_SCRIPT_DIR, 'cims.sh')
        # version 은 위에서 정규식 검증 통과 — shell 인자 안전.
        build_cmd = f'{cims_sh} build'
        if raw_version:
            build_cmd += f' -v {raw_version}'
        # build → pkg 순차. && 로 build 실패 시 pkg 안 함.
        chained = f'{build_cmd} && {cims_sh} pkg --no-bump'
        argv = ['/bin/bash', '-c', chained]
        label_version = f' -v {raw_version}' if raw_version else ''
        # build timeout 이 더 길어서 그 값을 사용 (pkg 는 빠름).
        job_id = await _start_job('release', argv, _BUILD_TIMEOUT,
                                  label=f'cims.sh build{label_version} && pkg --no-bump')

    return HandlerResult(status=202, body={
        'job_id': job_id,
        'kind':   'release',
        'argv':   argv,
        'started_at': _JOBS[job_id]['started_at'],
        'message': 'started',
    })


# 패키지 산출물 정리 — build/dist/packages/*.tar.gz + packages/manifest.json 삭제.
# 빌드 결과 (build/dist/<comp>/) 는 건드리지 않음 — 다음 빌드 시 cmake 캐시 활용 위해.
async def _clean_packages() -> HandlerResult:
    if not _DIST_PKG_DIR:
        return HandlerResult(status=500, body={'error': 'DIST_PKG_DIR not set'})
    pkg_dir = _DIST_PKG_DIR
    manifest = os.path.join(pkg_dir, 'manifest.json')
    removed_tarballs = 0
    removed_manifest = False
    errors = []
    if os.path.isdir(pkg_dir):
        for f in os.listdir(pkg_dir):
            if f.endswith('.tar.gz'):
                try:
                    os.remove(os.path.join(pkg_dir, f))
                    removed_tarballs += 1
                except OSError as e:
                    errors.append(f'{f}: {e}')
    if os.path.isfile(manifest):
        try:
            os.remove(manifest)
            removed_manifest = True
        except OSError as e:
            errors.append(f'manifest.json: {e}')
    return HandlerResult(status=200, body={
        'removed_tarballs': removed_tarballs,
        'removed_manifest': removed_manifest,
        'errors': errors,
    })


async def _get_job_status(job_id: str) -> HandlerResult:
    job = _JOBS.get(job_id)
    if not job:
        return HandlerResult(status=404, body={'error': f'job not found: {job_id}'})
    tail = ''
    try:
        with open(job['log_path'], 'rb') as f:
            data = f.read().decode('utf-8', errors='replace')
            tail = '\n'.join(data.splitlines()[-100:])
    except Exception:
        pass
    now = time.time()
    return HandlerResult(status=200, body={
        'job_id':      job_id,
        'kind':        job.get('kind'),
        'label':       job.get('label', ''),
        'argv':        job['argv'],
        'started_at':  job['started_at'],
        'ended_at':    job['ended_at'],
        'elapsed':     (job['ended_at'] or now) - job['started_at'],
        'done':        job['done'],
        'returncode':  job['returncode'],
        'verdict':     job['verdict'] if job['done'] else None,
        'stdout_tail': tail,
    })


async def _get_manifest() -> HandlerResult:
    mf = _read_manifest()
    if mf is None:
        return HandlerResult(status=404, body={
            'error': 'manifest.json 없음 — 패키지화 먼저 실행 (cims.sh pkg)',
        })
    # manifest 파일 자체의 sha256 (S6 immutability gate 매칭용)
    p = os.path.join(_DIST_PKG_DIR, 'manifest.json')
    h = hashlib.sha256()
    try:
        with open(p, 'rb') as f:
            for chunk in iter(lambda: f.read(64 * 1024), b''):
                h.update(chunk)
        mf['_self_sha256'] = h.hexdigest()
    except Exception:
        pass
    return HandlerResult(status=200, body=mf)


async def _list_packages() -> HandlerResult:
    mf = _read_manifest()
    if mf is None:
        return HandlerResult(status=200, body={
            'manifest_present': False,
            'packages': _scan_packages_fallback(),
        })
    return HandlerResult(status=200, body={
        'manifest_present': True,
        'ts':       mf.get('ts'),
        'git':      mf.get('git'),
        'host':     mf.get('host'),
        'packages': mf.get('packages') or [],
    })


async def _download_package(module: str) -> HandlerResult:
    if module not in _valid_modules():
        return HandlerResult(status=422, body={'error': f'invalid module: {module!r}'})
    path = _find_tarball(module)
    if not path:
        return HandlerResult(status=404, body={
            'error': f'no tarball for {module} — 패키지화 먼저 실행',
        })
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except Exception as e:
        return HandlerResult(status=500, body={'error': f'read failed: {e}'})
    fname = os.path.basename(path)
    return HandlerResult(
        status=200, body=data,
        headers={
            'Content-Type':        'application/octet-stream',
            'Content-Disposition': f'attachment; filename="{fname}"',
            'X-Package-Module':    module,
            'X-Package-Filename':  fname,
        },
        media_type='application/octet-stream',
    )


CIMS_BUILD_HANDLER_LIST = [
    (_BUILD_BASE, handle_build, {}),
]
