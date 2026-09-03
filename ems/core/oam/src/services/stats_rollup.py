"""SIP 호·메시지 통계 — 1분 기저 집계 (sip_statistics.md §4~6).

원본(호 이력·SIP 원문)을 조회 때마다 훑지 않도록, **1분 버킷 집계**를 미리 만들어 둔다.
분보다 큰 단위(5m·10m·1h·1d·1w·1M)는 전부 1분의 정수배라 이 계층의 합산으로 유도된다 —
기저를 5분으로 잡으면 1분을 영원히 만들 수 없으므로 되돌릴 수 없는 선택을 피한 것이다.

저장:  {ServiceLogging.Dir}/stats/1m/YYYY/MM/DD.jsonl   (레코드 = 버킷 × 서비스)
상태:  {ServiceLogging.Dir}/stats/.rollup_state.json

**비율은 저장하지 않는다.** 비율은 합산이 불가능해서(5분 = 1분 비율 5개의 평균이 아니다)
롤업이 성립하지 않는다. 분자·분모만 적고 비율은 조회 시 만든다(§5.1).

## 미결 호 되짚기 (§6.1)

호 이력은 호가 **끝나야** 완성되는데(`duration`·`end_reason` 이 종료 시 채워짐) 버킷 귀속은
`invite_time` 이다. 그래서 긴 통화는 제 버킷이 이미 집계된 뒤에 완성된다. 되짚지 않으면 그
호는 영구히 누락되고, 긴 통화일수록 많이 빠져 통계가 실제보다 나쁘게 나온다.

고정 시간창(최근 N분 재계산)으로 풀지 않는다 — 창 값은 최대 통화시간을 예측해야 정할 수
있고, 예측 불가능한 값을 설계 입력으로 삼으면 창을 넘는 통화가 조용히 사라진다. 대신 미결
호를 목록으로 들고 다니다가, 끝난 호의 **버킷만** 다시 계산한다. 비용이 창 크기가 아니라
동시통화 수에 비례한다.

되짚기 대상 버킷의 날짜 파일이 이미 보존기간을 넘겨 사라졌으면 `late_dropped` 로 센다.
조용히 버리지 않는다 — 이 값이 계속 오르면 1분 계층 보존기간이 짧다는 신호다.
"""

import json
import os
import tempfile
import threading
from datetime import datetime, timedelta

from util.log_util import Logger

logger = Logger()

_MIN_FMT = '%Y-%m-%d %H:%M'
_STATE_NAME = '.rollup_state.json'

# ── 저장 계층 (§5.2) ──────────────────────────────────────────────────────
#  1m 만 원본에서 만들고, 1h·1d 는 그 **합산**이다. 5m·10m·1w·1M·1y 는 저장하지 않는다 —
#  조회 시 접으면 되고, 계층을 늘리면 롤업 경로와 보존 정책이 함께 늘어난다.
#
#  계층을 나누는 이유는 보존기간이다. 1분을 길게 보관하면(90일 = 12만 버킷) 월 단위 조회에
#  쓰지도 않는 정밀도를 위해 용량을 낸다. 같은 90일이 1d 계층에서는 90 레코드다.
UNITS = ('1m', '1h', '1d')

# 요청 단위 → 읽을 계층. 그 단위를 만들 수 있는 **가장 거친** 계층을 고른다.
_GRAN_UNIT = {'1m': '1m', '5m': '1m', '10m': '1m',
              '1h': '1h',
              '1d': '1d', '1w': '1d', '1M': '1d', '1y': '1d'}

# 선호 계층이 없는 날은 더 잔 계층으로 내려간다(잔 것은 항상 접을 수 있다).
_FALLBACK = {'1d': ('1d', '1h', '1m'), '1h': ('1h', '1m'), '1m': ('1m',)}


def unit_for(gran: str) -> str:
    return _GRAN_UNIT.get(gran, '1m')


def _subdir(unit: str) -> str:
    return os.path.join('stats', unit if unit in UNITS else '1m')

_lock = threading.Lock()
_service_log_dir = ''
_config: dict = {}
_enabled = False


def init(service_log_dir: str, config: dict = None, enabled: bool = True) -> None:
    """oam-svc 기동 시 1회. `ptt_index.init` 과 같은 규약 — 미초기화면 전부 no-op.

    config 는 접속 서비스 조회(services/access_services)에 그대로 넘긴다 — 서비스 판정에
    agent proxy 와 런타임 경로가 필요하다.
    """
    global _service_log_dir, _config, _enabled
    _service_log_dir = service_log_dir or ''
    _config = config or {}
    _enabled = bool(enabled and _service_log_dir)


def enabled() -> bool:
    return _enabled


def stats_root() -> str:
    return os.path.join(_service_log_dir, 'stats') if _service_log_dir else ''


# ──────────────────────────────────────────────────────────────
#  버킷 유틸
# ──────────────────────────────────────────────────────────────

def _minute(ts: str) -> str:
    """임의 시각 문자열 → 'YYYY-MM-DD HH:MM' (버킷 시작). 해석 불가면 ''.

    호 이력은 ISO('T'), 조회 파라미터는 공백 구분자다 — 여기서 한 번만 맞춘다.
    """
    if not ts:
        return ''
    t = ts.replace('T', ' ', 1)
    return t[:16] if len(t) >= 16 and t[4] == '-' else ''


def _day_of(bucket: str) -> str:
    return bucket[:10]


def _hour_of(bucket: str) -> str:
    return bucket[:13]


def _day_path(day: str, unit: str = '1m') -> str:
    """day='YYYY-MM-DD' → {stats}/{unit}/YYYY/MM/DD.jsonl"""
    if not stats_root() or len(day) < 10:
        return ''
    return os.path.join(_service_log_dir, _subdir(unit), day[0:4], day[5:7], day[8:10] + '.jsonl')


