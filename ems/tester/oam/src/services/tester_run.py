"""run 오케스트레이터 — 워커 배분·부하 프로파일 구동·관측 수집·요약/판정 (test_instrument.md §2·§5·§6).

  RunManager (프로세스 단일)
    ├ StreamServer   : Tester.WorkerStreamIp:Port TCP 수신 — 워커 JSONL(hello/agg/event/log) → 해당 run 의 Recorder
    ├ Recorder(run)  : <DataDir>/runs/<id>/{run.json, metrics.sqlite(1초 버킷), events.jsonl} + 병합 누계(요약용)
    │                  + SSE fan-out (tester_bus: stream=agg|events|runs)
    └ Driver(run)    : 스레드 — 컴파일 → 풀 생성 → run 시작 → 프로파일(constant/soak/step/ramp/burst · 단발) →
                       stop_on/IHS 판정 → 중단(drain) → 요약·verdict → 색인 저장

verdict: pass = 모든 expect 만족 · fail = 기대치 미달 또는 stop_on 발동 · aborted = 운영자 중단 · error = 워커/컴파일 오류.
백분위는 워커 히스토그램(로그 상한 버킷)에서 상한값으로 근사한다 — 기대치 판정은 보수적(상한 ≤ 목표)이다.
"""
from __future__ import annotations

import json
import os
import socket
import sqlite3
import threading
import time
import traceback
import uuid
from datetime import datetime
from typing import Dict, List, Optional

from services import tester_store as store
from services import tester_samples
from services import tester_workers, tester_compile, tester_target, tester_observe, tester_fixtures
from services.tester_bus import publish
from services.tester_models import RunRecord, RunRequest, LoadProfile, RATIO_METRICS, LOWER_BETTER_RATIOS

_LOG = None


def _log(level: str, msg: str) -> None:
    if _LOG is not None:
        getattr(_LOG, {'info': 'log_info', 'warn': 'log_warning', 'error': 'log_error'}.get(level, 'log_info'))(msg)


def _now_iso() -> str:
    return datetime.now().isoformat(timespec='seconds')


# ──────────────────────────────────────────────────────────────────────────
#  히스토그램 병합·백분위 근사
# ──────────────────────────────────────────────────────────────────────────

def _bucket_key(k: str) -> float:
    return float('inf') if k == 'inf' else float(k)


class Hist:
    __slots__ = ('count', 'sum', 'min', 'max', 'buckets')

    def __init__(self):
        self.count = 0
        self.sum = 0.0
        self.min = None
        self.max = None
        self.buckets: Dict[str, int] = {}

    def merge(self, h: dict) -> None:
        c = int(h.get('count') or 0)
        if c <= 0:
            return
        self.count += c
        self.sum += float(h.get('sum') or 0)
        if h.get('min') is not None:
            self.min = float(h['min']) if self.min is None else min(self.min, float(h['min']))
        if h.get('max') is not None:
            self.max = float(h['max']) if self.max is None else max(self.max, float(h['max']))
        for k, v in (h.get('buckets') or {}).items():
            self.buckets[k] = self.buckets.get(k, 0) + int(v)

    def percentile(self, p: float) -> Optional[float]:
        """p ∈ (0,1] — 누적 수가 p·count 를 넘는 첫 버킷의 상한. inf 버킷이면 max."""
        if self.count == 0:
            return None
        target = p * self.count
        acc = 0
        for k in sorted(self.buckets, key=_bucket_key):
            acc += self.buckets[k]
            if acc >= target:
                return self.max if k == 'inf' else min(_bucket_key(k), self.max if self.max is not None else _bucket_key(k))
        return self.max

    def to_dict(self) -> dict:
        return {'count': self.count, 'mean': (self.sum / self.count) if self.count else None,
                'min': self.min, 'max': self.max,
                'p50': self.percentile(0.5), 'p95': self.percentile(0.95), 'p99': self.percentile(0.99)}


def run_series(run_id: str, limit: int = 7200) -> dict:
    """<DataDir>/runs/<id>/metrics.sqlite 의 1초 버킷을 초 단위로 합쳐 열(column) 형태로 돌려준다.
    카운터 = 워커 합, 게이지 = 합(cpu_pct 는 최대), 타이머 = 버킷 병합 뒤 p95 — 콘솔 결과 화면의 시간축 차트와
    비교 화면이 쓴다. 파일이 없으면(진행 전·삭제됨) 빈 시계열."""
    path = os.path.join(store.run_dir(run_id), 'metrics.sqlite')
    out = {'t': [], 'counters': {}, 'gauges': {}, 'timers': {}}
    if not os.path.isfile(path):
        return out
    db = sqlite3.connect(path)
    try:
        rows = db.execute('SELECT t, worker, counters, gauges, timers FROM agg ORDER BY t').fetchall()
    finally:
        db.close()
    by_t: Dict[int, dict] = {}
    for t, _w, c, g, tm in rows:
        b = by_t.setdefault(int(t), {'c': {}, 'g': {}, 'tm': {}})
        try:
            for k, v in (json.loads(c) or {}).items():
                b['c'][k] = b['c'].get(k, 0) + int(v)
            for k, v in (json.loads(g) or {}).items():
                b['g'][k] = max(b['g'].get(k, 0), float(v)) if k == 'cpu_pct' else b['g'].get(k, 0) + float(v)
            for k, h in (json.loads(tm) or {}).items():
                b['tm'].setdefault(k, Hist()).merge(h)
        except Exception:
            continue
    ts = sorted(by_t)[-max(1, limit):]
    ckeys = sorted({k for t in ts for k in by_t[t]['c']})
    gkeys = sorted({k for t in ts for k in by_t[t]['g']})
    tkeys = sorted({k for t in ts for k in by_t[t]['tm']})
    out['t'] = ts
    out['counters'] = {k: [by_t[t]['c'].get(k, 0) for t in ts] for k in ckeys}
    out['gauges'] = {k: [by_t[t]['g'].get(k) for t in ts] for k in gkeys}
    out['timers'] = {k: {'p95': [(by_t[t]['tm'][k].percentile(0.95) if k in by_t[t]['tm'] else None) for t in ts],
                         'count': [(by_t[t]['tm'][k].count if k in by_t[t]['tm'] else 0) for t in ts]} for k in tkeys}
    return out


def run_hist(run_id: str, timer: str) -> Optional[dict]:
    """metrics.sqlite 전 구간의 지연 지표 버킷 분포(로그 상한 1·2·5·…) — 결과 화면 행 펼침 히스토그램. 없으면 None."""
    path = os.path.join(store.run_dir(run_id), 'metrics.sqlite')
    if not os.path.isfile(path):
        return None
    db = sqlite3.connect(path)
    try:
        rows = db.execute('SELECT timers FROM agg').fetchall()
    finally:
        db.close()
    h = Hist()
    for (tm,) in rows:
        try:
            d = (json.loads(tm) or {}).get(timer)
        except Exception:
            continue
        if d:
            h.merge(d)
    if h.count == 0:
        return None
    buckets = [{'ub': (None if k == 'inf' else _bucket_key(k)), 'count': v} for k, v in sorted(h.buckets.items(), key=lambda kv: _bucket_key(kv[0]))]
    return {'timer': timer, **h.to_dict(), 'buckets': buckets}


