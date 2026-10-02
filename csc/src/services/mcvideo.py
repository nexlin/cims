"""MCVideo 설정 평면 — 그룹 문서의 MCVideo 몫·MCVideo user profile·service configuration (docs/design/features/mcvideo.md §5.1).

MCVideo 는 MCPTT 의 확장이 아니라 나란한 MC 서비스다(TS 23.280 §3 — 한 그룹 = 서비스 집합). 이 모듈이 서비스 경계를 가진다:
  · 그룹 문서(TS 24.481 §7.2) — MCVideo <service>(enabler = MCVideo ICSI) · <list-service> 의 mcvideo-* 속성 · entry
    <mcvideo-mcvideo-id> · 규칙 action mcvideo-* 의 조각. 문서 틀은 services.mcptt.get_group_xml 이 짓고 여기 조각을 끼운다.
  · MCVideo user profile(TS 24.484 §9.3, AUID org.3gpp.mcvideo.user-profile) — 행(mcvideo_user_profile) = 이용 자격.
  · MCVideo service configuration(TS 24.484 §9.4, AUID org.3gpp.mcvideo.service-config) — 시스템 전역 1건, 값은 설정
    `McVideoServiceConfig.*`(MCPTT `ServiceConfig.*` 와 같은 규칙). 송출·수신 제어 서버(CMP)의 타이머 정본이다.
값의 SoT = DB 표 mcvideo_group_attrs / mcvideo_user_profile (sql/migrate_mcvideo.sql). MCVideo ID = MCPTT ID(§7 D1) 라 신원·그룹 목록은
services.mcptt 의 USERS·GROUPS 를 그대로 쓴다.
"""
from __future__ import annotations

import html as _html
from typing import Optional

from util.log_util import Logger

logger = Logger()

ICSI_MCPTT = "urn:urn-7:3gpp-service.ims.icsi.mcptt"       # TS 24.379 Annex D (그룹 문서 MCPTT enabler — TS 24.481 §7.2.2)
ICSI_MCVIDEO = "urn:urn-7:3gpp-service.ims.icsi.mcvideo"   # TS 24.281 Annex E.2.1
NS_GI = "urn:3gpp:ns:mcpttGroupInfo:1.0"
NS_USER_PROFILE = "urn:3gpp:ns:mcvideo:user-profile:1.0"   # TS 24.484 §9.3.2.4
NS_SERVICE_CONFIG = "urn:3gpp:ns:mcvideoServiceConfig:1.0"  # TS 24.484 §9.4.2.4
AUID_USER_PROFILE = "org.3gpp.mcvideo.user-profile"        # TS 24.484 §9.3.2.2
AUID_SERVICE_CONFIG = "org.3gpp.mcvideo.service-config"    # TS 24.484 §9.4.2.2
MIME_USER_PROFILE = "application/vnd.3gpp.mcvideo-user-profile+xml"      # TS 24.484 §9.3.2.5
MIME_SERVICE_CONFIG = "application/vnd.3gpp.mcvideo-service-config+xml"  # §9.4.2.5 (본문은 "application/" 누락 — IANA 형)
NS_UE_CONFIG = "urn:3gpp:mcvideo:mcvideoUEConfig:1.0"       # TS 24.484 §9.2.2.3
AUID_UE_CONFIG = "org.3gpp.mcvideo.ue-config"               # TS 24.484 §9.2.2.2
MIME_UE_CONFIG = "application/vnd.3gpp.mcvideo-ue-config+xml"            # §9.2.2.5

# 그룹 속성 기본값 = DB 열 기본값(sql/migrate_mcvideo.sql). 보호 둘은 **요소가 없으면 true 로 읽히므로**(TS 24.481 §7.2.8)
#   문서에 늘 명시한다 — E2E(GMK) 가 서기 전까지 false(mcvideo.md §7 D7).
GROUP_ATTR_DEFAULTS = {
    "invite_members": False,                 # mcvideo-on-network-invite-members — false = chat (§7 D5)
    "max_duration_sec": 3600,                # mcvideo-on-network-maximum-duration (TNG3)
    "max_transmitters": 2,                   # mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members
    "audio_encodings": ["AMR-WB"],           # mcvideo-preferred-audio-encodings (rtpmap encoding name)
    "video_encodings": ["H264"],             # mcvideo-preferred-video-encodings
    "video_resolutions": None,               # mcvideo-preferred-video-resolutions — None = 요소 생략
    "video_frame_rate": None,                # mcvideo-preferred-video-frame-rate — None = 요소 생략
    "reception_hang_timer_sec": 30,          # on-network-reception-hang-timer (T5, TS 24.581 §11.1.3)
    "min_number_to_start": 0,                # mcvideo-on-network-minimum-number-to-start
    "group_priority": None,                  # mcvideo-on-network-group-priority 0..255 — None = 생략(가장 낮음)
    "protect_media": False,                  # mcvideo-protect-media
    "protect_transmission_control": False,   # mcvideo-protect-transmission-control
    "allow_conference_state": True,          # mcvideo-on-network-allow-conference-state
}
# 그룹 선호 코덱으로 받는 이름 = 서버가 집행하는 코덱뿐이다(mcvideo.md §7 D13 — CSP 는 음성 AMR-WB · 영상 H.264 만 받고 그 밖의 offer 는
#   488, TS 24.281 §9.2.2.4.1.1 9)). 그 밖의 이름을 그룹 선호로 두면 문서대로 offer 한 단말이 거절된다. 비교는 대소문자 무시
#   (rtpmap encoding name — RFC 4566 §6), 저장·문서는 이 표기.
SUPPORTED_ENCODINGS = {"audio_encodings": ("AMR-WB",), "video_encodings": ("H264",)}
MAX_DURATION_MAX = 86400
MAX_TRANSMITTERS_MAX = 16                    # CMP 동시 송출 상한(자원) — K6 계약 범위 1..16
RECEPTION_HANG_TIMER_MAX = 3600

# user profile 기본값 = DB 열 기본값. 행이 없으면 MCVideo 자격이 없다(문서 404). N2(`max_affiliations_n2` — <MaxAffiliationsN2>,
#   동시 MCVideo 제휴 그룹 상한)는 MCPTT 의 N2 와 따로다(서비스마다 제휴가 따로다 — mcvideo.md §5.1). 열은 migrate_mcvideo_n2.sql.
PROFILE_DEFAULTS = {"max_video_streams": 1, "max_calls_n6": 1, "max_affiliations_n2": 4}
# ptt_subscriptions.id(MSISDN) → {max_video_streams, max_calls_n6, max_affiliations_n2} — load_shared_data 가 채우고 관리 API 가
#   갱신한다.
MCVIDEO_PROFILES: dict = {}

