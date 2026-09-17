"""대상 관측 — run 동안 대상 호스트 자원을 모으고(stop_on.target_cpu_pct 원천), run 끝에 target_evidence 를 판정한다 (test_instrument.md §5).

원천 = 대상 OAM API(토폴로지 oam 노드, 토큰은 tester_target.resolve_token):
  · 호스트 자원  GET /api/v1/agents/{id}/metrics  → items[{ts, cpu_pct, mem_pct, load_avg}] (agent heartbeat, 약 3 s 간격, 최신이 앞)
      관측 대상 agent = 대상 노드의 감시 프로세스(`nodes.*.procs`)를 패키지/프로세스 이름으로 가진 배포들의 agent.
      프로세스별 CPU 는 heartbeat 에 없다 — 호스트 SSH 관측(`hosts.*.ssh`)이 그 원천이고 여기서는 다루지 않는다.
  · 증거          GET /api/v1/recordings · /api/v1/alerts · /api/v1/events  — run 창(started~ended)으로 자른 건수

발생기 측 관측이 1차 판정이고 target_evidence 는 2차다(§4): 증거가 어긋나면 run 은 fail 이지만, 원천을 읽지 못한 항목(`ok=None`)은
판정에서 빼고 참고(note)로만 남긴다 — 대상 OAM 이 죽었다고 시험이 실패로 뒤집히지는 않는다.
"""
from __future__ import annotations

import sqlite3
import threading
import time
from datetime import datetime
from typing import Dict, List, Optional

from services import tester_target
from services.tester_bus import publish
from services.tester_models import Scenario, Topology

POLL_S = 3.0


def _ts(s: str) -> Optional[float]:
    try:
        return datetime.fromisoformat(str(s).replace('Z', '')).timestamp()
    except Exception:
        return None


def observed_agents(client: tester_target.OamClient, topology: Topology) -> Dict[int, str]:
    """관측할 agent — {agent_id: 표시 이름}. 대상 노드 procs 와 이름이 맞는 배포의 agent."""
    procs = {p for n in topology.target.nodes.values() for p in (n.procs or [])}
    out: Dict[int, str] = {}
    for d in client.deployments():
        names = {str(d.get('package_name') or ''), str(d.get('process_name') or '')}
        if procs & names and d.get('agent_id') is not None:
            out[int(d['agent_id'])] = str(d.get('agent_name') or d['agent_id'])
    return out


class TargetObserver(threading.Thread):
    """run 하나의 대상 자원 수집기 — metrics.sqlite `target(t, agent, cpu_pct, mem_pct, load)` + SSE `target`."""

    def __init__(self, run_id: str, db_path: str, topology: Topology, requester_token: Optional[str]):
        super().__init__(name=f'tester-observe-{run_id}', daemon=True)
        self.run_id, self.db_path, self.topology, self._token = run_id, db_path, topology, requester_token
        self._stop_ev = threading.Event()
        self._lock = threading.Lock()
        self._recent: Dict[int, List[float]] = {}     # agent → 최근 cpu_pct(최대 3)
        self.agents: Dict[int, str] = {}
        self.note: Optional[str] = None
        self.samples = 0
        self.peak_cpu: Optional[float] = None

    @staticmethod
    def wanted(topology: Topology) -> bool:
        oam = topology.oam_ref()
        return oam is not None and ('agent_heartbeat' in (oam.observe or []) or 'oam_stats' in (oam.observe or []))

    def stop(self) -> None:
        self._stop_ev.set()

    def cpu_now(self) -> Optional[float]:
        """stop_on 판정값 — agent 별 최근 3 표본 평균의 최댓값(한 표본의 튐으로 멈추지 않게)."""
        with self._lock:
            vals = [sum(v) / len(v) for v in self._recent.values() if v]
        return max(vals) if vals else None

    def run(self) -> None:
        oam = self.topology.oam_ref()
        try:
            client = tester_target.OamClient(oam.url, tester_target.resolve_token(oam, self._token))
            self.agents = observed_agents(client, self.topology)
        except Exception as e:      # 관측 실패는 run 을 막지 않는다
            self.note = f'대상 관측 불가 — {e}'
            return
        if not self.agents:
            self.note = '대상 관측 불가 — 대상 노드 procs 와 맞는 배포가 대상 OAM 에 없다'
            return
        db = sqlite3.connect(self.db_path, check_same_thread=False)
        db.execute('CREATE TABLE IF NOT EXISTS target (t INTEGER, agent TEXT, cpu_pct REAL, mem_pct REAL, load REAL)')
        db.execute('CREATE INDEX IF NOT EXISTS target_t ON target (t)')
        last_seen: Dict[int, float] = {}
        while not self._stop_ev.is_set():
            for aid, name in self.agents.items():
                try:
                    st, out = client._req('GET', f'/api/v1/agents/{aid}/metrics', timeout=6)
                except Exception:
                    continue
                if st != 200:
                    continue
                items = sorted(((_ts(x.get('ts')), x) for x in (out or {}).get('items') or []), key=lambda p: p[0] or 0)
                for t, x in items:
                    if t is None or t <= last_seen.get(aid, time.time() - 2 * POLL_S) or x.get('cpu_pct') is None:
                        continue
                    last_seen[aid] = t
                    cpu = float(x['cpu_pct'])
                    load = None
                    try:
                        load = float(str(x.get('load_avg') or '').split(',')[0])
                    except ValueError:
                        pass
                    db.execute('INSERT INTO target VALUES (?,?,?,?,?)', (int(t), name, cpu, x.get('mem_pct'), load))
                    with self._lock:
                        r = self._recent.setdefault(aid, [])
                        r.append(cpu)
                        del r[:-3]
                        self.samples += 1
                        self.peak_cpu = cpu if self.peak_cpu is None else max(self.peak_cpu, cpu)
                    publish('target', {'run_id': self.run_id, 't': int(t), 'agent': name, 'cpu_pct': cpu, 'mem_pct': x.get('mem_pct')})
            db.commit()
            self._stop_ev.wait(POLL_S)
        db.commit()
        db.close()


