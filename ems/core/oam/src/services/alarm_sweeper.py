"""알람 sweeper 공용 코어 (알람 표준화 X.733/32.111 — alarm_standardization.md).

소유권(oam_base_service_split §4): 서비스 계열 규칙(process_unresponsive(csp/cmp probe)/
db_down/rtp_pct_gte, scope != 'agent')의 평가·발화는 **oam-svc** 소유 — CSP/CMP probe·DB
접속이 서비스 관측 설정(oam-svc config)과 함께 움직인다. 단일 프로세스(--role all)에서는
oam_app 이 같은 코어를 호출해 동작 무변경. agent 계열(disk_high/module_down — 프로세스
생존의 전 모듈 정본, 감지 L1) 평가는 base(oam_app) 잔류 — transition/emit 코어를 여기서
함께 쓴다. 감지 3계층은 표준화 §3.4(b).

open-state: state = { akey("code@mo_instance"): {'alarm_id','severity','detected_by'} } —
호출 프로세스가 보유. 소유 파티션의 키는 detected_by 가 유일하다 (표준화 §3.4(b)·
파이프라인 §4.3) — 구 mo 접두(cims/*) 판정은 detected_by 없는 구 레코드 흡수용으로만 남는다.
mo_instance 루트는 소유 주체(서버명/그룹명) — 관측 주소의 신원 해석은 build_mo_root_resolver.
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


def _entry_detected_by(v):
    return v.get('detected_by') if isinstance(v, dict) else None


class _Safe(dict):
    def __missing__(self, k):  # 템플릿에 없는 키는 빈 문자열 (KeyError 방지)
        return ''


def fmt(tmpl: str, **kw) -> str:
    return (tmpl or '').format_map(_Safe(kw))


def _is_legacy_service_akey(akey: str) -> bool:
    """구 mo 루트(cims/*) — detected_by 없는 구 레코드의 파티션 폴백 판정 전용."""
    mo = akey.split('@', 1)[1] if '@' in akey else ''
    return mo.startswith('cims/')


def partition_of(detected_by: str, akey: str) -> str:
    """open-state 소유 파티션 판정 — 키는 detected_by 가 유일 (표준화 §3.4(b)·
    파이프라인 §4.3). 구 레코드의 인스턴스 접미(self:<node>·agent:<host>)는 클래스
    매칭으로, detected_by 부재 구 레코드는 mo 접두(cims/*)로 흡수한다."""
    db = detected_by or ''
    if db == 'self' or db.startswith('self:'):
        return 'self'
    if db in ('oam-svc', 'oam'):
        return 'service'
    if db == 'agent' or db.startswith('agent:'):
        return 'agent'
    return 'service' if _is_legacy_service_akey(akey) else 'agent'


def restore_open_state(service_log_dir: str, scope: str = 'all', days: int = 30,
                       log=None) -> dict:
    """기동 시 활성 알람 복원. scope: 'service'=OAM 관측 계열(oam-svc/oam),
    'agent'=agent 계열, 'self'=모듈 자기보고, 'all'=자기보고 제외 전부(role=all 대행),
    **'base'=agent 계열 + base 자신이 발화한 것**(detected_by='oam').
    분리 배포에서 base/oam-svc/FM ingest 가 각자 소유 계열만 추적하도록 나눈다 —
    파티션 판정은 partition_of (detected_by 일원화).

    `'base'` 가 따로 있는 이유: `partition_of` 는 `oam`·`oam-svc` 를 같은 `service`
    파티션으로 묶는데, 분리 배포(role=base)의 base 는 `scope='agent'` 만 복원해서
    **자기가 `detected_by='oam'` 으로 발화한 알람을 재기동 후 못 찾았다** — 닫아야 할
    때 in-memory 에 키가 없어 close 가 발행되지 않고 영구 미해소로 남는다(실측:
    detected_by=oam 이 open 10 / close 7). 파티션 분류는 그대로 두고 복원 조건만
    detected_by 로 정밀화한다 (drift_sweeper.drift_open_keys 와 같은 규약).
    드리프트 계열(A-PRC-003 @ `<그룹명>/config/<coll>`)은 `_drift_open` 이 따로
    복원하므로 여기에도 함께 들어오지만, base 의 `_alert_open` 쪽에서는 평가 주체가
    없어 무해하다(다음 기동 시 close 반영으로 자연 정리)."""
    from services import alert_log
    state: dict = {}
    if not service_log_dir:
        return state
    try:
        meta = alert_log.compute_open_state(service_log_dir, days=days, with_meta=True)
        if scope == 'base':
            meta = {k: m for k, m in meta.items()
                    if partition_of(m.get('detected_by'), k) == 'agent'
                    or (m.get('detected_by') or '') == 'oam'}
        elif scope in ('self', 'service', 'agent'):
            meta = {k: m for k, m in meta.items()
                    if partition_of(m.get('detected_by'), k) == scope}
        else:   # 'all' — 자기보고(self)는 FM ingest 가 자체 복원
            meta = {k: m for k, m in meta.items()
                    if partition_of(m.get('detected_by'), k) != 'self'}
        restored = {k: {'alarm_id': m['alarm_id'], 'severity': m.get('perceived_severity'),
                        'detected_by': m.get('detected_by') or ''}
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
        state[akey] = {'alarm_id': alarm_id, 'severity': sev, 'detected_by': detected_by}
        emit_alarm(service_log_dir, 'open', rule, mo_instance, detected_by, msg_open, alarm_id,
                   threshold_info=threshold_info)
        if log:
            log.log_info(f"[alarm] OPEN {akey} sev={sev} — {msg_open}")
    elif is_open and cur is not None:
        alarm_id = _entry_alarm_id(cur)
        cur_sev = _entry_severity(cur)
        state[akey] = {'alarm_id': alarm_id, 'severity': sev, 'detected_by': detected_by}
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


def close_migrated_keys(state: dict, service_log_dir: str, detected_by: str, match,
                        reason: str, log=None) -> int:
    """이행 종결 — match(akey) 인 활성키를 원 akey(code·mo 그대로)로 close 발행.
    코드·mo 루트가 함께 바뀐 이행(표준화 §6 — 활성키 자체가 바뀜)에 쓴다. 지속
    조건은 다음 평가/동기화가 현행 code@mo 로 재발화한다. 종결 건수 반환."""
    n = 0
    for akey in [k for k in list(state) if match(k)]:
        alarm_id = _entry_alarm_id(state.pop(akey))
        code, _, mo = akey.partition('@')
        emit_alarm(service_log_dir, 'close', {'code': code}, mo, detected_by, reason, alarm_id)
        n += 1
        if log:
            log.log_info(f"[alarm] MIGRATE-CLOSE {akey}")
    return n


def server_mo_root(agent) -> str:
    """서버(agent)의 mo 루트 — **불변 id 파생** (표준화 §3.4(b), identifier_model.md).
    mo_instance 는 활성 알람 식별키의 절반이라 가변 이름을 넣으면 서버 이름을 바꾼
    순간 열린 알람을 같은 키로 찾지 못해 영영 닫히지 않는다. 사람이 읽는 이름은
    표시 계층이 id 로 해석해 붙인다(`mo_label`)."""
    return f"a{(agent or {}).get('id')}"


def group_mo_root(group) -> str:
    """서버 그룹의 mo 루트 — 불변 id 파생. HA 서비스 키와 같은 어휘(`g<id>`)."""
    return f"g{(group or {}).get('id')}"


def build_mo_root_resolver(config):
    """관측 주소 → 소유 주체 루트(**id 파생**) 해석기 (표준화 §3.4(b)).
    VIP 관측 = 그룹 루트(`g<id>`), 노드 주소 관측 = 서버 루트(`a<id>`). 어휘의 정본은
    인벤토리 — 여기서는 그 실체화본(ha_groups VIP·agent 등록 IP/인터페이스)으로
    해석하고, 해석 불가 주소는 주소 그대로 루트로 쓴다(비표준 배포 폴백).
    스토어 적재 비용이 있으므로 스윕당 1회 생성한다."""
    vip_to_group: dict = {}
    addr_to_server: dict = {}
    try:
        from services import file_store, ha_lookup
        for g in ha_lookup.ha_groups_all(config):
            root = group_mo_root(g)
            for vip in ha_lookup.group_vip_set(g):
                vip_to_group.setdefault(vip, root)
        for a in file_store.load_all(file_store.domain_dir(config, 'agents')):
            root = server_mo_root(a)
            if a.get('ip'):
                addr_to_server.setdefault(str(a['ip']), root)
            for itf in (a.get('interfaces') or []):
                if isinstance(itf, dict) and itf.get('ip'):
                    addr_to_server.setdefault(str(itf['ip']), root)
    except Exception:
        pass

    def resolve(addr) -> str:
        addr = str(addr or '')
        return vip_to_group.get(addr) or addr_to_server.get(addr) or addr
    return resolve


def build_mo_label_resolver(config):
    """mo 루트(`a<id>`/`g<id>`) → 사람이 읽는 이름. 표시 전용 (§3.4(b) userLabel).
    조회 시점에 해석하므로 이름이 바뀌면 과거 레코드의 표시도 현재 이름을 따른다."""
    names: dict = {}
    try:
        from services import file_store, ha_lookup
        for g in ha_lookup.ha_groups_all(config):
            if g.get('name'):
                names[group_mo_root(g)] = str(g['name'])
        for a in file_store.load_all(file_store.domain_dir(config, 'agents')):
            if a.get('name'):
                names[server_mo_root(a)] = str(a['name'])
    except Exception:
        pass

    def label(mo) -> str:
        """mo_instance 전체를 받아 루트만 이름으로 치환한 표시 문자열을 돌려준다."""
        mo = str(mo or '')
        if not mo:
            return mo
        root, sep, rest = mo.partition('/')
        nm = names.get(root)
        return f"{nm}{sep}{rest}" if nm else mo
    return label


def cert_earliest_days_left(path):
    """인증서 파일의 **가장 이른 만료**까지 남은 일수 — A-PRC-009 cert_expiring 판정 공용.

    반환 `(days_left, not_after_iso)`, 읽기/파싱 실패 시 `(None, None)`.
    체인 PEM(leaf+CA)이면 CA 만료도 함께 걸린다 — leaf 만 보면 CA 가 먼저 죽는 구성을
    놓친다(csp/CspListenerManager.cpp `_certEarliestDaysLeft` 와 같은 규약).
    `openssl` CLI 는 OAM 이 CA·서버 인증서 생성에 이미 쓰는 도구라 새 의존이 아니다."""
    import subprocess as _sp, datetime as _dt, re as _re
    try:
        with open(path, 'r') as f:
            pem = f.read()
    except OSError:
        return None, None
    blocks = _re.findall(r'-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----', pem, _re.S)
    if not blocks:
        return None, None
    earliest = None
    for b in blocks:
        try:
            r = _sp.run(['openssl', 'x509', '-noout', '-enddate'], input=b,
                        capture_output=True, text=True, timeout=10)
            if r.returncode != 0:
                continue
            raw = (r.stdout or '').strip()
            if not raw.startswith('notAfter='):
                continue
            exp = _dt.datetime.strptime(raw[len('notAfter='):].strip(), '%b %d %H:%M:%S %Y %Z')
            exp = exp.replace(tzinfo=_dt.timezone.utc)
            if earliest is None or exp < earliest:
                earliest = exp
        except Exception:
            continue
    if earliest is None:
        return None, None
    return ((earliest - _dt.datetime.now(_dt.timezone.utc)).days,
            earliest.isoformat(timespec='seconds'))


def mgmt_mo_root(config) -> str:
    """관리평면 공통 신원 — OAM 관측 객체(DB 등)의 mo 루트 (표준화 §3.3).
    관리 HA 그룹(oam 패키지 호스팅)의 **id 파생 루트**를 쓰고, 비 HA 배포는 OAM SystemId
    (그룹이 없으면 바뀔 이름 자체가 없다)."""
    try:
        from services import ha_lookup
        g = ha_lookup.ha_group_for_package(config, 'oam')
        if g and g.get('id') is not None:
            return group_mo_root(g)
    except Exception:
        pass
    return config.get('SystemId', 'oam')


def eval_service_rule(rule: dict, ctx: dict, rtp_threshold: int = 80) -> bool:
    chk = rule.get('check')
    if chk == 'process_unresponsive':
        # 원격 probe(STATS) 무응답 — 프로세스 생존(process_down, agent L1)과 별개 조건.
        return not bool(ctx.get(rule.get('target')))
    if chk == 'db_down':
        return not ctx.get('db_ok')
    if chk == 'rtp_pct_gte':
        return ctx.get('rtp_pct', 0) >= int(rule.get('threshold', rtp_threshold))
    return False


def _mo_module_segment(akey: str) -> str:
    """akey 의 mo 두 번째 세그먼트(모듈) — <루트>/<모듈>[/...] 규약 (표준화 §3.4(b))."""
    mo = akey.split('@', 1)[1] if '@' in akey else ''
    seg = mo.split('/')
    return seg[1] if len(seg) > 1 else ''


def sweep_service_rules(config: dict, state: dict, service_log_dir: str,
                        detected_by: str = 'oam-svc', rtp_threshold: int = 80, log=None):
    """서비스 계열 규칙 1회 평가 — CSP/CMP UDP probe + DB 체크 + RTP 사용률.
    probe 헬퍼는 handlers.stats 공유(3s 캐시). 규칙은 service_registry(코어 + descriptor).

    mo 는 관측 신원으로 런타임 합성한다 (표준화 §3.4(b) 소유 주체 루트) — 노드 주소
    관측 = <서버명>/<모듈>, VIP 관측 = <그룹명>/<모듈>, OAM 관측 객체(DB) =
    <관리그룹>/db. CMP 는 전 미디어 노드 개별 평가(AA 다중 노드) — probe 는
    endpoint 소유 서버별 <서버명>/cmp 로, RTP 사용률도 노드별 <서버명>/cmp/rtp_ports
    로 발화한다(전 노드 합산 아님). MediaServer.Endpoints/CmpIp 미설정이면 CMP 관측
    비활성(cmp 계열 규칙 skip). descriptor 의 mo_instance 명시값이 있으면 그대로 쓴다."""
    from services import service_registry
    from handlers.stats import (_get_csp_stats, _get_db,
                                _media_endpoints, _probe_cmp)
    resolve = build_mo_root_resolver(config)
    csp = _get_csp_stats(config)
    csp_addr = (config.get('CspNotify') or {}).get('Ip', '127.0.0.1')
    cmp_configured = bool(((config.get('MediaServer') or {}).get('Endpoints'))
                          or config.get('CmpIp'))
    cmp_nodes = []          # [(mo_root, port, stats dict)]
    if cmp_configured:
        raw = [(ip, port, _probe_cmp(ip, port)) for ip, port in _media_endpoints(config)]
        root_count: dict = {}
        for ip, _port, _stats in raw:
            root_count[resolve(ip)] = root_count.get(resolve(ip), 0) + 1
        for ip, port, stats in raw:
            root = resolve(ip)
            # 같은 서버의 다중 endpoint — 루트에 포트 접미로 활성키 충돌 방지 (예외 배치)
            if root_count[root] > 1:
                root = f"{root}:{port}"
            cmp_nodes.append((root, port, stats))
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
    total = sum((s.get('rtp_ports_total', 0) or 0) for _, _, s in cmp_nodes)
    used = sum((s.get('rtp_ports_used', 0) or 0) for _, _, s in cmp_nodes)
    agg_pct = int(round(used / total * 100)) if total > 0 else 0
    ctx = {'csp': csp, 'db_ok': db_ok, 'rtp_pct': agg_pct}
    rules = [r for r in service_registry.alert_rules(config) if r.get('scope') != 'agent']
    # 코드/mo 루트 이행 종결 (표준화 §6) — 구 mo(cims/*) 활성키는 code·mo 가 함께
    # 바뀌어 어떤 현행 평가에도 안 잡힌다. 원 akey 로 종결하고, 지속 조건은 아래
    # 평가가 현행 code@mo 로 재발화. cims/ha/* 는 drift 스위퍼 소유라 제외.
    close_migrated_keys(
        state, service_log_dir, detected_by,
        lambda k: '@cims/' in k and not k.split('@', 1)[1].startswith('cims/ha/'),
        "알람 코드/mo 루트 이행 종결 — 지속 조건은 현행 정의 코드로 재발행", log=log)
    for r in rules:
        chk = r.get('check')
        thr = r.get('threshold', rtp_threshold)
        code = r.get('code')
        if chk == 'process_unresponsive' and r.get('target') == 'cmp':
            cur_mo = set()
            for root, _port, stats in cmp_nodes:
                mo = r.get('mo_instance') or f"{root}/cmp"
                cur_mo.add(mo)
                transition(state, service_log_dir, r, mo, detected_by,
                           not bool(stats),
                           fmt(r.get('msg_open'), mo=mo, threshold=thr),
                           fmt(r.get('msg_close'), mo=mo, threshold=thr), log=log)
            # 평가 대상에서 이탈한 인스턴스(endpoint 제거/신원 재해석)의 open 은 영영
            # 안 닫히므로 close. 같은 code 의 csp probe 계열과는 모듈 세그먼트로 구분.
            for akey in [k for k in list(state)
                         if k.startswith(f"{code}@") and _mo_module_segment(k) == 'cmp'
                         and k.split('@', 1)[1] not in cur_mo]:
                mo = akey.split('@', 1)[1]
                transition(state, service_log_dir, r, mo, detected_by, False, '',
                           fmt(r.get('msg_close'), mo=mo, threshold=thr)
                           or f"{mo} 관측 대상 제외 — 정리", log=log)
            continue
        if chk == 'rtp_pct_gte':
            if not cmp_nodes:
                continue    # CMP 관측 비활성 — 사용률 평가 불가
            cur_mo = set()
            for root, _port, stats in cmp_nodes:
                node_total = stats.get('rtp_ports_total', 0) or 0
                node_used = stats.get('rtp_ports_used', 0) or 0
                if node_total <= 0:
                    continue    # probe 실패/무관측 — 판정 보류 (표준화 §3.4(d))
                pct = int(round(node_used / node_total * 100))
                mo = r.get('mo_instance') or f"{root}/cmp/rtp_ports"
                cur_mo.add(mo)
                rr, is_open, thr_n = r, pct >= int(thr), thr
                if isinstance(r.get('thresholds'), dict):
                    # 단계 임계 — 도달 단계가 rule severity, 승격/완화는 change 경로.
                    sev, sthr = staged_severity(r, pct)
                    rr = {**r, 'perceived_severity': sev} if sev else r
                    is_open = sev is not None
                    if sthr is not None:
                        thr_n = int(sthr)
                tinfo = {'observed': pct, 'threshold': thr_n, 'unit': r.get('unit') or '%'}
                transition(state, service_log_dir, rr, mo, detected_by, is_open,
                           fmt(r.get('msg_open'), mo=mo, pct=pct, threshold=thr_n),
                           fmt(r.get('msg_close'), mo=mo, pct=pct, threshold=thr_n),
                           threshold_info=tinfo, log=log)
            # 관측이 전무(전 probe 실패)하면 아무 판정도 하지 않는다 (표준화 §3.4(d)) —
            # stale 정리는 실제 관측된 노드가 있을 때만.
            if cur_mo:
                for akey in [k for k in list(state)
                             if k.startswith(f"{code}@") and k.split('@', 1)[1] not in cur_mo]:
                    mo = akey.split('@', 1)[1]
                    transition(state, service_log_dir, r, mo, detected_by, False, '',
                               f"{mo} 관측 대상 제외 — 정리", log=log)
            continue
        # 단일 인스턴스 규칙 — 관측 신원으로 mo 합성
        if chk == 'process_unresponsive':
            mo = r.get('mo_instance') or f"{resolve(csp_addr)}/{r.get('target', 'csp')}"
        elif chk == 'db_down':
            mo = r.get('mo_instance') or f"{mgmt_mo_root(config)}/db"
        else:
            mo = r.get('mo_instance') or f"{mgmt_mo_root(config)}/{r.get('target', '')}"
        is_open = eval_service_rule(r, ctx, rtp_threshold)
        msg_open = fmt(r.get('msg_open'), mo=mo, threshold=thr)
        msg_close = fmt(r.get('msg_close'), mo=mo, threshold=thr)
        transition(state, service_log_dir, r, mo, detected_by, is_open, msg_open, msg_close,
                   log=log)