# service configuration on-network 파라미터 (설정 McVideoServiceConfig.*) — 타이머는 밀리초, 카운터는 횟수.
#   TcTimersCounters = <on-network><anyExt><tc-timers-counters-R14>(TS 24.484 §9.4.2.1 on-network 6 d) — 단말 T100~T104 는
#   초(xs:unsignedByte), 서버 값은 xs:duration. XSD 상 17개 전부 필수라 늘 다 싣는다. 기본값의 정본 = 전송 제어 정의 테이블
#   docs/design/features/mcvideo_tc_defs.yaml(계약 K5 — TS 24.581 §11 기본값, 규격 기본값이 없는 T100~T104·T2 는 CIMS 1 s)이고
#   tests/test_csc_mcvideo.py 가 같은지 대조한다 — 값을 바꾸려면 yaml 부터. 그룹 호의 T1·T5 는 이 문서가 아니라 그룹 문서
#   (on-network-hang-timer·on-network-reception-hang-timer)가 정본이다(§11.1.3) — 여기 T1·T5 는 1:1 호 값이다.
#   ResourcePriority 는 MCPTT 네임스페이스를 재사용한다(TS 24.281 §6.2.8.1.16) — MCPTT ServiceConfig 와 같은 값.
_SERVICE_CONFIG_PARAM_DEFAULTS = {
    "TcTimersCounters": {
        "T100-transmission-request": 1, "T101-transmission-end-request": 1, "T102-queue-position-request": 1,
        "T103-receive-media-request": 1, "T104-receive-media-release": 1,
        "private-call-hang-timer": 30000, "T2-transmission-idle": 1000, "T3-transmission-revoke": 1000,
        "T4-transmission-granted": 1000, "reception-hang-time": 30000, "T6-reception-granted": 1000,
        "T11-stream-reception-idle": 10000,
        "C2-transmission-idle": 10, "C4-transmission-granted": 3, "C6-reception-granted": 3,
        "C7-reception-accpeted": 2, "C11-media-receivers": 4,
    },
    "ResourcePriority": {"Namespace": "mcpttp", "Emergency": "15", "ImminentPeril": "8", "Normal": "0"},
}
# XSD 가 xs:unsignedByte 인 요소(나머지 T* 는 xs:duration, C* 는 unsignedByte).
_TC_UBYTE_SECONDS = ("T100-transmission-request", "T101-transmission-end-request", "T102-queue-position-request",
                     "T103-receive-media-request", "T104-receive-media-release")
SERVICE_CONFIG_PARAMS: dict = {}


def _esc(v) -> str:
    return _html.escape(str(v if v is not None else ''), quote=True)


def _xs_duration_sec(sec) -> str:
    try:
        v = max(0, int(sec))
    except (TypeError, ValueError):
        v = 0
    return f"PT{v}S"


def _xs_duration_ms(ms) -> str:
    try:
        v = max(0, int(ms))
    except (TypeError, ValueError):
        v = 0
    return f"PT{v // 1000}S" if v % 1000 == 0 else f"PT{v / 1000:g}S"


def apply_config(config: dict) -> None:
    """설정 McVideoServiceConfig.*·McVideoUeConfig.* 적용 — services.mcptt.apply_config 가 부른다(SIGUSR1 리로드 포함)."""
    global SERVICE_CONFIG_PARAMS, UE_CONFIG_PARAMS
    v = (config or {}).get('McVideoServiceConfig') or {}
    if not isinstance(v, dict):
        logger.log_error("[CMS] McVideoServiceConfig 가 객체가 아님 — 기본값 사용")
        v = {}
    SERVICE_CONFIG_PARAMS = v
    u = (config or {}).get('McVideoUeConfig') or {}
    UE_CONFIG_PARAMS = u if isinstance(u, dict) else {}


def _svc_param(section: str, key: str):
    cur = SERVICE_CONFIG_PARAMS.get(section) if isinstance(SERVICE_CONFIG_PARAMS.get(section), dict) else {}
    v = (cur or {}).get(key)
    if v is None or (isinstance(v, str) and not v.strip()):
        return _SERVICE_CONFIG_PARAM_DEFAULTS[section][key]
    return v


# MCVideo UE configuration 의 단말 상한(TS 24.484 §9.2.2.1 <common> — 설정 McVideoUeConfig.*). 한 단말이 동시에 갖는 개별 호(Nc10)·
#   그룹 호(Nc4)·그룹 호의 동시 송출(Nc5) 수. CIMS 1차 범위는 영상 호 하나·송출 하나(mcvideo.md §7 D8)라 기본 1.
UE_CONFIG_PARAMS: dict = {}
_UE_CONFIG_DEFAULTS = {"MaxSimulCallNc10": 1, "MaxSimulCallNc4": 1, "MaxSimulTransNc5": 1}


def _ue_param(key: str) -> int:
    try:
        return max(1, int(UE_CONFIG_PARAMS.get(key) or _UE_CONFIG_DEFAULTS[key]))      # xs:positiveInteger
    except (TypeError, ValueError):
        return _UE_CONFIG_DEFAULTS[key]


# ── 그룹 속성 (DB ↔ dict) ──────────────────────────────────────────────────────────────────────────────

def _split_list(v) -> list:
    if v is None:
        return []
    if isinstance(v, (list, tuple)):
        items = v
    else:
        items = str(v).split(',')
    return [s for s in (str(x).strip() for x in items) if s]


def attrs_from_row(row: dict) -> dict:
    """mcvideo_group_attrs 행 → 속성 dict (GROUP_ATTR_DEFAULTS 와 같은 키)."""
    d = dict(GROUP_ATTR_DEFAULTS)
    d.update({
        "invite_members": bool(row.get('invite_members') or 0),
        "max_duration_sec": int(row.get('max_duration_sec') if row.get('max_duration_sec') is not None
                                else GROUP_ATTR_DEFAULTS['max_duration_sec']),
        "max_transmitters": int(row.get('max_transmitters') or GROUP_ATTR_DEFAULTS['max_transmitters']),
        "audio_encodings": _split_list(row.get('audio_encodings')) or list(GROUP_ATTR_DEFAULTS['audio_encodings']),
        "video_encodings": _split_list(row.get('video_encodings')) or list(GROUP_ATTR_DEFAULTS['video_encodings']),
        "video_resolutions": (row.get('video_resolutions') or None),
        "video_frame_rate": (row.get('video_frame_rate') or None),
        "reception_hang_timer_sec": int(row.get('reception_hang_timer_sec')
                                        if row.get('reception_hang_timer_sec') is not None
                                        else GROUP_ATTR_DEFAULTS['reception_hang_timer_sec']),
        "min_number_to_start": int(row.get('min_number_to_start') or 0),
        "group_priority": (int(row['group_priority']) if row.get('group_priority') is not None else None),
        "protect_media": bool(row.get('protect_media') or 0),
        "protect_transmission_control": bool(row.get('protect_transmission_control') or 0),
        "allow_conference_state": bool(row.get('allow_conference_state')
                                       if row.get('allow_conference_state') is not None else 1),
    })
    return d


_ATTR_SELECT = (
    "SELECT g.mcptt_group_id AS mcptt_group_id, a.invite_members, a.max_duration_sec, a.max_transmitters, "
    "a.audio_encodings, a.video_encodings, a.video_resolutions, a.video_frame_rate, a.reception_hang_timer_sec, "
    "a.min_number_to_start, a.group_priority, a.protect_media, a.protect_transmission_control, "
    "a.allow_conference_state "
    "FROM mcvideo_group_attrs a JOIN ptt_groups g ON g.id = a.group_id"
)


