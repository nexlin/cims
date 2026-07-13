"""
CSC 전체 기능 테스트 스크립트

포함 기능:
- IdMS 인증 (PKCE 필수)
- Refresh Token (자동 갱신)
- GMS (그룹 관리)
- CMS (사용자 프로필, 서비스 설정)
- KMS (키 관리)
"""

import asyncio
import aiohttp
import ssl
import json
import time
import hashlib
import base64
import secrets
import os
from typing import Optional, Dict

# === CONFIGURATION ===
TEST_USER = "tel:+82571900034"
TEST_GROUP = "tel:3000"

# 토큰 캐시 파일
CONFIG_FILE = "data/ptt_token_config.json"

# ==================== 토큰 관리 함수 ====================

def load_token_config() -> Optional[Dict]:
    """토큰 config 로드"""
    if not os.path.exists(CONFIG_FILE):
        return None
    
    try:
        with open(CONFIG_FILE, 'r') as f:
            return json.load(f)
    except:
        return None

def save_token_config(token_data: Dict):
    """토큰 config 저장"""
    config = {
        "access_token": token_data.get("access_token"),
        "refresh_token": token_data.get("refresh_token"),
        "id_token": token_data.get("id_token"),
        "expires_at": int(time.time()) + token_data.get("expires_in", 3600),
        "user_id": "tel:+2001",
        "client_id": "MCPTT_UE",
        "scope": "openid 3gpp:mcptt:ptt_server"
    }
    
    with open(CONFIG_FILE, 'w') as f:
        json.dump(config, f, indent=2)
    
    print(f"✅ Token saved to {CONFIG_FILE}")

def is_token_expired(config: Dict) -> bool:
    """토큰 만료 확인 (시간 기반)"""
    return time.time() >= config.get("expires_at", 0)

# ==================== 인증 함수 ====================

async def full_authentication(session, base_url):
    """전체 인증 (authreq + tokenreq with PKCE)"""
    print("\n" + "=" * 60)
    print("🔐 Full Authentication (PKCE)")
    print("=" * 60)
    
    # PKCE 생성
    code_verifier = base64.urlsafe_b64encode(
        secrets.token_bytes(32)
    ).decode('utf-8').rstrip('=')
    
    code_challenge = base64.urlsafe_b64encode(
        hashlib.sha256(code_verifier.encode('utf-8')).digest()
    ).decode('utf-8').rstrip('=')
    
    print(f"PKCE code_verifier: {code_verifier[:20]}...")
    print(f"PKCE code_challenge: {code_challenge[:20]}...")
    
    # Auth Request
    print("\n--- Auth Request ---")
    async with session.get(f"{base_url}/idms/authreq", params={
        "client_id": "MCPTT_UE",
        "user_name": TEST_USER,
        "user_password": "123456",
        "redirect_uri": "http://client/cb",
        "state": "mystate",
        "scope": "openid 3gpp:mcptt:ptt_server",
        "code_challenge": code_challenge,
        "code_challenge_method": "S256"
    }, allow_redirects=False) as resp:
        print(f"Status: {resp.status}")
        
        if resp.status == 302:
            location = resp.headers['Location']
            import urllib.parse
            parsed = urllib.parse.urlparse(location)
            qs = urllib.parse.parse_qs(parsed.query)
            code = qs.get("code", [None])[0]
        else:
            try:
                data = await resp.json()
                code = data.get("code")
            except:
                print(await resp.text())
                return None
        
        if not code:
            print("❌ Failed to get auth code")
            return None
        
        print(f"Auth Request Success. Code: {code}")
        
        # [Observation] 3초 대기 (auth_code 파일 확인용)
        print("Waiting 3 seconds for observation...")
        await asyncio.sleep(3)
        
        # Token Request
        print("\n--- Token Request ---")
    async with session.post(f"{base_url}/idms/tokenreq", json={
        "grant_type": "authorization_code",
        "code": code,
        "code_verifier": code_verifier,
        "redirect_uri": "http://client/cb",
        "client_id": "MCPTT_UE"
    }) as resp:
        if resp.status == 200:
            token_data = await resp.json()
            print("✅ Authentication successful")
            
            # Config 저장
            save_token_config(token_data)
            
            return load_token_config()
        else:
            print(f"❌ Authentication failed: {resp.status}")
            return None