def run_sip_dumps(run_id: str) -> List[dict]:
    """runs/<id>/sip/ 의 덤프 목록 — {call_id(파일 이름 기준), bytes, messages}. 결과 화면의 'SIP 덤프' 목록."""
    d = os.path.join(store.run_dir(run_id), 'sip')
    out = []
    if os.path.isdir(d):
        for fn in sorted(os.listdir(d)):
            if not fn.endswith('.log'):
                continue
            p = os.path.join(d, fn)
            n = 0
            try:
                with open(p, 'r', encoding='utf-8', errors='replace') as f:
                    for line in f:
                        if line.startswith('>>> ') or line.startswith('<<< '):
                            n += 1
            except OSError:
                continue
            out.append({'call_id': fn[:-4], 'bytes': os.path.getsize(p), 'messages': n})
    return out


def run_call_events(run_id: str, call_id: str, limit: int = 200) -> List[dict]:
    """events.jsonl 에서 Call-ID 로 고른 실패 건 — SIP 사다리 드로어의 원천(워커 SIP 덤프 이전은 후속)."""
    p = os.path.join(store.run_dir(run_id), 'events.jsonl')
    out: List[dict] = []
    if not os.path.isfile(p):
        return out
    with open(p, 'r', encoding='utf-8') as f:
        for ln in f:
            try:
                rec = json.loads(ln)
            except Exception:
                continue
            if str(rec.get('call_id') or '') == call_id:
                out.append(rec)
                if len(out) >= limit:
                    break
    return out


# ──────────────────────────────────────────────────────────────────────────
#  Recorder — run 하나의 저장·누계·SSE
# ──────────────────────────────────────────────────────────────────────────

class Recorder:
    def __init__(self, run_id: str):
        self.run_id = run_id
        self.dir = store.run_dir(run_id)
        os.makedirs(os.path.join(self.dir, 'sip'), exist_ok=True)
        self._lock = threading.Lock()
        self._db = sqlite3.connect(os.path.join(self.dir, 'metrics.sqlite'), check_same_thread=False)
        self._db.execute('CREATE TABLE IF NOT EXISTS agg (t INTEGER, worker TEXT, counters TEXT, gauges TEXT, timers TEXT)')
        self._db.execute('CREATE INDEX IF NOT EXISTS agg_t ON agg (t)')
        self._events = open(os.path.join(self.dir, 'events.jsonl'), 'a', encoding='utf-8')
        self.counters: Dict[str, int] = {}
        self.timers: Dict[str, Hist] = {}
        self.gauges_by_worker: Dict[str, dict] = {}
        self.events_n = 0
        self.sip_dumps = 0
        self.workers_seen: List[str] = []
        self.window: List[dict] = []      # 최근 버킷(카운터만) — step/IHS 창 판정용
        self.last_t = 0

    def on_record(self, rec: dict) -> None:
        kind = rec.get('kind')
        with self._lock:
            w = str(rec.get('worker') or '')
            if w and w not in self.workers_seen:
                self.workers_seen.append(w)
            if kind == 'agg':
                c = rec.get('counters') or {}
                g = rec.get('gauges') or {}
                tm = rec.get('timers') or {}
                for k, v in c.items():
                    self.counters[k] = self.counters.get(k, 0) + int(v)
                for k, h in tm.items():
                    self.timers.setdefault(k, Hist()).merge(h)
                self.gauges_by_worker[w] = g
                t = int(rec.get('t') or 0)
                self.last_t = max(self.last_t, t)
                self.window.append({'t': t, 'counters': c})
                if len(self.window) > 3600:
                    del self.window[:len(self.window) - 3600]
                self._db.execute('INSERT INTO agg VALUES (?,?,?,?,?)',
                                 (t, w, json.dumps(c), json.dumps(g), json.dumps(tm)))
                self._db.commit()
                publish('agg', {'run_id': self.run_id, 't': t, 'worker': w, 'counters': c, 'gauges': g,
                                'timers': {k: {'count': h.get('count'), 'sum': h.get('sum'), 'max': h.get('max')} for k, h in tm.items()}})
            elif kind == 'event':
                self.events_n += 1
                self._events.write(json.dumps(rec, ensure_ascii=False) + '\n')
                self._events.flush()
                publish('events', {'run_id': self.run_id, **rec})
            elif kind == 'sip':
                self._write_sip(rec)
            elif kind in ('log', 'hello'):
                publish('runs', {'run_id': self.run_id, 'kind': kind, 'worker': w,
                                 'msg': rec.get('msg') or rec.get('version')})

    SIP_DUMP_MAX = 2000      # run 하나의 덤프 파일 수 상한(워커 Sip.DumpMax × 워커 수의 안전판)

    def _write_sip(self, rec: dict) -> None:
        """워커 SIP 덤프 → runs/<id>/sip/<call_id>.log (사람이 읽는 사다리 원문 — 방향·시각·transport·상대 + 메시지).
        같은 Call-ID 를 두 워커가 올리면(양 끝이 다른 워커) 한 파일에 이어 적는다."""
        call_id = str(rec.get('call_id') or '')
        msgs = rec.get('messages') or []
        if not call_id or not msgs:
            return
        p = os.path.join(self.dir, 'sip', store._safe_name(call_id) + '.log')
        if not os.path.exists(p):
            if self.sip_dumps >= self.SIP_DUMP_MAX:
                return
            self.sip_dumps += 1
        with open(p, 'a', encoding='utf-8') as f:
            f.write(f"# call_id {call_id} · worker {rec.get('worker')} · instance {rec.get('instance')} · {len(msgs)} messages\n")
            for m in msgs:
                t = float(m.get('t') or 0)
                ts = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(t)) + f'.{int((t % 1) * 1000):03d}'
                text = str(m.get('text') or '').replace('\r\n', '\n').rstrip('\n')
                first = text.split('\n', 1)[0]
                # 블록 머리 = `>>> `(송신)/`<<< `(수신) + 요청·상태 줄 — 콘솔 SIP 드로어가 이 줄로 사다리를 그린다
                arrow = '>>>' if m.get('dir') == 'tx' else '<<<'
                f.write(f"{arrow} {first}   · {ts} {m.get('transport')} {m.get('peer')} · {rec.get('worker')}\n")
                f.write(text + '\n\n')

    def gauges_sum(self) -> dict:
        with self._lock:
            out: Dict[str, float] = {}
            for g in self.gauges_by_worker.values():
                for k, v in g.items():
                    if k == 'cpu_pct':
                        out[k] = max(out.get(k, 0), float(v))
                    else:
                        out[k] = out.get(k, 0) + float(v)
            return out

    def window_counters(self, since_t: int) -> dict:
        with self._lock:
            out: Dict[str, int] = {}
            for b in self.window:
                if b['t'] >= since_t:
                    for k, v in b['counters'].items():
                        out[k] = out.get(k, 0) + int(v)
            return out

    def snapshot(self) -> dict:
        with self._lock:
            return {'counters': dict(self.counters), 'timers': {k: h.to_dict() for k, h in self.timers.items()},
                    'events': self.events_n, 'workers': list(self.workers_seen), 'last_t': self.last_t}

    def close(self) -> None:
        with self._lock:
            try:
                self._db.commit()
                self._db.close()
            except Exception:
                pass
            try:
                self._events.close()
            except Exception:
                pass


