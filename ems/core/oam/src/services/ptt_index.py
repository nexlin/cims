"""ptt_index.py — PTT 세션 읽기 모델(인덱스).

**녹취 디렉터리가 정본이고 이 인덱스는 파생물이다.** 지우면 다음 조회에서 다시 만들어진다.

왜 두는가
---------
세션 목록은 종전에 조회할 때마다 `ptt/*/YYYY/MM/DD/HH` 를 전부 glob 했다. 그룹 100개 ×
1년이면 87만 디렉터리라 평면 정렬·페이징·기간 확대가 그 위에서는 성립하지 않는다.
일자별 요약 파일을 만들어 두면 과거 조회는 파일 한 번 읽기로 끝나고, 스캔은 '오늘'
버킷으로 한정된다.

왜 OAM 이 쓰는가 (CSP 가 아니라)
--------------------------------
발언 턴·발화 시간·동시 발언은 CMP 가 `segments.jsonl` 에 기록하고 CSP 는 모른다. CSP 가
인덱스를 쓰면 지표가 빠지거나 CMP 까지 두 번째 writer 로 끌어들여야 한다. 읽는 쪽이
읽기 모델을 소유하면 writer 가 하나고, C++ 변경이 없어 CSP/CMP 재배포 위험도 없다.

저장
----
    {ServiceLogDir}/ptt/index/YYYYMMDD.jsonl      세션 1건 = 1줄 (세션 **시작일** 기준)

    지난 날짜 — 파일이 없으면 그 날짜 버킷만 스캔해 1회 생성, 이후 불변.
    오늘      — 스위퍼가 주기적으로 오늘 버킷만 재스캔해 전체 재작성.
    진행중    — 인덱스에 넣지 않는다. state/ptt/*.json (CSP 가 쓴다) 로 실시간 도출.

자정을 넘긴 세션은 **시작일** 파일에 들어간다. 세션키에 시작 시각이 박혀 있어 판정이
자명하고, 다음 날 버킷으로 이어진 부분은 session_dirs() 가 따라가므로 한 줄로 온전하다.
"""

import json
import os
import re
import time
import threading
import glob as _glob
from datetime import datetime, timedelta

# ── 설정 ──────────────────────────────────────────────────────
_calls_dir: str = ""
_enabled: bool = True

# 일자별 캐시 — {'YYYYMMDD': (stamp, rows)}. 지난 날짜는 불변이라 stamp 를 보지 않는다.
_cache: dict = {}
_lock = threading.Lock()

# 세션 디렉터리 이름 — CSP CallDir::_sesDirName 규약 S{yyyymmddHHMMSSuuuuuu}_{n}
_SES_RE = re.compile(r'^S(\d{14,20})_(\d+)$')


def init(service_log_dir: str, enabled: bool = True) -> None:
    global _calls_dir, _enabled
    _calls_dir = service_log_dir or ""
    _enabled = bool(enabled)
    with _lock:
        _cache.clear()


def enabled() -> bool:
    return _enabled and bool(_calls_dir)


# ── 파일 유틸 (모듈 자립 — flow_logger 와 순환 import 를 만들지 않는다) ──

def _read_json(path: str) -> dict:
    try:
        with open(path, 'r') as f:
            o = json.load(f)
        return o if isinstance(o, dict) else {}
    except Exception:
        return {}


