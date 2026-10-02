import os
import re
import socket
import select
import json
import glob
import time
import uuid
import datetime
import jwt
import asyncio
import hashlib
import base64
import secrets as _secrets
from typing import Dict, Tuple, Optional, List

from httpsrv.handler import HandlerArgs, HandlerResult, BodyData
from util.log_util import Logger
from services.idms_storage import IdmsStorage
from services import logger as _logger
from services import subscriptions as _subs      # 가입 테이블 레지스트리(volte/voip/ptt — sip_service_model.md §2-9)
from services import mcvideo as _mcvideo        # MCVideo 설정 평면(그룹 문서 조각·user profile·service config — mcvideo.md §5.1)
from services import idms_keys as _idms_keys    # IdMS 토큰 서명 키(RS256 — TS 33.180 B.2.2.1)

# --- Configuration & Data ---
# 토큰 서명 (TS 33.180 B.2.2.1 — access token 은 JSON web digital signature 프로파일 RFC 7515 · OIDC Core §15.1 — OP 는 ID token
#   을 RS256 으로 서명한다). 서명 = RS256(`services/idms_keys` 의 RSA 키, 공개 키는 `GET /idms/jwks`) — 단말과 분리 배치된 리소스
#   서버가 공개 키로 검증한다. `IdMs.SigningAlg=HS256` 은 공유 비밀 MAC 로 서명하는 옛 방식(서명 키를 못 읽을 때도 이쪽으로 선다).
#   검증은 RS256 을 받고, HS256 토큰은 `IdMs.AcceptHs256`(전환기 — 판올림 전에 나간 토큰) 일 때만 받는다.
# HS256 시크릿 — 기본값 = 프로세스 시작 시 임의 생성(예측 불가). `IdMs.JwtSecret` 로 고정한다(미설정 시 재기동마다 HS256 토큰 무효).
SECRET_KEY = _secrets.token_urlsafe(32)
SIGNING_ALG_CONFIG = "RS256"      # IdMs.SigningAlg
ACCEPT_HS256 = True               # IdMs.AcceptHs256
# KMS master secret — 가입자별 키 material 파생용(HKDF). 구 구현은 전 사용자 동일 고정 hex 였음.
#   ⚠ 본 파생은 가입자별 **구조적 프로비저닝**(UserDecryptKey/SSK/PVT 가 사용자마다 다름)을 제공하나,
#   참값 ECCSI/SAKKE(RFC 6507/6508)는 pairing 암호 라이브러리가 필요한 후속 과제다(E2E 암호화 도입 시).
KMS_MASTER_SECRET = _secrets.token_bytes(32)
# ── IdMS scope 카탈로그 (TS 33.180 Annex B.4.2.2 — MC 서비스별 authorization scope) ──
#   토큰 scope = 요청 ∩ 카탈로그 (RFC 6749 §3.3: 모르는 값은 제외하고 토큰 응답 `scope` 로 허가분을 알림).
#   리소스 서버는 자기 scope 를 검사한다(B.10 — `require_scope`, IdMs.ScopeEnforcement).
#   MCVideo 4종은 MCVideo 이용 자격(mcvideo_user_profile 행)이 있는 사용자에게만 준다(grant_scope 의 mcptt_id 인자).
#   CIMS 앱은 로그인 시 전부 grant 받고 AccountManager 가 refresh 로 용도별(provisioning / MC 서비스) 토큰을 좁혀 발급받는다.
SCOPE_OPENID       = "openid"
SCOPE_PROVISIONING = "cims:provisioning"      # 자체 — 디바이스 부트스트랩(/provisioning/*)
SCOPE_PTT_SERVICE  = "3gpp:mc:ptt_service"
SCOPE_DATA_SERVICE = "3gpp:mc:data_service"
SCOPE_PTT_GMS      = "3gpp:mc:ptt_group_management_service"
SCOPE_PTT_CMS      = "3gpp:mc:ptt_config_management_service"
SCOPE_PTT_KMS      = "3gpp:mc:ptt_key_management_service"
SCOPE_DATA_GMS     = "3gpp:mc:data_group_management_service"
SCOPE_DATA_CMS     = "3gpp:mc:data_config_management_service"
SCOPE_DATA_KMS     = "3gpp:mc:data_key_management_service"
SCOPE_VIDEO_SERVICE = "3gpp:mc:video_service"
SCOPE_VIDEO_GMS    = "3gpp:mc:video_group_management_service"
SCOPE_VIDEO_CMS    = "3gpp:mc:video_config_management_service"
SCOPE_VIDEO_KMS    = "3gpp:mc:video_key_management_service"
SCOPE_MC_SERVICES  = (SCOPE_PTT_SERVICE, SCOPE_PTT_GMS, SCOPE_PTT_CMS, SCOPE_PTT_KMS,
                      SCOPE_DATA_SERVICE, SCOPE_DATA_GMS, SCOPE_DATA_CMS, SCOPE_DATA_KMS)
SCOPE_VIDEO_SERVICES = (SCOPE_VIDEO_SERVICE, SCOPE_VIDEO_GMS, SCOPE_VIDEO_CMS, SCOPE_VIDEO_KMS)
SCOPE_CATALOG      = frozenset((SCOPE_OPENID, SCOPE_PROVISIONING) + SCOPE_MC_SERVICES + SCOPE_VIDEO_SERVICES)
# 전환기 별칭 — 구 단일 scope(TS 33.179 표기)는 MC 서비스 scope 8개 전체로 확장한다(종전에 그 하나가 열어 주던
#   범위와 동일). 토큰에는 확장분과 함께 구 문자열도 실린다(요청 scope 를 문자열 대조하는 단말 호환).
#   별칭 제거 = 우리 앱·협력업체가 신 이름으로 옮긴 뒤 별도 결정 (mcx_identity_scope.md §5).
SCOPE_LEGACY_MCPTT = "3gpp:mcptt:ptt_server"
SCOPE_ALIASES      = {SCOPE_LEGACY_MCPTT: SCOPE_MC_SERVICES}
# 리소스 서버 scope 검사 모드 (IdMs.ScopeEnforcement): off=검사 없음 / log=would-deny 로그만 / enforce=403.
SCOPE_ENFORCEMENT  = "enforce"
# 클라이언트 등록 검사(TS 33.180 B.3 — 클라이언트는 IdM 서버에 등록돼 있고 client_id·redirect_uri 가 등록 값과 같아야 한다)와
#   인증·토큰 요청의 필수 파라미터 검사(B.4.2.2·B.4.2.4) 모드 — IdMs.ClientEnforcement: off=검사 없음 / log=would-reject 로그만 /
#   enforce=400. 등록 저장소 = IdMs.Clients([{ClientId, RedirectUris[]}]) → client_id → 허용 redirect_uri 집합.
CLIENT_ENFORCEMENT = "log"
IDMS_CLIENTS: dict = {}
ACR_PASSWORD = "3gpp:acr:password"      # 최소 연동 요건의 ACR 값(B.4.2.2 acr_values) — CIMS IdMS 는 이 방식만 한다

# IdMS 신원 값 — 설정이 비면 apply_config 가 PTT 도메인(Provisioning.Services.ptt.domain)에서 유도한다.
#   Issuer = IdMs.Issuer > McpttServer.PublicUrl(URL 형, TS 33.180 B.2.1.2) > idms.<Domain>
#   Domain = IdMs.Domain > PTT 도메인 > 코드 기본값 / KmsUri = IdMs.KmsUri > kms.<Domain>
_IDMS_DOMAIN_DEFAULT = "mcptt.com"
IDMS_ISSUER = "idms." + _IDMS_DOMAIN_DEFAULT
KMS_URI = "kms." + _IDMS_DOMAIN_DEFAULT
IDMS_DOMAIN = _IDMS_DOMAIN_DEFAULT
KMS_CLIENT_REQ_URL = "http://localhost:4421/keymanagement/identity/v1/init"
USERS = {}            # tel:+msisdn → {password,...} (XCAP/profile 키 = MCPTT ID)
# 사용자 MCPTT 프로파일 (ptt_user_profile) — ptt_subscriptions.id(MSISDN) → {allow_*, emergency_group_*}.
#   user-profile XCAP 문서(TS 24.484) 산출 + admin PUT 캐시 갱신. 부재 = DEFAULT_USER_PROFILE.
PTT_PROFILES = {}
DEFAULT_USER_PROFILE = {
    "allow_emergency_call": True,
    "allow_emergency_alert": True,
    "allow_adhoc_call": True,
    "emergency_group_mode": "DedicatedGroup",
    "emergency_group_id": None,
    "allow_emergency_private_call": True,
    "private_emergency_mode": "LocallyDetermined",
    "emergency_private_recipient": None,
    "allow_ambient_listening": False,   # CIMS 확장 cims:allow-ambient-listening — PTT 그룹 호 청취 자격 (관제사, 기본 없음)
    "allow_create_group": False,        # CIMS 확장 allow-create-group — GMS XCAP 그룹 생성 자격 (관제사, 기본 없음)
    "allow_non_ack_users_info": False,  # TS 24.484 anyExt allow-to-receive-non-acknowledged-users-information —
                                        #   그룹 호 개시자로서 미응답 멤버 INFO 수신 자격 (TS 24.379 §6.3.3.3, 부재 = false)
    # 해제 인가 (TS 24.484 ruleset, migrate_ptt_user_profile_cancel_authz.sql) — 부재 시 값은 USER_PROFILE_OPT_ABSENT_SQL
    "allow_cancel_group_emergency": False,  # allow-cancel-group-emergency — 그룹의 진행 중 긴급 상태 해제
                                            #   (TS 24.379 §6.3.3.1.13.4 local policy = 개시자 ∨ 이 값)
    "allow_cancel_imminent_peril": True,    # allow-cancel-imminent-peril — 임박 위험 해제 (§6.3.3.1.13.6, 개시자 예외 없음)
    "allow_cancel_emergency_alert": True,   # allow-cancel-emergency-alert — 긴급 경보 취소 (§6.3.3.1.13.3)
                                            #   = 부재 시 allow_emergency_alert 값
    # 개별 호 인가 (TS 24.484 ruleset, migrate_ptt_user_profile_private_call.sql) — 요소가 없으면 false 로 읽히므로 문서에 늘 싣는다
    "allow_private_call": True,                # allow-private-call — 개별 호 발신 (TS 24.379 §11.1.1.3.1.1 — false 면 403 107)
    "allow_private_call_to_any_user": True,    # allow-private-call-to-any-user — PrivateCallList 밖 상대에게도 (false 면 목록 밖 403 144)
    "allow_private_call_participation": True,  # allow-private-call-participation — 개별 호 착신 참가 (§11.1.1.3.2 — false 면 403 127)
}
# 마이그레이션으로 뒤에 붙은 프로파일 컬럼(선택 컬럼)의 **부재 시 값** — 컬럼 미적용 DB 의 SELECT 대체식이자 행·키가 없을 때의 값
#   (user_profile_opt_default). 여기 없는 선택 컬럼은 상수 0(자격 없음). allow_cancel_emergency_alert 은 발령 인가
#   (allow_emergency_alert)를 잇는다 — 컬럼 도입 전 문서가 발령 값을 <allow-cancel-emergency-alert> 로 냈다.
USER_PROFILE_OPT_ABSENT_SQL = {
    "allow_cancel_imminent_peril": "1",
    "allow_cancel_emergency_alert": "allow_emergency_alert",
    "allow_private_call": "1",
    "allow_private_call_to_any_user": "1",
    "allow_private_call_participation": "1",
}
# 선택 컬럼 전부(적재·관리 API 가 같은 목록을 쓴다) → 그 열을 더하는 마이그레이션.
USER_PROFILE_OPT_COLS = {
    "allow_ambient_listening": "sql/migrate_ptt_ambient_listening.sql",
    "allow_create_group": "sql/migrate_ptt_allow_create_group.sql",
    "allow_non_ack_users_info": "sql/migrate_ptt_non_ack_users_info.sql",
    "allow_cancel_group_emergency": "sql/migrate_ptt_user_profile_cancel_authz.sql",
    "allow_cancel_imminent_peril": "sql/migrate_ptt_user_profile_cancel_authz.sql",
    "allow_cancel_emergency_alert": "sql/migrate_ptt_user_profile_cancel_authz.sql",
    "allow_private_call": "sql/migrate_ptt_user_profile_private_call.sql",
    "allow_private_call_to_any_user": "sql/migrate_ptt_user_profile_private_call.sql",
    "allow_private_call_participation": "sql/migrate_ptt_user_profile_private_call.sql",
}


def user_profile_opt_default(col, prof=None) -> bool:
    """선택 컬럼 col 의 부재 시 값 — prof = 같은 프로파일의 나머지 값(대체식이 다른 컬럼을 가리키면 그 값)."""
    expr = USER_PROFILE_OPT_ABSENT_SQL.get(col, "0")
    if expr in ("0", "1"):
        return expr == "1"
    return bool((prof or {}).get(expr, DEFAULT_USER_PROFILE.get(expr, False)))


# IdMS 로그인 자격 — CIMS 로그인 ID(인증) ↔ MCPTT ID(서비스 신원) 분리.
#   login_id(예 test001) → {password, user_id, mcptt_id(tel:+msisdn 파생), name}
LOGIN_ACCOUNTS = {}
GROUPS = {}
TOKENS = {}
GROUP_DIR = None

# IDMS Storage
storage = IdmsStorage()

logger = Logger()

# TTL 설정 (config에서 읽음, 기본값은 프로덕션 값)
AUTH_CODE_TTL = 60               # 60초
ACCESS_TOKEN_TTL = 3600          # 1시간
REFRESH_TOKEN_TTL = 7 * 24 * 3600  # 7일

# S4: service-config (TS 24.484 §8.4) — 시스템 전역 문서 1건. 가입자별 오버라이드는 규격 근거가 없어 두지 않는다.
#   인가(1:1·긴급·경보·그룹 생성)는 이 문서의 요소가 아니다 — user profile ruleset(§8.3.2.7)·그룹 문서(TS 24.481)가 정본.
#   값의 SoT 두 곳:
#     · DB `mcptt_service_config` 단일 행(콘솔 편집) — N2(user-profile MaxAffiliationsN2 기본값)·N6(user-profile
#       MaxSimultaneousCallsN6 — 관제/그 밖 두 값, user_max_calls_n6)·broadcast-group 계층 수.
#       아래 기본값은 그 행(N6 는 그 열 — migrate_mcptt_n6.sql)이 없을 때의 폴백. 키는 DB 컬럼·관리 API JSON 과 같은 언더스코어 표기.
#     · 설정 `ServiceConfig.*`(SERVICE_CONFIG_PARAMS) — on-network emergency-call·transmit-time·fc-timers-counters·Resource-Priority.
#       기본값 = CMP floor 타이머 기본값·CSP fan-out 의 mcpttp 서열·긴급 그룹 호 시한 없음.
SERVICE_CONFIG_DEFAULTS = {
    "max_affiliations_n2": 10,
    # N6 = 동시 그룹 호 상한(TS 24.484 §8.3.2.1 8)e)i) <MaxSimultaneousCallsN6>, TS 24.379 §10.1.1.3.1.1 5) — 넘으면 486 + 103).
    #   사용자마다의 값이고 판정은 «관제 = 그 회선의 사람에게 역할 배정이 있다» 하나다(user_max_calls_n6 — CSP 집행과 같은 판정).
    "max_calls_n6": 5,
    "max_calls_n6_dispatch": 10,
    "num_levels_group_hierarchy": 3,
    "num_levels_user_hierarchy": 3,
}
# mcptt_service_config 의 선택 열(마이그레이션이 더하는 것) — 열이 없는 DB 는 코드 기본값을 쓴다.
SERVICE_CONFIG_OPT_COLS = ("max_calls_n6", "max_calls_n6_dispatch")
# on-network 규격 파라미터 (config ServiceConfig.*). 타이머는 밀리초, 카운터는 횟수.
#   FcTimersCounters·TransmitTime 은 floor 제어 서버(CMP)의 **정본**이다 — CSP 가 이 문서를 받아(TS 24.484 Annex A.2.3, 내부 API
#   /internal/mcptt/service-config) 그룹 세션 개시 때 PTT_GROUP_ADD floor_timers 로 CMP 에 싣는다(T1·T2·T3·T7·T8·T20·C7·C20).
#   CMP 설정 Floor*Sec 는 문서를 못 받았을 때의 폴백이다. T15·T16·T17·T55·T56·C17·C55·C56 은 CMP 가 쓰지 않는 기능
#   (MBMS·사전 수립 세션)의 값이라 규격 예시값(§A)을 싣는다.
#   ResourcePriority 는 RFC 8101 `mcpttp` 네임스페이스 서열(TS 24.379 §6.2.8.1.15) — CSP fan-out 과 같다.
#   EmergencyCall.GroupTimeLimit 은 controlling MCPTT function(CSP)의 TNG2(진행 중 긴급 그룹 호 타이머, TS 24.379 §6.3.3.1.16·
#   부속서 F 타이머 표)다 — CSP 가 같은 문서의 <emergency-call><group-time-limit> 을 읽는다. 0(기본) = 요소 생략 = TNG2 미가동.
#   PrivateCall·AdhocGroupCall 은 개별 호·애드혹 그룹 호의 세션 타이머 값이다(§8.4.2.1 on-network 2)·13)d)) — 그룹 문서가 없는
#   호라 T4·최대 시간을 이 문서에서 얻는다(TS 24.380 표 11.1.3-1 T4 · TS 24.379 §6.3.8.2 · §17.4.2.2 13)). CSP 가 받아
#   개별·애드혹 세션의 T4 를 CMP floor_timers 로 싣고 최대 시간을 센다. 시간 값 0 = 요소 생략 = 그 타이머 미가동.
_SERVICE_CONFIG_PARAM_DEFAULTS = {
    "FcTimersCounters": {
        "T1-end-of-rtp-media": 4000, "T3-stop-talking-grace": 3000, "T7-floor-idle": 0, "T8-floor-revoke": 1000,
        "T11-end-of-RTP-dual": 4000, "T12-stop-talking-dual": 30000, "T15-conversation": 30000,
        "T16-map-group-to-bearer": 500, "T17-unmap-group-to-bearer": 200, "T20-floor-granted": 1000,
        "T55-connect": 2000, "T56-disconnect": 2000,
        "C7-floor-idle": 3, "C17-unmap-group-to-bearer": 3, "C20-floor-granted": 3, "C55-connect": 3, "C56-disconnect": 3,
    },
    "ResourcePriority": {"Namespace": "mcpttp", "Emergency": "15", "ImminentPeril": "8", "Normal": "0"},
    # transmit-time/time-limit = on-network 그룹 발언 시간 한도(§8.4.2.1 on-network 4)) — floor 제어 서버 T2(TS 24.380 §6.3.4.4)
    "TransmitTime": {"TimeLimit": 30000},
    # emergency-call/group-time-limit = 진행 중 긴급 그룹 호 시한(§8.4.2.1 on-network) — controlling 기능 TNG2. 0 = 요소 생략
    "EmergencyCall": {"GroupTimeLimit": 0},
    # private-call = 개별 호(§8.4.2.7 on-network 3)~5)) — hang-time = T4(규격 기본 30초, TS 24.380 표 11.1.3-1), 최대 시간 =
    #   발언권 제어가 있는 호 / 없는 호(full-duplex)의 «maximum of duration of private call»(TS 24.379 §6.3.8.2 2)).
    "PrivateCall": {"HangTime": 30000, "MaxDurationWithFloorControl": 3600000, "MaxDurationWithoutFloorControl": 3600000},
    # anyExt/adhoc-group-call = 애드혹 그룹 호(§8.4.2.7 on-network 47)~51)) — 요소가 없으면 «애드혹 미지원»(§8.4.2.6)이라
    #   필수 자식 allow-adhoc-group-call-support·max-no-participants 와 함께 늘 싣는다. hang-time = T4, broadcast-hang-time =
    #   일제 애드혹 호의 T4(TS 24.380 표 11.1.3-1), max-duration-of-call = TNG3(TS 24.379 §17.4.2.2 13)).
    "AdhocGroupCall": {"AllowSupport": True, "MaxNoParticipants": 64, "HangTime": 30000, "BroadcastHangTime": 30000,
                       "MaxDurationOfCall": 3600000},
}
SERVICE_CONFIG_PARAMS = {}
_SERVICE_CONFIG_PARAMS_LOADED = False   # 첫 적재 뒤 재적재(SIGUSR1)에서만 변경 통지
# DB 사본 — load_shared_data 가 채우고 admin PUT 이 갱신한다(update_service_config_cache).
SERVICE_CONFIG = dict(SERVICE_CONFIG_DEFAULTS)

CSP_NOTIFY_IP = "127.0.0.1"
CSP_NOTIFY_PORT = 4421
# PSP (PTT 시그널링) — 별도 인스턴스 분리 시 사용. CSP 와 동일하면 broadcast 가
# 동일 endpoint 1번만 호출 (자동 dedup).
PSP_NOTIFY_IP = ""           # ""=PSP 미설정 (legacy: CSP 만 사용)
PSP_NOTIFY_PORT = 4421

# 자동 프로비저닝(/provisioning/me, android_ue_provisioning.md §3) —
#   서비스 kind 별 시그널링 서버/도메인. host 빈값이면 요청 Host(=UE 가 접속한 IP) 사용(올인원 기본).
#   다중 노드면 host 를 CSP/PSP 대표(VIP) 주소로 지정.
PROVISIONING = {}            # config Provisioning: {"Services":{"volte":{name,host,port,tcp_port,tls_port,transport,domain,…}, "voip":{…}, "ptt":{...}}}
_RECORDINGS_DIR = ''         # 녹취 영역(Recording.Dir) — 통합 이력 조회(/provisioning/history)·녹취 범위 판정 원천
_STATE_DIR = ''              # 상태 영역(State.Dir) — 통합 이력의 진행 중 세션
_DB_CONFIG = None            # CimsDatabase (가입자 라이브 조회용)
_OAM_CONFIG: dict = {}       # Recording.OamUrl / Fm.OamIp — 통합 이력 PTT 창 조회의 OAM 세션 인덱스 프록시용
_MCPTT_PORT = 4430           # csc McpttServer.Port (응답 csc.port)
# 단말이 도달하는 MCPTT 서비스(IdMS/GMS/CMS/KMS) 공개 base URL — **단일 정본**.
#   McpttServer.PublicUrl 설정값(정규화: 스킴 필수·후행 / 제거). 비면 요청 Host 유도(올인원).
#   단말에 주소를 알려주는 모든 자리(ue-init-config XCAP-root-URI · openid-configuration ·
#   authreq 폼 action · /provisioning/me csc · CSP 에 주는 xcap-root)가 이 값에서 파생된다.
_MCPTT_PUBLIC_URL = ''

# ue-init-config 규격 파라미터값 (config UeInitConfig.* — 주소류는 토폴로지 유도라 여기 없음).
#   빈 dict 면 코드 기본값(_UE_INIT_DEFAULTS) — 설정 섹션이 없는 배포본(업그레이드 직후)도 기본값 문서를 낸다.
#   Timers = 단말 발언권 참여자 타이머(초, xs:unsignedByte — TS 24.380 표 11.1.1-1). T100·T101 은 재전송 간격이고 재전송 총
#   시간이 6초 미만이어야 한다(NOTE 1 — T100 shall · NOTE 2 — T101 should). 카운터 C100·C101 기본 3회와 곱해 6초 미만이 되는
#   초 단위 값은 1 이다. T103 = T1(service configuration T1-end-of-rtp-media 4초)과 같게(표 «Should be equal to T1»),
#   T132 = 규격 기본 2초, T104 = 규격 기본값이 없는 사이트 값.
UE_INIT_CONFIG = {}
_UE_INIT_DEFAULTS = {
    "Name": "CIMS",
    "Timers": {"T100": 1, "T101": 1, "T103": 4, "T104": 4, "T132": 2},
    "Hplmn": {"Plmn": "", "McpttConRef": "internet", "McCommonCoreConRef": "internet", "McIdConRef": "internet"},
    "HttpProxy": "",
    "TlsMutualAuthentication": False,
    "IntegrityProtection": False,
    "ConfidentialityProtection": False,
    "GroupCreationXui": "",
    "ServiceDetails": {"Mcptt": {"Enable": True, "ServerUri": ""},
                       "McVideo": {"Enable": False, "ServerUri": ""},
                       "McData": {"Enable": False, "ServerUri": ""}},
}
_UE_INIT_LAST_GOOD = {}      # base_url → (xml, etag): 설정값이 문서를 깨뜨렸을 때 유지할 마지막 정상 문서
_UE_INIT_LOADED = False      # 첫 적재 뒤 재적재(SIGUSR1)에서만 변경 통지(UE_INIT_CONFIG_CHANGED)


def _request_host(args, default_port: bool = True) -> str:
    """요청 Host 헤더 (없으면 IdMS 도메인 유도). PublicUrl 미설정 시의 폴백 재료."""
    fallback = f"{IDMS_DOMAIN}:{_MCPTT_PORT}" if default_port else IDMS_DOMAIN
    return (args.headers.get('host') or args.headers.get('Host') or fallback).strip()


def public_base_url(args) -> str:
    """단말이 도달하는 MCPTT 서비스 공개 base URL (후행 '/' 없음) — 단일 정본.

    McpttServer.PublicUrl 이 있으면 그 값, 없으면 요청 Host 유도(올인원 기본).
    리버스 프록시·VIP·다중 노드 구성에서는 PublicUrl 을 명시해야 단말이 받는 주소가
    실제 도달 주소와 일치한다."""
    if _MCPTT_PUBLIC_URL:
        return _MCPTT_PUBLIC_URL
    return f"https://{_request_host(args)}"


def public_host_port(args):
    """공개 base URL 의 (host, port) — /provisioning/me 의 csc 블록용."""
    rest = public_base_url(args).split('://', 1)[-1].split('/', 1)[0]
    if ':' in rest:
        host, _, port = rest.rpartition(':')
        if host and port.isdigit():
            return host, int(port)
    return rest, _MCPTT_PORT


def public_xcap_root(args) -> str:
    """xcap-diff NOTIFY 의 xcap-root · MCData FD URL base (후행 '/' 포함).

    CSP 가 내부 API 로 조회하는 값. PublicUrl 이 있으면 그 값, 없으면 CSP 요청의 Host
    에서 host 만 취해 McpttServer.Port 를 붙인다 — CSP 는 admin 포트(4421)로 오므로
    포트를 그대로 쓰면 안 된다(단일 노드에서만 정확, 다중 노드는 PublicUrl 필수)."""
    if _MCPTT_PUBLIC_URL:
        base = _MCPTT_PUBLIC_URL
    else:
        host = _request_host(args, default_port=False)
        if ':' in host:
            host = host.rpartition(':')[0]          # admin 포트 제거
        base = f"https://{host}:{_MCPTT_PORT}"
    return base + '/'


# IdMS 규격 로그인 폼(TS 24.482 §6.3.1) 입력칸 이름 — 외부 단말/SDK 가 헤드리스로 채울 때 찾는 name.
IDMS_FORM_LOGIN_FIELD = "username"
IDMS_FORM_PASSWORD_FIELD = "password"
# authreq redirect_uri 허용 목록 (RFC 6749 §3.1.2.3 정확 일치). 비면 전부 허용.
IDMS_REDIRECT_URI_ALLOW = []


def _group_uri(gid: str) -> str:
    """mcptt_group_id 식별자 → GMS 그룹 URI.
    E.164 숫자면 tel:+{gid}, '+' 로 시작하면 그대로 tel:, 그 외(g001 등)는 tel:{gid}."""
    if not gid:
        return "tel:"
    if gid.startswith('+'):
        return f"tel:{gid}"
    if gid.isdigit():
        return f"tel:+{gid}"
    return f"tel:{gid}"


def _users_has_title(cur) -> bool:
    """users.title(직함) 존재 여부 — migrate_users_title.sql 미적용 DB 허용."""
    cur.execute(
        "SELECT COUNT(*) AS cnt FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='users' AND COLUMN_NAME='title'"
    )
    return cur.fetchone()['cnt'] > 0


def resolve_idms_identity(idms_config: dict, ptt_domain: str, public_url: str):
    """IdMS 신원 3종 (issuer, domain, kms_uri) 유도 — 설정 명시값 > 유도값.
    Issuer: IdMs.Issuer > McpttServer.PublicUrl(URL 형, TS 33.180 B.2.1.2 "IdM 서버의 URL") > idms.<domain>
    Domain: IdMs.Domain > Provisioning.Services.ptt.domain > 코드 기본값 / KmsUri: IdMs.KmsUri > kms.<domain>"""
    idms_config = idms_config or {}
    domain = str(idms_config.get('Domain') or ptt_domain or _IDMS_DOMAIN_DEFAULT).strip()
    issuer = str(idms_config.get('Issuer') or public_url or f"idms.{domain}").strip()
    kms_uri = str(idms_config.get('KmsUri') or f"kms.{domain}").strip()
    return issuer, domain, kms_uri


def parse_idms_clients(items) -> dict:
    """IdMs.Clients([{ClientId, RedirectUris}]) → {client_id: {redirect_uri…}}. RedirectUris 는 목록 또는 쉼표·줄바꿈 구분 문자열."""
    out: dict = {}
    for c in (items or []):
        if not isinstance(c, dict):
            continue
        cid = str(c.get('ClientId') or c.get('client_id') or '').strip()
        uris = c.get('RedirectUris') or c.get('redirect_uris') or []
        if isinstance(uris, str):
            uris = [u for u in (x.strip() for x in uris.replace('\n', ',').split(',')) if u]
        if cid:
            out.setdefault(cid, set()).update(str(u).strip() for u in uris if str(u).strip())
    return out