def target_series(db_path: str) -> dict:
    """metrics.sqlite target 표 → {agents: {이름: {t[], cpu_pct[], mem_pct[]}}} — 결과 화면 대상 자원 차트."""
    out: Dict[str, dict] = {}
    try:
        db = sqlite3.connect(db_path)
        rows = db.execute('SELECT t, agent, cpu_pct, mem_pct FROM target ORDER BY t').fetchall()
        db.close()
    except sqlite3.Error:
        return {'agents': {}}
    for t, agent, cpu, mem in rows:
        a = out.setdefault(agent, {'t': [], 'cpu_pct': [], 'mem_pct': []})
        a['t'].append(t)
        a['cpu_pct'].append(cpu)
        a['mem_pct'].append(mem)
    return {'agents': out}


def _in_window(ts: Optional[float], t0: float, t1: float) -> bool:
    return ts is not None and t0 <= ts <= t1


def evaluate_evidence(scenario: Scenario, topology: Topology, started: float, ended: float,
                      requester_token: Optional[str]) -> List[dict]:
    """target_evidence → [{kind, code, min, max, observed, ok(True|False|None), why}] — 창은 시작 −2 s ~ 종료 +10 s(대상의 기록 지연)."""
    if not scenario.target_evidence:
        return []
    t0, t1 = started - 2, ended + 10
    oam = topology.oam_ref()
    client = None
    err = None
    if oam is None:
        err = '대상 oam 노드가 없다'
    else:
        try:
            client = tester_target.OamClient(oam.url, tester_target.resolve_token(oam, requester_token))
        except Exception as e:
            err = str(e)
    cache: Dict[str, object] = {}

    def fetch(path: str, key: str):
        if path not in cache:
            try:
                st, out = client._req('GET', path, timeout=15)
                cache[path] = (out or {}).get(key) if st == 200 else RuntimeError(f'{path} → {st}')
            except Exception as e:
                cache[path] = e
        v = cache[path]
        if isinstance(v, Exception):
            raise v
        return v or []

    res = []
    for ev in scenario.target_evidence:
        r = {'kind': ev.kind, 'code': ev.code, 'min': ev.min, 'max': ev.max, 'observed': None, 'ok': None, 'why': None}
        if client is None:
            r['why'] = f'판정 불가 — {err}'
            res.append(r)
            continue
        try:
            if ev.kind == 'recording_created':
                rows = fetch('/api/v1/recordings?limit=500', 'recordings')
                n = sum(1 for x in rows if _in_window(_ts(x.get('start_time')), t0, t1))
            elif ev.kind == 'alarm_raised':
                rows = fetch('/api/v1/alerts?days=2&limit=5000', 'events')
                n = sum(1 for x in rows if _in_window(_ts(x.get('ts')), t0, t1) and str(x.get('action') or 'raise') != 'close'
                        and str(x.get('severity') or '') != 'cleared' and (not ev.code or x.get('code') == ev.code))
            elif ev.kind == 'event_logged':
                rows = fetch('/api/v1/events?days=2&limit=5000', 'events')
                n = sum(1 for x in rows if _in_window(_ts(x.get('ts')), t0, t1) and (not ev.code or x.get('code') == ev.code))
            else:
                r['why'] = f'판정 불가 — {ev.kind} 는 대상 OAM API 에 원천이 없다(모듈 로그 조회는 호스트 SSH 관측 몫)'
                res.append(r)
                continue
            r['observed'] = n
            ok = (ev.min is None or n >= ev.min) and (ev.max is None or n <= ev.max)
            r['ok'] = ok
            if not ok:
                r['why'] = f'관측 {n} — 기대 ' + ' · '.join(x for x in (f'≥ {ev.min}' if ev.min is not None else '', f'≤ {ev.max}' if ev.max is not None else '') if x)
        except Exception as e:
            r['why'] = f'판정 불가 — {e}'
        res.append(r)
    return res
