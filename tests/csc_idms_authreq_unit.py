#!/usr/bin/env python3
"""
CSC IdMS authreq 이중 모드 + ue-init-config 설정화 — **오프라인 단위시험** (서버 기동 없음).

`services.mcptt` 를 직접 import 해 핸들러를 호출한다. 검증 대상:
  A. ue-init-config 생성기 — UeInitConfig.* 설정 반영·escape·well-formed·ETag 내용파생·
     anyExt/*-Service-Details on/off·깨진 문서 시 마지막 정상 문서 유지
  B. authreq 3분기 — ① GET+자격 → JSON(종전 호환) ② GET(자격 없음) → HTML 폼(입력칸 이름=설정)
     ③ POST 폼 → 302 Location(code&state, PKCE 결박) / 실패 → 폼 재표시(200) / redirect 허용목록

사용법:  python3 tests/csc_idms_authreq_unit.py
(vendor site-packages 는 csc/vendor 또는 설치본 modules/csc/current/csc/vendor 에서 찾는다.)
"""
import asyncio
import os
import re
import sys
import tempfile
import urllib.parse
from xml.dom import minidom

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
sys.path.insert(0, os.path.join(ROOT, 'csc', 'src'))
for _v in (os.path.join(ROOT, 'csc', 'vendor'), '/opt/cims-agent/modules/csc/current/csc/vendor'):
    if os.path.isdir(_v):
        sys.path.append(_v)
        break

_TMP = tempfile.mkdtemp(prefix='csc_authreq_unit_')
os.chdir(_TMP)   # Logger()/file_store 가 CWD 기준 산출물을 만들지 않도록 격리

from services import mcptt                      # noqa: E402
from httpsrv.handler import HandlerArgs         # noqa: E402

g_pass, g_fail = 0, 0


def check(cond, msg):
    global g_pass, g_fail
    if cond:
        g_pass += 1
        print(f"  PASS  {msg}")
    else:
        g_fail += 1
        print(f"  FAIL  {msg}")


def cfg(**over):
    base = {
        'IdMs': {'Domain': 'ptt.mnc033.mcc450.3gppnetwork.org', 'JwtSecret': 'unit'},
        'Provisioning': {'Services': {'ptt': {'domain': 'ptt.mnc033.mcc450.3gppnetwork.org'}}},
        'McpttServer': {'Port': 4430},
        'CimsRuntimeDir': _TMP,
    }
    base.update(over)
    return base


def args(method, query=None, body=None, host='cims.example:4430'):
    return HandlerArgs(method=method, full_path='/idms/authreq', client_ip='127.0.0.1', client_port=1,
                       query_params=query or {}, headers={'host': host}, body=body)


def run(coro):
    return asyncio.run(coro)


def pkce_ctx(**over):
    d = {'client_id': 'MCPTT_UE', 'redirect_uri': 'https://localhost/callback', 'state': 'st1',
         'scope': 'openid ptt_service', 'code_challenge': 'E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM',
         'code_challenge_method': 'S256', 'nonce': 'n1'}
    d.update(over)
    return d


def form_inputs(html):
    """<input …> 들을 {name: (type, value)} 로."""
    out = {}
    for tag in re.findall(r'<input[^>]*>', html):
        n = re.search(r'name="([^"]*)"', tag)
        t = re.search(r'type="([^"]*)"', tag)
        v = re.search(r'value="([^"]*)"', tag)
        if n:
            out[n.group(1)] = (t.group(1) if t else 'text', v.group(1) if v else '')
    return out