def apply_config(config):
    """설정 스칼라 값만 모듈 전역에 재적용 — 가입자/그룹 데이터 로드는 하지 않는다.

    기동 시 `load_shared_data()` 가 호출하고, 배포 설정 변경(agent job_update_config →
    SIGUSR1) 시 `csc_app` 의 reload 훅이 다시 호출한다. config_template 의
    `restart:false` (런타임 리로드 가능) 필드가 실제로 재기동 없이 반영되는 경로.
    bind 계열(Server.Port/McpttServer.Port 등 기동 시 소켓에 캡처된 값)은 여기서
    전역만 갱신되고 실제 반영은 재기동이 필요하다."""
    db_config  = config.get('CimsDatabase')
    group_path = config.get('Data', {}).get('Group')
    # UE initial configuration 변경 통지의 비교 기준 — 이 문서는 주소류(PublicUrl·PTT 도메인)도 담으므로 어떤 값도 바꾸기 전에
    #   뜬다. 첫 적재(기동)는 비교하지 않는다(함수 끝).
    global _UE_INIT_LOADED
    _ui_prev = _ue_init_ref_etag() if _UE_INIT_LOADED else None

    # Read IdMs config
    global SECRET_KEY, IDMS_ISSUER, KMS_URI, IDMS_DOMAIN, KMS_CLIENT_REQ_URL
    global AUTH_CODE_TTL, ACCESS_TOKEN_TTL, REFRESH_TOKEN_TTL
    idms_config = config.get('IdMs', {})
    if idms_config.get('JwtSecret'):
        SECRET_KEY = idms_config['JwtSecret']
    else:
        logger.log_error("[IdMS] IdMs.JwtSecret 미설정 — 임의 시크릿 사용(재기동 시 토큰 무효화). "
                         "운영은 IdMs.JwtSecret 설정 권장.")
    global SIGNING_ALG_CONFIG, ACCEPT_HS256
    _alg = str(idms_config.get('SigningAlg') or 'RS256').strip().upper()
    if _alg not in ('RS256', 'HS256'):
        logger.log_error(f"[IdMS] IdMs.SigningAlg='{_alg}' 미지 값 — RS256 으로 동작")
        _alg = 'RS256'
    SIGNING_ALG_CONFIG = _alg
    ACCEPT_HS256 = bool(idms_config.get('AcceptHs256', True))
    if idms_config.get('KmsClientReqUrl'):
        KMS_CLIENT_REQ_URL = idms_config['KmsClientReqUrl']
    if idms_config.get('AuthCodeTtl'):
        AUTH_CODE_TTL = int(idms_config['AuthCodeTtl'])
    if idms_config.get('AccessTokenTtl'):
        ACCESS_TOKEN_TTL = int(idms_config['AccessTokenTtl'])
    if idms_config.get('RefreshTokenTtl'):
        REFRESH_TOKEN_TTL = int(idms_config['RefreshTokenTtl'])
    global SCOPE_ENFORCEMENT
    _enf = str(idms_config.get('ScopeEnforcement') or 'enforce').strip().lower()
    if _enf not in ('off', 'log', 'enforce'):
        logger.log_error(f"[IdMS] IdMs.ScopeEnforcement='{_enf}' 미지 값 — enforce 로 동작")
        _enf = 'enforce'
    SCOPE_ENFORCEMENT = _enf
    # 클라이언트 등록(TS 33.180 B.3) — IdMs.Clients = [{ClientId, RedirectUris[]}], IdMs.ClientEnforcement = off|log|enforce(기본 log)
    global CLIENT_ENFORCEMENT, IDMS_CLIENTS
    _cenf = str(idms_config.get('ClientEnforcement') or 'log').strip().lower()
    if _cenf not in ('off', 'log', 'enforce'):
        logger.log_error(f"[IdMS] IdMs.ClientEnforcement='{_cenf}' 미지 값 — log 로 동작")
        _cenf = 'log'
    CLIENT_ENFORCEMENT = _cenf
    IDMS_CLIENTS = parse_idms_clients(idms_config.get('Clients'))
    if CLIENT_ENFORCEMENT == 'enforce' and not IDMS_CLIENTS:
        logger.log_error("[IdMS] IdMs.ClientEnforcement=enforce 인데 IdMs.Clients 가 비었다 — 모든 인증 요청이 거절된다")

    # 규격 로그인 폼 입력칸 이름 · redirect_uri 허용 목록 (둘 다 리로드 가능 — 다음 요청부터)
    global IDMS_FORM_LOGIN_FIELD, IDMS_FORM_PASSWORD_FIELD, IDMS_REDIRECT_URI_ALLOW
    IDMS_FORM_LOGIN_FIELD = str(idms_config.get('FormLoginField') or 'username').strip() or 'username'
    IDMS_FORM_PASSWORD_FIELD = str(idms_config.get('FormPasswordField') or 'password').strip() or 'password'
    allow = idms_config.get('RedirectUriAllow') or []
    if isinstance(allow, str):                      # 콤마 구분 문자열도 수용
        allow = [a for a in (s.strip() for s in allow.split(',')) if a]
    IDMS_REDIRECT_URI_ALLOW = [str(a).strip() for a in allow if str(a).strip()]

    # ue-init-config 규격 파라미터값 — 문서 ETag 가 내용 파생이라 값이 바뀌면 자동 갱신
    global UE_INIT_CONFIG
    UE_INIT_CONFIG = config.get('UeInitConfig') or {}
    if not isinstance(UE_INIT_CONFIG, dict):
        logger.log_error("[CMS] UeInitConfig 가 객체가 아님 — 기본값 사용")
        UE_INIT_CONFIG = {}
    # service-config on-network 규격 파라미터값 — 같은 규칙(ETag 내용 파생, SIGUSR1 리로드)
    global SERVICE_CONFIG_PARAMS, _SERVICE_CONFIG_PARAMS_LOADED
    _sc_prev = get_service_config_xml(None)[1] if _SERVICE_CONFIG_PARAMS_LOADED else None
    _SERVICE_CONFIG_PARAMS_LOADED = True
    SERVICE_CONFIG_PARAMS = config.get('ServiceConfig') or {}
    if not isinstance(SERVICE_CONFIG_PARAMS, dict):
        logger.log_error("[CMS] ServiceConfig 가 객체가 아님 — 기본값 사용")
        SERVICE_CONFIG_PARAMS = {}
    # 재적재(SIGUSR1)로 문서가 바뀌었으면 CSP 에 알린다 — CSP 가 문서를 다시 받아 floor 값을 CMP 로 전달하고,
    #   cms 구독 단말에 xcap-diff 를 push 한다(admin PUT 과 같은 통지).
    if _sc_prev is not None:
        _sc_now = get_service_config_xml(None)[1]
        if _sc_now != _sc_prev:
            notify_csp("SERVICE_CONFIG_CHANGED", "", "PUT", etag=(_sc_now or "").strip('"'))
    # MCVideo service-config 규격 파라미터값(McVideoServiceConfig.*) — 같은 규칙. 바뀌면 같은 통지로 CSP 가 다시 받는다.
    _mv_prev = _mcvideo.get_service_config_xml()[1] if _sc_prev is not None else None
    _mcvideo.apply_config(config)
    if _mv_prev is not None:
        _mv_now = _mcvideo.get_service_config_xml()[1]
        if _mv_now != _mv_prev:
            notify_csp("SERVICE_CONFIG_CHANGED", "mcvideo", "PUT", etag=(_mv_now or "").strip('"'))
    # user-profile 규격 파라미터값 — 같은 규칙(ETag 내용 파생, SIGUSR1 리로드)
    global USER_PROFILE_CONFIG
    USER_PROFILE_CONFIG = config.get('UserProfile') or {}
    if not isinstance(USER_PROFILE_CONFIG, dict):
        logger.log_error("[CMS] UserProfile 이 객체가 아님 — 기본값 사용")
        USER_PROFILE_CONFIG = {}

    global CSP_NOTIFY_IP, CSP_NOTIFY_PORT, PSP_NOTIFY_IP, PSP_NOTIFY_PORT
    notify_cfg = config.get('CspNotify', {})
    if notify_cfg.get('Ip'):
        CSP_NOTIFY_IP = notify_cfg['Ip']
    if notify_cfg.get('Port'):
        CSP_NOTIFY_PORT = int(notify_cfg['Port'])
    psp_cfg = config.get('PspNotify', {})
    if psp_cfg.get('Ip'):
        PSP_NOTIFY_IP = psp_cfg['Ip']
    if psp_cfg.get('Port'):
        PSP_NOTIFY_PORT = int(psp_cfg['Port'])
    # 목적지가 바뀌면 기존 connected 소켓은 폐기 (다음 notify 에서 새로 연결).
    _reset_notify_socks()
    logger.log_info(f"Notify targets: CSP={CSP_NOTIFY_IP}:{CSP_NOTIFY_PORT} "
                    f"PSP={PSP_NOTIFY_IP or '(unset)'}:{PSP_NOTIFY_PORT}")

    # 자동 프로비저닝(/provisioning/me) — DB 핸들 + 서비스별 시그널링/도메인 매핑 보관.
    global _DB_CONFIG, PROVISIONING, _MCPTT_PORT, _MCPTT_PUBLIC_URL, _RECORDINGS_DIR, _STATE_DIR, _OAM_CONFIG
    _DB_CONFIG = db_config
    # 통합 이력 PTT 창 조회가 OAM 세션 인덱스를 프록시할 때의 접속 설정(dispatch_recordings._oam_base/_http 와 같은 키).
    _OAM_CONFIG = {k: config.get(k) for k in ('Recording', 'Fm') if config.get(k) is not None}
    PROVISIONING = config.get('Provisioning', {}) or {}
    # 사이트 영역 — 배포본은 OAM 이 base oam 사이트 디렉터리에서 유도해 준다(services/site_paths, 비면 단일 루트).
    from services import site_paths as _site
    _RECORDINGS_DIR = _site.recordings_dir(config)
    _STATE_DIR = _site.state_dir(config)
    _mcptt_conf = config.get('McpttServer', {}) or {}
    _MCPTT_PORT = int(_mcptt_conf.get('Port', 4430))
    # 공개 base URL — 스킴 없으면 https 보정, 후행 '/' 제거. 비면 요청 Host 유도(올인원).
    _pub = str(_mcptt_conf.get('PublicUrl') or '').strip().rstrip('/')
    if _pub and '://' not in _pub:
        _pub = 'https://' + _pub
    _MCPTT_PUBLIC_URL = _pub
    logger.log_info(f"MCPTT public base URL: {_MCPTT_PUBLIC_URL or '(요청 Host 유도)'}")

    # IdMS 신원 값 유도 — 비면 PTT 도메인에서 파생(단일 정본). 템플릿 기본값이 비어 있어 배포 overlay 에
    #   실리지 않으므로, 운영자가 콘솔에 명시할 때만 그 값을 쓴다.
    #   PTT 도메인의 정본은 CSP access_services(관리 store 미러) — csc.json 항목은 미러 미도달 시 폴백(services/access_services).
    from services import access_services as _access_services
    _access_services.configure(config)
    _ptt_domain = _access_services.ptt_domain(PROVISIONING)
    IDMS_ISSUER, IDMS_DOMAIN, KMS_URI = resolve_idms_identity(idms_config, _ptt_domain, _MCPTT_PUBLIC_URL)
    logger.log_info(f"IdMS identity: issuer={IDMS_ISSUER} domain={IDMS_DOMAIN} kms={KMS_URI} "
                    f"scope_enforcement={SCOPE_ENFORCEMENT}")

    global GROUP_DIR
    if group_path:
        GROUP_DIR = group_path

    # UE initial configuration(TS 24.484 §7.2)도 변경 구독을 지원한다(§7.2.2.12 → §6.3.13.3) — 재적재로 문서가 바뀌었으면
    #   CSP 에 알린다. CSP 가 cms 구독 단말에 문서 선택자 org.3gpp.mcptt.ue-init-config/users/sip:<MCS UE ID>/<MCS UE ID> 의
    #   xcap-diff NOTIFY(RFC 5875)를 보내고 단말은 문서를 다시 받는다 — service-config 의 SERVICE_CONFIG_CHANGED 와 같은 경로.
    if _ui_prev is not None:
        _ui_now = _ue_init_ref_etag()
        if _ui_now != _ui_prev:
            notify_csp("UE_INIT_CONFIG_CHANGED", "", "PUT", etag=(_ui_now or "").strip('"'))
    _UE_INIT_LOADED = True