def load_group_attrs(cur, mcptt_group_id: Optional[str] = None) -> Optional[dict]:
    """mcptt_group_id → 속성 dict. 표가 없으면(마이그레이션 전) None — 호출자는 MCVideo 그룹이 없는 것으로 다룬다."""
    try:
        if mcptt_group_id is None:
            cur.execute(_ATTR_SELECT)
        else:
            cur.execute(_ATTR_SELECT + " WHERE g.mcptt_group_id=%s", (mcptt_group_id,))
        return {r['mcptt_group_id']: attrs_from_row(r) for r in cur.fetchall()}
    except Exception as e:
        logger.log_info(f"mcvideo_group_attrs load skipped (pre-migration?): {e}")
        return None


# N2 열(migrate_mcvideo_n2.sql)이 있는가 — 한 번 보이면 기억한다(운용 중 마이그레이션을 적용해도 다음 조회가 집는다).
_N2_COLUMN = {'present': False}


def n2_column_present(cur) -> bool:
    if _N2_COLUMN['present']:
        return True
    try:
        cur.execute("SHOW COLUMNS FROM mcvideo_user_profile LIKE 'max_affiliations_n2'")
        _N2_COLUMN['present'] = bool(cur.fetchall())
    except Exception:
        return False
    return _N2_COLUMN['present']


def profile_columns(cur) -> str:
    """SELECT 열 목록 — N2 열이 없으면(마이그레이션 전) 빼고 읽는다(그 회선의 N2 는 기본값)."""
    return "max_video_streams, max_calls_n6" + (", max_affiliations_n2" if n2_column_present(cur) else "")


def profile_of_row(r: dict) -> dict:
    """mcvideo_user_profile 행 → 자격 dict (빠진 열·NULL 은 기본값)."""
    return {k: int(r.get(k) or PROFILE_DEFAULTS[k]) for k in PROFILE_DEFAULTS}


def load_user_profiles(cur) -> bool:
    """mcvideo_user_profile 전부 → MCVIDEO_PROFILES. 표가 없으면 False(자격 0건)."""
    try:
        cur.execute(f"SELECT ptt_id, {profile_columns(cur)} FROM mcvideo_user_profile")
        rows = cur.fetchall()
    except Exception as e:
        logger.log_info(f"mcvideo_user_profile load skipped (pre-migration?): {e}")
        MCVIDEO_PROFILES.clear()
        return False
    MCVIDEO_PROFILES.clear()
    for r in rows:
        MCVIDEO_PROFILES[r['ptt_id']] = profile_of_row(r)
    logger.log_info(f"Loaded {len(MCVIDEO_PROFILES)} MCVideo user profiles")
    return True


def write_group_attrs(cur, group_pk: int, attrs: Optional[dict]) -> None:
    """그룹 한 건의 MCVideo 속성을 쓴다 — attrs None = MCVideo 서비스 끔(행 삭제), dict = 켬(없는 키는 기존값·기본값)."""
    if attrs is None:
        cur.execute("DELETE FROM mcvideo_group_attrs WHERE group_id=%s", (group_pk,))
        return
    cur.execute("SELECT * FROM mcvideo_group_attrs WHERE group_id=%s", (group_pk,))
    row = cur.fetchone()
    cur_attrs = attrs_from_row(row) if row else dict(GROUP_ATTR_DEFAULTS)
    cur_attrs.update({k: v for k, v in attrs.items() if k in GROUP_ATTR_DEFAULTS and v is not None})
    # None 을 "생략" 으로 쓰는 열(해상도·프레임률·그룹 우선순위)은 명시 None 도 반영한다.
    for k in ("video_resolutions", "video_frame_rate", "group_priority"):
        if k in attrs:
            cur_attrs[k] = attrs[k]
    vals = (
        1 if cur_attrs['invite_members'] else 0, int(cur_attrs['max_duration_sec']), int(cur_attrs['max_transmitters']),
        ",".join(_split_list(cur_attrs['audio_encodings'])), ",".join(_split_list(cur_attrs['video_encodings'])),
        cur_attrs['video_resolutions'], cur_attrs['video_frame_rate'], int(cur_attrs['reception_hang_timer_sec']),
        int(cur_attrs['min_number_to_start']), cur_attrs['group_priority'],
        1 if cur_attrs['protect_media'] else 0, 1 if cur_attrs['protect_transmission_control'] else 0,
        1 if cur_attrs['allow_conference_state'] else 0,
    )
    cur.execute(
        "INSERT INTO mcvideo_group_attrs (group_id, invite_members, max_duration_sec, max_transmitters, "
        "audio_encodings, video_encodings, video_resolutions, video_frame_rate, reception_hang_timer_sec, "
        "min_number_to_start, group_priority, protect_media, protect_transmission_control, allow_conference_state, "
        "update_time) VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, NOW()) "
        "ON DUPLICATE KEY UPDATE invite_members=VALUES(invite_members), max_duration_sec=VALUES(max_duration_sec), "
        "max_transmitters=VALUES(max_transmitters), audio_encodings=VALUES(audio_encodings), "
        "video_encodings=VALUES(video_encodings), video_resolutions=VALUES(video_resolutions), "
        "video_frame_rate=VALUES(video_frame_rate), reception_hang_timer_sec=VALUES(reception_hang_timer_sec), "
        "min_number_to_start=VALUES(min_number_to_start), group_priority=VALUES(group_priority), "
        "protect_media=VALUES(protect_media), protect_transmission_control=VALUES(protect_transmission_control), "
        "allow_conference_state=VALUES(allow_conference_state), update_time=NOW()",
        (group_pk,) + vals)


def validate_attrs(attrs: dict) -> Optional[str]:
    """관리 API·XCAP PUT 공통 범위 검사 — 오류 문구 또는 None."""
    def _int_in(k, lo, hi):
        v = attrs.get(k)
        if v is None:
            return None
        try:
            n = int(v)
        except (TypeError, ValueError):
            return f"{k} is not an integer"
        return None if lo <= n <= hi else f"{k} out of range ({lo}..{hi})"
    for k, lo, hi in (("max_duration_sec", 0, MAX_DURATION_MAX), ("max_transmitters", 1, MAX_TRANSMITTERS_MAX),
                      ("reception_hang_timer_sec", 0, RECEPTION_HANG_TIMER_MAX), ("min_number_to_start", 0, 65535),
                      ("group_priority", 0, 255)):
        err = _int_in(k, lo, hi)
        if err:
            return err
    for k in ("audio_encodings", "video_encodings"):
        if k in attrs and attrs[k] is not None:
            names = _split_list(attrs[k])
            if not names:
                return f"{k} needs at least one encoding name"
            bad = [n for n in names if supported_encoding(k, n) is None]
            if bad:
                return (f"{k}: {', '.join(bad)} is not a codec the server accepts "
                        f"(supported: {', '.join(SUPPORTED_ENCODINGS[k])})")
            attrs[k] = [supported_encoding(k, n) for n in names]          # 정본 표기로
    return None


