"""워커 클라이언트·발견 — 컨트롤러 → cims-tester-worker HTTP/JSON (test_instrument.md §6.1).

발견: 토폴로지 `workers`(호스트 참조 + 포트 — url 파생). 자기 base 배포 목록(`GET /api/v1/deployments`)에서의
자동 발견은 base 토큰 위임이 정리되면 여기에 보탠다.
표준 라이브러리만 쓴다 — 워커는 관리망 안 HTTP(평문) 이고 요청은 작고 드물다.
"""
from __future__ import annotations

import json
import socket
import time
import urllib.error
import urllib.request
from typing import List, Optional


class WorkerError(Exception):
    pass


class WorkerClient:
    def __init__(self, name: str, url: str, cpus: Optional[int] = None, host_id: Optional[str] = None,
                 media: Optional[dict] = None):
        self.name = name
        self.url = url.rstrip('/')
        self.cpus = cpus
        self.host_id = host_id          # 토폴로지 hosts 키
        self.media = media or {}        # 토폴로지 선언(samples·max_rtp_streams) — health 와 대조
        self.health: Optional[dict] = None
        self.health_error: Optional[str] = None

    @property
    def host(self) -> str:
        u = self.url.split('://', 1)[-1]
        return u.split('/', 1)[0].rsplit(':', 1)[0]

    def _req(self, method: str, path: str, body=None, timeout: float = 10):
        data = json.dumps(body).encode('utf-8') if body is not None else None
        req = urllib.request.Request(self.url + path, data=data, method=method)
        if data is not None:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read().decode('utf-8', 'replace')
                return r.status, (json.loads(raw) if raw else {})
        except urllib.error.HTTPError as e:
            raw = e.read().decode('utf-8', 'replace')
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, {'error': raw}
        except Exception as e:
            raise WorkerError(f'{self.name} {method} {path}: {e}')

    def probe(self) -> Optional[dict]:
        """GET /health — 실패해도 예외 대신 None (health_error 에 이유). 시계 오차(clock_skew_ms)를 덧붙인다."""
        try:
            t0 = time.time()
            st, doc = self._req('GET', '/health', timeout=5)
            if st != 200 or not isinstance(doc, dict):
                self.health, self.health_error = None, f'HTTP {st}'
                return None
            mid = (t0 + time.time()) / 2
            doc['clock_skew_ms'] = int(doc.get('clock_unix_ms', mid * 1000) - mid * 1000)
            self.health, self.health_error = doc, None
            return doc
        except WorkerError as e:
            self.health, self.health_error = None, str(e)
            return None

    def pool_create(self, doc: dict) -> dict:
        st, out = self._req('POST', '/pools', doc, timeout=60)
        if st not in (200, 201):
            raise WorkerError(f'{self.name} pool_create {st}: {out}')
        return out

    def run_start(self, doc: dict) -> dict:
        st, out = self._req('POST', '/runs', doc, timeout=30)
        if st != 202:
            raise WorkerError(f'{self.name} run_start {st}: {out}')
        return out

    def run_rate(self, run_id: str, rate: float) -> None:
        st, out = self._req('POST', f'/runs/{run_id}/rate', {'rate_saps': rate})
        if st != 200:
            raise WorkerError(f'{self.name} run_rate {st}: {out}')

    def run_stop(self, run_id: str, drain_s: int) -> None:
        st, out = self._req('POST', f'/runs/{run_id}/stop', {'drain_s': drain_s})
        if st not in (200, 202):
            raise WorkerError(f'{self.name} run_stop {st}: {out}')

    def run_get(self, run_id: str) -> Optional[dict]:
        st, out = self._req('GET', f'/runs/{run_id}')
        return out if st == 200 else None

    def local_ip_toward(self) -> str:
        """이 워커로 갈 때 쓰는 로컬 IP — 관측 스트림 목적지(RunStart.stream)를 워커 관점의 주소로 준다."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect((self.host, 7100))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return '127.0.0.1'

    def to_dict(self) -> dict:
        return {'name': self.name, 'url': self.url, 'host': self.host_id, 'cpus': self.cpus, 'media': self.media or None,
                'health': self.health, 'error': self.health_error, 'up': self.health is not None}


def discover(topology_doc: dict) -> List[WorkerClient]:
    """토폴로지(v2) 의 workers[] → 클라이언트. url = http://<hosts[host].ip>:<port> 파생(§4). host 가 없는 항목은 건너뛴다."""
    out = []
    hosts = topology_doc.get('hosts') or {}
    for i, w in enumerate(topology_doc.get('workers') or []):
        if not isinstance(w, dict):
            continue
        h = hosts.get(str(w.get('host') or ''))
        if not isinstance(h, dict) or not h.get('ip'):
            continue
        url = f"http://{h['ip']}:{int(w.get('port') or 7100)}"
        out.append(WorkerClient(str(w.get('name') or f'w{i + 1}'), url, w.get('cpus'), str(w.get('host')), w.get('media')))
    return out


def probe_all(workers: List[WorkerClient]) -> None:
    """워커 health 를 동시에 묻는다(각 5 s 시한) — 계획 미리보기·연결 검사가 워커 수만큼 기다리지 않도록."""
    import threading
    ths = [threading.Thread(target=w.probe, daemon=True) for w in workers]
    for t in ths:
        t.start()
    for t in ths:
        t.join(6)