# ──────────────────────────────────────────────────────────────
#  레코드
# ──────────────────────────────────────────────────────────────

def _empty(bucket: str, svc: str) -> dict:
    return {
        'bucket': bucket,
        'unit': '1m',
        'svc': svc,
        'call': {
            'attempts': 0, 'sessions': 0, 'talked': 0, 'completed': 0,
            'reasons': {},
            'duration_sum_sec': 0,
            'pdd_sum_ms': 0, 'pdd_n': 0,
            'legs_invited': 0, 'legs_joined': 0,
            # 그룹 축 — PTT 만. 분 단위라 키 수는 그 분에 활성이던 그룹 수로 묶인다
            # (전체 그룹 수와 무관). 값은 카운터라 상위 단위로 그대로 합산된다.
            'by_group': {},
        },
        'msg': {'in': {}, 'out': {}},
        'open': 0,
        'late_dropped': 0,
    }


def _bump(d: dict, key: str, n: int = 1) -> None:
    d[key] = d.get(key, 0) + n


# ──────────────────────────────────────────────────────────────
#  원본 스캔 — 시간(HH) 디렉터리 단위
# ──────────────────────────────────────────────────────────────
#  분마다 원본을 훑으면 같은 시간 디렉터리를 60번 연다. 되짚기까지 겹치면 더 는다.
#  그래서 **시간 단위로 한 번 읽고 분으로 쪼갠다** — 로그 트리가 이미 HH 로 나뉘어 있어
#  이 경계가 자연스럽다.


def _scan_volte_hour(root: str, hour: str) -> list:
    """그 시간에 **시작한** VoLTE 호 목록 → [(minute, rec, path)].

    hour='YYYY-MM-DD HH'. 버킷 귀속은 invite_time 이므로 파일이 놓인 시간 디렉터리와
    invite_time 의 시가 같다(CallDir 이 시작 시각으로 디렉터리를 정한다).
    """
    import glob as _glob
    base = os.path.join(root, 'volte', hour[0:4], hour[5:7], hour[8:10], hour[11:13])
    if not os.path.isdir(base):
        return []
    out = []
    for path in _glob.glob(os.path.join(base, '**', '*.d', 'call.json'), recursive=True):
        try:
            with open(path, 'r', encoding='utf-8') as f:
                rec = json.load(f)
        except (OSError, ValueError):
            continue
        mi = _minute(rec.get('invite_time', ''))
        if mi:
            out.append((mi, rec, path))
    return out


def _scan_ptt_day(day: str) -> list:
    """그 날 시작한 PTT 세션 → [(minute, row)]. 소스는 세션 읽기 모델(ptt_index).

    디렉터리를 직접 훑지 않는 이유는 `_calc_ptt_stats` 와 같다 — 콘솔 호 이력과 세션 판정
    기준이 두 벌이 되면 두 화면이 다른 값을 낸다.
    """
    from services import ptt_index
    rows = ptt_index.day(day[0:4] + day[5:7] + day[8:10]) or []
    out = []
    for r in rows:
        mi = _minute(r.get('start', '') or r.get('start_time', ''))
        # 시작일이 이 날짜인 세션만 — 자정을 넘긴 세션은 두 날의 목록에 모두 나타날 수
        # 있고, 그대로 접으면 같은 세션을 두 번 센다.
        if mi and _day_of(mi) == day:
            out.append((mi, r))
    return out


def _scan_msg_hour(root: str, hour: str, dmap: dict) -> dict:
    """그 시간의 SIP 원문 → {minute: {svc: {'in'|'out': {키: 건수}}}}."""
    import glob as _glob
    from handlers.stats import _classify_service, _parse_msg_method, _ts_full

    base = os.path.join(root, hour[0:4], hour[5:7], hour[8:10], hour[11:13])
    if not os.path.isdir(base):
        return {}
    out: dict = {}
    patterns = [os.path.join(base, '*_sip.msg.jsonl'),
                os.path.join(base, '*_sip.msg.[0-9][0-9].jsonl')]
    for pattern in patterns:
        for fpath in _glob.glob(pattern):
            try:
                with open(fpath, 'r', encoding='utf-8') as f:
                    for line in f:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            entry = json.loads(line)
                        except ValueError:
                            continue
                        mi = _minute(_ts_full(fpath, entry.get('ts')))
                        if not mi:
                            continue
                        msg = entry.get('msg', '')
                        svc = _classify_service(msg, dmap) or 'unknown'
                        # dir 은 CSP·CSC 공통으로 RX(수신)/TX(송신) 이다.
                        io = 'out' if str(entry.get('dir', '')).upper() == 'TX' else 'in'
                        key = _parse_msg_method(msg)
                        _bump(out.setdefault(mi, {}).setdefault(svc, {}).setdefault(io, {}), key)
            except OSError:
                continue
    return out


# ──────────────────────────────────────────────────────────────
#  집계
# ──────────────────────────────────────────────────────────────

def _fold_volte(rec: dict, agg: dict) -> None:
    """VoLTE 호 1건을 버킷 집계에 접는다. 판정 근거는 §2.1 표."""
    c = agg['call']
    _bump(c, 'attempts')
    answered = bool(rec.get('answer_time'))
    state = rec.get('state', '')
    reason = rec.get('end_reason') or ''
    dur = int(rec.get('duration', 0) or 0)

    if answered:
        _bump(c, 'sessions')
        pdd = _pdd_ms(rec.get('invite_time', ''), rec.get('answer_time', ''))
        if pdd is not None:
            _bump(c, 'pdd_sum_ms', pdd)
            _bump(c, 'pdd_n')
    if dur > 0:
        _bump(c, 'talked')
        _bump(c, 'duration_sum_sec', dur)
    if answered and state == 'ended' and reason == 'normal':
        _bump(c, 'completed')
    if state == 'ended' and reason:
        _bump(c['reasons'], reason)
    # 1:1 통화의 leg 은 발신 1 + 착신 1. 참여율 분모는 착신 leg 이다(§1.2).
    _bump(c, 'legs_invited')
    if answered:
        _bump(c, 'legs_joined')
    if not reason:
        _bump(agg, 'open')