def supported_encoding(kind: str, name: str) -> Optional[str]:
    """그룹 선호 코덱 이름 → 서버 집행 코덱의 정본 표기(대소문자 무시). 받지 않는 이름은 None."""
    for c in SUPPORTED_ENCODINGS[kind]:
        if c.lower() == (name or '').strip().lower():
            return c
    return None


# ── 관리 API (docs/api/admin_api.md — 그룹 `mcvideo` · PTT 회선 MCVideo 자격) ─────────────────────────────────

SCHEMA_ERROR = {'error': 'schema_not_migrated', 'detail': 'mcvideo tables absent — sql/migrate_mcvideo.sql not applied'}
N2_SCHEMA_ERROR = {'error': 'schema_not_migrated',
                   'detail': 'mcvideo_user_profile.max_affiliations_n2 absent — sql/migrate_mcvideo_n2.sql not applied'}
# user profile 수치 범위 — C9(MaxSimultaneousVideoStreams)는 CMP 가 받는 max_rx_streams 범위(1..16, cmp_media_api.md §7.9),
#   N6(MaxSimultaneousCallsN6)은 규격이 상한을 두지 않아(TS 24.484 §9.3.2.1 xs:unsignedByte) 운영상 무의미한 값만 거른다.
#   N2 는 규격이 상한을 두지 않아(xs:nonNegativeInteger) MCPTT N2 관리 범위와 같은 1..1000.
PROFILE_LIMITS = {"max_video_streams": (1, 16), "max_calls_n6": (1, 16), "max_affiliations_n2": (1, 1000)}
_API_BOOL_ATTRS = ("invite_members", "protect_media", "protect_transmission_control", "allow_conference_state")
_API_TEXT_ATTRS = ("video_resolutions", "video_frame_rate")


def tables_present(cur) -> bool:
    """MCVideo 표가 있는가(마이그레이션 적용). 캐시하지 않는다 — 운영 중에 적용해도 곧바로 쓴다."""
    cur.execute("SHOW TABLES LIKE 'mcvideo_group_attrs'")
    return cur.fetchone() is not None


def api_group_attrs(value):
    """관리 API 그룹 본문의 `mcvideo` → (attrs, 오류). None = MCVideo 서비스 끔 · dict = 켬(준 키만 반영 — 나머지는 기존값, 새 그룹은
    기본값). 키 = GROUP_ATTR_DEFAULTS, 코덱 목록은 배열 또는 쉼표 문자열. 보호 true 는 XCAP PUT 과 같은 이유로 받지 않는다(§7 D7)."""
    if value is None:
        return None, None
    if not isinstance(value, dict):
        return None, "mcvideo must be an object or null"
    unknown = sorted(set(value) - set(GROUP_ATTR_DEFAULTS))
    if unknown:
        return None, f"mcvideo: unknown field(s) {', '.join(unknown)}"
    attrs = dict(value)
    for k in _API_BOOL_ATTRS:
        if k in attrs and not isinstance(attrs[k], bool):
            return None, f"mcvideo.{k} must be a boolean"
    for k in _API_TEXT_ATTRS:
        if k in attrs and attrs[k] is not None:
            if not isinstance(attrs[k], str):
                return None, f"mcvideo.{k} must be a string or null"
            attrs[k] = attrs[k].strip() or None
    for k in ("audio_encodings", "video_encodings"):
        if k in attrs and attrs[k] is not None:
            attrs[k] = _split_list(attrs[k])
    for k in ('protect_media', 'protect_transmission_control'):
        if attrs.get(k):
            return None, f"mcvideo.{k} true needs end-to-end keys (not supported)"
    err = validate_attrs(attrs)
    if err:
        return None, f"mcvideo.{err}"
    return attrs, None


def api_profile(body: dict, current: Optional[dict] = None):
    """관리 API PUT `/users/{pid}/ptt/{msisdn}/mcvideo` 본문 → (자격 dict, 오류). 준 키만 바꾸고 나머지는 current(없으면 기본값)."""
    if not isinstance(body, dict):
        return None, "JSON body required"
    unknown = sorted(set(body) - set(PROFILE_LIMITS))
    if unknown:
        return None, f"unknown field(s) {', '.join(unknown)}"
    out = dict(PROFILE_DEFAULTS, **(current or {}))
    for k, (lo, hi) in PROFILE_LIMITS.items():
        if k not in body:
            continue
        v = body[k]
        if isinstance(v, bool) or not isinstance(v, int):
            return None, f"{k} must be an integer"
        if not lo <= v <= hi:
            return None, f"{k} out of range ({lo}..{hi})"
        out[k] = v
    return out, None


def read_user_profile(cur, ptt_id: str) -> Optional[dict]:
    cur.execute(f"SELECT {profile_columns(cur)} FROM mcvideo_user_profile WHERE ptt_id=%s", (ptt_id,))
    r = cur.fetchone()
    if not r:
        return None
    return profile_of_row(r)


def write_user_profile(cur, ptt_id: str, prof: dict) -> None:
    """자격 쓰기 — N2 열이 없으면(마이그레이션 전) 그 열을 빼고 쓴다. N2 를 바꾸려는 요청은 호출측이 먼저 400 으로 막는다."""
    if n2_column_present(cur):
        cur.execute(
            "INSERT INTO mcvideo_user_profile (ptt_id, max_video_streams, max_calls_n6, max_affiliations_n2, update_time) "
            "VALUES (%s, %s, %s, %s, NOW()) ON DUPLICATE KEY UPDATE max_video_streams=VALUES(max_video_streams), "
            "max_calls_n6=VALUES(max_calls_n6), max_affiliations_n2=VALUES(max_affiliations_n2), update_time=NOW()",
            (ptt_id, int(prof['max_video_streams']), int(prof['max_calls_n6']), int(prof['max_affiliations_n2'])))
        return
    cur.execute(
        "INSERT INTO mcvideo_user_profile (ptt_id, max_video_streams, max_calls_n6, update_time) "
        "VALUES (%s, %s, %s, NOW()) ON DUPLICATE KEY UPDATE max_video_streams=VALUES(max_video_streams), "
        "max_calls_n6=VALUES(max_calls_n6), update_time=NOW()",
        (ptt_id, int(prof['max_video_streams']), int(prof['max_calls_n6'])))


def delete_user_profile(cur, ptt_id: str) -> bool:
    cur.execute("DELETE FROM mcvideo_user_profile WHERE ptt_id=%s", (ptt_id,))
    return cur.rowcount > 0


def set_profile_cache(ptt_id: str, prof: Optional[dict]) -> None:
    """MCVIDEO_PROFILES 갱신 — user profile 문서(404/내용)·IdMS scope·mcvideo_id claim 이 곧바로 따른다."""
    for k in (ptt_id, ptt_id.lstrip('+'), '+' + ptt_id.lstrip('+')):
        MCVIDEO_PROFILES.pop(k, None)
    if prof is not None:
        MCVIDEO_PROFILES[ptt_id] = dict(prof)


# ── 그룹 문서 조각 (TS 24.481 §7.2.2) ─────────────────────────────────────────────────────────────────────

