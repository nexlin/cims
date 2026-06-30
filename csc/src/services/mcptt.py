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
IDMS_ISSUER = "idms.mcptt.com"
KMS_URI = "kms.mcptt.com"
IDMS_DOMAIN = "mcptt.com"
KMS_CLIENT_REQ_URL = "http://localhost:4420/keymanagement/identity/v1/init"
USERS = {}
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

CSP_NOTIFY_IP = "127.0.0.1"
CSP_NOTIFY_PORT = 4421
# PSP (PTT 시그널링) — 별도 인스턴스 분리 시 사용. CSP 와 동일하면 broadcast 가
# 동일 endpoint 1번만 호출 (자동 dedup).
PSP_NOTIFY_IP = ""           # ""=PSP 미설정 (legacy: CSP 만 사용)
PSP_NOTIFY_PORT = 4421


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
            db_users_loaded = True
            logger.log_info(f"Users loaded from DB: {len(USERS)}")
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
                    # 멤버 목록 + users 테이블에서 이름 조회 (group_id=surrogate → mcptt_group_id JOIN)
                    cur.execute(
                        "SELECT g.mcptt_group_id AS mcptt_group_id, gm.user_id, gm.priority, gm.role, gm.mcptt_id, "
                        "       u.name AS user_name "
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
                                "priority": row['priority'], "joined_at": ""
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
                                "priority": m.get('priority', 5), "joined_at": ""
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
def create_tokens(user_id, scope, client_id="mcptt_client"):
    now = int(time.time())
    
    # ID Token
    id_token_payload = {
        "mcptt_id": user_id,
        "iss": IDMS_ISSUER,
        "sub": str(uuid.uuid4()),
        "aud": "mcptt_client",
        "exp": now + ACCESS_TOKEN_TTL,
        "iat": now
    }
    id_token = jwt.encode(id_token_payload, SECRET_KEY, algorithm="HS256")
    
    # Access Token
    access_token_payload = {
        "mcptt_id": user_id,
        "aud": "mcptt_client",
        "exp": now + ACCESS_TOKEN_TTL,
        "scope": scope.split() if scope else []
    }
    access_token = jwt.encode(access_token_payload, SECRET_KEY, algorithm="HS256")
    
    # Refresh Token (UUID + 영속성 저장)
    refresh_token = str(uuid.uuid4())
    refresh_data = {
        "user_id": user_id,
        "client_id": client_id,
        "scope": scope,
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
  xmlns:mcpttgi="urn:3gpp:ns:mcpttGroupInfo:1.0">
  <list-service uri="{group_uri}">
    <display-name xml:lang="en-us">{group['display_name']}</display-name>
    <list>"""
    
    for member in group['members']:
        xml += f"""
      <entry uri="{member['uri']}">
        <rl:display-name>{member['name']}</rl:display-name>
        <mcpttgi:on-network-required/>
        <mcpttgi:participant-type>{member.get('role', 'participant')}</mcpttgi:participant-type>
        <mcpttgi:user-priority>{member.get('priority', 5)}</mcpttgi:user-priority>
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
    xml = """<?xml version="1.0" encoding="UTF-8"?>
<mcptt-service-config xmlns="urn:3gpp:ns:mcpttServiceConfig:1.0"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <num-levels-group-hierarchy>3</num-levels-group-hierarchy>
  <num-levels-user-hierarchy>3</num-levels-user-hierarchy>
  <max-affiliations-N2>10</max-affiliations-N2>
  <allow-create-delete-group>true</allow-create-delete-group>
  <allow-private-call>true</allow-private-call>
  <allow-emergency-call>true</allow-emergency-call>
  <allow-alert>true</allow-alert>
  <on-network>
    <allow-transmit-request>true</allow-transmit-request>
    <max-on-network-affiliations-N2>10</max-on-network-affiliations-N2>
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

def get_kms_keyprov_xml(user_uri):
    now = datetime.datetime.now(datetime.timezone.utc)
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
<ValidFrom>2016-11-01T00:00:00+09:00</ValidFrom>
<ValidTo>2036-10-31T23:59:59+09:00</ValidTo>
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

    # 사용자 인증
    if user_name not in USERS:
        logger.log_error(f"[IdMS] Auth Req Failed: user {user_name} not found in USERS keys: {list(USERS.keys())}")
        return HandlerResult(
            status=401,
            body={"error": "access_denied", "error_description": "사용자를 찾을 수 없습니다"},
            media_type="application/json"
        )

    if USERS[user_name]['password'] != user_password:
        logger.log_error(f"[IdMS] Auth Req Failed: Password mismatch for {user_name}")
        return HandlerResult(
            status=401,
            body={"error": "access_denied", "error_description": "비밀번호가 올바르지 않습니다"},
            media_type="application/json"
        )

    # auth-code 생성
    code = str(uuid.uuid4())
    now = int(time.time())
    
    # auth-code 데이터
    auth_data = {
        "user_id": user_name,
        "client_id": client_id,
        "redirect_uri": redirect_uri,
        "scope": scope,
        "state": state,
        "issued_at": now,
        "expires_at": now + AUTH_CODE_TTL,
        "used": False
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
        
        # 7. 성공 - 토큰 발급
        user_id = auth_data["user_id"]
        scope = auth_data.get("scope", "")
        
        id_token, access_token, refresh_token = create_tokens(user_id, scope, client_id)
        
        # auth-code 삭제 (1회성)
        storage.delete_auth_code(code)
        
        logger.log_info(f"Token issued for user: {user_id}")
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
        user_id = token_data["user_id"]
        scope = token_data.get("scope", "")
        
        # 새 토큰 발급
        id_token, access_token, new_refresh_token = create_tokens(user_id, scope, client_id)
        
        # 기존 토큰 회수
        storage.revoke_refresh_token(refresh_token, rotated_to=new_refresh_token)
        
        logger.log_info(f"Refresh token rotated for user: {user_id}")
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


# Route Mapping (MCPTT server — port 4430)
CSC_HANDLER_LIST = [
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