def _fold_ptt(row: dict, agg: dict) -> None:
    """PTT 세션 1건을 버킷 집계에 접는다.

    **성공률(성립/시도)은 여기서 낼 수 없다** — 실패한 그룹통화 시도가 원천에 없다(§8 Y6).
    기록이 있다는 것 자체가 성립을 뜻하므로 attempts 를 세면 항상 100% 가 된다. 그래서
    attempts/sessions 는 건드리지 않고 **소통(turn_count>0)과 참여(people/member_count)만**
    적는다. Y6 이 해소되면 attempts 가 채워지고 나머지는 그대로 성립한다.
    """
    c = agg['call']
    _bump(c, 'sessions')
    turns = int(row.get('turns', 0) or 0)
    if turns > 0:
        _bump(c, 'talked')
    dur = _dur_sec(row.get('start', ''), row.get('end', '') or '')
    if dur > 0:
        _bump(c, 'duration_sum_sec', dur)
    invited = int(row.get('member_count', 0) or 0)
    joined = len(row.get('people') or [])
    _bump(c, 'legs_invited', invited)
    _bump(c, 'legs_joined', min(joined, invited) if invited else joined)
    # 표시용 그룹 식별자는 mcptt_group_id. group_key 는 ptt_groups.id(surrogate)라 저장 경로
    # 키일 뿐 운영자가 보는 이름이 아니다 — 없을 때만 폴백한다(_calc_ptt_stats 와 같은 규칙).
    gid = row.get('mcptt_group_id') or row.get('group_key') or 'unknown'
    g = c['by_group'].setdefault(str(gid), {'sessions': 0, 'talked': 0})
    _bump(g, 'sessions')
    if turns > 0:
        _bump(g, 'talked')
    if row.get('state') != 'ended':
        _bump(agg, 'open')


def _pdd_ms(invite_ts: str, answer_ts: str):
    """PDD(post-dial delay) = answer_time - invite_time, 밀리초. 산출 불가면 None."""
    a, b = _parse(invite_ts), _parse(answer_ts)
    if a is None or b is None:
        return None
    d = int((b - a).total_seconds() * 1000)
    return d if d >= 0 else None


def _dur_sec(start: str, end: str) -> int:
    """구간 길이(초). 미종료(end 없음)면 0 — 진행중을 평균에 넣으면 조회 시점에 값이 흔들린다."""
    a, b = _parse(start), _parse(end)
    if a is None or b is None:
        return 0
    d = int((b - a).total_seconds())
    return d if d > 0 else 0


def _parse(ts: str):
    if not ts:
        return None
    try:
        return datetime.strptime(ts.replace('T', ' ', 1)[:19], '%Y-%m-%d %H:%M:%S')
    except (ValueError, TypeError):
        return None


def build_minutes(root: str, minutes: set, config: dict = None) -> dict:
    """대상 분들의 집계 레코드를 원본에서 만든다 → {(bucket, svc): record}.

    시간 디렉터리 단위로 원본을 읽고 분으로 쪼갠다 — 대상이 흩어져 있어도 같은 시간이면
    한 번만 읽는다.
    """
    from services import access_services

    out: dict = {}
    if not minutes:
        return out

    def _agg(bucket, svc):
        return out.setdefault((bucket, svc), _empty(bucket, svc))

    hours = sorted({_hour_of(m) for m in minutes})
    days = sorted({_day_of(m) for m in minutes})

    # 서비스 판정 맵 — 시간마다 다시 만들 이유가 없어 한 번만.
    try:
        dmap = access_services.domain_kind_map(config if config is not None else _config)
    except Exception as e:
        logger.log_warning(f"[stats-rollup] 서비스 판정 맵 조회 실패 — unknown 으로 집계: {e}")
        dmap = {}

    for hour in hours:
        for mi, rec, _path in _scan_volte_hour(root, hour):
            if mi in minutes:
                _fold_volte(rec, _agg(mi, 'volte'))
        for mi, per_svc in _scan_msg_hour(root, hour, dmap).items():
            if mi not in minutes:
                continue
            for svc, io_counts in per_svc.items():
                m = _agg(mi, svc)['msg']
                for io, counts in io_counts.items():
                    for k, n in counts.items():
                        _bump(m[io], k, n)

    for day in days:
        for mi, row in _scan_ptt_day(day):
            if mi in minutes:
                _fold_ptt(row, _agg(mi, 'ptt'))

    return out


# ──────────────────────────────────────────────────────────────
#  일별 파일 병합 (원자적 교체)
# ──────────────────────────────────────────────────────────────

def day_path_at(root: str, day: str, unit: str = '1m') -> str:
    """읽기용 경로 — 집계 주체(oam-svc)가 아닌 프로세스도 조회하므로 root 를 인자로 받는다."""
    if not root or len(day) < 10:
        return ''
    return os.path.join(root, _subdir(unit), day[0:4], day[5:7], day[8:10] + '.jsonl')


