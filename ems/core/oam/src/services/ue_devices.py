"""단말 속성 — file-store `ue_devices` (mcptt_management_views.md §4.1).

원천 = CSP 가 REGISTER 에서 남기는 관측 줄 `{Stats.Dir}/ue_devices/YYYYMMDD.jsonl`
(`CCallDir::DeviceSeen` — `+sip.instance`(RFC 5626·7254)·`User-Agent`(RFC 3261 §20.41)·도달 경로). CSP 는 달라졌을
때·1 시간 간격으로만 쓰므로 줄 수는 등록 수에 비례하지 갱신 주기에 비례하지 않는다.

oam-svc 주기 루프가 `fold()` 로 새 줄만 읽어(파일별 바이트 오프셋 커서) 단말 레코드로 접는다. 레코드 키는 불변 id
`<가입 번호>__<instance>` (identifier_model.md) — 같은 번호에 단말이 바뀌면 레코드가 하나 더 생기고, 같은 단말의
재등록은 같은 레코드의 last_seen 만 민다. `instance` 가 없는 단말(옛 앱·pjsip 기본값 없이 UDP 등록)은
`<가입 번호>__-` 하나로 모은다.

레코드 필드: subscription_id · kind · instance_id · imei · model · os · app · app_version · user_agent · transport ·
addr · node · expires · registered · first_seen · last_seen · last_register · last_unregister.
IMEI 는 `urn:gsma:imei:` 일 때만 채운다(`urn:uuid:` 는 기기 고유 값이지만 IMEI 가 아니다).
"""
import json
import os
import re
from typing import Optional

from services import file_store

# oam-svc 소유 공간(runtime_store_v2 — modules/<owner>/runtime). 관리 store 는 단일 writer 라 oam-svc 는 자기 서브트리에
#   소유권 리스를 잡고 쓴다(oam_svc_app — 계측기와 같은 규약). 조회 핸들러도 oam-svc 안에서 돈다.
OWNER_ROOT = os.path.join('modules', 'oam-svc', 'runtime')
DOMAIN = os.path.join(OWNER_ROOT, 'ue_devices')
_SRC_DIR = 'ue_devices'           # {Stats.Dir} 아래 CSP 관측 줄
_CURSOR = '.fold_cursor.json'     # 도메인 디렉터리 안 — {파일명: 바이트 오프셋}
_CURSOR_KEEP_DAYS = 3             # 커서는 최근 파일만 기억한다(지난 날 파일은 다 읽었다)

# CIMS-PTT/1.4.2 (Android 14; SM-G991N) · CIMS-VoLTE/… · CIMS-Dispatch/… (ue_sdk.md §4.2 userAgentOf)
_UA_RE = re.compile(r'^\s*([^/\s]+)/(\S+)\s*\(([^;)]*);\s*([^)]*)\)')
_UA_PRODUCT_RE = re.compile(r'^\s*([^/\s]+)(?:/(\S+))?')


def parse_user_agent(ua: str) -> dict:
    """User-Agent → {app, app_version, os, model}. 형식 밖이면 첫 product 토큰만."""
    ua = ua or ''
    m = _UA_RE.match(ua)
    if m:
        return {'app': m.group(1), 'app_version': m.group(2), 'os': m.group(3).strip(), 'model': m.group(4).strip()}
    m = _UA_PRODUCT_RE.match(ua)
    if m and ua.strip():
        return {'app': m.group(1), 'app_version': m.group(2) or '', 'os': '', 'model': ''}
    return {'app': '', 'app_version': '', 'os': '', 'model': ''}


def _features(raw: str) -> list:
    """Contact feature tag 줄(`;` 구분) → 목록. 값의 퍼센트 인코딩(`urn%3Aurn-7%3A…`)은 표시용으로 푼다."""
    from urllib.parse import unquote
    return [unquote(f) for f in raw.split(';') if f]


def imei_of(instance: str) -> str:
    """`urn:gsma:imei:<TAC 8>-<SNR 6>-<spare>` (RFC 7254) → 15 자리 숫자. 그 밖의 URN 은 빈 문자열."""
    s = (instance or '').strip().lower()
    if not s.startswith('urn:gsma:imei:'):
        return ''
    body = s[len('urn:gsma:imei:'):].split(';', 1)[0]
    digits = ''.join(c for c in body if c.isdigit())
    return digits[:15] if len(digits) >= 14 else ''


def mask_imei(imei: str) -> str:
    """개인 식별 정보 — 가운데를 가린다(`3512…7890`)."""
    if not imei:
        return ''
    return imei[:4] + '…' + imei[-4:] if len(imei) > 8 else '…'