def load_shared_data(config):
    # Do not reassign global variables, modify them in place
    USERS.clear()
    GROUPS.clear()

    # 스칼라 설정(IdMs/Notify/Provisioning/GROUP_DIR)은 리로드 가능 단위로 분리.
    apply_config(config)

    # Config keys match csc.json structure
    user_path = config.get('Data', {}).get('User')
    group_path = config.get('Data', {}).get('Group')
    db_config = config.get('CimsDatabase')

    # IdmsStorage: file_store 기반 (Phase 8). 전체 config 전달 (runtime_root 추출용).
    storage.init_db(config)
    logger.log_info("IdmsStorage initialized (file_store)")
    # 토큰 서명 키 — runtime store 의 키를 읽는다(없으면 만든다). 못 읽으면 HS256 으로 선다(로그인은 계속 된다).
    if SIGNING_ALG_CONFIG == 'RS256' and not _idms_keys.init(config):
        logger.log_error("[IdMS] RS256 서명 키를 쓸 수 없다 — HS256(IdMs.JwtSecret)으로 서명한다. "
                         "단말·외부 리소스 서버는 이 토큰의 서명을 검증할 수 없다(TS 33.180 B.2.2.1).")
    
    # Load users: DB primary, file fallback
    db_users_loaded = False
    if db_config:
        try:
            import pymysql, pymysql.cursors
            conn = pymysql.connect(
                host=db_config.get('Host', '127.0.0.1'),
                port=int(db_config.get('Port', 3306)),
                user=db_config.get('User', 'root'),
                password=db_config.get('Password', ''),
                database=db_config.get('Db', 'cims'),
                charset='utf8mb4',
                cursorclass=pymysql.cursors.DictCursor,
            )
            with conn:
                with conn.cursor() as cur:
                    for _kind, table in _subs.tables(cur):
                        # SIP Digest passwd 는 로그인 자격이 아니다(소거됨, sip_access_security.md §4.7 ⑤).
                        #   IdMS 로그인 = users.login_id/passwd(LOGIN_ACCOUNTS) — 여기서는 신원 목록만 싣는다.
                        cur.execute(f"SELECT id FROM {table}")
                        for row in cur.fetchall():
                            uid = row['id']
                            uri = f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}"
                            if uri not in USERS:
                                USERS[uri] = {"password": None, "name": uid, "msisdn": uid,
                                              "profile_etag": "etag_" + uri}
                                logger.log_info(f"Loaded DB User: {uri}")

                    # 사용자 MCPTT 프로파일 (SOS 대상 결정·개시 인가) — 마이그레이션 전이면 스킵
                    try:
                        # allow_ambient_listening / allow_create_group / allow_non_ack_users_info / allow_cancel_* 는 각
                        #   migrate_ptt_*.sql 이후에만 — 컬럼이 없으면 부재 시 값(USER_PROFILE_OPT_ABSENT_SQL, 없으면 상수 0 —
                        #   선행 배포 무해).
                        opt_cols = []
                        for c in USER_PROFILE_OPT_COLS:
                            cur.execute(f"SHOW COLUMNS FROM ptt_user_profile LIKE '{c}'")
                            opt_cols.append(c if cur.fetchone() else f"{USER_PROFILE_OPT_ABSENT_SQL.get(c, '0')} AS {c}")
                        cur.execute(
                            "SELECT ptt_id, allow_emergency_call, allow_emergency_alert, "
                            "allow_adhoc_call, emergency_group_mode, emergency_group_id, "
                            "allow_emergency_private_call, private_emergency_mode, "
                            f"emergency_private_recipient, {', '.join(opt_cols)} "
                            "FROM ptt_user_profile")
                        PTT_PROFILES.clear()
                        for r in cur.fetchall():
                            PTT_PROFILES[r['ptt_id']] = {
                                "allow_emergency_call": bool(r['allow_emergency_call']),
                                "allow_emergency_alert": bool(r['allow_emergency_alert']),
                                "allow_adhoc_call": bool(r['allow_adhoc_call']),
                                "emergency_group_mode": r['emergency_group_mode'],
                                "emergency_group_id": r['emergency_group_id'],
                                "allow_emergency_private_call": bool(r['allow_emergency_private_call']),
                                "private_emergency_mode": r['private_emergency_mode'],
                                "emergency_private_recipient": r['emergency_private_recipient'],
                                **{c: bool(r[c]) for c in USER_PROFILE_OPT_COLS},
                            }
                        logger.log_info(f"Loaded {len(PTT_PROFILES)} user MCPTT profiles")
                    except Exception as pe:
                        logger.log_info(f"ptt_user_profile load skipped (pre-migration?): {pe}")

                    # MCVideo 이용 자격 (mcvideo_user_profile — 행 = 자격, TS 24.484 §9.3). 표 부재는 자격 0건.
                    _mcvideo.load_user_profiles(cur)

                    # MCPTT 시스템 서비스 설정 (TS 24.484 service-config) — 단일 행(id=1).
                    #   행/테이블 부재는 기본값 유지.
                    try:
                        opt = service_config_opt_cols(cur)
                        cur.execute(
                            "SELECT " + ", ".join(("max_affiliations_n2", "num_levels_group_hierarchy",
                                                   "num_levels_user_hierarchy") + opt) +
                            " FROM mcptt_service_config WHERE id=1")
                        row = cur.fetchone()
                        if row:
                            SERVICE_CONFIG.update({k: int(row[k]) for k in
                                                   ("max_affiliations_n2", "num_levels_group_hierarchy",
                                                    "num_levels_user_hierarchy") + opt})
                            logger.log_info(f"Loaded MCPTT service config: {SERVICE_CONFIG}")
                    except Exception as se:
                        logger.log_info(f"mcptt_service_config load skipped (pre-migration?): {se}")

                    # IdMS 로그인 계정(login_id) — MCPTT ID 는 ptt(없으면 volte) msisdn 에서 tel:+ 파생.
                    _load_login_accounts(cur)
            db_users_loaded = True
            logger.log_info(f"Users loaded from DB: {len(USERS)} (login accounts: {len(LOGIN_ACCOUNTS)})")
        except Exception as e:
            logger.log_error(f"DB user load failed, will fall back to files: {e}")

    if not db_users_loaded and user_path:
        logger.log_info(f"Loading users from files (DB unavailable): {user_path}...")
        user_files = glob.glob(os.path.join(user_path, '**', '*.json'), recursive=True)
        for fpath in user_files:
            try:
                with open(fpath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    filename = os.path.basename(fpath).split('.')[0]
                    user_id = filename
                    uri = f"tel:{user_id}" if user_id.startswith('+') else f"tel:+{user_id}"
                    USERS[uri] = {
                        "password": data.get('passwd', 'password123'),
                        "name": data.get('name', 'Unknown User'),
                        "msisdn": user_id,
                        "profile_etag": "etag_" + uri
                    }
                    logger.log_info(f"Loaded File User: {uri}")
            except Exception as e:
                logger.log_error(f"Error loading user {fpath}: {e}")

    # Load groups: DB primary, file fallback
    db_groups_loaded = False
    if db_config:
        try:
            import pymysql, pymysql.cursors
            conn = pymysql.connect(
                host=db_config.get('Host', '127.0.0.1'),
                port=int(db_config.get('Port', 3306)),
                user=db_config.get('User', 'root'),
                password=db_config.get('Password', ''),
                database=db_config.get('Db', 'cims'),
                charset='utf8mb4',
                cursorclass=pymysql.cursors.DictCursor,
            )
            with conn:
                with conn.cursor() as cur:
                    # 행→dict 변환은 단일 그룹 동기화(sync_group_from_db)와 같은 함수를 쓴다 —
                    #   기동 적재와 CRUD 후 갱신이 같은 모양을 보장.
                    cur.execute(_GROUP_SELECT)
                    for row in cur.fetchall():
                        GROUPS[_group_uri(row['mcptt_group_id'])] = _group_row_to_dict(row)
                    # 멤버 목록 + users 테이블에서 이름·직함 조회 (group_id=surrogate → mcptt_group_id JOIN)
                    cur.execute(_member_select_sql(cur) + " ORDER BY g.mcptt_group_id, gm.priority")
                    for row in cur.fetchall():
                        g_uri = _group_uri(row['mcptt_group_id'])
                        if g_uri in GROUPS:
                            GROUPS[g_uri]['members'].append(_member_row_to_dict(row))
                    # MCVideo 서비스 속성 (mcvideo_group_attrs — 표 부재 = 마이그레이션 전, MCVideo 그룹 없음)
                    for gid, attrs in (_mcvideo.load_group_attrs(cur) or {}).items():
                        g_uri = _group_uri(gid)
                        if g_uri in GROUPS:
                            GROUPS[g_uri]['mcvideo'] = attrs
                    for uri in GROUPS:
                        logger.log_info(f"Loaded DB Group: {uri} ({len(GROUPS[uri]['members'])} members)")
            db_groups_loaded = True
            logger.log_info(f"Groups loaded from DB: {len(GROUPS)}")
        except Exception as e:
            logger.log_error(f"DB group load failed, will fall back to files: {e}")

    # JSON 파일 그룹 로드 (DB 미연결 시 폴백)
    if not db_groups_loaded and group_path:
        logger.log_info(f"Loading groups from files (DB unavailable): {group_path}...")
        group_files = glob.glob(os.path.join(group_path, '*.json'))
        for fpath in group_files:
            try:
                with open(fpath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    group_id = os.path.basename(fpath).split('.')[0]
                    uri = f"tel:{group_id}" if group_id.startswith('+') else f"tel:+{group_id}"
                    members = []
                    for m in data.get('users', []):
                        m_id = m.get('id', '')
                        if m_id:
                            m_uri = f"tel:{m_id}" if m_id.startswith('+') else f"tel:+{m_id}"
                            members.append({
                                "uri": m_uri, "name": m_uri,
                                "role": m.get('role', 'participant'),
                                "priority": m.get('priority', 5), "joined_at": "",
                                "title": m.get('title', '')
                            })
                    GROUPS[uri] = {
                        "display_name": data.get('name', 'Group'),
                        "etag": data.get('etag', f"etag_{uri}"),
                        "created_by": data.get('created_by', ''),
                        "created_at": data.get('created_at', ''),
                        "members": members
                    }
                    logger.log_info(f"Loaded File Group: {uri}")
            except Exception as e:
                logger.log_error(f"Error loading group {fpath}: {e}")

def _load_login_accounts(cur) -> None:
    """users.login_id/passwd → LOGIN_ACCOUNTS 전량 교체. 기동 적재와 admin API 변경 후 갱신이 같은 코드를 쓴다.

    `mcptt_id` = MC 서비스 신원 — **PTT 가입(ptt_subscriptions)이 있는 사람만** 가진다(TS 24.482 §4.1·TS 33.180 B.4.2.2: 토큰의
    scope·MC service ID 는 그 사용자가 인가된 MC 서비스로 정해진다). 전화 전용(volte·voip) 계정은 None — MC scope·`mcptt_id`
    claim 을 받지 못하고 CMS·GMS·KMS 를 열 수 없다. `line_id` = 그 사람의 첫 회선(ptt → volte → voip)의 tel URI — 프로비저닝
    (/provisioning/*)이 사람을 찾는 신원이다(token_line_id)."""
    # 회선 = ptt → volte → voip 순의 첫 가입(voip 테이블은 있을 때만 — services.subscriptions 프로브)
    voip_col = ("(SELECT id FROM voip_subscriptions WHERE user_id=u.id LIMIT 1) voip "
                if _subs.has_table(cur, 'voip') else "NULL voip ")
    cur.execute(
        "SELECT u.id uid, u.login_id, u.passwd, u.name, "
        "(SELECT id FROM ptt_subscriptions WHERE user_id=u.id LIMIT 1) ptt, "
        "(SELECT id FROM volte_subscriptions WHERE user_id=u.id LIMIT 1) volte, " + voip_col +
        "FROM users u WHERE u.login_id IS NOT NULL AND u.login_id<>''")
    fresh = {}
    def _tel(msisdn):
        if not msisdn:
            return None
        m = str(msisdn)
        return m if m.startswith('tel:') else (f"tel:{m}" if m.startswith('+') else f"tel:+{m}")

    for r in cur.fetchall():
        fresh[r['login_id']] = {
            "password": r.get('passwd') or '', "user_id": r['uid'],
            "mcptt_id": _tel(r.get('ptt')),
            "line_id": _tel(r.get('ptt') or r.get('volte') or r.get('voip')),
            "name": r.get('name'),
        }
    LOGIN_ACCOUNTS.clear()
    LOGIN_ACCOUNTS.update(fresh)


def refresh_login_accounts() -> bool:
    """DB 에서 IdMS 로그인 계정을 재조회해 LOGIN_ACCOUNTS 에 반영한다.
    admin API 의 가입자 login_id/passwd 변경·가입 번호 추가/삭제(MCPTT ID 파생)가 재기동 없이 로그인에
    보이도록 admin.py 가 호출한다 (LOGIN_ACCOUNTS 는 기동 시 1회 적재 — 이 갱신이 없으면 재기동 전까지 stale)."""
    if not _DB_CONFIG:
        return False
    try:
        import pymysql, pymysql.cursors
        conn = pymysql.connect(
            host=_DB_CONFIG.get('Host', '127.0.0.1'),
            port=int(_DB_CONFIG.get('Port', 3306)),
            user=_DB_CONFIG.get('User', 'root'),
            password=_DB_CONFIG.get('Password', ''),
            database=_DB_CONFIG.get('Db', 'cims'),
            charset='utf8mb4',
            cursorclass=pymysql.cursors.DictCursor,
            connect_timeout=5,
        )
        with conn:
            with conn.cursor() as cur:
                _load_login_accounts(cur)
        logger.log_info(f"refresh_login_accounts: {len(LOGIN_ACCOUNTS)} login accounts")
        return True
    except Exception as e:
        logger.log_error(f"refresh_login_accounts failed: {e}")
        return False


# ── ptt_groups ↔ in-memory GROUPS — 행→dict 단일 변환 ──────────────────────────
#   기동 적재(load_shared_data)와 CRUD 후 단일 그룹 동기화(sync_group_from_db)가 같은 함수를 쓴다.
#   admin API(콘솔) 와 GMS XCAP(가입자) 두 쓰기 경로 모두 DB 를 정본으로 쓰고 이 동기화로 캐시를
#   맞춘다 — 어느 경로도 GROUPS 를 직접 조립하지 않는다.
# 그룹 호 타이머 (TS 24.481 §7.2.2 o·§7.2.7) — 그룹 문서 <on-network-hang-timer>(T4 Inactivity,
#   TS 24.380 §6.3.4.3.5 · Table 11.1.3-1 기본 30초) · <on-network-maximum-duration>(TNG3, TS 24.379 §6.3.8.1).
#   범위 상한은 CMP floor_timers.t4_inactivity 계약(0..3600)과 같다. 0 = 미사용/무제한 — 규격에 없는 CIMS 약속이라 문서에는
#   0 을 싣지 않는다(mcptt_timers.md §7 D8): T4 0 = <on-network-hang-timer> 생략(요소가 없으면 T4 를 걸지 않는다), TNG3 0 =
#   편성 그룹은 값이 필수(TS 24.481 §7.2.7 «invite-members true 면 shall contain a value»)라 GROUP_MAX_DURATION_UNLIMITED 를
#   싣는다. chat 그룹은 TNG3 를 돌리지 않으므로(상시 세션 — TS 24.379 §6.3.3.5.1 은 요소가 있을 때만 켠다) 요소를 싣지 않는다.
GROUP_HANG_TIMER_DEFAULT = 30
GROUP_HANG_TIMER_MAX = 3600
GROUP_MAX_DURATION_DEFAULT = 3600
GROUP_MAX_DURATION_MAX = 86400
# «무제한» 의 문서 표기 — 규격·스키마에 xs:duration 상한이 없어(XML Schema Part 2 §3.2.6 — 자리수 무제한) 32비트 부호 있는
#   초 카운터의 최댓값(약 68년)을 쓴다: 설정 범위(0..GROUP_MAX_DURATION_MAX)와 겹치지 않아 XCAP PUT 이 0 으로 되읽을 수 있고,
#   초를 int32 로 드는 수신 측(단말 SDK parseXsDuration 등)이 넘치지 않는 가장 큰 값이다. 이 값 이상은 PUT 에서 0 으로 읽는다.
GROUP_MAX_DURATION_UNLIMITED = 2 ** 31 - 1
GROUP_TYPES = ('prearranged', 'chat')   # 일제 통화는 그룹 종류가 아니라 호 속성(<broadcast-ind>)
# 확인 통화 설정(acknowledged call setup) — TS 24.481 §7.2.2 s)t)u) · TS 24.379 §6.3.3.3(TNG1)·§10.1.1.4.2.
#   <on-network-minimum-number-to-start>(xs:unsignedShort — 개시자 200 OK 전 멤버 200 수, 0 = 기다리지 않음),
#   <on-network-timeout-for-acknowledgement-of-required-members>(xs:duration = TNG1),
#   <on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>(proceed|abandon —
#   그 밖의 값은 abandon 으로 해석, §7.2.2 u)). 필수 멤버 = <entry> 의 <on-network-required>(§7.2.4.2).
GROUP_MIN_TO_START_MAX = 65535
GROUP_ACK_TIMEOUT_DEFAULT = 5
GROUP_ACK_TIMEOUT_MAX = 300
GROUP_ACK_ACTIONS = ('proceed', 'abandon')
# 그룹 우선순위 <on-network-group-priority>·멤버 우선순위 <user-priority> = mcpttgi:priorityType(0..255, TS 24.481 §7.2.4.2) — 값이
#   클수록 높다(§7.2.8). 두 쓰기 경로(admin API·GMS XCAP PUT)가 범위 밖 값을 거절한다.
PRIORITY_TYPE_MAX = 255


def norm_ack_action(value) -> str:
    """만료 동작 값 — proceed 가 아니면 abandon (TS 24.481 §7.2.2 u) 의 해석 규칙)."""
    return 'proceed' if str(value or '').strip() == 'proceed' else 'abandon'


def xs_duration(sec: int) -> str:
    """초 → xs:duration (PT{n}S)."""
    return f"PT{int(sec)}S"


def parse_xs_duration(text: Optional[str]) -> Optional[int]:
    """xs:duration(PnDTnHnMnS, 소수 초 절사) → 초. 형식이 아니면 None — 순수 정수(초)도 받는다(관대한 수신)."""
    if text is None:
        return None
    t = text.strip()
    if t.isdigit():
        return int(t)
    m = re.fullmatch(r'P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)(?:\.\d+)?S)?)?', t)
    if not m or t in ('P', 'PT'):
        return None
    d, h, mi, se = (int(x) if x else 0 for x in m.groups())
    return ((d * 24 + h) * 60 + mi) * 60 + se


_GROUP_SELECT = (
    "SELECT id, mcptt_group_id, name, priority, encryption, "
    "emergency_call, emergency_alert, allow_conference_state, "
    "allow_sds, allow_fd, max_sds_size, max_auto_recv, "
    "org_code, session_start, session_end, "
    "group_type, on_network, max_members, require_affiliation, alias, "
    "hang_timer_sec, max_duration_sec, "
    "min_number_to_start, ack_timeout_sec, ack_action, "
    "authorized_user_id, "
    "(SELECT id FROM ptt_subscriptions WHERE user_id=ptt_groups.authorized_user_id "
    " ORDER BY id LIMIT 1) AS authorized_user_msisdn "
    "FROM ptt_groups"
)


def _member_select_sql(cur) -> str:
    """멤버 행 SELECT (WHERE/ORDER BY 는 호출자가 붙인다). users.title 은 컬럼 존재 시만."""
    title_col = ", u.title AS user_title" if _users_has_title(cur) else ""
    return (
        "SELECT g.mcptt_group_id AS mcptt_group_id, gm.user_id, gm.priority, gm.role, gm.mcptt_id, "
        "       gm.on_network_required, gm.implicit_affiliation, "
        f"       u.name AS user_name{title_col} "
        "FROM ptt_group_members gm "
        "JOIN ptt_groups g ON g.id = gm.group_id "
        "LEFT JOIN ptt_subscriptions ps ON ps.id = gm.user_id "
        "LEFT JOIN users u ON u.id = ps.user_id"
    )


def _group_row_to_dict(row: dict) -> dict:
    gid = row['mcptt_group_id']
    return {
        "display_name": row['name'],
        "priority": row.get('priority', 5),
        "encryption": bool(row.get('encryption', 0)),
        "emergency_call": bool(row.get('emergency_call', 0)),
        "emergency_alert": bool(row.get('emergency_alert', 1)),
        "allow_conference_state": bool(row.get('allow_conference_state', 1)),
        "allow_sds": bool(row.get('allow_sds', 1)),
        "allow_fd": bool(row.get('allow_fd', 0)),
        "max_sds_size": row.get('max_sds_size', 10000),
        "max_auto_recv": row.get('max_auto_recv', 1048576),
        "org_code": row.get('org_code', ''),
        "group_type": row.get('group_type', 'prearranged'),
        "on_network": bool(row.get('on_network', 1)),
        "max_members": row.get('max_members', 0),
        "require_affiliation": bool(row.get('require_affiliation', 1)),
        "hang_timer_sec": int(row.get('hang_timer_sec', GROUP_HANG_TIMER_DEFAULT)),
        "max_duration_sec": int(row.get('max_duration_sec', GROUP_MAX_DURATION_DEFAULT)),
        "min_number_to_start": int(row.get('min_number_to_start') or 0),
        "ack_timeout_sec": int(row.get('ack_timeout_sec', GROUP_ACK_TIMEOUT_DEFAULT)),
        "ack_action": norm_ack_action(row.get('ack_action')),
        "alias": row.get('alias', ''),
        "session_start": row['session_start'].isoformat() if row.get('session_start') else None,
        "session_end": row['session_end'].isoformat() if row.get('session_end') else None,
        "etag": f"etag_{gid}",
        "created_by": "", "created_at": "",
        # 그룹 소유 (3GPP TS 23.280 authorized user) — 파생 MCPTT ID(표시·열람 인가) + users.id(GMS 쓰기 인가)
        "authorized_user": (f"tel:{row['authorized_user_msisdn']}"
                            if row.get('authorized_user_msisdn') else ""),
        "authorized_user_id": row.get('authorized_user_id'),
        # MCVideo 서비스 — mcvideo_group_attrs 행이 있으면 속성 dict, 없으면 None(MCPTT 전용 그룹). 적재 경로가 채운다.
        "mcvideo": None,
        "members": []
    }


def _member_row_to_dict(row: dict) -> dict:
    uid = row['user_id']
    m_uri = row.get('mcptt_id') or (f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}")
    return {
        "uri": m_uri, "name": row.get('user_name') or m_uri,
        "role": row.get('role') or "participant",
        "priority": row['priority'], "joined_at": "",
        "title": row.get('user_title') or "",
        "required": bool(row.get('on_network_required') or 0),
        "implicit_affiliation": bool(row.get('implicit_affiliation') or 0),
    }


def _db_connect():
    """CimsDatabase 설정으로 pymysql 연결 (DictCursor, 5초). _DB_CONFIG 없으면 None."""
    if not _DB_CONFIG:
        return None
    import pymysql, pymysql.cursors
    return pymysql.connect(
        host=_DB_CONFIG.get('Host', '127.0.0.1'),
        port=int(_DB_CONFIG.get('Port', 3306)),
        user=_DB_CONFIG.get('User', 'root'),
        password=_DB_CONFIG.get('Password', ''),
        database=_DB_CONFIG.get('Db', 'cims'),
        charset='utf8mb4',
        cursorclass=pymysql.cursors.DictCursor,
        connect_timeout=5,
    )


def sync_group_from_db(group_id: str) -> bool:
    """DB 의 그룹 한 건(속성+멤버)을 재조회해 in-memory GROUPS 에 반영한다. 행이 없으면 캐시에서 제거.
    admin API 그룹 CRUD 와 GMS XCAP PUT/DELETE 가 쓰기 후 공통으로 호출한다 — GROUPS 는 기동 시
    1회 적재라 이 동기화가 없으면 재기동 전까지 stale(신규 그룹은 GMS 목록에 없고 삭제 그룹이 남는다).
    반환값 = 캐시에 그룹이 존재하는가."""
    uri = _group_uri(group_id)
    try:
        conn = _db_connect()
        if conn is None:
            return uri in GROUPS
        with conn:
            with conn.cursor() as cur:
                cur.execute(_GROUP_SELECT + " WHERE mcptt_group_id=%s", (group_id,))
                row = cur.fetchone()
                if not row:
                    GROUPS.pop(uri, None)
                    logger.log_info(f"sync_group_from_db({group_id}): removed from cache")
                    return False
                grp = _group_row_to_dict(row)
                cur.execute(_member_select_sql(cur) + " WHERE g.mcptt_group_id=%s ORDER BY gm.priority",
                            (group_id,))
                grp['members'] = [_member_row_to_dict(r) for r in cur.fetchall()]
                mv = _mcvideo.load_group_attrs(cur, group_id)
                grp['mcvideo'] = (mv or {}).get(group_id)
        GROUPS[uri] = grp
        logger.log_info(f"sync_group_from_db({group_id}): {len(grp['members'])} members")
        return True
    except Exception as e:
        logger.log_error(f"sync_group_from_db({group_id}) failed: {e}")
        return uri in GROUPS


def refresh_group_members(group_id: str) -> bool:
    """호환 이름 — sync_group_from_db 로 위임(속성까지 함께 갱신). 기존 호출자(admin.py·시험) 유지."""
    return sync_group_from_db(group_id)


# [FIX] Notify CSP logic
_notify_seq = 0

# 목적지별 **연결형(connected) UDP 소켓**. 비연결 sendto 는 수신 프로세스가 없어도
# 항상 성공하므로 제어평면 단절(예: 목적지 IP 오설정)이 무기한 침묵한다. connect 된
# 소켓은 커널이 ICMP port-unreachable 을 큐잉해 send/recv 에서 ECONNREFUSED 로
# 관측되므로, 도달 실패를 즉시 ERROR 로 남길 수 있다.
_notify_socks: Dict[Tuple[str, int], socket.socket] = {}


def _notify_sock(ip: str, port: int) -> socket.socket:
    key = (ip, int(port))
    sock = _notify_socks.get(key)
    if sock is None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)
        sock.connect((ip, int(port)))
        _notify_socks[key] = sock
    return sock


def _drop_notify_sock(ip: str, port: int) -> None:
    sock = _notify_socks.pop((ip, int(port)), None)
    if sock is not None:
        try:
            sock.close()
        except Exception:
            pass


def _reset_notify_socks() -> None:
    """설정 리로드로 목적지가 바뀐 경우 등, 캐시된 소켓을 버린다.

    close 를 직접 호출하지 않는다 — 리로드는 SIGUSR1 핸들러(메인 스레드)에서 돌고
    `_send_notify` 는 요청 처리 스레드에서 도는데, 사용 중 fd 를 닫으면 EBADF 로
    한 건이 유실될 수 있다. 참조만 끊으면 `_send_notify` 의 지역 참조가 사라지는
    시점에 인터프리터가 닫는다."""
    _notify_socks.clear()


def _send_notify(ip: str, port: int, label: str, payload: bytes) -> bool:
    """단일 목적지 발송. 도달 실패(수신 프로세스 없음)면 False + ERROR 로그."""
    for attempt in (0, 1):
        try:
            sock = _notify_sock(ip, port)
            sock.send(payload)
        except ConnectionRefusedError as e:
            # 직전 datagram 이 유발한 잔여 ICMP 일 수 있으므로 새 소켓으로 1회 재시도.
            _drop_notify_sock(ip, port)
            if attempt == 0:
                continue
            logger.log_error(f"Notify {label} {ip}:{port} 미도달 — 수신 프로세스 없음 ({e})")
            return False
        except OSError as e:
            _drop_notify_sock(ip, port)
            logger.log_error(f"Notify {label} {ip}:{port} 발송 실패: {e}")
            return False
        # 발송 직후 ICMP 회신 확인 — loopback/LAN 은 이 창 안에 도착한다.
        try:
            readable, _, _ = select.select([sock], [], [], 0.05)
            if readable:
                sock.recv(4096)   # 응답(STATS_RESPONSE 등)은 이 경로에서 쓰지 않는다
        except ConnectionRefusedError as e:
            _drop_notify_sock(ip, port)
            logger.log_error(f"Notify {label} {ip}:{port} 미도달 — 수신 프로세스 없음 ({e})")
            return False
        except OSError:
            pass
        return True
    return False


def _notify_targets(event_type):
    """이벤트 타입별 endpoint 라우팅 — (ip, port, label) tuple list.
    - GROUP_CHANGED: PSP 단독 (PSP 미설정 시 CSP fallback)
    - USER_CHANGED + 기타: CSP + PSP 양쪽 broadcast (둘이 같은 IP/port 면 dedup)
    """
    targets = []
    seen = set()
    def _add(ip, port, label):
        if not ip:
            return
        key = (ip, int(port))
        if key in seen:
            return
        seen.add(key)
        targets.append((ip, int(port), label))

    # GROUP_CHANGED 포함 모든 이벤트를 CSP + PSP 양쪽 broadcast (dedup).
    #   PTT-AS 가 통합 csp(Roles.PTT_AS) 인지 분리 psp 인지 토폴로지에 무관하게
    #   PTT-AS 노드가 반드시 수신하도록. (과거 GROUP_CHANGED→PSP 단독 라우팅은 csp 가
    #   PTT-AS 인 구성에서 신규 그룹이 csp 의 in-memory map 에 로드되지 않는 버그를 유발.)
    _add(CSP_NOTIFY_IP, CSP_NOTIFY_PORT, "CSP")
    _add(PSP_NOTIFY_IP, PSP_NOTIFY_PORT, "PSP")
    return targets


def notify_csp(event_type, uri, action, etag="", sesid="", caller="", service=""):
    """CSC → CSP/PSP UDP notify.
    sesid/service 를 payload에 실어 시그널링 서버가 flow 로그 상관관계를 유지.
    - sesid 미지정 시 자동 발행 (caller 또는 빈값 기반)
    - caller 미지정 시 uri에서 자동 추출
    - service 미지정 시 이벤트 타입으로 추정:
        CSC_RESTART → system, *_CHANGED/* → console (admin 트리거), 그 외 → mcptt
    - 라우팅: GROUP_CHANGED → PSP 전용, USER_CHANGED/기타 → CSP+PSP broadcast.
      PSP 미설정 (PSP_NOTIFY_IP=="") 이거나 PSP 가 CSP 와 동일하면 단일 endpoint.
    """
    global _notify_seq
    _notify_seq += 1
    try:
        # caller 미지정 시 uri에서 추출 (tel:+82..., sip:user@domain)
        if not caller and uri:
            if uri.startswith("tel:"):
                caller = uri[4:]
            elif uri.startswith("sip:"):
                caller = uri[4:].split("@", 1)[0]
        if not sesid:
            sesid = _logger.issue_sesid(caller, "csc")
        if not service:
            if event_type in ("CSC_RESTART", "HEARTBEAT", "STATS_REQUEST", "STATS_RESPONSE"):
                service = "system"
            elif event_type in (
                "USER_CHANGED", "GROUP_CHANGED", "PHONE_GROUP_CHANGED", "ROLE_CHANGED",
                # CSP 런타임 설정 변경 알림 (admin 트리거)
                "LISTENER_CHANGED", "TRUNK_CHANGED",
                "ROUTE_RULE_CHANGED", "ACCESS_LIST_CHANGED",
            ):
                service = "console"
            else:
                service = "mcptt"

        data = {
            "trans_id": str(_notify_seq),
            "event": event_type,
            "uri": uri,
            "action": action,
            "etag": etag,
            "sesid": sesid,
            "service": service,
        }
        msg = json.dumps(data)

        targets = _notify_targets(event_type)
        payload = msg.encode('utf-8')
        delivered, failed = [], []
        for ip, port, label in targets:
            dst = f"{label}({ip}:{port})"
            (delivered if _send_notify(ip, port, label, payload) else failed).append(dst)

        # CSC 자체 flow/msg 로그 — peer 는 첫 target (혹은 CSP fallback)
        peer_ip, peer_port = (targets[0][0], targets[0][1]) if targets else (CSP_NOTIFY_IP, CSP_NOTIFY_PORT)
        _logger.log_flow(
            service=service,
            from_actor="csc", to_actor="csp",
            proto="CSC", method=event_type,
            detail=f"{action} {uri}".strip() if (action or uri) else "",
            sesid=sesid,
            iface="csp",
            body=msg,
            peer=f"{peer_ip}:{peer_port}",
            mid=str(_notify_seq),
            caller=caller,
        )

        if failed:
            logger.log_error(f"Notify 미도달 → {','.join(failed)}"
                             + (f" (도달: {','.join(delivered)})" if delivered else "")
                             + f": {msg}")
        else:
            logger.log_info(f"Notify Sent → {','.join(delivered) or '(none)'}: {msg}")
    except Exception as e:
        logger.log_error(f"Notify Failed: {e}")


_CONFIG_EVENT_BY_ENTITY = {
    "listener": "LISTENER_CHANGED",
    "trunk":    "TRUNK_CHANGED",
    "route":    "ROUTE_RULE_CHANGED",
    "access":   "ACCESS_LIST_CHANGED",
}


def notify_config_change(entity: str, entity_id, action: str,
                          actor: str = "", reason: str = "") -> None:
    """CSP 런타임 설정 변경 알림.
    - admin API 가 DB CUD 완료 후 호출
    - CSC 메모리/파일 캐시를 해당 entity 만 재조회 (write-through)
    - ETag 포함 UDP notify → CSP 가 HTTP pull 로 동기화

    Args:
        entity: 'listener' | 'trunk' | 'route' | 'access'
        entity_id: DB id (int) 또는 조합 키
        action: 'CREATE' | 'UPDATE' | 'DELETE'
        actor: 감사로그용 변경자 (JWT sub)
        reason: 감사로그용 사유
    """
    event = _CONFIG_EVENT_BY_ENTITY.get(entity)
    if not event:
        logger.log_warning(f"notify_config_change: unknown entity {entity}")
        return

    # 캐시 새로고침 (DB→메모리→파일)
    etag = ""
    try:
        from services import config_cache as _cfg
        if _cfg.CONFIG_CACHE is not None and not _cfg.CONFIG_CACHE.is_read_only():
            _cfg.CONFIG_CACHE.refresh_entity(entity)
            etag = _cfg.CONFIG_CACHE.get_meta(entity).get("etag", "")
    except Exception as e:
        logger.log_error(f"notify_config_change: cache refresh failed: {e}")

    uri = f"{entity}/{entity_id}"
    notify_csp(event, uri=uri, action=action, etag=etag,
               caller=actor or "admin", service="console")


def audit_config_change(db_cfg: dict, actor: str, actor_ip: str,
                        entity: str, entity_id, action: str,
                        before: dict = None, after: dict = None,
                        etag_before: str = "", etag_after: str = "",
                        reason: str = "") -> None:
    """csp 설정 변경 감사 — FM 이벤트 스트림(kind=audit)으로 발신
    (alarm_self_reporting.md §6 — 구 {CimsRuntimeDir}/csp_config_audit JSONL 흡수).

    db_cfg 인자는 호환을 위해 유지 (구 시그니처).
    실패/FM 비활성 시 프로세스 로그로만 남김 (운영 기능 차단 안 함).
    """
    try:
        from services import fm_reporter as _fm
        params = {
            "actor": actor, "actor_ip": actor_ip,
            "entity": entity, "entity_id": str(entity_id), "action": action,
            "before": before, "after": after,
            "etag_before": etag_before, "etag_after": etag_after,
            "reason": (reason or "")[:512] if reason else None,
            "entity_ko": getattr(_fm, "CONFIG_ENTITY_KO", {}).get(entity, entity),
            "action_ko": getattr(_fm, "CONFIG_ACTION_KO", {}).get(action, action),
        }
        message = (f"설정 변경 — {actor}({actor_ip}) 이(가) {params['entity_ko']} {entity_id} "
                   f"{params['action_ko']}")
        r = _fm.get()
        if r is not None:
            # mo 는 발신 주체(csc) 서버명 루트 — <node>/csc/config/<entity> (표준화 §3.4(b)).
            r.send_event('config_change', kind='audit',
                         mo=f"{r.node}/csc/config/{entity}", params=params, message=message)
        else:
            logger.log_info(f"audit_config_change (fm 비활성): {message}")
    except Exception as e:
        logger.log_warning(f"audit_config_change: {e}")


def save_group_to_file(group_uri, group_data):
    if not GROUP_DIR:
        return
    
    group_id = group_uri.replace("tel:+", "")
    fpath = os.path.join(GROUP_DIR, f"{group_id}.json")
    
    # Convert internal format to persistent JSON format
    users_list = []
    for m in group_data['members']:
        m_uri = m['uri']
        m_id = m_uri.replace("tel:+", "")
        priority = m.get('priority', 5)
        users_list.append({
            "id": m_id, 
            "priority": priority,
            "role": m.get('role', 'participant'),
            "joined_at": m.get('joined_at', '')
        })

    json_data = {
        "name": group_data['display_name'],
        "created_by": group_data.get('created_by', ''),
        "created_at": group_data.get('created_at', ''),
        "etag": group_data.get('etag', ''),
        "users": users_list
    }
    
    if not os.path.exists(GROUP_DIR):
        try:
            os.makedirs(GROUP_DIR)
        except Exception as e:
            logger.log_error(f"Failed to create group dir {GROUP_DIR}: {e}")
            return

    try:
        with open(fpath, 'w', encoding='utf-8') as f:
            json.dump(json_data, f, indent=4)
        logger.log_info(f"Saved Group to {fpath}")
    except Exception as e:
        logger.log_error(f"Error saving group {fpath}: {e}")

def delete_group_file(group_uri):
    if not GROUP_DIR:
        return

    group_id = group_uri.replace("tel:+", "")
    fpath = os.path.join(GROUP_DIR, f"{group_id}.json")
    
    if os.path.exists(fpath):
        try:
            os.remove(fpath)
            logger.log_info(f"Deleted Group file {fpath}")
        except Exception as e:
            logger.log_error(f"Error deleting group file {fpath}: {e}")

# --- PKCE Helper ---
def verify_pkce(code_verifier: str, code_challenge: str, method: str = "S256") -> bool:
    """
    PKCE 검증
    
    Args:
        code_verifier: 클라이언트가 보낸 원본
        code_challenge: 저장된 해시
        method: S256 또는 plain
    
    Returns:
        검증 성공 여부
    """
    if method == "S256":
        # SHA256 해시 계산
        computed = base64.urlsafe_b64encode(
            hashlib.sha256(code_verifier.encode('utf-8')).digest()
        ).decode('utf-8').rstrip('=')
        return computed == code_challenge
    elif method == "plain":
        return code_verifier == code_challenge
    else:
        return False

# --- Token Logic ---
# scope        = access_token 에 실리는 (좁혀진) 용도 scope.
# refresh_scope= 회전된 refresh_token 에 보존할 scope. None 이면 scope 와 동일.
#   scope 분리 refresh 시 access 만 좁히고 refresh 는 원 grant(broad) 유지 → 다음 다른-용도 refresh 가능.
def account_cred(login_id: str) -> str:
    """로그인 자격의 지문 — refresh token 에 실어 두고 재발급 때 지금 계정과 대조한다(TS 33.180 B.5.3 «refresh 때 계정이 여전히
    유효한지 확인하고 아니면 회수» RECOMMENDED). 비밀번호가 바뀌면 값이 달라진다. 계정이 없으면 빈 문자열."""
    acct = LOGIN_ACCOUNTS.get(login_id or '')
    if not acct:
        return ''
    return hashlib.sha256(f"{login_id}:{acct.get('password') or ''}".encode('utf-8')).hexdigest()[:16]


def create_tokens(subject, scope, client_id="mcptt_client", nonce=None, refresh_scope=None, mcptt_id=None, mc_user=True):
    """토큰 3종 발급. `scope` 는 이미 허가 계산(grant_scope / refresh 축소)을 거친 공백 구분 문자열.

    mc_user=False = MC 서비스 신원이 없는 계정(전화 전용 — PTT 가입 없음): `mcptt_id`·`mcdata_id`·`mcvideo_id` claim 을 싣지 않는다
    (프로비저닝 scope 만 쓰는 토큰). mc_user=True 이고 mcptt_id 가 없으면 subject 를 MC 신원으로 쓴다(DB 없는 legacy 로그인).

    claim 은 TS 33.180 Annex B: ID token = iss/sub/aud/exp/iat + mcptt_id/mcdata_id(+nonce),
    access token = exp/scope(공백 구분 문자열)/client_id + mcptt_id/mcdata_id (iss/sub/aud/iat 는 RFC 7519 추가분).
    단일 MC service ID 구성(TS 23.280 §10.1.4.1)이라 mcdata_id = mcptt_id 값. MCVideo 이용 자격이 있는 사용자는
    mcvideo_id(같은 값, B.2.1.3·B.2.2.3 "REQUIRED for MCVideo")도 싣는다 — 자격이 없으면 MCVideo ID 가 없다."""
    now = int(time.time())
    # sub = CIMS 로그인 ID(인증 신원). mcptt_id = 규격 MCPTT 서비스 신원(분리). 미지정 시 subject 로 폴백.
    sub = subject
    mcptt = (mcptt_id or subject) if mc_user else None
    scope = " ".join(scope.split()) if isinstance(scope, str) else " ".join(scope or [])

    mcvideo_claim = {"mcvideo_id": mcptt} if mcptt and _mcvideo.has_profile(_ptt_msisdn_of(mcptt)) else {}
    mc_claims = {"mcptt_id": mcptt, **mcvideo_claim, "mcdata_id": mcptt} if mcptt else {}

    # ID Token (OIDC) — nonce 가 있으면 반영(S2b: CSRF/replay 방지, OIDC Core §3.1.2.1)
    id_token_payload = {
        **mc_claims,
        "iss": IDMS_ISSUER,
        "sub": sub,
        "aud": client_id or "mcptt_client",
        "exp": now + ACCESS_TOKEN_TTL,
        "iat": now
    }
    if nonce:
        id_token_payload["nonce"] = nonce
    id_token = sign_token(id_token_payload)

    # Access Token — sub=login_id, mcptt_id/mcdata_id=MC 서비스 신원, client_id=요청 클라이언트(B.2.2.2).
    access_token_payload = {
        **mc_claims,
        "iss": IDMS_ISSUER,
        "sub": sub,
        "aud": "mcptt_client",
        "client_id": client_id or "mcptt_client",
        "iat": now,
        "exp": now + ACCESS_TOKEN_TTL,
        "scope": scope
    }
    access_token = sign_token(access_token_payload)

    # Refresh Token (UUID + 영속성 저장) — subject/mcptt_id 보존(refresh 재발급 시 동일 신원).
    refresh_token = str(uuid.uuid4())
    refresh_data = {
        "user_id": subject,
        "mcptt_id": mcptt,
        "client_id": client_id,
        "scope": refresh_scope if refresh_scope is not None else scope,
        "cred": account_cred(subject),
        "issued_at": now,
        "expires_at": now + REFRESH_TOKEN_TTL,
        "revoked": False,
        "rotated_to": None
    }
    storage.save_refresh_token(refresh_token, refresh_data)
    
    return id_token, access_token, refresh_token

def signing_alg() -> str:
    """지금 서명에 쓰는 방식 — 설정이 RS256 이고 서명 키가 준비됐으면 RS256, 아니면 HS256."""
    return 'RS256' if SIGNING_ALG_CONFIG == 'RS256' and _idms_keys.ready() else 'HS256'


def sign_token(payload: dict) -> str:
    """ID token·access token 서명(JWS compact, RFC 7515). RS256 은 헤더에 `kid`(RFC 7638 thumbprint)를 싣는다 — 검증자가
    JWKS 에서 키를 고른다."""
    if signing_alg() == 'RS256':
        return jwt.encode(payload, _idms_keys.signing_key(), algorithm="RS256", headers={"kid": _idms_keys.signing_kid()})
    return jwt.encode(payload, SECRET_KEY, algorithm="HS256")


def validate_access_token(token):
    """access token 검증(TS 33.180 B.11.2). 헤더의 `alg` 로 키 종류를 고르고 그 방식 하나로만 검증한다 — RS256 은 `kid` 의 공개
    키, HS256 은 공유 비밀(전환기 `IdMs.AcceptHs256` 또는 HS256 서명 구성일 때만). 그 밖의 `alg`(none 포함)는 거절한다."""
    try:
        header = jwt.get_unverified_header(token) or {}
        alg = header.get('alg')
        if alg == 'RS256':
            key = _idms_keys.public_key(header.get('kid'))
            if key is None:
                raise ValueError(f"unknown kid {header.get('kid')!r}")
        elif alg == 'HS256' and (ACCEPT_HS256 or signing_alg() == 'HS256'):
            key = SECRET_KEY
        else:
            raise ValueError(f"alg {alg!r} not accepted")
        payload = jwt.decode(token, key, algorithms=[alg], options={"verify_signature": True}, audience="mcptt_client")
        return payload
    except Exception as e:
        logger.log_error(f"Token validation error: {e}")
        return None


# ── scope 계산 ──
def expand_scopes(scope) -> list:
    """scope(공백 구분 문자열 또는 목록) → 별칭 확장·중복 제거 목록(입력 순서 유지, 별칭 원문도 남김).
    카탈로그 여과는 하지 않는다 — 발급은 grant_scope, 검사는 token_scopes 가 쓴다."""
    items = scope.split() if isinstance(scope, str) else list(scope or [])
    out: list = []
    for s in items:
        for v in (s,) + tuple(SCOPE_ALIASES.get(s, ())):
            if v not in out:
                out.append(v)
    return out


def grant_scope(requested, mcptt_id=None, mc_user=True):
    """허가 scope 계산 — 요청 ∩ (카탈로그 ∪ 별칭). 반환 (허가 공백 구분 문자열, 제외된 요청 항목 목록).
    별칭은 확장 집합과 원문을 함께 허가한다. 모르는 값은 제외 — 토큰 응답 `scope` 로 알린다.
    사용자 단위 인가(TS 33.180 B.4.2.2 — scope 는 사용자가 인가된 MC 서비스만 · TS 24.482 §4.1): mc_user=False(PTT 가입이 없는
    계정)면 MC 서비스 scope 전부(3gpp:mc:* 12종·구 별칭)를 주지 않는다. mcptt_id 를 주면 MCVideo 4종은 MCVideo 이용 자격
    (mcvideo_user_profile 행)이 있을 때만."""
    items = requested.split() if isinstance(requested, str) else list(requested or [])
    video_ok = mcptt_id is None or _mcvideo.has_profile(_ptt_msisdn_of(mcptt_id))
    granted: list = []
    dropped: list = []
    for s in items:
        if not mc_user and (s in SCOPE_MC_SERVICES or s in SCOPE_VIDEO_SERVICES or s in SCOPE_ALIASES):
            dropped.append(s)
            continue
        if s in SCOPE_VIDEO_SERVICES and not video_ok:
            dropped.append(s)
            continue
        if s in SCOPE_CATALOG or s in SCOPE_ALIASES:
            for v in expand_scopes([s]):
                if v not in granted:
                    granted.append(v)
        else:
            dropped.append(s)
    return " ".join(granted), dropped


def _ptt_msisdn_of(mcptt_id) -> str:
    """MCPTT ID(tel:+E.164 / sip:user@dom) → ptt_subscriptions.id 표기(USERS 의 msisdn). 모르면 사용자부 그대로."""
    for key in (mcptt_id, f"tel:{_norm_mcptt_uri(mcptt_id)}"):
        u = USERS.get(key or '')
        if u:
            return u.get('msisdn', '')
    return _norm_mcptt_uri(mcptt_id)


def token_scopes(payload: dict) -> set:
    """토큰의 유효 scope 집합 — 문자열/배열 양식 모두 수용(이행 전 발급 토큰은 배열), 별칭은 검사 시점에도 확장."""
    return set(expand_scopes((payload or {}).get('scope') or []))


def _bearer_challenge(error: str = '', scope: str = '') -> dict:
    """RFC 6750 §3 `WWW-Authenticate: Bearer` 헤더."""
    v = f'Bearer realm="{IDMS_DOMAIN}"'
    if error:
        v += f', error="{error}"'
    if scope:
        v += f', scope="{scope}"'
    return {"WWW-Authenticate": v}


def unauthorized(args) -> HandlerResult:
    """Bearer 토큰이 없거나 틀렸을 때의 응답 — Authorization 에 Bearer 토큰이 **없으면 403**(TS 24.482 A.2.3 1) — X-3GPP-Asserted-Identity
    도 받지 않으므로 신원을 알 길이 없는 요청), 토큰이 있었는데 검증에 실패했으면 **401** + error=invalid_token(A.2.3 2)a) →
    RFC 6750 §3.1 — 단말이 토큰을 갱신하는 신호)."""
    auth = str(args.headers.get('authorization') or args.headers.get('Authorization') or '').strip()
    if not (auth[:7].lower() == 'bearer ' and auth[7:].strip()):
        return HandlerResult(status=403, body={"error": "bearer_token_required"}, media_type="application/json")
    return HandlerResult(status=401, body={"error": "invalid_token"}, media_type="application/json",
                         headers=_bearer_challenge('invalid_token'))


def token_line_id(payload: dict) -> str:
    """토큰 주인의 회선 신원(tel URI) — 프로비저닝·관제 관리 API 가 사람을 찾는 값. MC 서비스 신원(mcptt_id)이 있으면 그것,
    없으면(전화 전용 계정) 로그인 계정의 첫 회선, 그것도 없으면 sub(DB 없는 legacy 로그인은 sub 가 tel URI 다)."""
    payload = payload or {}
    if payload.get('mcptt_id'):
        return payload['mcptt_id']
    acct = LOGIN_ACCOUNTS.get(payload.get('sub') or '') or {}
    return acct.get('line_id') or payload.get('sub') or ''


def require_scope(args, payload: dict, endpoint: str, *accepted: str) -> Optional[HandlerResult]:
    """리소스 서버 scope 검사(TS 33.180 B.10) — `accepted` 중 하나가 토큰에 있어야 통과(None).

    IdMs.ScopeEnforcement: off=검사 없음 / log=판정만 계산해 `would-deny` 한 줄 로그 후 통과(라이브 관찰 창) /
    enforce=403 insufficient_scope + WWW-Authenticate 에 필요한 scope 명시(RFC 6750 §3.1). 로그 한 줄에
    엔드포인트·신원·client_id·보유·요구를 모두 담아 grep 한 번으로 구 클라이언트를 찾을 수 있게 한다."""
    if SCOPE_ENFORCEMENT == 'off':
        return None
    have = token_scopes(payload)
    if any(s in have for s in accepted):
        return None
    line = (f"[IdMS][scope] would-deny endpoint={endpoint} method={args.method} "
            f"mcptt_id={(payload or {}).get('mcptt_id')} client_id={(payload or {}).get('client_id')} "
            f"granted={' '.join(sorted(have)) or '(none)'} required={'|'.join(accepted)}")
    if SCOPE_ENFORCEMENT != 'enforce':
        logger.log_warning(line)
        return None
    logger.log_error(line.replace('would-deny', 'deny', 1))
    return HandlerResult(status=403, body={"error": "insufficient_scope", "required": list(accepted)},
                         media_type="application/json",
                         headers=_bearer_challenge('insufficient_scope', ' '.join(accepted)))

# --- XML Generators ---
def _content_etag(content: str) -> str:
    """문서 내용 파생 ETag (RFC 7232). 구 정적 ETag(etag_{gid}/svcfg_etag_v1 등)는 문서가
    바뀌어도 동일 → If-None-Match 가 304 를 반환해 클라이언트가 stale 캐시를 받던 결함을 해소.
    내용이 바뀌면 ETag 가 바뀌어 정상적으로 새 문서를 받는다."""
    return '"' + hashlib.sha256((content or '').encode('utf-8')).hexdigest()[:16] + '"'


def _norm_mcptt_uri(u: str) -> str:
    """MCPTT URI 정규화(비교용) — scheme(sip:/tel:) 제거 + @도메인 제거 + 소문자.

    도메인 제거는 **단일 PTT 도메인 전제의 절충**이다: 외부 규격 단말은 신원을 sip: 완전형
    (sip:user@domain)으로 쓰는데, 우리 토큰/DB 는 tel: 형(도메인 없음)이라 도메인을 남기면
    본인 문서 접근이 403 으로 오탐된다. 사용자부(E.164)가 시스템에서 유일하므로 안전.
    다중 도메인 연동(타 시스템 상호접속)을 하게 되면 도메인 인지 비교로 재설계할 것."""
    s = (u or '').strip().lower()
    for p in ('sip:', 'tel:'):
        if s.startswith(p):
            s = s[len(p):]
            break
    return s.split('@', 1)[0]


def _uri_eq(a: str, b: str) -> bool:
    na = _norm_mcptt_uri(a)
    return na != '' and na == _norm_mcptt_uri(b)


def _is_group_member(group: dict, uri: str) -> bool:
    """uri 가 그룹의 멤버(또는 authorized_user)인지 — XCAP 그룹문서 접근 인가용 (TS 24.481)."""
    if not group:
        return False
    if any(_uri_eq(m.get('uri'), uri) for m in group.get('members', [])):
        return True
    return _uri_eq(group.get('authorized_user'), uri)


# 그룹 문서 <preferred-voice-encodings> 값 — CSP 그룹 호의 서비스 코덱(Setup.Media.Codecs 첫 항목, psip 기본 AMR-WB)과 같아야 한다.
#   MCVideo 그룹의 선호 음성 코덱 기본(services.mcvideo.GROUP_ATTR_DEFAULTS audio_encodings)과도 같은 값이다.
SERVICE_VOICE_ENCODING = "AMR-WB"
# 그룹 문서 <mcdata-default-charset> — IANA Character Sets 의 MIBenum(TS 24.282 §6.2.2.1). 106 = UTF-8.
MCDATA_TEXT_CHARSET_MIBENUM = 106


def _priority_type(v, default: int = 0) -> int:
    """mcpttgi:priorityType(xs:unsignedShort 0..255, TS 24.481 §7.2.4.2) — 범위 밖 저장값은 문서에서 경계로 자른다(쓰기 경로는 거절)."""
    try:
        return max(0, min(PRIORITY_TYPE_MAX, int(v)))
    except (TypeError, ValueError):
        return default


def get_group_xml(group_uri):
    group = GROUPS.get(group_uri)
    if not group:
        return None, None
    import html as _html
    # 이름·직함·조직 코드·URI 는 운영자 입력이다 — 텍스트·속성 모두 escape 해 문서를 well-formed 로 지킨다(RFC 4825 §6)
    esc = lambda v: _html.escape(str(v if v is not None else ''), quote=True)

    # 그룹 = 서비스 집합(TS 23.280 §3) — MCVideo 속성(mcvideo_group_attrs 행)이 있으면 MCPTT 그룹이자 MCVideo 그룹이다.
    mcvideo_attrs = group.get('mcvideo')
    has_mcdata = bool(group.get('allow_sds', True) or group.get('allow_fd', False))

    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:ocp="urn:oma:xml:xdm:common-policy"
  xmlns:oxe="urn:oma:xml:xdm:extensions"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"
  xmlns:cims="urn:cims:groupinfo:1.0">
  <list-service uri="{esc(group_uri)}">
    <display-name xml:lang="en-us">{esc(group['display_name'])}</display-name>
    <list>"""

    for member in group['members']:
        xml += f"""
      <entry uri="{esc(member['uri'])}">
        <rl:display-name>{esc(member['name'])}</rl:display-name>"""
        # 필수 멤버만 <on-network-required> — 있으면 제어 기능이 개시자 응답 전에 그 멤버의 200 을 기다린다(TNG1,
        #   TS 24.379 §6.3.3.3). 없으면 필수 멤버가 아니다(TS 24.481 §7.2.4.2).
        if member.get('required'):
            xml += """
        <mcpttgi:on-network-required/>"""
        xml += f"""
        <mcpttgi:participant-type>{member.get('role', 'participant')}</mcpttgi:participant-type>
        <mcpttgi:user-priority>{_priority_type(member.get('priority', 5))}</mcpttgi:user-priority>"""
        # 서비스별 신원 — MCVideo·MCData 그룹 문서의 entry 는 <mcvideo-mcvideo-id>·<mcdata-mcdata-id> 를 **반드시** 싣는다
        #   (TS 24.481 §7.2.2). 단일 MC service ID 라 값 = entry uri(MCPTT ID, TS 23.280 §10.1.4.1 — mcvideo.md §7 D1).
        if mcvideo_attrs is not None:
            xml += _mcvideo.entry_xml(member['uri'])
        if has_mcdata:
            xml += f"""
        <mcpttgi:mcdata-mcdata-id uri="{esc(member['uri'])}"/>"""
        # 직함 — 3GPP 미정의 필드라 CIMS 전용 네임스페이스 확장으로 전달
        # (<entry> 는 ##other lax 확장 허용, 표준 단말은 무시 — TS 24.481 정합)
        if member.get('title'):
            xml += f"""
        <cims:user-title>{esc(member['title'])}</cims:user-title>"""
        xml += """
      </entry>"""

    grp_priority = _priority_type(group.get('priority', 5))
    encryption_val = 'true' if group.get('encryption') else 'false'
    emergency_val = 'true' if group.get('emergency_call') else 'false'
    # allow-imminent-peril-call 은 emergency_call 미러 — condition(긴급·임박)은 단일 게이트.
    #   규격 요소(TS 24.481)는 유지하되 별도 설정 축을 두지 않는다.
    imminent_val = emergency_val
    alert_val = 'true' if group.get('emergency_alert', True) else 'false'
    # on-network-allow-conference-state (TS 24.481 §7.2.4.2) — 멤버의 conference 이벤트(RFC 4575) 구독 허용.
    #   CSP 가 초기 SUBSCRIBE 에서 판정(TS 24.379 §10.1.3.4.1, 불허 403 Warning 138). 관제사 청취 범위는 별도 축.
    conf_state_val = 'true' if group.get('allow_conference_state', True) else 'false'
    org_code = group.get('org_code', '')
    group_type = group.get('group_type', 'prearranged')
    # 정원 = <on-network-max-participant-count>(§7.2.8 — 그룹 세션 최대 참가자). 0 = 무제한이라 요소를 싣지 않는다(0 은 «0명» 으로
    #   읽힌다). 관례값을 대신 실으면 그 값을 되돌려 저장하는 클라이언트가 정원을 굳힌다.
    max_count = int(group.get('max_members') or 0)
    affil_required = 'true' if group.get('require_affiliation', True) else 'false'
    # MCData 그룹 메시징 게이트 (TS 24.481 §7.2.4.2 — mcpttgi 네임스페이스 표준 요소)
    sds_val = 'true' if group.get('allow_sds', True) else 'false'
    fd_val = 'true' if group.get('allow_fd', False) else 'false'
    max_sds = int(group.get('max_sds_size') or 0)
    # 그룹 종류 = <on-network-invite-members> (TS 24.481 §7.2.2 a — true=prearranged, false=chat).
    invite_members = 'true' if group_type != 'chat' else 'false'
    hang_timer = int(group.get('hang_timer_sec', GROUP_HANG_TIMER_DEFAULT))
    max_duration = int(group.get('max_duration_sec', GROUP_MAX_DURATION_DEFAULT))
    # 그룹 호 타이머(TS 24.481 §7.2.2 o)p)) — 0 은 문서에 싣지 않는다(GROUP_MAX_DURATION_UNLIMITED 주석): T4 0 = 생략,
    #   TNG3 = 편성 그룹만(0 = 무제한 표기), chat 그룹은 생략(TNG3 미가동).
    timers = ''
    if hang_timer > 0:
        timers += f"""
    <mcpttgi:on-network-hang-timer>{xs_duration(hang_timer)}</mcpttgi:on-network-hang-timer>"""
    if group_type != 'chat':
        timers += f"""
    <mcpttgi:on-network-maximum-duration>{xs_duration(max_duration if max_duration > 0 else GROUP_MAX_DURATION_UNLIMITED)}</mcpttgi:on-network-maximum-duration>"""
    min_to_start = int(group.get('min_number_to_start') or 0)
    ack_timeout = int(group.get('ack_timeout_sec', GROUP_ACK_TIMEOUT_DEFAULT))
    ack_action = norm_ack_action(group.get('ack_action'))
    xml += """
    </list>"""
    # on-network 를 끈 그룹 = <on-network-disabled/>(§7.2.2 g) · §7.2.8) — 제어 기능은 그 그룹의 호를 403 + 115 로 거절한다
    #   (TS 24.379 §6.3.5.2 5)a)).
    if not group.get('on_network', True):
        xml += """
    <mcpttgi:on-network-disabled/>"""
    # MCData 몫(§7.2.2 MCData 목록 순) — 보호 둘은 없으면 true(GDK 로 보호 필수, §7.2.8)라 false 를 명시한다(E2E 미구현 — MCPTT·MCVideo
    #   몫과 같다). 그룹 우선순위는 MCPTT 와 같은 값(한 그룹 = 서비스 집합, 없으면 «가장 낮음» 으로 읽힌다).
    if has_mcdata:
        xml += """
    <mcpttgi:mcdata-protect-media>false</mcpttgi:mcdata-protect-media>
    <mcpttgi:mcdata-protect-transmission-control>false</mcpttgi:mcdata-protect-transmission-control>"""
    xml += f"""
    <mcpttgi:mcdata-allow-short-data-service>{sds_val}</mcpttgi:mcdata-allow-short-data-service>
    <mcpttgi:mcdata-allow-file-distribution>{fd_val}</mcpttgi:mcdata-allow-file-distribution>"""
    if has_mcdata:
        xml += f"""
    <mcpttgi:mcdata-on-network-group-priority>{grp_priority}</mcpttgi:mcdata-on-network-group-priority>"""
    if max_sds > 0:
        xml += f"""
    <mcpttgi:mcdata-on-network-max-data-size-for-SDS>{max_sds}</mcpttgi:mcdata-on-network-max-data-size-for-SDS>"""
    max_auto = int(group.get('max_auto_recv') or 0)
    if max_auto > 0:
        xml += f"""
    <mcpttgi:mcdata-on-network-max-data-size-auto-recv>{max_auto}</mcpttgi:mcdata-on-network-max-data-size-auto-recv>"""
    # 그룹 SDS 의 TEXT payload 문자 집합(§7.2.2 r) — IANA MIBenum, TS 24.282 §6.2.2.1). CIMS 의 SDS 본문은 UTF-8 이다.
    if has_mcdata:
        xml += f"""
    <mcpttgi:mcdata-default-charset>{MCDATA_TEXT_CHARSET_MIBENUM}</mcpttgi:mcdata-default-charset>"""
    # 그룹 영상은 MCVideo <service>(TS 24.481 §7.2.8)로 싣는다 — MCPTT 몫에는 영상 요소가 없다(mcvideo.md §8).
    xml += f"""
    <mcpttgi:on-network-invite-members>{invite_members}</mcpttgi:on-network-invite-members>"""
    if max_count > 0:
        xml += f"""
    <mcpttgi:on-network-max-participant-count>{max_count}</mcpttgi:on-network-max-participant-count>"""
    # 선호 음성 코덱(§7.2.2 j) — 단말은 그룹 호 offer 에 이 코덱을 넣는다(TS 24.379 §6.2.1 2)b)). 값 = 서버가 집행하는 서비스 코덱
    #   (CSP Setup.Media.Codecs 첫 항목, 기본 AMR-WB — 그 코덱이 없는 offer 는 488).
    # 보호 둘 <protect-media>·<protect-floor-control-signalling>(§7.2.2 v)w)) 은 요소가 없으면 true(GMK 필수·floor 보호 필수, §7.2.8)
    #   라 false 를 명시한다 — CIMS 는 E2E 미디어 보호(GMK)를 하지 않는다(mcx_e2e_security.md, MCVideo 몫과 같다).
    xml += f"""
    <mcpttgi:preferred-voice-encodings><mcpttgi:encoding name="{esc(SERVICE_VOICE_ENCODING)}"/></mcpttgi:preferred-voice-encodings>
    <mcpttgi:on-network-require-affiliation>{affil_required}</mcpttgi:on-network-require-affiliation>{timers}
    <mcpttgi:on-network-minimum-number-to-start>{min_to_start}</mcpttgi:on-network-minimum-number-to-start>
    <mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>{xs_duration(ack_timeout)}</mcpttgi:on-network-timeout-for-acknowledgement-of-required-members>
    <mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>{ack_action}</mcpttgi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members>
    <mcpttgi:protect-media>false</mcpttgi:protect-media>
    <mcpttgi:protect-floor-control-signalling>false</mcpttgi:protect-floor-control-signalling>
    <mcpttgi:on-network-require-talker-id>false</mcpttgi:on-network-require-talker-id>
    <mcpttgi:on-network-group-priority>{grp_priority}</mcpttgi:on-network-group-priority>
    <mcpttgi:on-network-encryption>{encryption_val}</mcpttgi:on-network-encryption>"""
    if mcvideo_attrs is not None:
        xml += _mcvideo.list_service_xml(mcvideo_attrs)
    # 규칙 — 멤버(<is-list-member>)에게 그룹 호 개시(<allow-initiate-conference>)·진행 중 세션 합류(<join-handling>)·명단 열람
    #   (<on-network-allow-getting-member-list> — 없으면 false 라 규격 단말은 명단을 못 읽는다, TS 24.481 §7.2.8·§7.2.12.1)을 허용한다.
    #   제어 기능이 개시·합류를 인가하는 근거 요소다(TS 24.379 §6.3.5.3·§6.3.5.4, TS 24.281 §6.3.5.3·§6.3.5.4). 요소 이름공간은
    #   OMA list-service(기본 이름공간, TS 24.481 Annex A.2.2 예시).
    xml += f"""
    <cp:ruleset>
      <cp:rule id="a7c">
        <cp:conditions>
          <is-list-member/>
        </cp:conditions>
        <cp:actions>
          <allow-initiate-conference>true</allow-initiate-conference>
          <join-handling>true</join-handling>
          <mcpttgi:on-network-allow-getting-member-list>true</mcpttgi:on-network-allow-getting-member-list>
          <mcpttgi:allow-MCPTT-emergency-call>{emergency_val}</mcpttgi:allow-MCPTT-emergency-call>
          <mcpttgi:allow-imminent-peril-call>{imminent_val}</mcpttgi:allow-imminent-peril-call>
          <mcpttgi:allow-MCPTT-emergency-alert>{alert_val}</mcpttgi:allow-MCPTT-emergency-alert>
          <mcpttgi:on-network-allow-conference-state>{conf_state_val}</mcpttgi:on-network-allow-conference-state>"""
    if mcvideo_attrs is not None:
        xml += _mcvideo.actions_xml(mcvideo_attrs)
    # 멤버의 그룹 데이터 송신 인가(§7.2.2 MCData actions b)) — 없으면 false 라 규격 단말·제어 기능이 «이 그룹에는 아무도 못 보낸다»
    #   로 읽는다(TS 24.282 §11.1 2)). CIMS 의 송신 권한은 멤버 단위로 가르지 않는다 — 멤버면 보낸다.
    if has_mcdata:
        xml += """
          <mcpttgi:mcdata-allow-transmit-data-in-this-group>true</mcpttgi:mcdata-allow-transmit-data-in-this-group>"""
    # MCPTT <service> — enabler = MCPTT ICSI (TS 24.481 §7.2.2, ICSI 는 TS 24.379). MCVideo 그룹이면 MCVideo <service> 를 더한다.
    xml += f"""
        </cp:actions>
      </cp:rule>
    </cp:ruleset>
    <oxe:supported-services>
     <oxe:service enabler="{_mcvideo.ICSI_MCPTT}">
      <oxe:group-media>
       <mcpttgi:mcptt-speech/>
      </oxe:group-media>
     </oxe:service>"""
    if mcvideo_attrs is not None:
        xml += _mcvideo.service_xml()
    # MCData 서비스 enabler (TS 24.481 §7.2.2 — ICSI 값은 TS 24.282 §6.2.1.1)
    if group.get('allow_sds', True):
        xml += """
     <oxe:service enabler="urn:urn-7:3gpp-service.ims.icsi.mcdata.sds"/>"""
    if group.get('allow_fd', False):
        xml += """
     <oxe:service enabler="urn:urn-7:3gpp-service.ims.icsi.mcdata.fd"/>"""
    xml += """
    </oxe:supported-services>"""
    if org_code:
        xml += f"""
    <mcpttgi:org-code>{esc(org_code)}</mcpttgi:org-code>"""
    # 그룹 소유 (3GPP TS 23.280 authorized user = 관리주체)
    authorized_user = group.get('authorized_user', '')
    if authorized_user:
        xml += f"""
    <mcpttgi:authorized-user>{esc(authorized_user)}</mcpttgi:authorized-user>"""
    xml += """
  </list-service>
</group>"""
    return xml, _content_etag(xml)

def get_user_profile(msisdn):
    """사용자 MCPTT 프로파일 조회 — 부재 시 기본값 (모드 DedicatedGroup·긴급그룹 미지정·인가 전부 허용)."""
    return PTT_PROFILES.get(msisdn, DEFAULT_USER_PROFILE)


def update_user_profile_cache(ptt_id, profile):
    """admin PUT 반영 — profile=None 이면 캐시 제거(행 삭제). user-profile ETag 는 내용 파생이라 자동 갱신."""
    if profile is None:
        PTT_PROFILES.pop(ptt_id, None)
    else:
        PTT_PROFILES[ptt_id] = profile


def get_service_config():
    """MCPTT 시스템 서비스 설정 (TS 24.484) — DB 사본의 복제본을 준다(호출자 수정 방지)."""
    return dict(SERVICE_CONFIG)


def update_service_config_cache(cfg):
    """admin PUT 반영 — service-config ETag 는 내용 파생이라 값이 바뀌면 자동 갱신된다."""
    SERVICE_CONFIG.update(cfg)


def service_config_opt_cols(cur) -> tuple:
    """mcptt_service_config 에 있는 선택 열(SERVICE_CONFIG_OPT_COLS) — 마이그레이션 전 DB 는 빈 튜플."""
    cur.execute("SHOW COLUMNS FROM mcptt_service_config LIKE 'max_calls_n6%'")
    have = {r['Field'] if isinstance(r, dict) else r[0] for r in cur.fetchall()}
    return tuple(c for c in SERVICE_CONFIG_OPT_COLS if c in have)


# «관제» 판정(D2) — 그 PTT 회선의 사람(ptt_subscriptions.user_id)에게 역할 배정(role_assignments principal_type='user', roles 행이
#   있는 것)이 있다. CSP 의 판정(CCspRoleMap::SelectForLine — DbManager::LoadAllRoles 가 배정을 그 사람의 회선으로 펼친다)과
#   같은 표·같은 펼침이다. 판정 데이터는 역할 배정 한 곳이고 N6 를 따로 적어 두지 않는다(mcptt_authorization.md §2.4).
_DISPATCH_LINE_SQL = (
    "SELECT 1 FROM role_assignments a JOIN roles r ON r.id = a.role_id "
    "JOIN ptt_subscriptions s ON CAST(s.user_id AS CHAR) = a.principal_id "
    "WHERE a.principal_type='user' AND s.id=%s LIMIT 1")


def is_dispatch_line(msisdn: str) -> bool:
    """PTT 회선 msisdn 의 사람이 역할을 배정받았는가. DB 없음·표 미적용·조회 실패 = False(그 밖 단말)."""
    conn = _db_connect()
    if conn is None or not msisdn:
        return False
    try:
        with conn:
            with conn.cursor() as cur:
                cur.execute(_DISPATCH_LINE_SQL, (msisdn,))
                return cur.fetchone() is not None
    except Exception as e:
        if '1146' not in str(e):            # ER_NO_SUCH_TABLE — 역할 표 미적용 DB 는 조용히 «그 밖»
            logger.log_error(f"[CMS] dispatch role lookup for {msisdn} failed: {e}")
        return False


def user_max_calls_n6(msisdn: str) -> int:
    """사용자의 N6(<MaxSimultaneousCallsN6>, TS 24.484 §8.3.2.1 8)e)i)) — 관제(역할 배정) = max_calls_n6_dispatch, 그 밖 =
    max_calls_n6(mcptt_service_config, 기본 10·5). CSP 는 같은 두 값과 같은 판정으로 486 + 103 을 집행한다(TS 24.379 §10.1.1.3.1.1 5))."""
    key = "max_calls_n6_dispatch" if is_dispatch_line(msisdn) else "max_calls_n6"
    try:
        return max(1, int(SERVICE_CONFIG.get(key) or SERVICE_CONFIG_DEFAULTS[key]))   # xs:positiveInteger
    except (TypeError, ValueError):
        return SERVICE_CONFIG_DEFAULTS[key]


# ── MCPTT user profile 규격 파라미터값 (config UserProfile.*) — 문서 상수 요소. 빈/미지정 = 코드 기본값.
USER_PROFILE_CONFIG = {}
#   N6(<MaxSimultaneousCallsN6>)는 여기 없다 — 사용자마다의 값이라 mcptt_service_config 두 값과 역할 배정에서 낸다(user_max_calls_n6).
_USER_PROFILE_DEFAULTS = {
    "MaxSimultaneousTransmissionsN7": 1,  # 동시 송신 상한 (OnNetwork)
    "Priority": 0,                        # 사용자 우선순위 (unsignedShort)
    "MissionCriticalOrganization": "",    # 빈값 = UeInitConfig.Name
    # 아래 둘은 XSD·§8.3.2.1 상 **선택**이지만 필수로 읽는 단말이 있어 항상 싣는다(규격 위반 아님).
    "ParticipantType": "user",            # 사용자의 기능 범주 (§8.3.2.7 예: first responder·dispatch)
    "Language": "en",                     # <Name>·<alias-entry> 의 xml:lang — 한 문서 안에서 같은 값
}


def _user_profile_cfg(key):
    v = USER_PROFILE_CONFIG.get(key) if isinstance(USER_PROFILE_CONFIG, dict) else None
    if v is None or (isinstance(v, str) and not v.strip()):
        return _USER_PROFILE_DEFAULTS[key]
    return v


def get_user_profile_xml(user_uri, owner_uid=None):
    """MCPTT user profile 문서 (TS 24.484 §8.3.2, ns urn:3gpp:mcptt:user-profile:1.0).

    규격 단말은 로그인 뒤 이 문서에서 **그룹 목록·연락처·긴급 대상·개시 인가**를 읽는다. 소스는 전부 기존 정본:
      - 그룹 = GROUPS 멤버십(GMS 문서 URI 와 같은 키) → <OnNetwork><MCPTTGroupInfo>(제휴 가능 그룹) ·
        <ImplicitAffiliations>(= 멤버 implicit_affiliation 이 켜진 그룹만 — 서버가 등록 때 제휴한다). 소유(authorized_user_id ==
        owner_uid)한 소속 그룹은 entry anyExt 에 cims:authorized-user 표시(단말 편집·삭제 노출 근거). 소유만 하고
        멤버가 아닌 그룹은 서비스 목록이 아니라 싣지 않는다(관리 목록 = GMS JSON/관리 API 몫).
      - 연락처 = 내 그룹의 동료 멤버 → <Common><PrivateCall><PrivateCallList>(그룹 문서로 이미 보이는 범위라 추가 노출 없음).
      - 긴급 = ptt_user_profile(emergency_group_mode/id·private_emergency_mode/recipient) → MCPTT-group-call 의
        EmergencyCall/ImminentPerilCall/EmergencyAlert, PrivateCall 의 EmergencyCall(MCPTTPrivateRecipient), OnNetwork 의
        PrivateEmergencyAlert. §8.3.2.1 8d)ii·8e)ii~iv·10f) 가 "shall" 로 요구하는 요소라 **항상 싣는다** — 대상이
        미지정이면 entry-info 로 표현한다: 그룹은 `UseCurrentlySelectedGroup`(uri-entry = 폴백 그룹: 지정 그룹 > 첫 소속
        그룹, §8.3.2.7), 사설은 `LocallyDetermined`(uri-entry = 폴백 수신자: 지정 수신자 > 첫 연락처). 개시 **인가**는
        요소 유무가 아니라 <cp:ruleset> 의 allow-* 가 말한다 — DedicatedGroup 모드에 긴급그룹 미지정이면 그룹 긴급
        개시·경보를, UsePreConfigured 모드에 수신자 미지정이면 긴급 사설콜을 false 로 내린다(CSP 의 403 판정과 일치,
        mcptt_emergency_modes.md). MCPTTPrivateRecipient 는 XSD sequence 상 ProSeUserID-entry(User-Info-ID 6옥텟 hex)가
        필수 자식이라 off-network 미지원인 우리는 영값(000000000000)을 싣는다.
      - 상한 = mcptt_service_config.max_affiliations_n2(MaxAffiliationsN2) · N6 = user_max_calls_n6(관제 = 역할 배정 → 
        max_calls_n6_dispatch, 그 밖 → max_calls_n6 — CSP 집행과 같은 판정) + UserProfile.*(N7·Priority·조직명).
      - 인가 = <cp:ruleset>(RFC 4745 common-policy) — actions 자식은 규격 요소(§8.3.2.1 11) 목록 순: 개별 호 발신·수동/자동 개시·
        강제 자동 응답 → 긴급 그룹콜·긴급 사설콜 개시 → 그룹 긴급 해제·긴급 사설콜 해제(= 긴급 사설콜 개시 인가) → 임박 위험 호(= 긴급
        그룹콜 인가 — 긴급·임박은 한 게이트)·임박 위험 해제 → 경보 발령·경보 취소 → 개별 호 any-user·착신 참가 → anyExt(K 착신
        any-user · L · R · S 애드혹 참가 true · AA 애드혹 참가자 변경 false — 그 절차가 없다)) + cims 확장(그룹 생성·PTT 그룹 호 청취 자격
        <cims:allow-ambient-listening> — 비멤버 관제사의 recvonly 합류 자격(dispatch_center.md §5.6)이라 규격 ambient listening
        (TS 24.379 §11 원격 개시 1:1 호, §8.3.2.1 11)xxxviii)C)·D))과 다른 것이다). 해제 인가
        <allow-cancel-group-emergency>(기본 false — 서버 판정은 local policy 개시자 ∨ 이 값, TS 24.379 §6.3.3.1.13.4)·
        <allow-cancel-imminent-peril>(기본 true, §6.3.3.1.13.6)·<allow-cancel-emergency-alert>(부재 = 발령 인가 값,
        §6.3.3.1.13.3)은 ptt_user_profile allow_cancel_* 그대로 — 개시 인가와 달리 대상 결정 가능 여부와 AND 하지 않는다
        (해제는 이미 선 긴급 상태·경보가 대상이다). ad hoc 인가는
        규격 <anyExt><allow-adhoc-group-call>(TS 24.484 §8.3.2.1 11)xxxviii)R), Rel-18). <cims:allow-adhoc-group-call> 은
        옛 ptt-client 가 읽는 전환기 별칭 — 단말 SDK 이식(ptt-client P3) 뒤 뺀다. 같은 anyExt 에 미응답 멤버 알림 자격
        <allow-to-receive-non-acknowledged-users-information>(11)xxxviii)L), 표 8.3.2.7-49 — 값 = allow_non_ack_users_info,
        controlling 기능이 개시자에게 확인 통화 미응답 멤버 INFO 를 보낼지, TS 24.379 §6.3.3.3)를 목록 순서대로 앞에 싣는다.
    루트 <Status>true</Status>(§8.3.2.1 3, 프로파일 활성). **선택이지만 필수로 읽는 단말이 있어 항상 싣는 것** =
    alias-entry 의 index·xml:lang 속성, <ParticipantType>(§8.3.2.1 f, 값 = UserProfile.ParticipantType 설정).
    xml:lang 은 <Name> 과 같은 UserProfile.Language 를 써 한 문서 안에서 어긋나지 않게 한다.
    텍스트는 전부 escape. ETag 는 내용 파생."""
    user = USERS.get(user_uri)
    if not user:
        return None, None
    import html as _html
    esc = lambda v: _html.escape(str(v if v is not None else ''), quote=True)

    display_name = user.get('name') or user_uri
    prof = get_user_profile(user.get('msisdn', ''))
    mode = prof.get('emergency_group_mode') or 'DedicatedGroup'
    egid = prof.get('emergency_group_id')
    pmode = prof.get('private_emergency_mode') or 'LocallyDetermined'
    precip = prof.get('emergency_private_recipient')

    def et(tag, uri, name=None, info=None, ext='', index=None):
        """EntryType — sequence(uri-entry, display-name?, anyExt?) + entry-info·index 속성. <entry> 는 "index" 를 가져야 한다
        (§8.3.2.1 «The <entry> elements: 2) shall contain an "index" attribute» — 목록 안에서 유일하면 된다, §8.3.2.7)."""
        a = f' index="{index}"' if index is not None else ''
        a += f' entry-info="{esc(info)}"' if info else ''
        dn = f'<display-name>{esc(name)}</display-name>' if name else ''
        return f'<{tag}{a}><uri-entry>{esc(uri)}</uri-entry>{dn}{ext}</{tag}>'

    # 소속 그룹(멤버) — 소유(authorized_user)만으로는 목록에 넣지 않는다(_is_group_member 와 다른 기준).
    my_groups = sorted(((g_uri, g) for g_uri, g in GROUPS.items()
                        if any(_uri_eq(m.get('uri'), user_uri) for m in g.get('members', []))),
                       key=lambda x: x[0])
    owner_ext = '<anyExt><cims:authorized-user>true</cims:authorized-user></anyExt>'
    group_entries = ''.join(
        et('entry', g_uri, g.get('display_name'),
           ext=owner_ext if (owner_uid is not None and g.get('authorized_user_id') == owner_uid) else '', index=i)
        for i, (g_uri, g) in enumerate(my_groups, 1))
    # 암시적 제휴 = 관리자가 이 사용자·그룹에 정한 것만(멤버 implicit_affiliation) — 참여 기능이 서비스 인가 때 이 목록에
    #   제휴를 기록한다(TS 24.379 §7.3.2 13) → §9.2.2.2.15, CSP _ApplyImplicitAffiliations). 소속 전부가 아니다.
    implicit_entries = ''.join(
        et('entry', g_uri, g.get('display_name'), index=i) for i, (g_uri, g) in enumerate(
            [(g_uri, g) for g_uri, g in my_groups
             if any(_uri_eq(m.get('uri'), user_uri) and m.get('implicit_affiliation') for m in g.get('members', []))], 1))

    # 연락처 = 동료 멤버(본인 제외, 정규화 키로 중복 제거, URI 순 — ETag 안정)
    contacts = {}
    for _, g in my_groups:
        for mbr in g.get('members', []):
            u = mbr.get('uri') or ''
            if not u or _uri_eq(u, user_uri):
                continue
            contacts.setdefault(_norm_mcptt_uri(u), (u, mbr.get('name') or u))
    contact_entries = ''.join(et('PrivateCallURI', u, n) for _, (u, n) in sorted(contacts.items()))

    # 긴급 그룹 대상 entry — 항상 존재(§8.3.2.1 8e). 전용 그룹이 지정돼 있으면 DedicatedGroup, 아니면(모드가
    #   UseCurrentlySelectedGroup 이거나 DedicatedGroup 인데 미지정) UseCurrentlySelectedGroup + 폴백 uri-entry
    #   (지정 그룹 > 첫 소속 그룹 > 본인 URI — 소속 그룹이 없는 퇴화 케이스, 그룹콜 자체가 불가하므로 무해).
    dedicated = bool(mode == 'DedicatedGroup' and egid)
    eg_uri = _group_uri(egid) if egid else (my_groups[0][0] if my_groups else user_uri)
    eg_entry = et('entry', eg_uri, GROUPS.get(eg_uri, {}).get('display_name'),
                  'DedicatedGroup' if dedicated else 'UseCurrentlySelectedGroup', index=1)
    # 긴급 사설콜 수신자 entry — 항상 존재(§8.3.2.1 8d·10f). 사전 지정이면 UsePreConfigured, 아니면 LocallyDetermined
    #   + 폴백 uri-entry(§8.3.2.7: 선택 상대가 없을 때 쓰는 값 — 지정 수신자 > 첫 연락처 > 본인 URI(퇴화)).
    preconfigured = bool(pmode == 'UsePreConfigured' and precip)
    first_contact = sorted(contacts.items())[0][1][0] if contacts else None
    pr_uri = f"tel:{precip}" if precip else (first_contact or user_uri)
    pr_entry = et('entry', pr_uri, None, 'UsePreConfigured' if preconfigured else 'LocallyDetermined', index=1)
    # ProSe(off-network) 미지원 — MCPTTPrivateRecipientEntryType 의 필수 자식 ProSeUserID-entry 는 User-Info-ID 영값
    #   (6옥텟 hex, §8.3.2.7 "shall be 6 octets")로 채운다. "index" 속성은 필수다(§8.3.2.1 «The <ProSeUserID-entry> elements: 4)»).
    prose_entry = '<ProSeUserID-entry index="1"><User-Info-ID>000000000000</User-Info-ID></ProSeUserID-entry>'

    def _b(k, default=True):
        return "true" if prof.get(k, default) else "false"

    # 인가 = 운영자 allow_* AND 대상 결정 가능 여부 — DedicatedGroup 모드에 긴급그룹 미지정이면 그룹 긴급 개시·경보,
    #   UsePreConfigured 모드에 수신자 미지정이면 긴급 사설콜을 미인가로 내린다(CSP 의 403 판정과 같은 규칙,
    #   mcptt_emergency_modes.md §5). 요소 생략이 아니라 ruleset 이 인가를 말하는 것이 규격의 방식이다.
    group_target_ok = not (mode == 'DedicatedGroup' and not egid)
    private_target_ok = not (pmode == 'UsePreConfigured' and not precip)

    def _ba(k, ok, default=True):
        return "true" if (prof.get(k, default) and ok) else "false"

    # 해제 인가는 대상 결정 가능 여부와 AND 하지 않는다 — 이미 선 긴급 상태·경보를 푸는 자격이다. 경보 취소의 부재 시 값 =
    #   발령 인가(옛 캐시 항목·컬럼 미적용 DB 가 종전 문서와 같은 값을 낸다).
    cancel_alert_dflt = user_profile_opt_default('allow_cancel_emergency_alert', prof)
    # 개별 호 인가(§8.3.2.1 11)vii)~x)·xxvii)·xxix)·anyExt K)) — 요소가 없으면 false(표 8.3.2.7-7·-27·-48)라 늘 싣는다.
    #   발신 = allow_private_call, 수동·자동 개시는 발신 인가를 따르고(따로 가르지 않는다), 강제 자동 응답(Priv-Answer-Mode)은 주지 않는다.
    #   any-user = 발신 인가 ∧ allow_private_call_to_any_user(false 면 상대는 PrivateCallList 로 한정). 착신 = allow_private_call_participation,
    #   IncomingPrivateCallList 를 두지 않으므로 «누구에게서나 받는다»(allow-to-receive-private-call-from-any-user)도 같은 값.
    priv_out = _b('allow_private_call')
    priv_any = "true" if (prof.get('allow_private_call', True) and prof.get('allow_private_call_to_any_user', True)) else "false"
    priv_in = _b('allow_private_call_participation')

    org = str(_user_profile_cfg('MissionCriticalOrganization') or _ue_init_cfg('Name') or _UE_INIT_DEFAULTS['Name'])
    ptype = str(_user_profile_cfg('ParticipantType'))
    lang = str(_user_profile_cfg('Language'))
    n6 = user_max_calls_n6(user.get('msisdn', ''))
    n7 = int(_user_profile_cfg('MaxSimultaneousTransmissionsN7'))
    prio = int(_user_profile_cfg('Priority'))
    n2 = int(SERVICE_CONFIG.get('max_affiliations_n2') or 0)

    # <Common>
    # alias-entry 의 index·xml:lang 은 XSD 상 선택(IndexType use 없음, xml:lang 은 ref)이지만 필수로 읽는 단말이
    #   있어 병기한다(규격 위반 아님 — 선택 속성을 채우는 것뿐). lang 은 <Name> 과 같은 값이라 문서가 자기모순이 없다.
    common = f'<UserAlias><alias-entry index="1" xml:lang="{esc(lang)}">{esc(display_name)}</alias-entry></UserAlias>'
    common += et('MCPTTUserID', user_uri)
    # PrivateCall 은 항상(8d "shall include one"). PrivateCallList 는 "one or more" 라 연락처가 없으면 폴백 수신자
    #   (지정 수신자 > 본인 URI — 퇴화) 하나를 싣는다. MCPTTPrivateCallType 은 sequence: PrivateCallList → EmergencyCall.
    pc_list = contact_entries or et('PrivateCallURI', pr_uri)
    pc_emerg = f'<EmergencyCall><MCPTTPrivateRecipient>{pr_entry}{prose_entry}</MCPTTPrivateRecipient></EmergencyCall>'
    common += f'<PrivateCall><PrivateCallList>{pc_list}</PrivateCallList>{pc_emerg}</PrivateCall>'
    gc = f'<MaxSimultaneousCallsN6>{n6}</MaxSimultaneousCallsN6>'
    # EmergencyCall 을 ImminentPerilCall 앞에 — 첫 MCPTTGroupInitiation 을 SOS 대상으로 읽는 단말 호환
    gc += f'<EmergencyCall><MCPTTGroupInitiation>{eg_entry}</MCPTTGroupInitiation></EmergencyCall>'
    gc += f'<ImminentPerilCall><MCPTTGroupInitiation>{eg_entry}</MCPTTGroupInitiation></ImminentPerilCall>'
    gc += f'<EmergencyAlert>{eg_entry}</EmergencyAlert>'
    gc += f'<Priority>{prio}</Priority>'
    common += f'<MCPTT-group-call>{gc}</MCPTT-group-call>'
    # f) may contain one <ParticipantType> — 선택이지만 필수로 읽는 단말이 있어 항상 싣는다. 값은 사이트 단위
    #   설정(UserProfile.ParticipantType) — 역할(관제사 등) 단위 파생은 DB 조회가 필요해 두지 않았다.
    common += f'<ParticipantType>{esc(ptype)}</ParticipantType>'
    common += f'<MissionCriticalOrganization>{esc(org)}</MissionCriticalOrganization>'

    # <OnNetwork>
    # 10)b) «shall include one <MCPTTGroupInfo>» — 소속 그룹이 없어도 요소는 싣는다(ListEntryType 은 빈 내용을 허용, XSD).
    on = f'<MCPTTGroupInfo>{group_entries}</MCPTTGroupInfo>'
    on += f'<MaxAffiliationsN2>{n2}</MaxAffiliationsN2>'
    if implicit_entries:
        on += f'<ImplicitAffiliations>{implicit_entries}</ImplicitAffiliations>'
    on += f'<MaxSimultaneousTransmissionsN7>{n7}</MaxSimultaneousTransmissionsN7>'
    on += f'<PrivateEmergencyAlert>{pr_entry}</PrivateEmergencyAlert>'   # 10f) "shall include one"

    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<mcptt-user-profile xmlns="urn:3gpp:mcptt:user-profile:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:cims="urn:cims:mcptt:ext:1.0"
  XUI-URI="{esc(user_uri)}" user-profile-index="1">
  <Name xml:lang="{esc(lang)}">{esc(display_name)}</Name>
  <Status>true</Status>
  <Common index="1">{common}</Common>
  <cp:ruleset>
    <cp:rule id="mcptt-user-authorisation">
      <cp:actions>
        <allow-private-call>{priv_out}</allow-private-call>
        <allow-manual-commencement>{priv_out}</allow-manual-commencement>
        <allow-automatic-commencement>{priv_out}</allow-automatic-commencement>
        <allow-force-auto-answer>false</allow-force-auto-answer>
        <allow-emergency-group-call>{_ba('allow_emergency_call', group_target_ok)}</allow-emergency-group-call>
        <allow-emergency-private-call>{_ba('allow_emergency_private_call', private_target_ok)}</allow-emergency-private-call>
        <allow-cancel-group-emergency>{_b('allow_cancel_group_emergency', False)}</allow-cancel-group-emergency>
        <allow-cancel-private-emergency-call>{_b('allow_emergency_private_call')}</allow-cancel-private-emergency-call>
        <allow-imminent-peril-call>{_ba('allow_emergency_call', group_target_ok)}</allow-imminent-peril-call>
        <allow-cancel-imminent-peril>{_b('allow_cancel_imminent_peril', True)}</allow-cancel-imminent-peril>
        <allow-activate-emergency-alert>{_ba('allow_emergency_alert', group_target_ok)}</allow-activate-emergency-alert>
        <allow-cancel-emergency-alert>{_b('allow_cancel_emergency_alert', cancel_alert_dflt)}</allow-cancel-emergency-alert>
        <allow-private-call-to-any-user>{priv_any}</allow-private-call-to-any-user>
        <allow-private-call-participation>{priv_in}</allow-private-call-participation>
        <anyExt>  <!-- TS 24.484 §8.3.2.1 11)xxxviii) — 자식 순서는 그 목록(K → L → R → S → AA) 순 -->
          <allow-to-receive-private-call-from-any-user>{priv_in}</allow-to-receive-private-call-from-any-user>
          <allow-to-receive-non-acknowledged-users-information>{_b('allow_non_ack_users_info', False)}</allow-to-receive-non-acknowledged-users-information>
          <allow-adhoc-group-call>{_b('allow_adhoc_call')}</allow-adhoc-group-call>
          <allow-adhoc-group-call-participation>true</allow-adhoc-group-call-participation>
          <allow-to-modify-adhoc-group-call-participants-info>false</allow-to-modify-adhoc-group-call-participants-info>
        </anyExt>
        <cims:allow-adhoc-group-call>{_b('allow_adhoc_call')}</cims:allow-adhoc-group-call>
        <cims:allow-create-group>{_b('allow_create_group', False)}</cims:allow-create-group>
        <cims:allow-ambient-listening>{_b('allow_ambient_listening', False)}</cims:allow-ambient-listening>
      </cp:actions>
    </cp:rule>
  </cp:ruleset>
  <OnNetwork index="1">{on}</OnNetwork>
</mcptt-user-profile>"""
    return xml, _content_etag(xml)