def read_day_at(root: str, day: str, unit: str = '1m') -> list:
    p = day_path_at(root, day, unit)
    if not p or not os.path.isfile(p):
        return []
    rows = []
    try:
        with open(p, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    continue
    except OSError:
        return []
    return rows


def read_day(day: str, unit: str = '1m') -> list:
    return read_day_at(_service_log_dir, day, unit)


def read_range(root: str, from_dt: str, to_dt: str) -> list:
    """[from_dt, to_dt] 안의 1분 레코드. 경계는 **분 단위 포함**이다.

    집계가 없는 구간은 빈 목록을 돌려준다 — 호출측이 원본 스캔으로 폴백할 근거가 된다
    (`StatsRollup.Enabled=false` 이거나 롤업 도입 전 구간).
    """
    a, b = _parse(from_dt), _parse(to_dt)
    if a is None or b is None:
        return []
    lo, hi = _minute(from_dt), _minute(to_dt)
    out = []
    cur = a.date()
    end = b.date()
    guard = 0
    while cur <= end and guard < 800:
        for r in read_day_at(root, cur.strftime('%Y-%m-%d')):
            bk = r.get('bucket', '')
            if lo <= bk <= hi:
                out.append(r)
        cur += timedelta(days=1)
        guard += 1
    return out


def _write_day(day: str, rows: list, unit: str = '1m') -> bool:
    """일별 파일 원자적 교체 — 읽는 쪽이 반쪽 파일을 보지 않게(ptt_index 와 같은 방식)."""
    p = _day_path(day, unit)
    if not p:
        return False
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix='.tmp.', dir=os.path.dirname(p))
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            for r in sorted(rows, key=lambda x: (x.get('bucket', ''), x.get('svc', ''))):
                f.write(json.dumps(r, ensure_ascii=False) + '\n')
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, p)
        return True
    except OSError as e:
        logger.log_error(f"[stats-rollup] {day} 기록 실패: {e}")
        return False


def merge_days(records: dict, buckets: set, fresh_days: set = None) -> tuple:
    """계산된 레코드를 일별 파일에 반영. `buckets` 는 이번에 다시 계산한 버킷 전체다.

    대상 버킷의 기존 행은 **결과가 비어도 지운다** — 되짚기로 값이 줄어든 경우(예: 오분류된
    호가 다른 서비스로 옮겨간 경우) 옛 행이 남으면 이중 계산된다.

    `fresh_days` = 정상 진행(watermark 전진)으로 새로 집계하는 날. 여기 없는 날에 파일이
    **없다면** 보존기간이 지나 이미 지워진 날이다 — 되짚기가 그 날을 되살리면 purge 가
    무의미해지므로 쓰지 않고 `late_dropped` 로 센다(§6.1).

    반환 (기록한 일수, late_dropped 버킷 수).
    """
    fresh_days = fresh_days or set()
    by_day: dict = {}
    for (bucket, svc), rec in records.items():
        by_day.setdefault(_day_of(bucket), {})[(bucket, svc)] = rec
    for b in buckets:
        by_day.setdefault(_day_of(b), {})

    written = 0
    late = 0
    for day, new_rows in by_day.items():
        day_buckets = {b for b in buckets if _day_of(b) == day}
        existing = read_day(day)
        exists = bool(_day_path(day)) and os.path.isfile(_day_path(day))
        if not exists and day not in fresh_days:
            # 보존기간이 지나 사라진 날 — 되살리지 않는다.
            late += len({b for (b, _svc) in new_rows}) or len(day_buckets)
            continue
        kept = [r for r in existing if r.get('bucket') not in day_buckets]
        merged = kept + [r for r in new_rows.values() if not _is_empty(r)]
        if not merged and not exists:
            # 빈 날에 0바이트 파일을 만들지 않는다 — 있는 파일이 곧 "집계된 날" 이다.
            continue
        if _write_day(day, merged):
            written += 1
    return written, late


def _is_empty(rec: dict) -> bool:
    """호도 메시지도 없는 버킷은 적지 않는다 — 빈 분이 대부분이라 파일만 커진다."""
    c = rec.get('call') or {}
    if any(int(c.get(k, 0) or 0) for k in
           ('attempts', 'sessions', 'talked', 'completed', 'legs_invited')):
        return False
    m = rec.get('msg') or {}
    return not ((m.get('in') or {}) or (m.get('out') or {}))


# ──────────────────────────────────────────────────────────────
#  파생 계층 (1m → 1h → 1d)
# ──────────────────────────────────────────────────────────────
#  원본은 한 번만 읽는다 — 1m 이 정확하면 그 위는 단순 합산이다. 그래서 파생은 그 날의
#  **1m 파일 전체를 다시 접어** 만든다(증분 누적 아님). 하루가 최대 1440 × 서비스 수라
#  통째 접기가 싸고, 되짚기로 1m 이 바뀐 날도 자동으로 정합해진다.

def fold_records(rows: list, unit: str) -> list:
    """저장 레코드들을 상위 단위로 접는다 → 같은 모양의 레코드 목록.

    계층별로 스키마가 같아야 롤업이 단순 합산으로 성립한다(§5.1) — 그래서 입력과 같은
    모양(`bucket/unit/svc/call/msg/open/late_dropped`)을 낸다.
    """
    out: dict = {}
    for r in rows:
        bk = bucket_of(r.get('bucket', ''), unit)
        sv = r.get('svc') or 'unknown'
        if not bk:
            continue
        tgt = out.get((bk, sv))
        if tgt is None:
            tgt = out[(bk, sv)] = _empty(bk, sv)
            tgt['unit'] = unit
        c = tgt['call']
        src = r.get('call') or {}
        for k in ('attempts', 'sessions', 'talked', 'completed',
                  'duration_sum_sec', 'pdd_sum_ms', 'pdd_n', 'legs_invited', 'legs_joined'):
            c[k] = c.get(k, 0) + int(src.get(k, 0) or 0)
        for k, v in (src.get('reasons') or {}).items():
            c['reasons'][k] = c['reasons'].get(k, 0) + int(v or 0)
        for gid, gv in (src.get('by_group') or {}).items():
            g = c['by_group'].setdefault(gid, {'sessions': 0, 'talked': 0})
            for k in ('sessions', 'talked'):
                g[k] = g.get(k, 0) + int((gv or {}).get(k, 0) or 0)
        for io in ('in', 'out'):
            m = tgt['msg'][io]
            for k, v in ((r.get('msg') or {}).get(io) or {}).items():
                m[k] = m.get(k, 0) + int(v or 0)
        tgt['open'] = tgt.get('open', 0) + int(r.get('open', 0) or 0)
        tgt['late_dropped'] = tgt.get('late_dropped', 0) + int(r.get('late_dropped', 0) or 0)
    return list(out.values())


