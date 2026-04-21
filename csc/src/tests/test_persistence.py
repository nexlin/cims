
import aiohttp
import asyncio
import ssl
import sys

async def test_create_group():
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    connector = aiohttp.TCPConnector(ssl=ssl_context)
    base_url = "https://localhost:4420"
    headers = {'Authorization': 'Bearer dummy_token'} # Mock auth not fully checked for valid token in GMS yet, or use valid one?
    # Actually csc_service checks validate_access_token. I need a valid token.
    # So I must login first.

    async with aiohttp.ClientSession(connector=connector) as session:
        # 1. Login
        async with session.get(f"{base_url}/idms/authreq", params={"client_id":"MCPTT_UE", "user_name":"tel:+1000", "user_password":"1234", "response_type":"code", "scope":"openid 3gpp:mcptt:ptt_server", "redirect_uri":"http://client/cb", "state":"mystate"}, allow_redirects=False) as resp:
            if resp.status == 200:
                data = await resp.json()
                code = data.get('code')
            elif resp.status == 302:
                location = resp.headers['Location']
                import urllib.parse
                parsed = urllib.parse.urlparse(location)
                qs = urllib.parse.parse_qs(parsed.query)
                code = qs['code'][0]
            else:
                print("Login Failed")
                return

        async with session.post(f"{base_url}/idms/tokenreq", json={"grant_type": "authorization_code", "code": code}) as resp:
            token_data = await resp.json()
            access_token = token_data['access_token']
            headers = {'Authorization': f'Bearer {access_token}'}

        # 2. Create Group
        new_group_uri = "tel:+9999"
        headers['Content-Type'] = 'application/x-www-form-urlencoded' # or application/json
        async with session.put(f"{base_url}/org.openmobilealliance.groups/users/tel:+1000/{new_group_uri}", headers=headers, data="dummy=data") as resp:
            if resp.status in [200, 201]:
                print(f"Group {new_group_uri} Created")
            else:
                text = await resp.text()
                print(f"Group Create Failed: {resp.status} Body: {text}")

async def test_check_group():
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE
    connector = aiohttp.TCPConnector(ssl=ssl_context)
    base_url = "https://localhost:4420"

    async with aiohttp.ClientSession(connector=connector) as session:
        # 1. Login (Need token again)
        async with session.get(f"{base_url}/idms/authreq", params={"client_id":"MCPTT_UE", "user_name":"tel:+1000", "user_password":"1234", "response_type":"code", "scope":"openid 3gpp:mcptt:ptt_server", "redirect_uri":"http://client/cb", "state":"mystate"}, allow_redirects=False) as resp:
           # Simply assume success for brevity or same logic
            if resp.status == 200:
                data = await resp.json()
                code = data['code']
            else:
                print("Login Failed")
                return
        
        async with session.post(f"{base_url}/idms/tokenreq", json={"grant_type": "authorization_code", "code": code}) as resp:
            token_data = await resp.json()
            access_token = token_data['access_token']
            headers = {'Authorization': f'Bearer {access_token}'}

        # 2. Check Group
        new_group_uri = "tel:+9999"
        async with session.get(f"{base_url}/org.openmobilealliance.groups/users/tel:+1000/{new_group_uri}", headers=headers) as resp:
            if resp.status == 200:
                print(f"Group {new_group_uri} Found (Persistence Works)")
            else:
                print(f"Group {new_group_uri} Not Found (Persistence Failed)")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "check":
        asyncio.run(test_check_group())
    else:
        asyncio.run(test_create_group())