def _svc_param(section, key):
    """ServiceConfig.<section>.<key> — 설정 → 코드 기본값 순. 빈 문자열은 미지정."""
    cur = (SERVICE_CONFIG_PARAMS.get(section) or {}) if isinstance(SERVICE_CONFIG_PARAMS.get(section), dict) else {}
    v = cur.get(key)
    if v is None or (isinstance(v, str) and not v.strip()):
        return _SERVICE_CONFIG_PARAM_DEFAULTS[section][key]
    return v


def _xs_duration(ms) -> str:
    """밀리초 → xs:duration 초 표기 PT<n>S (TS 24.484 §8.4.2.6 — 소수 허용, 예 PT0.5S)."""
    try:
        v = max(0, int(ms))
    except (TypeError, ValueError):
        v = 0
    return f"PT{v // 1000}S" if v % 1000 == 0 else f"PT{v / 1000:g}S"


def get_service_config_xml(user_uri):
    """MCPTT service configuration 문서 (TS 24.484 §8.4) — **시스템 전역** 1건을 XML 로 산출한다.

    구조 = §8.4.2.1·§8.4.2.3 스키마: <service-configuration-info> › <service-configuration-params domain> ›
      <common><broadcast-group>(계층 수) · <on-network>(스키마 순: <emergency-call><group-time-limit> 선택 — 설정
      ServiceConfig.EmergencyCall.GroupTimeLimit 이 0·빈 값·잘못된 값이면 요소째 생략(TNG2 미가동, TS 24.379 §6.3.3.1.16) ·
      <private-call> 선택 — 개별 호 <hang-time>(T4)·<max-duration-with-floor-control>·<max-duration-without-floor-control>,
      값이 0 인 자식은 싣지 않고 셋 다 0 이면 요소째 생략 · <transmit-time><time-limit> · <fc-timers-counters> 필수 ·
      <signalling-protection> 둘 다 false(요소가 없으면 true 로 읽혀 단말이 mcptt-info 를 CSK 로 암호화·서명한다 — §8.4.2.6 ·
      TS 24.379 §6.6.2.3.1·§6.6.3.3.1. CIMS 는 시그널링 XML 보호를 하지 않고 구간 보호는 SIP TLS 다 — MCVideo 문서와 같다) ·
      <protection-between-mcptt-servers> 둘 다 false(서버 간 보호 — 없으면 true, §8.4.2.6 NOTE 4. CIMS 는 서버 간 연동을 하지 않는다) ·
      <emergency-/imminent-peril-/normal-resource-priority> 필수 — 각 <resource-priority-namespace>·<resource-priority-priority> ·
      <anyExt><adhoc-group-call> — 필수 자식 <allow-adhoc-group-call-support>·<max-no-participants> 뒤에 <hang-time>(T4)·
      <broadcast-hang-time>(일제 애드혹 호의 T4)·<max-duration-of-call>(TNG3), 값이 0 인 시간 요소는 싣지 않는다).
    사용자마다 달라지지 않으므로 user_uri 는 호출자 인가(self-access 검증)에만 쓰인다. ETag 는 내용 파생이라 값이 바뀌면
    자동 갱신되고, 단말은 xcap-diff(cms) NOTIFY 로 재조회한다.
    """
    import html as _html
    esc = lambda v: _html.escape(str(v if v is not None else ''), quote=True)
    cfg = SERVICE_CONFIG

    def _i(k):
        return int(cfg.get(k, SERVICE_CONFIG_DEFAULTS[k]))

    from services import access_services as _access_services
    domain = (_access_services.ptt_domain(PROVISIONING) or IDMS_DOMAIN).strip()

    fc = []
    for k in _SERVICE_CONFIG_PARAM_DEFAULTS["FcTimersCounters"]:
        v = _svc_param("FcTimersCounters", k)
        if k.startswith("T"):
            val = _xs_duration(v)
        else:
            try:
                val = max(0, min(65535, int(v)))            # xs:unsignedShort
            except (TypeError, ValueError):
                val = _SERVICE_CONFIG_PARAM_DEFAULTS["FcTimersCounters"][k]
        fc.append(f"        <{k}>{val}</{k}>")
    ns = _svc_param("ResourcePriority", "Namespace")

    def _rp(elem, key):
        return (f"      <{elem}>\n"
                f"        <resource-priority-namespace>{esc(ns)}</resource-priority-namespace>\n"
                f"        <resource-priority-priority>{esc(_svc_param('ResourcePriority', key))}</resource-priority-priority>\n"
                f"      </{elem}>")

    # <emergency-call> 은 on-network 시퀀스의 첫 자식(선택) — 값이 없으면 요소째 뺀다(빈 <emergency-call> 을 싣지 않는다)
    try:
        eg_limit = int(_svc_param("EmergencyCall", "GroupTimeLimit"))
    except (TypeError, ValueError):
        eg_limit = 0
    emerg = (f"      <emergency-call>\n"
             f"        <group-time-limit>{_xs_duration(eg_limit)}</group-time-limit>\n"
             f"      </emergency-call>\n") if eg_limit > 0 else ""

    def _ms(section, key):
        try:
            return max(0, int(_svc_param(section, key)))
        except (TypeError, ValueError):
            return 0

    def _durations(section, pairs, indent):
        # 시간 요소 — 0 은 «그 타이머를 돌리지 않는다» 라 요소를 싣지 않는다(PT0S 는 «0초» 로 읽힌다)
        return "".join(f"{indent}<{elem}>{_xs_duration(_ms(section, key))}</{elem}>\n"
                       for elem, key in pairs if _ms(section, key) > 0)

    # <private-call> — on-network 시퀀스에서 <emergency-call> 다음(§8.4.2.3 on-networkType). 자식이 없으면 요소째 뺀다.
    priv = _durations("PrivateCall", (("hang-time", "HangTime"),
                                      ("max-duration-with-floor-control", "MaxDurationWithFloorControl"),
                                      ("max-duration-without-floor-control", "MaxDurationWithoutFloorControl")),
                      "        ")
    priv = f"      <private-call>\n{priv}      </private-call>\n" if priv else ""
    # <anyExt><adhoc-group-call> — on-network 의 마지막 자식(§8.4.2.1 on-network 13)d)). adhoc-group-callType 은 앞의 두 자식이
    #   필수다(minOccurs 기본 1) — max-no-participants 는 xs:positiveInteger.
    try:
        adhoc_max = max(1, min(65535, int(_svc_param("AdhocGroupCall", "MaxNoParticipants"))))
    except (TypeError, ValueError):
        adhoc_max = _SERVICE_CONFIG_PARAM_DEFAULTS["AdhocGroupCall"]["MaxNoParticipants"]
    adhoc = (f"      <anyExt>\n"
             f"        <adhoc-group-call>\n"
             f"          <allow-adhoc-group-call-support>{_xml_bool(_svc_param('AdhocGroupCall', 'AllowSupport'))}"
             f"</allow-adhoc-group-call-support>\n"
             f"          <max-no-participants>{adhoc_max}</max-no-participants>\n"
             + _durations("AdhocGroupCall", (("hang-time", "HangTime"), ("broadcast-hang-time", "BroadcastHangTime"),
                                             ("max-duration-of-call", "MaxDurationOfCall")), "          ") +
             f"        </adhoc-group-call>\n"
             f"      </anyExt>\n")

    nl = "\n"
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<service-configuration-info xmlns="urn:3gpp:ns:mcpttServiceConfig:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <service-configuration-params domain="{esc(domain)}">
    <common>
      <broadcast-group>
        <num-levels-group-hierarchy>{_i('num_levels_group_hierarchy')}</num-levels-group-hierarchy>
        <num-levels-user-hierarchy>{_i('num_levels_user_hierarchy')}</num-levels-user-hierarchy>
      </broadcast-group>
    </common>
    <on-network>
{emerg}{priv}      <transmit-time>
        <time-limit>{_xs_duration(_svc_param('TransmitTime', 'TimeLimit'))}</time-limit>
      </transmit-time>
      <fc-timers-counters>
{nl.join(fc)}
      </fc-timers-counters>
      <signalling-protection>
        <confidentiality-protection>false</confidentiality-protection>
        <integrity-protection>false</integrity-protection>
      </signalling-protection>
      <protection-between-mcptt-servers>
        <allow-signalling-protection>false</allow-signalling-protection>
        <allow-floor-control-protection>false</allow-floor-control-protection>
      </protection-between-mcptt-servers>
{_rp('emergency-resource-priority', 'Emergency')}
{_rp('imminent-peril-resource-priority', 'ImminentPeril')}
{_rp('normal-resource-priority', 'Normal')}
{adhoc}    </on-network>
  </service-configuration-params>