def rebuild_derived(day: str) -> dict:
    """그 날의 1m 파일에서 1h·1d 를 다시 만든다. 반환 {unit: 레코드 수}.

    1m 이 없으면 파생도 두지 않는다 — 근거 없는 상위 계층이 남으면 조회가 그것을 믿는다.
    """
    base = read_day(day, '1m')
    made = {}
    for unit in ('1h', '1d'):
        recs = fold_records(base, unit) if base else []
        if recs:
            _write_day(day, recs, unit)
        else:
            p = _day_path(day, unit)
            if p and os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
        made[unit] = len(recs)
    return made


# ──────────────────────────────────────────────────────────────
#  상태 (watermark + 미결 호)
# ──────────────────────────────────────────────────────────────

def _state_path() -> str:
    root = stats_root()
    return os.path.join(root, _STATE_NAME) if root else ''


def load_state() -> dict:
    p = _state_path()
    if p and os.path.isfile(p):
        try:
            with open(p, 'r', encoding='utf-8') as f:
                st = json.load(f)
            if isinstance(st, dict):
                st.setdefault('watermark', '')
                st.setdefault('open', {})
                st.setdefault('late_dropped_total', 0)
                return st
        except (OSError, ValueError):
            pass
    return {'watermark': '', 'open': {}, 'late_dropped_total': 0}


def save_state(st: dict) -> None:
    p = _state_path()
    if not p:
        return
    try:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix='.tmp.', dir=os.path.dirname(p))
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            json.dump(st, f, ensure_ascii=False)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, p)
    except OSError as e:
        logger.log_error(f"[stats-rollup] 상태 기록 실패: {e}")


# ──────────────────────────────────────────────────────────────
#  실행
# ──────────────────────────────────────────────────────────────

# 첫 기동(watermark 없음)에 며칠치를 소급 집계할지. 원본이 남아 있어도 무한정 거슬러
# 올라가면 첫 롤업이 몇 분씩 걸린다 — 과거는 필요할 때 rebuild_range 로 다시 만든다.
_BACKFILL_DAYS = 1


def _now_minute() -> datetime:
    return datetime.now().replace(second=0, microsecond=0)


def _pending_minutes(watermark: str) -> set:
    """[watermark+1분, 지금-1분] — **완결된 분만** 집계한다.

    진행 중인 분을 집계하면 그 분이 끝나기 전 값이 확정돼 뒤에 온 메시지가 누락된다.
    """
    end = _now_minute() - timedelta(minutes=1)
    start = _parse(watermark + ':00') if watermark else None
    if start is None:
        start = end - timedelta(days=_BACKFILL_DAYS)
    start = start + timedelta(minutes=1)
    out = set()
    cur = start
    # 안전판 — 오래 멈춰 있었어도 한 번에 무한정 돌지 않는다(다음 주기에 이어서 한다).
    guard = 0
    while cur <= end and guard < 3 * 1440:
        out.add(cur.strftime(_MIN_FMT))
        cur += timedelta(minutes=1)
        guard += 1
    return out


def _refresh_open(st: dict, fresh: dict) -> set:
    """미결 목록 갱신 → 이번에 완성돼 되짚어야 할 버킷 집합.

    `fresh` = 이번 집계에서 새로 관찰한 미결 호 {키: 버킷}. 기존 목록의 호는 원본을 다시
    읽어 종료 여부를 확인한다.
    """
    dirty = set()
    still: dict = {}

    for key, meta in (st.get('open') or {}).items():
        bucket = (meta or {}).get('bucket', '')
        kind = (meta or {}).get('kind', 'volte')
        if not bucket:
            continue
        if _is_closed(key, kind):
            dirty.add(bucket)          # 끝났다 → 그 버킷만 다시 계산
        else:
            still[key] = meta          # 아직 미결 → 계속 들고 간다

    still.update(fresh)
    st['open'] = still
    return dirty


def _is_closed(key: str, kind: str) -> bool:
    """미결로 잡아둔 호가 끝났는가. 원본이 사라졌으면 '끝난 것'으로 본다(목록 누수 방지)."""
    if kind == 'volte':
        if not os.path.isfile(key):
            return True
        try:
            with open(key, 'r', encoding='utf-8') as f:
                rec = json.load(f)
        except (OSError, ValueError):
            return True
        return bool(rec.get('end_reason'))
    if kind == 'ptt':
        from services import ptt_index
        day, sess = key.split('|', 1) if '|' in key else ('', key)
        for r in ptt_index.day(day) or []:
            if r.get('key') == sess:
                return r.get('state') == 'ended'
        return True
    return True


def _observe_open(root: str, minutes: set) -> dict:
    """대상 분에서 아직 끝나지 않은 호 → {키: {'bucket':…, 'kind':…}}."""
    out = {}
    for hour in sorted({_hour_of(m) for m in minutes}):
        for mi, rec, path in _scan_volte_hour(root, hour):
            if mi in minutes and not rec.get('end_reason'):
                out[path] = {'bucket': mi, 'kind': 'volte'}
    for day in sorted({_day_of(m) for m in minutes}):
        ymd = day[0:4] + day[5:7] + day[8:10]
        for mi, row in _scan_ptt_day(day):
            if mi in minutes and row.get('state') != 'ended':
                out[f"{ymd}|{row.get('key', '')}"] = {'bucket': mi, 'kind': 'ptt'}
    return out