def list_service_xml(attrs: dict) -> str:
    """<list-service> 의 MCVideo 자식 — §7.2.2 MCVideo 목록 순(a 초대 → b 최대 시간 → c·d 보호 → e·f 선호 코덱 → g·h 해상도·프레임률 →
    i~l 실시간 모드 → m 동시 송출 → n 시작 인원 → o 그룹 우선순위 → z7 수신 hang timer). 실시간 모드는 그룹 호 = 비긴급 실시간
    (TS 23.281 §5.5.3 표 5.5.3-1 — QoS 선택용, 호 절차에는 쓰이지 않는다)으로 고정한다."""
    a = dict(GROUP_ATTR_DEFAULTS, **(attrs or {}))
    x = f"\n    <mcpttgi:mcvideo-on-network-invite-members>{'true' if a['invite_members'] else 'false'}" \
        f"</mcpttgi:mcvideo-on-network-invite-members>"
    if int(a['max_duration_sec'] or 0) > 0:
        x += f"\n    <mcpttgi:mcvideo-on-network-maximum-duration>{_xs_duration_sec(a['max_duration_sec'])}" \
             f"</mcpttgi:mcvideo-on-network-maximum-duration>"
    x += f"\n    <mcpttgi:mcvideo-protect-media>{'true' if a['protect_media'] else 'false'}</mcpttgi:mcvideo-protect-media>"
    x += f"\n    <mcpttgi:mcvideo-protect-transmission-control>{'true' if a['protect_transmission_control'] else 'false'}" \
         f"</mcpttgi:mcvideo-protect-transmission-control>"
    for tag, key in (('mcvideo-preferred-audio-encodings', 'audio_encodings'),
                     ('mcvideo-preferred-video-encodings', 'video_encodings')):
        # 저장값에 서버가 받지 않는 이름이 남아 있어도(옛 저장분) 문서에는 싣지 않는다 — 하나도 안 남으면 기본값
        names = [n for n in (supported_encoding(key, x) for x in _split_list(a[key])) if n] or list(GROUP_ATTR_DEFAULTS[key])
        enc = "".join(f'<mcpttgi:encoding name="{_esc(n)}"/>' for n in names)
        x += f"\n    <mcpttgi:{tag}>{enc}</mcpttgi:{tag}>"
    if a.get('video_resolutions'):
        x += f"\n    <mcpttgi:mcvideo-preferred-video-resolutions>{_esc(a['video_resolutions'])}" \
             f"</mcpttgi:mcvideo-preferred-video-resolutions>"
    if a.get('video_frame_rate'):
        x += f"\n    <mcpttgi:mcvideo-preferred-video-frame-rate>{_esc(a['video_frame_rate'])}" \
             f"</mcpttgi:mcvideo-preferred-video-frame-rate>"
    x += "\n    <mcpttgi:mcvideo-non-urgent-real-time-video-mode>true</mcpttgi:mcvideo-non-urgent-real-time-video-mode>"
    x += "\n    <mcpttgi:mcvideo-active-real-time-video-mode>non-urgent-real-time</mcpttgi:mcvideo-active-real-time-video-mode>"
    x += f"\n    <mcpttgi:mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members>{int(a['max_transmitters'])}" \
         f"</mcpttgi:mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members>"
    x += f"\n    <mcpttgi:mcvideo-on-network-minimum-number-to-start>{int(a['min_number_to_start'])}" \
         f"</mcpttgi:mcvideo-on-network-minimum-number-to-start>"
    if a.get('group_priority') is not None:
        x += f"\n    <mcpttgi:mcvideo-on-network-group-priority>{int(a['group_priority'])}" \
             f"</mcpttgi:mcvideo-on-network-group-priority>"
    if int(a['reception_hang_timer_sec'] or 0) > 0:
        x += f"\n    <mcpttgi:on-network-reception-hang-timer>{_xs_duration_sec(a['reception_hang_timer_sec'])}" \
             f"</mcpttgi:on-network-reception-hang-timer>"
    return x


def entry_xml(member_uri: str) -> str:
    """entry 의 MCVideo 자식 — <mcvideo-mcvideo-id uri>(MCVideo 그룹 문서면 필수, §7.2.2). MCVideo ID = MCPTT ID(§7 D1)."""
    return f'\n        <mcpttgi:mcvideo-mcvideo-id uri="{_esc(member_uri)}"/>'


def actions_xml(attrs: dict) -> str:
    """규칙 <cp:actions> 의 MCVideo 자식(§7.2.2 MCVideo actions a~d). 긴급·임박·경보는 1차 범위 밖(mcvideo.md §6 V8)이라 false —
    요소 부재도 false 지만 MCPTT 쪽 값과 헷갈리지 않게 명시한다."""
    a = dict(GROUP_ATTR_DEFAULTS, **(attrs or {}))
    return ("\n          <mcpttgi:mcvideo-allow-emergency-call>false</mcpttgi:mcvideo-allow-emergency-call>"
            "\n          <mcpttgi:mcvideo-allow-emergency-alert>false</mcpttgi:mcvideo-allow-emergency-alert>"
            "\n          <mcpttgi:mcvideo-allow-imminent-peril-call>false</mcpttgi:mcvideo-allow-imminent-peril-call>"
            f"\n          <mcpttgi:mcvideo-on-network-allow-conference-state>"
            f"{'true' if a['allow_conference_state'] else 'false'}</mcpttgi:mcvideo-on-network-allow-conference-state>")


def service_xml() -> str:
    """<supported-services> 의 MCVideo <service> — enabler = MCVideo ICSI, <group-media> 에 <mcvideo-video-media>(§7.2.2)."""
    return (f'\n     <oxe:service enabler="{ICSI_MCVIDEO}">'
            "\n      <oxe:group-media>"
            "\n       <mcpttgi:mcvideo-video-media/>"
            "\n      </oxe:group-media>"
            "\n     </oxe:service>")