# ──────────────────────────────────────────────────────────────────────────
#  StreamServer — 워커 → 컨트롤러 TCP JSONL
# ──────────────────────────────────────────────────────────────────────────

class StreamServer:
    def __init__(self, ip: str, port: int, dispatch):
        self.ip, self.port, self.dispatch = ip, port, dispatch
        self._srv: Optional[socket.socket] = None
        self._stop = False
        self._thread: Optional[threading.Thread] = None
        self.connections = 0

    def start(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.ip, self.port))
        s.listen(64)
        s.settimeout(0.5)
        self._srv = s
        self._thread = threading.Thread(target=self._accept, name='tester-stream', daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop = True
        try:
            if self._srv:
                self._srv.close()
        except Exception:
            pass

    def _accept(self) -> None:
        while not self._stop:
            try:
                c, addr = self._srv.accept()
            except socket.timeout:
                continue
            except Exception:
                if self._stop:
                    return
                continue
            self.connections += 1
            threading.Thread(target=self._serve, args=(c, addr), daemon=True).start()

    def _serve(self, c: socket.socket, addr) -> None:
        buf = b''
        c.settimeout(60)
        try:
            while not self._stop:
                try:
                    d = c.recv(65536)
                except socket.timeout:
                    continue
                if not d:
                    break
                buf += d
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    if not line.strip():
                        continue
                    try:
                        rec = json.loads(line.decode('utf-8', 'replace'))
                    except Exception:
                        continue
                    try:
                        self.dispatch(rec, addr)
                    except Exception as e:
                        _log('error', f'[stream] dispatch error: {e}')
        finally:
            try:
                c.close()
            except Exception:
                pass


# ──────────────────────────────────────────────────────────────────────────
#  Driver — run 하나의 수명주기
# ──────────────────────────────────────────────────────────────────────────

class RunDriver(threading.Thread):
    def __init__(self, mgr: 'RunManager', run_id: str, req: RunRequest, scenario, topology, topology_doc,
                 profile: Optional[LoadProfile], profile_name: Optional[str], topology_name: str,
                 requester_token: Optional[str] = None):
        super().__init__(name=f'tester-run-{run_id}', daemon=True)
        self._requester_token = requester_token      # 대상 OAM 호출용(환경변수 토큰이 없을 때) — 메모리에서만, 기록하지 않는다
        self.mgr, self.run_id, self.req = mgr, run_id, req
        self.scenario, self.topology, self.topology_doc = scenario, topology, topology_doc
        self.profile, self.profile_name, self.topology_name = profile, profile_name, topology_name
        self.rec = Recorder(run_id)
        self.workers: List[tester_workers.WorkerClient] = []
        self.plan: Optional[dict] = None
        self.state = 'starting'
        self.verdict = 'running'
        self.rate = 0.0
        self.doc_rate: Optional[float] = None
        self.stop_reason: Optional[str] = None
        self._stop_req = threading.Event()
        self._hold = threading.Event()        # 단계 고정 — 프로파일 시계 정지(율 유지)
        self.held_s = 0.0
        self.started_at = _now_iso()
        self._t_started = time.time()
        self.ended_at: Optional[str] = None
        self.observer: Optional[tester_observe.TargetObserver] = None
        self.ssh_observer: Optional[tester_observe.SshObserver] = None    # hosts.*.ssh — 호스트/프로세스 자원 + log_errors
        self.evidence_results: List[dict] = []
        self.notes: List[str] = []
        self.expect_results: List[dict] = []
        self.step_log: List[dict] = []
        self.seeder: Optional[tester_target.CspSeeder] = None
        self.fixtures: Optional[tester_fixtures.FixtureApplier] = None   # 시나리오 fixtures — 대상 CSC 관리 API 로 적용, run 끝에 복원
        self.target_build: Optional[str] = None

    # ── 외부 제어
    def request_stop(self, reason: str = 'operator') -> None:
        self.stop_reason = reason
        self._stop_req.set()

    def set_hold(self, on: bool) -> None:
        """단계 고정(hold) — step/ramp/constant 의 남은 시간 계산을 멈춘다. 율은 그대로라 운영자가 현 단계를 오래 볼 수 있다."""
        if on:
            self._hold.set()
        else:
            self._hold.clear()
        self.step_log.append({'t': time.time(), 'rate': self.rate, 'event': 'hold' if on else 'resume'})
        self._publish_state({'hold': on})

    def set_rate(self, rate: float) -> None:
        self.rate = rate
        for w in self.workers:
            try:
                w.run_rate(self.run_id, rate * self.plan['workers'][w.name]['share'])
            except tester_workers.WorkerError as e:
                self.notes.append(str(e))
        self._publish_state()

    # ── 본체
    def run(self) -> None:
        try:
            self._prepare()
            self._drive()
        except tester_compile.CompileError as e:
            self.verdict = 'error'
            self.notes.append(f'compile: {e}')
            _log('error', f'[run {self.run_id}] compile error: {e}')
        except tester_workers.WorkerError as e:
            self.verdict = 'error'
            self.notes.append(f'worker: {e}')
            _log('error', f'[run {self.run_id}] worker error: {e}')
        except tester_target.TargetError as e:
            self.verdict = 'error'
            self.notes.append(f'target: {e}')
            _log('error', f'[run {self.run_id}] target error: {e}')
        except Exception as e:
            self.verdict = 'error'
            self.notes.append(f'{e}')
            _log('error', f'[run {self.run_id}] error: {e}\n{traceback.format_exc()}')
        finally:
            try:
                self._finish()
            except Exception as e:
                _log('error', f'[run {self.run_id}] finish error: {e}\n{traceback.format_exc()}')

    def _publish_state(self, extra: Optional[dict] = None) -> None:
        rec = {'run_id': self.run_id, 'state': self.state, 'verdict': self.verdict, 'rate_saps': self.rate,
               'scenario_id': self.scenario.id, 'profile': self.profile_name}
        if extra:
            rec.update(extra)
        publish('runs', rec)

    def _prepare(self) -> None:
        self.workers = tester_workers.discover(self.topology_doc)
        if not self.workers:
            raise tester_compile.CompileError('토폴로지에 workers 가 없다')
        down = []
        for w in self.workers:
            h = w.probe()
            if h is None:
                down.append(f'{w.name}({w.url}): {w.health_error}')
            elif abs(int(h.get('clock_skew_ms') or 0)) > 50:
                self.notes.append(f'{w.name}: 시계 오차 {h.get("clock_skew_ms")} ms (> 50) — 지연 지표가 흔들릴 수 있다')
            if h is not None and h.get('active_run'):
                raise tester_workers.WorkerError(f'{w.name}: 다른 run 이 진행 중 ({h.get("active_run")})')
        if down:
            raise tester_workers.WorkerError('워커 미응답: ' + '; '.join(down))
        stream_port = int(((self.mgr.config.get('Tester') or {}).get('WorkerStreamPort')) or 7110)

        def stream_for(w):
            adv = ((self.mgr.config.get('Tester') or {}).get('WorkerStreamAdvertiseIp') or '').strip()
            ip = adv or w.local_ip_toward()
            return f'{ip}:{stream_port}'

        self.plan = tester_compile.compile_run(
            self.run_id, self.scenario, self.topology, self.topology_doc, self.profile,
            self.req.bindings, self.workers, stream_for, self.req.instances, self.req.rate_saps)
        # 역할이 전부 해석되는 워커만 참여(피어 풀은 그 워커 하나에 고정) — 나머지 워커는 건드리지 않는다
        self.workers = [w for w in self.workers if w.name in self.plan['workers']]
        for n in self.plan.get('resolve_notes') or []:
            self.notes.append(f'제외 워커: {n}')
        # 용량 검사
        for w in self.workers:
            need = sum(len(p['identities']) for p in self.plan['workers'][w.name]['pools'])
            cap = int((w.health or {}).get('max_endpoints') or 0)
            if cap and need > cap:
                raise tester_compile.CompileError(f'{w.name}: 필요 단말 {need} > 용량 {cap}')
        self.rate = float(self.plan['rate_total'])
        self.state = 'provisioning'
        self.target_build = tester_target.csp_build(self.topology, self._requester_token) if self.topology.target.kind == 'cims' else None
        self._publish_state()
        # 피어 풀 — 대상 CSP 컬렉션 시드(접속점·remote_nodes·routes(inbound_auth)·route_sets·rules·routing_policies·acl scope=route) → run 끝에 복원
        if self.plan.get('peer_pools'):
            used = set(self.plan['peer_pools'])
            self.seeder = tester_target.CspSeeder.for_run(self.topology, used, self._requester_token)
            if self.seeder is not None:
                applied = self.seeder.apply()
                self.notes.append(f'csp seed(dep {self.seeder.dep_id}, ln={self.seeder.local_node_ref}): '
                                  + ', '.join(f'{k}+{v}' for k, v in applied.items()))
                time.sleep(1.5)   # SIGUSR1 reload — 리스너 bind·라우팅 캐시 반영 여유
        # 시험 픽스처(§4) — 전화 그룹·역할·가입 서비스를 대상의 운영 프로비저닝 경로(CSC 관리 API)로 적용·확인. 풀 생성(REGISTER) 앞 — 픽업 축은 다음 등록부터
        if self.plan.get('fixtures'):
            self.fixtures = tester_fixtures.FixtureApplier.for_run(self.topology, self.plan['fixtures'], self._requester_token)
            try:
                self.fixtures.apply()
                bad = self.fixtures.verify()
            except tester_fixtures.FixtureError as e:
                raise tester_target.TargetError(str(e))
            if bad:
                self.fixtures.revert()
                raise tester_target.TargetError('픽스처 확인 실패: ' + '; '.join(bad))
            self.notes.append('fixtures applied: ' + ' · '.join(tester_fixtures.summarize(self.plan['fixtures'])))
            self.notes.extend(self.fixtures.notes)
        # 샘플 라이브러리 사본(§4) — 계획이 참조한 샘플 파일이 워커에 없으면(이름+크기) 여기서 밀어 넣는다. 워커의 run 시작 검사(sample_missing)가 뒤를 받친다
        tester_samples.sync_for_run(self.workers, self.plan.get('samples') or {}, notes=self.notes)
        for w in self.workers:
            for p in self.plan['workers'][w.name]['pools']:
                w.pool_create(p)
        # 대상 관측(§5) — oam 노드 observe 에 agent_heartbeat/oam_stats 가 있으면 호스트 자원을 run 동안 모은다
        if tester_observe.TargetObserver.wanted(self.topology):
            self.observer = tester_observe.TargetObserver(self.run_id, os.path.join(self.rec.dir, 'metrics.sqlite'),
                                                          self.topology, self._requester_token)
            self.observer.start()
        # 호스트 SSH 관측(§5) — ssh 가 있는 호스트의 CPU/메모리 + 대상 노드 procs 의 프로세스별 CPU/RSS, run 끝에 nodes.*.logs 의 ERROR 증분
        if tester_observe.SshObserver.wanted(self.topology):
            self.ssh_observer = tester_observe.SshObserver(self.run_id, os.path.join(self.rec.dir, 'metrics.sqlite'), self.topology)
            self.ssh_observer.start()
        for w in self.workers:
            w.run_start(self.plan['workers'][w.name]['run'])
        self.state = 'running'
        self.step_log.append({'t': time.time(), 'rate': self.rate, 'event': 'start'})
        self._publish_state({'workers': [w.name for w in self.workers], 'plan': {
            'roles': self.plan['roles'], 'identities': self.plan['identities'], 'max_instances': self.plan['max_instances']}})
        _log('info', f'[run {self.run_id}] started — scenario={self.scenario.id} profile={self.profile_name} '
                     f'workers={[w.name for w in self.workers]} rate={self.rate} plan_roles={self.plan["roles"]}')

    def _workers_all_stopped(self) -> bool:
        for w in self.workers:
            g = w.run_get(self.run_id)
            if g is None:
                continue
            if g.get('state') not in ('stopped',):
                return False
        return True

    def _wait(self, seconds: float, poll: float = 1.0) -> bool:
        """seconds 동안 대기 — 중단 요청·stop_on 발동·(단발) 워커 종료면 False. hold 중에는 시계가 멈춘다."""
        end = time.time() + seconds
        while time.time() < end:
            if self._stop_req.is_set():
                return False
            if self._check_stop_on():
                return False
            if self.plan.get('max_instances') and self._workers_all_stopped():
                return False
            if self._hold.is_set():
                t0 = time.time()
                time.sleep(min(poll, 0.5))
                dt = time.time() - t0
                end += dt
                self.held_s += dt
                continue
            time.sleep(min(poll, max(0.05, end - time.time())))
        return not self._stop_req.is_set()

    def _check_stop_on(self) -> bool:
        if self.profile is None:
            return False
        so = self.profile.stop_on
        win = self.rec.window_counters(int(time.time()) - 60)
        attempts = win.get('attempts', 0)
        if attempts < 10:
            return False
        if so.csp_5xx_pct is not None:
            five = sum(v for k, v in win.items() if k.startswith('codes.5'))
            pct = 100.0 * five / attempts
            if pct > so.csp_5xx_pct:
                self.request_stop(f'stop_on csp_5xx_pct {pct:.2f} > {so.csp_5xx_pct}')
                self.verdict = 'fail'
                return True
        if so.ser_pct_min is not None:
            ser = 100.0 * win.get('sessions', 0) / attempts
            if ser < so.ser_pct_min:
                self.request_stop(f'stop_on ser_pct {ser:.2f} < {so.ser_pct_min}')
                self.verdict = 'fail'
                return True
        if so.target_cpu_pct is not None:
            vals = [o.cpu_now() for o in (self.observer, self.ssh_observer) if o is not None]
            vals = [v for v in vals if v is not None]
            cpu = max(vals) if vals else None
            if cpu is not None and cpu > so.target_cpu_pct:
                self.request_stop(f'stop_on target_cpu_pct {cpu:.1f} > {so.target_cpu_pct}')
                self.verdict = 'fail'
                return True
            if self.observer is None and self.ssh_observer is None and not any(n.startswith('target_cpu:') for n in self.notes):
                self.notes.append('target_cpu: 대상 관측이 꺼져 있다(oam 노드 observe 에 agent_heartbeat 없음·hosts.*.ssh 없음) — stop_on.target_cpu_pct 미적용')
        return False

    def _drive(self) -> None:
        p = self.profile
        if p is None:
            # 단발 — 워커가 max_instances 를 다 내고 스스로 닫을 때까지 (상한: 인스턴스 × (ht+40s))
            ht = int(self.plan['bindings'].get('ht') or 0)
            limit = 60 + int(self.plan['max_instances'] or 1) * (ht + 40) / max(1.0, self.rate)
            self._wait(limit)
            return
        if p.model in ('constant', 'soak'):
            self._wait(float(p.duration_s))
            return
        if p.model == 'step':
            rate = float(p.start)
            prev_fail = self.rec.snapshot()['counters']
            while True:
                self.step_log.append({'t': time.time(), 'rate': rate, 'event': 'step'})
                self._publish_state({'step_rate': rate})
                t0 = int(time.time())
                if not self._wait(float(p.hold_s)):
                    return
                win = self.rec.window_counters(t0)
                attempts = win.get('attempts', 0)
                bad = win.get('failed', 0) + win.get('skipped', 0)
                ihs = 100.0 * bad / attempts if attempts else 0.0
                self.step_log[-1].update({'attempts': attempts, 'ihs_pct': ihs})
                if attempts and ihs > p.ihs_threshold_pct:
                    self.notes.append(f'step {rate} saps: IHS {ihs:.2f}% > {p.ihs_threshold_pct}% — DOC = {self.doc_rate}')
                    self.stop_reason = 'ihs'
                    break
                self.doc_rate = rate
                if rate >= float(p.max):
                    break
                rate = min(float(p.max), rate + float(p.step))
                self.set_rate(rate)
            return
        if p.model == 'ramp':
            start, mx, ramp_s = float(p.start), float(p.max), float(p.ramp_s)
            t0 = time.time()
            while time.time() - t0 < ramp_s:
                frac = (time.time() - t0) / ramp_s
                self.set_rate(start + (mx - start) * frac)
                if not self._wait(5.0):
                    return
            self.set_rate(mx)
            self._wait(float(p.hold_s))
            return
        if p.model == 'burst':
            t0 = time.time()
            while time.time() - t0 < float(p.duration_s):
                self.set_rate(float(p.burst_size))
                if not self._wait(1.0):
                    return
                self.set_rate(0.0)
                if not self._wait(max(0.0, float(p.burst_interval_s) - 1.0)):
                    return
            return

    def _finish(self) -> None:
        self.state = 'stopping'
        self._publish_state()
        drain = 5 + int(self.plan['bindings'].get('ht') or 0) if self.plan else 5
        for w in self.workers:
            try:
                w.run_stop(self.run_id, drain)
            except tester_workers.WorkerError as e:
                self.notes.append(str(e))
        end = time.time() + drain + 15
        while time.time() < end and self.workers:
            try:
                if self._workers_all_stopped():
                    break
            except tester_workers.WorkerError:
                break
            time.sleep(1)
        time.sleep(1.5)   # 마지막 agg 도착 여유
        if self.seeder is not None and self.seeder.applied:
            errs = self.seeder.restore()
            self.notes.append('csp seed restored' if not errs else 'csp seed restore FAILED: ' + '; '.join(errs))
        if self.fixtures is not None and self.fixtures.applied:
            errs = self.fixtures.revert()
            self.notes.append('fixtures reverted' if not errs else 'fixtures revert FAILED: ' + '; '.join(errs))
        self.ended_at = _now_iso()
        t_ended = time.time()
        if self.observer is not None:
            self.observer.stop()
            self.observer.join(timeout=10)
            if self.observer.note:
                self.notes.append(self.observer.note)
        log_errors = None
        if self.ssh_observer is not None:
            self.ssh_observer.stop()
            self.ssh_observer.join(timeout=15)
            try:
                log_errors = self.ssh_observer.log_errors()
            except Exception as e:
                self.notes.append(f'log_errors 조회 실패: {e}')
            if self.ssh_observer.note:
                self.notes.append(self.ssh_observer.note)
        snap = self.rec.snapshot()
        summary = self._summary(snap)
        peaks = [o.peak_cpu for o in (self.observer, self.ssh_observer) if o is not None and o.peak_cpu is not None]
        if peaks:
            summary['target_cpu_peak_pct'] = max(peaks)
        if self.ssh_observer is not None and self.ssh_observer.proc_peak:
            summary['target_proc_peak_pct'] = {k: round(v, 1) for k, v in self.ssh_observer.proc_peak.items()}
            summary['target_rss_delta_mb'] = self.ssh_observer.rss_delta_mb()   # 소크 누수 판정 원천 — 처음↔끝 RSS 차
            slope = self.ssh_observer.rss_slope_mb_per_h()
            if slope:
                summary['target_rss_slope_mb_per_h'] = slope                    # 최소제곱 기울기(MB/h) — 소크 보고서의 누수 추정
            fd = self.ssh_observer.fd_delta()
            if fd:
                summary['target_fd_delta'] = fd                                   # 열린 fd 수 처음↔끝 차(소켓·파일 누수)
        if log_errors is not None:
            summary['target_log_errors'] = log_errors
        # 대상 증거(2차 판정) — 운영자 중단·오류 run 은 판정하지 않는다
        if self.scenario.target_evidence and self.verdict == 'running' and not (
                self._stop_req.is_set() and (self.stop_reason or '').startswith('operator')):
            try:
                self.evidence_results = tester_observe.evaluate_evidence(
                    self.scenario, self.topology, self._t_started, t_ended, self._requester_token, log_errors,
                    rss_delta=(self.ssh_observer.rss_delta_mb() if self.ssh_observer is not None and self.ssh_observer.rss_first else None),
                    fd_delta=(self.ssh_observer.fd_delta() if self.ssh_observer is not None and self.ssh_observer.fd_first else None))
            except Exception as e:
                self.notes.append(f'target_evidence 판정 실패: {e}')
        if self.verdict == 'running':
            if self._stop_req.is_set() and (self.stop_reason or '').startswith('operator'):
                self.verdict = 'aborted'
            else:
                # pass = 기대치 전부 만족 + 실패 인스턴스 0 (단계가 끝까지 못 간 시도는 기대치 유무와 무관하게 실패다)
                #        + 시도 ≥ 1. 단말 부족 skipped 는 실패가 아니라 요약·IHS 에만 반영된다.
                #        + 대상 증거(2차)에 어긋남 없음 — 원천을 못 읽은 증거(ok=None)는 판정에서 빼고 참고로만 남긴다.
                ev_bad = [r for r in self.evidence_results if r.get('ok') is False]
                self.verdict = 'pass' if (all(r['ok'] for r in self.expect_results) and summary.get('attempts', 0) > 0
                                          and summary.get('failed', 0) == 0 and not ev_bad) else 'fail'
                for r in ev_bad:
                    self.notes.append(f"target_evidence {r['kind']}{'(' + r['code'] + ')' if r.get('code') else ''}: {r.get('why')}")
                for r in self.evidence_results:
                    if r.get('ok') is None:
                        self.notes.append(f"target_evidence {r['kind']}: {r.get('why')}")
                if summary.get('failed', 0) > 0:
                    self.notes.append(f"실패 인스턴스 {summary['failed']} — events 참조")
                if summary.get('attempts', 0) == 0 and self.verdict == 'fail':
                    self.notes.append('시도가 0 — 등록 실패·워커 미도달 여부를 events 로 확인')
        self.state = 'stopped'
        record = RunRecord(id=self.run_id, scenario_id=self.scenario.id, topology=self.topology_name,
                           profile=self.profile_name, started_at=self.started_at, ended_at=self.ended_at,
                           verdict=self.verdict, workers=[w.name for w in self.workers], summary=summary,
                           target_build=self.target_build, label=self.req.label, stop_reason=self.stop_reason)
        store.save_run_index(record)
        detail = {
            **record.model_dump(exclude_none=True),
            'label': self.req.label,
            'bindings': self.plan['bindings'] if self.plan else {},
            'plan': ({'roles': self.plan['roles'], 'identities': self.plan['identities'], 'phases': self.plan.get('phases'),
                      'max_instances': self.plan['max_instances'], 'rate_total': self.plan['rate_total'],
                      'peer_pools': self.plan.get('peer_pools'), 'pinned': self.plan.get('pinned'),
                      'steps': self.plan.get('steps')} if self.plan else None),
            'held_s': round(self.held_s, 1),
            'profile_doc': (self.profile.model_dump(exclude_none=True) if self.profile else None),
            'counters': snap['counters'], 'timers': snap['timers'], 'events': snap['events'],
            'expect_results': self.expect_results, 'evidence_results': self.evidence_results, 'step_log': self.step_log,
            'stop_reason': self.stop_reason, 'doc_rate': self.doc_rate, 'notes': self.notes,
            'fixtures': (self.fixtures.report() if self.fixtures is not None else None),
        }
        with open(os.path.join(self.rec.dir, 'run.json'), 'w', encoding='utf-8') as f:
            json.dump(detail, f, ensure_ascii=False, indent=2)
        self.rec.close()
        self._publish_state({'summary': summary})
        _log('info', f'[run {self.run_id}] {self.verdict} — {summary}')
        self.mgr._done(self.run_id)

    # ── 요약 (RFC 6076 표) + expect 판정
    def _summary(self, snap: dict) -> dict:
        c, t = snap['counters'], snap['timers']
        attempts = c.get('attempts', 0)
        sessions = c.get('sessions', 0)
        completed = c.get('completed', 0)
        out = {
            'attempts': attempts, 'sessions': sessions, 'completed': completed,
            'failed': c.get('failed', 0), 'skipped': c.get('skipped', 0),
            'registered_ok': c.get('registered_ok', 0), 'registered_fail': c.get('registered_fail', 0),
            'ser_pct': (100.0 * sessions / attempts) if attempts else None,
            'scr_pct': (100.0 * completed / sessions) if sessions else None,
            'rtp_rx': c.get('rtp_rx', 0), 'rtp_lost': c.get('rtp_lost', 0),
            'rtp_loss_pct': (100.0 * c.get('rtp_lost', 0) / (c.get('rtp_rx', 0) + c.get('rtp_lost', 0)))
                            if (c.get('rtp_rx', 0) + c.get('rtp_lost', 0)) else None,
            'doc_saps': self.doc_rate,
            'codes': ','.join(f'{k[6:]}:{v}' for k, v in sorted(c.items()) if k.startswith('codes.')) or None,
        }
        # 피어 pbx/mgcf 축 관측(있을 때만) — 비율은 RATIO_METRICS 정의로
        for k in ('progress_tx', 'early_media', 'prack_tx', 'prack_rx', 'reinvite_ok', 'reinvite_fail', 'reinvite_rx',
                  'dtmf_tx', 'dtmf_sent', 'dtmf_rx', 'q850_tx', 'q850_rx', 'refer_tx',
                  'rtp_tx', 'rtp_silent_legs', 'media_send', 'media_stop', 'skipped_rtp_cap', 'early_rtp_ok', 'early_rtp_rx',
                  # 보류 음악(announcements.md §3.3) — hold/resume 송신 · 피보류 단말 RTP 증분 도달/패킷 수
                  'hold_tx', 'resume_tx', 'remote_hold', 'remote_resume', 'moh_rtp_ok', 'moh_rtp_rx',
                  # 전달·합류·구독(volte_supplementary_services §5·§6 · dispatch_center §5 · RFC 6665)
                  'consult_tx', 'consult_ok', 'refer_attended_tx', 'pickup_tx', 'pickup_ok', 'replaces_tx', 'replaces_ok',
                  'join_tx', 'join_ok', 'join_ssrc2', 'ringing_leg_cancelled', 'subscribe_tx', 'notify_rx', 'publish_tx',
                  # RFC 6076 SEER/ISA 원천 · RTCP 수신 통계
                  'invite_tx', 'seer_ok', 'isa_fail', 'rtcp_rx', 'rtcp_rr_rx',
                  # MCData SDS(TS 24.282) — 송신·그룹 송신·도착·disposition 요청/회신·통지 수신
                  'sds_tx', 'sds_group_tx', 'sds_rx', 'sds_disposition_req', 'sds_disposition_rx', 'sds_notif_rx',
                  # 실단말(real-ue, §3.3) — 실스택 leg 수·RTP 카운터·표본 없음·프로세스 종료
                  'real_legs', 'real_rtp_tx', 'real_rtp_rx', 'real_rtp_lost', 'real_rtp_silent_legs', 'real_rtp_nosample', 'real_ue_exit',
                  # 영상(invite.media.video) — 오퍼에 m=video 를 실은 수·활성 answer·워커에 비디오 파일이 없어 오디오만 나간 수
                  'video_offered', 'video_ok', 'video_unavailable',
                  # PTT — affiliation·그룹 세션·floor(TS 24.380)
                  'affiliated_ok', 'affiliated_fail', 'group_calls', 'group_joined', 'floor_request_tx', 'floor_granted',
                  'floor_denied', 'floor_queued', 'floor_revoked', 'floor_release_tx'):
            if c.get(k):
                out[k] = c[k]
        for name, (num, den) in RATIO_METRICS.items():
            if name in ('ser_pct', 'scr_pct'):
                continue
            if c.get(den):
                out[name] = 100.0 * c.get(num, 0) / c[den]
        for pref in ('refer_codes.', 'subscribe_codes.', 'publish_codes.', 'sds_codes.'):
            codes = ','.join(f'{k[len(pref):]}:{v}' for k, v in sorted(c.items()) if k.startswith(pref))
            if codes:
                out[pref[:-1]] = codes
        q850 = ','.join(f'{k[5:]}:{v}' for k, v in sorted(c.items()) if k.startswith('q850.'))
        if q850:
            out['q850_causes'] = q850
        if c.get('real_rtp_rx', 0) + c.get('real_rtp_lost', 0):
            out['real_rtp_loss_pct'] = 100.0 * c.get('real_rtp_lost', 0) / (c.get('real_rtp_rx', 0) + c.get('real_rtp_lost', 0))
        for name in ('rrd_ms', 'srd_ms', 'sdd_ms', 'jitter_ms', 'sdt_s',
                     'group_fanout_ms', 'floor_grant_ms', 'floor_taken_ms', 'floor_queue_ms', 'floor_idle_ms', 'affiliate_ms',
                     'real_srd_ms', 'real_jitter_ms', 'sds_delay_ms', 'fd_upload_ms', 'fd_delay_ms', 'fd_download_ms'):
            h = t.get(name)
            if h:
                out[f'{name}_p50'] = h.get('p50')
                out[f'{name}_p95'] = h.get('p95')
                out[f'{name}_max'] = h.get('max')
        # MOS(G.107 추정, 1~4.5)는 로그 버킷 백분위가 거칠어 평균·최솟값(최악 leg)으로 요약한다 — 기대치도 min
        for name in ('mos', 'rtcp_remote_loss_pct', 'real_mos'):
            h = t.get(name)
            if h:
                out[f'{name}_mean'] = h.get('mean')
                out[f'{name}_min'] = h.get('min')
                out[f'{name}_max'] = h.get('max')
        # expect 판정 — 단계별
        self.expect_results = []
        for i, s in enumerate(self.scenario.flow):
            for metric, exp in (s.expect or {}).items():
                r = {'step': i, 'kind': s.step, 'metric': metric, 'expect': exp if isinstance(exp, (int, float)) else exp.model_dump(exclude_none=True)}
                if metric == 'code':
                    want = int(exp) if isinstance(exp, (int, float)) else None
                    if s.step == 'register':
                        got_bad = c.get('registered_fail', 0)
                        r.update({'observed': f'ok={c.get("registered_ok", 0)} fail={got_bad}', 'ok': got_bad == 0 if want == 200 else True})
                    elif s.step in ('invite', 'group_call', 'pickup', 'replaces', 'join') and want is not None and want >= 300:
                        # 기대한 거절(ACL 403·라우팅 reject·타 그룹 픽업 403·링잉 호 없음 404) — 그 코드가 관측되고 실패 인스턴스가 없어야 한다
                        r.update({'observed': f'codes.{want}={c.get(f"codes.{want}", 0)} failed={c.get("failed", 0)}',
                                  'ok': c.get(f'codes.{want}', 0) > 0 and c.get('failed', 0) == 0})
                    elif s.step in ('pickup', 'replaces', 'join'):
                        ok_n = c.get(f'{s.step}_ok', 0)
                        r.update({'observed': f'{s.step}_ok={ok_n} failed={c.get("failed", 0)}', 'ok': ok_n > 0 and c.get('failed', 0) == 0})
                    elif s.step in ('subscribe', 'publish'):
                        # 최종 응답 코드 카운터(워커 <step>_codes.N) — 기대 코드가 관측되고 실패 인스턴스가 없어야 한다(403·489 도 기대값이 된다)
                        want = want or 200
                        key = f'{s.step}_codes.{want}'
                        r.update({'observed': f'{key}={c.get(key, 0)} failed={c.get("failed", 0)}', 'ok': c.get(key, 0) > 0 and c.get('failed', 0) == 0})
                    elif s.step in ('invite', 'answer', 'bye', 'group_call'):
                        r.update({'observed': f'sessions={sessions} failed={c.get("failed", 0)}', 'ok': c.get('failed', 0) == 0 if want == 200 else True})
                    elif s.step == 'reject':
                        r.update({'observed': f'codes.{want}={c.get(f"codes.{want}", 0)}', 'ok': c.get(f'codes.{want}', 0) > 0})
                    elif s.step == 'progress':
                        # 피어가 183 을 냈고 실패 인스턴스가 없어야 한다(발신자 1xx 도달은 워커가 시한으로 판정)
                        r.update({'observed': f'progress_tx={c.get("progress_tx", 0)} early_media={c.get("early_media", 0)} failed={c.get("failed", 0)}',
                                  'ok': c.get('progress_tx', 0) > 0 and c.get('failed', 0) == 0})
                    elif s.step in ('hold', 'resume'):
                        r.update({'observed': f'reinvite_ok={c.get("reinvite_ok", 0)} fail={c.get("reinvite_fail", 0)}',
                                  'ok': c.get('reinvite_ok', 0) > 0 and c.get('reinvite_fail', 0) == 0 if want == 200 else True})
                    elif s.step == 'refer':
                        want = want or 202
                        r.update({'observed': f'refer_codes.{want}={c.get(f"refer_codes.{want}", 0)} failed={c.get("failed", 0)}',
                                  'ok': c.get(f'refer_codes.{want}', 0) > 0 and c.get('failed', 0) == 0})
                    else:
                        r.update({'observed': None, 'ok': True})
                elif metric in RATIO_METRICS:
                    num, den = RATIO_METRICS[metric]
                    val = out.get(metric)
                    lower = metric in LOWER_BETTER_RATIOS
                    if isinstance(exp, (int, float)):
                        want = float(exp)
                    else:
                        want = float((getattr(exp, 'max', None) if lower else getattr(exp, 'min', None)) or 0)
                    if not c.get(den):
                        r.update({'observed': None, 'ok': False, 'why': f'분모 {den} 없음'})
                    else:
                        r.update({'observed': val, 'ok': val is not None and (val <= want if lower else val >= want)})
                else:
                    h = t.get(metric)
                    if not h or not h.get('count'):
                        r.update({'observed': None, 'ok': False, 'why': '표본 없음'})
                    elif isinstance(exp, (int, float)):
                        r.update({'observed': h.get('max'), 'ok': h.get('max') is not None and h['max'] <= float(exp)})
                    else:
                        ok = True
                        obs = {}
                        for q in ('p50', 'p95', 'p99', 'max'):
                            want = getattr(exp, q, None)
                            if want is None:
                                continue
                            got = h.get(q)
                            obs[q] = got
                            if got is None or got > float(want):
                                ok = False
                        if exp.min is not None:
                            obs['min'] = h.get('min')
                            if h.get('min') is None or h['min'] < float(exp.min):
                                ok = False
                        r.update({'observed': obs, 'ok': ok})
                self.expect_results.append(r)
        out['expect_ok'] = sum(1 for r in self.expect_results if r['ok'])
        out['expect_total'] = len(self.expect_results)
        return out

    def live(self) -> dict:
        snap = self.rec.snapshot()
        return {'id': self.run_id, 'state': self.state, 'verdict': self.verdict, 'rate_saps': self.rate,
                'scenario_id': self.scenario.id, 'topology': self.topology_name, 'profile': self.profile_name,
                'started_at': self.started_at, 'workers': [w.name for w in self.workers],
                'counters': snap['counters'], 'timers': snap['timers'], 'gauges': self.rec.gauges_sum(),
                'events': snap['events'], 'doc_rate': self.doc_rate, 'notes': self.notes, 'step_log': self.step_log,
                'hold': self._hold.is_set(), 'held_s': round(self.held_s, 1), 'label': self.req.label,
                'target_build': self.target_build, 'stop_reason': self.stop_reason,
                'plan': ({'roles': self.plan['roles'], 'phases': self.plan.get('phases'), 'max_instances': self.plan['max_instances'],
                          'rate_total': self.plan['rate_total'], 'steps': self.plan.get('steps')} if self.plan else None),
                'profile_doc': (self.profile.model_dump(exclude_none=True) if self.profile else None)}


# ──────────────────────────────────────────────────────────────────────────
#  RunManager
# ──────────────────────────────────────────────────────────────────────────

class RunManager:
    def __init__(self):
        self.config: dict = {}
        self._lock = threading.Lock()
        self._active: Dict[str, RunDriver] = {}
        self._recent: Dict[str, RunDriver] = {}
        self.stream: Optional[StreamServer] = None

    def init(self, config: dict, logger=None) -> None:
        global _LOG
        _LOG = logger
        self.config = config or {}
        tcfg = self.config.get('Tester') or {}
        ip = str(tcfg.get('WorkerStreamIp') or '0.0.0.0')
        port = int(tcfg.get('WorkerStreamPort') or 7110)
        self.stream = StreamServer(ip, port, self._dispatch)
        self.stream.start()
        _log('info', f'[tester] worker stream listening on {ip}:{port}')

    def shutdown(self) -> None:
        if self.stream:
            self.stream.stop()
        for d in list(self._active.values()):
            d.request_stop('shutdown')

    def _dispatch(self, rec: dict, addr) -> None:
        rid = rec.get('run_id')
        if not rid:
            return   # hello/log 는 run_id 가 없다
        with self._lock:
            d = self._active.get(rid) or self._recent.get(rid)
        if d is not None:
            d.rec.on_record(rec)

    def start(self, req: RunRequest, requester_token: Optional[str] = None) -> RunDriver:
        scenario, sdoc, errs = store.get_scenario(req.scenario_id)
        if sdoc is None:
            raise ValueError(f'scenario_not_found: {req.scenario_id}')
        if scenario is None:
            raise ValueError(f'invalid_scenario: {errs}')
        topo_rec = store.find_topology(int(req.topology_id) if req.topology_id is not None else req.topology)
        if topo_rec is None:
            raise ValueError('topology_not_found')
        topology = store.topology_model(topo_rec)
        if topology is None:
            raise ValueError('invalid_topology')
        profile = None
        if req.profile:
            profile, pdoc, perrs = store.get_profile(req.profile)
            if pdoc is None:
                raise ValueError(f'profile_not_found: {req.profile}')
            if profile is None:
                raise ValueError(f'invalid_profile: {perrs}')
        with self._lock:
            if self._active:
                raise RuntimeError('run_active: ' + ','.join(self._active))
            run_id = datetime.now().strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:6]
            d = RunDriver(self, run_id, req, scenario, topology, topo_rec.get('doc') or {}, profile, req.profile,
                          str((topo_rec.get('doc') or {}).get('name') or topo_rec.get('id')), requester_token)
            self._active[run_id] = d
        store.save_run_index(RunRecord(id=run_id, scenario_id=scenario.id, topology=d.topology_name,
                                       profile=req.profile, started_at=d.started_at, verdict='running', label=req.label))
        d.start()
        return d

    def _done(self, run_id: str) -> None:
        with self._lock:
            d = self._active.pop(run_id, None)
            if d is not None:
                self._recent[run_id] = d
                if len(self._recent) > 5:
                    self._recent.pop(next(iter(self._recent)))

    def get(self, run_id: str) -> Optional[RunDriver]:
        with self._lock:
            return self._active.get(run_id) or self._recent.get(run_id)

    def active(self) -> List[RunDriver]:
        with self._lock:
            return list(self._active.values())

    def stop(self, run_id: str) -> bool:
        d = self.get(run_id)
        if d is None or d.state == 'stopped':
            return False
        d.request_stop('operator')
        return True

    def hold(self, run_id: str, on: bool) -> bool:
        d = self.get(run_id)
        if d is None or d.state != 'running':
            return False
        d.set_hold(on)
        return True

    def rate(self, run_id: str, rate: float) -> bool:
        d = self.get(run_id)
        if d is None or d.state != 'running':
            return False
        d.set_rate(rate)
        return True


RUNS = RunManager()
