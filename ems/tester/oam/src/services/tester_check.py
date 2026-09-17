"""토폴로지 연결 검사 — 콘솔 [시험 > 토폴로지] 의 [연결 검사] (test_instrument.md §7).

run 을 걸기 전에 대상·워커에 닿는지 본다. 검사 항목 하나 = {name, ok, detail, ms, info?, target{kind,id}}. 항목 이름 규약:
  - `<노드>:<수신점 id>` : SIP 노드 수신점(sip.listeners) — UDP 는 OPTIONS(응답 어떤 코드든 도달 — 피어링 접속점은 Route 없는 소스에 403 을
                          주지만 그것도 도달이다), TCP/TLS 는 connect(+handshake, 인증서 검증 없음). edge=peering 항목은 run 시드가 열고
                          복원이 닫을 수 있어 실패를 참고(info)로 둔다
  - `<노드>:api`         : subscriber 노드 API(TCP/TLS connect) · `<노드>:db` : DB 포트 connect · `<노드>:oam` : 대상 OAM 토큰(배포 목록 200)
  - `<호스트>:ssh`       : SSH 포트 도달(관측 자체는 후속) · `worker_<이름>` : GET /health
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


def _item(name: str, ok: bool, detail: str, t0: float, kind: str = '', tid: str = '') -> dict:
    return {'name': name, 'ok': ok, 'detail': detail, 'ms': int((time.time() - t0) * 1000), 'target': {'kind': kind, 'id': tid}}


def _sip_options_udp(name: str, ip: str, port: int, domain: str, timeout: float = 2.0) -> dict:
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
        return _item(name, True, f'{ip}:{port} {first}', t0)
    except socket.timeout:
        return _item(name, False, f'{ip}:{port} OPTIONS 무응답 ({timeout:.0f}s)', t0)
    except Exception as e:
        return _item(name, False, f'{ip}:{port} {e}', t0)


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


def _oam_token(topology: Topology, nid: str, requester_token=None) -> dict:
    t0 = time.time()
    oam = topology.oam_ref()
    try:
        client = tester_target.OamClient(oam.url, tester_target.resolve_token(oam, requester_token))
        rows = client.deployments()
        try:
            dep = client.find_csp_deployment(oam.csp_deployment_id)
            return _item(f'{nid}:oam', True, f'{oam.url} 배포 {len(rows)}건 · CSP 배포 {dep}', t0)
        except tester_target.TargetError as e:
            return _item(f'{nid}:oam', False, f'{oam.url} 토큰 유효 · {e}', t0)
    except tester_target.TargetError as e:
        return _item(f'{nid}:oam', False, str(e), t0)


def check_topology(topology: Topology, topology_doc: dict, requester_token=None) -> List[dict]:
    items: List[dict] = []
    for nid, n in topology.target.nodes.items():
        ip = topology.node_ip(nid)
        if n.role == 'sip' and n.sip is not None:
            dom = n.sip.domains[0] if n.sip.domains else ''
            for lid, l in n.sip.listeners.items():
                lip = topology.listener_ip(nid, lid)
                it = (_sip_options_udp(f'{nid}:{lid}', lip, l.port, dom) if l.protocol == 'udp'
                      else _tcp_connect(f'{nid}:{lid}', lip, l.port, tls=(l.protocol == 'tls')))
                if not it['ok'] and l.edge == 'peering':
                    it['detail'] += ' — 피어링 접속점은 run 시드가 열고 복원이 닫을 수 있다(평상시 닫힘이면 정상)'
                    it['info'] = True
                items.append(it)
        elif n.role == 'subscriber' and n.api is not None:
            items.append(_tcp_connect(f'{nid}:api', ip, n.api.port, tls=n.api.tls))
        elif n.role == 'db' and n.db is not None:
            items.append(_tcp_connect(f'{nid}:db', ip, n.db.port, tls=False))
        elif n.role == 'oam':
            items.append(_oam_token(topology, nid, requester_token))
        elif n.role == 'media' and n.media is not None and n.media.control:
            it = _tcp_connect(f'{nid}:control', ip, n.media.control, tls=False)
            it['info'] = True   # CMP 제어는 UDP JSON — TCP 도달은 참고
            items.append(it)
        for it in items:
            if it['name'].startswith(nid + ':') and not it['target']['id']:
                it['target'] = {'kind': 'node', 'id': nid}
    for hid, h in topology.hosts.items():
        if h.ssh is not None:
            it = _tcp_connect(f'{hid}:ssh', h.ip, h.ssh.port, tls=False)
            it['target'] = {'kind': 'host', 'id': hid}
            if it['ok']:
                # 도달 뒤 실제 키 인증 + /proc 읽기(관측 명령과 같은 도구) — 실패하면 run 의 SSH 관측도 실패한다
                from services import tester_observe
                t0 = time.time()
                try:
                    out = tester_observe.ssh_run(h, 'echo "@T $(date +%s.%N) $(getconf CLK_TCK 2>/dev/null || echo 100)"; head -1 /proc/stat', timeout=10)
                    ok = '@T ' in out and 'cpu ' in out
                    it['detail'] += f" · ssh {h.ssh.user}@{h.ip} 인증·/proc 읽기 {'OK' if ok else '실패(출력 이상)'} {int((time.time() - t0) * 1000)} ms"
                    it['ok'] = it['ok'] and ok
                except Exception as e:
                    it['ok'] = False
                    it['detail'] += f' · ssh 실패: {e}'
            items.append(it)
    if not topology.target.nodes:
        t0 = time.time()
        items.append(_item('target', True, '대상 노드 없음 — 워커만 검사', t0))
    ws = tester_workers.discover(topology_doc)
    tester_workers.probe_all(ws)
    for w in ws:
        t0 = time.time()
        h = w.health
        if h is None:
            items.append(_item(f'worker_{w.name}', False, f'{w.url} {w.health_error}', t0, 'worker', w.name))
        else:
            med = h.get('media') or {}
            items.append(_item(f'worker_{w.name}', True,
                               f"{w.url} v{h.get('version')} 단말 {h.get('active_endpoints')}/{h.get('max_endpoints')} "
                               f"cpu {h.get('cpu_pct')}% skew {h.get('clock_skew_ms')} ms"
                               + (f" rtp {med.get('rtp_streams')}/{med.get('max_rtp_streams')}" if med else '')
                               + (f" · run {h.get('active_run')} 진행 중" if h.get('active_run') else ''), t0, 'worker', w.name))
    return items