async def refresh_token_flow(session, base_url, config):
    """Refresh Token으로 갱신"""
    print("\n" + "=" * 60)
    print("🔄 Refreshing Token")
    print("=" * 60)
    
    async with session.post(f"{base_url}/idms/tokenreq", json={
        "grant_type": "refresh_token",
        "refresh_token": config["refresh_token"],
        "client_id": config["client_id"]
    }) as resp:
        if resp.status == 200:
            token_data = await resp.json()
            print("✅ Token refreshed successfully")
            print(f"   Old refresh_token: {config['refresh_token']}")
            print(f"   New refresh_token: {token_data.get('refresh_token')}")
            
            # Config 저장
            save_token_config(token_data)
            
            return load_token_config()
        else:
            print(f"❌ Refresh failed: {resp.status}")
            # 전체 인증으로 폴백
            return await full_authentication(session, base_url)

async def get_valid_token(session, base_url):
    """유효한 토큰 획득 (캐싱 + 자동 갱신)"""
    
    # 1. Config 파일 확인
    config = load_token_config()
    
    if config:
        print("📁 Found cached token")
        
        # 2. 만료 확인 (시간 기반)
        if not is_token_expired(config):
            print("✅ Token is valid (time-based), verifying...")
            
            # 3. 실제 API 호출로 검증
            headers = {"Authorization": f"Bearer {config['access_token']}"}
            try:
                print(f"\n[CMS] GET User Profile for {TEST_USER}...")
                async with session.get(
                    f"{base_url}/org.3gpp.mcptt.user-profile/users/{TEST_USER}/user-profile",
                    headers=headers
                ) as resp:
                    if resp.status == 200:
                        print("✅ Token verified successfully")
                        return config
                    elif resp.status == 401:
                        print("⚠️  Token expired (401), refreshing...")
                        return await refresh_token_flow(session, base_url, config)
            except Exception as e:
                print(f"⚠️  Token validation failed: {e}, re-authenticating...")
        else:
            print("⚠️  Token expired (time-based), refreshing...")
            return await refresh_token_flow(session, base_url, config)
    
    # 5. 전체 인증 (authreq + tokenreq)
    print("🔐 No cached token, starting full authentication...")
    return await full_authentication(session, base_url)

# ==================== 메인 테스트 ====================