def _read_jsonl(path: str) -> list:
    out = []
    try:
        with open(path, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except ValueError:
                    continue
    except OSError:
        pass
    return out


# ═══════════════════════════════════════════════════════════════
#  경로 · 세션 디렉터리 (녹취 정본 쪽)
# ═══════════════════════════════════════════════════════════════

def ptt_root() -> str:
    return os.path.join(_calls_dir, "ptt") if _calls_dir else ""


def index_dir() -> str:
    return os.path.join(ptt_root(), "index") if _calls_dir else ""


def _sanitize(s: str) -> str:
    return "".join(c for c in (s or "") if c.isalnum() or c in "+-_.@")[:64]


def group_base(group_key: str) -> str:
    """PTT 그룹 base 디렉터리 ptt/{key} (없으면 '+' prefix 보정 시도)"""
    if not _calls_dir:
        return ""
    safe = _sanitize(group_key)
    base = os.path.join(ptt_root(), safe)
    if os.path.isdir(base):
        return base
    if group_key and not group_key.startswith('+'):
        alt = os.path.join(ptt_root(), "+" + safe)
        if os.path.isdir(alt):
            return alt
    return ""


def ses_start_iso(key: str) -> str:
    """세션키 → 시작 시각 ISO. 신형 키가 아니면 ''."""
    m = _SES_RE.match(key or "")
    if not m:
        return ""
    d = m.group(1)
    return f"{d[0:4]}-{d[4:6]}-{d[6:8]}T{d[8:10]}:{d[10:12]}:{d[12:14]}"


def ses_start_day(key: str) -> str:
    """세션키 → 시작 일자 'YYYYMMDD'. 구 녹취 키('YYYYMMDDHH')도 앞 8자리."""
    m = _SES_RE.match(key or "")
    if m:
        return m.group(1)[0:8]
    digits = "".join(c for c in (key or "") if c.isdigit())
    return digits[0:8] if len(digits) >= 8 else ""


def window_of(part_dir: str) -> str:
    """세션 버킷 디렉터리 → 시간창 'YYYYMMDDHH'.
    신형은 .../{Y}/{M}/{D}/{H}/{sesdir}, 구형은 .../{Y}/{M}/{D}/{H} 자체다."""
    p = (part_dir or "").rstrip(os.sep).split(os.sep)
    if p and _SES_RE.match(p[-1]):
        p = p[:-1]
    return "".join(p[-4:]) if len(p) >= 4 else ""


def bucket_parts(hh_dir: str) -> list:
    """시간버킷 안의 (세션키, 디렉터리) 목록.

    신형 = 버킷 하위 세션 디렉터리들. 구형(세션 디렉터리 도입 이전 녹취) = 버킷 자체가
    한 세션 — 이때 세션키는 종전과 같은 'YYYYMMDDHH' 라 기존 링크·이력이 그대로 열린다."""
    out = []
    try:
        for name in os.listdir(hh_dir):
            if _SES_RE.match(name) and os.path.isdir(os.path.join(hh_dir, name)):
                out.append((name, os.path.join(hh_dir, name)))
    except OSError:
        return []
    if out:
        return sorted(out)
    for fn in ("segments.jsonl", "events.jsonl", "session.json", "floor.jsonl"):
        if os.path.exists(os.path.join(hh_dir, fn)):
            return [(window_of(hh_dir), hh_dir)]
    return []


def session_dirs(group_key: str, key: str) -> list:
    """세션키 → 그 세션이 걸쳐 있는 시간버킷 디렉터리들 (시간순).

    세션이 시간을 넘기면 다음 버킷에 같은 이름의 디렉터리가 생긴다. 이름에 시작 시각이
    있으므로 시작 버킷부터 연속으로 짚어 나가면 되고, 끊기는 지점이 세션의 끝이다.
    구형 세션키('YYYYMMDDHH')는 그 버킷 하나가 전부다."""
    base = group_base(group_key)
    if not base:
        return []
    m = _SES_RE.match(key or "")
    if m:
        d = m.group(1)
        try:
            cur = datetime(int(d[0:4]), int(d[4:6]), int(d[6:8]), int(d[8:10]))
        except ValueError:
            return []
        out = []
        while True:
            p = os.path.join(base, cur.strftime("%Y"), cur.strftime("%m"),
                             cur.strftime("%d"), cur.strftime("%H"), key)
            if not os.path.isdir(p):
                break
            out.append(p)
            cur += timedelta(hours=1)
        return out
    w = "".join(c for c in (key or "") if c.isdigit())
    if len(w) >= 10:
        d = os.path.join(base, w[:4], w[4:6], w[6:8], w[8:10])
        if os.path.isdir(d):
            return [d]
    return []


def has_active_recording(d: str) -> bool:
    """녹취 진행 중 — CMP 가 열린 세그먼트를 *.recording 으로 쓰고 close 시 rename 한다."""
    return bool(_glob.glob(os.path.join(d, 'seg', '*', '*.recording')) or
                _glob.glob(os.path.join(d, '*.recording')))


# ═══════════════════════════════════════════════════════════════
#  세그먼트 지표
# ═══════════════════════════════════════════════════════════════

def seg_audio_tracks(s: dict) -> list:
    """세그먼트 한 행의 음성 슬롯 트랙 목록. CMP 의 tracks[] 가 정본이고, 그 이전
    녹취는 flat 키(audio_file/audio1_file/speaker_id_audioK)에서 합성한다."""
    raw = s.get("tracks")
    if isinstance(raw, list) and raw:
        return [{"slot": t.get("slot", 0), "speakers": t.get("speakers") or []}
                for t in raw
                if isinstance(t, dict) and t.get("kind") == "audio" and t.get("file")]
    out = []
    dur = int(s.get("duration_ms", 0) or 0)
    rep = s.get("speaker_id", "")
    for key, val in s.items():
        if not key.endswith("_file") or not val or not isinstance(val, str):
            continue
        prefix = key[:-len("_file")]
        if not prefix.startswith("audio"):
            continue
        tail = prefix[len("audio"):]
        slot = int(tail) if tail.isdigit() else 0
        sid = s.get(f"speaker_id_{prefix}", "") or (rep if slot == 0 else "")
        out.append({"slot": slot,
                    "speakers": [{"id": sid, "offset_ms": 0, "dur_ms": dur}] if sid else []})
    out.sort(key=lambda t: t["slot"])
    return out


def seg_max_concurrent(tracks: list) -> int:
    """세그먼트 안에서 동시에 열려 있던 화자 구간의 최대 수"""
    events = []
    for t in tracks:
        for sp in t.get("speakers") or []:
            d = int(sp.get("dur_ms", 0) or 0)
            if d <= 0:
                continue
            off = int(sp.get("offset_ms", 0) or 0)
            events.append((off, 1))
            events.append((off + d, -1))
    if not events:
        return 1 if tracks else 0
    events.sort(key=lambda e: (e[0], e[1]))   # 같은 시각이면 종료 먼저 — 인접은 겹침이 아니다
    cur = peak = 0
    for _, delta in events:
        cur += delta
        peak = max(peak, cur)
    return peak


# ═══════════════════════════════════════════════════════════════
#  그룹 디스크립터
# ═══════════════════════════════════════════════════════════════

def group_descriptor(group_key: str) -> dict:
    """ptt/{key}/group.json — 그룹의 **최신** 편성 스냅샷 + 분류(kind).

    kind 판정: private = 1:1 (priv-<caller>-<callee> ephemeral) / adhoc = group.json 은
    있으나 surrogate id 없음(DB 미등록) / group = DB 등록 그룹 / unknown = group.json 유실.
    """
    base = group_base(group_key)
    gj = _read_json(os.path.join(base, "group.json")) if base else {}
    members = [m.get("user_id", "") for m in (gj.get("members") or [])
               if isinstance(m, dict) and m.get("user_id")]
    if gj.get("group_type") == "private" or group_key.startswith("priv-"):
        kind = "private"
    elif not gj:
        kind = "unknown"
    elif not gj.get("id"):
        kind = "adhoc"
    else:
        kind = "group"
    return {
        "group_key": group_key,
        "kind": kind,
        "mcptt_group_id": gj.get("mcptt_group_id", "") or group_key,
        "name": gj.get("name", ""),
        "group_type": gj.get("group_type", ""),
        "video": bool(gj.get("video_enabled")),
        "member_count": gj.get("member_count", len(members)),
        "members": members,
        "floor_control": gj.get("floor_control", ""),
        "floor_policy": gj.get("floor_policy", ""),
        "max_talkers": gj.get("max_talkers", 0) or 0,
    }


# ═══════════════════════════════════════════════════════════════
#  요약 (녹취 → 인덱스 한 줄)
# ═══════════════════════════════════════════════════════════════

def summarize(group_key: str, key: str, parts: list = None, gd: dict = None) -> dict:
    """세션 하나를 인덱스 한 줄로. parts 미지정 시 세션 디렉터리를 직접 찾는다."""
    if parts is None:
        parts = session_dirs(group_key, key)
    if not parts:
        return {}
    if gd is None:
        gd = group_descriptor(group_key)

    speakers, windows = set(), []
    seg_count = turn_count = max_con = 0
    total_ms = talk_ms = 0
    st_min = en_max = ""
    active = False
    sj = {}
    now_window = datetime.now().strftime("%Y%m%d%H")

    for d in parts:
        windows.append(window_of(d))
        for s in _read_jsonl(os.path.join(d, "segments.jsonl")):
            seg_count += 1
            total_ms += int(s.get("duration_ms", 0) or 0)
            # 동시 발언·전이중 private call 은 한 세그먼트에 슬롯 트랙이 여럿이다 —
            #   speaker_id(대표 화자)만 세면 화자·발언이 과소 집계된다.
            tracks = seg_audio_tracks(s)
            max_con = max(max_con, seg_max_concurrent(tracks))
            for t in tracks:
                spans = t.get("speakers") or []
                turn_count += len(spans) or 1
                for sp in spans:
                    if sp.get("id"):
                        speakers.add(sp["id"])
                    talk_ms += int(sp.get("dur_ms", 0) or 0)
            if not tracks:
                sp = s.get("speaker_id", "")
                if sp:
                    speakers.add(sp)
                turn_count += 1
            stt, ent = s.get("start_time", ""), s.get("end_time", "")
            if stt and (not st_min or stt < st_min):
                st_min = stt
            if ent and (not en_max or ent > en_max):
                en_max = ent
        if window_of(d) == now_window and has_active_recording(d):
            active = True
        if not sj:
            # 세션 디스크립터 — CSP 가 세션 시작 버킷에 남긴 당시 스냅샷. floor 축은
            #   이것이 정본(그룹 루트 group.json 은 최신이라 과거 세션에 소급되면 왜곡).
            sj = _read_json(os.path.join(d, "session.json")) or {}

    # 참여자: session.json 멤버 ∪ 실제 발언 화자 ∪ 개시자. 발언 없이 참여만 한 멤버도 잡는다.
    people = set(speakers)
    for m in (sj.get("members") or []):
        if isinstance(m, dict) and m.get("user_id"):
            people.add(m["user_id"])
    initiator = sj.get("initiator", "")
    if initiator and initiator != "autojoin":
        people.add(initiator)

    w0 = windows[0] if windows else ""
    ses_iso = ses_start_iso(key)
    start = sj.get("start_time") or ses_iso or st_min or (
        f"{w0[0:4]}-{w0[4:6]}-{w0[6:8]}T{w0[8:10]}:00:00" if len(w0) >= 10 else "")
    end = None if active else (sj.get("end_time") or en_max or None)

    return {
        "key": key,
        "group_key": group_key,
        "kind": gd.get("kind", "unknown"),
        "mcptt_group_id": gd.get("mcptt_group_id", ""),
        "name": gd.get("name", ""),
        "group_type": gd.get("group_type", ""),
        "video": gd.get("video", False),
        "member_count": gd.get("member_count", 0),
        "sesid": sj.get("sesid", ""),
        "call_id": sj.get("call_id", ""),
        "initiator": initiator,
        "people": sorted(people),
        "speakers": sorted(speakers),
        "start": start,
        "end": end,
        "state": "active" if active else "ended",
        "windows": sorted(set(windows)),
        "turns": turn_count,
        "segments": seg_count,
        "speaker_count": len(speakers),
        "max_concurrent": max_con,
        "speech_ms": total_ms,
        "talk_ms": talk_ms,
        # floor 축은 세션 스냅샷이 정본, 없으면(구 녹취) 그룹 레벨 폴백
        "floor_control": sj.get("floor_control", "") or gd.get("floor_control", ""),
        "floor_policy": sj.get("floor_policy", "") or gd.get("floor_policy", ""),
        "max_talkers": sj.get("max_talkers", 0) or gd.get("max_talkers", 0) or 0,
        "legacy": not bool(_SES_RE.match(key)),
    }


# ═══════════════════════════════════════════════════════════════
#  스캔 · 인덱스
# ═══════════════════════════════════════════════════════════════

def scan_day(day: str) -> list:
    """그 날짜에 **시작한** 세션들을 녹취에서 직접 훑는다 (인덱스 생성의 근거).

    day 버킷에서 발견되더라도 세션키의 시작일이 day 가 아니면 건너뛴다 — 전날 시작해
    자정을 넘긴 세션은 전날 파일 소관이다(그쪽에서 session_dirs 가 이어 붙인다)."""
    root = ptt_root()
    if not root or not os.path.isdir(root) or len(day) < 8:
        return []
    yyyy, mm, dd = day[0:4], day[4:6], day[6:8]
    rows = []
    for gpath in sorted(_glob.glob(os.path.join(root, "*"))):
        gkey = os.path.basename(gpath)
        if gkey == "index" or not os.path.isdir(gpath):
            continue
        day_dir = os.path.join(gpath, yyyy, mm, dd)
        if not os.path.isdir(day_dir):
            continue
        gd = None
        seen = set()
        for hh_dir in sorted(_glob.glob(os.path.join(day_dir, "[0-9][0-9]"))):
            for key, part in bucket_parts(hh_dir):
                if key in seen or ses_start_day(key) != day:
                    continue
                seen.add(key)
                if gd is None:
                    gd = group_descriptor(gkey)
                row = summarize(gkey, key, gd=gd)
                if row:
                    rows.append(row)
    rows.sort(key=lambda r: (r.get("start") or "", r.get("key") or ""), reverse=True)
    return rows


def group_keys() -> list:
    """녹취가 있는 그룹 저장 키 목록 (ptt/* 디렉터리, index 제외)."""
    root = ptt_root()
    if not root or not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root)):
        if name == "index":
            continue
        if os.path.isdir(os.path.join(root, name)):
            out.append(name)
    return out