def parse_group_attrs(ls, ns: dict):
    """XCAP PUT 문서의 MCVideo 몫 → (지원 여부, 속성 dict).

    지원 여부 = <supported-services> 에 enabler 가 MCVideo ICSI 인 <service> 가 있는가(TS 24.481 §7.2.8 MCVideo 그룹 문서 조건).
    반환 (True, attrs) = MCVideo 켬 · (None, {}) = 문서가 MCVideo 를 말하지 않음(갱신이면 기존 상태 유지 — 전환기 규칙, mcvideo.md
    §8: MCVideo 를 모르는 단말의 PUT 이 서비스를 지우지 않게 한다. 끄기는 관리 API 몫, V7 에서 규격 의미(부재 = 끔)로 바꾼다).
    ValueError = 값 오류(400)."""
    has_service = any((s.get('enabler') or '').strip() == ICSI_MCVIDEO
                      for s in ls.findall('.//{urn:oma:xml:xdm:extensions}service'))
    if not has_service:
        return None, {}

    def _t(tag):
        n = ls.find(f'gi:{tag}', ns)
        return n.text.strip() if (n is not None and n.text is not None) else None

    def _b(tag):
        t = _t(tag)
        return None if t is None else t.lower() == 'true'

    def _i(tag):
        t = _t(tag)
        if t is None:
            return None
        try:
            return int(t)
        except ValueError:
            raise ValueError(f'{tag} is not an integer')

    def _dur(tag):
        t = _t(tag)
        if t is None:
            return None
        from services.mcptt import parse_xs_duration
        v = parse_xs_duration(t)
        if v is None:
            raise ValueError(f'{tag} is not an xs:duration')
        return v

    def _enc(tag):
        n = ls.find(f'gi:{tag}', ns)
        if n is None:
            return None
        return [(e.get('name') or '').strip() for e in n.findall('gi:encoding', ns) if (e.get('name') or '').strip()]

    attrs = {
        "invite_members": _b('mcvideo-on-network-invite-members'),
        "max_duration_sec": _dur('mcvideo-on-network-maximum-duration'),
        "protect_media": _b('mcvideo-protect-media'),
        "protect_transmission_control": _b('mcvideo-protect-transmission-control'),
        "audio_encodings": _enc('mcvideo-preferred-audio-encodings'),
        "video_encodings": _enc('mcvideo-preferred-video-encodings'),
        "max_transmitters": _i('mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members'),
        "min_number_to_start": _i('mcvideo-on-network-minimum-number-to-start'),
        "reception_hang_timer_sec": _dur('on-network-reception-hang-timer'),
        "allow_conference_state": None,
    }
    if _t('mcvideo-preferred-video-resolutions') is not None:
        attrs['video_resolutions'] = _t('mcvideo-preferred-video-resolutions') or None
    if _t('mcvideo-preferred-video-frame-rate') is not None:
        attrs['video_frame_rate'] = _t('mcvideo-preferred-video-frame-rate') or None
    if _t('mcvideo-on-network-group-priority') is not None:
        attrs['group_priority'] = _i('mcvideo-on-network-group-priority')
    cs = ls.find('.//{urn:ietf:params:xml:ns:common-policy}actions/gi:mcvideo-on-network-allow-conference-state', ns)
    if cs is not None and cs.text is not None:
        attrs['allow_conference_state'] = cs.text.strip().lower() == 'true'
    # MCVideo 그룹이려면 <service> 에 <group-media><mcvideo-video-media/> 가 있어야 한다(TS 24.481 §7.2.8 a)~e)).
    for svc in ls.findall('.//{urn:oma:xml:xdm:extensions}service'):
        if (svc.get('enabler') or '').strip() == ICSI_MCVIDEO and svc.find(f'.//{{{NS_GI}}}mcvideo-video-media') is None:
            raise ValueError('MCVideo <service> needs <group-media><mcvideo-video-media/>')
    # 멤버의 <mcvideo-mcvideo-id> — MCVideo ID 는 MCPTT ID 와 같은 값이다(단일 MC 서비스 신원). 다른 값은 담을 수 없어 받지 않는다.
    from services.mcptt import _uri_eq as _same_id
    for e in ls.findall('.//{urn:oma:xml:poc:list-service}entry'):
        mv = e.find(f'{{{NS_GI}}}mcvideo-mcvideo-id')
        if mv is not None and (mv.get('uri') or '').strip() and not _same_id(mv.get('uri'), e.get('uri')):
            raise ValueError(f"mcvideo-mcvideo-id of {e.get('uri')} differs from the entry uri (single MC service ID)")
    # 규칙 action mcvideo-allow-*(긴급 호·경보·임박 위험)는 지원하지 않는다 — true 를 받아 false 로 되돌려 내지 않고 거절한다.
    for act in ls.findall('.//{urn:ietf:params:xml:ns:common-policy}actions'):
        for el in act:
            name = el.tag.rsplit('}', 1)[-1]
            if name in ('mcvideo-allow-emergency-call', 'mcvideo-allow-emergency-alert', 'mcvideo-allow-imminent-peril-call') \
                    and (el.text or '').strip().lower() == 'true':
                raise ValueError(f'{name} true is not supported')
    # 보호 true 는 E2E(GMK) 가 설 때까지 받지 않는다 — true 로 저장하면 규격 단말이 GMK 없이 호를 열지 못한다(§7 D7).
    for k in ('protect_media', 'protect_transmission_control'):
        if attrs.get(k):
            raise ValueError(f'mcvideo-{k.replace("_", "-")} true needs end-to-end keys (not supported)')
    given = {k: v for k, v in attrs.items() if v is not None}
    err = validate_attrs(given)
    if err:
        raise ValueError(err)
    attrs.update(given)                       # 선호 코덱 이름은 정본 표기로 바뀌어 온다
    return True, attrs


# ── CMS 문서 ────────────────────────────────────────────────────────────────────────────────────────────

def profile_of(msisdn: str) -> Optional[dict]:
    """PTT 회선의 MCVideo 자격 행 — ptt_subscriptions.id 표기('+' 유무)를 가리지 않는다. 없으면 None."""
    if not msisdn:
        return None
    for k in (msisdn, msisdn.lstrip('+'), '+' + msisdn.lstrip('+')):
        if k in MCVIDEO_PROFILES:
            return MCVIDEO_PROFILES[k]
    return None


def has_profile(msisdn: str) -> bool:
    return profile_of(msisdn) is not None


