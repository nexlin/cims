import os
import socket
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

# --- Configuration & Data ---
# JWT 서명 시크릿. 구 하드코딩 default("mcptt_jwt_secret_change_me") 제거 — 알려진 기본값은
# 토큰 위조를 허용하므로 보안 결함. 기본값 = 프로세스 시작 시 임의 생성(예측 불가). 운영은
# IdMs.JwtSecret 로 고정 권장(미설정 시 재기동마다 토큰 무효화). IdMS 가 발급하고 동일 프로세스의
# XCAP 가 검증하므로 임의 시크릿으로도 정상 동작.
SECRET_KEY = _secrets.token_urlsafe(32)
# KMS master secret — 가입자별 키 material 파생용(HKDF). 구 구현은 전 사용자 동일 고정 hex 였음.
#   ⚠ 본 파생은 가입자별 **구조적 프로비저닝**(UserDecryptKey/SSK/PVT 가 사용자마다 다름)을 제공하나,
#   참값 ECCSI/SAKKE(RFC 6507/6508)는 pairing 암호 라이브러리가 필요한 후속 과제다(E2E 암호화 도입 시).
KMS_MASTER_SECRET = _secrets.token_bytes(32)
# IdMS scope 분리 — 평면별 토큰 용도 구분(TS 33.180 / 본 프로젝트 프로비저닝).
#   CIMS 앱 로그인은 두 scope 를 함께 grant 받고, AccountManager 가 refresh 로 용도별 토큰을 좁혀 발급.
SCOPE_PROVISIONING = "cims:provisioning"      # 디바이스 부트스트랩(/provisioning/me)
SCOPE_MCPTT        = "3gpp:mcptt:ptt_server"  # MCPTT 서비스 평면(XCAP/KMS/affiliation)

IDMS_ISSUER = "idms.mcptt.com"
KMS_URI = "kms.mcptt.com"
IDMS_DOMAIN = "mcptt.com"
KMS_CLIENT_REQ_URL = "http://localhost:4421/keymanagement/identity/v1/init"
USERS = {}            # tel:+msisdn → {password,...} (XCAP/profile 키 = MCPTT ID)
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

# S4: service-config 기본값 (TS 24.484). 하드코딩 리터럴 대신 dict 로 두어 config 로 덮어쓰고
#   가입자별(USERS[uri]['service_config']) 오버라이드를 허용한다. load_shared_data 에서 config 반영.
SERVICE_CONFIG_DEFAULTS = {
    "num-levels-group-hierarchy": 3,
    "num-levels-user-hierarchy": 3,
    "max-affiliations-N2": 10,
    "allow-create-delete-group": True,
    "allow-private-call": True,
    "allow-emergency-call": True,
    "allow-alert": True,
    "allow-transmit-request": True,
    "max-on-network-affiliations-N2": 10,
}

CSP_NOTIFY_IP = "127.0.0.1"
CSP_NOTIFY_PORT = 4421
# PSP (PTT 시그널링) — 별도 인스턴스 분리 시 사용. CSP 와 동일하면 broadcast 가
# 동일 endpoint 1번만 호출 (자동 dedup).
PSP_NOTIFY_IP = ""           # ""=PSP 미설정 (legacy: CSP 만 사용)
PSP_NOTIFY_PORT = 4421

# 자동 프로비저닝(/provisioning/me, android_ue_provisioning.md §3) —
#   서비스 kind 별 시그널링 서버/도메인. host 빈값이면 요청 Host(=UE 가 접속한 IP) 사용(올인원 기본).
#   다중 노드면 host 를 CSP/PSP 대표(VIP) 주소로 지정.
PROVISIONING = {}            # config Provisioning: {"Services":{"volte":{host,port,transport,domain}, "ptt":{...}}}
_DB_CONFIG = None            # CimsDatabase (가입자 라이브 조회용)
_MCPTT_PORT = 4430           # csc McpttServer.Port (응답 csc.port)


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


