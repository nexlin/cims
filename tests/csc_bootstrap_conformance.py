#!/usr/bin/env python3
"""
규격 순정 MCX 단말의 부트스트랩 전 구간 시뮬레이션 (TS 24.484 / TS 33.180)

외부(고객사) 단말이 밟는 표준 순서를 그대로 밟는다 — 핵심은 각 단계의 주소를
하드코딩하지 않고 **앞 단계가 준 문서에서 파싱**해 다음 단계로 가는 것. 낱개
endpoint 시험은 광고 주소가 틀려도 통과하지만, 이 사슬 검증은 그 드리프트를 잡는다
(conformance §R4-1 — 병행 서빙의 규격 표면 회귀 고정).

흐름:
  Step 1  익명 GET ue-init-config (XUI=인스턴스 UUID — 고객사 08-13 요청 형태)  → 200, well-formed
  Step 2  문서에서 IdMS 엔드포인트·CMS/GMS XCAP 루트 "파싱"
  Step 3  파싱한 IdMS 로 PKCE 로그인 — **자체 단말 간이형**(GET+자격 → JSON)        → 토큰
  Step 3b 파싱한 IdMS 로 PKCE 로그인 — **규격 흐름**(TS 24.482 §6.3.1: GET → HTML 폼 →
          POST form → 302 code&state → tokenreq form-urlencoded)                → 토큰
          (+ 음성: 틀린 비번 → 폼 재표시, code 미발급)
  Step 4  파싱한 CMS 루트로 user-profile GET — XUI 를 **sip:완전형(@도메인)** 으로
          (외부 단말 신원 표기 관용 — _norm_mcptt_uri 도메인 제거 검증)          → 200
  Step 5  service-config GET + ETag 재요청                                     → 200/304
  Step 6  파싱한 GMS 루트로 그룹 목록 GET                                      → 200
  Step 7  음성 대조 — 익명 user-profile → 401 / 남의 문서 → 403 (경계 무손상)

사용법:
  python3 tests/csc_bootstrap_conformance.py
  python3 tests/csc_bootstrap_conformance.py --host 121.161.164.45 --port 4430
  python3 tests/csc_bootstrap_conformance.py --login test004 --password 1234
"""

import argparse
import base64
import hashlib
import json
import os
import re
import ssl
import sys
import urllib.parse
import urllib.request
import uuid

CTX = ssl.create_default_context()
CTX.check_hostname = False
CTX.verify_mode = ssl.CERT_NONE   # 사설 CA — 시험 스크립트는 미검증 (단말은 CA 동봉 검증)

g_pass, g_fail = 0, 0


def check(cond, msg):
    global g_pass, g_fail
    if cond:
        g_pass += 1
        print(f"  PASS  {msg}")
    else:
        g_fail += 1
        print(f"  FAIL  {msg}")


def http_get(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10, context=CTX) as r:
            return r.status, r.read().decode(), dict((k.lower(), v) for k, v in r.headers.items())
    except urllib.error.HTTPError as e:
        return e.code, (e.read() or b'').decode(), {}
    except Exception as e:
        return 0, str(e), {}


