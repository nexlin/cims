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

def _udp_request(ip: str, port: int, data: dict, timeout: float = 2.0) -> dict:
    """UDP로 JSON 요청 보내고 응답 수신"""
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


def _get_csp_stats(config: dict) -> dict:
    """CSP에 stats 요청"""
    notify = config.get('CspNotify', {})
    ip = notify.get('Ip', '127.0.0.1')
    port = int(notify.get('Port', 4421))
    resp = _udp_request(ip, port, {"event": "STATS_REQUEST", "uri": "", "action": ""})
    return resp if resp.get('status') == 'OK' else {}


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
    """CMP에 stats 요청"""
    # CMP 주소는 CSP config의 RtpRelay에서 가져오거나 직접 설정
    cmp_ip = config.get('CmpIp', '127.0.0.1')
    cmp_port = int(config.get('CmpPort', 9000))
    resp = _udp_request(cmp_ip, cmp_port, {
        "trans_id": int(time.time()) % 100000,
        "payload": {"cmd": "STATS_REQUEST"}
    })
    if isinstance(resp.get('response'), dict):
        return resp['response']
    return {}


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

async def _health(config: dict) -> HandlerResult:
    csp = _get_csp_stats(config)
    cmp = _get_cmp_stats(config)

    # DB 체크
    db_ok = False
    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT 1")
                db_ok = True
    except Exception:
        pass

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
        },
        'cmp': {
            'sessions': cmp.get('sessions', 0),
            'groups': cmp.get('groups', 0),
            'rtp_ports': {
                'total': cmp.get('rtp_ports_total', 0),
                'used': cmp.get('rtp_ports_used', 0),
                'free': cmp.get('rtp_ports_free', 0),
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

    return HandlerResult(status=200, body=result)


# ──────────────────────────────────────────────────────────────
#  Message stats (Part 3.1)
# ──────────────────────────────────────────────────────────────

async def _messages_stats_v2(config, iface, date) -> HandlerResult:
    """msg_log JSONL 기반 인터페이스별 메시지 통계"""
    import glob as _glob

    if not date:
        date = datetime.now().strftime('%Y-%m-%d')
    d = date.replace('-', '')
    yyyy, mm, dd = d[:4], d[4:6], d[6:8]

    msg_log_dir = config.get('MsgLogDir', '')
    if not msg_log_dir:
        return HandlerResult(status=200, body={'date': date, 'interface': iface, 'buckets': [], 'method_counts': {}})

    # 인터페이스 → 파일 매핑
    iface_map = {
        'sip': ('csp', 'sip.jsonl'),
        'cmp': ('csp', 'cmp.jsonl'),
        'csc': ('csp', 'csc.jsonl'),
        'https': ('csc', 'mcptt.jsonl'),
    }

    if iface and iface in iface_map:
        comp, fname = iface_map[iface]
        patterns = [os.path.join(msg_log_dir, comp, yyyy, mm, dd, '*', fname)]
    else:
        # 전체: 모든 인터페이스
        patterns = [os.path.join(msg_log_dir, comp, yyyy, mm, dd, '*', fn) for comp, fn in iface_map.values()]

    # JSONL 파싱 → 시간대별 + 메서드별 집계
    hourly = {}  # hour → count
    method_counts = {}  # method → count

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
                            method = entry.get('method', 'unknown')

                            hourly[hour] = hourly.get(hour, 0) + 1
                            method_counts[method] = method_counts.get(method, 0) + 1
                        except:
                            pass
            except:
                pass

    buckets = [{'hour': h, 'count': hourly.get(h, 0)} for h in range(24)]
    # method_counts를 카운트 내림차순 정렬
    sorted_methods = dict(sorted(method_counts.items(), key=lambda x: -x[1]))

    return HandlerResult(status=200, body={
        'date': date,
        'interface': iface,
        'total': sum(hourly.values()),
        'buckets': buckets,
        'method_counts': sorted_methods,
    })


async def _messages_stats(config, gran, from_dt, to_dt, proto, date) -> HandlerResult:
    # 레거시: DB 기반 (하위 호환)
    # 현재는 DB call_logs 기반 간이 구현 (향후 jsonl 파싱 배치로 확장)
    if not date:
        date = datetime.now().strftime('%Y-%m-%d')

    try:
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                # SIP 메시지 통계 간이 구현: INVITE 수를 시간대별 집계
                cur.execute(
                    "SELECT HOUR(invite_time) AS h, COUNT(*) AS cnt "
                    "FROM volte_call_logs WHERE DATE(invite_time) = %s "
                    "GROUP BY HOUR(invite_time) ORDER BY h",
                    (date,)
                )
                voip_hourly = {int(r['h']): int(r['cnt']) for r in cur.fetchall()}

                cur.execute(
                    "SELECT HOUR(invite_time) AS h, COUNT(*) AS cnt "
                    "FROM ptt_call_logs WHERE DATE(invite_time) = %s "
                    "GROUP BY HOUR(invite_time) ORDER BY h",
                    (date,)
                )
                ptt_hourly = {int(r['h']): int(r['cnt']) for r in cur.fetchall()}

        buckets = []
        for h in range(24):
            buckets.append({
                'hour': h,
                'voip_invite': voip_hourly.get(h, 0),
                'ptt_invite': ptt_hourly.get(h, 0),
                'total': voip_hourly.get(h, 0) + ptt_hourly.get(h, 0),
            })

        return HandlerResult(status=200, body={
            'date': date, 'granularity': gran, 'buckets': buckets
        })
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


# ──────────────────────────────────────────────────────────────
#  Service stats (Part 3.2)
# ──────────────────────────────────────────────────────────────

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
        with _get_db(config) as conn:
            with conn.cursor() as cur:
                if svc in ('volte', 'summary'):
                    voip = _calc_voip_stats(cur, from_dt, to_dt, gran)
                else:
                    voip = None

                if svc in ('ptt', 'summary'):
                    ptt = _calc_ptt_stats(cur, from_dt, to_dt, gran)
                else:
                    ptt = None

        result = {'granularity': gran, 'from': from_dt, 'to': to_dt}
        if voip:
            result['volte'] = voip
        if ptt:
            result['ptt'] = ptt

        return HandlerResult(status=200, body=result)
    except Exception as e:
        return HandlerResult(status=500, body={'error': str(e)})


def _calc_voip_stats(cur, from_dt, to_dt, gran):
    # 총 호 시도
    cur.execute(
        "SELECT COUNT(*) AS total, "
        "SUM(CASE WHEN state='ended' AND sip_status=200 THEN 1 ELSE 0 END) AS success, "
        "AVG(CASE WHEN duration > 0 THEN duration ELSE NULL END) AS avg_duration, "
        "MAX(duration) AS max_duration "
        "FROM volte_call_logs WHERE invite_time BETWEEN %s AND %s",
        (from_dt, to_dt)
    )
    row = cur.fetchone()
    total = int(row['total'] or 0)
    success = int(row['success'] or 0)
    avg_dur = round(float(row['avg_duration'] or 0), 1)

    # 실패 사유 분포
    cur.execute(
        "SELECT end_reason, COUNT(*) AS cnt FROM volte_call_logs "
        "WHERE invite_time BETWEEN %s AND %s AND state='ended' "
        "GROUP BY end_reason",
        (from_dt, to_dt)
    )
    end_reasons = {r['end_reason'] or 'unknown': int(r['cnt']) for r in cur.fetchall()}

    # 시간대별 (gran에 따라)
    if gran in ('5m', '10m', '1h'):
        cur.execute(
            "SELECT HOUR(invite_time) AS h, COUNT(*) AS cnt, "
            "SUM(CASE WHEN sip_status=200 THEN 1 ELSE 0 END) AS ok "
            "FROM volte_call_logs WHERE invite_time BETWEEN %s AND %s "
            "GROUP BY HOUR(invite_time) ORDER BY h",
            (from_dt, to_dt)
        )
        buckets = []
        for r in cur.fetchall():
            cnt = int(r['cnt'] or 0)
            ok = int(r['ok'] or 0)
            rate = round(ok / cnt * 100, 1) if cnt > 0 else 0
            buckets.append({'hour': int(r['h']), 'attempts': cnt, 'success': ok, 'success_rate': rate})
    else:
        cur.execute(
            "SELECT DATE(invite_time) AS d, COUNT(*) AS cnt, "
            "SUM(CASE WHEN sip_status=200 THEN 1 ELSE 0 END) AS ok "
            "FROM volte_call_logs WHERE invite_time BETWEEN %s AND %s "
            "GROUP BY DATE(invite_time) ORDER BY d",
            (from_dt, to_dt)
        )
        buckets = []
        for r in cur.fetchall():
            cnt = int(r['cnt'] or 0)
            ok = int(r['ok'] or 0)
            rate = round(ok / cnt * 100, 1) if cnt > 0 else 0
            buckets.append({'date': str(r['d']), 'attempts': cnt, 'success': ok, 'success_rate': rate})

    return {
        'total_attempts': total,
        'total_success': success,
        'success_rate': round(success / total * 100, 1) if total > 0 else 0,
        'avg_duration_sec': avg_dur,
        'end_reasons': end_reasons,
        'buckets': buckets,
    }


def _calc_ptt_stats(cur, from_dt, to_dt, gran):
    cur.execute(
        "SELECT COUNT(*) AS total, "
        "AVG(CASE WHEN duration > 0 THEN duration ELSE NULL END) AS avg_duration "
        "FROM ptt_call_logs WHERE invite_time BETWEEN %s AND %s",
        (from_dt, to_dt)
    )
    row = cur.fetchone()
    total = int(row['total'] or 0)
    avg_dur = round(float(row['avg_duration'] or 0), 1)

    # 그룹별
    cur.execute(
        "SELECT group_id, COUNT(*) AS cnt FROM ptt_call_logs "
        "WHERE invite_time BETWEEN %s AND %s GROUP BY group_id ORDER BY cnt DESC",
        (from_dt, to_dt)
    )
    by_group = {r['group_id']: int(r['cnt']) for r in cur.fetchall()}

    # 시간대별
    if gran in ('5m', '10m', '1h'):
        cur.execute(
            "SELECT HOUR(invite_time) AS h, COUNT(*) AS cnt "
            "FROM ptt_call_logs WHERE invite_time BETWEEN %s AND %s "
            "GROUP BY HOUR(invite_time) ORDER BY h",
            (from_dt, to_dt)
        )
        buckets = [{'hour': int(r['h']), 'calls': int(r['cnt'])} for r in cur.fetchall()]
    else:
        cur.execute(
            "SELECT DATE(invite_time) AS d, COUNT(*) AS cnt "
            "FROM ptt_call_logs WHERE invite_time BETWEEN %s AND %s "
            "GROUP BY DATE(invite_time) ORDER BY d",
            (from_dt, to_dt)
        )
        buckets = [{'date': str(r['d']), 'calls': int(r['cnt'])} for r in cur.fetchall()]

    return {
        'total_calls': total,
        'avg_duration_sec': avg_dur,
        'by_group': by_group,
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
