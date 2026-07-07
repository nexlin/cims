"""알람 sweeper 공용 코어 (알람 표준화 X.733/32.111 — alarm_standardization.md).

소유권(oam_base_service_split §4): 서비스 계열 규칙(csp_down/cmp_down/db_down/rtp_high,
scope != 'agent')의 평가·발화는 **oam-svc** 소유 — CSP/CMP probe·DB 접속이 서비스 관측
설정(oam-svc config)과 함께 움직인다. 단일 프로세스(--role all)에서는 oam_app 이 같은
코어를 호출해 동작 무변경. agent 계열(disk_high/module_down) 평가는 base(oam_app) 잔류 —
transition/emit 코어를 여기서 함께 쓴다.

open-state: state = { akey("code@mo_instance"): alarm_id } — 호출 프로세스가 보유.
서비스 계열의 mo_instance 는 'cims/<target>' 접두 — restore_open_state 의 scope 분리 기준.
"""

import time


class _Safe(dict):
    def __missing__(self, k):  # 템플릿에 없는 키는 빈 문자열 (KeyError 방지)
        return ''


def fmt(tmpl: str, **kw) -> str:
    return (tmpl or '').format_map(_Safe(kw))


def _is_service_akey(akey: str) -> bool:
    mo = akey.split('@', 1)[1] if '@' in akey else ''
    return mo.startswith('cims/')


def restore_open_state(service_log_dir: str, scope: str = 'all', days: int = 30,
                       log=None) -> dict:
    """기동 시 활성 알람 복원. scope: 'service'=cims/* 만, 'agent'=cims/* 제외, 'all'=전부.
    분리 배포에서 base/oam-svc 가 각자 소유 계열만 추적하도록 나눈다."""
    from services import alert_log
    state: dict = {}
    if not service_log_dir:
        return state
    try:
        restored = alert_log.compute_open_state(service_log_dir, days=days)
        if scope == 'service':
            restored = {k: v for k, v in restored.items() if _is_service_akey(k)}
        elif scope == 'agent':
            restored = {k: v for k, v in restored.items() if not _is_service_akey(k)}
        state.update(restored)
        if restored and log:
            log.log_info(f"[alarm] restored open state ({scope}): {sorted(restored.keys())}")
    except Exception as e:
        if log:
            log.log_error(f"[alarm] restore failed: {e}")
    return state


def emit_alarm(service_log_dir: str, action: str, rule: dict, mo_instance: str,
               detected_by: str, message: str, alarm_id: str):
    """표준 알람 이벤트 기록 (code/severity/event_type/probable_cause/source/alarm_id)."""
    from datetime import datetime as _dt
    from services import alert_log
    sev = 'cleared' if action == 'close' else rule.get('perceived_severity', 'warning')
    rec = {
        'ts': _dt.now().isoformat(timespec='seconds'),
        'alarm_id': alarm_id,
        'type': rule.get('type'), 'code': rule.get('code'),
        'perceived_severity': sev, 'severity': sev,   # 'severity' 구 reader 호환
        'event_type': rule.get('event_type'), 'probable_cause': rule.get('probable_cause'),
        'source': {'mo_class': rule.get('mo_class'), 'mo_instance': mo_instance,
                   'detected_by': detected_by},
        'action': action, 'message': message,
    }
    if rule.get('effect'):
        rec['effect'] = rule['effect']
    if rule.get('recommended_action'):
        rec['recommended_action'] = rule['recommended_action']
    alert_log.record_event(service_log_dir, rec)


def transition(state: dict, service_log_dir: str, rule: dict, mo_instance: str,
               detected_by: str, is_open: bool, msg_open: str, msg_close: str, log=None):
    """활성식별 akey=(code@mo_instance). open 시 alarm_id 생성, close 가 동일 alarm_id 참조."""
    akey = f"{rule.get('code')}@{mo_instance}"
    was = akey in state
    if is_open and not was:
        alarm_id = f"{akey}@{int(time.time())}"
        state[akey] = alarm_id
        emit_alarm(service_log_dir, 'open', rule, mo_instance, detected_by, msg_open, alarm_id)
        if log:
            log.log_info(f"[alarm] OPEN {akey} sev={rule.get('perceived_severity')} — {msg_open}")
    elif not is_open and was:
        alarm_id = state.pop(akey)
        emit_alarm(service_log_dir, 'close', rule, mo_instance, detected_by, msg_close, alarm_id)
        if log:
            log.log_info(f"[alarm] CLEAR {akey}")