def get_user_profile_xml(user_uri: str):
    """MCVideo user profile 문서 (TS 24.484 §9.3.2, ns urn:3gpp:ns:mcvideo:user-profile:1.0) → (xml, etag). 자격 행이 없으면 (None, None).

    구조 = §9.3.2.1 의 필수 요소(Name 선택·Status·Common·OnNetwork·ruleset) — MCPTT user profile 생성기(services.mcptt
    .get_user_profile_xml)와 같은 원천·같은 규칙:
      - 그룹 = 이 사용자가 멤버인 **MCVideo 그룹**(GROUPS 의 mcvideo 속성) → <OnNetwork><MCVideoGroupInfo> 하나에 그룹 하나
        (MCVideoGroupInfoType = MCVideo-Group-ID 시퀀스). 암시적 제휴 <ImplicitAffiliations> = 그중 관리자가 이 멤버에게
        암시적 제휴로 정한 그룹(멤버 implicit_affiliation — MCPTT 문서와 같은 표시, 그룹 = 서비스 집합) — 참여 기능이 서비스
        인가 때 이 목록에 MCVideo 제휴를 기록한다(TS 24.281 §8.2.2.2.15, CSP _ApplyImplicitMcVideoAffiliations). chat 은
        합류가 곧 affiliation 이라(§7 D5) 이 목록이 없어도 쓸 수 있다.
      - 상한 = mcvideo_user_profile(MaxSimultaneousVideoStreams·N6·N2 — N2 는 MCPTT 와 따로, 기본 4) + UserProfile.*(Priority·조직명·
        참여자 유형·언어 — MCPTT 문서와 같은 설정).
      - 인가(ruleset) = 1차 범위(그룹 호) 밖의 개시 인가는 false — 1:1·긴급·임박·경보·원격 회수·ambient viewing·ad hoc
        (mcvideo.md §6 V8). 그룹 호 개시 인가는 user profile 요소가 아니다(TS 24.281 §9.2.1.3.1.1 3)).
      - 긴급 대상 entry 는 §9.3.2.1 8e ii~iv·9f 가 "shall include one" 이라 인가와 무관하게 싣는다 — UseCurrentlySelectedGroup·
        LocallyDetermined + 폴백 uri(첫 MCVideo 그룹 > 본인). PrivateCallList 는 "zero or more" 라 비운다(1:1 미지원).
      - <RemoteGroupSelectionURIList> 는 §9.3.2.1 9g 가 싣게 하지만 원격 선택 권한이 없으므로 빈 목록(XSD 는 entry 0 개 허용 —
        본문 "one or more" 와 어긋나 스키마를 따른다, mcvideo.md §9)."""
    from services import mcptt as _m
    user = _m.USERS.get(user_uri)
    if not user:
        return None, None
    prof = profile_of(user.get('msisdn', ''))
    if prof is None:
        return None, None

    display_name = user.get('name') or user_uri
    lang = str(_m._user_profile_cfg('Language'))
    org = str(_m._user_profile_cfg('MissionCriticalOrganization') or _m._ue_init_cfg('Name') or 'CIMS')
    ptype = str(_m._user_profile_cfg('ParticipantType'))
    prio = int(_m._user_profile_cfg('Priority'))
    n2 = max(1, int(prof.get('max_affiliations_n2') or PROFILE_DEFAULTS['max_affiliations_n2']))
    n6 = max(1, int(prof.get('max_calls_n6') or PROFILE_DEFAULTS['max_calls_n6']))
    streams = max(1, int(prof.get('max_video_streams') or PROFILE_DEFAULTS['max_video_streams']))

    def et(tag, uri, name=None, info=None, index=None):
        # <entry> 는 "index" 를 가져야 한다(§9.3.2.1 «The <entry> elements: 2)» — 목록 안에서 유일)
        a = f' index="{index}"' if index is not None else ''
        a += f' entry-info="{_esc(info)}"' if info else ''
        dn = f'<display-name>{_esc(name)}</display-name>' if name else ''
        return f'<{tag}{a}><uri-entry>{_esc(uri)}</uri-entry>{dn}</{tag}>'

    my_groups = sorted(((g_uri, g) for g_uri, g in _m.GROUPS.items()
                        if g.get('mcvideo') is not None
                        and any(_m._uri_eq(mb.get('uri'), user_uri) for mb in g.get('members', []))),
                       key=lambda x: x[0])
    fallback_group = my_groups[0][0] if my_groups else user_uri
    fallback_name = my_groups[0][1].get('display_name') if my_groups else None
    eg_entry = et('entry', fallback_group, fallback_name, 'UseCurrentlySelectedGroup', index=1)
    pr_entry = et('entry', user_uri, None, 'LocallyDetermined', index=1)
    # <ProSeUserID-entry> = <DiscoveryGroupID>(3옥텟 hex)·<User-Info-ID>(6옥텟 hex)·"index" 필수(§9.3.2.1 «The <ProSeUserID-entry>
    #   elements»). off-network(ProSe) 미지원이라 영값.
    prose = ('<ProSeUserID-entry index="1"><DiscoveryGroupID>000000</DiscoveryGroupID>'
             '<User-Info-ID>000000000000</User-Info-ID></ProSeUserID-entry>')

    common = f'<UserAlias><alias-entry index="1" xml:lang="{_esc(lang)}">{_esc(display_name)}</alias-entry></UserAlias>'
    common += et('MCVideoUserID', user_uri)
    common += ('<PrivateCall><PrivateCallList/>'
               f'<EmergencyCall><MCVideoPrivateRecipient>{pr_entry}{prose}</MCVideoPrivateRecipient></EmergencyCall>'
               '</PrivateCall>')
    gc = f'<MaxSimultaneousCallsN6>{n6}</MaxSimultaneousCallsN6>'
    gc += f'<EmergencyCall><MCVideoGroupInitiation>{eg_entry}</MCVideoGroupInitiation></EmergencyCall>'
    gc += f'<ImminentPerilCall><MCVideoGroupInitiation>{eg_entry}</MCVideoGroupInitiation></ImminentPerilCall>'
    gc += f'<EmergencyAlert>{eg_entry}</EmergencyAlert>'
    gc += f'<Priority>{prio}</Priority>'
    common += f'<MCVideo-group-call>{gc}</MCVideo-group-call>'
    common += f'<ParticipantType>{_esc(ptype)}</ParticipantType>'
    common += f'<MissionCriticalOrganization>{_esc(org)}</MissionCriticalOrganization>'

    on = ''.join(f'<MCVideoGroupInfo>{et("MCVideo-Group-ID", g_uri, g.get("display_name"))}</MCVideoGroupInfo>'
                 for g_uri, g in my_groups)
    on += f'<MaxAffiliationsN2>{n2}</MaxAffiliationsN2>'
    implicit = ''.join(et('entry', g_uri, g.get('display_name'), index=i) for i, (g_uri, g) in enumerate(
        [(g_uri, g) for g_uri, g in my_groups
         if any(_m._uri_eq(mb.get('uri'), user_uri) and mb.get('implicit_affiliation') for mb in g.get('members', []))], 1))
    if implicit:
        on += f'<ImplicitAffiliations>{implicit}</ImplicitAffiliations>'
    on += f'<MaxSimultaneousVideoStreams>{streams}</MaxSimultaneousVideoStreams>'
    on += f'<PrivateEmergencyAlert>{pr_entry}</PrivateEmergencyAlert>'
    on += '<RemoteGroupSelectionURIList/>'

    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<mcvideo-user-profile xmlns="{NS_USER_PROFILE}"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  XUI-URI="{_esc(user_uri)}" user-profile-index="1">
  <Name xml:lang="{_esc(lang)}">{_esc(display_name)}</Name>
  <Status>true</Status>
  <Common index="1">{common}</Common>
  <OnNetwork index="1">{on}</OnNetwork>
  <cp:ruleset>
    <cp:rule id="mcvideo-user-authorisation">
      <cp:actions>
        <allow-private-call>false</allow-private-call>
        <allow-emergency-group-call>false</allow-emergency-group-call>
        <allow-emergency-private-call>false</allow-emergency-private-call>
        <allow-imminent-peril-call>false</allow-imminent-peril-call>
        <allow-activate-emergency-alert>false</allow-activate-emergency-alert>
        <allow-revoke-transmit>false</allow-revoke-transmit>
        <anyExt>
          <allow-request-remote-initiated-ambient-viewing>false</allow-request-remote-initiated-ambient-viewing>
          <allow-request-locally-initiated-ambient-viewing>false</allow-request-locally-initiated-ambient-viewing>
          <allow-adhoc-group-call>false</allow-adhoc-group-call>
        </anyExt>
      </cp:actions>
    </cp:rule>
  </cp:ruleset>
