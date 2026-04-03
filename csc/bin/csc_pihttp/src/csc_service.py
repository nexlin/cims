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
from typing import Dict, Tuple, Optional, List

from util.pi_http.http_handler import HandlerArgs, HandlerResult, BodyData
from util.log_util import Logger
from idms_storage import IdmsStorage

# --- Configuration & Data ---
SECRET_KEY = "mcptt_jwt_secret_change_me"
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

def load_shared_data(config):
    # Do not reassign global variables, modify them in place
    USERS.clear()
    GROUPS.clear()
    
    # Config keys match csc.json structure
    user_path = config.get('Data', {}).get('User')
    group_path = config.get('Data', {}).get('Group')
    db_config = config.get('CimsDatabase')

    # Initialize MariaDB Storage
    if db_config:
        storage.init_db(db_config)
        logger.log_info("MariaDB Initialized for IDMS Storage")
    
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
                    for table in ('voip_subscriptions', 'ptt_subscriptions'):
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

    global CSP_NOTIFY_IP, CSP_NOTIFY_PORT
    notify_cfg = config.get('CspNotify', {})
    if notify_cfg.get('Ip'):
        CSP_NOTIFY_IP = notify_cfg['Ip']
    if notify_cfg.get('Port'):
        CSP_NOTIFY_PORT = int(notify_cfg['Port'])

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
                    cur.execute("SELECT id, name, video_enabled FROM ptt_groups")
                    for row in cur.fetchall():
                        gid = row['id']
                        uri = f"tel:{gid}" if gid.startswith('+') else f"tel:+{gid}"
                        GROUPS[uri] = {
                            "display_name": row['name'],
                            "video_enabled": bool(row.get('video_enabled', 0)),
                            "etag": f"etag_{gid}",
                            "created_by": "", "created_at": "",
                            "members": []
                        }
                    # 멤버 목록 + users 테이블에서 이름 조회
                    cur.execute(
                        "SELECT gm.group_id, gm.user_id, gm.priority, "
                        "       ps.auth_id, u.name AS user_name "
                        "FROM ptt_group_members gm "
                        "LEFT JOIN ptt_subscriptions ps ON ps.id = gm.user_id "
                        "LEFT JOIN users u ON u.id = ps.user_id "
                        "ORDER BY gm.group_id, gm.priority"
                    )
                    for row in cur.fetchall():
                        gid = row['group_id']
                        g_uri = f"tel:{gid}" if gid.startswith('+') else f"tel:+{gid}"
                        uid = row['user_id']
                        m_uri = f"tel:{uid}" if uid.startswith('+') else f"tel:+{uid}"
                        m_name = row.get('user_name') or m_uri
                        if g_uri in GROUPS:
                            GROUPS[g_uri]['members'].append({
                                "uri": m_uri, "name": m_name,
                                "role": "participant", "priority": row['priority'], "joined_at": ""
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
def notify_csp(event_type, uri, action, etag=""):
    try:
        data = {
            "event": event_type,
            "uri": uri,
            "action": action,
            "etag": etag
        }
        msg = json.dumps(data)
        
        # Connect to CSP (Localhost 4421)
        # Timeout to avoid blocking
        # Connect to CSP (Localhost 4421) - UDP
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(msg.encode('utf-8'), (CSP_NOTIFY_IP, CSP_NOTIFY_PORT))
        sock.close()
        logger.log_info(f"Notify Sent: {msg}")
    except Exception as e:
        logger.log_error(f"Notify Failed: {e}")

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
        <mcpttgi:user-priority>{member.get('priority', 5)}</mcpttgi:user-priority>
      </entry>"""
      
    video_val = 'true' if group.get('video_enabled') else 'false'
    xml += f"""
    </list>
    <mcpttgi:mcptt-video>{video_val}</mcpttgi:mcptt-video>
    <mcpttgi:on-network-invite-members>true</mcpttgi:on-network-invite-members>
    <mcpttgi:on-network-max-participant-count>10</mcpttgi:on-network-max-participant-count>
    <mcpttgi:on-network-hang-time>3</mcpttgi:on-network-hang-time>
    <mcpttgi:on-network-max-duration>3600</mcpttgi:on-network-max-duration>
    <mcpttgi:on-network-require-talker-id>false</mcpttgi:on-network-require-talker-id>
    <cp:ruleset>
      <cp:rule id="a7c">
         <cp:actions>
          <mcpttgi:allow-MCPTT-emergency-call>true</mcpttgi:allow-MCPTT-emergency-call>
          <mcpttgi:allow-imminent-peril-call>true</mcpttgi:allow-imminent-peril-call>
          <mcpttgi:allow-MCPTT-emergency-alert>true</mcpttgi:allow-MCPTT-emergency-alert>
        </cp:actions>
      </cp:rule>
    </cp:ruleset>
    <oxe:supported-services>
     <oxe:service enabler="example.mcptt">
      <oxe:group-media>
       <mcpttgi:mcptt-speech/>
      </oxe:group-media>
     </oxe:service>
    </oxe:supported-services>
    <mcpttgi:on-network-group-priority>5</mcpttgi:on-network-group-priority>
  </list-service>
</group>"""
    return xml, group['etag']

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
    return xml, user['profile_etag']

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
    return xml, "svcfg_etag_v1"

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
            notify_csp("group_change", group_uri, "PUT", GROUPS[group_uri].get('etag', ''))

            xml, etag = get_group_xml(group_uri)
            return HandlerResult(status=200, body=xml, media_type='application/vnd.oma.poc.groups+xml', headers={'Etag': etag})

        elif args.method == 'DELETE':
            if group_uri in GROUPS:
                del GROUPS[group_uri]
                # Persist deletion
                delete_group_file(group_uri)
                
                # [FIX] Notify CSP
                notify_csp("group_change", group_uri, "DELETE", "")

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