def main():
    # ── A. ue-init-config 생성기 ─────────────────────────────────────────────
    print("A   ue-init-config 설정화")
    mcptt.apply_config(cfg())
    mcptt.storage.init_db(cfg())
    base = 'https://cims.example:4430'
    xml0, etag0 = mcptt.get_ue_init_config_xml(base)
    minidom.parseString(xml0.encode())
    check('<T132>6</T132>' in xml0 and '<name>CIMS</name>' in xml0, "기본값 문서(T132=6, name=CIMS)")
    check('PLMN="45033"' in xml0, "PLMN 도메인 유도 45033")
    check('<MCPTT-Service-Details>' in xml0 and 'sip:mcptt_psi@ptt.mnc033.mcc450.3gppnetwork.org' in xml0,
          "anyExt MCPTT-Service-Details 기본 on + Server-URI 유도")
    check('<MCData-Service-Details>' not in xml0, "MCData-Service-Details 기본 off")
    check(xml0.rstrip().endswith('</mcptt-UE-initial-configuration>') and
          xml0.index('<anyExt>') > xml0.index('<confidentiality-protection-enabled>'),
          "anyExt 가 on-network sequence 마지막")

    mcptt.apply_config(cfg(UeInitConfig={
        'Name': 'Acme <MCX> & "Co"', 'Timers': {'T100': 7, 'T132': '300'},
        'Hplmn': {'Plmn': '00101', 'McpttConRef': 'mcx.apn'},
        'HttpProxy': 'http://proxy.acme:8080', 'TlsMutualAuthentication': True,
        'GroupCreationXui': 'https://xcap.acme/root',
        'ServiceDetails': {'Mcptt': {'Enable': True, 'ServerUri': 'sip:mcptt-as.acme'},
                           'McData': {'Enable': True}}}))
    xml1, etag1 = mcptt.get_ue_init_config_xml(base)
    minidom.parseString(xml1.encode())
    check('<name>Acme &lt;MCX&gt; &amp; &quot;Co&quot;</name>' in xml1, "값 html.escape (well-formed 유지)")
    check('<T100>7</T100>' in xml1 and '<T132>255</T132>' in xml1 and '<T101>4</T101>' in xml1,
          "Timers 설정 반영 + 범위 절단(300→255) + 미지정은 기본값")
    check('PLMN="00101"' in xml1 and '<MCPTT-to-con-ref>mcx.apn</MCPTT-to-con-ref>' in xml1, "HPLMN 설정 우선")
    check('<http-proxy>http://proxy.acme:8080</http-proxy>' in xml1 and
          '<mutual-authentication>true</mutual-authentication>' in xml1, "http-proxy·mutual-auth 반영")
    check('<group-creation-XUI>https://xcap.acme/root</group-creation-XUI>' in xml1, "group-creation-XUI 설정 우선")
    check('<Server-URI>sip:mcptt-as.acme</Server-URI>' in xml1 and
          '<MCData-Service-Details>' in xml1 and 'sip:mcdata_psi@' in xml1,
          "Service-Details: MCPTT ServerUri 지정 + MCData on(유도)")
    check(etag1 != etag0, "ETag 내용 파생 — 설정 변경으로 갱신")
    check('<idms-auth-endpoint>https://cims.example:4430/idms/authreq</idms-auth-endpoint>' in xml1,
          "주소류는 여전히 요청 Host 유도(계층①)")

    # 깨진 문서 → 마지막 정상 문서 유지
    _orig = mcptt._build_ue_init_config_xml
    mcptt._build_ue_init_config_xml = lambda b: "<broken><x></broken>"
    try:
        xml2, etag2 = mcptt.get_ue_init_config_xml(base)
    finally:
        mcptt._build_ue_init_config_xml = _orig
    check(xml2 == xml1 and etag2 == etag1, "well-formed 실패 시 마지막 정상 문서 유지")

    mcptt.apply_config(cfg())   # 기본값 복귀
    xml3, etag3 = mcptt.get_ue_init_config_xml(base)
    check(etag3 == etag0, "설정 복귀 → 원 ETag 복귀(결정적)")

    # ── B. authreq 3분기 ─────────────────────────────────────────────────────
    print("B   authreq 이중 모드")
    mcptt.LOGIN_ACCOUNTS.clear()
    mcptt.LOGIN_ACCOUNTS['test004'] = {'password': '1234', 'mcptt_id': 'tel:+82500000004'}

    # ① 자체 단말 JSON 경로 (종전 호환)
    r = run(mcptt.handle_auth_req(args('GET', dict(pkce_ctx(), user_name='test004', user_password='1234')), {}))
    check(r.status == 200 and isinstance(r.body, dict) and r.body.get('code') and r.body.get('state') == 'st1',
          "① GET+자격 → 200 JSON code/state")
    ad = mcptt.storage.get_auth_code(r.body['code'])
    check(ad and ad['mcptt_id'] == 'tel:+82500000004' and ad['code_challenge'] == pkce_ctx()['code_challenge']
          and ad['nonce'] == 'n1', "① auth-code 에 mcptt_id·PKCE·nonce 결박")
    r = run(mcptt.handle_auth_req(args('GET', dict(pkce_ctx(), user_name='test004', user_password='x')), {}))
    check(r.status == 401 and r.body.get('error') == 'access_denied', "① 틀린 비번 → 401 access_denied")
    r = run(mcptt.handle_auth_req(args('GET', dict(pkce_ctx(code_challenge=''), user_name='test004',
                                                   user_password='1234')), {}))
    check(r.status == 400, "① code_challenge 없음 → 400")
    r = run(mcptt.handle_auth_req(args('GET', dict(pkce_ctx(code_challenge_method='plain'),
                                                   user_name='test004', user_password='1234')), {}))
    check(r.status == 400, "① plain method → 400")
    q = dict(pkce_ctx(), user_name='test004', user_password='1234'); q.pop('redirect_uri')
    r = run(mcptt.handle_auth_req(args('GET', q), {}))
    check(r.status == 200 and r.body.get('Location') is None, "① redirect_uri 생략 허용(종전 호환)")

    # ② 규격 GET → 폼
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx()), {}))
    html = r.body if isinstance(r.body, str) else ''
    inputs = form_inputs(html)
    check(r.status == 200 and r.media_type.startswith('text/html'), "② GET(자격 없음) → 200 text/html")
    check(inputs.get('username', ('',))[0] == 'text' and inputs.get('password', ('',))[0] == 'password',
          "② 입력칸 name=username/password (기본 설정)")
    check(all(inputs.get(k, ('', ''))[1] == v for k, v in pkce_ctx().items()),
          "② hidden 문맥 7종 이월(client_id·redirect_uri·state·scope·nonce·PKCE)")
    check(re.search(r'action="https://cims\.example:4430/idms/authreq"', html) is not None and
          'method="post"' in html, "② form action=절대 URL(Host 유도), method=post")
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx(redirect_uri='')), {}))
    check(r.status == 400, "② redirect_uri 없는 폼 요청 → 400")
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx(response_type='token')), {}))
    check(r.status == 400, "② response_type≠code → 400")
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx(scope='ptt_service unknown')), {}))
    check(r.status == 200, "② 미지 scope 비거절")

    # 입력칸 이름 설정화
    mcptt.apply_config(cfg(IdMs={'Domain': 'ptt.mnc033.mcc450.3gppnetwork.org', 'JwtSecret': 'unit',
                                 'FormLoginField': 'j_username', 'FormPasswordField': 'j_password'}))
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx()), {}))
    inputs = form_inputs(r.body)
    check('j_username' in inputs and 'j_password' in inputs and 'username' not in inputs,
          "② IdMs.FormLoginField/FormPasswordField 반영")

    # ③ POST 폼 (설정된 입력칸 이름으로)
    form = dict(pkce_ctx(), j_username='test004', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    loc = r.headers.get('Location', '')
    qs = urllib.parse.parse_qs(urllib.parse.urlsplit(loc).query)
    check(r.status == 302 and loc.startswith('https://localhost/callback?') and qs.get('code') and
          qs.get('state') == ['st1'], f"③ POST 폼 → 302 Location code&state ({loc[:60]}…)")
    check(r.headers.get('Cache-Control') == 'no-store', "③ 302 Cache-Control: no-store")
    ad = mcptt.storage.get_auth_code(qs['code'][0])
    check(ad and ad['login_id'] == 'test004' and ad['mcptt_id'] == 'tel:+82500000004' and
          ad['code_challenge'] == pkce_ctx()['code_challenge'] and ad['redirect_uri'] == 'https://localhost/callback',
          "③ auth-code 에 신원·PKCE·redirect_uri 결박 (tokenreq 검증 대상)")
    form = dict(pkce_ctx(redirect_uri='https://cb.acme/x?app=1'), j_username='test004', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 302 and r.headers['Location'].startswith('https://cb.acme/x?app=1&code='),
          "③ redirect_uri 에 query 있으면 & 로 이어붙임")
    form = dict(pkce_ctx(state=''), j_username='test004', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 302 and 'state=' not in r.headers['Location'], "③ state 없으면 Location 에 state 생략")

    form = dict(pkce_ctx(), j_username='test004', j_password='wrong')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    inputs = form_inputs(r.body if isinstance(r.body, str) else '')
    check(r.status == 200 and r.media_type.startswith('text/html') and 'class="error"' in r.body and
          '비밀번호' in r.body, "③ 틀린 비번 → 200 폼 재표시 + 오류")
    check(inputs.get('state', ('', ''))[1] == 'st1' and inputs.get('code_challenge', ('', ''))[1],
          "③ 재표시 폼에 문맥 이월 유지")
    form = dict(pkce_ctx(), j_username='nobody', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 200 and 'class="error"' in r.body, "③ 미지 사용자 → 200 폼 재표시 + 오류")
    form = dict(pkce_ctx(redirect_uri=''), j_username='test004', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 400, "③ POST redirect_uri 없음 → 400")
    form = dict(pkce_ctx(code_challenge=''), j_username='test004', j_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 400, "③ POST code_challenge 없음 → 400")
    form = dict(pkce_ctx(), user_name='test004', user_password='1234')
    r = run(mcptt.handle_auth_req(args('POST', body=form), {}))
    check(r.status == 302, "③ POST 에 user_name/user_password 예비 필드도 수용")
    r = run(mcptt.handle_auth_req(args('PUT'), {}))
    check(r.status == 405, "PUT → 405")

    # redirect_uri 허용목록
    mcptt.apply_config(cfg(IdMs={'Domain': 'ptt.mnc033.mcc450.3gppnetwork.org', 'JwtSecret': 'unit',
                                 'RedirectUriAllow': ['https://localhost/callback', 'cims://cb']}))
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx()), {}))
    check(r.status == 200, "허용목록: 등록된 redirect_uri → 폼 200")
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx(redirect_uri='https://evil/cb')), {}))
    check(r.status == 400, "허용목록: 미등록 redirect_uri → 400 (폼 경로)")
    r = run(mcptt.handle_auth_req(args('GET', dict(pkce_ctx(redirect_uri='https://evil/cb'),
                                                   user_name='test004', user_password='1234')), {}))
    check(r.status == 400, "허용목록: 미등록 redirect_uri → 400 (JSON 경로도 동일 집행)")
    r = run(mcptt.handle_auth_req(args('POST', body=dict(pkce_ctx(redirect_uri='https://evil/cb'),
                                                          username='test004', password='1234')), {}))
    check(r.status == 400, "허용목록: 미등록 redirect_uri → 400 (POST)")
    mcptt.apply_config(cfg(IdMs={'Domain': 'ptt.mnc033.mcc450.3gppnetwork.org', 'JwtSecret': 'unit',
                                 'RedirectUriAllow': 'https://a/cb, https://localhost/callback'}))
    r = run(mcptt.handle_auth_req(args('GET', pkce_ctx()), {}))
    check(r.status == 200 and mcptt.IDMS_REDIRECT_URI_ALLOW == ['https://a/cb', 'https://localhost/callback'],
          "허용목록: 콤마 문자열 표기도 수용")

    print(f"\n결과: PASS {g_pass} / FAIL {g_fail}")
    sys.exit(1 if g_fail else 0)


if __name__ == '__main__':
    main()