def last_window(group_key: str) -> str:
    """그룹의 가장 최근 시간버킷 'YYYYMMDDHH'. 버킷 내용을 읽지 않고 연/월/일/시를
    각 단계 최대값으로 내려가며 찾는다 — 전체 glob(그룹당 수천 디렉터리) 대신 listdir 4회."""
    base = group_base(group_key)
    if not base:
        return ""
    cur, parts = base, []
    for _ in range(4):
        try:
            names = [n for n in os.listdir(cur)
                     if n.isdigit() and os.path.isdir(os.path.join(cur, n))]
        except OSError:
            return ""
        if not names:
            return ""
        pick = max(names)
        parts.append(pick)
        cur = os.path.join(cur, pick)
    return "".join(parts)


def count_on_day(group_key: str, day_str: str) -> int:
    """그 날짜에 그 그룹이 연 세션 수 (인덱스 기준)."""
    return sum(1 for r in day(day_str) if r.get("group_key") == group_key)


def _day_path(day: str) -> str:
    return os.path.join(index_dir(), f"{day}.jsonl")


def _write_day(day: str, rows: list) -> None:
    d = index_dir()
    if not d:
        return
    try:
        os.makedirs(d, exist_ok=True)
        tmp = _day_path(day) + ".tmp"
        with open(tmp, "w") as f:
            for r in rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        os.replace(tmp, _day_path(day))   # 원자적 교체 — 읽는 쪽이 반쪽 파일을 보지 않는다
    except OSError:
        pass