def device_key(user: str, instance: str) -> str:
    return f"{user}__{instance or '-'}"


def owner_root(config: dict) -> str:
    return os.path.join(file_store.runtime_root(config), OWNER_ROOT)


def domain_dir(config: dict) -> str:
    return file_store.domain_dir(config, DOMAIN)


def _load_cursor(ddir: str) -> dict:
    try:
        with open(os.path.join(ddir, _CURSOR), encoding='utf-8') as f:
            return json.load(f) or {}
    except (OSError, ValueError):
        return {}


def _save_cursor(ddir: str, cur: dict) -> None:
    keep = sorted(cur)[-_CURSOR_KEEP_DAYS:]
    file_store._atomic_write_json(os.path.join(ddir, _CURSOR), {k: cur[k] for k in keep})


def _current_key(recs: dict, user: str) -> str:
    """그 번호의 지금 단말 — 등록 중인 레코드, 없으면 최근 관측, 레코드가 아예 없으면 `<번호>__-` 를 만든다."""
    mine = [(k, r) for k, r in recs.items() if r.get('subscription_id') == user]
    live = [kr for kr in mine if kr[1].get('registered')]
    pick = max(live or mine, key=lambda kr: kr[1].get('last_seen') or '', default=None)
    if pick:
        return pick[0]
    k = device_key(user, '')
    recs[k] = {'subscription_id': user, 'instance_id': '', 'registered': False}
    return k


def _apply(row: dict, recs: dict, dirty: set) -> None:
    user = row.get('user') or ''
    if not user:
        return
    ts = row.get('ts') or ''
    ev = row.get('event')
    if ev == 'subscribe':
        # 문서 구독(GMS·CMS xcap-diff) — 지금 단말에 붙인다. expires 0 = 해지
        k = _current_key(recs, user)
        subs = recs[k].setdefault('subscriptions', {})
        pkg = row.get('package') or ''
        exp = int(row.get('expires') or 0)
        cur = subs.get(pkg) or {}
        if exp > 0:
            subs[pkg] = {'since': cur.get('since') if cur.get('expires') else ts, 'last': ts, 'expires': exp}
        else:
            subs[pkg] = {'since': cur.get('since'), 'last': ts, 'expires': 0, 'ended': ts}
        dirty.add(k)
        return
    if ev == 'media':
        # 코덱 능력 — 그 단말이 낸 INVITE 오퍼(SDP rtpmap). 선호 순
        k = _current_key(recs, user)
        recs[k]['codecs'] = {'audio': [c for c in (row.get('audio') or '').split(',') if c],
                             'video': [c for c in (row.get('video') or '').split(',') if c], 'seen': ts}
        dirty.add(k)
        return
    if ev == 'unregister':
        # 해제 줄에는 단말 식별이 없다 — 그 번호의 등록 중 레코드를 모두 내린다(등록은 번호 단위, CSP UserMap)
        for k, rec in recs.items():
            if rec.get('subscription_id') == user and rec.get('registered'):
                rec.update(registered=False, last_unregister=ts)
                dirty.add(k)
        return
    inst = row.get('instance') or ''
    k = device_key(user, inst)
    # 같은 번호의 다른 단말이 등록 중이면 그 단말은 밀려났다(바인딩은 번호당 하나 — registration_binding_set.md §8)
    for ok, other in recs.items():
        if ok != k and other.get('subscription_id') == user and other.get('registered'):
            other['registered'] = False
            dirty.add(ok)
    rec = recs.setdefault(k, {'subscription_id': user, 'instance_id': inst, 'first_seen': ts})
    ua = row.get('user_agent') or ''
    rec.update(parse_user_agent(ua))
    rec.update({
        'kind': row.get('kind') or rec.get('kind') or '', 'imei': imei_of(inst), 'user_agent': ua,
        'transport': row.get('transport') or '', 'addr': row.get('addr') or '', 'node': row.get('node') or '',
        'expires': int(row.get('expires') or 0), 'registered': True, 'last_seen': ts, 'last_register': ts,
        'features': _features(row.get('features') or ''),
    })
    dirty.add(k)