def load_shared_data(config):
    # Do not reassign global variables, modify them in place
    USERS.clear()
    GROUPS.clear()
    
    # Config keys match csc.json structure
    user_path = config.get('Data', {}).get('User')
    group_path = config.get('Data', {}).get('Group')
    db_config = config.get('CimsDatabase')

    # IdmsStorage: file_store 기반 (Phase 8). 전체 config 전달 (runtime_root 추출용).
    storage.init_db(config)
    logger.log_info("IdmsStorage initialized (file_store)")
    
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
                    for table in ('volte_subscriptions', 'ptt_subscriptions'):
                        cur.execute(f"SELECT id, passwd FROM {table}")
                        for row in cur.fetchall():
                            uid = row['id']
                            pw  = row['passwd'] or ''
                            uri = f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}"
                            if uri not in USERS:
                                USERS[uri] = {"password": pw, "name": uid, "profile_etag": "etag_" + uri}
                                logger.log_info(f"Loaded DB User: {uri}")

                    # IdMS 로그인 계정(login_id) — MCPTT ID 는 ptt(없으면 volte) msisdn 에서 tel:+ 파생.
                    LOGIN_ACCOUNTS.clear()
                    cur.execute(
                        "SELECT u.id uid, u.login_id, u.passwd, u.name, "
                        "(SELECT id FROM ptt_subscriptions WHERE user_id=u.id LIMIT 1) ptt, "
                        "(SELECT id FROM volte_subscriptions WHERE user_id=u.id LIMIT 1) volte "
                        "FROM users u WHERE u.login_id IS NOT NULL AND u.login_id<>''")
                    for r in cur.fetchall():
                        msisdn = r.get('ptt') or r.get('volte')
                        if msisdn:
                            mcptt_id = msisdn if str(msisdn).startswith('tel:') else (
                                f"tel:{msisdn}" if str(msisdn).startswith('+') else f"tel:+{msisdn}")
                        else:
                            mcptt_id = f"login:{r['login_id']}"
                        LOGIN_ACCOUNTS[r['login_id']] = {
                            "password": r.get('passwd') or '', "user_id": r['uid'],
                            "mcptt_id": mcptt_id, "name": r.get('name'),
                        }
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
                        "profile_etag": "etag_" + uri
                    }
                    logger.log_info(f"Loaded File User: {uri}")
            except Exception as e:
                logger.log_error(f"Error loading user {fpath}: {e}")

    # Read IdMs config
    global SECRET_KEY, IDMS_ISSUER, KMS_URI, IDMS_DOMAIN, KMS_CLIENT_REQ_URL
    global AUTH_CODE_TTL, ACCESS_TOKEN_TTL, REFRESH_TOKEN_TTL
    idms_config = config.get('IdMs', {})
    if idms_config.get('JwtSecret'):
        SECRET_KEY = idms_config['JwtSecret']
    else:
        logger.log_error("[IdMS] IdMs.JwtSecret 미설정 — 임의 시크릿 사용(재기동 시 토큰 무효화). "
                         "운영은 IdMs.JwtSecret 설정 권장.")
    if idms_config.get('Issuer'):
        IDMS_ISSUER = idms_config['Issuer']
    if idms_config.get('KmsUri'):
        KMS_URI = idms_config['KmsUri']
    if idms_config.get('Domain'):
        IDMS_DOMAIN = idms_config['Domain']
    if idms_config.get('KmsClientReqUrl'):
        KMS_CLIENT_REQ_URL = idms_config['KmsClientReqUrl']
    if idms_config.get('AuthCodeTtl'):
        AUTH_CODE_TTL = int(idms_config['AuthCodeTtl'])
    if idms_config.get('AccessTokenTtl'):
        ACCESS_TOKEN_TTL = int(idms_config['AccessTokenTtl'])
    if idms_config.get('RefreshTokenTtl'):
        REFRESH_TOKEN_TTL = int(idms_config['RefreshTokenTtl'])

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

    # 자동 프로비저닝(/provisioning/me) — DB 핸들 + 서비스별 시그널링/도메인 매핑 보관.
    global _DB_CONFIG, PROVISIONING, _MCPTT_PORT
    _DB_CONFIG = db_config
    PROVISIONING = config.get('Provisioning', {}) or {}
    _MCPTT_PORT = int((config.get('McpttServer', {}) or {}).get('Port', 4430))

    global GROUP_DIR
    if group_path:
        GROUP_DIR = group_path

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
                    cur.execute(
                        "SELECT id, mcptt_group_id, name, video_enabled, priority, encryption, "
                        "emergency_call, imminent_peril_call, emergency_alert, adhoc_enabled, "
                        "org_code, session_start, session_end, "
                        "group_type, on_network, max_members, require_affiliation, alias, "
                        "authorized_user_id, "
                        "(SELECT id FROM ptt_subscriptions WHERE user_id=ptt_groups.authorized_user_id "
                        " ORDER BY id LIMIT 1) AS authorized_user_msisdn "
                        "FROM ptt_groups"
                    )
                    for row in cur.fetchall():
                        gid = row['mcptt_group_id']
                        uri = _group_uri(gid)
                        GROUPS[uri] = {
                            "display_name": row['name'],
                            "video_enabled": bool(row.get('video_enabled', 0)),
                            "priority": row.get('priority', 5),
                            "encryption": bool(row.get('encryption', 0)),
                            "emergency_call": bool(row.get('emergency_call', 0)),
                            "imminent_peril_call": bool(row.get('imminent_peril_call', 1)),
                            "emergency_alert": bool(row.get('emergency_alert', 1)),
                            "adhoc_enabled": bool(row.get('adhoc_enabled', 0)),
                            "org_code": row.get('org_code', ''),
                            "group_type": row.get('group_type', 'prearranged'),
                            "on_network": bool(row.get('on_network', 1)),
                            "max_members": row.get('max_members', 0),
                            "require_affiliation": bool(row.get('require_affiliation', 1)),
                            "alias": row.get('alias', ''),
                            "session_start": row['session_start'].isoformat() if row.get('session_start') else None,
                            "session_end": row['session_end'].isoformat() if row.get('session_end') else None,
                            "etag": f"etag_{gid}",
                            "created_by": "", "created_at": "",
                            # 그룹 소유 (3GPP authorized user) — 파생 MCPTT ID
                            "authorized_user": (f"tel:{row['authorized_user_msisdn']}"
                                                if row.get('authorized_user_msisdn') else ""),
                            "members": []
                        }
                    # 멤버 목록 + users 테이블에서 이름·직함 조회 (group_id=surrogate → mcptt_group_id JOIN)
                    title_col = ", u.title AS user_title" if _users_has_title(cur) else ""
                    cur.execute(
                        "SELECT g.mcptt_group_id AS mcptt_group_id, gm.user_id, gm.priority, gm.role, gm.mcptt_id, "
                        f"       u.name AS user_name{title_col} "
                        "FROM ptt_group_members gm "
                        "JOIN ptt_groups g ON g.id = gm.group_id "
                        "LEFT JOIN ptt_subscriptions ps ON ps.id = gm.user_id "
                        "LEFT JOIN users u ON u.id = ps.user_id "
                        "ORDER BY g.mcptt_group_id, gm.priority"
                    )
                    for row in cur.fetchall():
                        g_uri = _group_uri(row['mcptt_group_id'])
                        uid = row['user_id']
                        m_uri = row.get('mcptt_id') or (f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}")
                        m_name = row.get('user_name') or m_uri
                        if g_uri in GROUPS:
                            GROUPS[g_uri]['members'].append({
                                "uri": m_uri, "name": m_name,
                                "role": row.get('role') or "participant",
                                "priority": row['priority'], "joined_at": "",
                                "title": row.get('user_title') or ""
                            })
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

