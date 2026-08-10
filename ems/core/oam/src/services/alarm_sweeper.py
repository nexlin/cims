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

# X.733 perceived severity 서열 (trend_indication 판정용 — 콘솔 SEV_RANK 와 동일 서열)
_SEV_ORDER = ['indeterminate', 'warning', 'minor', 'major', 'critical']


def _sev_rank(s: str) -> int:
    return _SEV_ORDER.index(s) if s in _SEV_ORDER else 0


def _entry_alarm_id(v):
    """open-state 값에서 alarm_id — 신형 {'alarm_id','severity'} / 구형 str 둘 다 허용."""
    return v.get('alarm_id') if isinstance(v, dict) else v


def _entry_severity(v):
    return v.get('severity') if isinstance(v, dict) else None


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
    """기동 시 활성 알람 복원. scope: 'service'=cims/* 만, 'agent'=cims/* 제외,
    'self'=모듈 자기보고(detected_by self:*)만, 'all'=자기보고 제외 전부.
    분리 배포에서 base/oam-svc/FM ingest 가 각자 소유 계열만 추적하도록 나눈다 —
    자기보고 계열은 mo 접두가 아니라 발화 주체(detected_by)로 구분한다
    (alarm_self_reporting.md §5)."""
    from services import alert_log
    state: dict = {}
    if not service_log_dir:
        return state
    try:
        meta = alert_log.compute_open_state(service_log_dir, days=days, with_meta=True)
        def _is_self(m):
            return (m.get('detected_by') or '').startswith('self:')
        if scope == 'self':
            meta = {k: m for k, m in meta.items() if _is_self(m)}
        else:
            meta = {k: m for k, m in meta.items() if not _is_self(m)}
            if scope == 'service':
                meta = {k: m for k, m in meta.items() if _is_service_akey(k)}
            elif scope == 'agent':
                meta = {k: m for k, m in meta.items() if not _is_service_akey(k)}
        restored = {k: {'alarm_id': m['alarm_id'], 'severity': m.get('perceived_severity')}
                    for k, m in meta.items()}
        state.update(restored)
        if restored and log:
            log.log_info(f"[alarm] restored open state ({scope}): {sorted(restored.keys())}")
    except Exception as e:
        if log:
            log.log_error(f"[alarm] restore failed: {e}")
    return state


def emit_alarm(service_log_dir: str, action: str, rule: dict, mo_instance: str,
               detected_by: str, message: str, alarm_id: str,
               threshold_info: dict = None, trend_indication: str = None):
    """표준 알람 이벤트 기록 (code/severity/event_type/probable_cause/source/alarm_id).

    32.111 alarmRaisedTime/alarmClearedTime/alarmChangedTime: open 은 raised_time,
    close 는 clear_time, change 는 change_time — close/change 는 raised_time(alarm_id
    occurrence epoch 복원)도 함께 명시해 레코드 단독으로 지속시간 산출 가능.
    threshold_info 는 임계 계열의 구조화 {observed, threshold, unit} (X.733 thresholdInfo).
    trend_indication 은 change(severity 변경, notifyChangedAlarm) 의 moreSevere|lessSevere."""
    from datetime import datetime as _dt
    from services import alert_log
    sev = 'cleared' if action == 'close' else rule.get('perceived_severity', 'warning')
    ts = _dt.now().isoformat(timespec='seconds')
    rec = {
        'ts': ts,
        'alarm_id': alarm_id,
        'type': rule.get('type'), 'code': rule.get('code'),
        'perceived_severity': sev, 'severity': sev,   # 'severity' 구 reader 호환
        'event_type': rule.get('event_type'), 'probable_cause': rule.get('probable_cause'),
        'source': {'mo_class': rule.get('mo_class'), 'mo_instance': mo_instance,
                   'detected_by': detected_by},
        'action': action, 'message': message,
    }
    if action == 'open':
        rec['raised_time'] = ts
    else:
        rec['clear_time' if action == 'close' else 'change_time'] = ts
        epoch = alarm_id.rsplit('@', 1)[-1] if alarm_id else ''
        if epoch.isdigit():
            rec['raised_time'] = _dt.fromtimestamp(int(epoch)).isoformat(timespec='seconds')
    if trend_indication:
        rec['trend_indication'] = trend_indication
    if threshold_info:
        rec['threshold_info'] = threshold_info
    if rule.get('effect'):
        rec['effect'] = rule['effect']
    if rule.get('recommended_action'):
        rec['recommended_action'] = rule['recommended_action']
    alert_log.record_event(service_log_dir, rec)