</service-configuration-info>"""
    return xml, _content_etag(xml)

def _ue_init_cfg(*path, default=None):
    """UeInitConfig.* 조회 — 설정 → 코드 기본값 순. 빈 문자열은 '미지정' 으로 취급(유도값 사용)."""
    cur, dft = UE_INIT_CONFIG, _UE_INIT_DEFAULTS
    for p in path:
        cur = cur.get(p) if isinstance(cur, dict) else None
        dft = dft.get(p) if isinstance(dft, dict) else None
    if cur is None or (isinstance(cur, str) and not cur.strip()):
        return dft if default is None else default
    return cur


def _xml_bool(v) -> str:
    if isinstance(v, str):
        return 'true' if v.strip().lower() in ('1', 'true', 'yes', 'on') else 'false'
    return 'true' if bool(v) else 'false'


def _xml_ubyte(v, dft) -> int:
    """xs:unsignedByte — 정수 아니면 기본값, 범위는 0~255 로 절단."""
    try:
        n = int(v)
    except (TypeError, ValueError):
        n = int(dft)
    return max(0, min(255, n))


def derive_hplmn(configured: str, domain: str) -> str:
    """<HPLMN PLMN> = MCC(3자리)+MNC(2·3자리) PLMN 코드(TS 24.484 §7.2.2.7 · TS 23.003 §2.2) — 설정 UeInitConfig.Hplmn.Plmn 이 정본.

    설정이 없거나 PLMN 코드가 아니면 도메인 표기 `mnc<MNC>.mcc<MCC>.…`(TS 23.003 §13 — MNC 는 세 자리로 쓰고 두 자리 MNC 는 앞에
    0 을 하나 채운다)에서 유도한다: 세 자리 앞자리가 0 이면 그 0 하나만 떼어 두 자리 MNC 로 읽는다(`mnc008` → `08`, `mnc033` →
    `33`). 도메인 표기만으로는 두 자리와 세 자리 MNC(`mnc012` 가 `12` 인지 `012` 인지)를 가를 수 없으므로 앞자리 0 인 세 자리 MNC
    사업자는 설정에 적는다. 둘 다 없으면 시험용 명목값 00101."""
    import re as _re
    if configured:
        if _re.fullmatch(r'\d{5,6}', configured):
            return configured
        logger.log_error(f"[CMS] UeInitConfig.Hplmn.Plmn '{configured}' 는 PLMN 코드(MCC 3 + MNC 2·3자리)가 아니다 — 도메인 유도값 사용")
    mm = _re.search(r'(?:^|\.)mnc(\d{2,3})\.mcc(\d{3})(?:\.|$)', domain or '')
    if not mm:
        return '00101'
    mnc = mm.group(1)
    if len(mnc) == 3 and mnc.startswith('0'):
        mnc = mnc[1:]
    return mm.group(2) + mnc


def _build_ue_init_config_xml(base_url: str) -> str:
    """설정(UeInitConfig.*)과 토폴로지 유도값으로 ue-init-config 문서를 조립한다 (검사 전 원문)."""
    import html as _html
    import re as _re
    esc = lambda s: _html.escape(str(s if s is not None else ''), quote=True)

    from services import access_services as _access_services
    domain = (_access_services.ptt_domain(PROVISIONING) or IDMS_DOMAIN).strip()

    plmn = derive_hplmn(str(_ue_init_cfg('Hplmn', 'Plmn')).strip(), domain)

    timers = ''.join(
        f"      <{t}>{_xml_ubyte(_ue_init_cfg('Timers', t), _UE_INIT_DEFAULTS['Timers'][t])}</{t}>\n"
        for t in ('T100', 'T101', 'T103', 'T104', 'T132'))

    group_creation_xui = str(_ue_init_cfg('GroupCreationXui')).strip() or base_url

    # 계층③ 확장 요소 — <on-network><anyExt> 아래 *-Service-Details (§7.2.2.3 "can be added under anyExt").
    #   Server-URI = participating function 의 PSI. 비우면 sip:{svc}_psi@도메인 (mcptt_psi 는 CSP 의
    #   affiliation notifier PSI 그대로, mcdata_psi 는 명목값).
    ext = ''
    #   MCVideo-Service-Details = MCVideo participating function PSI(TS 24.484 §7.2.2.1 b, TS 24.281 §4.2 1)). 순서 = §7.2.2.1
    #   목록(MCPTT → MCVideo → MCData). mcvideo_psi 는 CSP MCVideo 모듈(Roles.MCVIDEO)이 받는 PSI 다.
    for elem, key, psi in (('MCPTT-Service-Details', 'Mcptt', 'mcptt_psi'),
                           ('MCVideo-Service-Details', 'McVideo', 'mcvideo_psi'),
                           ('MCData-Service-Details', 'McData', 'mcdata_psi')):
        if _xml_bool(_ue_init_cfg('ServiceDetails', key, 'Enable')) != 'true':
            continue
        uri = str(_ue_init_cfg('ServiceDetails', key, 'ServerUri')).strip() or f"sip:{psi}@{domain}"
        ext += (f"      <{elem}>\n"
                f"        <IPv6-Required>false</IPv6-Required>\n"
                f"        <Server-URI>{esc(uri)}</Server-URI>\n"
                f"      </{elem}>\n")
    any_ext = f"    <anyExt>\n{ext}    </anyExt>\n" if ext else ''

    return f"""<?xml version="1.0" encoding="UTF-8"?>
<mcptt-UE-initial-configuration xmlns="urn:3gpp:mcptt:mcpttUEinitConfig:1.0" domain="{esc(domain)}">
  <name>{esc(_ue_init_cfg('Name'))}</name>
  <on-network>
    <Timers>
{timers}    </Timers>
    <HPLMN PLMN="{esc(plmn)}">
      <service>
        <MCPTT-to-con-ref>{esc(_ue_init_cfg('Hplmn', 'McpttConRef'))}</MCPTT-to-con-ref>
        <MC-common-core-to-con-ref>{esc(_ue_init_cfg('Hplmn', 'McCommonCoreConRef'))}</MC-common-core-to-con-ref>
        <MC-ID-to-con-ref>{esc(_ue_init_cfg('Hplmn', 'McIdConRef'))}</MC-ID-to-con-ref>
      </service>
    </HPLMN>
    <App-Server-Info>
      <idms-auth-endpoint>{esc(base_url)}/idms/authreq</idms-auth-endpoint>
      <idms-token-endpoint>{esc(base_url)}/idms/tokenreq</idms-token-endpoint>
      <http-proxy>{esc(_ue_init_cfg('HttpProxy', default=''))}</http-proxy>
      <gms>{esc(base_url)}</gms>
      <cms>{esc(base_url)}</cms>
      <kms>{esc(base_url)}/keymanagement/identity/v1</kms>
      <tls-tunnel-auth-method>
        <mutual-authentication>{_xml_bool(_ue_init_cfg('TlsMutualAuthentication'))}</mutual-authentication>
      </tls-tunnel-auth-method>
    </App-Server-Info>
    <GMS-URI>sip:gms_psi@{esc(domain)}</GMS-URI>
    <group-creation-XUI>{esc(group_creation_xui)}</group-creation-XUI>
    <GMS-XCAP-root-URI>{esc(base_url)}</GMS-XCAP-root-URI>
    <CMS-XCAP-root-URI>{esc(base_url)}</CMS-XCAP-root-URI>
    <integrity-protection-enabled>{_xml_bool(_ue_init_cfg('IntegrityProtection'))}</integrity-protection-enabled>
    <confidentiality-protection-enabled>{_xml_bool(_ue_init_cfg('ConfidentialityProtection'))}</confidentiality-protection-enabled>
{any_ext}  </on-network>
</mcptt-UE-initial-configuration>"""


def get_ue_init_config_xml(base_url):
    """MCS UE 초기 설정 문서 (TS 24.484 §7.2) — **로그인 전** 부트스트랩, 시스템 전역 1건.

    구조·요소명·네임스페이스는 §7.2.2.3 XSD 정본을 그대로 따른다 — <on-network> 는
    xs:sequence 라 **요소 순서가 강제**되고 나열 요소 전부 필수(minOccurs 기본 1)다.
    값은 3계층: ①주소류(IdMS/CMS/GMS/KMS/XCAP 루트·domain·PLMN·GMS-URI)는 토폴로지 SoT
    (Provisioning.Services.ptt / IdMs / 요청 Host)에서 유도 ②규격 파라미터값(Timers·con-ref·
    http-proxy·보호 플래그·group-creation-XUI·name)은 `UeInitConfig.*` 설정 ③확장 요소
    (*-Service-Details)는 설정 on/off (§R4-1).
    - Timers = floor 절차 타이머(TS 24.380: T100 release/T101 request/T103 end-of-media/
      T104 queue-pos/T132 queued-granted 사용자 행동, 초 단위).
    - GMS-URI = GMS 구독 프록시 **PSI** (§7.2.2.7-5) — 우리 gms_psi AoR 그대로.

    산출물은 minidom 으로 well-formed 검사한다 — 설정값이 문서를 깨뜨리면(이론상 escape 로
    막히지만) 경고를 남기고 **마지막 정상 문서**를 유지한다(없으면 기본값 문서).
    """
    from xml.dom import minidom as _minidom
    xml = _build_ue_init_config_xml(base_url)
    try:
        _minidom.parseString(xml.encode('utf-8'))
    except Exception as e:
        logger.log_error(f"[CMS] ue-init-config not well-formed ({e}) — UeInitConfig 설정 점검. "
                         f"{'마지막 정상 문서 유지' if base_url in _UE_INIT_LAST_GOOD else '기본값 문서로 대체'}")
        if base_url in _UE_INIT_LAST_GOOD:
            return _UE_INIT_LAST_GOOD[base_url]
        global UE_INIT_CONFIG
        saved, UE_INIT_CONFIG = UE_INIT_CONFIG, {}
        try:
            xml = _build_ue_init_config_xml(base_url)
        finally:
            UE_INIT_CONFIG = saved
    result = (xml, _content_etag(xml))
    _UE_INIT_LAST_GOOD[base_url] = result
    return result


def _ue_init_ref_etag() -> str:
    """재적재 전후를 비교하는 UE initial configuration 문서의 ETag — 공개 base URL(McpttServer.PublicUrl) 기준.

    PublicUrl 이 없으면(올인원) 주소류가 단말의 요청 Host 로 갈려 단말마다 문서가 다르므로 IdMS 도메인 기준 문서로 비교한다 —
    그때 통지의 new-etag 는 참고값이고, 단말은 자기 사본의 ETag(If-None-Match)로 다시 받는다."""
    return get_ue_init_config_xml(_MCPTT_PUBLIC_URL or f"https://{IDMS_DOMAIN}:{_MCPTT_PORT}")[1]


def get_kms_init_xml(user_uri):
    now = datetime.datetime.now(datetime.timezone.utc)
    valid_from = now.isoformat()
    valid_to = (now + datetime.timedelta(days=365*20)).isoformat()
    
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<KmsResponse Version="1.1.0" xmlns="http://org.csc.kms" xmlns:ds="http://www.w3.org/2000/09/xmldsig#">
<KmsUri>{KMS_URI}</KmsUri>
<UserUri>{user_uri}@{IDMS_DOMAIN}</UserUri>
<Time>{now.isoformat()}</Time>
<KmsId>kmsprovider12345</KmsId>
<ClientReqUrl>{KMS_CLIENT_REQ_URL}</ClientReqUrl>
<KmsMessage>
<KmsInit Version="1.0.0">
<KmsCertificate Version="1.0.0" Role="Root">
<KmsUri>{KMS_URI}</KmsUri>
<Issuer>www.mcptt.com</Issuer>
<ValidFrom>{valid_from}</ValidFrom>
<ValidTo>{valid_to}</ValidTo>
<UserIdFormat>2</UserIdFormat>
<UserKeyPeriod>2419200</UserKeyPeriod>
<UserKeyOffset>0</UserKeyOffset>
<PubEncKey>041C7B84B4FD620D49F3DC2366A7F62F48221D7B32D61D2A16685A015FDACF03CDDBAA66B78C597410C290EE3E8D7FE950193B87DABD3A33180DCEEF66893B24504EA22C9C7FD46BDCD385AF14EC71A57F94363692FA7FE0CE931BCF7A4F95A32723A459AC0ED72ECF17A8E9E2EBF94976E493134F5D11EE3D42165B5EF6E22FDD5269CBD01D339A5768521E36E1A1BEF2EC0D4B2606943DFAFB010A806F553E81350039EABD25FBF0758F25FC38E730553C19675B796DFE005C16696B3879388547282B3A3F56ADA1EA3C01AF77DE412EA62D4676D2386F745304B8B3AD63BB8E4E01C3C342B984B57512EA58A5049CE04BA2D00A36A3C78F46A364A670DE9F64</PubEncKey>
<PubAuthKey>0467EF33902289EA2F42A82912CFD12B517A321EED22D56EB9B5AA60A3A38F97B77A29B3875339F141E454E3A9CF53A3C0353B1A88868A39A15D74A7B235E09EB8</PubAuthKey>
<ParameterSet>1</ParameterSet>
</KmsCertificate>
</KmsInit>
</KmsMessage>
</KmsResponse>"""
    return xml

# S5: 가입자별 KMS 키 material 파생 (HKDF-Expand 유사, HMAC-SHA256 기반).
#   동일 (master_secret, user_uri, label) → 동일 값(재현 가능), 사용자마다 상이.
def _kms_derive(user_uri: str, label: str, nbytes: int) -> str:
    import hmac
    out = b""
    counter = 1
    info = f"{label}:{user_uri}".encode("utf-8")
    while len(out) < nbytes:
        out += hmac.new(KMS_MASTER_SECRET, info + bytes([counter]), hashlib.sha256).digest()
        counter += 1
    return out[:nbytes].hex().upper()

def get_kms_keyprov_xml(user_uri):
    # S5: 가입자별 키 프로비저닝(TS 33.180 Annex D.3.3 KmsKeyProv 구조). 구 전사용자 고정 hex 폐기 —
    #   UserDecryptKey(SAKKE RSK)/UserSigningKeySSK(ECCSI SSK)/UserPubTokenPVT(ECCSI PVT)를 가입자별 파생.
    #   ⚠ 파생값은 구조적 placeholder(가입자별 상이·재현가능)이며 참 ECCSI/SAKKE 점은 아니다(후속 과제).
    now = datetime.datetime.now(datetime.timezone.utc)
    valid_from = "2016-11-01T00:00:00+09:00"
    valid_to = "2036-10-31T23:59:59+09:00"
    key_period = 2419200
    key_period_no = int(time.time()) // key_period
    rsk = _kms_derive(user_uri, "UserDecryptKey", 128)      # SAKKE Receiver Secret Key
    ssk = _kms_derive(user_uri, "UserSigningKeySSK", 32)    # ECCSI Secret Signing Key
    pvt = _kms_derive(user_uri, "UserPubTokenPVT", 65)      # ECCSI Public Validation Token
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<KmsResponse Version="1.1.0" xmlns="http://org.csc.kms" xmlns:ds="http://www.w3.org/2000/09/xmldsig#">
<KmsUri>{KMS_URI}</KmsUri>
<UserUri>{user_uri}@{IDMS_DOMAIN}</UserUri>
<Time>{now.isoformat()}</Time>
<KmsId>kmsprovider12345</KmsId>
<ClientReqUrl>{KMS_CLIENT_REQ_URL}</ClientReqUrl>
<KmsMessage>
<KmsKeyProv Version="1.0.0">
<KmsKeySet Version="1.0.0">
<KmsUri>{KMS_URI}</KmsUri>
<CertUri>{KMS_URI}/cert1</CertUri>
<Issuer>www.mcptt.com</Issuer>
<UserUri>{user_uri}@{IDMS_DOMAIN}</UserUri>
<UserID>{user_uri}</UserID>
<ValidFrom>{valid_from}</ValidFrom>
<ValidTo>{valid_to}</ValidTo>
<KeyPeriodNo>{key_period_no}</KeyPeriodNo>
<UserDecryptKey>{rsk}</UserDecryptKey>
<UserSigningKeySSK>{ssk}</UserSigningKeySSK>
<UserPubTokenPVT>{pvt}</UserPubTokenPVT>
</KmsKeySet>
</KmsKeyProv>
</KmsMessage>
</KmsResponse>"""
    return xml

# --- Helpers ---
def extract_token(auth_header: str) -> Optional[dict]:
    if not auth_header:
        return None
    token = auth_header.replace('Bearer ', '')
    return validate_access_token(token)

# --- Handlers ---

# ── IdMS: Auth Req — 두 말투 병행 (한 핸들러 안 분기) ──────────────────────────────
#   ① 자체 단말: GET + user_name/user_password 쿼리 → 200 JSON {code,state,Location}
#      (규격 폼 왕복을 생략한 간이형 — android/core ProvisioningClient·cspsim 이 쓴다).
#   ② 규격 단말(TS 24.482 §6.3.1 / OIDC Core §3.1.2): GET(자격 없음) → 200 text/html 로그인 폼
#      → POST(form-urlencoded: 입력칸 + hidden 문맥) → **302 Location: redirect_uri?code&state**.
#      폼은 무상태 — OIDC 문맥(client_id·redirect_uri·state·scope·nonce·PKCE)을 hidden input 으로
#      이월한다(서버 세션 없음). 입력칸 이름은 IdMs.FormLoginField/FormPasswordField (외부 SDK 의
#      헤드리스 폼 자동화가 찾는 이름).
#   두 경로는 검증(PKCE S256 필수·redirect_uri 허용목록)·인증(_authenticate)·코드 발급(_issue_auth_code)
#   을 공유하고 응답 표현만 다르다.

_OIDC_CTX_KEYS = ('client_id', 'redirect_uri', 'state', 'scope', 'nonce',
                  'code_challenge', 'code_challenge_method', 'response_type', 'acr_values')


def _oidc_ctx(src: dict) -> dict:
    """요청(query 또는 form)에서 OIDC 인증 요청 문맥만 추려 정규화한다. `_given` = 요청에 실제로 실려 온 파라미터 이름
    (기본값으로 채운 것과 구분 — 필수 파라미터 검사용)."""
    src = src if isinstance(src, dict) else {}
    ctx = {k: str(src.get(k) or '') for k in _OIDC_CTX_KEYS}
    ctx['_given'] = {k for k in _OIDC_CTX_KEYS if ctx[k]}
    ctx['client_id'] = ctx['client_id'] or 'MCPTT_UE'
    ctx['code_challenge_method'] = ctx['code_challenge_method'] or 'S256'
    return ctx


def _client_problems(client_id: str, client_given: bool, redirect_uri: str) -> list:
    """클라이언트 등록 대조(TS 33.180 B.3) — client_id 가 등록돼 있고 redirect_uri 가 그 클라이언트의 등록 값이어야 한다."""
    out = []
    if not client_given:
        out.append("client_id is required")
    elif client_id not in IDMS_CLIENTS:
        out.append(f"client_id '{client_id}' is not registered")
    elif redirect_uri and redirect_uri not in IDMS_CLIENTS[client_id]:
        out.append(f"redirect_uri '{redirect_uri}' is not registered for client '{client_id}'")
    return out


def _client_gate(stage: str, client_id: str, problems: list, error: str = "invalid_request") -> Optional[HandlerResult]:
    """클라이언트 등록·필수 파라미터 검사의 판정(IdMs.ClientEnforcement) — off=검사 없음 / log=`would-reject` 한 줄 로그 후 통과
    (어떤 client_id·redirect_uri 가 쓰이는지 모으는 창) / enforce=400. problems 가 비면 통과(None)."""
    if not problems or CLIENT_ENFORCEMENT == 'off':
        return None
    line = f"[IdMS][client] would-reject stage={stage} client_id={client_id} problems={'; '.join(problems)}"
    if CLIENT_ENFORCEMENT != 'enforce':
        logger.log_warning(line)
        return None
    logger.log_error(line.replace('would-reject', 'reject', 1))
    return HandlerResult(status=400, body={"error": error, "error_description": problems[0]},
                         media_type="application/json", headers={"Cache-Control": "no-store"})


def _redirect_uri_allowed(uri: str) -> bool:
    """IdMs.RedirectUriAllow 가 비면 전부 허용, 있으면 정확 일치(RFC 6749 §3.1.2.3)."""
    return (not IDMS_REDIRECT_URI_ALLOW) or (uri in IDMS_REDIRECT_URI_ALLOW)


def _oidc_reject(desc: str) -> HandlerResult:
    logger.log_error(f"[IdMS] Auth Req rejected: {desc}")
    return HandlerResult(status=400, body={"error": "invalid_request", "error_description": desc},
                         media_type="application/json")


def _oidc_validate(ctx: dict, need_redirect: bool) -> Optional[HandlerResult]:
    """OIDC/PKCE 요청 검증 — 실패 시 400 HandlerResult, 통과 시 None.
    redirect_uri 는 폼 경로에선 필수(302 목적지), 자체 JSON 경로에선 선택(종전 호환)."""
    if not ctx['code_challenge']:
        return _oidc_reject("code_challenge is required (PKCE mandatory)")
    if ctx['code_challenge_method'] != 'S256':
        return _oidc_reject("only S256 is supported for code_challenge_method")
    if ctx['response_type'] and ctx['response_type'] != 'code':
        return _oidc_reject("only response_type=code is supported")
    if need_redirect and not ctx['redirect_uri']:
        return _oidc_reject("redirect_uri is required")
    if ctx['redirect_uri'] and not _redirect_uri_allowed(ctx['redirect_uri']):
        return _oidc_reject("redirect_uri is not registered")
    # 필수 파라미터(TS 33.180 표 B.4.2.2-1 — response_type·client_id·scope(openid 포함)·redirect_uri·state·acr_values)와
    #   클라이언트 등록(B.3). 판정은 IdMs.ClientEnforcement — log 로 내보내 쓰이는 값을 모은 뒤 enforce 로 올린다.
    given = ctx.get('_given') or set()
    problems = [f"{k} is required" for k in ('response_type', 'state', 'scope', 'redirect_uri', 'acr_values') if k not in given]
    if 'scope' in given and SCOPE_OPENID not in ctx['scope'].split():
        problems.append("scope must include openid")
    if 'acr_values' in given and ACR_PASSWORD not in ctx['acr_values'].split():
        problems.append(f"acr_values must include {ACR_PASSWORD}")
    problems += _client_problems(ctx['client_id'], 'client_id' in given, ctx['redirect_uri'])
    return _client_gate('authreq', ctx['client_id'], problems)


def _authenticate(login_id: str, password: str):
    """사용자 인증 — (True, mcptt_id, None) 또는 (False, None, 실패 사유).
    CIMS 로그인 ID(login_id) 우선: 토큰 sub=login_id, mcptt_id=규격 MCPTT ID(분리) — PTT 가입이 없는 계정은 mcptt_id 가 None 이다
    (MC 서비스 신원 없음, _load_login_accounts). DB 미연결 등으로 LOGIN_ACCOUNTS 가 비면 legacy: USERS(tel:+msisdn) 직접 로그인 —
    그 로그인 ID 가 곧 MC 신원이다."""
    acct = LOGIN_ACCOUNTS.get(login_id) if login_id else None
    expected_pw = None
    mcptt_id = login_id
    if acct is not None:
        expected_pw = acct.get("password")
        mcptt_id = acct.get("mcptt_id")
    elif login_id in USERS:
        expected_pw = USERS[login_id].get("password")
        mcptt_id = login_id
    if not expected_pw:
        logger.log_error(f"[IdMS] Auth Req Failed: login_id {login_id} not found (or no login credential)")
        return False, None, "사용자를 찾을 수 없습니다"
    if expected_pw != password:
        logger.log_error(f"[IdMS] Auth Req Failed: Password mismatch for {login_id}")
        return False, None, "비밀번호가 올바르지 않습니다"
    return True, mcptt_id, None


def _issue_auth_code(login_id: str, mcptt_id: str, ctx: dict) -> str:
    """auth-code 발급·영속 저장 — login_id(sub) 와 mcptt_id(서비스 신원) 분리 보관, PKCE 결박."""
    code = str(uuid.uuid4())
    now = int(time.time())
    storage.save_auth_code(code, {
        "login_id": login_id,
        "mcptt_id": mcptt_id,                   # None = MC 서비스 신원 없는 계정(전화 전용)
        "mc_user": bool(mcptt_id),
        "client_id": ctx['client_id'],
        "redirect_uri": ctx['redirect_uri'] or None,
        "scope": ctx['scope'],
        "state": ctx['state'],
        "issued_at": now,
        "expires_at": now + AUTH_CODE_TTL,
        "used": False,
        "nonce": ctx['nonce'],                  # S2b: token 발급 시 id_token 에 반영
        "code_challenge": ctx['code_challenge'],
        "code_challenge_method": ctx['code_challenge_method'],
    })
    return code


def _login_form_html(action_url: str, ctx: dict, error: str = '') -> str:
    """규격 로그인 폼(TS 24.482 §6.3.1 "form data to prompt … username and password").
    hidden 값·오류문은 전부 html.escape — 무상태(서버 세션 없음)."""
    import html as _html
    esc = lambda s: _html.escape(str(s or ''), quote=True)
    hidden = ''.join(
        f'    <input type="hidden" name="{esc(k)}" value="{esc(ctx[k])}">\n'
        for k in _OIDC_CTX_KEYS if ctx.get(k))
    err = f'  <p class="error" role="alert">{esc(error)}</p>\n' if error else ''
    return f"""<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CIMS MCX 로그인</title>