def refresh_group_members(group_id: str) -> bool:
    """DB에서 해당 그룹 멤버를 재조회해 in-memory GROUPS 에 반영한다.
    admin API 멤버 변경이 GMS 그룹 목록/문서에 즉시 보이도록 admin.py 가 호출
    (GROUPS 는 기동 시 1회 적재 — 이 갱신이 없으면 재기동 전까지 stale)."""
    if not _DB_CONFIG:
        return False
    uri = _group_uri(group_id)
    grp = GROUPS.get(uri)
    if not grp:
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
                title_col = ", u.title AS user_title" if _users_has_title(cur) else ""
                cur.execute(
                    "SELECT gm.user_id, gm.priority, gm.role, gm.mcptt_id, "
                    f"       u.name AS user_name{title_col} "
                    "FROM ptt_group_members gm "
                    "JOIN ptt_groups g ON g.id = gm.group_id "
                    "LEFT JOIN ptt_subscriptions ps ON ps.id = gm.user_id "
                    "LEFT JOIN users u ON u.id = ps.user_id "
                    "WHERE g.mcptt_group_id=%s ORDER BY gm.priority",
                    (group_id,)
                )
                members = []
                for row in cur.fetchall():
                    uid = row['user_id']
                    m_uri = row.get('mcptt_id') or (f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}")
                    members.append({
                        "uri": m_uri, "name": row.get('user_name') or m_uri,
                        "role": row.get('role') or "participant",
                        "priority": row['priority'], "joined_at": "",
                        "title": row.get('user_title') or ""
                    })
        grp['members'] = members
        logger.log_info(f"refresh_group_members({group_id}): {len(members)} members")
        return True
    except Exception as e:
        logger.log_error(f"refresh_group_members({group_id}) failed: {e}")
        return False


# [FIX] Notify CSP logic
_notify_seq = 0

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
                "USER_CHANGED", "GROUP_CHANGED",
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
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for ip, port, label in targets:
            try:
                sock.sendto(msg.encode('utf-8'), (ip, port))
            except Exception as e:
                logger.log_error(f"Notify {label} failed: {e}")
        sock.close()

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

        labels = ",".join(t[2] for t in targets) or "(none)"
        logger.log_info(f"Notify Sent → {labels}: {msg}")
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
    """csp 설정 변경 이력 JSONL append. {CimsRuntimeDir}/csp_config_audit/YYYY/MM/DD.jsonl.

    db_cfg 인자는 호환을 위해 유지 (구 시그니처). 실제로는 file_store 사용.
    실패는 조용히 삼킴 (운영 기능 차단 안 함).
    """
    try:
        from datetime import datetime as _dt
        from services import file_store as _fs
        from services import config_cache as _cc
        # init_config_cache 가 보관한 runtime config 에서 CimsRuntimeDir 추출
        runtime_config = getattr(_cc.CONFIG_CACHE, "_runtime_config", {}) if _cc.CONFIG_CACHE else {}
        domain_path = _fs.domain_dir(runtime_config or {}, "csp_config_audit")
        record = {
            "ts": _dt.now().isoformat(timespec='seconds'),
            "actor": actor, "actor_ip": actor_ip,
            "entity": entity, "entity_id": str(entity_id),
            "action": action,
            "before": before, "after": after,
            "etag_before": etag_before, "etag_after": etag_after,
            "reason": (reason or "")[:512] if reason else None,
        }
        _fs.jsonl_append(domain_path, "audit", record)
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
def create_tokens(subject, scope, client_id="mcptt_client", nonce=None, refresh_scope=None, mcptt_id=None):
    now = int(time.time())
    # sub = CIMS 로그인 ID(인증 신원). mcptt_id = 규격 MCPTT 서비스 신원(분리). 미지정 시 subject 로 폴백.
    sub = subject
    mcptt = mcptt_id or subject

    # ID Token (OIDC) — nonce 가 있으면 반영(S2b: CSRF/replay 방지, OIDC Core §3.1.2.1)
    id_token_payload = {
        "mcptt_id": mcptt,
        "iss": IDMS_ISSUER,
        "sub": sub,
        "aud": client_id or "mcptt_client",
        "exp": now + ACCESS_TOKEN_TTL,
        "iat": now
    }
    if nonce:
        id_token_payload["nonce"] = nonce
    id_token = jwt.encode(id_token_payload, SECRET_KEY, algorithm="HS256")

    # Access Token — S2a: OIDC 표준 클레임(sub/iss/iat) 보강. sub=login_id, mcptt_id=MCPTT 신원.
    access_token_payload = {
        "mcptt_id": mcptt,
        "iss": IDMS_ISSUER,
        "sub": sub,
        "aud": "mcptt_client",
        "iat": now,
        "exp": now + ACCESS_TOKEN_TTL,
        "scope": scope.split() if scope else []
    }
    access_token = jwt.encode(access_token_payload, SECRET_KEY, algorithm="HS256")

    # Refresh Token (UUID + 영속성 저장) — subject/mcptt_id 보존(refresh 재발급 시 동일 신원).
    refresh_token = str(uuid.uuid4())
    refresh_data = {
        "user_id": subject,
        "mcptt_id": mcptt,
        "client_id": client_id,
        "scope": refresh_scope if refresh_scope is not None else scope,
        "issued_at": now,
        "expires_at": now + REFRESH_TOKEN_TTL,
        "revoked": False,
        "rotated_to": None
    }
    storage.save_refresh_token(refresh_token, refresh_data)
    
    return id_token, access_token, refresh_token

def validate_access_token(token):
    try:
        payload = jwt.decode(token, SECRET_KEY, algorithms=["HS256"], options={"verify_signature": True}, audience="mcptt_client")
        return payload
    except Exception as e:
        logger.log_error(f"Token validation error: {e}")
        return None

# --- XML Generators ---
def _content_etag(content: str) -> str:
    """문서 내용 파생 ETag (RFC 7232). 구 정적 ETag(etag_{gid}/svcfg_etag_v1 등)는 문서가
    바뀌어도 동일 → If-None-Match 가 304 를 반환해 클라이언트가 stale 캐시를 받던 결함을 해소.
    내용이 바뀌면 ETag 가 바뀌어 정상적으로 새 문서를 받는다."""
    return '"' + hashlib.sha256((content or '').encode('utf-8')).hexdigest()[:16] + '"'


