"""
Console 정적 서빙 — OAM(HTTPS:4419)이 빌드된 콘솔(SPA)을 직접 서빙.

상용(부트스트랩) 배포에서 콘솔이 별도 HTTP dev 서버(vite)/npx serve 없이
OAM 과 **단일 HTTPS 오리진**으로 동작하기 위함:
  - 콘솔의 API 호출이 상대경로(/api/v1)라 same-origin 으로 그대로 동작 (CORS 불요)
  - air-gapped 환경에서 node/npm 불필요

라우팅: base_path "/" 로 등록 — httpsrv 라우터가 최장 일치(base_path 길이순)
이므로 /api/* 등 기존 핸들러가 항상 우선한다.

정적 디렉토리 해석은 oam_app 시작 시 1회 수행되어 config['_ConsoleStaticDir']
로 주입된다 (Console.StaticDir 설정 → versioned/flat 형제 console 탐색).
"""

import os
from urllib.parse import urlparse

from httpsrv.handler import HandlerArgs, HandlerResult

_MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js':   'application/javascript; charset=utf-8',
    '.mjs':  'application/javascript; charset=utf-8',
    '.css':  'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.svg':  'image/svg+xml',
    '.png':  'image/png',
    '.jpg':  'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.gif':  'image/gif',
    '.ico':  'image/x-icon',
    '.woff': 'font/woff',
    '.woff2': 'font/woff2',
    '.ttf':  'font/ttf',
    '.map':  'application/json',
    '.txt':  'text/plain; charset=utf-8',
    '.md':   'text/markdown; charset=utf-8',
    '.pptx': 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
}


def resolve_console_static_dir(config: dict, component_root: str) -> str:
    """콘솔 정적 디렉토리 결정 — 없으면 ''(서빙 비활성).

    우선순위 (콘솔 base/svc 분리 — 백엔드 oam-base/oam-svc 와 대칭):
      1) config Console.StaticDir (절대 또는 component_root 상대)
      2) oam-svc 동봉 svc 콘솔: <modules>/oam-svc/<ver>/oam-svc/console/dist
         — 배포된 oam-svc(최신 우선). oam-svc 배포 시 풀(svc=base+서비스) 콘솔로 자동 승격.
      3) 번들: <component_root>/console/dist — oam-base 동봉 base 콘솔(부트스트랩 기본)
      4) flat 형제:      <root대비 ../console/dist>          (build/dist 개발 트리)
    """
    import glob as _glob
    import re as _re
    cfg = (config.get('Console') or {}).get('StaticDir') or ''
    if cfg:
        p = cfg if os.path.isabs(cfg) else os.path.normpath(os.path.join(component_root, cfg))
        return p if os.path.isdir(p) else ''

    # 콘솔 base/svc 분리 — 배포된 oam-svc 에 동봉된 svc(=base+서비스) 콘솔이 oam-base 에
    # 동봉된 base 콘솔보다 **우선**. oam-svc 를 배포하면(서비스 평면 활성) base OAM 이
    # 자동으로 풀 UI 로 승격되고, 미배포(부트스트랩 직후)면 동봉 base 콘솔로 폴백.
    def _ver_key(path):
        # .../oam-svc/<ver>/oam-svc/console/dist → 버전 자연 정렬 (0.0.10 > 0.0.9)
        m = _re.search(r'/oam-svc/([^/]+)/oam-svc/console/dist$', path.replace(os.sep, '/'))
        if not m:
            return (0,)
        return tuple(int(x) if x.isdigit() else 0 for x in _re.split(r'[.\-]', m.group(1)))

    # (2) oam-svc 동봉 svc 콘솔: <modules>/oam-svc/<ver>/oam-svc/console/dist (최신 우선)
    svc_cons = sorted(_glob.glob(os.path.normpath(
        os.path.join(component_root, '..', '..', '..', 'oam-svc', '*', 'oam-svc', 'console', 'dist'))),
        key=_ver_key, reverse=True)
    for c in svc_cons:
        if os.path.isdir(c):
            return c

    # (3) 번들: <component_root>/console/dist — oam-base 동봉 base 콘솔(부트스트랩 기본)
    bundled = os.path.normpath(os.path.join(component_root, 'console', 'dist'))
    if os.path.isdir(bundled):
        return bundled

    # (4) flat 형제: <component_root>/../console/dist — build/dist 개발 트리
    flat = os.path.normpath(os.path.join(component_root, '..', 'console', 'dist'))
    if os.path.isdir(flat):
        return flat
    return ''


async def handle_console_static(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    static_dir = config.get('_ConsoleStaticDir') or ''
    if not static_dir:
        return HandlerResult(status=404, body={'error': 'console_not_bundled'},
                             media_type='application/json')
    if handler_args.method.upper() not in ('GET', 'HEAD'):
        return HandlerResult(status=405, body={'error': 'method_not_allowed'},
                             media_type='application/json')

    path = urlparse(handler_args.full_path).path
    # /api/* 는 본 핸들러 대상 아님 (최장일치로 도달하지 않지만 방어)
    if path.startswith('/api/'):
        return HandlerResult(status=404, body={'error': 'not_found'},
                             media_type='application/json')

    rel = path.lstrip('/') or 'index.html'
    base = os.path.realpath(static_dir)
    target = os.path.realpath(os.path.join(base, rel))
    # 경로 탈출 차단
    if not (target == base or target.startswith(base + os.sep)):
        return HandlerResult(status=403, body={'error': 'forbidden'},
                             media_type='application/json')
    if not os.path.isfile(target):
        # SPA 라우트 (/dashboard, /deploy/servers …) → index.html fallback.
        # 확장자가 있는 미존재 자산은 404 (조용한 흰화면 디버깅 함정 방지).
        if os.path.splitext(rel)[1]:
            return HandlerResult(status=404, body={'error': 'not_found'},
                                 media_type='application/json')
        target = os.path.join(base, 'index.html')
        if not os.path.isfile(target):
            return HandlerResult(status=404, body={'error': 'index_missing'},
                                 media_type='application/json')

    ext = os.path.splitext(target)[1].lower()
    mime = _MIME.get(ext, 'application/octet-stream')
    with open(target, 'rb') as f:
        data = f.read()
    # vite 빌드 자산(/assets/*.<hash>.*)은 불변 — 적극 캐시. index.html 은 no-cache.
    headers = {'Cache-Control': 'public, max-age=31536000, immutable'} \
        if '/assets/' in target.replace(os.sep, '/') else {'Cache-Control': 'no-cache'}
    return HandlerResult(status=200, body=data, media_type=mime, headers=headers)


CIMS_CONSOLE_STATIC_HANDLER_LIST = (
    ('/', handle_console_static, {}),
)