def transition(state: dict, service_log_dir: str, rule: dict, mo_instance: str,
               detected_by: str, is_open: bool, msg_open: str, msg_close: str,
               threshold_info: dict = None, log=None):
    """활성식별 akey=(code@mo_instance). open 시 alarm_id 생성, close 가 동일 alarm_id 참조.

    이미 열린 알람에 rule severity 가 달라진 open 판정이 오면 **action=change**
    (32.111 notifyChangedAlarm — 같은 alarm_id 유지, trend_indication 동반). 단계 임계
    (staged_severity)의 승격/완화가 이 경로로 흐른다. state 값은 {'alarm_id','severity'}
    — 구형 str 값(severity 미상)은 change 판정 없이 값만 신형으로 승격."""
    akey = f"{rule.get('code')}@{mo_instance}"
    cur = state.get(akey)
    sev = rule.get('perceived_severity', 'warning')
    if is_open and cur is None:
        alarm_id = f"{akey}@{int(time.time())}"
        state[akey] = {'alarm_id': alarm_id, 'severity': sev}
        emit_alarm(service_log_dir, 'open', rule, mo_instance, detected_by, msg_open, alarm_id,
                   threshold_info=threshold_info)
        if log:
            log.log_info(f"[alarm] OPEN {akey} sev={sev} — {msg_open}")
    elif is_open and cur is not None:
        alarm_id = _entry_alarm_id(cur)
        cur_sev = _entry_severity(cur)
        state[akey] = {'alarm_id': alarm_id, 'severity': sev}
        if cur_sev and sev != cur_sev:
            trend = 'moreSevere' if _sev_rank(sev) > _sev_rank(cur_sev) else 'lessSevere'
            emit_alarm(service_log_dir, 'change', rule, mo_instance, detected_by, msg_open,
                       alarm_id, threshold_info=threshold_info, trend_indication=trend)
            if log:
                log.log_info(f"[alarm] CHANGE {akey} {cur_sev}→{sev} ({trend})")
    elif not is_open and cur is not None:
        alarm_id = _entry_alarm_id(state.pop(akey))
        emit_alarm(service_log_dir, 'close', rule, mo_instance, detected_by, msg_close, alarm_id,
                   threshold_info=threshold_info)
        if log:
            log.log_info(f"[alarm] CLEAR {akey}")


def staged_severity(rule: dict, observed) -> tuple:
    """단계 임계 평가 — rule.thresholds = {severity: value} (예: {'minor':80,'major':90,
    'critical':95}). observed 가 도달한 최고 서열 단계를 고른다.
    반환 (severity|None, 해당 단계 임계값) — 미달이면 (None, 최저 단계 값),
    thresholds 규칙이 아니면 (None, None) 으로 호출측이 단일 임계 경로로 폴백."""
    ths = rule.get('thresholds')
    if not isinstance(ths, dict) or not ths:
        return None, None
    stages = []
    for s, v in ths.items():
        try:
            stages.append((_sev_rank(s), s, float(v)))
        except (TypeError, ValueError):
            continue
    if not stages:
        return None, None
    hit = [t for t in stages if observed >= t[2]]
    if hit:
        _, s, v = max(hit)
        return s, v
    return None, min(t[2] for t in stages)


def close_legacy_code(state: dict, service_log_dir: str, rule: dict, old_code: str,
                      detected_by: str, log=None) -> int:
    """코드 개정 이행 — 옛 code 로 열린 활성 알람을 종결(close). 조건이 지속이면 다음
    평가가 현행 code 로 재발행한다 (service_registry._CODE_REVISIONS, §3.4 코드 문법).
    종결 건수 반환."""
    n = 0
    for akey in [k for k in list(state) if k.startswith(f"{old_code}@")]:
        alarm_id = _entry_alarm_id(state.pop(akey))
        mo = akey.split('@', 1)[1]
        emit_alarm(service_log_dir, 'close', {**rule, 'code': old_code}, mo, detected_by,
                   f"알람 코드 개정({old_code}→{rule.get('code')}) 이행 종결 — "
                   f"지속 조건은 새 코드로 재발행", alarm_id)
        n += 1
        if log:
            log.log_info(f"[alarm] MIGRATE-CLOSE {akey} → {rule.get('code')}")
    return n


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
            base_mo = r.get('mo_instance') or 'cims/cmp'
            for suffix, stats in cmp_nodes:
                mo = f"{base_mo}/{suffix}"
                transition(state, service_log_dir, r, mo, detected_by,
                           not bool(stats),
                           fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr),
                           fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr), log=log)
            # 평가 대상에서 이탈한 인스턴스의 open 알람은 영영 안 닫히므로 여기서 close:
            # ①구 집계 인스턴스(cims/cmp) ②endpoint 목록에서 제거된 노드(cims/cmp/<ip>:<port>).
            cur_mo = {f"{base_mo}/{suffix}" for suffix, _ in cmp_nodes}
            code = r.get('code')
            stale = [k for k in list(state)
                     if (k == f"{code}@{base_mo}"
                         or (k.startswith(f"{code}@{base_mo}/")
                             and k.split('@', 1)[1] not in cur_mo))]
            for akey in stale:
                mo = akey.split('@', 1)[1]
                transition(state, service_log_dir, r, mo, detected_by, False, '',
                           fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr)
                           or f"{mo} 관측 대상 제외 — 정리", log=log)
            continue
        if r.get('check') == 'rtp_pct_gte' and not cmp_nodes:
            continue    # CMP 관측 비활성 — 사용률 평가 불가
        mo = r.get('mo_instance') or f"cims/{r.get('target', '')}"
        rr, is_open = r, eval_service_rule(r, ctx, rtp_threshold)
        if r.get('check') == 'rtp_pct_gte' and isinstance(r.get('thresholds'), dict):
            # 단계 임계 — 도달 단계가 rule severity 가 되고, 승격/완화는 transition 의
            # change 경로로 흐른다.
            sev, sthr = staged_severity(r, pct)
            rr = {**r, 'perceived_severity': sev} if sev else r
            is_open = sev is not None
            if sthr is not None:
                thr = int(sthr)
        msg_open = fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr)
        msg_close = fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr)
        tinfo = ({'observed': pct, 'threshold': thr, 'unit': r.get('unit') or '%'}
                 if r.get('check') == 'rtp_pct_gte' else None)
        transition(state, service_log_dir, rr, mo, detected_by, is_open, msg_open, msg_close,
                   threshold_info=tinfo, log=log)