def eval_service_rule(rule: dict, ctx: dict, rtp_threshold: int = 80) -> bool:
    chk = rule.get('check')
    if chk == 'process_down':
        return not bool(ctx.get(rule.get('target')))
    if chk == 'db_down':
        return not ctx.get('db_ok')
    if chk == 'rtp_pct_gte':
        return ctx.get('rtp_pct', 0) >= int(rule.get('threshold', rtp_threshold))
    return False


def sweep_service_rules(config: dict, state: dict, service_log_dir: str,
                        detected_by: str = 'oam-svc', rtp_threshold: int = 80, log=None):
    """서비스 계열 규칙 1회 평가 — CSP/CMP UDP probe + DB 체크 + RTP 사용률.
    probe 헬퍼는 handlers.stats 공유(3s 캐시). 규칙은 service_registry(코어 + descriptor).

    CMP 는 전 미디어 노드 개별 평가(AA 다중 노드) — process_down(target=cmp)은
    endpoint 마다 mo_instance='cims/cmp/<ip>:<port>' 로 발화해 어느 노드가 죽었는지
    식별한다. RTP 사용률은 전 노드 합산. MediaServer.Endpoints/CmpIp 미설정이면
    CMP 관측 비활성(cmp 계열 규칙 skip)."""
    from services import service_registry
    from handlers.stats import (_get_csp_stats, _get_db,
                                _media_endpoints, _probe_cmp)
    csp = _get_csp_stats(config)
    cmp_configured = bool(((config.get('MediaServer') or {}).get('Endpoints'))
                          or config.get('CmpIp'))
    cmp_nodes = []          # [(mo_suffix, stats dict)]
    if cmp_configured:
        cmp_nodes = [(f"{ip}:{port}", _probe_cmp(ip, port))
                     for ip, port in _media_endpoints(config)]
    try:
        conn = _get_db(config)
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT 1")
            db_ok = True
        finally:
            conn.close()
    except Exception:
        db_ok = False
    total = sum((s.get('rtp_ports_total', 0) or 0) for _, s in cmp_nodes)
    used = sum((s.get('rtp_ports_used', 0) or 0) for _, s in cmp_nodes)
    pct = int(round(used / total * 100)) if total > 0 else 0
    ctx = {'csp': csp, 'db_ok': db_ok, 'rtp_pct': pct}
    rules = [r for r in service_registry.alert_rules(config) if r.get('scope') != 'agent']
    for r in rules:
        thr = r.get('threshold', rtp_threshold)
        if r.get('check') == 'process_down' and r.get('target') == 'cmp':
            # 노드별 개별 알람 — endpoint 목록이 바뀌면 사라진 노드의 open 알람은
            # 남을 수 있으나(평가 대상 이탈) 재기동 시 restore 후 대상 재구성으로 수렴.
            base_mo = r.get('mo_instance') or 'cims/cmp'
            for suffix, stats in cmp_nodes:
                mo = f"{base_mo}/{suffix}"
                transition(state, service_log_dir, r, mo, detected_by,
                           not bool(stats),
                           fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr),
                           fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr), log=log)
            # 구 집계 인스턴스(cims/cmp)로 열려 복원된 알람은 노드별 전환 후 평가
            # 대상이 없어 영영 안 닫히므로 여기서 close.
            if f"{r.get('code')}@{base_mo}" in state:
                transition(state, service_log_dir, r, base_mo, detected_by, False, '',
                           fmt(r.get('msg_close'), mo=base_mo, pct=pct, threshold=thr),
                           log=log)
            continue
        if r.get('check') == 'rtp_pct_gte' and not cmp_nodes:
            continue    # CMP 관측 비활성 — 사용률 평가 불가
        mo = r.get('mo_instance') or f"cims/{r.get('target', '')}"
        msg_open = fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr)
        msg_close = fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr)
        transition(state, service_log_dir, r, mo, detected_by,
                   eval_service_rule(r, ctx, rtp_threshold), msg_open, msg_close, log=log)
