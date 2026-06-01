"""
CIMS Stats & Health REST API
실시간 모니터링 + 통계 집계

Routes:
  GET /api/v1/stats/health                          헬스체크 + 실시간 상태
  GET /api/v1/stats/messages                        메시지 통계
  GET /api/v1/stats/service/voip                    VoIP 서비스 통계
  GET /api/v1/stats/service/ptt                     PTT 서비스 통계
  GET /api/v1/stats/service/summary                 일간 KPI 요약
"""

import os
import glob
import json
import socket
import time
import asyncio
from datetime import datetime, timedelta
from urllib.parse import urlparse, parse_qs, unquote
from pathlib import PurePath

import pymysql
import pymysql.cursors

from httpsrv.handler import HandlerArgs, HandlerResult


def _get_db(config: dict):
    db = config.get('CimsDatabase', {})
    return pymysql.connect(
        host=db.get('Host', '127.0.0.1'),
        port=int(db.get('Port', 3306)),
        user=db.get('User', 'root'),
        password=db.get('Password', ''),
        database=db.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )


def _dt(val):
    return val.isoformat() if val else None


def _path_parts(full_path: str, base: str):
    path = urlparse(full_path).path
    try:
        rel = PurePath(path).relative_to(PurePath(base))
        return tuple(unquote(p) for p in rel.parts)
    except ValueError:
        return ()


# ──────────────────────────────────────────────────────────────
#  UDP 통신 헬퍼 (CSP/CMP stats 수집)
# ──────────────────────────────────────────────────────────────

