"""콘솔 D1 — 위젯 카탈로그 + 프로파일 템플릿 + 사용자별 레이아웃 (oam_base_service_split §6).

base OAM 이 full 콘솔 번들(전 위젯)을 단독 서빙(I2)하되, "사용자가 무엇을 보는가"는 서버 저장
레이아웃으로 개인화한다(D1). 권한(RBAC)과 표현(레이아웃)은 직교하며(D7), **카탈로그/저장 모두
서버가 RBAC 를 강제**한다(레이아웃이 위젯을 숨기는 것에 보안을 의존하지 않음 — 심층방어).

이 핸들러는 console.py(`/api/v1/console`) 보다 더 구체적인 경로를 소유한다(controller 최장 일치):
  GET    /api/v1/console/catalog        위젯 카탈로그 = (RBAC 허용) 필터 + 서비스 가용 annotate
  GET    /api/v1/console/profiles       사용자 role 로 허용된 프로파일 템플릿 목록
  GET    /api/v1/console/layouts/me     본인 레이아웃 (override 있으면 그것, 없으면 프로파일 기본)
  PUT    /api/v1/console/layouts/me     본인 레이아웃 override 저장 (서버측 RBAC 강제)
  DELETE /api/v1/console/layouts/me     override 삭제 → 프로파일 기본으로 리셋

저장: file_store 도메인 `console_user_layouts`(console 카테고리, base 소유 I5), key=login_id.
console.py 의 `console_layouts`(페이지별 위젯 배치, 공유) 와 별개 도메인.

서비스 가용성(D1): widget.requires_service 의 업스트림이 미설치/disabled 면 `available:false` 로
내려보내 콘솔이 "서비스 일시 불가/미설치"로 표기한다(빈 화면 금지). 설치 판정은 게이트웨이 라우트
테이블(분리 배포) ∪ in-process(단일프로세스 all 모드).
"""
from urllib.parse import urlparse, unquote
from pathlib import PurePath
from datetime import datetime
import json

from httpsrv.handler import HandlerArgs, HandlerResult
from services import file_store
from handlers import auth

try:
    from services.admin_auth import role_rank, ROLES
except Exception:
    _RANK = {'user': 0, 'monitor': 1, 'operator': 2, 'manager': 3, 'admin': 4}
    ROLES = tuple(_RANK)
    def role_rank(r):  # noqa
        return _RANK.get(r or '', 0)

try:
    from handlers import gateway as _gateway
except Exception:
    _gateway = None

try:
    from handlers import console_accounts as _accounts
except Exception:
    _accounts = None


_BASE = '/api/v1/console'
_DOMAIN = 'console_user_layouts'

# 알려진 서비스 모듈(분리 배포 시 게이트웨이 업스트림). base(None) 위젯은 항상 가용.
_KNOWN_SERVICES = ('csc', 'oam-svc')