def day(day_str: str, force: bool = False) -> list:
    """그 날짜의 세션 목록. 지난 날짜는 파일이 정답이고, 오늘은 스캔이 정답이다."""
    if not enabled() or len(day_str) < 8:
        return scan_day(day_str) if _calls_dir else []
    today = datetime.now().strftime("%Y%m%d")
    is_today = (day_str >= today)

    if not force:
        with _lock:
            hit = _cache.get(day_str)
        # 지난 날짜는 불변이라 캐시를 그대로 쓰되, 인덱스 파일이 사라졌으면 캐시도 버린다 —
        #   "파일을 지우면 다시 만들어진다" 가 재기동 없이도 성립해야 한다.
        if hit and (not is_today or (time.time() - hit[0]) < 5) \
                and (is_today or os.path.exists(_day_path(day_str))):
            return hit[1]

    rows = None
    if not is_today and not force:
        p = _day_path(day_str)
        if os.path.exists(p):
            rows = _read_jsonl(p)
    if rows is None:
        rows = scan_day(day_str)
        # 오늘 파일도 써 둔다 — OAM 재기동 직후 첫 조회가 스캔을 다시 돌지 않게.
        _write_day(day_str, rows)

    with _lock:
        _cache[day_str] = (time.time(), rows)
    return rows