def _norm_mcptt_uri(u: str) -> str:
    """MCPTT URI 정규화(비교용) — scheme(sip:/tel:) 제거 + 소문자."""
    s = (u or '').strip().lower()
    for p in ('sip:', 'tel:'):
        if s.startswith(p):
            return s[len(p):]
    return s


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


def get_group_xml(group_uri):
    group = GROUPS.get(group_uri)
    if not group:
        return None, None

    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<group xmlns="urn:oma:xml:poc:list-service"
  xmlns:rl="urn:ietf:params:xml:ns:resource-lists"
  xmlns:cp="urn:ietf:params:xml:ns:common-policy"
  xmlns:ocp="urn:oma:xml:xdm:common-policy"
  xmlns:oxe="urn:oma:xml:xdm:extensions"
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0"
  xmlns:cims="urn:cims:groupinfo:1.0">
  <list-service uri="{group_uri}">
    <display-name xml:lang="en-us">{group['display_name']}</display-name>
    <list>"""

    for member in group['members']:
        xml += f"""
      <entry uri="{member['uri']}">
        <rl:display-name>{member['name']}</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>{member.get('role', 'participant')}</mcpttgi:participant-type>
        <mcpttgi:user-priority>{member.get('priority', 5)}</mcpttgi:user-priority>"""
        # 직함 — 3GPP 미정의 필드라 CIMS 전용 네임스페이스 확장으로 전달
        # (<entry> 는 ##other lax 확장 허용, 표준 단말은 무시 — TS 24.481 정합)
        if member.get('title'):
            xml += f"""
        <cims:user-title>{member['title']}</cims:user-title>"""
        xml += """
      </entry>"""

    video_val = 'true' if group.get('video_enabled') else 'false'
    grp_priority = group.get('priority', 5)
    encryption_val = 'true' if group.get('encryption') else 'false'
    emergency_val = 'true' if group.get('emergency_call') else 'false'
    imminent_val = 'true' if group.get('imminent_peril_call', True) else 'false'
    alert_val = 'true' if group.get('emergency_alert', True) else 'false'
    org_code = group.get('org_code', '')
    group_type = group.get('group_type', 'prearranged')
    # max_members 0(무제한) 이면 관례값 10 노출
    max_count = group.get('max_members') or 10
    affil_required = 'true' if group.get('require_affiliation', True) else 'false'
    xml += f"""
    </list>
    <mcpttgi:session-type>{group_type}</mcpttgi:session-type>
    <mcpttgi:mcptt-video>{video_val}</mcpttgi:mcptt-video>
    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>{max_count}</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-require-affiliation>{affil_required}</mcpttgi:on-network-require-affiliation>
    <mcpttgi:on-network-hang-time>3</mcpttgi:on-network-hang-time>
    <mcpttgi:on-network-max-duration>3600</mcpttgi:on-network-max-duration>
    <mcpttgi:on-network-require-talker-id>false</mcpttgi:on-network-require-talker-id>
    <mcpttgi:on-network-group-priority>{grp_priority}</mcpttgi:on-network-group-priority>
    <mcpttgi:on-network-encryption>{encryption_val}</mcpttgi:on-network-encryption>
    <cp:ruleset>
      <cp:rule id="a7c">
         <cp:actions>
          <mcpttgi:allow-MCPTT-emergency-call>{emergency_val}</mcpttgi:allow-MCPTT-emergency-call>
          <mcpttgi:allow-imminent-peril-call>{imminent_val}</mcpttgi:allow-imminent-peril-call>
          <mcpttgi:allow-MCPTT-emergency-alert>{alert_val}</mcpttgi:allow-MCPTT-emergency-alert>
        </cp:actions>
      </cp:rule>
    </cp:ruleset>
    <oxe:supported-services>
     <oxe:service enabler="example.mcptt">
      <oxe:group-media>
       <mcpttgi:mcptt-speech/>
      </oxe:group-media>
     </oxe:service>
    </oxe:supported-services>"""
    if org_code:
        xml += f"""
    <mcpttgi:org-code>{org_code}</mcpttgi:org-code>"""
    # 그룹 소유 (3GPP TS 23.280 authorized user = 관리주체)
    authorized_user = group.get('authorized_user', '')
    if authorized_user:
        xml += f"""
    <mcpttgi:authorized-user>{authorized_user}</mcpttgi:authorized-user>"""
    xml += """
  </list-service>
</group>"""
    return xml, _content_etag(xml)

def get_user_profile_xml(user_uri):
    user = USERS.get(user_uri)
    if not user:
        return None, None

    display_name = user.get('name', user_uri)
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<mcptt-user-profile xmlns="urn:3gpp:ns:mcpttUserProfile:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  user-profile-index="1">
  <Name>
    <display-name xml:lang="en">{display_name}</display-name>
  </Name>
  <Common>
    <MCPTTUserID>{user_uri}</MCPTTUserID>
    <PrivateCall>
      <MaxSimultaneousCallsN6>1</MaxSimultaneousCallsN6>
      <MaxCallsN7>1</MaxCallsN7>
      <EmergencyCall>
        <MCPTTUserID>{user_uri}</MCPTTUserID>
      </EmergencyCall>
    </PrivateCall>
    <EmergencyAlert>
      <MCPTTUserID>{user_uri}</MCPTTUserID>
    </EmergencyAlert>
  </Common>
  <OnNetwork>
    <MCPTTUserID>{user_uri}</MCPTTUserID>
  </OnNetwork>
</mcptt-user-profile>"""
    return xml, _content_etag(xml)