def run_once() -> dict:
    """1회 집계. 반환 = 요약(로그/진단용)."""
    if not _enabled:
        return {'skipped': 'disabled'}
    with _lock:
        st = load_state()
        pending = _pending_minutes(st.get('watermark', ''))
        dirty = _refresh_open(st, _observe_open(_service_log_dir, pending))
        targets = pending | dirty
        if not targets:
            return {'minutes': 0, 'buckets': 0, 'open': len(st.get('open') or {})}

        records = build_minutes(_service_log_dir, targets, _config)
        days, late = merge_days(records, targets,
                                 fresh_days={_day_of(m) for m in pending})
        # 1m 이 바뀐 날의 파생 계층을 다시 접는다 — 조회가 계층을 골라 읽으므로
        # 여기서 미루면 상위 단위 조회가 옛 값을 본다.
        for _d in sorted({_day_of(m) for m in targets}):
            rebuild_derived(_d)

        # 버킷의 `open` 은 그 분에 시작해 아직 안 끝난 호의 수다 — 되짚기의 진행 상황이
        # 화면에 그대로 보이게 하려는 것이라 집계 시점 값을 그대로 둔다.
        if pending:
            st['watermark'] = max(pending)
        if late:
            st['late_dropped_total'] = int(st.get('late_dropped_total', 0)) + late
        save_state(st)

        return {
            'minutes': len(pending),
            'revisited': len(dirty),
            'buckets': len(records),
            'days': days,
            'open': len(st.get('open') or {}),
            'late_dropped': late,
        }


def rebuild_range(from_day: str, to_day: str) -> int:
    """구간을 원본에서 다시 집계 (운영/검증용). 반환 = 기록한 버킷 수.

    watermark 를 건드리지 않는다 — 과거 재생성이 이후의 정상 집계를 되돌리면 안 된다.
    """
    if not _enabled:
        return 0
    a, b = _parse(from_day + ' 00:00:00'), _parse(to_day + ' 00:00:00')
    if a is None or b is None:
        return 0
    if b < a:
        a, b = b, a
    total = 0
    with _lock:
        cur = a
        while cur <= b:
            day = cur.strftime('%Y-%m-%d')
            minutes = {(cur + timedelta(minutes=i)).strftime(_MIN_FMT) for i in range(1440)}
            records = build_minutes(_service_log_dir, minutes, _config)
            merge_days(records, minutes, fresh_days={day})
            rebuild_derived(day)
            total += len(records)
            logger.log_info(f"[stats-rollup] rebuild {day}: 버킷 {len(records)}건")
            cur += timedelta(days=1)
    return total


# 1시간 계층 보존 하한 — 전년 동월 비교가 깨지지 않게(알람 스트림 90일 클램프와 같은 방식).
_RETAIN_MIN = {'1h': 365}


def purge_old(retain: dict) -> dict:
    """계층별 보존기간 초과 일별 파일 삭제 → {unit: 삭제 파일 수}.

    `retain` = {'1m': 14, '1h': 400, '1d': 0}. 0 이하는 무제한(no-op) — daily_jsonl 규약.
    계층을 나눈 이유가 보존기간이므로 여기서 계층별로 다르게 적용해야 의미가 있다.
    """
    if not _enabled:
        return {}
    from services import daily_jsonl
    out = {}
    for unit in UNITS:
        days = int((retain or {}).get(unit, 0) or 0)
        lo = _RETAIN_MIN.get(unit)
        if days > 0 and lo and days < lo:
            logger.log_warning(f"[stats-rollup] {unit} 보존 {days}일 → 하한 {lo}일로 올림")
            days = lo
        if days <= 0:
            continue
        n = daily_jsonl.purge_old(_service_log_dir, _subdir(unit), days)
        if n:
            out[unit] = n
    return out


# 원본에서 즉석 집계할 **날 수 상한**. 하루 즉석 집계는 시간 디렉터리 24개 남짓을 훑는다
# (실측: 16일 재집계 1.9초). 게이트웨이 프록시 타임아웃(5초) 안에 끝나야 하므로 14일로 둔다.
# 넘는 구간은 채우지 않고 **응답에 빠진 날짜를 실어** 알린다 — 조용히 작은 값을 내면 운영자가
# 보존기간 밖 구간을 실제 감소로 읽는다.
_SCAN_DAY_BUDGET = 14