def _udp_request(ip: str, port: int, data: dict, timeout: float = 1.0) -> dict:
    """UDP로 JSON 요청 보내고 응답 수신. timeout 단축(down 서버 fail-fast)."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(timeout)
        msg = json.dumps(data).encode('utf-8')
        sock.sendto(msg, (ip, port))
        resp_data, _ = sock.recvfrom(4096)
        sock.close()
        return json.loads(resp_data.decode('utf-8'))
    except Exception:
        return {}


# csp/cmp 상태 단기 캐시 — down 서버 probe 가 timeout 까지 블로킹하므로, 다중 위젯/스위퍼의
# 반복 요청이 매번 probe 하지 않도록 TTL 캐시. (정상 서버는 즉시 응답하므로 영향 미미.)
_STATS_CACHE: dict = {}
_STATS_TTL = 3.0


def _cached(key: str, producer):
    now = time.time()
    e = _STATS_CACHE.get(key)
    if e and now - e[0] < _STATS_TTL:
        return e[1]
    v = producer()
    _STATS_CACHE[key] = (now, v)
    return v


def _get_csp_stats(config: dict) -> dict:
    """CSP에 stats 요청 (3s 캐시)."""
    def probe():
        notify = config.get('CspNotify', {})
        ip = notify.get('Ip', '127.0.0.1')
        port = int(notify.get('Port', 4421))
        resp = _udp_request(ip, port, {"event": "STATS_REQUEST", "uri": "", "action": ""})
        return resp if resp.get('status') == 'OK' else {}
    return _cached('csp', probe)


def _service_log_dir(config: dict) -> str:
    """ServiceLogDir 를 config 에서 조회. csc_app.py 와 동일 로직."""
    sl = config.get('ServiceLogging', {})
    d = sl.get('Dir', '')
    if not d:
        d = config.get('ServiceLogDir', config.get('MsgLogDir', ''))
    return d


def _load_active_states(config: dict, kind: str) -> list:
    """{ServiceLogDir}/state/{kind}/*.json 을 읽어 가입자별 활성 상태 리스트 반환.
       CSP 가 원자 쓰기(.tmp+rename)로 관리하므로 부분 쓰기 읽음은 없음.
       .tmp 잔여 파일은 무시.
    """
    base = _service_log_dir(config)
    if not base:
        return []
    pattern = os.path.join(base, 'state', kind, '*.json')
    items = []
    for fpath in glob.glob(pattern):
        if fpath.endswith('.tmp'):
            continue
        try:
            with open(fpath, 'r') as f:
                items.append(json.loads(f.read()))
        except Exception:
            # 경합/파일 깨짐 등 — 조용히 skip
            pass
    return items


def _get_cmp_stats(config: dict) -> dict:
    """CMP에 stats 요청 (3s 캐시)."""
    def probe():
        cmp_ip = config.get('CmpIp', '127.0.0.1')
        cmp_port = int(config.get('CmpPort', 9000))
        resp = _udp_request(cmp_ip, cmp_port, {
            "trans_id": int(time.time()) % 100000,
            "payload": {"cmd": "STATS_REQUEST"}
        })
        if isinstance(resp.get('response'), dict):
            return resp['response']
        return {}
    return _cached('cmp', probe)


def _check_db_health(config: dict) -> bool:
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT 1")
        return True
    except Exception:
        return False


# ──────────────────────────────────────────────────────────────
#  Handlers
# ──────────────────────────────────────────────────────────────

_STATS_BASE = '/api/v1/stats'


async def handle_stats(handler_args: HandlerArgs, kwargs: dict) -> HandlerResult:
    config = kwargs.get('config', {})
    parsed = urlparse(handler_args.full_path)
    qs = parse_qs(parsed.query)
    parts = _path_parts(handler_args.full_path, _STATS_BASE)
    method = handler_args.method.upper()

    if method != 'GET':
        return HandlerResult(status=405, body={'error': 'Method Not Allowed'})

    def qp(name, default=None):
        vals = qs.get(name)
        return unquote(vals[0]) if vals else default

    try:
        if len(parts) == 0:
            return HandlerResult(status=200, body={'endpoints': [
                '/api/v1/stats/health', '/api/v1/stats/messages',
                '/api/v1/stats/service/voip', '/api/v1/stats/service/ptt',
                '/api/v1/stats/service/summary'
            ]})

        if parts[0] == 'health':
            return await _health(config)

        if parts[0] == 'subscribers':
            return await _subscribers_status(config)

        if parts[0] == 'messages':
            iface = parts[1] if len(parts) > 1 else None  # sip, cmp, csc, https
            date = qp('date')
            return await _messages_stats_v2(config, iface, date)

        if parts[0] == 'service':
            svc = parts[1] if len(parts) > 1 else 'summary'
            gran = qp('granularity', '1d')
            from_dt = qp('from')
            to_dt = qp('to')
            date = qp('date')
            return await _service_stats(config, svc, gran, from_dt, to_dt, date)

        return HandlerResult(status=404, body={'error': 'Not Found'})
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


# ──────────────────────────────────────────────────────────────
#  Health check (Part 1 대시보드 데이터)
# ──────────────────────────────────────────────────────────────

def _get_dashboard_counts(config: dict) -> dict:
    """대시보드 KPI 용 DB 카운트 (3s 캐시). 가입자/번호/등록/그룹.
    등록 = register_time NOT NULL AND (logout_time NULL OR register_time > logout_time)
    — csp 가 REGISTER 시 register_time, 로그아웃/만료 시 logout_time 을 갱신함."""
    def probe():
        try:
            with _get_db(config) as conn:
                with conn.cursor() as cur:
                    cur.execute("""
                        SELECT
                          (SELECT COUNT(*) FROM users)                                   AS subscribers_total,
                          (SELECT COUNT(*) FROM volte_subscriptions)                      AS volte_numbers,
                          (SELECT COUNT(*) FROM volte_subscriptions
                             WHERE register_time IS NOT NULL
                               AND (logout_time IS NULL OR register_time > logout_time))  AS volte_registered,
                          (SELECT COUNT(*) FROM ptt_subscriptions)                        AS ptt_numbers,
                          (SELECT COUNT(*) FROM ptt_subscriptions
                             WHERE register_time IS NOT NULL
                               AND (logout_time IS NULL OR register_time > logout_time))  AS ptt_registered,
                          (SELECT COUNT(*) FROM ptt_groups)                               AS ptt_groups_total
                    """)
                    row = cur.fetchone()
                    if not row:
                        return {}
                    # DictCursor / tuple 양쪽 대응
                    keys = ['subscribers_total', 'volte_numbers', 'volte_registered',
                            'ptt_numbers', 'ptt_registered', 'ptt_groups_total']
                    if isinstance(row, dict):
                        return {k: int(row.get(k) or 0) for k in keys}
                    return {k: int(row[i] or 0) for i, k in enumerate(keys)}
        except Exception:
            return {}
    return _cached('counts', probe)


async def _health(config: dict) -> HandlerResult:
    # csp/cmp UDP probe + DB 체크를 thread 로 병렬 — 이벤트 루프 비블로킹(down 서버 timeout 이
    # 다른 요청을 막지 않도록). 캐시(_cached)와 함께 /stats/health 지연 대폭 감소.
    csp, cmp, db_ok, counts = await asyncio.gather(
        asyncio.to_thread(_get_csp_stats, config),
        asyncio.to_thread(_get_cmp_stats, config),
        asyncio.to_thread(_check_db_health, config),
        asyncio.to_thread(_get_dashboard_counts, config),
    )

    result = {
        'health': {
            'csp': 'up' if csp else 'down',
            'cmp': 'up' if cmp else 'down',
            'db': 'up' if db_ok else 'down',
        },
        'csp': {
            'registered_users': csp.get('registered_users', 0),
            'active_calls': csp.get('active_calls', 0),
            'db_connected': csp.get('db_connected', False),
            'roles': csp.get('roles', {}),
            # 대시보드 KPI — DB 카운트 (가입자/번호/등록/그룹). probe 실패 시 0.
            'subscribers_total': counts.get('subscribers_total', 0),
            'volte_numbers':     counts.get('volte_numbers', 0),
            'volte_registered':  counts.get('volte_registered', 0),
            'ptt_numbers':       counts.get('ptt_numbers', 0),
            'ptt_registered':    counts.get('ptt_registered', 0),
            'ptt_groups_total':  counts.get('ptt_groups_total', 0),
        },
        'cmp': {
            'sessions': cmp.get('sessions', 0),
            'groups': cmp.get('groups', 0),
            # VoIP 풀 (하위호환: rtp_ports = VoIP)
            'rtp_ports': {
                'total': cmp.get('rtp_ports_total', 0),
                'used': cmp.get('rtp_ports_used', 0),
                'free': cmp.get('rtp_ports_free', 0),
            },
            # PTT(그룹통화) 전용 풀 — cmp STATS ptt_rtp_ports_* (구버전 cmp 면 0).
            'rtp_ports_ptt': {
                'total': cmp.get('ptt_rtp_ports_total', 0),
                'used': cmp.get('ptt_rtp_ports_used', 0),
                'free': cmp.get('ptt_rtp_ports_free', 0),
            },
        },
        'record_enable': csp.get('record_enable', False),
    }

    # v3 (2026-04-22): 가입자별 state 파일 기반 실시간 활성 통화 조회.
    #   CSP 가 {ServiceLogDir}/state/{volte,ptt}/{subscriber}.json 에 원자 쓰기로 관리.
    #   VoLTE: 한 통화당 caller+callee 2개 파일 → call_id 로 dedup 해서 통화 목록.
    #   PTT: 가입자별 참여 상태 → group_id 별 집계.
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')

    # VoLTE: call_id 기준 dedup (caller 쪽을 우선)
    voip_calls = {}
    for st in volte_states:
        cid = st.get('call_id', '')
        if not cid:
            continue
        entry = voip_calls.setdefault(cid, {
            'call_id': cid,
            'session_id': st.get('session_id', ''),
            'state': st.get('state'),
            'video': st.get('video', False),
            'invite_time': st.get('started_at'),
            'answered_at': st.get('answered_at'),
        })
        role = st.get('role', '')
        sub = st.get('subscriber_id', '')
        if role == 'caller':
            entry['initiator'] = sub
            entry['callee'] = st.get('peer_id', entry.get('callee', ''))
        elif role == 'callee':
            entry['callee'] = sub
            entry.setdefault('initiator', st.get('peer_id', ''))
    result['active_voip'] = sorted(voip_calls.values(), key=lambda x: x.get('invite_time') or '')

    # PTT: group_id 별 참여자 집계
    ptt_groups = {}
    for st in ptt_states:
        gid = st.get('group_id', '')
        if not gid:
            continue
        grp = ptt_groups.setdefault(gid, {
            'call_id': st.get('call_id', ''),
            'group_id': gid,
            'session_id': st.get('session_id', ''),
            'invite_time': st.get('started_at'),
            'state': st.get('state', 'active'),
            'members': [],
            'initiator': None,
        })
        sub = st.get('subscriber_id', '')
        role = st.get('role', 'member')
        if role == 'initiator':
            grp['initiator'] = sub
        grp['members'].append({'subscriber_id': sub, 'role': role})
    result['active_ptt'] = sorted(ptt_groups.values(), key=lambda x: x.get('invite_time') or '')

    # active_calls 보정: csp STATS 의 GetActiveVoipCallCount 는 현재 no-op(항상 0,
    # file-based CallDir 로 전환됨). state 파일 기반 실시간 집계가 SoT 이므로 그 수로 덮음
    # (VoLTE 통화 수 + PTT 활성 그룹 수). csp 값이 더 크면(미래 복구 대비) 큰 쪽 유지.
    file_active = len(result['active_voip']) + len(result['active_ptt'])
    result['csp']['active_calls'] = max(result['csp'].get('active_calls', 0), file_active)

    return HandlerResult(status=200, body=result)


# ──────────────────────────────────────────────────────────────
#  Message stats (Part 3.1)
# ──────────────────────────────────────────────────────────────

_SIP_REQUEST_METHODS = {
    'INVITE', 'ACK', 'BYE', 'CANCEL', 'OPTIONS', 'REGISTER', 'PRACK',
    'SUBSCRIBE', 'NOTIFY', 'PUBLISH', 'INFO', 'REFER', 'MESSAGE', 'UPDATE',
}


def _parse_msg_method(msg: str) -> str:
    """SIP/CMP 원문(msg)에서 통계용 키 추출.
    - 요청: 첫 줄 첫 토큰이 메서드 (INVITE/REGISTER/BYE/...).
    - 응답: 'SIP/2.0 200 OK' → 상태코드('200'/'401'/'180'/...).
    - CMP JSON: payload.cmd (HEARTBEAT/ADD/REMOVE/...).
    """
    if not msg:
        return 'unknown'
    first = msg.replace('\r', '\n').split('\n', 1)[0].strip()
    if not first:
        return 'unknown'
    # CMP/CSC JSON-over-UDP
    if first.startswith('{'):
        try:
            j = json.loads(msg)
            return (j.get('payload', {}) or {}).get('cmd', 'json') or 'json'
        except Exception:
            return 'json'
    tok = first.split()
    if first.startswith('SIP/2.0'):
        return tok[1] if len(tok) > 1 else 'response'   # status code
    method = tok[0].upper()
    return method if method in _SIP_REQUEST_METHODS else method


async def _messages_stats_v2(config, iface, date) -> HandlerResult:
    """service_log JSONL 기반 인터페이스별 메시지 통계.

    실제 레이아웃: {ServiceLogDir}/YYYY/MM/DD/HH/csp_01_{sip|cmp|csc}.msg.jsonl
    각 라인 = {ts,dir,peer,caller,callee,sesid,proto,msg}. method 는 msg 본문에서 파싱.
    (옛 MsgLogDir/{comp}/.../{iface}.jsonl 레이아웃 + entry['method'] 가정은 폐기됨.)
    """
    import glob as _glob

    if not date:
        date = datetime.now().strftime('%Y-%m-%d')
    d = date.replace('-', '')
    yyyy, mm, dd = d[:4], d[4:6], d[6:8]

    base = _service_log_dir(config)
    if not base:
        return HandlerResult(status=200, body={'date': date, 'interface': iface,
                                               'total': 0, 'buckets': [], 'method_counts': {}})

    ifaces = [iface] if iface in ('sip', 'cmp', 'csc') else ['sip', 'cmp', 'csc']
    patterns = [os.path.join(base, yyyy, mm, dd, '*', f'csp_01_{ifc}.msg.jsonl') for ifc in ifaces]

    hourly = {}         # hour → count
    method_counts = {}  # method/status → count

    for pattern in patterns:
        for fpath in _glob.glob(pattern):
            try:
                with open(fpath, 'r') as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            entry = json.loads(line)
                            ts = entry.get('ts', '')
                            hour = int(ts.split(':')[0]) if ':' in ts else 0
                            method = _parse_msg_method(entry.get('msg', ''))
                            hourly[hour] = hourly.get(hour, 0) + 1
                            method_counts[method] = method_counts.get(method, 0) + 1
                        except Exception:
                            pass
            except Exception:
                pass

    buckets = [{'hour': h, 'count': hourly.get(h, 0)} for h in range(24)]
    sorted_methods = dict(sorted(method_counts.items(), key=lambda x: -x[1]))

    return HandlerResult(status=200, body={
        'date': date,
        'interface': iface,
        'total': sum(hourly.values()),
        'buckets': buckets,
        'method_counts': sorted_methods,
    })


# ──────────────────────────────────────────────────────────────
#  Service stats (Part 3.2) — 파일 기반 (call.json / call.jsonl 스캔)
#
#  v3 (2026-04-22) 이후 call_logs DB 테이블 DROP. service_log/{volte|ptt}/
#  YYYY/MM/DD/HH/.../*.d/call.json 이 SoT. 옛 _messages_stats / DB 기반
#  _calc_*_stats 는 모두 제거됨 (msg_log JSONL 기반 _messages_stats_v2 가
#  /api/v1/stats/messages 를 처리).
# ──────────────────────────────────────────────────────────────

def _iter_call_jsons(config: dict, call_type: str, from_dt: str, to_dt: str):
    """[from_dt, to_dt] 범위 내 .d/call.json (volte) 또는 .d/call.jsonl (ptt) 파싱 결과 yield.

    날짜 단위로만 디렉토리 스캔 → from/to 의 분/초는 결과 객체 ts 비교로 필터.
    """
    base = _service_log_dir(config)
    if not base:
        return
    try:
        f_day = datetime.strptime(from_dt[:10], '%Y-%m-%d').date()
        t_day = datetime.strptime(to_dt[:10], '%Y-%m-%d').date()
    except Exception:
        return
    day = f_day
    while day <= t_day:
        yyyy = f"{day.year:04d}"
        mm = f"{day.month:02d}"
        dd = f"{day.day:02d}"
        date_base = os.path.join(base, call_type, yyyy, mm, dd)
        if os.path.isdir(date_base):
            for cj_path in glob.glob(os.path.join(date_base, '**', '*.d', 'call.json'), recursive=True):
                try:
                    with open(cj_path, 'r', encoding='utf-8') as f:
                        yield json.load(f)
                except Exception:
                    continue
            # PTT 는 call.jsonl (세션별 누적) — 마지막 세션만 사용
            for cjl_path in glob.glob(os.path.join(date_base, '**', '*.d', 'call.jsonl'), recursive=True):
                try:
                    last = None
                    with open(cjl_path, 'r', encoding='utf-8') as f:
                        for line in f:
                            line = line.strip()
                            if not line:
                                continue
                            try:
                                last = json.loads(line)
                            except Exception:
                                continue
                    if last:
                        yield last
                except Exception:
                    continue
        day += timedelta(days=1)


def _ts_of(record: dict, call_type: str) -> str:
    """call_type 별 시작 시각 필드 추출 (volte=invite_time, ptt=start_time).

    call.json 은 ISO 'T' 구분자("2026-06-01T15:52:37"), from/to 파라미터는 공백
    구분자("2026-06-01 00:00:00"). 문자열 비교 시 'T'(0x54) > ' '(0x20) 라 전 레코드가
    to_dt 초과로 배제되는 버그가 있었음 → 여기서 ' ' 로 정규화해 비교/버킷 모두 일치시킴.
    """
    if call_type == 'ptt':
        ts = record.get('start_time', '') or record.get('invite_time', '')
    else:
        ts = record.get('invite_time', '')
    return ts.replace('T', ' ', 1) if ts else ts


def _bucket_key(ts: str, gran: str) -> str:
    """gran 별 버킷 키 — '시간(0-23)' 또는 'YYYY-MM-DD'."""
    if not ts:
        return ''
    if gran in ('5m', '10m', '1h'):
        # HH 추출 (ISO ts 의 11..13 또는 'T' 다음 두 자리)
        try:
            return str(int(ts[11:13]))
        except Exception:
            return ''
    return ts[:10]


async def _service_stats(config, svc, gran, from_dt, to_dt, date) -> HandlerResult:
    if not from_dt:
        if date:
            from_dt = date + ' 00:00:00'
            to_dt = date + ' 23:59:59'
        else:
            today = datetime.now().strftime('%Y-%m-%d')
            from_dt = today + ' 00:00:00'
            to_dt = today + ' 23:59:59'
    elif not to_dt:
        to_dt = from_dt

    try:
        voip = _calc_voip_stats(config, from_dt, to_dt, gran) if svc in ('volte', 'voip', 'summary') else None
        ptt = _calc_ptt_stats(config, from_dt, to_dt, gran) if svc in ('ptt', 'summary') else None

        result = {'granularity': gran, 'from': from_dt, 'to': to_dt}
        if voip:
            result['volte'] = voip
        if ptt:
            result['ptt'] = ptt

        return HandlerResult(status=200, body=result)
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _calc_voip_stats(config, from_dt, to_dt, gran):
    total = 0
    success = 0
    durations = []
    end_reasons: dict = {}
    by_bucket_total: dict = {}  # bucket_key -> count
    by_bucket_ok: dict = {}     # bucket_key -> count

    for rec in _iter_call_jsons(config, 'volte', from_dt, to_dt):
        ts = _ts_of(rec, 'volte')
        if not ts or ts < from_dt or ts > to_dt:
            continue
        total += 1
        state = rec.get('state', '')
        reason = rec.get('end_reason') or 'unknown'
        dur = int(rec.get('duration', 0) or 0)
        is_success = (state == 'ended' and reason == 'normal')
        if is_success:
            success += 1
        if dur > 0:
            durations.append(dur)
        if state == 'ended':
            end_reasons[reason] = end_reasons.get(reason, 0) + 1
        bk = _bucket_key(ts, gran)
        if bk:
            by_bucket_total[bk] = by_bucket_total.get(bk, 0) + 1
            if is_success:
                by_bucket_ok[bk] = by_bucket_ok.get(bk, 0) + 1

    if gran in ('5m', '10m', '1h'):
        keys = sorted(by_bucket_total.keys(), key=lambda k: int(k))
        buckets = []
        for k in keys:
            cnt = by_bucket_total[k]
            ok = by_bucket_ok.get(k, 0)
            buckets.append({'hour': int(k), 'attempts': cnt, 'success': ok,
                            'success_rate': round(ok / cnt * 100, 1) if cnt > 0 else 0})
    else:
        keys = sorted(by_bucket_total.keys())
        buckets = []
        for k in keys:
            cnt = by_bucket_total[k]
            ok = by_bucket_ok.get(k, 0)
            buckets.append({'date': k, 'attempts': cnt, 'success': ok,
                            'success_rate': round(ok / cnt * 100, 1) if cnt > 0 else 0})

    avg_dur = round(sum(durations) / len(durations), 1) if durations else 0
    return {
        'total_attempts': total,
        'total_success': success,
        'success_rate': round(success / total * 100, 1) if total > 0 else 0,
        'avg_duration_sec': avg_dur,
        'end_reasons': end_reasons,
        'buckets': buckets,
    }


def _calc_ptt_stats(config, from_dt, to_dt, gran):
    total = 0
    durations = []
    by_group: dict = {}
    by_bucket: dict = {}

    for rec in _iter_call_jsons(config, 'ptt', from_dt, to_dt):
        ts = _ts_of(rec, 'ptt')
        if not ts or ts < from_dt or ts > to_dt:
            continue
        total += 1
        gid = rec.get('group_id', '') or 'unknown'
        by_group[gid] = by_group.get(gid, 0) + 1
        dur = int(rec.get('duration', 0) or 0)
        if dur > 0:
            durations.append(dur)
        bk = _bucket_key(ts, gran)
        if bk:
            by_bucket[bk] = by_bucket.get(bk, 0) + 1

    if gran in ('5m', '10m', '1h'):
        buckets = [{'hour': int(k), 'calls': by_bucket[k]}
                   for k in sorted(by_bucket.keys(), key=lambda k: int(k))]
    else:
        buckets = [{'date': k, 'calls': by_bucket[k]} for k in sorted(by_bucket.keys())]

    by_group_sorted = dict(sorted(by_group.items(), key=lambda x: -x[1]))
    avg_dur = round(sum(durations) / len(durations), 1) if durations else 0
    return {
        'total_calls': total,
        'avg_duration_sec': avg_dur,
        'by_group': by_group_sorted,
        'buckets': buckets,
    }


# ──────────────────────────────────────────────────────────────
#  Subscribers real-time status (가입자별 실시간 접속/통화 상태)
# ──────────────────────────────────────────────────────────────

async def _subscribers_status(config: dict) -> HandlerResult:
    """모든 가입자의 VoLTE/PTT 접속 상태 + 실시간 통화 상태 반환.
       v3: call_logs 테이블 DROP 후 state 파일이 SOT — CSP 가
       {ServiceLogDir}/state/{volte|ptt}/{subscriber}.json 에 원자 쓰기로 관리한다."""

    # 1) state 파일에서 active 통화 인덱스 구축
    volte_states = _load_active_states(config, 'volte')
    ptt_states = _load_active_states(config, 'ptt')

    voip_active_by_sub = {}   # subscriber_id → [{call_id, peer, role, state, invite_time}]
    for st in volte_states:
        sub = st.get('subscriber_id', '')
        if not sub:
            continue
        voip_active_by_sub.setdefault(sub, []).append({
            'call_id': st.get('call_id', ''),
            'peer': st.get('peer_id', ''),
            'role': st.get('role', ''),
            'state': st.get('state', ''),
            'invite_time': st.get('started_at'),
            'answered_at': st.get('answered_at'),
            'video': st.get('video', False),
        })

    # PTT: 그룹별 참여자 집계 (active_members = state 파일 기반)
    group_active_members = {}  # group_id → count
    for st in ptt_states:
        gid = st.get('group_id', '')
        if gid:
            group_active_members[gid] = group_active_members.get(gid, 0) + 1

    # CMP 에서 floor holder 조회 시도 (실패해도 무관)
    group_floor_holder = {}
    try:
        cmp_stats = _get_cmp_stats(config)
        for gd in (cmp_stats or {}).get('group_details', []) or []:
            gid = gd.get('group_id', '')
            if gid and gd.get('floor_holder'):
                group_floor_holder[gid] = gd['floor_holder']
    except Exception:
        pass

    # 2) DB 에서 가입자 목록 + ptt_group_members total count 조회
    subscribers = []
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT u.id AS person_id, u.name, "
                    "vs.id AS voip_id, vs.imsi AS voip_imsi, vs.service_ref AS voip_service_ref, "
                    "vs.register_time AS voip_reg_time, vs.logout_time AS voip_logout_time, "
                    "ps.id AS ptt_id, ps.imsi AS ptt_imsi, ps.service_ref AS ptt_service_ref, "
                    "ps.register_time AS ptt_reg_time, ps.logout_time AS ptt_logout_time "
                    "FROM users u "
                    "LEFT JOIN volte_subscriptions vs ON vs.user_id = u.id "
                    "LEFT JOIN ptt_subscriptions ps ON ps.user_id = u.id "
                    "ORDER BY u.name"
                )
                rows = cur.fetchall()

                # ptt_group_members 의 그룹별 총 멤버 수 캐시
                group_total = {}
                if group_active_members:
                    placeholders = ','.join(['%s'] * len(group_active_members))
                    cur.execute(
                        f"SELECT group_id, COUNT(*) AS cnt FROM ptt_group_members "
                        f"WHERE group_id IN ({placeholders}) GROUP BY group_id",
                        tuple(group_active_members.keys())
                    )
                    for r in cur.fetchall():
                        group_total[r['group_id']] = r['cnt']

                # 가입자별 PTT 참여 레코드 구축 (state 파일로부터)
                ptt_active_by_sub = {}
                for st in ptt_states:
                    sub = st.get('subscriber_id', '')
                    gid = st.get('group_id', '')
                    if not sub:
                        continue
                    ptt_active_by_sub.setdefault(sub, []).append({
                        'call_id': st.get('call_id', ''),
                        'group_id': gid,
                        'state': st.get('state', 'active'),
                        'role': st.get('role', 'member'),
                        'invite_time': st.get('started_at'),
                        'total_members': group_total.get(gid, 0),
                        'active_members': group_active_members.get(gid, 0),
                        'floor_holder': group_floor_holder.get(gid),
                    })

                for row in rows:
                    voip_id = row.get('voip_id')
                    ptt_id = row.get('ptt_id')

                    voip_online = False
                    if voip_id and row.get('voip_reg_time'):
                        if not row.get('voip_logout_time') or row['voip_reg_time'] > row['voip_logout_time']:
                            voip_online = True

                    ptt_online = False
                    if ptt_id and row.get('ptt_reg_time'):
                        if not row.get('ptt_logout_time') or row['ptt_reg_time'] > row['ptt_logout_time']:
                            ptt_online = True

                    sub = {
                        'person_id': row['person_id'],
                        'name': row['name'],
                        'volte': None,
                        'ptt': None,
                    }

                    if voip_id:
                        sub['volte'] = {
                            'msisdn': voip_id,
                            'online': voip_online,
                            'register_time': _dt(row.get('voip_reg_time')),
                            'calls': voip_active_by_sub.get(voip_id, []),
                        }

                    if ptt_id:
                        sub['ptt'] = {
                            'msisdn': ptt_id,
                            'online': ptt_online,
                            'register_time': _dt(row.get('ptt_reg_time')),
                            'groups': ptt_active_by_sub.get(ptt_id, []),
                        }

                    subscribers.append(sub)

    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})

    return HandlerResult(status=200, body={
        'total': len(subscribers),
        'subscribers': subscribers,
    })


CIMS_STATS_HANDLER_LIST = [
    (_STATS_BASE, handle_stats, {}),
]