def rebuild(day_str: str) -> int:
    """그 날짜 인덱스를 녹취에서 다시 만든다 (운영/검증용). 반환 = 세션 수."""
    rows = day(day_str, force=True)
    return len(rows)


def range_days(from_day: str, to_day: str) -> list:
    """[from_day, to_day] 구간 (최대 90일). 시작 시각 내림차순."""
    out = []
    try:
        d0 = datetime.strptime(from_day, "%Y%m%d")
        d1 = datetime.strptime(to_day, "%Y%m%d")
    except ValueError:
        return out
    if d1 < d0:
        d0, d1 = d1, d0
    span = min((d1 - d0).days, 89)
    for i in range(span + 1):
        out.extend(day((d0 + timedelta(days=i)).strftime("%Y%m%d")))
    out.sort(key=lambda r: (r.get("start") or "", r.get("key") or ""), reverse=True)
    return out


# ═══════════════════════════════════════════════════════════════
#  진행중 세션 — state/ptt/*.json (CSP 가 쓴다)
# ═══════════════════════════════════════════════════════════════

def live() -> list:
    """지금 열려 있는 세션. 인덱스에 넣지 않고 매번 상태 파일에서 도출한다
    (파일 수 = 통화 중 가입자 수라 저렴하고, 종료 즉시 사라져 stale 이 없다)."""
    if not _calls_dir:
        return []
    root = ptt_root()
    keys = {}   # (group_key, session key) → True
    for p in _glob.glob(os.path.join(_calls_dir, "state", "ptt", "*.json")):
        st = _read_json(p)
        rec = st.get("record_dir") or ""
        if not rec:
            continue
        try:
            rel = os.path.relpath(rec, root).split(os.sep)
        except ValueError:
            continue
        # ptt/{gkey}/{Y}/{M}/{D}/{H}/{sesdir}  (구 상태 파일은 그룹 base 만 가리킨다)
        if len(rel) >= 6 and _SES_RE.match(rel[5]):
            keys[(rel[0], rel[5])] = True
    out = []
    for (gkey, key) in keys:
        row = summarize(gkey, key)
        if row:
            row["state"] = "active"
            row["end"] = None
            out.append(row)
    out.sort(key=lambda r: (r.get("start") or ""), reverse=True)
    return out


# ═══════════════════════════════════════════════════════════════
#  스위퍼 — 오늘 인덱스 갱신
# ═══════════════════════════════════════════════════════════════

def sweep() -> int:
    """오늘 인덱스를 다시 만든다. 반환 = 세션 수 (호출측 로깅용)."""
    if not enabled():
        return 0
    return rebuild(datetime.now().strftime("%Y%m%d"))