def read_range_filled(root: str, from_dt: str, to_dt: str, config: dict = None,
                      budget: int = None, gran: str = '1m') -> tuple:
    """구간 레코드 + 커버리지 → (rows, coverage).

    **날마다 계층을 고른다.** 요청 단위가 감당되는 가장 거친 계층(`unit_for`)을 먼저 보고,
    그 날에 없으면 더 잔 계층으로 내려간다(잔 것은 항상 접을 수 있다). 어느 계층에도
    없으면 원본에서 즉석 집계한다.

    이렇게 나누는 이유는 보존기간이다 — 1분은 짧게(14일), 일 계층은 무제한으로 두면
    월 단위 조회가 1분 보존에 묶이지 않는다. 계층이 하나였을 때는 1분의 보존기간이
    전체 조회 지평을 결정해, 두 달 조회가 조용히 2주로 잘렸다(실측).

    즉석 집계 결과는 **적지 않는다** — 보존기간이 지나 지운 날을 되살리면 purge 가
    무의미해진다(merge_days 도 같은 이유로 그 날을 거부한다).
    """
    empty_cov = {'days': 0, 'unit': unit_for(gran), 'by_unit': {},
                 'rollup': 0, 'scanned': 0, 'missing': 0, 'missing_days': []}
    a, b = _parse(from_dt), _parse(to_dt)
    if a is None or b is None:
        return [], empty_cov
    budget = _SCAN_DAY_BUDGET if budget is None else budget
    lo, hi = _minute(from_dt), _minute(to_dt)
    chain = _FALLBACK.get(unit_for(gran), ('1m',))

    days, cur = [], a.date()
    while cur <= b.date() and len(days) < 800:
        days.append(cur.strftime('%Y-%m-%d'))
        cur += timedelta(days=1)

    rows = []
    by_unit: dict = {}
    missing = []
    for d in days:
        # 구간이 이 날을 **온전히** 덮는가. 덮지 않는 날(구간의 양 끝)에 거친 계층을 쓰면
        # 버킷을 쪼갤 수 없어 총계가 부풀어진다 — 그런 날만 1분 계층으로 내려가 정확히 자른다.
        whole = lo <= f'{d} 00:00' and f'{d} 23:59' <= hi
        use = chain if whole else ('1m',) + tuple(u for u in chain if u != '1m')
        for unit in use:
            if not os.path.isfile(day_path_at(root, d, unit) or ''):
                continue
            for r in read_day_at(root, d, unit):
                bt = parse_bucket(r.get('bucket', ''))
                if bt is None:
                    continue
                # 잔 계층은 버킷 시작이 구간 안이어야 한다. 온전히 덮인 날의 거친 계층은
                # 버킷 전체가 그 날 안이므로 추가 판정이 필요 없다.
                if not whole and not (lo <= bt.strftime(_MIN_FMT) <= hi):
                    continue
                rows.append(r)
            by_unit[unit] = by_unit.get(unit, 0) + 1
            break
        else:
            missing.append(d)

    # 예산을 넘으면 **최근 날부터** 채운다 — 오래된 쪽이 빠지는 것이 덜 놀랍다.
    fill = missing[-budget:] if budget > 0 else []
    omitted = [d for d in missing if d not in set(fill)]
    for d in fill:
        day0 = _parse(d + ' 00:00:00')
        minutes = set()
        for i in range(1440):
            mi = (day0 + timedelta(minutes=i)).strftime(_MIN_FMT)
            if lo <= mi <= hi:
                minutes.add(mi)
        if minutes:
            rows.extend(build_minutes(root, minutes, config).values())

    return rows, {
        'days': len(days), 'unit': unit_for(gran), 'by_unit': by_unit,
        'rollup': sum(by_unit.values()), 'scanned': len(fill),
        'missing': len(omitted), 'missing_days': omitted[:40],
    }


# ──────────────────────────────────────────────────────────────
#  조회 — 1분 계층 합산 (§7)
# ──────────────────────────────────────────────────────────────
#  분보다 큰 단위는 전부 1분의 정수배다. 그래서 별도 계층을 두지 않고 여기서 접는다.
#  이 합산이 성립하는 근거는 **비율을 저장하지 않는다**는 §5.1 의 결정이다 — 저장된 것이
#  전부 분자·분모라 단순 덧셈으로 상위 단위가 만들어진다.

GRANULARITIES = ('1m', '5m', '10m', '1h', '1d', '1w', '1M', '1y')

_GRAN_MINUTES = {'1m': 1, '5m': 5, '10m': 10, '1h': 60, '1d': 1440}


def parse_bucket(label: str):
    """버킷 라벨 → datetime. 계층이 여럿이라 라벨 모양도 여럿이다.

    'YYYY-MM-DD HH:MM' | 'YYYY-MM-DD' | 'YYYY-MM' | 'YYYY'
    분 라벨만 받으면 1h·1d 계층 레코드가 조회에서 **조용히 전부 버려진다**(실측).
    """
    v = (label or '').strip()
    if len(v) == 16:
        return _parse(v + ':00')
    if len(v) == 10:
        return _parse(v + ' 00:00:00')
    if len(v) == 7:
        return _parse(v + '-01 00:00:00')
    if len(v) == 4:
        return _parse(v + '-01-01 00:00:00')
    return _parse(v)