def http_post_json(url, data):
    req = urllib.request.Request(url, data=json.dumps(data).encode(),
                                 headers={'Content-Type': 'application/json'}, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=10, context=CTX) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, {}
    except Exception as e:
        return 0, {'error': str(e)}


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """302 를 따라가지 않는다 — 규격 흐름은 Location 의 code/state 를 단말이 읽는 것이 핵심."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER_NOREDIR = urllib.request.build_opener(_NoRedirect, urllib.request.HTTPSHandler(context=CTX))


def http_post_form(url, fields, parse_json=False):
    """application/x-www-form-urlencoded POST (리다이렉트 미추종). → (status, body_text_or_json, headers)"""
    data = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(url, data=data,
                                 headers={'Content-Type': 'application/x-www-form-urlencoded'}, method='POST')
    try:
        with _OPENER_NOREDIR.open(req, timeout=10) as r:
            body = r.read().decode()
            return r.status, (json.loads(body) if parse_json else body), dict((k.lower(), v) for k, v in r.headers.items())
    except urllib.error.HTTPError as e:
        body = (e.read() or b'').decode()
        hdr = dict((k.lower(), v) for k, v in e.headers.items()) if e.headers else {}
        return e.code, body, hdr
    except Exception as e:
        return 0, str(e), {}


def form_inputs(html):
    """<form> 안의 <input …> 을 {name: (type, value)} 로 — 규격 단말이 폼을 헤드리스로 채우는 방식."""
    out = {}
    for tag in re.findall(r'<input[^>]*>', html):
        n = re.search(r'name="([^"]*)"', tag)
        t = re.search(r'type="([^"]*)"', tag)
        v = re.search(r'value="([^"]*)"', tag)
        if n:
            out[n.group(1)] = (t.group(1) if t else 'text', v.group(1) if v else '')
    return out


def xml_text(xml, tag):
    m = re.search(rf"<{tag}>([^<]*)</{tag}>", xml)
    return m.group(1).strip() if m else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default=os.environ.get('CSC_HOST', '127.0.0.1'))
    ap.add_argument('--port', type=int, default=int(os.environ.get('CSC_PORT', '4430')))
    ap.add_argument('--login', default='test004')
    ap.add_argument('--password', default='1234')
    ap.add_argument('--other-user', default='tel:+82500000001',
                    help='Step 7 남의 문서 403 확인용 (본인과 다른 실존 가입자)')
    args = ap.parse_args()
    base_req = f"https://{args.host}:{args.port}"

    # ── Step 1: 익명 ue-init-config (로그인 전 — 토큰 없음, XUI=인스턴스 UUID) ──
    print("Step 1  ue-init-config (익명)")
    inst = str(uuid.uuid4())
    st, body, hdr = http_get(f"{base_req}/org.3gpp.mcptt.ue-init-config/users/sip:{inst}/{inst}")
    check(st == 200, f"200 OK (got {st})")
    check('mcptt-UE-initial-configuration' in body, "규격 루트 엘리먼트")
    check(bool(hdr.get('etag')), "ETag 존재")
    try:
        from xml.dom import minidom
        minidom.parseString(body.encode())
        check(True, "well-formed XML (UeInitConfig 설정값 escape 검증)")
    except Exception as e:
        check(False, f"well-formed XML ({e})")

    # ── Step 2: 문서에서 다음 단계 주소 파싱 (하드코딩 금지 — 사슬 검증의 핵심) ──
    print("Step 2  광고 주소 파싱 (TS 24.484 §7.2.2.3 요소명)")
    auth_ep  = xml_text(body, 'idms-auth-endpoint')
    token_ep = xml_text(body, 'idms-token-endpoint')
    cms_root = xml_text(body, 'CMS-XCAP-root-URI')
    gms_root = xml_text(body, 'GMS-XCAP-root-URI')
    m = re.search(r'domain="([^"]+)"', body)
    domain = m.group(1) if m else ''
    gms_psi = xml_text(body, 'GMS-URI')
    check(all([auth_ep, token_ep, cms_root, gms_root, domain]), f"필수 요소 (domain={domain})")
    check(bool(re.search(r'xmlns="urn:3gpp:mcptt:mcpttUEinitConfig:1.0"', body)), "규격 네임스페이스")
    check(gms_psi.startswith('sip:gms_psi@'), f"GMS-URI = 구독 프록시 PSI ({gms_psi})")

    # ── Step 3: 파싱한 IdMS 로 PKCE 로그인 ──
    print("Step 3  IdMS PKCE 로그인 (파싱한 엔드포인트로)")
    verifier = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b'=').decode()
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b'=').decode()
    q = urllib.parse.urlencode({'user_name': args.login, 'user_password': args.password,
                                'client_id': 'MCPTT_UE', 'redirect_uri': 'http://localhost/cb',
                                'code_challenge': challenge, 'code_challenge_method': 'S256',
                                'scope': '3gpp:mcptt:ptt_server'})
    st, body2, _ = http_get(f"{auth_ep}?{q}")
    code = ''
    try:
        code = json.loads(body2).get('code', '')
    except Exception:
        pass
    check(st == 200 and code, f"authreq 200 + code (got {st})")
    st, tok = http_post_json(token_ep, {'code': code, 'code_verifier': verifier, 'client_id': 'MCPTT_UE',
                                        'redirect_uri': 'http://localhost/cb',
                                        'grant_type': 'authorization_code'})
    access = tok.get('access_token', '')
    check(st == 200 and access, f"tokenreq 200 + access_token (got {st})")
    mcptt_id = ''
    try:
        pl = access.split('.')[1]
        mcptt_id = json.loads(base64.urlsafe_b64decode(pl + '=' * (-len(pl) % 4))).get('mcptt_id', '')
    except Exception:
        pass
    check(bool(mcptt_id), f"토큰 mcptt_id ({mcptt_id})")
    bearer = {'Authorization': f'Bearer {access}'}

    # ── Step 3b: 규격 흐름 (TS 24.482 §6.3.1 / OIDC Core §3.1.2) — 자격 없는 GET → 폼 → POST → 302 ──
    print("Step 3b IdMS 규격 흐름 (GET 폼 → POST form → 302 → tokenreq form-urlencoded)")
    verifier_b = base64.urlsafe_b64encode(os.urandom(32)).rstrip(b'=').decode()
    challenge_b = base64.urlsafe_b64encode(hashlib.sha256(verifier_b.encode()).digest()).rstrip(b'=').decode()
    state_b = base64.urlsafe_b64encode(os.urandom(8)).rstrip(b'=').decode()
    redirect_b = 'https://localhost/callback'
    q = urllib.parse.urlencode({'response_type': 'code', 'client_id': 'MCPTT_UE', 'redirect_uri': redirect_b,
                                'code_challenge': challenge_b, 'code_challenge_method': 'S256',
                                'scope': 'openid ptt_service', 'state': state_b, 'nonce': 'n-3b'})
    st, html, hdr = http_get(f"{auth_ep}?{q}")
    check(st == 200 and 'text/html' in hdr.get('content-type', ''), f"자격 없는 GET → 200 text/html (got {st})")
    inputs = form_inputs(html)
    m = re.search(r'<form[^>]*action="([^"]+)"', html)
    action = m.group(1) if m else ''
    login_field = next((n for n, (t, _) in inputs.items() if t == 'text'), '')
    pw_field = next((n for n, (t, _) in inputs.items() if t == 'password'), '')
    check(bool(action) and bool(login_field) and bool(pw_field),
          f"폼 파싱: action={action.split('//')[-1][:40]} login={login_field} pw={pw_field}")
    hidden = {n: v for n, (t, v) in inputs.items() if t == 'hidden'}
    check(hidden.get('state') == state_b and hidden.get('code_challenge') == challenge_b and
          hidden.get('redirect_uri') == redirect_b, "hidden 문맥 이월(state·code_challenge·redirect_uri)")
    # 음성 — 틀린 비번: 폼 재표시(200), 302/코드 없음
    st, body_neg, hdr_neg = http_post_form(action, dict(hidden, **{login_field: args.login, pw_field: 'wrong-pw'}))
    check(st == 200 and 'error' in body_neg and not hdr_neg.get('location'),
          f"틀린 비번 → 200 폼 재표시 + 오류, Location 없음 (got {st})")
    # 정상 — 302 Location redirect_uri?code&state
    st, _, hdr_ok = http_post_form(action, dict(hidden, **{login_field: args.login, pw_field: args.password}))
    loc = hdr_ok.get('location', '')
    qs = urllib.parse.parse_qs(urllib.parse.urlsplit(loc).query)
    code_b = (qs.get('code') or [''])[0]
    check(st == 302 and loc.startswith(redirect_b + '?') and code_b and qs.get('state') == [state_b],
          f"POST 폼 → 302 Location code&state (got {st})")
    # tokenreq — 규격 형식(form-urlencoded)으로
    st, tok_b, _ = http_post_form(token_ep, {'grant_type': 'authorization_code', 'code': code_b,
                                             'code_verifier': verifier_b, 'client_id': 'MCPTT_UE',
                                             'redirect_uri': redirect_b}, parse_json=True)
    access_b = tok_b.get('access_token', '') if isinstance(tok_b, dict) else ''
    check(st == 200 and access_b and tok_b.get('id_token'), f"tokenreq(form-urlencoded) 200 + access/id_token (got {st})")
    try:
        pl = access_b.split('.')[1]
        mcptt_id_b = json.loads(base64.urlsafe_b64decode(pl + '=' * (-len(pl) % 4))).get('mcptt_id', '')
    except Exception:
        mcptt_id_b = ''
    check(mcptt_id_b == mcptt_id, f"규격 흐름 토큰의 mcptt_id 가 간이형과 동일 ({mcptt_id_b})")
    st, _, _ = http_post_form(token_ep, {'grant_type': 'authorization_code', 'code': code_b,
                                         'code_verifier': verifier_b, 'client_id': 'MCPTT_UE',
                                         'redirect_uri': redirect_b}, parse_json=True)
    check(st == 400, f"code 재사용 → 400 (1회성) (got {st})")

    # ── Step 4: user-profile — XUI 를 외부 단말식 sip:완전형(@도메인)으로 ──
    print("Step 4  user-profile (XUI=sip:완전형 — 신원 표기 관용)")
    bare = mcptt_id.split(':', 1)[-1]
    xui_full = urllib.parse.quote(f"sip:{bare}@{domain}", safe='')
    st, body, hdr = http_get(f"{cms_root}/org.3gpp.mcptt.user-profile/users/{xui_full}/user-profile", bearer)
    check(st == 200, f"sip:{bare}@{domain} 형 XUI 로 200 (got {st})")

    # ── Step 5: service-config + ETag 캐시 ──
    print("Step 5  service-config + ETag")
    xui = urllib.parse.quote(mcptt_id, safe='')
    st, body, hdr = http_get(f"{cms_root}/org.3gpp.mcptt.service-config/users/{xui}/service-config", bearer)
    check(st == 200, f"200 (got {st})")
    etag = hdr.get('etag', '')
    st2, _, _ = http_get(f"{cms_root}/org.3gpp.mcptt.service-config/users/{xui}/service-config",
                         dict(bearer, **{'If-None-Match': etag}))
    check(st2 == 304, f"If-None-Match 재요청 304 (got {st2})")

    # ── Step 6: 그룹 목록 (GMS 루트) ──
    print("Step 6  그룹 목록 (GMS)")
    st, body, _ = http_get(f"{gms_root}/org.openmobilealliance.groups/users/{xui}", bearer)
    check(st == 200, f"200 (got {st})")

    # ── Step 7: 음성 대조 — 익명 개방이 경계를 안 넘는가 ──
    print("Step 7  경계 대조 (401/403)")
    st, _, _ = http_get(f"{cms_root}/org.3gpp.mcptt.user-profile/users/{xui}/user-profile")
    check(st == 401, f"익명 user-profile 401 (got {st})")
    other = urllib.parse.quote(args.other_user, safe='')
    st, _, _ = http_get(f"{cms_root}/org.3gpp.mcptt.user-profile/users/{other}/user-profile", bearer)
    check(st == 403, f"남의 프로파일 403 (got {st})")

    print(f"\n결과: PASS {g_pass} / FAIL {g_fail}")
    sys.exit(1 if g_fail else 0)


if __name__ == '__main__':
    main()