def fold(config: dict, stats_dir: str) -> int:
    """새 관측 줄을 접어 레코드를 갱신한다 → 읽은 줄 수. 파일은 날짜순, 파일 안은 쓴 순서."""
    src = os.path.join(stats_dir or '', _SRC_DIR)
    if not stats_dir or not os.path.isdir(src):
        return 0
    ddir = domain_dir(config)
    cur = _load_cursor(ddir)
    files = sorted(f for f in os.listdir(src) if f.endswith('.jsonl'))
    # 커서보다 오래된 파일은 이미 다 읽었다 — 커서가 비었으면(첫 기동) 전부 읽는다
    oldest = min(cur) if cur else ''
    recs = None       # 새 줄이 있을 때만 도메인을 읽는다
    dirty: set = set()
    n = 0
    for name in files:
        if cur and name < oldest:
            continue
        path = os.path.join(src, name)
        off = int(cur.get(name, 0) or 0)
        try:
            size = os.path.getsize(path)
            if size < off:          # 파일이 바뀌었다(재작성) — 처음부터
                off = 0
            if size == off:
                cur[name] = off
                continue
            with open(path, 'rb') as f:
                f.seek(off)
                data = f.read()
        except OSError:
            continue
        end = data.rfind(b'\n') + 1     # 쓰는 중인 마지막 줄은 다음 차례
        for raw in data[:end].splitlines():
            try:
                row = json.loads(raw)
            except ValueError:
                continue
            if recs is None:
                recs = {device_key(r['subscription_id'], r.get('instance_id') or ''): r
                        for r in file_store.load_all(ddir) if r.get('subscription_id')}
            _apply(row, recs, dirty)
            n += 1
        cur[name] = off + end
    for k in dirty:
        file_store.save(ddir, k, recs[k])
    _save_cursor(ddir, cur)
    return n


def load_all(config: dict) -> list:
    return [r for r in file_store.load_all(domain_dir(config)) if r.get('subscription_id')]


def by_user(records: list) -> dict:
    """{가입 번호: [레코드…]} — 등록 중인 것 먼저, 그다음 last_seen 최근순."""
    out: dict = {}
    for r in records:
        out.setdefault(r['subscription_id'], []).append(r)
    for v in out.values():
        v.sort(key=lambda r: r.get('last_seen') or '', reverse=True)
        v.sort(key=lambda r: not r.get('registered'))     # 안정 정렬 — 등록 중 먼저, 같은 쪽은 최근순
    return out


_SUB_PACKAGES = ('gms', 'cms')


def subscription_view(rec: dict, now: Optional[float] = None) -> dict:
    """{gms|cms: {active, since, expires_at, ended}} — 활성 = 마지막 수락 + 부여 만료가 지금보다 뒤이고 해지되지 않음.
    CSP 는 만료의 절반 간격으로 갱신을 남기므로 살아 있는 구독은 이 판정에서 끊기지 않는다."""
    from datetime import datetime, timedelta
    now_dt = datetime.fromtimestamp(now) if now else datetime.now()
    out = {}
    for pkg in _SUB_PACKAGES:
        s = ((rec or {}).get('subscriptions') or {}).get(pkg)
        if not s:
            out[pkg] = {'active': False, 'since': None, 'expires_at': None, 'ended': None}
            continue
        exp_at = None
        try:
            if s.get('expires'):
                exp_at = datetime.fromisoformat(s['last']) + timedelta(seconds=int(s['expires']))
        except (KeyError, ValueError, TypeError):
            exp_at = None
        out[pkg] = {'active': bool(exp_at and exp_at > now_dt), 'since': s.get('since'),
                    'expires_at': exp_at.isoformat(timespec='seconds') if exp_at else None, 'ended': s.get('ended')}
    return out


def view(rec: Optional[dict], raw_imei: bool = False, now: Optional[float] = None) -> Optional[dict]:
    """화면용 — IMEI 는 관리자만 원문(`raw_imei`), 그 밖에는 가운데를 가린다."""
    if not rec:
        return None
    imei = rec.get('imei') or ''
    return {
        'instance_id': rec.get('instance_id') or '', 'imei': imei if raw_imei else mask_imei(imei),
        'imei_masked': bool(imei) and not raw_imei, 'app': rec.get('app') or '', 'app_version': rec.get('app_version') or '',
        'os': rec.get('os') or '', 'model': rec.get('model') or '', 'user_agent': rec.get('user_agent') or '',
        'transport': rec.get('transport') or '', 'addr': rec.get('addr') or '', 'node': rec.get('node') or '',
        'expires': rec.get('expires') or 0, 'registered': bool(rec.get('registered')),
        'first_seen': rec.get('first_seen') or '', 'last_seen': rec.get('last_seen') or '',
        'last_register': rec.get('last_register') or '', 'last_unregister': rec.get('last_unregister') or '',
        'features': rec.get('features') or [],
        'codecs': rec.get('codecs') or {'audio': [], 'video': [], 'seen': ''},
        'subscriptions': subscription_view(rec, now),
    }