def get_service_config_xml(user_uri):
    # S4: 동적 생성 — SERVICE_CONFIG_DEFAULTS 에 가입자별 오버라이드(USERS[uri]['service_config'])를
    #   덮어써 사용자/시스템별 정책을 반영한다. ETag 는 내용 파생이라 값이 바뀌면 자동 갱신.
    cfg = dict(SERVICE_CONFIG_DEFAULTS)
    user = USERS.get(user_uri) or {}
    over = user.get('service_config') if isinstance(user.get('service_config'), dict) else None
    if over:
        cfg.update(over)

    def _b(k):  # bool → "true"/"false"
        return "true" if cfg.get(k) else "false"

    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<mcptt-service-config xmlns="urn:3gpp:ns:mcpttServiceConfig:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <num-levels-group-hierarchy>{int(cfg.get('num-levels-group-hierarchy', 3))}</num-levels-group-hierarchy>
  <num-levels-user-hierarchy>{int(cfg.get('num-levels-user-hierarchy', 3))}</num-levels-user-hierarchy>
  <max-affiliations-N2>{int(cfg.get('max-affiliations-N2', 10))}</max-affiliations-N2>
  <allow-create-delete-group>{_b('allow-create-delete-group')}</allow-create-delete-group>
  <allow-private-call>{_b('allow-private-call')}</allow-private-call>
  <allow-emergency-call>{_b('allow-emergency-call')}</allow-emergency-call>
  <allow-alert>{_b('allow-alert')}</allow-alert>
  <on-network>
    <allow-transmit-request>{_b('allow-transmit-request')}</allow-transmit-request>
    <max-on-network-affiliations-N2>{int(cfg.get('max-on-network-affiliations-N2', 10))}</max-on-network-affiliations-N2>
  </on-network>