async def test_csc():
    # SSL Context for HTTPS (Self-signed)
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    # Use https://
    base_url = "https://localhost:4421"
    
    # Pass SSL context to connector
    connector = aiohttp.TCPConnector(ssl=ssl_context)
    async with aiohttp.ClientSession(connector=connector) as session:
        
        # ==================== 토큰 획득 ====================
        config = await get_valid_token(session, base_url)
        
        if not config:
            print("❌ Failed to get valid token")
            return
        
        # 헤더 설정
        headers = {"Authorization": f"Bearer {config['access_token']}"}
        access_token = config['access_token']
        
        # ==================== GMS 테스트 ====================
        print("\n" + "=" * 60)
        print("Testing GMS (Group Management)")
        print("=" * 60)
        
        print(f"\n[GMS] GET Group {TEST_GROUP}...")
        print("\n--- Testing GMS: Group Retrieval ---")
        async with session.get(f"{base_url}/org.openmobilealliance.groups/users/{TEST_USER}/{TEST_GROUP}", headers=headers) as resp:
            print(f"Status: {resp.status}")
            etag = resp.headers.get('ETag')
            print(f"ETag: {etag}")
            if resp.status == 404:
                print("GMS Retrieval Failed")

        print(f"\n[GMS] PUT Group {TEST_GROUP} (Create/Update)...")
        print("\n--- Testing GMS: Group Creation (PUT) ---")
        group_data = {
            "uri": TEST_GROUP,
            "display-name": "Test Group 2000"
        }
        async with session.put(f"{base_url}/org.openmobilealliance.groups/users/{TEST_USER}/{TEST_GROUP}", json=group_data, headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Response:\n{xml_text}") # XML 전체 출력
            if resp.status == 200:
                print("GMS Create Success")

        print("\n⏳ Waiting 10 seconds for manual verification...")
        await asyncio.sleep(10)

        print("\n--- Testing GMS: Created Group Retrieval (GET) ---")
        async with session.get(f"{base_url}/org.openmobilealliance.groups/users/{TEST_USER}/{TEST_GROUP}", headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Response:\n{xml_text}") # XML 전체 출력
            if resp.status == 200:
                print("GMS Created Group Found")

        print(f"\n[GMS] DELETE Group {TEST_GROUP}...")
        print("\n--- Testing GMS: Group Deletion (DELETE) ---")
        async with session.delete(f"{base_url}/org.openmobilealliance.groups/users/{TEST_USER}/{TEST_GROUP}", headers=headers) as resp:
            print(f"Status: {resp.status}")
            if resp.status == 200:
                print("GMS Delete Success")

        print("\n--- Testing GMS: Deleted Group Retrieval (GET) ---")
        async with session.get(f"{base_url}/org.openmobilealliance.groups/users/{TEST_USER}/{TEST_GROUP}", headers=headers) as resp:
            print(f"Status: {resp.status}")
            if resp.status == 404:
                print("GMS Deleted Group Not Found (Correct)")

        # ==================== CMS 테스트 ====================
        print("\n" + "=" * 60)
        print("Testing CMS (Configuration Management)")
        print("=" * 60)
        
        print("\n--- Testing CMS: User Profile ---")
        async with session.get(f"{base_url}/org.3gpp.mcptt.user-profile/users/{TEST_USER}/user-profile", headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Preview: {xml_text[:200]}...")
            if resp.status == 200:
                print("CMS Success")

        # ==================== KMS 테스트 ====================
        print("\n" + "=" * 60)
        print("Testing KMS (Key Management)")
        print("=" * 60)
        
        print("\n--- Testing KMS: Init ---")
        kms_init_data = {
            "UserUri": f"{TEST_USER}@ps-lte.com",
            "ClientId": "MCPTT_UE"
        }
        async with session.post(f"{base_url}/keymanagement/identity/v1/init", json=kms_init_data, headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Preview: {xml_text[:200]}...")
            if resp.status == 200:
                print("KMS Init Success")

        print("\n--- Testing KMS: Key Provisioning ---")
        kms_keyprov_data = {
            "UserUri": f"{TEST_USER}@ptt.mnc031.mcc450.3gppnetwork.org",
            "ClientId": "MCPTT_UE"
        }
        async with session.post(f"{base_url}/keymanagement/identity/v1/keyprov", json=kms_keyprov_data, headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Preview: {xml_text[:200]}...")
            if resp.status == 200:
                print("KMS KeyProv Success")

        print(f"\n[CMS] GET Service Config for {TEST_USER}...")
        print("\n--- Testing CMS: Service Config ---")
        async with session.get(f"{base_url}/org.3gpp.mcptt.service-config/users/{TEST_USER}/service-config", headers=headers) as resp:
            print(f"Status: {resp.status}")
            xml_text = await resp.text()
            print(f"XML Preview: {xml_text[:200]}...")
            if resp.status == 200:
                print("CMS Service Config Success")
        
        print("\n" + "=" * 60)
        print("✅ All Tests Completed!")
        print("=" * 60)

if __name__ == "__main__":
    asyncio.run(test_csc())