def bucket_of(label: str, gran: str) -> str:
    """버킷 라벨 → 요청 단위의 버킷 시작 라벨. 라벨은 항상 **시작 시각**이다(§4.2).

    거친 라벨을 더 잔 단위로 되돌릴 수는 없다(일 → 시). 그런 조합은 조회 계층 선택
    (`unit_for`)이 애초에 만들지 않는다.
    """
    dt = parse_bucket(label)
    if dt is None:
        return ''
    if gran == '1m':
        return dt.strftime(_MIN_FMT)
    if gran in ('5m', '10m'):
        step = _GRAN_MINUTES[gran]
        return dt.replace(minute=(dt.minute // step) * step).strftime(_MIN_FMT)
    if gran == '1h':
        return dt.replace(minute=0).strftime(_MIN_FMT)
    if gran == '1d':
        return dt.strftime('%Y-%m-%d')
    if gran == '1w':
        # 주는 일요일 시작 — weekday() 는 월=0 이라 일요일은 6.
        return (dt.date() - timedelta(days=(dt.weekday() + 1) % 7)).strftime('%Y-%m-%d')
    if gran == '1M':
        return dt.strftime('%Y-%m')
    if gran == '1y':
        return dt.strftime('%Y')
    return ''


def bucket_start_iso(bucket: str) -> str:
    """버킷 시작을 오프셋 포함 ISO 로. 화면이 라벨 파싱 없이 시각을 알 수 있게 한다.

    오프셋은 **호스트 로컬**을 그대로 쓴다 — 운영 전제는 KST(§4.2)지만, 다른 존의 호스트에서
    +09:00 을 박아 내면 틀린 값을 사실처럼 내보내게 된다.
    """
    dt = parse_bucket(bucket)
    return dt.astimezone().isoformat() if dt else ''


def _zero_call() -> dict:
    return {'attempts': 0, 'sessions': 0, 'talked': 0, 'completed': 0,
            'duration_sum_sec': 0, 'pdd_sum_ms': 0, 'pdd_n': 0,
            'legs_invited': 0, 'legs_joined': 0, 'open': 0, 'late_dropped': 0,
            'reasons': {}, 'by_group': {}}


def _add_call(dst: dict, src: dict, open_n: int = 0, late_n: int = 0) -> None:
    for k in ('attempts', 'sessions', 'talked', 'completed',
              'duration_sum_sec', 'pdd_sum_ms', 'pdd_n', 'legs_invited', 'legs_joined'):
        dst[k] = dst.get(k, 0) + int(src.get(k, 0) or 0)
    for k, v in (src.get('reasons') or {}).items():
        dst['reasons'][k] = dst['reasons'].get(k, 0) + int(v or 0)
    for gid, gv in (src.get('by_group') or {}).items():
        tgt = dst['by_group'].setdefault(gid, {'sessions': 0, 'talked': 0})
        for k in ('sessions', 'talked'):
            tgt[k] = tgt.get(k, 0) + int((gv or {}).get(k, 0) or 0)
    dst['open'] = dst.get('open', 0) + int(open_n or 0)
    dst['late_dropped'] = dst.get('late_dropped', 0) + int(late_n or 0)


def _add_msg(dst: dict, src: dict) -> None:
    for io in ('in', 'out'):
        tgt = dst.setdefault(io, {})
        for k, v in (src.get(io) or {}).items():
            tgt[k] = tgt.get(k, 0) + int(v or 0)


def _rate(num: int, den: int) -> float:
    """백분율 1자리. 분모 0 은 0 — 표시용 파생값이며 저장·롤업 근거가 아니다(§5.1)."""
    return round(num / den * 100, 1) if den > 0 else 0


def with_rates(c: dict) -> dict:
    """분자·분모에 비율을 덧붙인다. **분모를 지운 채로 내지 않는다** — 비율만 받은 화면은
    구간을 다시 합칠 수 없고, 3건 중 2건과 3만건 중 2만건을 같은 무게로 보여준다."""
    out = dict(c)
    out['reasons'] = dict(c.get('reasons') or {})
    # 그룹 축은 건수 내림차순. 분포 위젯은 {키: 수} 한 겹을 기대하므로 세션수만 뽑은
    # `by_group_sessions` 를 함께 낸다 — 두 겹 map 을 화면이 다시 펴지 않게.
    bg = {k: dict(v) for k, v in (c.get('by_group') or {}).items()}
    out['by_group'] = dict(sorted(bg.items(), key=lambda x: -x[1].get('sessions', 0)))
    out['by_group_sessions'] = {k: v.get('sessions', 0) for k, v in out['by_group'].items()}
    out['success_rate'] = _rate(c.get('sessions', 0), c.get('attempts', 0))
    out['talk_rate'] = _rate(c.get('talked', 0), c.get('attempts', 0))
    out['completion_rate'] = _rate(c.get('completed', 0), c.get('sessions', 0))
    out['join_rate'] = _rate(c.get('legs_joined', 0), c.get('legs_invited', 0))
    # 세션을 분모로 한 소통률 — **PTT 용**이다. PTT 는 실패한 시도가 원천에 없어
    # attempts 가 0 이고(§8 Y6), 그러면 talk_rate 가 분모 0 으로 항상 0% 가 된다.
    # "세션은 섰는데 아무도 발언하지 못한" floor 장애는 이 값에서만 드러난다.
    out['talk_rate_sessions'] = _rate(c.get('talked', 0), c.get('sessions', 0))
    n = c.get('pdd_n', 0)
    out['avg_pdd_ms'] = round(c.get('pdd_sum_ms', 0) / n) if n else 0
    t = c.get('talked', 0)
    out['avg_duration_sec'] = round(c.get('duration_sum_sec', 0) / t, 1) if t else 0
    return out


def aggregate(rows: list, gran: str, svc: str = 'all', include_msg: bool = False) -> tuple:
    """1분 레코드들을 요청 단위로 접는다 → (buckets, totals).

    buckets = [{bucket, bucket_start, <svc>: {…}, all: {…}}]  (시간 오름차순)
    totals  = {<svc>: {…}, 'all': {…}}

    **`all` 은 svc 필터와 무관하게 항상 전체 서비스 합계다.** 필터한 부분합을 `all` 로 내면
    읽는 쪽이 그걸 시스템 전체로 오해한다 — 필터는 어떤 서비스 칸을 낼지만 정한다.

    `include_msg` 는 메시지 축까지 실을지. 호 조회에는 필요 없고 응답만 커진다.
    """
    want = None if svc in (None, '', 'all') else {svc}
    by_bucket: dict = {}
    totals: dict = {}

    def _cell(store, key):
        return store.setdefault(key, {'call': _zero_call(), 'msg': {'in': {}, 'out': {}}})

    for r in rows:
        sv = r.get('svc') or 'unknown'
        bk = bucket_of(r.get('bucket', ''), gran)
        if not bk:
            continue
        slot = by_bucket.setdefault(bk, {})
        keys = ['all'] + ([sv] if (want is None or sv in want) else [])
        for key in keys:
            for target in (_cell(slot, key), _cell(totals, key)):
                _add_call(target['call'], r.get('call') or {},
                          open_n=r.get('open', 0), late_n=r.get('late_dropped', 0))
                if include_msg:
                    _add_msg(target['msg'], r.get('msg') or {})

    def _out(cell):
        o = with_rates(cell['call'])
        if include_msg:
            o['msg'] = cell['msg']
        return o

    buckets = []
    for bk in sorted(by_bucket):
        entry = {'bucket': bk, 'bucket_start': bucket_start_iso(bk)}
        for key, cell in by_bucket[bk].items():
            entry[key] = _out(cell)
        buckets.append(entry)

    return buckets, {k: _out(v) for k, v in totals.items()}