</mcptt-service-config>"""
    return xml, _content_etag(xml)

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

# IdMS: Auth Req
async def handle_auth_req(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # GET /idms/authreq?client_id=...
    params = args.query_params
    user_name = params.get('user_name')
    # (로깅은 pi_http post_hook 에서 자동 처리)
    user_password = params.get('user_password')
    client_id = params.get('client_id', 'MCPTT_UE')
    redirect_uri = params.get('redirect_uri')
    state = params.get('state', '')
    scope = params.get('scope', '')
    nonce = params.get('nonce', '')   # S2b: OIDC nonce (id_token 에 반영)

    # PKCE 파라미터 (필수)
    code_challenge = params.get('code_challenge')
    code_challenge_method = params.get('code_challenge_method', 'S256')
    
    # PKCE 필수 검증
    if not code_challenge:
        logger.log_error("[IdMS] Auth Req: code_challenge is required")
        return HandlerResult(
            status=400, 
            body={
                "error": "invalid_request", 
                "error_description": "code_challenge is required (PKCE mandatory)"
            }, 
            media_type="application/json"
        )
    
    # code_challenge_method 검증
    if code_challenge_method != 'S256':
        logger.log_error(f"[IdMS] Auth Req: unsupported code_challenge_method: {code_challenge_method}")
        return HandlerResult(
            status=400, 
            body={
                "error": "invalid_request", 
                "error_description": "only S256 is supported for code_challenge_method"
            }, 
            media_type="application/json"
        )
    
    logger.log_info(f"[IdMS] Auth Req: user={user_name}, client={client_id}, pkce=S256")

    # 사용자 인증 — CIMS 로그인 ID(login_id) 우선. 토큰 sub=login_id, mcptt_id=규격 MCPTT ID(분리).
    #   (DB 미연결 등으로 LOGIN_ACCOUNTS 가 비면 legacy: USERS(tel:+msisdn) 직접 로그인 호환.)
    acct = LOGIN_ACCOUNTS.get(user_name)
    expected_pw = None
    mcptt_id = user_name
    if acct is not None:
        expected_pw = acct.get("password")
        mcptt_id = acct.get("mcptt_id") or user_name
    elif user_name in USERS:
        expected_pw = USERS[user_name].get("password")
        mcptt_id = user_name
    if expected_pw is None:
        logger.log_error(f"[IdMS] Auth Req Failed: login_id {user_name} not found")
        return HandlerResult(status=401,
            body={"error": "access_denied", "error_description": "사용자를 찾을 수 없습니다"},
            media_type="application/json")
    if expected_pw != user_password:
        logger.log_error(f"[IdMS] Auth Req Failed: Password mismatch for {user_name}")
        return HandlerResult(status=401,
            body={"error": "access_denied", "error_description": "비밀번호가 올바르지 않습니다"},
            media_type="application/json")

    # auth-code 생성
    code = str(uuid.uuid4())
    now = int(time.time())

    # auth-code 데이터 — login_id(sub) 와 mcptt_id(서비스 신원) 분리 보관.
    auth_data = {
        "login_id": user_name,
        "mcptt_id": mcptt_id,
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": scope,
        "state": state,
        "issued_at": now,
        "expires_at": now + AUTH_CODE_TTL,
        "used": False,
        "nonce": nonce   # S2b: token 발급 시 id_token 에 반영
    }

    # PKCE 저장 (있으면)
    if code_challenge:
        auth_data["code_challenge"] = code_challenge
        auth_data["code_challenge_method"] = code_challenge_method
    
    # 영속성 저장
    storage.save_auth_code(code, auth_data)
    
    # 응답
    response_data = {
        "Location": redirect_uri,
        "code": code,
        "state": state
    }
    return HandlerResult(status=200, body=response_data, media_type="application/json")

# IdMS: Token Req
async def handle_token_req(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # POST /idms/tokenreq
    # (로깅은 pi_http post_hook 에서 자동 처리)
    data = args.body
    if not isinstance(data, dict):
        return HandlerResult(status=400, body={"error": "invalid_request"}, media_type="application/json")

    grant_type = data.get('grant_type')
    logger.log_info(f"[IdMS] Token Req: grant_type={grant_type}")
    
    # ==================== authorization_code ====================
    if grant_type == 'authorization_code':
        code = data.get('code')
        code_verifier = data.get('code_verifier')
        client_id = data.get('client_id', 'MCPTT_UE')
        redirect_uri = data.get('redirect_uri')
        
        if not code:
            return HandlerResult(status=400, body={"error": "invalid_request"}, media_type="application/json")
        
        # 1. auth-code 조회
        auth_data = storage.get_auth_code(code)
        if not auth_data:
            logger.log_error(f"Auth code not found: {code}")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 2. 만료 체크
        now = int(time.time())
        if now > auth_data.get("expires_at", 0):
            logger.log_error(f"Auth code expired: {code}")
            storage.delete_auth_code(code)
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 3. 1회성 체크
        if auth_data.get("used", False):
            logger.log_error(f"Auth code already used: {code}")
            storage.delete_auth_code(code)
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 4. client_id 일치 확인
        if auth_data.get("client_id") != client_id:
            logger.log_error(f"Client ID mismatch: {client_id} != {auth_data.get('client_id')}")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 5. redirect_uri 일치 확인 (있으면)
        if redirect_uri and auth_data.get("redirect_uri") != redirect_uri:
            logger.log_error(f"Redirect URI mismatch")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 6. PKCE 검증 (필수)
        if "code_challenge" not in auth_data:
            logger.log_error("PKCE: code_challenge not found in auth_data")
            return HandlerResult(status=400, body={"error": "invalid_grant", "error_description": "PKCE required"}, media_type="application/json")
        
        if not code_verifier:
            logger.log_error("PKCE: code_verifier missing")
            return HandlerResult(status=400, body={"error": "invalid_grant", "error_description": "code_verifier required"}, media_type="application/json")
        
        if not verify_pkce(code_verifier, auth_data["code_challenge"], auth_data.get("code_challenge_method", "S256")):
            logger.log_error("PKCE: verification failed")
            return HandlerResult(status=400, body={"error": "invalid_grant", "error_description": "PKCE verification failed"}, media_type="application/json")
            
            logger.log_info("PKCE: verification success")
        
        # 7. 성공 - 토큰 발급 (sub=login_id, mcptt_id=서비스 신원 분리. nonce 반영)
        login_id = auth_data.get("login_id") or auth_data.get("user_id")
        mcptt_id = auth_data.get("mcptt_id", login_id)
        scope = auth_data.get("scope", "")
        nonce = auth_data.get("nonce", "")

        id_token, access_token, refresh_token = create_tokens(
            login_id, scope, client_id, nonce=nonce, mcptt_id=mcptt_id)
        
        # auth-code 삭제 (1회성)
        storage.delete_auth_code(code)
        
        logger.log_info(f"Token issued for login_id={login_id} mcptt_id={mcptt_id}")
        return HandlerResult(status=200, body={
            "access_token": access_token,
            "refresh_token": refresh_token,
            "id_token": id_token,
            "token_type": "Bearer",
            "expires_in": 3600
        }, media_type="application/json")
    
    # ==================== refresh_token ====================
    elif grant_type == 'refresh_token':
        refresh_token = data.get('refresh_token')
        client_id = data.get('client_id', 'MCPTT_UE')
        
        if not refresh_token:
            return HandlerResult(status=400, body={"error": "invalid_request"}, media_type="application/json")
        
        # 1. refresh_token 조회
        token_data = storage.get_refresh_token(refresh_token)
        if not token_data:
            logger.log_error(f"Refresh token not found")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 2. revoked/만료 확인
        now = int(time.time())
        if token_data.get("revoked", False):
            logger.log_error(f"Refresh token revoked")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        if now > token_data.get("expires_at", 0):
            logger.log_error(f"Refresh token expired")
            storage.revoke_refresh_token(refresh_token)
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 3. client_id 일치 확인
        if token_data.get("client_id") != client_id:
            logger.log_error(f"Client ID mismatch for refresh token")
            return HandlerResult(status=400, body={"error": "invalid_grant"}, media_type="application/json")
        
        # 4. refresh token rotation
        login_id = token_data["user_id"]                 # = subject(login_id)
        mcptt_id = token_data.get("mcptt_id", login_id)  # 규격 MCPTT 신원 보존
        granted_scope = token_data.get("scope", "") or ""

        # scope 분리: refresh 요청이 scope 를 명시하면 원 grant 의 subset 으로 좁혀 발급한다.
        #   (AccountManager 가 authTokenType 별로 provisioning / mcptt 토큰을 따로 받기 위함.)
        requested_scope = (data.get('scope') or "").strip()
        if requested_scope:
            granted_set = set(granted_scope.split())
            req = [s for s in requested_scope.split() if s in granted_set]
            scope = " ".join(req) if req else granted_scope   # 교집합 없으면 원 scope 유지
        else:
            scope = granted_scope

        # 새 토큰 발급 — access 는 좁힌 scope, refresh 는 원 grant(broad) 보존(다음 다른-용도 refresh 가능).
        id_token, access_token, new_refresh_token = create_tokens(
            login_id, scope, client_id, refresh_scope=granted_scope, mcptt_id=mcptt_id)
        
        # 기존 토큰 회수
        storage.revoke_refresh_token(refresh_token, rotated_to=new_refresh_token)
        
        logger.log_info(f"Refresh token rotated for user: {login_id}")
        return HandlerResult(status=200, body={
            "access_token": access_token,
            "refresh_token": new_refresh_token,
            "id_token": id_token,
            "token_type": "Bearer",
            "expires_in": 3600
        }, media_type="application/json")
    
    return HandlerResult(status=400, body={"error": "unsupported_grant_type"}, media_type="application/json")

# GMS: List groups for a user
# GET /org.openmobilealliance.groups/users/{user_uri}
async def handle_user_groups(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")

    from urllib.parse import unquote
    path = args.full_path
    parts = [p for p in path.split('/') if p]
    # parts: ['org.openmobilealliance.groups', 'users', 'user_uri']
    user_uri = unquote(parts[-1]) if len(parts) >= 3 else token_payload.get('mcptt_id', '')

    logger.log_info(f"[GMS] List groups for user: {user_uri}")

    result = []
    for group_uri, group in GROUPS.items():
        for member in group.get('members', []):
            if member['uri'] == user_uri:
                result.append({
                    "uri": group_uri,
                    "display_name": group['display_name'],
                    "etag": group['etag'],
                    "member_count": len(group['members']),
                })
                break

    import json as _json
    return HandlerResult(status=200, body=_json.dumps(result), media_type='application/json')


# GMS: unified handler — dispatches on path depth
# GET  /org.openmobilealliance.groups/users/{user_uri}              → list user's groups (JSON)
# GET/PUT/DELETE /org.openmobilealliance.groups/users/{user_uri}/{group_uri} → specific group
async def handle_group_management(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")

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
            if grp and not _is_group_member(grp, requester):
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
            display_name = f"Group {group_uri}"
            now_str = datetime.datetime.now().isoformat()

            GROUPS[group_uri] = {
                "display_name": display_name,
                "etag": f"etag_{int(time.time())}",
                "created_by": user_uri,
                "created_at": now_str,
                "members": [
                    {"uri": user_uri, "name": "Owner", "role": "owner", "priority": 5, "joined_at": now_str}
                ]
            }
            save_group_to_file(group_uri, GROUPS[group_uri])
            notify_csp("GROUP_CHANGED", group_uri, "PUT", GROUPS[group_uri].get('etag', ''))

            xml, etag = get_group_xml(group_uri)
            return HandlerResult(status=200, body=xml, media_type='application/vnd.oma.poc.groups+xml', headers={'Etag': etag})

        elif args.method == 'DELETE':
            if group_uri in GROUPS:
                del GROUPS[group_uri]
                # Persist deletion
                delete_group_file(group_uri)
                
                # [FIX] Notify CSP
                notify_csp("GROUP_CHANGED", group_uri, "DELETE", "")

                return HandlerResult(status=200)
            else:
                return HandlerResult(status=404)

        return HandlerResult(status=405)
    except Exception as e:
        import traceback
        traceback.print_exc()
        logger.log_error(f"GMS Error: {e}")
        return HandlerResult(status=500, body=str(e))

# CMS: User Profile
async def handle_user_profile(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    # GET /org.3gpp.mcptt.user-profile/users/{user_id}/user-profile
    # (로깅은 pi_http post_hook 에서 자동 처리)
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")
        
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
    xml, etag = get_user_profile_xml(user_uri)

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
        return HandlerResult(status=401, body="Missing or Invalid Token")
        
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

# KMS: Init & KeyProv
async def handle_kms_init(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")
        
    user_uri = token_payload.get('mcptt_id')
    logger.log_info(f"[KMS] Init: {user_uri}")
    
    xml = get_kms_init_xml(user_uri)
    return HandlerResult(status=200, body=xml, media_type='application/xml')

async def handle_kms_keyprov(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token_payload = extract_token(args.headers.get('authorization'))
    if not token_payload:
        return HandlerResult(status=401, body="Missing or Invalid Token")
        
    user_uri = token_payload.get('mcptt_id')
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
        scope = payload.get("scope", [])
        return HandlerResult(status=200, body={
            "active": True,
            "mcptt_id": payload.get("mcptt_id"),
            "aud": payload.get("aud"),
            "exp": payload.get("exp"),
            "iat": payload.get("iat"),
            "scope": " ".join(scope) if isinstance(scope, list) else scope
        }, media_type="application/json")
    else:
        return HandlerResult(status=200, body={"active": False}, media_type="application/json")


# S1: OIDC Discovery — GET /.well-known/openid-configuration (TS 33.180 / OIDC Discovery 1.0)
#   단말이 엔드포인트를 하드코딩하지 않고 동적 발견. base URL 은 요청 Host 헤더에서 유도.
async def handle_openid_config(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    host = (args.headers.get('host') or args.headers.get('Host') or f"{IDMS_DOMAIN}").strip()
    base = f"https://{host}"
    doc = {
        "issuer": IDMS_ISSUER,
        "authorization_endpoint": f"{base}/idms/authreq",
        "token_endpoint": f"{base}/idms/tokenreq",
        "introspection_endpoint": f"{base}/idms/introspect",
        "token_endpoint_auth_methods_supported": ["none"],
        "grant_types_supported": ["authorization_code", "refresh_token"],
        "response_types_supported": ["code"],
        "code_challenge_methods_supported": ["S256"],
        "scopes_supported": ["openid", "3gpp:mcptt:ptt_server"],
        "subject_types_supported": ["public"],
        "id_token_signing_alg_values_supported": ["HS256"],
        "claims_supported": ["sub", "iss", "iat", "exp", "aud", "mcptt_id", "nonce", "scope"],
    }
    return HandlerResult(status=200, body=doc, media_type="application/json")


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

def _provision_service(kind: str, sid: str, imsi: str, auth_id: str, host_ip: str,
                       sip_password: str = "") -> dict:
    svc = (PROVISIONING.get('Services') or {}).get(kind, {}) if isinstance(PROVISIONING, dict) else {}
    account = {
        "msisdn": sid,
        "imsi": imsi or "",
        "authId": auth_id or "",        # 빈값이면 단말이 imsi@domain 합성
        # SIP Digest 비번 = 서비스 가입(subscription) 비번. CIMS 로그인(IdMS) 비번과 별개 —
        # CSP 는 이 비번으로 REGISTER 를 검증한다. 비어 있으면 단말이 로그인 비번으로 폴백.
        "sipPassword": sip_password or None,
    }
    if kind == "ptt":
        account["mcpttId"] = sid if sid.startswith(("tel:", "sip:")) else f"tel:{sid}"
    return {
        "kind": kind,
        "sip": {
            "host": svc.get('host') or host_ip,     # 빈값 → 요청 Host(올인원). 다중노드면 CSP/PSP VIP.
            "port": int(svc.get('port', 5060)),
            "transport": svc.get('transport', 'UDP'),
            "domain": svc.get('domain') or IDMS_DOMAIN,
        },
        "account": account,
    }

async def handle_provisioning_me(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    token = extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return HandlerResult(status=401, body={"error": "invalid_token"}, media_type="application/json")
    # scope 분리: provisioning 토큰만 허용(빈 scope=레거시 허용). mcptt 전용 토큰은 거부 → 평면 혼용 방지.
    _sc = token.get('scope') or []
    if isinstance(_sc, str):
        _sc = _sc.split()
    if _sc and SCOPE_PROVISIONING not in _sc:
        return HandlerResult(status=403,
                             body={"error": "insufficient_scope", "required": SCOPE_PROVISIONING},
                             media_type="application/json")
    msisdn = _msisdn_from_id(token.get('mcptt_id') or token.get('sub') or '')
    host_ip = (args.headers.get('host') or args.headers.get('Host') or '').split(':')[0]
    if not _DB_CONFIG:
        return HandlerResult(status=503, body={"error": "db_unavailable"}, media_type="application/json")

    services: list = []
    display_name = None
    try:
        import pymysql
        conn = pymysql.connect(host=_DB_CONFIG.get('Host', '127.0.0.1'),
                               port=int(_DB_CONFIG.get('Port', 3306)),
                               user=_DB_CONFIG.get('User', 'root'),
                               password=_DB_CONFIG.get('Password', ''),
                               database=_DB_CONFIG.get('Db', 'cims'), connect_timeout=5)
        try:
            cur = conn.cursor()
            # 로그인 msisdn 으로 person(user_id) 확인 → 그 person 의 volte+ptt 전 서비스 반환.
            user_id = None
            for t in ('volte_subscriptions', 'ptt_subscriptions'):
                cur.execute(f"SELECT user_id FROM {t} WHERE id=%s", (msisdn,))
                r = cur.fetchone()
                if r:
                    user_id = r[0]
                    break
            if user_id is not None:
                for t, kind in (('volte_subscriptions', 'volte'), ('ptt_subscriptions', 'ptt')):
                    cur.execute(f"SELECT id, imsi, auth_id, passwd FROM {t} WHERE user_id=%s ORDER BY id", (user_id,))
                    for sid, imsi, auth_id, passwd in cur.fetchall():
                        services.append(_provision_service(kind, sid, imsi or '', auth_id or '', host_ip, passwd or ''))
                cur.execute("SELECT name FROM users WHERE id=%s", (user_id,))
                rr = cur.fetchone()
                display_name = rr[0] if rr else None
        finally:
            conn.close()
    except Exception as e:
        logger.log_error(f"[provisioning/me] DB error: {e}")
        return HandlerResult(status=503, body={"error": "db_error", "detail": str(e)}, media_type="application/json")

    # 홈 국가코드(digits, 예 '82') — 단말 번호 로컬 표기(+82… → 0…)의 SoT.
    # 설정 Provisioning.CountryCode 우선, 없으면 로그인 msisdn 에서 유도.
    country = ''
    if isinstance(PROVISIONING, dict):
        country = str(PROVISIONING.get('CountryCode') or '').lstrip('+').strip()
    if not country:
        country = _country_code_of(msisdn)
    body = {
        "user": {"displayName": display_name, "loginId": token.get('sub') or msisdn},
        "csc": {"host": host_ip, "port": _MCPTT_PORT},
        "countryCode": country,     # 판정 불가 시 "" (null 금지 — Android org.json 이 "null" 문자열화)
        "services": services,
    }
    logger.log_info(f"[provisioning/me] msisdn={msisdn} user_id services={[s['kind'] for s in services]} cc={country}")
    return HandlerResult(status=200, body=body, media_type="application/json")


async def handle_provisioning_directory(args: HandlerArgs, kwargs: dict) -> HandlerResult:
    """회사 전화번호부 — 조직 트리 + VoLTE 가입자. 단말 '회사 연락처'(읽기전용) 소스.

    provisioning scope 토큰 필요. 조직(organizations) 계층(parent_id)과 전 VoLTE 가입자를 반환한다.
    `orgs[]` = 조직 트리(code/name/parent code/sort), `entries[]` = 가입자(org=조직 code).
    users.org_id 는 조직 코드(organizations.code)를 담는다.
    """
    token = extract_token(args.headers.get('authorization') or args.headers.get('Authorization'))
    if not token:
        return HandlerResult(status=401, body={"error": "invalid_token"}, media_type="application/json")
    _sc = token.get('scope') or []
    if isinstance(_sc, str):
        _sc = _sc.split()
    if _sc and SCOPE_PROVISIONING not in _sc:
        return HandlerResult(status=403,
                             body={"error": "insufficient_scope", "required": SCOPE_PROVISIONING},
                             media_type="application/json")
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
            # 가입자 — org = users.org_id(조직 code)
            cur.execute(
                "SELECT u.org_id AS org, u.name AS name, v.id AS msisdn "
                "FROM volte_subscriptions v JOIN users u ON u.id = v.user_id "
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
    # IdMS (3GPP TS 33.180 / OAuth 2.0 PKCE)
    ("/idms/authreq",     handle_auth_req,          {}),
    ("/idms/tokenreq",    handle_token_req,          {}),
    ("/idms/introspect",  handle_token_introspect,   {}),
    # GMS — list: GET /users/{user_uri}  |  CRUD: /users/{user_uri}/{group_uri}
    ("/org.openmobilealliance.groups/users", handle_group_management, {}),
    # CMS (3GPP TS 24.484)
    ("/org.3gpp.mcptt.user-profile/users",   handle_user_profile,   {}),
    ("/org.3gpp.mcptt.service-config/users", handle_service_config,  {}),
    # KMS (3GPP TS 33.180 / MIKEY-SAKKE)
    ("/keymanagement/identity/v1/init",    handle_kms_init,    {}),
    ("/keymanagement/identity/v1/keyprov", handle_kms_keyprov, {}),
]