<style>
body{{font-family:sans-serif;margin:2em auto;max-width:22em;padding:0 1em}}
label{{display:block;margin:.8em 0 .2em}} input[type=text],input[type=password]{{width:100%;padding:.5em;box-sizing:border-box}}
button{{margin-top:1.2em;padding:.6em 1.4em}} .error{{color:#b00020}}
</style>
</head>
<body>
  <h1>MCX 로그인</h1>
{err}  <form method="post" action="{esc(action_url)}" accept-charset="utf-8" autocomplete="off">
{hidden}    <label for="cims-login">아이디 (MC ID)</label>
    <input id="cims-login" type="text" name="{esc(IDMS_FORM_LOGIN_FIELD)}" autofocus required>
    <label for="cims-password">비밀번호</label>
    <input id="cims-password" type="password" name="{esc(IDMS_FORM_PASSWORD_FIELD)}" required>
    <button type="submit">로그인</button>
  </form>
</body>
</html>
"""


def _authreq_action_url(args: HandlerArgs) -> str:
    """폼 action = 이 authreq 의 절대 URL (공개 base URL 정본 — openid-configuration·ue-init-config 와 같은 규칙).
    헤드리스 SDK 가 상대 경로를 못 풀 수 있어 절대 URL 로 준다."""
    return f"{public_base_url(args)}/idms/authreq"


async def handle_auth_req(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # (로깅은 pi_http post_hook 에서 자동 처리)
    if args.method == 'POST':
        # ② 규격 폼 제출 — form-urlencoded (controller 가 dict 로 파싱). 입력칸 이름은 설정값,
        #    user_name/user_password 도 예비로 받는다(자체 도구가 POST 로 올 때).
        form = args.body if isinstance(args.body, dict) else {}
        ctx = _oidc_ctx(form)
        bad = _oidc_validate(ctx, need_redirect=True)
        if bad:
            return bad
        login_id = str(form.get(IDMS_FORM_LOGIN_FIELD) or form.get('user_name') or '').strip()
        password = str(form.get(IDMS_FORM_PASSWORD_FIELD) or form.get('user_password') or '')
        logger.log_info(f"[IdMS] Auth Req(form): user={login_id}, client={ctx['client_id']}, pkce=S256")
        ok, mcptt_id, why = _authenticate(login_id, password)
        if not ok:
            # 인증 실패 = 폼 재표시 + 오류(200) — 단말이 다시 채워 제출할 수 있게 문맥을 그대로 이월.
            return HandlerResult(status=200, body=_login_form_html(_authreq_action_url(args), ctx, error=why),
                                 media_type="text/html; charset=utf-8",
                                 headers={"Cache-Control": "no-store"})
        code = _issue_auth_code(login_id, mcptt_id, ctx)
        q = {"code": code}
        if ctx['state']:
            q["state"] = ctx['state']
        from urllib.parse import urlencode as _urlencode
        sep = '&' if '?' in ctx['redirect_uri'] else '?'
        location = f"{ctx['redirect_uri']}{sep}{_urlencode(q)}"
        return HandlerResult(status=302, headers={"Location": location, "Cache-Control": "no-store"})

    if args.method != 'GET':
        return HandlerResult(status=405)

    params = args.query_params or {}
    ctx = _oidc_ctx(params)
    if 'user_name' in params or 'user_password' in params:
        # ① 자체 단말 간이형 — GET 에 자격이 실려 오면 폼 없이 바로 판정, 결과는 JSON.
        bad = _oidc_validate(ctx, need_redirect=False)
        if bad:
            return bad
        login_id = str(params.get('user_name') or '')
        logger.log_info(f"[IdMS] Auth Req: user={login_id}, client={ctx['client_id']}, pkce=S256")
        ok, mcptt_id, why = _authenticate(login_id, str(params.get('user_password') or ''))
        if not ok:
            return HandlerResult(status=401, body={"error": "access_denied", "error_description": why},
                                 media_type="application/json")
        code = _issue_auth_code(login_id, mcptt_id, ctx)
        return HandlerResult(status=200,
                             body={"Location": ctx['redirect_uri'] or None, "code": code, "state": ctx['state']},
                             media_type="application/json")

    # ② 규격 OIDC Authentication Request(자격 없음) — 검증 후 200 + 로그인 폼.
    bad = _oidc_validate(ctx, need_redirect=True)
    if bad:
        return bad
    logger.log_info(f"[IdMS] Auth Req(form prompt): client={ctx['client_id']}, redirect_uri={ctx['redirect_uri']}")
    return HandlerResult(status=200, body=_login_form_html(_authreq_action_url(args), ctx),
                         media_type="text/html; charset=utf-8", headers={"Cache-Control": "no-store"})

# IdMS: Token Req
async def handle_token_req(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # POST /idms/tokenreq
    # (로깅은 pi_http post_hook 에서 자동 처리)
    data = args.body
    # 토큰 응답은 캐시에 남기지 않는다(TS 33.180 B.4.2.5·B.5.3 예시 · RFC 6749 §5.1 — 오류 응답 포함)
    no_store = {"Cache-Control": "no-store", "Pragma": "no-cache"}

    def _err(error: str, desc: str = '') -> HandlerResult:
        body = {"error": error}
        if desc:
            body["error_description"] = desc
        return HandlerResult(status=400, body=body, media_type="application/json", headers=dict(no_store))

    if not isinstance(data, dict):
        return _err("invalid_request")

    grant_type = data.get('grant_type')
    logger.log_info(f"[IdMS] Token Req: grant_type={grant_type}")
    
    # ==================== authorization_code ====================
    if grant_type == 'authorization_code':
        code = data.get('code')
        code_verifier = data.get('code_verifier')
        client_given = bool(data.get('client_id'))
        client_id = data.get('client_id') or 'MCPTT_UE'
        redirect_uri = data.get('redirect_uri')

        if not code:
            return _err("invalid_request")

        # 1. auth-code 조회
        auth_data = storage.get_auth_code(code)
        if not auth_data:
            logger.log_error(f"Auth code not found: {code}")
            return _err("invalid_grant")

        # 2. 만료 체크
        now = int(time.time())
        if now > auth_data.get("expires_at", 0):
            logger.log_error(f"Auth code expired: {code}")
            storage.delete_auth_code(code)
            return _err("invalid_grant")

        # 3. 1회성 체크
        if auth_data.get("used", False):
            logger.log_error(f"Auth code already used: {code}")
            storage.delete_auth_code(code)
            return _err("invalid_grant")

        # 4. client_id 일치 확인
        if auth_data.get("client_id") != client_id:
            logger.log_error(f"Client ID mismatch: {client_id} != {auth_data.get('client_id')}")
            return _err("invalid_grant")

        # 5. redirect_uri — 실려 왔으면 인증 요청의 값과 같아야 한다(TS 33.180 표 B.4.2.4-1 «identical», RFC 6749 §4.1.3)
        if redirect_uri and auth_data.get("redirect_uri") != redirect_uri:
            logger.log_error(f"Redirect URI mismatch")
            return _err("invalid_grant")
        # 5a. 필수 파라미터 client_id·redirect_uri(표 B.4.2.4-1)와 클라이언트 등록(B.3) — 판정은 IdMs.ClientEnforcement.
        #   redirect_uri 가 빠지면 코드를 가로챈 쪽이 인증 요청의 redirect 를 몰라도 토큰을 받는다(코드 가로채기 방어 한 겹).
        problems = _client_problems(client_id, client_given, redirect_uri or '')
        if not redirect_uri:
            problems.append("redirect_uri is required")
        deny = _client_gate('tokenreq', client_id, problems, error="invalid_grant")
        if deny:
            return deny

        # 6. PKCE 검증 (필수)
        if "code_challenge" not in auth_data:
            logger.log_error("PKCE: code_challenge not found in auth_data")
            return _err("invalid_grant", "PKCE required")

        if not code_verifier:
            logger.log_error("PKCE: code_verifier missing")
            return _err("invalid_grant", "code_verifier required")

        if not verify_pkce(code_verifier, auth_data["code_challenge"], auth_data.get("code_challenge_method", "S256")):
            logger.log_error("PKCE: verification failed")
            return _err("invalid_grant", "PKCE verification failed")

        # 7. 성공 - 토큰 발급 (sub=login_id, mcptt_id=서비스 신원 분리. nonce 반영)
        login_id = auth_data.get("login_id") or auth_data.get("user_id")
        mcptt_id = auth_data.get("mcptt_id")
        # MC 서비스 신원이 있는 계정인가 — 옛 코드(키 없음)는 mcptt_id 유무로. 없으면 MC scope·claim 을 주지 않는다(IDM-1).
        mc_user = bool(auth_data.get("mc_user", bool(mcptt_id)))
        nonce = auth_data.get("nonce", "")
        # 허가 scope = 요청 ∩ 카탈로그(별칭 확장) ∩ 사용자 인가. 제외분은 로그 + 응답 `scope` 로 실제 허가분을 알린다(RFC 6749 §5.1).
        scope, dropped = grant_scope(auth_data.get("scope", ""), mcptt_id=mcptt_id if mc_user else None, mc_user=mc_user)
        if dropped:
            logger.log_info(f"[IdMS] scope not granted (unknown or not authorised): {' '.join(dropped)} "
                            f"login_id={login_id}")

        id_token, access_token, refresh_token = create_tokens(
            login_id, scope, client_id, nonce=nonce, mcptt_id=mcptt_id, mc_user=mc_user)

        # auth-code 삭제 (1회성)
        storage.delete_auth_code(code)

        logger.log_info(f"Token issued for login_id={login_id} mcptt_id={mcptt_id} scope={scope}")
        return HandlerResult(status=200, body={
            "access_token": access_token,
            "refresh_token": refresh_token,
            "id_token": id_token,
            "token_type": "Bearer",
            "expires_in": ACCESS_TOKEN_TTL,
            "scope": scope,
        }, media_type="application/json", headers=dict(no_store))

    # ==================== refresh_token ====================
    elif grant_type == 'refresh_token':
        refresh_token = data.get('refresh_token')
        client_id = data.get('client_id')

        if not refresh_token:
            return _err("invalid_request")

        # 1. refresh_token 조회
        token_data = storage.get_refresh_token(refresh_token)
        if not token_data:
            logger.log_error(f"Refresh token not found")
            return _err("invalid_grant")

        # 2. revoked/만료 확인
        now = int(time.time())
        if token_data.get("revoked", False):
            logger.log_error(f"Refresh token revoked")
            return _err("invalid_grant")

        if now > token_data.get("expires_at", 0):
            logger.log_error(f"Refresh token expired")
            storage.revoke_refresh_token(refresh_token)
            return _err("invalid_grant")

        # 3. client_id — 실려 왔으면 발급 때의 클라이언트와 같아야 한다(refresh 요청의 필수 파라미터는 grant_type 뿐 — 표 B.5.2-1)
        if client_id and token_data.get("client_id") != client_id:
            logger.log_error(f"Client ID mismatch for refresh token")
            return _err("invalid_grant")
        client_id = token_data.get("client_id") or client_id or 'MCPTT_UE'

        # 3a. 계정 재확인(TS 33.180 B.5.3 RECOMMENDED) — 계정이 지워졌거나 비밀번호가 바뀌었으면 refresh token 을 회수한다.
        #   LOGIN_ACCOUNTS 가 비면(DB 없는 legacy 로그인) 확인할 원천이 없어 건너뛴다. `cred` 가 없는 옛 refresh token 은 계정
        #   유무만 본다.
        login_id = token_data["user_id"]                 # = subject(login_id)
        acct = LOGIN_ACCOUNTS.get(login_id) if LOGIN_ACCOUNTS else None
        if LOGIN_ACCOUNTS:
            cred = token_data.get("cred")
            if acct is None or (cred and cred != account_cred(login_id)):
                logger.log_error(f"[IdMS] refresh token revoked — account {'removed' if acct is None else 'credential changed'}: "
                                 f"login_id={login_id}")
                storage.revoke_refresh_token(refresh_token)
                return _err("invalid_grant")

        # 4. refresh token rotation — MC 서비스 신원은 지금 계정에서 다시 읽는다(PTT 가입이 생기거나 없어졌을 수 있다)
        mcptt_id = acct.get("mcptt_id") if acct is not None else token_data.get("mcptt_id", login_id)
        mc_user = bool(mcptt_id)
        granted_scope = token_data.get("scope", "") or ""
        if not mc_user:
            granted_scope = grant_scope(granted_scope, mc_user=False)[0]

        # scope 분리: refresh 요청이 scope 를 명시하면 원 grant 의 subset 으로 좁혀 발급한다.
        #   (AccountManager 가 authTokenType 별로 provisioning / MC 서비스 토큰을 따로 받기 위함.)
        #   양쪽을 별칭 확장한 뒤 교집합 — 이행 전 발급된 refresh(구 scope 문자열 저장)도 재로그인 없이 이어진다.
        granted_full = expand_scopes(granted_scope)
        requested_scope = (data.get('scope') or "").strip()
        if requested_scope:
            _gs = set(granted_full)
            req = [s for s in expand_scopes(requested_scope) if s in _gs]
            scope = " ".join(req) if req else " ".join(granted_full)   # 교집합 없으면 원 scope 유지
        else:
            scope = " ".join(granted_full)

        # 새 토큰 발급 — access 는 좁힌 scope, refresh 는 원 grant(broad) 보존(다음 다른-용도 refresh 가능).
        id_token, access_token, new_refresh_token = create_tokens(
            login_id, scope, client_id, refresh_scope=granted_scope, mcptt_id=mcptt_id, mc_user=mc_user)

        # 기존 토큰 회수
        storage.revoke_refresh_token(refresh_token, rotated_to=new_refresh_token)

        logger.log_info(f"Refresh token rotated for user: {login_id} scope={scope}")
        return HandlerResult(status=200, body={
            "access_token": access_token,
            "refresh_token": new_refresh_token,
            "id_token": id_token,
            "token_type": "Bearer",
            "expires_in": ACCESS_TOKEN_TTL,
            "scope": scope,
        }, media_type="application/json", headers=dict(no_store))

    return _err("unsupported_grant_type")


# ── GMS XCAP 그룹 CRUD — 가입자(관제사) 주체 (mcptt_authorization.md §3, TS 24.481 Ut PUT/DELETE) ──
#   인가 축 둘: 생성 = 프로파일 allow_create_group(OAM 부여), 수정·삭제 = 소유(ptt_groups.authorized_user_id
#   == 토큰 가입자 users.id). 관리 API(4421, 콘솔 토큰)와 정본(DB)·캐시 동기화(sync_group_from_db)를 공유하고
#   토큰 realm 은 섞지 않는다(PKCE 토큰은 여기 GMS 에서만).
import re as _re
import xml.etree.ElementTree as _ET

GMS_GROUP_ID_RE = _re.compile(r'^g-[0-9a-f]{8}$')   # 단말이 새 그룹에 붙이는 식별자 (XCAP 클라이언트 명명)
_GMS_MAX_BODY = 256 * 1024
_NS = {
    'poc': 'urn:oma:xml:poc:list-service',
    'rl': 'urn:ietf:params:xml:ns:resource-lists',
    'cp': 'urn:ietf:params:xml:ns:common-policy',
    'gi': 'urn:3gpp:ns:mcpttGroupInfo:1.0',
    'cims': 'urn:cims:groupinfo:1.0',
}


def _admin_manages_group(payload: dict, group: Optional[dict]) -> bool:
    """역할의 PTT 그룹 관리 능력(`can(user, ptt_group.manage, group)` — mcptt_authorization.md §4.1)으로 그룹을 관리할 수
    있는가 — 소유자가 아니어도 `scope`(그룹 org_code 가 directory_write 범위 안)·`all` 이면 GET/PUT/DELETE 허용.
    group=None(신규 생성)은 scope|all 만(own 은 소유 판정이라 생성 인가가 아니다). 관제 앱 관리 API 와 같은 판정."""
    conn = _db_connect()
    if conn is None:
        return False
    try:
        from handlers import dispatch_directory as _dd
        from services import authz as _az
        with conn:
            with conn.cursor() as cur:
                _msisdn, uid = _dd.caller_identity(cur, payload)
                if uid is None:
                    return False
                principal = _az.user_principal(uid)
                if group is None:
                    role = _az.role_of(cur, principal)
                    return bool(role) and _az.effective_ptt_group_manage(role) in ('scope', 'all')
                target = {'kind': 'ptt_group', 'id': group.get('id'), 'org_code': group.get('org_code') or '',
                          'authorized_user_id': group.get('authorized_user_id')}
                ok, _reason = _az.can(principal, 'ptt_group.manage', target, cur=cur)
                return ok
    except Exception as e:
        logger.log_warning(f"[GMS] admin scope check failed: {e}")
        return False


def _token_user_id(payload: dict) -> Optional[int]:
    """토큰 sub(=login_id) → users.id. LOGIN_ACCOUNTS 는 admin API 변경 후 refresh_login_accounts 로 최신."""
    acct = LOGIN_ACCOUNTS.get((payload or {}).get('sub') or '')
    try:
        return int(acct['user_id']) if acct and acct.get('user_id') is not None else None
    except (TypeError, ValueError):
        return None


def _requester_ptt_id(payload: dict) -> str:
    """토큰 mcptt_id(tel:+E.164) → ptt_subscriptions.id / PTT_PROFILES 키(+E.164)."""
    u = _norm_mcptt_uri((payload or {}).get('mcptt_id') or '')
    return u if (not u or u.startswith('+')) else f"+{u}" if u.isdigit() else u


def _gms_gid_from_uri(group_uri: str) -> Tuple[str, str]:
    """XCAP 경로의 그룹 URI → (mcptt_group_id, 도메인). tel:g-… / sip:g-…@dom 모두 수용."""
    s = (group_uri or '').strip()
    dom = ''
    if '@' in s:
        dom = s.split('@', 1)[1].lower()
    return _norm_mcptt_uri(s), dom


def _ptt_domains() -> set:
    ds = {IDMS_DOMAIN.lower()}
    try:
        from services import access_services as _access_services
        d = _access_services.ptt_domain(PROVISIONING)
        if d:
            ds.add(str(d).lower())
    except Exception:
        pass
    return ds


def validate_new_gms_group_id(gid: str, dom: str = '') -> Optional[str]:
    """새 그룹 식별자 검증 — 오류 문자열 또는 None. 형식 g-<8hex>, 예약 접두사 거부, sip: 형이면 PTT 도메인."""
    if not gid:
        return 'group id required'
    if gid.startswith(('adhoc-', 'priv-')):
        return "group id prefix 'adhoc-'/'priv-' is reserved for on-the-fly sessions"
    if not GMS_GROUP_ID_RE.match(gid):
        return "group id must be 'g-' + 8 lowercase hex (client-named, XCAP)"
    if dom and dom not in _ptt_domains():
        return f"group uri domain '{dom}' is not the PTT domain"
    return None


def _xtext(el, path: str) -> Optional[str]:
    n = el.find(path, _NS)
    return n.text.strip() if (n is not None and n.text is not None) else None


def _xbool(el, path: str) -> Optional[bool]:
    t = _xtext(el, path)
    return None if t is None else t.lower() == 'true'


def _xint(el, path: str) -> Optional[int]:
    t = _xtext(el, path)
    try:
        return None if t is None else int(t)
    except ValueError:
        return None


def parse_group_document_xml(xml_text: str) -> dict:
    """application/vnd.oma.poc.groups+xml (get_group_xml 이 내는 문서와 같은 포맷) → 속성 dict.
    없는 요소는 None(갱신 시 기존값 유지, 생성 시 기본값). members 는 요소가 있을 때만 리스트.
    ValueError = 스키마 위반(400)."""
    if not xml_text or len(xml_text) > _GMS_MAX_BODY:
        raise ValueError('empty or oversized body')
    if '<!DOCTYPE' in xml_text or '<!ENTITY' in xml_text:
        raise ValueError('DTD not allowed')
    try:
        root = _ET.fromstring(xml_text)
    except _ET.ParseError as e:
        raise ValueError(f'malformed XML: {e}')
    ls = root if root.tag == f"{{{_NS['poc']}}}list-service" else root.find('poc:list-service', _NS)
    if ls is None:
        raise ValueError('list-service element missing')
    out = {
        'display_name': _xtext(ls, 'poc:display-name'),
        'group_type': None,
        'hang_timer_sec': parse_xs_duration(_xtext(ls, 'gi:on-network-hang-timer')),
        'max_duration_sec': parse_xs_duration(_xtext(ls, 'gi:on-network-maximum-duration')),
        'min_number_to_start': _xint(ls, 'gi:on-network-minimum-number-to-start'),
        'ack_timeout_sec': parse_xs_duration(
            _xtext(ls, 'gi:on-network-timeout-for-acknowledgement-of-required-members')),
        'ack_action': None,
        'allow_sds': _xbool(ls, 'gi:mcdata-allow-short-data-service'),
        'allow_fd': _xbool(ls, 'gi:mcdata-allow-file-distribution'),
        'max_sds_size': _xint(ls, 'gi:mcdata-on-network-max-data-size-for-SDS'),
        'max_auto_recv': _xint(ls, 'gi:mcdata-on-network-max-data-size-auto-recv'),
        'max_members': _xint(ls, 'gi:on-network-max-participant-count'),
        'require_affiliation': _xbool(ls, 'gi:on-network-require-affiliation'),
        'priority': _xint(ls, 'gi:on-network-group-priority'),
        'encryption': _xbool(ls, 'gi:on-network-encryption'),
        'emergency_call': _xbool(ls, './/cp:actions/gi:allow-MCPTT-emergency-call'),
        'emergency_alert': _xbool(ls, './/cp:actions/gi:allow-MCPTT-emergency-alert'),
        'allow_conference_state': _xbool(ls, './/cp:actions/gi:on-network-allow-conference-state'),
        'org_code': _xtext(ls, 'gi:org-code'),
        'on_network': None,
        'members': None,
        # MCVideo 서비스 — None = 문서가 MCVideo 를 말하지 않음(기존 상태 유지), dict = MCVideo <service> 가 있어 켜고 속성 반영
        #   (services.mcvideo.parse_group_attrs — 전환기 규칙은 그 함수 설명).
        'mcvideo': None,
    }
    _mv_on, _mv_attrs = _mcvideo.parse_group_attrs(ls, _NS)
    if _mv_on:
        out['mcvideo'] = _mv_attrs
    # 그룹 종류 = <on-network-invite-members> (TS 24.481 §7.2.2 a). 없으면 그대로 둔다.
    #   broadcast 는 그룹 종류가 아니다(일제 통화 = 호 속성, TS 24.379 §4.12).
    inv = _xbool(ls, 'gi:on-network-invite-members')
    if inv is not None:
        out['group_type'] = 'prearranged' if inv else 'chat'
    # TNG3 «무제한» 표기(GET 이 0 대신 싣는 값) 이상은 0 으로 되읽는다 — 받은 문서를 그대로 PUT 해도 DB 값이 바뀌지 않는다.
    #   요소가 없으면 None(갱신 = 기존값 유지) — T4 0 을 생략한 문서의 왕복도 0 을 지킨다.
    if out['max_duration_sec'] is not None and out['max_duration_sec'] >= GROUP_MAX_DURATION_UNLIMITED:
        out['max_duration_sec'] = 0
    for k, tag, lo, hi in (('hang_timer_sec', 'on-network-hang-timer', 0, GROUP_HANG_TIMER_MAX),
                           ('max_duration_sec', 'on-network-maximum-duration', 0, GROUP_MAX_DURATION_MAX),
                           ('ack_timeout_sec', 'on-network-timeout-for-acknowledgement-of-required-members',
                            1, GROUP_ACK_TIMEOUT_MAX)):
        if _xtext(ls, f'gi:{tag}') is not None and out[k] is None:
            raise ValueError(f'{tag} is not an xs:duration')
        if out[k] is not None and not (lo <= out[k] <= hi):
            raise ValueError(f'{tag} out of range ({lo}..{hi} s)')
    # priorityType 0..255(TS 24.481 §7.2.4.2) — 그룹 우선순위·멤버 우선순위 둘 다. 숫자가 아니거나 범위 밖이면 스키마 위반.
    if _xtext(ls, 'gi:on-network-group-priority') is not None and not (
            out['priority'] is not None and 0 <= out['priority'] <= PRIORITY_TYPE_MAX):
        raise ValueError(f'on-network-group-priority is not a priorityType (0..{PRIORITY_TYPE_MAX})')
    # <on-network-disabled/> 가 있으면 on-network 를 끈 그룹(§7.2.8). 없으면 그대로 둔다(갱신 = 기존값 유지).
    if ls.find('gi:on-network-disabled', _NS) is not None:
        out['on_network'] = False
    if _xtext(ls, 'gi:on-network-minimum-number-to-start') is not None and not (
            out['min_number_to_start'] is not None and 0 <= out['min_number_to_start'] <= GROUP_MIN_TO_START_MAX):
        raise ValueError('on-network-minimum-number-to-start is not an xs:unsignedShort')
    act = _xtext(ls, 'gi:on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members')
    if act is not None:
        out['ack_action'] = norm_ack_action(act)   # 정의 밖 값 = abandon (§7.2.2 u))
    lst = ls.find('poc:list', _NS)
    if lst is not None:
        members = []
        for e in lst.findall('poc:entry', _NS):
            uri = (e.get('uri') or '').strip()
            uid = _norm_mcptt_uri(uri)
            if uid and uid[0] != '+' and uid.isdigit():
                uid = '+' + uid
            if not uid:
                raise ValueError('entry without uri')
            role = (_xtext(e, 'gi:participant-type') or 'participant').lower()
            if role not in ('chair', 'participant'):
                raise ValueError(f"participant-type '{role}' not one of chair/participant")
            prio = _xint(e, 'gi:user-priority')
            if _xtext(e, 'gi:user-priority') is not None and not (prio is not None and 0 <= prio <= PRIORITY_TYPE_MAX):
                raise ValueError(f'user-priority of {uri} is not a priorityType (0..{PRIORITY_TYPE_MAX})')
            members.append({'user_id': uid, 'mcptt_id': uri if uri.lower().startswith(('tel:', 'sip:')) else None,
                            'role': role, 'priority': prio if prio is not None else 0,
                            'required': e.find('gi:on-network-required', _NS) is not None})
        out['members'] = members
    return out


_GMS_CREATE_DEFAULTS = {
    'priority': 5, 'encryption': False, 'emergency_call': False,
    'emergency_alert': True, 'allow_conference_state': True, 'allow_sds': True, 'allow_fd': False,
    'max_sds_size': 10000, 'max_auto_recv': 1048576, 'org_code': None, 'group_type': 'prearranged',
    'max_members': 0, 'require_affiliation': True, 'on_network': True,
    'hang_timer_sec': GROUP_HANG_TIMER_DEFAULT, 'max_duration_sec': GROUP_MAX_DURATION_DEFAULT,
    'min_number_to_start': 0, 'ack_timeout_sec': GROUP_ACK_TIMEOUT_DEFAULT, 'ack_action': 'abandon',
}
_GMS_BOOL_COLS = ('encryption', 'emergency_call', 'emergency_alert', 'allow_conference_state',
                  'allow_sds', 'allow_fd', 'require_affiliation', 'on_network')
_GMS_ATTR_COLS = ('priority', 'encryption', 'emergency_call', 'emergency_alert',
                  'allow_conference_state', 'allow_sds', 'allow_fd', 'max_sds_size', 'max_auto_recv', 'org_code',
                  'group_type', 'max_members', 'require_affiliation', 'hang_timer_sec', 'max_duration_sec',
                  'min_number_to_start', 'ack_timeout_sec', 'ack_action', 'on_network')


def _gms_unknown_members(cur, members: list) -> list:
    ids = [m['user_id'] for m in members]
    if not ids:
        return []
    cur.execute("SELECT id FROM ptt_subscriptions WHERE id IN (%s)" % ",".join(["%s"] * len(ids)), ids)
    known = {r['id'] for r in cur.fetchall()}
    return [i for i in ids if i not in known]


def gms_write_group(gid: str, doc: dict, owner_user_id: Optional[int], create: bool) -> Tuple[int, dict]:
    """DB 에 그룹 생성(INSERT)/갱신(UPDATE, 준 필드만) + 멤버 교체 → (status, err). 성공 시 (0, {}).
    호출자가 인가를 마친 뒤 부른다. 커밋 후 sync_group_from_db 로 캐시를 맞추는 것은 호출자 몫이 아니라 여기서 한다."""
    conn = _db_connect()
    if conn is None:
        return 503, {'error': 'db_unavailable'}
    try:
        with conn:
            with conn.cursor() as cur:
                if doc.get('members') is not None:
                    unknown = _gms_unknown_members(cur, doc['members'])
                    if unknown:
                        return 400, {'error': 'unknown_member', 'detail': unknown}
                # 정원은 필수 멤버 수보다 작을 수 없다(TS 24.379 §6.3.5.5 NOTE 4 — GMS 검증 몫, 관리 API 와 같은 규칙). 준 값이 없으면
                #   기존 값(갱신)·기본 0(생성)과 기존 멤버로 본다.
                if doc.get('members') is not None or doc.get('max_members') is not None:
                    mm, req = doc.get('max_members'), None
                    if not create and (mm is None or doc.get('members') is None):
                        cur.execute("SELECT g.max_members, (SELECT COUNT(*) FROM ptt_group_members m WHERE m.group_id=g.id "
                                    "AND m.on_network_required=1) AS n_req FROM ptt_groups g WHERE g.mcptt_group_id=%s", (gid,))
                        row = cur.fetchone() or {}
                        mm = int(row.get('max_members') or 0) if mm is None else mm
                        req = int(row.get('n_req') or 0)
                    if doc.get('members') is not None:
                        req = sum(1 for m in doc['members'] if m.get('required'))
                    if mm and req and req > int(mm):
                        return 400, {'error': 'required_exceeds_max_members',
                                     'detail': f'required members ({req}) exceed on-network-max-participant-count ({mm})'}
                if create:
                    vals = {k: (doc[k] if doc.get(k) is not None else _GMS_CREATE_DEFAULTS[k])
                            for k in _GMS_ATTR_COLS}
                    for k in _GMS_BOOL_COLS:
                        vals[k] = 1 if vals[k] else 0
                    cur.execute(
                        "INSERT INTO ptt_groups (mcptt_group_id, name, " + ", ".join(_GMS_ATTR_COLS) +
                        ", authorized_user_id) VALUES (" + ", ".join(["%s"] * (len(_GMS_ATTR_COLS) + 3)) + ")",
                        [gid, doc.get('display_name') or gid] + [vals[k] for k in _GMS_ATTR_COLS] + [owner_user_id])
                    gpk = cur.lastrowid
                else:
                    cur.execute("SELECT id FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))
                    row = cur.fetchone()
                    if not row:
                        return 404, {'error': 'not_found'}
                    gpk = row['id']
                    sets, args = [], []
                    if doc.get('display_name') is not None:
                        sets.append("name=%s"); args.append(doc['display_name'])
                    for k in _GMS_ATTR_COLS:
                        if doc.get(k) is not None:
                            sets.append(f"{k}=%s"); args.append((1 if doc[k] else 0) if k in _GMS_BOOL_COLS else doc[k])
                    if sets:
                        cur.execute("UPDATE ptt_groups SET " + ", ".join(sets) + " WHERE id=%s", args + [gpk])
                if doc.get('mcvideo') is not None:
                    _mcvideo.write_group_attrs(cur, gpk, doc['mcvideo'])
                if doc.get('members') is not None:
                    # 암시적 제휴(user profile 설정 — TS 24.484)는 그룹 문서(TS 24.481)에 없는 요소라 문서 교체가
                    #   지우지 않게 교체 전 값을 잇는다.
                    cur.execute("SELECT user_id FROM ptt_group_members WHERE group_id=%s AND implicit_affiliation=1",
                                (gpk,))
                    keep = {r['user_id'] for r in cur.fetchall()}
                    cur.execute("DELETE FROM ptt_group_members WHERE group_id=%s", (gpk,))
                    for m in doc['members']:
                        cur.execute("INSERT IGNORE INTO ptt_group_members "
                                    "(group_id, user_id, priority, role, mcptt_id, on_network_required, "
                                    "implicit_affiliation) VALUES (%s, %s, %s, %s, %s, %s, %s)",
                                    (gpk, m['user_id'], int(m.get('priority') or 0), m.get('role') or 'participant',
                                     m.get('mcptt_id'), 1 if m.get('required') else 0,
                                     1 if m['user_id'] in keep else 0))
            conn.commit()
    except Exception as e:
        logger.log_error(f"gms_write_group({gid}, create={create}) failed: {e}")
        return 500, {'error': 'db_error', 'detail': str(e)}
    sync_group_from_db(gid)
    return 0, {}


def gms_delete_group(gid: str) -> Tuple[int, dict]:
    conn = _db_connect()
    if conn is None:
        return 503, {'error': 'db_unavailable'}
    try:
        with conn:
            with conn.cursor() as cur:
                cur.execute("DELETE FROM ptt_groups WHERE mcptt_group_id=%s", (gid,))   # FK CASCADE 가 멤버 정리
                n = cur.rowcount
            conn.commit()
    except Exception as e:
        logger.log_error(f"gms_delete_group({gid}) failed: {e}")
        return 500, {'error': 'db_error', 'detail': str(e)}
    sync_group_from_db(gid)
    return (0, {}) if n else (404, {'error': 'not_found'})


def _json_result(status: int, body: dict, headers=None) -> HandlerResult:
    return HandlerResult(status=status, body=json.dumps(body), media_type='application/json', headers=headers or {})

# GMS: List groups for a user
# GET /org.openmobilealliance.groups/users/{user_uri}
async def handle_user_groups(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # scope 검사는 진입점 handle_group_management 가 수행(여기는 그 위임 대상 — 중복 로그 방지).
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)

    from urllib.parse import unquote
    path = args.full_path
    parts = [p for p in path.split('/') if p]
    # parts: ['org.openmobilealliance.groups', 'users', 'user_uri']
    user_uri = unquote(parts[-1]) if len(parts) >= 3 else token_payload.get('mcptt_id', '')

    logger.log_info(f"[GMS] List groups for user: {user_uri}")

    # 소유자(authorized_user_id == 토큰 가입자 users.id)면 멤버가 아니어도 목록에 넣는다 — 편집·삭제 대상 열거용.
    #   is_owner 는 앱이 [편집]/[삭제] 노출 여부를 정할 근거(GMS PUT/DELETE 인가와 같은 판정).
    my_uid = _token_user_id(token_payload)
    result = []
    for group_uri, group in GROUPS.items():
        is_owner = my_uid is not None and group.get('authorized_user_id') == my_uid
        is_member = any(_uri_eq(m.get('uri'), user_uri) for m in group.get('members', []))
        if is_member or is_owner:
            result.append({
                "uri": group_uri,
                "display_name": group['display_name'],
                "etag": group['etag'],
                "member_count": len(group['members']),
                "is_owner": is_owner,
            })

    return HandlerResult(status=200, body=json.dumps(result), media_type='application/json')


# GMS: unified handler — dispatches on path depth
# GET  /org.openmobilealliance.groups/users/{user_uri}              → list user's groups (JSON)
# GET/PUT/DELETE /org.openmobilealliance.groups/users/{user_uri}/{group_uri} → specific group
async def handle_group_management(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    # GMS scope — 그룹 문서가 MCPTT/MCVideo/MCData 공용(한 그룹 = 서비스 집합)이라 셋 중 하나면 통과 (TS 33.180 B.4.2.2).
    deny = require_scope(args, token_payload, 'GMS', SCOPE_PTT_GMS, SCOPE_VIDEO_GMS, SCOPE_DATA_GMS)
    if deny:
        return deny

    path = args.full_path
    parts = [p for p in path.split('/') if p]

    # 인가 (item 2): XCAP 사용자 트리 소유 검사 — /users/{tree_owner}/ 는 토큰 본인 트리만 접근.
    #   타 사용자 트리(그룹목록 enumerate 포함) 접근 차단 (수평 권한 상승 방지).
    from urllib.parse import unquote as _unq
    requester = (token_payload or {}).get('mcptt_id')
    tree_owner = _unq(parts[2]) if len(parts) >= 3 else ""
    if tree_owner and not _uri_eq(requester, tree_owner):
        logger.log_error(f"[GMS] Forbidden: token '{requester}' != XCAP tree owner '{tree_owner}'")
        return HandlerResult(status=403, body="Forbidden: cannot access another user's XCAP tree")

    # 서비스 로그: GMS 요청 기록 (그룹 ID가 있으면 해당 그룹 디렉터리에)
    group_uri = parts[3] if len(parts) >= 4 else ""
    user_uri = parts[2] if len(parts) >= 3 else ""
    gid = group_uri.replace("tel:", "").replace("sip:", "").split("@")[0] if group_uri else ""
    # GMS base log 는 post_hook 에서 자동, 그룹별 participants.jsonl 만 별도 기록
    if gid:
        _logger.log_ptt_service(gid, "in", "HTTPS", f"GMS {args.method} {user_uri}", "")
    # ['org.openmobilealliance.groups', 'users', 'user_uri']        → list
    # ['org.openmobilealliance.groups', 'users', 'user_uri', 'group_uri'] → specific

    if len(parts) == 3 and args.method == 'GET':
        # Delegate to list handler
        return await handle_user_groups(args, kwargs)

    from urllib.parse import unquote
    group_uri = unquote(parts[-1])
    user_uri = unquote(parts[-2])

    logger.log_info(f"[GMS] Group Management: {args.method} {group_uri}")

    try:
        if args.method == 'GET':
            # 인가 (item 2): 멤버(또는 authorized_user)만 그룹 문서 열람 (TS 24.481).
            grp = GROUPS.get(group_uri)
            if grp and not _is_group_member(grp, requester) and not _admin_manages_group(token_payload, grp):
                logger.log_error(f"[GMS] Forbidden: '{requester}' not a member of group '{group_uri}'")
                return HandlerResult(status=403, body="Forbidden: not a member of this group")
            xml, etag = get_group_xml(group_uri)
            if xml:
                if_none_match = args.headers.get('if-none-match', '')
                if if_none_match and if_none_match == etag:
                    return HandlerResult(status=304)
                return HandlerResult(status=200, body=xml, media_type='application/vnd.oma.poc.groups+xml', headers={'Etag': etag})
            else:
                return HandlerResult(status=404)

        elif args.method == 'PUT':
            # GMS XCAP PUT (TS 24.481 Ut) — 본문 = GET 이 내는 그룹 문서와 같은 포맷. 신규 = 생성 자격, 기존 = 소유.
            gid, dom = _gms_gid_from_uri(group_uri)
            uri_key = _group_uri(gid)
            existing = GROUPS.get(uri_key)
            if gid.startswith(('adhoc-', 'priv-')):
                return _json_result(400, {'error': 'reserved_prefix'})
            my_uid = _token_user_id(token_payload)
            if existing is None:
                err = validate_new_gms_group_id(gid, dom)
                if err:
                    return _json_result(400, {'error': 'invalid_group_id', 'detail': err})
                if not get_user_profile(_requester_ptt_id(token_payload)).get('allow_create_group') \
                        and not _admin_manages_group(token_payload, None):
                    logger.log_error(f"[GMS] PUT {gid} denied: '{requester}' lacks allow_create_group")
                    return _json_result(403, {'error': 'group_creation_not_allowed'})
                if my_uid is None:
                    return _json_result(403, {'error': 'group_creation_not_allowed', 'detail': 'token subject has no users.id'})
            else:
                owner = existing.get('authorized_user_id')
                # 소유자 또는 역할 관리 범위(ptt_group_manage scope|all) 안의 그룹 — 관리자는 소유권을 뺏지 않는다(authorized_user 유지).
                if (my_uid is None or owner != my_uid) and not _admin_manages_group(token_payload, existing):
                    logger.log_error(f"[GMS] PUT {gid} denied: '{requester}' is not the owner (owner={owner})")
                    # 타인 소유 = 409(클라이언트 명명 id 충돌 — 다른 id 로 다시), 소유자 없음(콘솔 생성) = 403.
                    if owner is not None and my_uid is not None:
                        return _json_result(409, {'error': 'uri_taken', 'detail': 'group is owned by another user'})
                    return _json_result(403, {'error': 'not_group_owner'})
                if_match = args.headers.get('if-match', '')
                if if_match:
                    _x, cur_etag = get_group_xml(uri_key)
                    if if_match.strip('"') != (cur_etag or '').strip('"'):
                        return _json_result(412, {'error': 'etag_mismatch', 'etag': cur_etag})
            try:
                doc = parse_group_document_xml(args.body.decode('utf-8', 'replace') if isinstance(args.body, (bytes, bytearray)) else (args.body or ''))
            except ValueError as ve:
                return _json_result(400, {'error': 'invalid_group_document', 'detail': str(ve)})
            if not _DB_CONFIG:
                # DB 없는 개발 환경 폴백 — 파일 그룹만 갱신(종전 동작). 운영은 항상 DB.
                grp = existing or {"created_by": user_uri, "created_at": datetime.datetime.now().isoformat(),
                                   "members": [], "authorized_user": requester or "",
                                   "authorized_user_id": my_uid}
                grp["display_name"] = doc.get('display_name') or grp.get('display_name') or gid
                grp["etag"] = f"etag_{int(time.time())}"
                if doc.get('members') is not None:
                    grp["members"] = [{"uri": m.get('mcptt_id') or f"tel:{m['user_id']}", "name": m['user_id'],
                                       "role": m['role'], "priority": m['priority'], "joined_at": ""} for m in doc['members']]
                if doc.get('mcvideo') is not None:
                    mv = dict(_mcvideo.GROUP_ATTR_DEFAULTS, **(grp.get('mcvideo') or {}))
                    mv.update({k: v for k, v in doc['mcvideo'].items() if v is not None})
                    grp["mcvideo"] = mv
                GROUPS[uri_key] = grp
                save_group_to_file(uri_key, grp)
            else:
                st, err = gms_write_group(gid, doc, my_uid, create=existing is None)
                if st:
                    return _json_result(st, err)
            notify_csp("GROUP_CHANGED", uri_key, "PUT", GROUPS.get(uri_key, {}).get('etag', ''))
            xml, etag = get_group_xml(uri_key)
            return HandlerResult(status=201 if existing is None else 200, body=xml,
                                 media_type='application/vnd.oma.poc.groups+xml', headers={'Etag': etag})

        elif args.method == 'DELETE':
            gid, _dom = _gms_gid_from_uri(group_uri)
            uri_key = _group_uri(gid)
            existing = GROUPS.get(uri_key)
            if existing is None:
                return _json_result(404, {'error': 'not_found'})
            my_uid = _token_user_id(token_payload)
            if (my_uid is None or existing.get('authorized_user_id') != my_uid) \
                    and not _admin_manages_group(token_payload, existing):
                logger.log_error(f"[GMS] DELETE {gid} denied: '{requester}' is not the owner")
                return _json_result(403, {'error': 'not_group_owner'})
            if not _DB_CONFIG:
                GROUPS.pop(uri_key, None)
                delete_group_file(uri_key)
            else:
                st, err = gms_delete_group(gid)
                if st:
                    return _json_result(st, err)
            notify_csp("GROUP_CHANGED", uri_key, "DELETE", "")
            return HandlerResult(status=200)

    except Exception as e:
        import traceback
        traceback.print_exc()
        logger.log_error(f"GMS Error: {e}")
        return HandlerResult(status=500, body=str(e))

# CMS: User Profile
# CMS: UE 초기 설정 (로그인 전 부트스트랩)
async def handle_ue_init_config(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # GET /org.3gpp.mcptt.ue-init-config/users/{XUI}/{docname}
    #
    # **익명 GET** — 로그인 전 문서라 토큰이 없다(규격 순서상 인증보다 앞 단계). 내용이
    #   공개 주소뿐이라 민감도도 없다. XUI 는 UE 인스턴스 ID(UUID 등) — 사용자 신원이
    #   아니므로 검증하지 않고, 어떤 XUI/문서명이 와도 같은 전역 문서를 준다.
    if args.method != 'GET':
        return HandlerResult(status=405)

    base = public_base_url(args)
    xml, etag = get_ue_init_config_xml(base)

    if_none_match = args.headers.get('if-none-match', '')
    if if_none_match and if_none_match == etag:
        return HandlerResult(status=304)
    logger.log_info(f"[CMS] UE init config served (base={base})")
    return HandlerResult(status=200, body=xml,
                         media_type='application/vnd.3gpp.mcptt-ue-init-config+xml',
                         headers={'Etag': etag})


async def handle_user_profile(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # GET /org.3gpp.mcptt.user-profile/users/{user_id}/user-profile
    # (로깅은 pi_http post_hook 에서 자동 처리)
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'CMS', SCOPE_PTT_CMS)
    if deny:
        return deny
        
    path = args.full_path
    try:
        start = path.find('/users/') + 7
        end = path.find('/user-profile', start)
        user_uri = path[start:end]
    except:
        return HandlerResult(status=400)

    # 인가 (item 2): 본인 user-profile 만 접근 (수평 권한 상승 방지).
    from urllib.parse import unquote as _unq
    if not _uri_eq(token_payload.get('mcptt_id'), _unq(user_uri)):
        logger.log_error(f"[CMS] Forbidden: token '{token_payload.get('mcptt_id')}' != user-profile '{user_uri}'")
        return HandlerResult(status=403, body="Forbidden: cannot access another user's profile")

    logger.log_info(f"[CMS] User Profile: {user_uri}")
    # 문서 생성은 **토큰의 정본 신원**으로 — 경로 XUI 는 표기 변형(sip:user@domain 완전형 등)일
    #   수 있고 USERS 키는 tel: 정본이라, 원문 조회는 본인인데도 404 가 난다(시뮬레이터 실측).
    #   본인 확인(_uri_eq)을 통과했으므로 두 표기는 동일 인물이다.
    xml, etag = get_user_profile_xml(token_payload.get('mcptt_id'), owner_uid=_token_user_id(token_payload))

    if xml:
        if_none_match = args.headers.get('if-none-match', '')
        if if_none_match and if_none_match == etag:
            return HandlerResult(status=304)
        return HandlerResult(status=200, body=xml, media_type='application/vnd.3gpp.mcptt-user-profile+xml', headers={'Etag': etag})
    else:
        return HandlerResult(status=404)

# CMS: Service Config
async def handle_service_config(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # GET /org.3gpp.mcptt.service-config/users/{user_id}/service-config
    # (로깅은 pi_http post_hook 에서 자동 처리)
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'CMS', SCOPE_PTT_CMS)
    if deny:
        return deny
        
    path = args.full_path
    try:
        start = path.find('/users/') + 7
        end = path.find('/service-config', start)
        user_uri = path[start:end]
    except:
        return HandlerResult(status=400)

    # 인가 (item 2): 본인 service-config 만 접근 (수평 권한 상승 방지).
    from urllib.parse import unquote as _unq
    if not _uri_eq(token_payload.get('mcptt_id'), _unq(user_uri)):
        logger.log_error(f"[CMS] Forbidden: token '{token_payload.get('mcptt_id')}' != service-config '{user_uri}'")
        return HandlerResult(status=403, body="Forbidden: cannot access another user's service-config")

    logger.log_info(f"[CMS] Service Config: {user_uri}")
    xml, etag = get_service_config_xml(user_uri)

    if xml:
        if_none_match = args.headers.get('if-none-match', '')
        if if_none_match and if_none_match == etag:
            return HandlerResult(status=304)
        return HandlerResult(status=200, body=xml, media_type='application/vnd.3gpp.mcptt-service-config+xml', headers={'Etag': etag})
    else:
        return HandlerResult(status=404)

# CMS: MCVideo user profile (TS 24.484 §9.3) — CMSXCAPROOT/org.3gpp.mcvideo.user-profile/users/{MCVideo ID}/{문서 이름}
#   문서 이름 = mcvideo-user-profile-<index>.xml(§9.3.1A). 1건만 두므로 이름은 가리지 않는다.
async def handle_mcvideo_user_profile(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if args.method != 'GET':
        return HandlerResult(status=405)
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'CMS', SCOPE_VIDEO_CMS)
    if deny:
        return deny
    path = args.full_path
    start = path.find('/users/')
    if start < 0:
        return HandlerResult(status=400)
    xui = path[start + 7:].split('/', 1)[0]
    from urllib.parse import unquote as _unq
    if not _uri_eq(token_payload.get('mcptt_id'), _unq(xui)):
        logger.log_error(f"[CMS] Forbidden: token '{token_payload.get('mcptt_id')}' != mcvideo user-profile '{xui}'")
        return HandlerResult(status=403, body="Forbidden: cannot access another user's profile")
    # 문서 생성은 토큰의 정본 신원으로(MCPTT user profile 과 같은 이유 — 경로 XUI 는 표기 변형일 수 있다).
    xml, etag = _mcvideo.get_user_profile_xml(token_payload.get('mcptt_id'))
    if not xml:
        return HandlerResult(status=404)     # MCVideo 이용 자격 없음(mcvideo_user_profile 행 없음)
    inm = args.headers.get('if-none-match', '')
    if inm and inm == etag:
        return HandlerResult(status=304)
    return HandlerResult(status=200, body=xml, media_type=_mcvideo.MIME_USER_PROFILE, headers={'Etag': etag})


# CMS: MCVideo service configuration (TS 24.484 §9.4) — **전역 문서**(§9.4.2.9):
#   CMSXCAPROOT/org.3gpp.mcvideo.service-config/global/mcvideo-service-config.xml. 모든 사용자 읽기 전용.
async def handle_mcvideo_service_config(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    if args.method != 'GET':
        return HandlerResult(status=405)
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'CMS', SCOPE_VIDEO_CMS)
    if deny:
        return deny
    xml, etag = _mcvideo.get_service_config_xml()
    inm = args.headers.get('if-none-match', '')
    if inm and inm == etag:
        return HandlerResult(status=304)
    return HandlerResult(status=200, body=xml, media_type=_mcvideo.MIME_SERVICE_CONFIG, headers={'Etag': etag})


# KMS: Init & KeyProv
async def handle_kms_init(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'KMS', SCOPE_PTT_KMS, SCOPE_VIDEO_KMS, SCOPE_DATA_KMS)
    if deny:
        return deny
        
    user_uri = token_payload.get('mcptt_id')
    if not user_uri:                                   # MC 서비스 신원 없는 계정(전화 전용) — 키를 내지 않는다
        return HandlerResult(status=403, body={"error": "no_mc_service_identity"}, media_type="application/json")
    logger.log_info(f"[KMS] Init: {user_uri}")
    
    xml = get_kms_init_xml(user_uri)
    return HandlerResult(status=200, body=xml, media_type='application/xml')

async def handle_kms_keyprov(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return unauthorized(args)
    deny = require_scope(args, token_payload, 'KMS', SCOPE_PTT_KMS, SCOPE_VIDEO_KMS, SCOPE_DATA_KMS)
    if deny:
        return deny
        
    user_uri = token_payload.get('mcptt_id')
    if not user_uri:
        return HandlerResult(status=403, body={"error": "no_mc_service_identity"}, media_type="application/json")
    logger.log_info(f"[KMS] Key Provision: {user_uri}")
    
    xml = get_kms_keyprov_xml(user_uri)
    return HandlerResult(status=200, body=xml, media_type='application/xml')

# IdMS: Token Introspection (RFC 7662)
async def handle_token_introspect(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # POST /idms/introspect
    if args.method != 'POST':
        return HandlerResult(status=405)

    data = args.body
    token = None
    if isinstance(data, dict):
        token = data.get('token')
    elif isinstance(data, str):
        for part in data.split('&'):
            if part.startswith('token='):
                token = part[6:]
                break

    if not token:
        return HandlerResult(status=400, body={"error": "invalid_request"}, media_type="application/json")

    payload = validate_access_token(token)
    if payload:
        # scope 는 RFC 7662 대로 공백 구분 문자열. 이행 전 발급(배열) 토큰도 같은 형으로 돌려준다.
        return HandlerResult(status=200, body={
            "active": True,
            "sub": payload.get("sub"),
            "iss": payload.get("iss"),
            "client_id": payload.get("client_id"),
            "mcptt_id": payload.get("mcptt_id"),
            "mcdata_id": payload.get("mcdata_id") or payload.get("mcptt_id"),
            "aud": payload.get("aud"),
            "exp": payload.get("exp"),
            "iat": payload.get("iat"),
            "scope": " ".join(expand_scopes(payload.get("scope") or [])),
        }, media_type="application/json")
    else:
        return HandlerResult(status=200, body={"active": False}, media_type="application/json")


# S1: OIDC Discovery — GET /.well-known/openid-configuration (TS 33.180 / OIDC Discovery 1.0)
#   단말이 엔드포인트를 하드코딩하지 않고 동적 발견. base URL 은 공개 base URL 정본(public_base_url).
async def handle_openid_config(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    base = public_base_url(args)
    doc = {
        "issuer": IDMS_ISSUER,
        "authorization_endpoint": f"{base}/idms/authreq",
        "token_endpoint": f"{base}/idms/tokenreq",
        "introspection_endpoint": f"{base}/idms/introspect",
        "jwks_uri": f"{base}/idms/jwks",
        "token_endpoint_auth_methods_supported": ["none"],
        "grant_types_supported": ["authorization_code", "refresh_token"],
        "response_types_supported": ["code"],
        "code_challenge_methods_supported": ["S256"],
        # 신 이름(TS 33.180 B.4.2.2) + 전환기 별칭(구 단말). MCVideo 미지원.
        "scopes_supported": [SCOPE_OPENID, SCOPE_PROVISIONING, *SCOPE_MC_SERVICES, *SCOPE_VIDEO_SERVICES,
                             SCOPE_LEGACY_MCPTT],
        "subject_types_supported": ["public"],
        "id_token_signing_alg_values_supported": [signing_alg()],
        "claims_supported": ["sub", "iss", "iat", "exp", "aud", "nonce", "scope", "client_id",
                             "mcptt_id", "mcvideo_id", "mcdata_id"],
    }
    return HandlerResult(status=200, body=doc, media_type="application/json")


# JWKS — GET /idms/jwks (openid-configuration `jwks_uri`, OIDC Discovery §3 · RFC 7517). 토큰 서명 공개 키 — 단말(ID token 검증
#   TS 33.180 B.11.1 · OIDC Core §3.1.3.7)과 분리 배치된 리소스 서버가 읽는다. 인증 없음(공개 키). HS256 구성이면 빈 집합.
async def handle_idms_jwks(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    return HandlerResult(status=200, body=_idms_keys.jwks(), headers={"Cache-Control": "public, max-age=3600"},
                         media_type="application/json")


# ── 자동 프로비저닝 (GET /provisioning/me, android_ue_provisioning.md §3) ──
#   Bearer access_token → mcptt_id → DB 가입자(person 기준 volte+ptt) → 서비스별 프로파일 JSON.
#   단말은 로그인 1회로 접속/계정 정보를 받아 자동 구성(수동설정 불필요).
def _msisdn_from_id(u: str) -> str:
    """mcptt_id/sub (tel:+82.../sip:+82...@dom/+82...) → 가입자 키 msisdn(+82...)."""
    s = (u or '').strip()
    low = s.lower()
    for p in ('sip:', 'tel:'):
        if low.startswith(p):
            s = s[len(p):]
            break
    return s.split('@', 1)[0].strip()

# 2자리 E.164 국가코드 집합 (ITU-T E.164 할당분) — _country_code_of 유도용.
_E164_CC2 = {
    "20", "27", "30", "31", "32", "33", "34", "36", "39", "40", "41", "43", "44", "45",
    "46", "47", "48", "49", "51", "52", "53", "54", "55", "56", "57", "58", "60", "61",
    "62", "63", "64", "65", "66", "81", "82", "84", "86", "90", "91", "92", "93", "94",
    "95", "98",
}

def _country_code_of(msisdn: str) -> str:
    """E.164 msisdn → 국가코드(digits, 예 '82'). ITU 자릿수 규칙: 1(NANP)/7=1자리,
    유효 2자리 셋, 그 외 3자리. 판정 불가 시 빈 문자열."""
    d = ''.join(ch for ch in (msisdn or '') if ch.isdigit())
    if len(d) < 4:
        return ""
    if d[0] in ('1', '7'):
        return d[0]
    return d[:2] if d[:2] in _E164_CC2 else d[:3]

_PHONE_KINDS = _subs.PHONE_KINDS      # 전화 가족(volte·voip) — 같은 CSP 전화 경로. 가입 테이블은 kind 마다 따로(services.subscriptions)


def service_entry(kind: str, service_ref: str = "") -> tuple:
    """(와이어 kind, 서비스 항목) — 가입 행의 service_ref(= CSP access_services.name)로 접속 서비스를 고른다
    (같은 kind 의 서비스가 여럿일 때 — 이름 매칭은 그 회선의 테이블 kind 안에서만, sip_service_model.md §2-9).
    정의(name·kind·domain·realm·media_srtp·sec_mechanisms…)는 CSP access_services 미러가 정본이고 csc.json
    `Provisioning.Services.<kind>` 는 단말 도달 정보(host·포트·transport…)를 보탠다 — 미러가 없으면 csc.json 만으로
    (services/access_services.entry, sip_service_model.md §2-9, android_ue_provisioning.md §4). 와이어 kind 는 고른 서비스의
    kind(volte|voip|ptt)라 단말이 회선 종류를 안다."""
    from services import access_services as _access_services
    return _access_services.entry(kind, service_ref, PROVISIONING)


def _provision_service(kind: str, sid: str, imsi: str, auth_id: str, host_ip: str,
                       sip_transport: str = "", sip_ha1: str = "", aka: dict = None,
                       service_ref: str = "") -> dict:
    kind, svc = service_entry(kind, service_ref)
    account = {
        "msisdn": sid,
        "imsi": imsi or "",
        "authId": auth_id or "",        # 빈값이면 단말이 imsi@domain 합성
        # SIP Digest 자료 (sip_access_security.md §4.7). sipHa1 = H(A1)=MD5(imsi@domain:realm:pw) —
        #   단말은 이것만으로 response 를 계산한다(pjsip PJSIP_CRED_DATA_DIGEST). CIMS 로그인(IdMS)
        #   비번과 별개. sipPassword 는 항상 null(§4.7 ⑤ — 평문 미배포, 키는 단말 호환으로 유지).
        #   sipHa1 도 없으면 단말은 SIP 계정을 구성하지 않는다(로그인 비번은 IdMS 자격 — SIP 에 쓰지 않음).
        "sipHa1": sip_ha1 or None,
        "sipPassword": None,
        # 인증 체계 (sip_access_security.md §8.2). aka 면 소프트-K 프로비저닝 — 단말이 USIM 역할이므로
        #   K/OPc 원문이 여기(토큰 인증 + TLS 채널)로만 내려간다. sipHa1 은 aka 에서 의미 없다.
        "authScheme": "aka" if aka else "digest",
    }
    if aka:
        account["sipHa1"] = None
        account["aka"] = {"k": aka.get("k", ""), "opc": aka.get("opc", ""), "amf": aka.get("amf", "8000")}
    if kind == "ptt":
        account["mcpttId"] = sid if sid.startswith(("tel:", "sip:")) else f"tel:{sid}"
    # 가용 transport 목록 — 서버는 세 transport 를 동시에 청취하며 강제하지 않는다. 단말이 이 중
    #   하나를 고르고, 고른 경로가 그 단말의 도달 경로(바인딩)가 된다(sip_tls_signaling.md §7.1).
    #   transport 마다 포트가 다르므로 목록에 포트를 함께 싣는다 — 같은 포트로 평문/TLS 를 겸하지
    #   않는다. tcp_port 미설정 = 평문 포트 공용(CSP 는 UDP/TCP 를 같은 포트로 청취).
    #   ⚠ 각 포트는 CSP local_nodes 의 bind_port 와 일치해야 한다(csc 는 CSP 를 조회하지 않는다).
    plain_port = int(svc.get('port', 5060))
    tls_port = int(svc.get('tls_port') or 0)
    transports = [
        {"transport": "UDP", "port": plain_port},
        {"transport": "TCP", "port": int(svc.get('tcp_port') or plain_port)},
    ]
    if tls_port:
        transports.append({"transport": "TLS", "port": tls_port})
    # 기본값(권장) = 가입자 override(subscriptions.sip_transport) 우선, 없으면 서비스 설정.
    #   강제가 아니라 권장값이며 단말이 목록에서 바꿀 수 있다. 목록에 없으면(예: TLS 포트 미설정)
    #   첫 항목으로 강등한다 — 도달 불가한 기본값을 내리지 않는다.
    default = (sip_transport or svc.get('transport', 'UDP') or 'UDP').upper()
    # 채널 보호 메커니즘 (RFC 3329 sec-agree, sip_access_security.md §8.3): 서비스가 제시하는 목록.
    #   ipsec-3gpp(IMS AKA+IPsec)면 보호 포트쌍(port_ps/port_pc — CSP IPSEC local_node)도 싣는다.
    #   ⚠ csc.json Provisioning.Services.<kind>.sec_mechanisms / ipsec_port_ps / ipsec_port_pc 는
    #     CSP access_services.sec_mechanisms / IPSEC local_node 와 일치해야 한다(csc 는 CSP 를 조회하지 않는다).
    security = [str(m) for m in (svc.get('sec_mechanisms') or ['tls'])]
    ipsec = None
    if 'ipsec-3gpp' in security and int(svc.get('ipsec_port_ps') or 0):
        ipsec = {"port_ps": int(svc.get('ipsec_port_ps')), "port_pc": int(svc.get('ipsec_port_pc') or 0)}
    elif 'ipsec-3gpp' in security:
        security = [m for m in security if m != 'ipsec-3gpp']   # 포트쌍 미설정 = 도달 경로 없음
    # 채널 정책 집행 — 서버 게이트와 같은 술어 (sip_access_security.md §3.1·§8.2,
    #   CspUser::requiresTls = sip_transport=TLS ∨ auth_scheme=aka): TLS 정책 가입자와 AKA 가입자는
    #   비-TLS 채널의 요청이 403 이므로 단말이 고를 수 없다 — 목록을 TLS 하나로 좁히고 enforced 를
    #   표시한다. 예외: 서비스가 ipsec-3gpp 를 제시하면 AKA 의 유효 채널이 TLS 또는 IPsec SA 두
    #   갈래라 좁히지 않는다(IPsec 부트스트랩 = 평문 초기 REGISTER, §8.3). TLS 포트 미설정이면
    #   도달 경로가 없어 좁히지 못한다(운영 오설정 — 단말은 403 을 받는다).
    enforced = ((sip_transport or '').upper() == 'TLS' or (bool(aka) and ipsec is None)) and bool(tls_port)
    if enforced:
        transports = [t for t in transports if t['transport'] == 'TLS']
    if not any(t['transport'] == default for t in transports):
        default = transports[0]['transport']
    default_port = next(t['port'] for t in transports if t['transport'] == default)
    # 미디어 SRTP(SDES) 정책 (media_security.md §7.2): 단말 srtp_use 의 SoT — 서버 접속서비스
    #   정책과 같은 값을 내려 단말·서버가 한 SoT 를 본다. 값이 이상하면 off(조용한 상향 금지).
    #   ⚠ csc.json Provisioning.Services.<kind>.media_srtp 는 CSP access_services.media_srtp 와
    #     일치해야 한다(csc 는 CSP 를 조회하지 않는다).
    media_security = str(svc.get('media_srtp') or 'off').lower()
    if media_security not in ('off', 'optional', 'required'):
        media_security = 'off'
    # 접속서비스 능력 — 단말이 기능 노출을 결정하는 서버측 사실. smsGateway = 외부망 휴대전화 SMS/LMS 게이트웨이
    #   (IBCF→SMSC TS 24.341 / SMPP) 연결 여부(dispatch_desktop_ui.md §4.3 — 외부 번호 [문자] 활성 조건). CIMS 는 게이트웨이를
    #   내장하지 않으므로 기본 false; 등록 가입자 간 MESSAGE 전달은 이 값과 무관하다.
    sms_gw = svc.get('sms_gateway', False)
    # UDP→TCP 승격 비활성(sip.udpNoTcpSwitch) — 단말 pjsip 의 RFC 3261 §18.1.1 승격을 끄는 사이트 옵션(통제된 망,
    #   IP 프래그먼트 통과 전제). csc.json Provisioning.Services.<kind>.udp_no_tcp_switch, 기본 false(규격대로).
    udp_no_tcp = svc.get('udp_no_tcp_switch', False)
    udp_no_tcp = udp_no_tcp is True or str(udp_no_tcp).lower() == 'true'
    capabilities = {"smsGateway": sms_gw is True or str(sms_gw).lower() == 'true'}
    profile = {
        "kind": kind,
        "capabilities": capabilities,
        "sip": {
            "host": svc.get('host') or host_ip,     # 빈값 → 요청 Host(올인원). 다중노드면 CSP/PSP VIP.
            # port/transport = 기본값의 유효 쌍. 목록을 모르는 구 단말이 이 두 필드만 읽으므로 유지한다.
            "port": default_port,
            "transport": default,
            "transports": transports,
            "default": default,
            "enforced": enforced,   # true = 서버가 이 transport 를 집행(다른 채널 403)
            "domain": svc.get('domain') or IDMS_DOMAIN,
            "security": security,   # RFC 3329 제시 목록 — ["tls"] | ["tls","ipsec-3gpp"]
            "mediaSecurity": media_security,   # off|optional|required — 단말 srtp_use 정책
            "udpNoTcpSwitch": udp_no_tcp,      # true = 단말이 큰 요청도 UDP 로(승격 없음) — 사이트 옵션
            **({"ipsec": ipsec} if ipsec else {}),
        },
        "account": account,
    }
    if kind == "ptt":
        # MCData 서비스 설정 (TS 24.484 <max-payload-size-sds-cplane-bytes> 대응).
        # 0/미설정=무제한(단말은 항상 C-plane). 값 설정 시 초과 SDS 를 MSRP(media plane)로 전환.
        # ⚠ CSP csp.json Setup.McData.MaxPayloadSizeSdsCplaneBytes 와 운영자 동기 유지.
        mcdata_cfg = (PROVISIONING.get('McData') or {}) if isinstance(PROVISIONING, dict) else {}
        profile["mcdata"] = {
            "maxPayloadSdsCplaneBytes": int(mcdata_cfg.get('MaxPayloadSdsCplaneBytes', 0) or 0),
        }
        # GMS XCAP 그룹 생성 자격 (mcptt_authorization.md §3) — 앱이 [새 그룹] 노출 여부를 결정.
        #   수정·삭제 자격은 그룹 소유라 그룹 목록의 is_owner 로 준다(여기 아님).
        profile["allowCreateGroup"] = bool(get_user_profile(sid).get('allow_create_group'))
    return profile

# ─── 관제 데스크 발견(discovery) — /provisioning/me `phoneGroup`·`dispatch` 두 블록 (dispatch_center.md §8.4) ───

def _extension_of(msisdn: str) -> str:
    """내선 라벨 = E.164 끝자리 N 자리(설정 Provisioning.ExtensionDigits, 기본 4, 0=전체).
    망 주소가 아니라 관제 앱 주소록·그룹원 띠의 표시 라벨이다(dispatch_desktop_ui.md §13)."""
    digits = ''.join(ch for ch in (msisdn or '') if ch.isdigit())
    n = 4
    if isinstance(PROVISIONING, dict) and PROVISIONING.get('ExtensionDigits') not in (None, ''):
        try:
            n = int(PROVISIONING.get('ExtensionDigits'))
        except (TypeError, ValueError):
            n = 4
    return digits[-n:] if n > 0 else digits


def _tel_uri(msisdn: str) -> str:
    """가입 id(E.164) → tel: URI. 이미 scheme 이 있으면 그대로(시스템 관례 = tel: 형, `_provision_service` mcpttId 와 동일)."""
    s = (msisdn or '').strip()
    if not s:
        return ''
    return s if s.startswith(('tel:', 'sip:')) else f"tel:{s}"


def _content_etag_json(obj) -> str:
    """내용 파생 ETag(RFC 7232 강한 검증자) — 정규화 JSON 의 sha256 앞 32 hex, 따옴표 포함. 같은 내용=같은 값."""
    canon = json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(',', ':'))
    return '"' + hashlib.sha256(canon.encode('utf-8')).hexdigest()[:32] + '"'


_MEMBER_SELECT = ("SELECT u.id, u.name, s.id, COALESCE(m.group_id,''), "
                  "(SELECT MIN(p.id) FROM ptt_subscriptions p WHERE p.user_id=u.id) ")
_MEMBER_ORDER = " ORDER BY CASE WHEN m.group_id=%s THEN 0 ELSE 1 END, m.group_id, m.alert_order, s.id"


def _member_sql(cur) -> str:
    """전화 가족 회선(volte∪voip — 테이블 존재에 따라 UNION, services.subscriptions) ⋈ users ⟕ phone_group_members."""
    return (_MEMBER_SELECT + "FROM " + _subs.phone_union_sql(cur) + " s JOIN users u ON u.id=s.user_id "
            "LEFT JOIN phone_group_members m ON m.user_id=s.id")


def _member_rows(cur, group_ids, own_gid: str, all_subscribers: bool = False) -> list:
    """전화 회선(volte·voip) ⋈ users ⟕ phone_group_members → 항목 {userId, name, volteAor, pttId, extension, groupId}.
    volteAor 는 전화 가족 회선의 AoR(키 이름은 전화 가족 축 'volte'). group_ids = 그 전화 그룹들의 멤버만, all_subscribers =
    전 가입자(WHERE 없음). 투영·정렬은 하나 — 자기 그룹(alert_order) → 그 외."""
    params = []
    sql = _member_sql(cur)
    if not all_subscribers:
        ids = sorted(g for g in (group_ids or set()) if g)
        if not ids:
            return []
        sql += " WHERE m.group_id IN (" + ",".join(["%s"] * len(ids)) + ")"
        params += ids
    params.append(own_gid or '')
    cur.execute(sql + _MEMBER_ORDER, params)
    return [{"userId": uid, "name": name or "", "volteAor": _tel_uri(vid),
             "pttId": _tel_uri(pid) if pid else "", "extension": _extension_of(vid), "groupId": mg or ""}
            for uid, name, vid, mg, pid in cur.fetchall()]


def _ptt_only_rows(cur) -> list:
    """PTT 전용 가입자(전화 회선 volte·voip 없음 — 현장 PTT 단말) — monitor_call=all 에서만 감시 대상: 관제 앱이 그 PTT 회선에
    dialog 를 구독해 타인 간 사설콜·애드혹 세션을 본다(dispatch_center.md §5.6a). volteAor 는 빈 문자열, 목록 끝."""
    cur.execute("SELECT u.id, u.name, MIN(p.id) FROM ptt_subscriptions p JOIN users u ON u.id=p.user_id "
                "WHERE NOT EXISTS (SELECT 1 FROM " + _subs.phone_union_sql(cur) + " v WHERE v.user_id=u.id) "
                "GROUP BY u.id, u.name ORDER BY MIN(p.id)")
    return [{"userId": uid, "name": name or "", "volteAor": "", "pttId": _tel_uri(pid),
             "extension": _extension_of(pid), "groupId": ""} for uid, name, pid in cur.fetchall()]


def _phone_group_block(cur, user_id) -> Optional[dict]:
    """`phoneGroup` — person 의 회선이 전화 그룹 소속일 때. members[] = 같은 그룹원(그룹원 상태 띠·BLF·지정 픽업 대상,
    CanWatch 규칙 1). 유선 전화 기능이며 관제 권한과 무관하다. 미소속·테이블 미적용이면 None."""
    from handlers import dispatch as _pg
    gid = _pg.phone_group_of_person(cur, user_id)
    if not gid:
        return None
    cur.execute("SELECT g.id, g.name, COALESCE(g.pilot_id,'') FROM phone_groups g WHERE g.id=%s", (gid,))
    r = cur.fetchone()
    if not r:
        return None
    members = [{k: v for k, v in m.items() if k != 'groupId'} for m in _member_rows(cur, {gid}, gid)]
    block = {"groupId": gid, "groupName": r[1] or "", "pilotId": r[2] or "", "members": members}
    block["etag"] = _content_etag_json(block)
    return block


def _role_org_code(cur, role: dict) -> str:
    org_id = role.get('org_id')
    if org_id is None:
        return ''
    cur.execute("SELECT code FROM organizations WHERE id=%s", (org_id,))
    r = cur.fetchone()
    return ((r['code'] if isinstance(r, dict) else r[0]) if r else '') or ''


def _dispatch_block(cur, user_id, pg: Optional[dict]) -> Optional[dict]:
    """`dispatch` — person 에게 역할이 배정돼 있을 때(mcptt_authorization.md §2). **서버가 범위 enum 을 해석한 대상 목록**:
    - members[]    dialog 감시(RFC 4235) 대상 = CSP `CanWatch` 와 같은 규칙(dispatch_center.md §5.2): 자기 전화 그룹원은
                   규칙 1(같은 픽업 그룹)로 항상 + 역할 `monitor_call` — own=자기 그룹, listed=대상 그룹원, all=전 가입자
                   (+ PTT 전용 가입자, volteAor=""). 항목 `groupId` = 그 가입자의 전화 그룹.
    - pttTargets[] conference 구독·청취 대상 = `CanListenPtt` 와 같은 규칙(§5.6): listed 대상, all 전 그룹.
    - 전환기 합성 필드(구 앱): groupId/groupName/pilotId(=phoneGroup), monitorScope(=monitorCall), directoryAdmin(=directoryWrite).
      전화 그룹만 있고 역할이 없으면 합성 필드 + members[](그룹원) 만, 범위는 none.
    앱은 enum 을 해석하지 않는다. 범위 판정 규칙은 CSP(게이트)와 여기(목록) 두 곳에만 있고 같아야 한다."""
    from services import authz as _az
    role = _az.role_of(cur, _az.user_principal(user_id))
    own_gid = pg['groupId'] if pg else ''
    synth = {"groupId": own_gid, "groupName": pg['groupName'] if pg else "", "pilotId": pg['pilotId'] if pg else ""}
    if role is None:
        if not pg:
            return None
        block = dict(synth, monitorScope="none", pttListen="none", listenVisibility="hidden",
                     directoryAdmin="none", orgCode="",
                     members=[dict(m, groupId=own_gid) for m in pg['members']], pttTargets=[])
        block["etag"] = _content_etag_json(block)
        return block
    mon_mode = role.get('monitor_call') or 'none'
    ptt_mode = role.get('ptt_listen') or 'none'
    mon_targets, _ptt_targets = _az.role_targets(cur, role['id'])
    if mon_mode == 'all':
        members = _member_rows(cur, None, own_gid, all_subscribers=True) + _ptt_only_rows(cur)
    elif mon_mode == 'listed':
        members = _member_rows(cur, ({own_gid} if own_gid else set()) | set(mon_targets), own_gid)
    else:   # none / own — 같은 픽업 그룹은 CanWatch 규칙 1 로 항상 허용
        members = _member_rows(cur, {own_gid}, own_gid) if own_gid else []
    if ptt_mode == 'all':
        cur.execute("SELECT mcptt_group_id, name FROM ptt_groups ORDER BY mcptt_group_id")
        rows = cur.fetchall()
    elif ptt_mode == 'listed':
        cur.execute("SELECT g.mcptt_group_id, g.name FROM role_ptt_targets t "
                    "JOIN ptt_groups g ON g.id=t.ptt_group_id WHERE t.role_id=%s ORDER BY g.mcptt_group_id", (role['id'],))
        rows = cur.fetchall()
    else:
        rows = []
    targets = [{"id": mid, "uri": _group_uri(mid), "name": name or ""} for mid, name in rows]
    dw = role.get('directory_write') or 'none'
    block = {"roleId": role['id'], "roleName": role.get('name') or "",
             "monitorCall": mon_mode, "pttListen": ptt_mode, "listenVisibility": role.get('listen_visibility') or "hidden",
             "directoryWrite": dw, "orgCode": _role_org_code(cur, role),
             "members": members, "pttTargets": targets}
    block.update(synth)
    block["monitorScope"] = mon_mode
    block["directoryAdmin"] = dw
    block["etag"] = _content_etag_json(block)
    return block


def dispatch_discovery(cur, user_id) -> dict:
    """/provisioning/me 의 두 블록 — {"phoneGroup": …|None, "dispatch": …|None}. 호출자는 None 인 키를 생략한다
    (테이블 미적용 DB 는 예외 → 호출자가 두 블록 다 생략)."""
    pg = _phone_group_block(cur, user_id)
    return {"phoneGroup": pg, "dispatch": _dispatch_block(cur, user_id, pg)}


async def handle_provisioning_me(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token = extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return unauthorized(args)
    # scope 분리: provisioning 토큰만 허용(빈 scope=레거시 허용). mcptt 전용 토큰은 거부 → 평면 혼용 방지.
    _sc = token.get('scope') or []
    if isinstance(_sc, str):
        _sc = _sc.split()
    if _sc and SCOPE_PROVISIONING not in _sc:
        return HandlerResult(status=403,
                             body={"error": "insufficient_scope", "required": SCOPE_PROVISIONING},
                             media_type="application/json",
                             headers=_bearer_challenge('insufficient_scope', SCOPE_PROVISIONING))
    msisdn = _msisdn_from_id(token_line_id(token))
    # 시그널링(CSP/PSP) host 폴백 = 요청 Host (올인원 전제). CSC 자기 주소는 공개 URL 정본에서.
    host_ip = (args.headers.get('host') or args.headers.get('Host') or '').split(':')[0]
    csc_host, csc_port = public_host_port(args)
    if not _DB_CONFIG:
        return HandlerResult(status=503, body={"error": "db_unavailable"}, media_type="application/json")

    services: list = []
    display_name = None
    blocks = {}
    try:
        import pymysql
        conn = pymysql.connect(host=_DB_CONFIG.get('Host', '127.0.0.1'),
                               port=int(_DB_CONFIG.get('Port', 3306)),
                               user=_DB_CONFIG.get('User', 'root'),
                               password=_DB_CONFIG.get('Password', ''),
                               database=_DB_CONFIG.get('Db', 'cims'), connect_timeout=5)
        try:
            cur = conn.cursor()
            # 로그인 msisdn 으로 person(user_id) 확인 → 그 person 의 volte·voip·ptt 전 서비스 반환(테이블 = kind, 레지스트리 순서).
            user_id = None
            for _k, t in _subs.tables(cur):
                cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (msisdn,))
                r = cur.fetchone()
                if r:
                    user_id = r[0]
                    break
            if user_id is not None:
                for kind, t in _subs.tables(cur):
                    # sip_transport 는 가입자 단위 override (migrate_subscription_transport.sql).
                    #   구 스키마(컬럼 부재) DB 에서도 동작하도록 실패 시 기존 질의로 폴백한다.
                    #   AKA 열(auth_scheme/k_enc/opc_enc/amf)은 migrate_subscription_aka.sql 이후에만 — 같은 폴백 사슬.
                    #   service_ref(접속서비스 name)로 그 kind 안의 서비스 항목을 고른다(같은 kind 의 서비스가 여럿일 때 —
                    #   sip_service_model.md §2-9). kind 는 테이블에서 온다.
                    try:
                        cur.execute(f"SELECT id, imsi, auth_id, sip_transport, ha1, auth_scheme, k_enc, opc_enc, amf, "
                                    f"COALESCE(service_ref,'') FROM {t} WHERE user_id=%s ORDER BY id", (user_id,))
                        rows = cur.fetchall()
                    except Exception:
                        try:
                            cur.execute(f"SELECT id, imsi, auth_id, sip_transport, ha1, COALESCE(service_ref,'') FROM {t} "
                                        "WHERE user_id=%s ORDER BY id", (user_id,))
                            rows = [(r[0], r[1], r[2], r[3], r[4], 'digest', '', '', '', r[5]) for r in cur.fetchall()]
                        except Exception:
                            try:
                                cur.execute(f"SELECT id, imsi, auth_id, sip_transport, COALESCE(service_ref,'') FROM {t} "
                                            "WHERE user_id=%s ORDER BY id", (user_id,))
                                rows = [(r[0], r[1], r[2], r[3], '', 'digest', '', '', '', r[4]) for r in cur.fetchall()]
                            except Exception:
                                cur.execute(f"SELECT id, imsi, auth_id FROM {t} WHERE user_id=%s ORDER BY id",
                                            (user_id,))
                                rows = [(r[0], r[1], r[2], None, '', 'digest', '', '', '', '') for r in cur.fetchall()]
                    for sid, imsi, auth_id, transport, ha1, scheme, k_enc, opc_enc, amf, sref in rows:
                        aka = None
                        if scheme == 'aka' and k_enc and opc_enc:
                            try:
                                from services.auc import auc as _auc
                                k, opc = _auc.decrypt_keys(k_enc, opc_enc)
                                aka = {"k": k.hex(), "opc": opc.hex(), "amf": amf or "8000"}
                            except Exception as e:
                                logger.log_error(f"[provisioning/me] aka key material unavailable for {sid}: {e}")
                                aka = {"k": "", "opc": "", "amf": amf or "8000"}
                        services.append(_provision_service(kind, sid, imsi or '', auth_id or '', host_ip,
                                                          transport or '', ha1 or '', aka, service_ref=sref or ''))
                cur.execute("SELECT name FROM users WHERE id=%s", (user_id,))
                rr = cur.fetchone()
                display_name = rr[0] if rr else None
                # 전화 그룹·관제 역할 (dispatch_center.md §8.4) — `phoneGroup`(소속 전화 그룹·대표번호·그룹원) +
                #   `dispatch`(배정 역할·범위 + 서버가 해석한 감시 대상 members[]/pttTargets[]/etag, `dispatch_discovery`).
                #   테이블 미적용 DB 에서는 블록을 생략한다(null 금지 — Android org.json 문자열화).
                try:
                    blocks = dispatch_discovery(cur, user_id) or {}
                except Exception as e:
                    logger.log_info(f"[provisioning/me] phoneGroup/dispatch blocks skipped: {e}")
                    blocks = {}
        finally:
            conn.close()
    except Exception as e:
        logger.log_error(f"[provisioning/me] DB error: {e}")
        return HandlerResult(status=503, body={"error": "db_error", "detail": str(e)}, media_type="application/json")

    # 홈 국가코드(digits, 예 '82') — 단말 번호 로컬 표기(+82… → 0…)의 SoT.
    # ① 접속서비스 다이얼 플랜 country_code(CSP 정본 미러 — 서버 번호 번역과 같은 값, sip_service_model.md §2-10)
    # ② 설정 Provisioning.CountryCode ③ 로그인 msisdn 에서 유도.
    country = ''
    try:
        from services import access_services as _acc
        country = _acc.country_code(None, '', 'volte')
    except Exception as e:
        logger.log_info(f"[provisioning/me] access_services country_code skipped: {e}")
    if not country and isinstance(PROVISIONING, dict):
        country = str(PROVISIONING.get('CountryCode') or '').lstrip('+').strip()
    if not country:
        country = _country_code_of(msisdn)
    body = {
        "user": {"displayName": display_name, "loginId": token.get('sub') or msisdn},
        "csc": {"host": csc_host, "port": csc_port},
        "countryCode": country,     # 판정 불가 시 "" (null 금지 — Android org.json 이 "null" 문자열화)
        "services": services,
    }
    for key in ("phoneGroup", "dispatch"):
        if blocks.get(key):
            body[key] = blocks[key]
    dispatch = body.get("dispatch")
    # 버전(ETag) — 응답 전체의 내용 해시(RFC 7232). 단말 If-None-Match 일치 시 304 — 관제 앱의 주기 재조회
    #   (발견 목록 갱신 감지)가 전송 없이 끝난다. 블록의 etag 는 블록 단위 값(대상 변경만 볼 때).
    etag = _content_etag_json(body)
    inm = args.headers.get('if-none-match') or args.headers.get('If-None-Match')
    if inm and inm == etag:
        logger.log_info(f"[provisioning/me] msisdn={msisdn} not-modified etag={etag}")
        return HandlerResult(status=304, headers={"ETag": etag})
    disp_log = (f" dispatch={dispatch.get('roleId') or '-'}/{dispatch.get('groupId') or '-'}"
                f"/{dispatch['monitorScope']}:{len(dispatch['members'])}"
                f"/{dispatch['pttListen']}:{len(dispatch['pttTargets'])}") if dispatch else ""
    logger.log_info(f"[provisioning/me] msisdn={msisdn} services={[s['kind'] for s in services]} cc={country}"
                    f"{disp_log} etag={etag}")
    return HandlerResult(status=200, body=body, headers={"ETag": etag}, media_type="application/json")


# ── 통합 이력 조회 (GET /provisioning/history, dispatch_center.md §5.6·§8.4) ──

_HISTORY_KINDS = ("call", "ptt", "message")


def _dispatch_scope_sets(cur, user_id) -> Optional[dict]:
    """관제사의 **역할** 범위를 이력·녹취 대조용 집합으로(dispatch_center.md §5.7a·§5.7b) — dispatch_discovery 의
    `dispatch` 블록(/provisioning/me 와 같은 SoT)을 재사용해 members(monitor_call 감시 대상 VoLTE user-part)·
    ptt_groups(ptt_listen 청취 대상 mcptt_group_id)를 뽑는다. 역할이 없거나 두 범위가 모두 none 이면 None(→ 403
    no_monitor_scope). 범위 규칙(CanWatch 규칙 2/CanListenPtt)은 dispatch_discovery 한 곳."""
    from services import dispatch_history as _dh
    blocks = dispatch_discovery(cur, user_id) or {}
    d = blocks.get("dispatch") if isinstance(blocks, dict) else None
    if not d or not d.get("roleId"):
        return None
    mon = d.get("monitorCall") or d.get("monitorScope") or "none"
    ptt = d.get("pttListen") or "none"
    if mon == "none" and ptt == "none":
        return None
    return {
        "roleId": d["roleId"],
        "groupId": d.get("groupId") or "",
        "monitorCall": mon,
        "pttListen": ptt,
        "members": ({_dh.userpart(m.get("volteAor")) for m in d.get("members", []) if m.get("volteAor")}
                    if mon != "none" else set()),
        "ptt_groups": {t.get("id") for t in d.get("pttTargets", []) if t.get("id")},
    }


async def handle_provisioning_history(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """관제 데스크 통합 이력 — `?kind=call|ptt|message&since=&limit=` (PKCE provisioning 토큰).

    진행 중(live) 상태는 표준 구독(RFC 4235/4575)이 담당하고 이 API 는 대체하지 않는다 — 관제 그룹
    **범위 안의 지난 이력**만 커서로 준다. 범위 밖·관제 미소속은 403. 열람은 감사(E-AUD-016 tap_mode=history)."""
    token = extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return unauthorized(args)
    _sc = token.get('scope') or []
    if isinstance(_sc, str):
        _sc = _sc.split()
    if _sc and SCOPE_PROVISIONING not in _sc:
        return HandlerResult(status=403, body={"error": "insufficient_scope", "required": SCOPE_PROVISIONING},
                             media_type="application/json",
                             headers=_bearer_challenge('insufficient_scope', SCOPE_PROVISIONING))
    qp = getattr(args, 'query_params', None) or {}

    def _q(name, default=None):
        v = qp.get(name)
        if isinstance(v, list):
            return v[0] if v else default
        return v if v not in (None, '') else default

    kind = str(_q('kind', 'call')).lower()
    if kind not in _HISTORY_KINDS:
        return HandlerResult(status=400, body={"error": "invalid_kind", "allowed": list(_HISTORY_KINDS)},
                             media_type="application/json")
    from services import dispatch_history as _dh
    since_dt = _dh.parse_ts(_q('since'))
    until_dt = _dh.parse_ts(_q('until'))                 # 창 조회(이력 화면) — 없으면 폴링 커서(now 까지)
    try:
        limit = int(_q('limit', 200))
    except (TypeError, ValueError):
        limit = 200

    msisdn = _msisdn_from_id(token_line_id(token))
    if not _DB_CONFIG:
        return HandlerResult(status=503, body={"error": "db_unavailable"}, media_type="application/json")
    scope = None
    try:
        import pymysql
        conn = pymysql.connect(host=_DB_CONFIG.get('Host', '127.0.0.1'), port=int(_DB_CONFIG.get('Port', 3306)),
                               user=_DB_CONFIG.get('User', 'root'), password=_DB_CONFIG.get('Password', ''),
                               database=_DB_CONFIG.get('Db', 'cims'), connect_timeout=5)
        try:
            cur = conn.cursor()
            user_id = None
            for _k, t in _subs.tables(cur):
                cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (msisdn,))
                r = cur.fetchone()
                if r:
                    user_id = r[0]
                    break
            group_key_of = {}
            if user_id is not None:
                scope = _dispatch_scope_sets(cur, user_id)
                # 청취 그룹 → 녹취 저장 키(ptt_groups.id) — PTT 창 조회가 OAM 세션 인덱스를 그 키로 좁힌다.
                if kind == "ptt" and until_dt is not None and scope and scope.get("ptt_groups"):
                    cur.execute("SELECT id, mcptt_group_id FROM ptt_groups")
                    group_key_of = {str(r[0]): r[1] for r in (cur.fetchall() or [])}
        finally:
            conn.close()
    except Exception as e:
        logger.log_error(f"[provisioning/history] DB error: {e}")
        return HandlerResult(status=503, body={"error": "db_error", "detail": str(e)}, media_type="application/json")

    # 역할 없음·두 범위 모두 none = 감시 범위 없음 → 403 (범위 밖 열람 거부, dispatch_center.md §5.7a).
    if not scope:
        return HandlerResult(status=403, body={"error": "no_monitor_scope"}, media_type="application/json")

    # PTT 창 조회(이력 화면) = 콘솔 PTT 이력의 읽기 모델(OAM ptt_index — 발언 턴·화자·발화·동시 발언·참여자)을 프록시.
    #   범위 = 청취 그룹의 저장 키. OAM 에 닿지 않으면 파일 스캔으로 폴백(지표 없음). 폴링(until 없음)은 파일 스캔.
    items = None
    if kind == "ptt" and until_dt is not None:
        from handlers import dispatch_recordings as _dr
        w_since, w_until = _dh.window(since_dt, until_dt)
        keys = {k for k, g in group_key_of.items() if g in scope["ptt_groups"]}
        oam_items = _dr.fetch_ptt_sessions(_OAM_CONFIG, w_since, w_until, keys)
        if oam_items is not None:
            rows = [r for r in (_dh.ptt_row_from_oam(it, _RECORDINGS_DIR) for it in oam_items)
                    if r and r["groupId"] in scope["ptt_groups"]]
            items, next_since, hours = _dh.finish_rows(rows, w_since, w_until, limit)
    if items is None:
        items, next_since, hours = _dh.query_ex(_RECORDINGS_DIR, _STATE_DIR, kind, scope, since_dt, limit, until_dt)
    # 앱(HistoryClient) 와이어 계약 — dispatch_desktop_ui.md §13 / android_ue_provisioning.md §3-2.
    #   items[]{id,time,kind,event,from,to,group,duration,emergency,text,recordingId,hasRecording + 종류별 확장 필드}
    #   + 최상위 next·hours(시간대 분포) + 응답 ETag/304.
    wire = [_dh.format_item(r) for r in items]
    body = {"items": wire, "next": next_since, "hours": hours}
    etag = _content_etag_json(body)
    inm = args.headers.get('if-none-match') or args.headers.get('If-None-Match')
    if inm and inm == etag:
        logger.log_info(f"[provisioning/history] msisdn={msisdn} kind={kind} not-modified etag={etag}")
        return HandlerResult(status=304, headers={"ETag": etag})

    # 감사 — 당사자 모르게 이력을 열람하는 동작(E-AUD-016 tap_mode=history). 실패는 조회를 막지 않는다.
    #   변경 없는 폴링(304)은 감사하지 않는다(위에서 반환) — 실제 열람(새 항목/최초)만 남긴다.
    try:
        from services import fm_reporter as _fm
        r = _fm.get()
        if r is not None:
            r.send_event('call_monitored', kind='audit', mo=f"{r.node}/csc",
                         params={"monitor": msisdn, "role": scope["roleId"], "group": scope["groupId"],
                                 "tap_mode": "history", "hist_kind": kind, "count": len(wire),
                                 "monitor_call": scope["monitorCall"], "ptt_listen": scope["pttListen"],
                                 "kind_ko": f"{getattr(_fm, 'HIST_KIND_KO', {}).get(kind, kind)} 이력", "phase_ko": "열람",
                                 "who_ko": f"{msisdn}, {len(wire)}건 (역할 {scope['roleId']})"},
                         message=(f"{getattr(_fm, 'HIST_KIND_KO', {}).get(kind, kind)} 이력 열람 — {msisdn}, "
                                  f"{len(wire)}건 (역할 {scope['roleId']})"))
    except Exception as e:
        logger.log_warning(f"[provisioning/history] audit emit failed: {e}")

    logger.log_info(f"[provisioning/history] msisdn={msisdn} kind={kind} role={scope['roleId']} "
                    f"m={len(scope['members'])} g={len(scope['ptt_groups'])} → {len(wire)} items etag={etag}")
    return HandlerResult(status=200, body=body, headers={"ETag": etag}, media_type="application/json")


async def handle_provisioning_directory(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """회사 전화번호부 — 조직 트리 + 가입자. 단말 '회사 연락처'(읽기전용) 소스.

    provisioning scope 토큰 필요. 조직(organizations) 계층(parent_id)과 가입자를 반환한다.
    `?service=volte|voip|ptt` 로 가입 테이블을 고른다(기본 volte). `volte` 는 전화 가족(volte∪voip) 합산 — 관제·전화 단말의
    회사 연락처에 유선 번호가 빠지지 않게(sip_service_model.md §2-9). `voip` 는 유선 회선만, PTT 단말은 `service=ptt` 로
    1:1 private call 대상(ptt_subscriptions)을 받는다.
    `orgs[]` = 조직 트리(code/name/parent code/sort), `entries[]` = 가입자(org=조직 code).
    users.org_id 는 조직 코드(organizations.code)를 담는다. ETag 는 내용 해시라 서비스별로 다르다.
    """
    token = extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return unauthorized(args)
    _sc = token.get('scope') or []
    if isinstance(_sc, str):
        _sc = _sc.split()
    if _sc and SCOPE_PROVISIONING not in _sc:
        return HandlerResult(status=403,
                             body={"error": "insufficient_scope", "required": SCOPE_PROVISIONING},
                             media_type="application/json",
                             headers=_bearer_challenge('insufficient_scope', SCOPE_PROVISIONING))
    if not _DB_CONFIG:
        return HandlerResult(status=503, body={"error": "db_unavailable"}, media_type="application/json")

    orgs: list = []
    entries: list = []
    try:
        import pymysql
        conn = pymysql.connect(host=_DB_CONFIG.get('Host', '127.0.0.1'),
                               port=int(_DB_CONFIG.get('Port', 3306)),
                               user=_DB_CONFIG.get('User', 'root'),
                               password=_DB_CONFIG.get('Password', ''),
                               database=_DB_CONFIG.get('Db', 'cims'), connect_timeout=5)
        try:
            cur = conn.cursor()
            # 조직 트리 — parent_id(id)를 parent code 로 환산해 내려준다.
            cur.execute("SELECT id, code, name, parent_id, sort_order FROM organizations")
            rows = cur.fetchall()
            id2code = {r[0]: r[1] for r in rows}
            for _id, code, name, parent_id, so in rows:
                orgs.append({"code": code or "", "name": name or "",
                             "parent": id2code.get(parent_id, "") if parent_id is not None else "",
                             "sort": so or 0})
            # 가입자 — org = users.org_id(조직 code). service 인자로 가입 테이블 선택.
            service = (getattr(args, 'query_params', None) or {}).get('service') or 'volte'
            if service == 'ptt':
                rel = _subs.table('ptt')
            elif service == 'voip':
                rel = _subs.table('voip') if _subs.has_table(cur, 'voip') else "(SELECT id, user_id FROM volte_subscriptions WHERE 0)"
            else:
                rel = _subs.phone_union_sql(cur)          # 전화 가족 합산(volte∪voip)
            cur.execute(
                f"SELECT u.org_id AS org, u.name AS name, v.id AS msisdn "
                f"FROM {rel} v JOIN users u ON u.id = v.user_id "
                "ORDER BY u.name")
            for org, name, msisdn in cur.fetchall():
                entries.append({"org": org or "", "name": name or "", "msisdn": msisdn or ""})
        finally:
            conn.close()
    except Exception as e:
        logger.log_error(f"[provisioning/directory] DB error: {e}")
        return HandlerResult(status=503, body={"error": "db_error", "detail": str(e)}, media_type="application/json")

    # 버전(ETag) — 내용 해시. 단말의 If-None-Match 와 같으면 304(다운로드 생략).
    import hashlib, json as _json
    payload = {"orgs": orgs, "entries": entries}
    canon = _json.dumps(payload, sort_keys=True, ensure_ascii=False, separators=(',', ':'))
    etag = '"' + hashlib.sha256(canon.encode('utf-8')).hexdigest()[:32] + '"'
    inm = args.headers.get('if-none-match') or args.headers.get('If-None-Match')
    if inm and inm == etag:
        logger.log_info(f"[provisioning/directory] not-modified etag={etag}")
        return HandlerResult(status=304, headers={"ETag": etag})
    logger.log_info(f"[provisioning/directory] orgs={len(orgs)} entries={len(entries)} etag={etag}")
    return HandlerResult(status=200, body=payload, headers={"ETag": etag}, media_type="application/json")


# Route Mapping (MCPTT server — port 4430)
CSC_HANDLER_LIST = [
    # OIDC Discovery (3GPP TS 33.180 / OpenID Connect Discovery 1.0)
    ("/.well-known/openid-configuration", handle_openid_config, {}),
    # 자동 프로비저닝 (UE 로그인 후 서비스별 프로파일)
    ("/provisioning/me", handle_provisioning_me, {}),
    # 회사 전화번호부 (조직별 VoLTE 가입자 — 단말 '회사 연락처' 읽기전용 소스)
    ("/provisioning/directory", handle_provisioning_directory, {}),
    # 관제 데스크 통합 이력 (call|ptt|message — 범위 게이트, 감사 E-AUD-016)
    ("/provisioning/history", handle_provisioning_history, {}),
    # IdMS (3GPP TS 33.180 / OAuth 2.0 PKCE)
    ("/idms/authreq",     handle_auth_req,          {}),
    ("/idms/tokenreq",    handle_token_req,          {}),
    ("/idms/introspect",  handle_token_introspect,   {}),
    ("/idms/jwks",        handle_idms_jwks,          {}),
    # GMS — list: GET /users/{user_uri}  |  CRUD: /users/{user_uri}/{group_uri}
    ("/org.openmobilealliance.groups/users", handle_group_management, {}),
    # CMS (3GPP TS 24.484)
    ("/org.3gpp.mcptt.ue-init-config/users", handle_ue_init_config, {}),  # 로그인 전 — 익명
    ("/org.3gpp.mcptt.user-profile/users",   handle_user_profile,   {}),
    ("/org.3gpp.mcptt.service-config/users", handle_service_config,  {}),
    ("/org.3gpp.mcvideo.user-profile/users",    handle_mcvideo_user_profile,   {}),   # MCVideo (TS 24.484 §9.3)
    ("/org.3gpp.mcvideo.service-config/global", handle_mcvideo_service_config, {}),   # 전역 문서 (§9.4.2.9)
    # KMS (3GPP TS 33.180 / MIKEY-SAKKE)
    ("/keymanagement/identity/v1/init",    handle_kms_init,    {}),
    ("/keymanagement/identity/v1/keyprov", handle_kms_keyprov, {}),
]