</mcvideo-user-profile>"""
    return xml, _m._content_etag(xml)


def get_service_config_xml():
    """MCVideo service configuration 문서 (TS 24.484 §9.4.2) — 시스템 전역 1건 → (xml, etag).

    구조 = <service-configuration-info> › <service-configuration-params domain> › <on-network>(시퀀스: <signalling-protection>
    선택 → <protection-between-mcvideo-servers> 선택 → emergency·imminent-peril·normal resource-priority 필수 →
    <anyExt><tc-timers-counters-R14>). <protection-between-mcvideo-servers> 의 둘도 없으면 true(§9.4.2.7 NOTE 1)라 false 를 명시한다
    (서버 간 연동 없음).
    <signalling-protection> 의 둘은 **없으면 true**(XSD default, TS 24.281 §6.6.2.1·§6.6.3.1)라 false 를 명시한다 — CIMS 는
    MCVideo 시그널링 XML 보호(CSK)를 하지 않는다(구간 보호는 SIP TLS, mcvideo.md §7 D7). R14 요소 이름은 XSD 를 따른다
    (본문 "C7-reception-accepted"·"T103-receive-media-requset" 는 XSD 와 다르다 — mcvideo.md §9)."""
    from services import mcptt as _m
    from services import access_services as _access_services
    domain = (_access_services.ptt_domain(_m.PROVISIONING) or _m.IDMS_DOMAIN).strip()
    tc = []
    for k, dflt in _SERVICE_CONFIG_PARAM_DEFAULTS["TcTimersCounters"].items():
        v = _svc_param("TcTimersCounters", k)
        if k in _TC_UBYTE_SECONDS or k.startswith("C"):
            try:
                val = max(0, min(255, int(v)))                 # xs:unsignedByte
            except (TypeError, ValueError):
                val = dflt
        else:
            val = _xs_duration_ms(v)
        tc.append(f"          <{k}>{val}</{k}>")
    ns = _svc_param("ResourcePriority", "Namespace")

    def _rp(elem, key):
        return (f"      <{elem}>\n"
                f"        <resource-priority-namespace>{_esc(ns)}</resource-priority-namespace>\n"
                f"        <resource-priority-priority>{_esc(_svc_param('ResourcePriority', key))}</resource-priority-priority>\n"
                f"      </{elem}>")

    nl = "\n"
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<service-configuration-info xmlns="{NS_SERVICE_CONFIG}">
  <service-configuration-params domain="{_esc(domain)}">
    <on-network>
      <signalling-protection>
        <confidentiality-protection>false</confidentiality-protection>
        <integrity-protection>false</integrity-protection>
      </signalling-protection>
      <protection-between-mcvideo-servers>
        <allow-signalling-protection>false</allow-signalling-protection>
        <allow-transmission-control-protection>false</allow-transmission-control-protection>
      </protection-between-mcvideo-servers>
{_rp('emergency-resource-priority', 'Emergency')}
{_rp('imminent-peril-resource-priority', 'ImminentPeril')}
{_rp('normal-resource-priority', 'Normal')}
      <anyExt>
        <tc-timers-counters-R14>
{nl.join(tc)}
        </tc-timers-counters-R14>
      </anyExt>
    </on-network>
  </service-configuration-params>
</service-configuration-info>"""
    return xml, _m._content_etag(xml)


def get_ue_config_xml(user_uri: str, ue_id: str = ''):
    """MCVideo UE configuration 문서 (TS 24.484 §9.2 — AUID org.3gpp.mcvideo.ue-config) → (xml, etag). 자격 행이 없으면 (None, None).

    XCAP URI = `…/org.3gpp.mcvideo.ue-config/users/sip:<MCVideo ID>/<MCS UE ID>`(§9.2.1A) — master 문서에서 만들고 `<mcvideo-UE-id>`
    를 그 단말의 UE ID 로 채운다. 구조·요소 이름은 §9.2.2.3 XSD 를 따른다: 본문(§9.2.2.1)은 `MCVideo-…`, XSD 는 `MCVIDEO-…` 이고
    XSD 는 루트 끝 `<anyExt>`·`<Prioritized-MCVIDEO-Group>` 항목 하나 이상·`<Relayed-MCVIDEO-Group>` 을 필수로 둔다(mcvideo.md §9).
      - <common>: 단말 상한 Nc10·Nc4·Nc5(설정 McVideoUeConfig.*) + 그룹 우선순위 목록 = 이 사용자가 멤버인 MCVideo 그룹과 그 그룹
        우선순위(없으면 본인 URI·0 한 줄 — XSD 가 한 줄 이상을 요구한다).
      - <on-network>: IPv6Preferred false · Relay-Service false(UE-to-network relay 미지원) — 그래서 Relayed 그룹은 빈 값 한 줄."""
    from services import mcptt as _m
    from services import access_services as _access_services
    user = _m.USERS.get(user_uri)
    if not user or profile_of(user.get('msisdn', '')) is None:
        return None, None
    domain = (_access_services.ptt_domain(_m.PROVISIONING) or _m.IDMS_DOMAIN).strip()
    my_groups = sorted(((g_uri, g) for g_uri, g in _m.GROUPS.items()
                        if g.get('mcvideo') is not None
                        and any(_m._uri_eq(mb.get('uri'), user_uri) for mb in g.get('members', []))),
                       key=lambda x: x[0])
    prios = [(g_uri, _m._priority_type(g.get('priority'), 0)) for g_uri, g in my_groups] or [(user_uri, 0)]
    prio_xml = ''.join(f'<MCVIDEO-Group-Priority><MCVIDEO-Group-ID>{_esc(u)}</MCVIDEO-Group-ID>'
                       f'<group-priority-hierarchy>{p}</group-priority-hierarchy></MCVIDEO-Group-Priority>' for u, p in prios)
    ue_id = (ue_id or '').strip()
    ue = f'<mcvideo-UE-id><Instance-ID-URN>{_esc(ue_id)}</Instance-ID-URN></mcvideo-UE-id>' if ue_id else ''
    xml = (f'<?xml version="1.0" encoding="UTF-8"?>'
           f'<mcvideo-UE-configuration xmlns="{NS_UE_CONFIG}" domain="{_esc(domain)}" XUI-URI="{_esc(user_uri)}">'
           f'{ue}'
           f'<common>'
           f'<MCVIDEO-Private-Call><Max-Simul-Call-Nc10>{_ue_param("MaxSimulCallNc10")}</Max-Simul-Call-Nc10></MCVIDEO-Private-Call>'
           f'<MCVIDEO-Group-Call><Max-Simul-Call-Nc4>{_ue_param("MaxSimulCallNc4")}</Max-Simul-Call-Nc4>'
           f'<Max-Simul-Trans-Nc5>{_ue_param("MaxSimulTransNc5")}</Max-Simul-Trans-Nc5>'
           f'<Prioritized-MCVIDEO-Group>{prio_xml}</Prioritized-MCVIDEO-Group></MCVIDEO-Group-Call>'
           f'</common>'
           f'<on-network><IPv6Preferred>false</IPv6Preferred><Relay-Service>false</Relay-Service>'
           f'<Relayed-MCVIDEO-Group><MCVIDEO-Group-ID></MCVIDEO-Group-ID><Relay-Service-Code></Relay-Service-Code></Relayed-MCVIDEO-Group>'
           f'</on-network>'
           f'<anyExt/>'
           f'</mcvideo-UE-configuration>')
    return xml, _m._content_etag(xml)
