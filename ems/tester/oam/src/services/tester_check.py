"""토폴로지 연결 검사 — 콘솔 [시험 > 토폴로지] 의 [연결 검사] (test_instrument.md §7).

run 을 걸기 전에 대상·워커에 닿는지 본다. 검사 항목 하나 = {name, ok, detail, ms}.
  - csp.udp   : SIP OPTIONS(UDP) → 응답(어떤 코드든) 이면 도달. 무응답 = 미도달.
  - csp.tcp / csp.tls : TCP connect(+TLS handshake, 인증서 검증 없음 — 도달 확인이 목적).
  - csp.peering : 피어링 접속점(있을 때) — 평상시엔 닫혀 있는 것이 정상(run 중 시드로 열림)이라 참고 항목.
  - csc       : TCP(+TLS) connect.
  - oam       : GET /api/v1/deployments 토큰 검증(200) — 시드·target_build 가 이 토큰을 쓴다.
  - worker.<name> : GET /health.
CSP 에 보내는 OPTIONS 는 등록 없이 처리되는 요청(RFC 3261 §11) — 가입자 상태를 건드리지 않는다.
"""
from __future__ import annotations

import socket
import ssl
import time
import uuid
from typing import List

from services import tester_workers
from services import tester_target
from services.tester_models import Topology


def _item(name: str, ok: bool, detail: str, t0: float) -> dict:
    return {'name': name, 'ok': ok, 'detail': detail, 'ms': int((time.time() - t0) * 1000)}


def _sip_options_udp(ip: str, port: int, domain: str, timeout: float = 2.0) -> dict:
    t0 = time.time()
    branch = 'z9hG4bK' + uuid.uuid4().hex[:16]
    call_id = uuid.uuid4().hex
    host = domain or ip
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(timeout)
        s.connect((ip, port))
        local_ip, local_port = s.getsockname()[:2]
        msg = (f'OPTIONS sip:{host} SIP/2.0\r\n'
               f'Via: SIP/2.0/UDP {local_ip}:{local_port};branch={branch};rport\r\n'
               f'Max-Forwards: 70\r\n'
               f'From: <sip:cims-tester@{host}>;tag={uuid.uuid4().hex[:8]}\r\n'
               f'To: <sip:{host}>\r\n'
               f'Call-ID: {call_id}\r\n'
               f'CSeq: 1 OPTIONS\r\n'
               f'Contact: <sip:cims-tester@{local_ip}:{local_port}>\r\n'
               f'User-Agent: oam-cims-tester\r\n'
               f'Accept: application/sdp\r\n'
               f'Content-Length: 0\r\n\r\n')
        s.send(msg.encode('utf-8'))
        data = s.recv(4096)
        s.close()
        first = data.split(b'\r\n', 1)[0].decode('utf-8', 'replace')
        return _item('csp.udp', True, first, t0)
    except socket.timeout:
        return _item('csp.udp', False, f'{ip}:{port} OPTIONS 무응답 ({timeout:.0f}s)', t0)
    except Exception as e:
        return _item('csp.udp', False, f'{ip}:{port} {e}', t0)


def _tcp_connect(name: str, ip: str, port: int, tls: bool, timeout: float = 2.0) -> dict:
    t0 = time.time()
    try:
        raw = socket.create_connection((ip, port), timeout=timeout)
        try:
            if tls:
                ctx = ssl.create_default_context()
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                with ctx.wrap_socket(raw, server_hostname=ip) as ts:
                    ver = ts.version() or 'TLS'
                    return _item(name, True, f'{ip}:{port} {ver} 연결', t0)
            return _item(name, True, f'{ip}:{port} TCP 연결', t0)
        finally:
            try:
                raw.close()
            except Exception:
                pass
    except Exception as e:
        return _item(name, False, f'{ip}:{port} {e}', t0)


def _oam_token(topology: Topology) -> dict:
    t0 = time.time()
    oam = topology.target.oam
    if oam is None:
        return _item('oam', True, '대상 OAM 미설정(시드·target_build 없이 실행)', t0)
    try:
        client = tester_target.OamClient(oam.url, tester_target.token_from_env(oam))
        rows = client.deployments()
        try:
            dep = client.find_csp_deployment(oam.csp_deployment_id)
            return _item('oam', True, f'{oam.url} 배포 {len(rows)}건 · CSP 배포 {dep}', t0)
        except tester_target.TargetError as e:
            return _item('oam', False, f'{oam.url} 토큰 유효 · {e}', t0)
    except tester_target.TargetError as e:
        return _item('oam', False, str(e), t0)


def check_topology(topology: Topology, topology_doc: dict) -> List[dict]:
    csp = topology.target.csp
    items: List[dict] = [_sip_options_udp(csp.ip, csp.udp, csp.domain_volte or '')]
    items.append(_tcp_connect('csp.tcp', csp.ip, csp.tcp, tls=False))
    items.append(_tcp_connect('csp.tls', csp.ip, csp.tls, tls=True))
    if csp.peering is not None:
        p = csp.peering
        pi = _tcp_connect('csp.peering', p.ip or csp.ip, p.port, tls=(p.protocol == 'tls')) if p.protocol != 'udp' \
            else _sip_options_udp(p.ip or csp.ip, p.port, csp.domain_volte or '')
        pi['name'] = 'csp.peering'
        if not pi['ok']:
            pi['detail'] += ' — 피어링 접속점은 run 시드가 열고 복원이 닫는다(평상시 닫힘 정상)'
            pi['info'] = True
        items.append(pi)
    if topology.target.csc is not None:
        c = topology.target.csc
        items.append(_tcp_connect('csc', c.host, c.port, tls=c.tls))
    items.append(_oam_token(topology))
    for w in tester_workers.discover(topology_doc):
        t0 = time.time()
        h = w.probe()
        if h is None:
            items.append(_item(f'worker.{w.name}', False, f'{w.url} {w.health_error}', t0))
        else:
            items.append(_item(f'worker.{w.name}', True,
                               f"{w.url} v{h.get('version')} 단말 {h.get('active_endpoints')}/{h.get('max_endpoints')} "
                               f"cpu {h.get('cpu_pct')}% skew {h.get('clock_skew_ms')} ms"
                               + (f" · run {h.get('active_run')} 진행 중" if h.get('active_run') else ''), t0))
    return items