# ── 위젯 카탈로그(정책 SoT) ─────────────────────────────────────────────────
#  콘솔 프런트(widgets registry)가 컴포넌트를 소유하고, 서버는 RBAC/가용성 정책을 소유.
#  area: 운용(ops) / 관리(admin). requires_service: None=base · 'csc' · 'oam-svc'.
#  min_role: 이 위젯을 카탈로그에 노출할 최소 권한(서버 강제). default_w: 12-col 폭.
_CATALOG = [
    # base(인프라/관제) — 서비스 무관, 항상 가용
    {'id': 'core.system-cards',  'title': '시스템 형상',      'area': 'ops',   'requires_service': None,       'min_role': 'monitor',  'default_w': 12},
    {'id': 'core.node-health',   'title': '노드 헬스',        'area': 'ops',   'requires_service': None,       'min_role': 'monitor',  'default_w': 6},
    {'id': 'core.alerts',        'title': '알람',             'area': 'ops',   'requires_service': None,       'min_role': 'monitor',  'default_w': 6},
    {'id': 'core.agents',        'title': 'Agent 현황',       'area': 'admin', 'requires_service': None,       'min_role': 'operator', 'default_w': 6},
    {'id': 'core.ha',            'title': 'HA 그룹',          'area': 'admin', 'requires_service': None,       'min_role': 'operator', 'default_w': 6},
    {'id': 'core.deployment',    'title': '배포/패키지',      'area': 'admin', 'requires_service': None,       'min_role': 'manager',  'default_w': 12},
    {'id': 'core.external',      'title': '외부 시스템',      'area': 'admin', 'requires_service': None,       'min_role': 'operator', 'default_w': 6},
    # oam-svc(서비스 관측/녹취/flow/검증)
    {'id': 'svc.service-status', 'title': '서비스 현황',      'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'monitor',  'default_w': 12},
    {'id': 'svc.stats-volte',    'title': 'VoLTE 통계',       'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'monitor',  'default_w': 6},
    {'id': 'svc.stats-ptt',      'title': 'PTT 통계',         'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'monitor',  'default_w': 6},
    {'id': 'svc.history-volte',  'title': 'VoLTE 호 이력',    'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'monitor',  'default_w': 12},
    {'id': 'svc.history-ptt',    'title': 'PTT 세션 이력',    'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'monitor',  'default_w': 12},
    {'id': 'svc.abnormal',       'title': '비정상 세션 이력', 'area': 'ops',   'requires_service': 'oam-svc', 'min_role': 'operator', 'default_w': 12},
    {'id': 'svc.verification',   'title': '검증(S1~S6)',      'area': 'admin', 'requires_service': 'oam-svc', 'min_role': 'manager',  'default_w': 12},
    # csc(가입자/조직/PTT그룹)
    {'id': 'csc.organizations',  'title': '조직',             'area': 'admin', 'requires_service': 'csc',      'min_role': 'operator', 'default_w': 6},
    {'id': 'csc.subscribers',    'title': '사용자',           'area': 'admin', 'requires_service': 'csc',      'min_role': 'operator', 'default_w': 12},
    {'id': 'csc.ptt-groups',     'title': 'PTT 그룹',         'area': 'admin', 'requires_service': 'csc',      'min_role': 'operator', 'default_w': 12},
]
_CATALOG_BY_ID = {w['id']: w for w in _CATALOG}

# ── 프로파일 템플릿 ─────────────────────────────────────────────────────────
#  role 별 시작 레이아웃(대시보드 위젯 세트). 사용자는 이 위에 개인화를 레이어한다.
#  allow_roles = 이 프로파일을 선택/할당 가능한 role 들.
_PROFILES = [
    {'id': 'monitor',  'label': '감시자',  'allow_roles': ('monitor', 'operator', 'manager', 'admin'),
     'dashboard': ['core.system-cards', 'core.node-health', 'core.alerts',
                   'svc.service-status', 'svc.stats-volte', 'svc.stats-ptt']},
    {'id': 'operator', 'label': '운용자',  'allow_roles': ('operator', 'manager', 'admin'),
     'dashboard': ['core.system-cards', 'core.node-health', 'core.alerts', 'core.agents',
                   'svc.service-status', 'svc.history-volte', 'svc.history-ptt', 'svc.abnormal']},
    {'id': 'manager',  'label': '관리자(운영)', 'allow_roles': ('manager', 'admin'),
     'dashboard': ['core.system-cards', 'core.node-health', 'core.deployment',
                   'csc.subscribers', 'csc.ptt-groups', 'svc.verification']},
    {'id': 'admin',    'label': '관리자(전체)', 'allow_roles': ('admin',),
     'dashboard': ['core.system-cards', 'core.node-health', 'core.agents', 'core.ha',
                   'core.deployment', 'core.external', 'csc.subscribers', 'svc.verification']},
]
_PROFILE_BY_ID = {p['id']: p for p in _PROFILES}

# role → 기본 프로파일 (계정에 base_profile 미지정 시).
_DEFAULT_PROFILE_FOR_ROLE = {
    'user': 'monitor', 'monitor': 'monitor', 'operator': 'operator',
    'manager': 'manager', 'admin': 'admin',
}


def _parts(full_path: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(_BASE))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


def _body_dict(handler_args: HandlerArgs):
    b = getattr(handler_args, 'body', None)
    if isinstance(b, dict):
        return b
    if isinstance(b, (bytes, bytearray)):
        try: return json.loads(b.decode('utf-8'))
        except Exception: return None
    if isinstance(b, str):
        try: return json.loads(b)
        except Exception: return None
    return None


def _dir(config):
    return file_store.domain_dir(config, _DOMAIN)


# 이 프로세스가 **직접(in-process) 서빙하는** 서비스 모듈 — oam_app 이 기동 시 알려준다.
# role=all 이면 {'oam-svc'}(+ csc 동봉 시 'csc'), role=base 면 빈 집합.
_INPROC: set = set()
_INPROC_SET = False          # oam_app 이 알려줬는가 (구 경로 호환 판별용)


def set_inprocess_services(services) -> None:
    """기동 시 1회 — 이 프로세스가 직접 서빙하는 서비스 모듈 집합을 등록."""
    global _INPROC, _INPROC_SET
    _INPROC = {str(s).lower() for s in (services or set())}
    _INPROC_SET = True


def installed_services(config: dict) -> set:
    """가용 서비스 모듈 집합 = **내가 직접 서빙하는 것 ∪ 게이트웨이로 도달 가능한 것**.

      · in-process(`_INPROC`): role=all 이 자기 프로세스에 등록한 서비스 핸들러(stats/녹취/flow/검증
        = oam-svc, csc 동봉 시 csc).
      · 라우트 테이블: 다른 서버에 배포돼 게이트웨이 프록시로 닿는 서비스.

    **라우트 0개 = 서비스 0개** 는 role=base 에서만 성립한다(그때 `_INPROC` 이 비어 있다).
    전체 폴백은 하지 않는다 — 과거 폴백 버그로 base 에서 미배포 서비스가 가용으로 오보돼
    base 대시보드에 svc 위젯이 노출됐다.

    게이트웨이 등록 여부로 role 을 추정하지 않는다: `register_gateway` 는 **role=all 하이브리드**
    (csc 만 프록시, oam-svc 는 in-process)에서도 호출되므로 그걸 base 로 읽으면 in-process 서비스가
    통째로 미가용이 된다 — API 문서(`/api-docs`)에서 stats 계열이 사라지고 위젯 가용성도 오판한다.

    반환값은 **소문자로 정규화**한다 — 라우트 테이블은 배포 모듈명을 대문자('OAM-SVC'/'CSC')로 저장하는데
    비교 대상(_KNOWN_SERVICES, 위젯 requires_service, API 문서 module)은 소문자라 그대로 두면 전부 미가용으로
    오판한다.
    """
    routed = set()
    try:
        if _gateway is not None:
            routed = {str(r['module']).lower() for r in _gateway.enabled_routes(config) if r.get('module')}
    except Exception:
        routed = set()
    if _INPROC_SET:
        return set(_INPROC) | routed
    # oam_app 이 알려주지 않은 경로(구 호출자·단위 테스트) — 게이트웨이 미장착이면 전부 가용.
    gw_active = _gateway is not None and getattr(_gateway, '_ADMIN_SERVER', None) is not None
    return routed if gw_active else set(_KNOWN_SERVICES)


def _filter_catalog(role: str, config: dict) -> list:
    """role 로 RBAC 필터(min_role 강제) + 서비스 가용 annotate(D7)."""
    rank = role_rank(role)
    inst = installed_services(config)
    out = []
    for w in _CATALOG:
        if rank < role_rank(w['min_role']):
            continue   # RBAC: 권한 미달 위젯은 카탈로그에 노출하지 않음(서버 강제)
        req = w.get('requires_service')
        out.append({
            'id': w['id'], 'title': w['title'], 'area': w['area'],
            'requires_service': req, 'default_w': w['default_w'],
            'available': (req is None) or (req in inst),
        })
    return out


def _allowed_widget_ids(role: str) -> set:
    rank = role_rank(role)
    return {w['id'] for w in _CATALOG if rank >= role_rank(w['min_role'])}


def _profiles_for_role(role: str) -> list:
    return [{'id': p['id'], 'label': p['label'], 'dashboard': p['dashboard']}
            for p in _PROFILES if role in p['allow_roles']]


def _account_base_profile(config: dict, login_id: str, role: str):
    """계정에 저장된 base_profile, 없으면 role 기본."""
    prof = None
    if _accounts is not None:
        try:
            acc = _accounts.get_account(config, login_id) or {}
            prof = acc.get('base_profile')
        except Exception:
            prof = None
    if prof not in _PROFILE_BY_ID:
        prof = _DEFAULT_PROFILE_FOR_ROLE.get(role, 'monitor')
    return prof


def _profile_layout(profile_id: str, role: str) -> dict:
    """프로파일 템플릿 → 기본 레이아웃(대시보드 위젯 배치). RBAC 로 한번 더 필터."""
    p = _PROFILE_BY_ID.get(profile_id) or _PROFILE_BY_ID['monitor']
    allowed = _allowed_widget_ids(role)
    widgets = [wid for wid in p['dashboard'] if wid in allowed]
    return {'pages': [{'slug': '/dashboard', 'widgets': widgets}],
            'widgets': {'dashboard': widgets}}


async def handle_console_layouts(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parts = _parts(handler_args.full_path)
    method = (handler_args.method or 'GET').upper()

    payload, err = auth.require_auth(handler_args)
    if err:
        return err
    role = payload.get('role') or 'user'
    login_id = payload.get('login_id') or str(payload.get('sub') or '')

    # GET /catalog
    if parts == ('catalog',) and method == 'GET':
        return HandlerResult(status=200, body={
            'role': role,
            'installed_services': sorted(installed_services(config)),
            'widgets': _filter_catalog(role, config),
        })

    # GET /profiles
    if parts == ('profiles',) and method == 'GET':
        return HandlerResult(status=200, body={
            'role': role,
            'default': _account_base_profile(config, login_id, role),
            'profiles': _profiles_for_role(role),
        })

    # /layouts/me
    if parts == ('layouts', 'me'):
        d = _dir(config)
        if method == 'GET':
            stored = file_store.load(d, login_id)
            base_profile = (stored or {}).get('base_profile') \
                or _account_base_profile(config, login_id, role)
            if stored and stored.get('layout'):
                return HandlerResult(status=200, body={
                    'login_id': login_id, 'role': role, 'base_profile': base_profile,
                    'source': 'override', 'layout': stored['layout'],
                    'updated_at': stored.get('updated_at')})
            return HandlerResult(status=200, body={
                'login_id': login_id, 'role': role, 'base_profile': base_profile,
                'source': 'profile', 'layout': _profile_layout(base_profile, role)})

        if method == 'PUT':
            body = _body_dict(handler_args)
            if not isinstance(body, dict):
                return HandlerResult(status=400, body={'error': 'json body required'})
            layout = body.get('layout')
            if not isinstance(layout, dict):
                return HandlerResult(status=400, body={'error': 'layout object required'})
            # D7 서버측 RBAC 강제 — 레이아웃에 권한 밖 위젯이 들어오면 거부(심층방어).
            allowed = _allowed_widget_ids(role)
            used = set()
            for pg in (layout.get('pages') or []):
                used.update(pg.get('widgets') or [])
            for wl in (layout.get('widgets') or {}).values():
                used.update(wl or [])
            unknown = [w for w in used if w not in _CATALOG_BY_ID]
            forbidden = [w for w in used if w in _CATALOG_BY_ID and w not in allowed]
            if unknown:
                return HandlerResult(status=400, body={'error': 'unknown widget id', 'widgets': unknown})
            if forbidden:
                return HandlerResult(status=403, body={'error': 'widget not permitted for role', 'widgets': forbidden})
            base_profile = body.get('base_profile')
            if base_profile not in _PROFILE_BY_ID:
                base_profile = _account_base_profile(config, login_id, role)
            rec = {
                'login_id': login_id, 'base_profile': base_profile,
                'layout': layout, 'overrides_from_profile': True,
                'updated_at': datetime.now().isoformat(timespec='seconds'),
            }
            file_store.save(d, login_id, rec)
            return HandlerResult(status=200, body={'saved': True, 'login_id': login_id})

        if method == 'DELETE':
            ok = file_store.delete(d, login_id)
            return HandlerResult(status=200, body={'reset': ok, 'login_id': login_id})

    return HandlerResult(status=404, body={'error': 'Not Found'})


CIMS_CONSOLE_LAYOUTS_HANDLER_LIST = [
    (f'{_BASE}/catalog',     handle_console_layouts, {}),
    (f'{_BASE}/profiles',    handle_console_layouts, {}),
    (f'{_BASE}/layouts/me',  handle_console_layouts, {}),
]
