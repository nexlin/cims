"""대상 관측 — run 동안 대상 호스트 자원을 모으고(stop_on.target_cpu_pct 원천), run 끝에 target_evidence 를 판정한다 (test_instrument.md §5).

원천 = 대상 OAM API(토폴로지 oam 노드, 토큰은 tester_target.resolve_token):
  · 호스트 자원  GET /api/v1/agents/{id}/metrics  → items[{ts, cpu_pct, mem_pct, load_avg}] (agent heartbeat, 약 3 s 간격, 최신이 앞)
      관측 대상 agent = 대상 노드의 감시 프로세스(`nodes.*.procs`)를 패키지/프로세스 이름으로 가진 배포들의 agent.
      프로세스별 CPU 는 heartbeat 에 없다 — 호스트 SSH 관측(`hosts.*.ssh`, 아래 SshObserver)이 그 원천이다.
  · 증거          GET /api/v1/recordings · /api/v1/alerts · /api/v1/events  — run 창(started~ended)으로 자른 건수

발생기 측 관측이 1차 판정이고 target_evidence 는 2차다(§4): 증거가 어긋나면 run 은 fail 이지만, 원천을 읽지 못한 항목(`ok=None`)은
판정에서 빼고 참고(note)로만 남긴다 — 대상 OAM 이 죽었다고 시험이 실패로 뒤집히지는 않는다.
"""
from __future__ import annotations

import os
import shlex
import sqlite3
import subprocess
import threading
import time
from datetime import datetime
from typing import Dict, List, Optional, Tuple

from services import tester_target
from services.tester_bus import publish
from services.tester_models import Host, Scenario, Topology

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
    procs: Dict[str, dict] = {}
    try:
        db = sqlite3.connect(db_path)
        try:
            prow = db.execute('SELECT t, host, proc, cpu_pct, rss_mb, fds FROM target_proc ORDER BY t').fetchall()
        except sqlite3.Error:   # fds 열이 없는 옛 run
            prow = [(*r, None) for r in db.execute('SELECT t, host, proc, cpu_pct, rss_mb FROM target_proc ORDER BY t').fetchall()]
        db.close()
    except sqlite3.Error:
        prow = []
    for t, host, proc, cpu, rss, fds in prow:
        a = procs.setdefault(f'{host}/{proc}', {'t': [], 'cpu_pct': [], 'rss_mb': [], 'fds': []})
        a['t'].append(t)
        a['cpu_pct'].append(cpu)
        a['rss_mb'].append(rss)
        a['fds'].append(fds)
    return {'agents': out, 'procs': procs}


def _in_window(ts: Optional[float], t0: float, t1: float) -> bool:
    return ts is not None and t0 <= ts <= t1


def _growth_evidence(ev, deltas: Optional[dict], unit: str) -> dict:
    """rss_growth_mb / fd_growth — 호스트 SSH 관측의 프로세스별 처음↔끝 차(deltas {"host/proc": v}) 중 ev.proc 에 맞는 것의 최댓값을 판정한다.
    ev.proc 은 "host/proc" 또는 프로세스 이름(모든 호스트). None(원천 없음)·해당 프로세스 없음 = 판정 불가."""
    r = {'kind': ev.kind, 'code': ev.code, 'proc': ev.proc, 'min': ev.min, 'max': ev.max, 'observed': None, 'ok': None, 'why': None}
    if deltas is None:
        r['why'] = '판정 불가 — hosts.*.ssh 와 nodes.*.procs 가 있어야 프로세스별 RSS/fd 를 관측한다'
        return r
    picked = {k: v for k, v in deltas.items() if not ev.proc or k == ev.proc or k.split('/', 1)[-1] == ev.proc}
    if not picked:
        r['why'] = f'판정 불가 — 관측된 프로세스 없음(procs={sorted(deltas) or "[]"}' + (f', proc={ev.proc!r}' if ev.proc else '') + ')'
        return r
    worst = max(picked, key=lambda k: picked[k])
    v = picked[worst]
    r['observed'] = v
    r['ok'] = (ev.min is None or v >= ev.min) and (ev.max is None or v <= ev.max)
    if not r['ok']:
        r['why'] = f'{worst} {v:+g} {unit} — 기대 ' + ' · '.join(x for x in (f'≥ {ev.min}' if ev.min is not None else '', f'≤ {ev.max}' if ev.max is not None else '') if x)
    return r


def evaluate_evidence(scenario: Scenario, topology: Topology, started: float, ended: float,
                      requester_token: Optional[str], log_errors: Optional[int] = None,
                      rss_delta: Optional[Dict[str, float]] = None, fd_delta: Optional[Dict[str, int]] = None) -> List[dict]:
    """target_evidence → [{kind, code, min, max, observed, ok(True|False|None), why}] — 창은 시작 −2 s ~ 종료 +10 s(대상의 기록 지연).
    log_errors = 호스트 SSH 관측(SshObserver.log_errors)이 센 run 동안 늘어난 모듈 로그 ERROR 줄 수(None = 원천 없음 → 판정 불가).
    rss_delta/fd_delta = 같은 관측의 프로세스별 RSS(MB)·열린 fd 처음↔끝 차(소크 누수 판정 rss_growth_mb/fd_growth — None = 원천 없음)."""
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
        if ev.kind in ('rss_growth_mb', 'fd_growth'):
            res.append(_growth_evidence(ev, rss_delta if ev.kind == 'rss_growth_mb' else fd_delta, 'MB' if ev.kind == 'rss_growth_mb' else 'fd'))
            continue
        r = {'kind': ev.kind, 'code': ev.code, 'min': ev.min, 'max': ev.max, 'observed': None, 'ok': None, 'why': None}
        if ev.kind == 'log_errors':
            # 원천 = 호스트 SSH 관측(hosts.*.ssh + nodes.*.logs) — 대상 OAM 이 아니다
            if log_errors is None:
                r['why'] = '판정 불가 — hosts.*.ssh 와 nodes.*.logs 가 있어야 run 동안 늘어난 모듈 로그 ERROR 줄을 센다'
            else:
                r['observed'] = log_errors
                r['ok'] = (ev.min is None or log_errors >= ev.min) and (ev.max is None or log_errors <= ev.max)
                if not r['ok']:
                    r['why'] = f'관측 {log_errors} — 기대 ' + ' · '.join(x for x in (f'≥ {ev.min}' if ev.min is not None else '', f'≤ {ev.max}' if ev.max is not None else '') if x)
            res.append(r)
            continue
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
                r['why'] = f'판정 불가 — {ev.kind} 는 대상 OAM API 에 원천이 없다'
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


# ──────────────────────────────────────────────────────────────────────────
#  호스트 SSH 관측 — hosts.*.ssh 가 있는 호스트에 대해 run 동안 /proc 를 읽어 호스트 CPU/메모리/load + 대상 노드 procs 의 프로세스별 CPU/RSS 를
#  모으고(agent heartbeat 가 없는 최소 배치·타 IMS·IP-PBX 호스트도 관측 가능), run 끝에 nodes.*.logs 의 늘어난 ERROR/FATAL 줄을 센다(log_errors).
#  원격 명령은 표준 도구(sh·awk·pgrep·stat·tail·grep)만 — 대상에 아무것도 설치하지 않는다. 프로세스 CPU 는 /proc/<pid>/stat 의 utime+stime 틱
#  차분 / CLK_TCK / 경과 시간(ps 의 pcpu 는 수명 평균이라 쓰지 않는다). 소크 누수 판정 원천 = 프로세스 RSS 의 처음↔끝 차(rss_delta_mb).
# ──────────────────────────────────────────────────────────────────────────

SSH_POLL_S = 3.0
_LOG_ERR_RE = r'\bERROR\b|\bFATAL\b|\[ERROR\]|\[FATAL\]|\bLOG_ERROR\b'


def ssh_argv(host: Host, remote_cmd: str) -> List[str]:
    """`ssh -i <key_env 파일> -p <port> -o BatchMode=yes … user@ip <cmd>` — 개인키 경로는 환경변수에서(레코드에 비밀 없음)."""
    key = os.environ.get(host.ssh.key_env or '', '').strip() if host.ssh else ''
    argv = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=accept-new', '-o', 'LogLevel=ERROR',
            '-p', str(host.ssh.port)]
    if key:
        argv += ['-i', key]
    argv += [f'{host.ssh.user}@{host.ip}', remote_cmd]
    return argv


def ssh_run(host: Host, remote_cmd: str, timeout: float = 12) -> str:
    """기본 실행기 — 실패는 예외(호출자가 note 로 남긴다)."""
    if host.ssh and host.ssh.key_env and not os.environ.get(host.ssh.key_env, '').strip():
        raise RuntimeError(f'환경변수 {host.ssh.key_env} 에 SSH 개인키 경로가 없다')
    p = subprocess.run(ssh_argv(host, remote_cmd), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f'ssh {host.ssh.user}@{host.ip} rc={p.returncode}: {(p.stderr or p.stdout).strip()[-200:]}')
    return p.stdout


def _sample_cmd(procs: List[str], logs: List[str]) -> str:
    """한 번의 원격 호출로 시각·CLK_TCK·/proc/stat·meminfo·loadavg·프로세스별 (틱, RSS)·로그 파일 크기를 받는다."""
    parts = ['echo "@T $(date +%s.%N) $(getconf CLK_TCK 2>/dev/null || echo 100)"', 'head -1 /proc/stat | sed "s/^/@S /"',
             "awk '/^MemTotal|^MemAvailable/{print \"@M\", $1, $2}' /proc/meminfo", 'echo "@L $(cut -d" " -f1 /proc/loadavg)"']
    for c in procs:
        q = shlex.quote(c)
        # 프로세스 행 = 이름 pid 틱 RSS(kB) 열린 fd 수 — fd 수는 소켓·파일 누수(소크 판정 fd_growth)의 원천
        parts.append(f'for p in $(pgrep -x -- {q} 2>/dev/null); do echo "@P {q} $p $(awk \'{{print $14+$15}}\' /proc/$p/stat 2>/dev/null) '
                     f'$(awk \'/VmRSS/{{print $2}}\' /proc/$p/status 2>/dev/null) $(ls /proc/$p/fd 2>/dev/null | wc -l)"; done')
    for g in logs:
        parts.append(f'for f in {g}; do [ -f "$f" ] && echo "@F $(stat -c %s "$f") $f"; done')
    return '; '.join(parts)


def parse_sample(text: str) -> dict:
    """원격 출력 → {t, clk, cpu:[…8], mem_total, mem_avail, load, procs:{(proc,pid): (ticks, rss_kb, fds)}, files:{path: size}}. fds 는 구 출력(5 열)이면 None."""
    out: dict = {'t': None, 'clk': 100, 'cpu': None, 'mem_total': None, 'mem_avail': None, 'load': None, 'procs': {}, 'files': {}}
    for ln in text.splitlines():
        f = ln.split()
        if not f:
            continue
        try:
            if f[0] == '@T':
                out['t'] = float(f[1]); out['clk'] = int(f[2]) if len(f) > 2 else 100
            elif f[0] == '@S' and f[1] == 'cpu':
                out['cpu'] = [int(x) for x in f[2:10]]
            elif f[0] == '@M':
                if f[1].startswith('MemTotal'): out['mem_total'] = int(f[2])
                elif f[1].startswith('MemAvailable'): out['mem_avail'] = int(f[2])
            elif f[0] == '@L':
                out['load'] = float(f[1])
            elif f[0] == '@P' and len(f) >= 5:
                out['procs'][(f[1], int(f[2]))] = (int(f[3]), int(f[4]), int(f[5]) if len(f) >= 6 else None)
            elif f[0] == '@F' and len(f) >= 3:
                out['files'][' '.join(f[2:])] = int(f[1])
        except (ValueError, IndexError):
            continue
    return out


def host_cpu_pct(prev: dict, cur: dict) -> Optional[float]:
    """/proc/stat 첫 줄 차분 — (전체 − idle − iowait) / 전체 × 100."""
    a, b = prev.get('cpu'), cur.get('cpu')
    if not a or not b:
        return None
    tot = sum(b) - sum(a)
    idle = (b[3] + b[4]) - (a[3] + a[4])
    return None if tot <= 0 else max(0.0, min(100.0, 100.0 * (tot - idle) / tot))


class SshObserver(threading.Thread):
    """run 하나의 호스트 SSH 관측 — metrics.sqlite `target(t, agent=<host id>, cpu_pct, mem_pct, load)` + `target_proc(t, host, proc, pid, cpu_pct, rss_mb)`
    + SSE `target`/`target_proc`. runner(host, cmd) → 출력 문자열(시험은 가짜 실행기를 넣는다)."""

    def __init__(self, run_id: str, db_path: str, topology: Topology, runner=None, poll_s: float = SSH_POLL_S):
        super().__init__(name=f'tester-ssh-{run_id}', daemon=True)
        self.run_id, self.db_path, self.topology = run_id, db_path, topology
        self._runner = runner or ssh_run
        self._poll = poll_s
        self._stop_ev = threading.Event()
        self._lock = threading.Lock()
        self._recent: Dict[str, List[float]] = {}
        self.hosts: Dict[str, dict] = {}          # host id → {procs, logs, prev(sample), base_files{path: size}}
        self.note: Optional[str] = None
        self.errors: Dict[str, str] = {}
        self.samples = 0
        self.peak_cpu: Optional[float] = None
        self.proc_peak: Dict[str, float] = {}     # "host/proc" → 피크 CPU %
        self.rss_first: Dict[str, float] = {}     # "host/proc" → 첫 RSS MB
        self.rss_last: Dict[str, float] = {}
        self.fd_first: Dict[str, int] = {}        # "host/proc" → 첫 열린 fd 수(소켓·파일 누수 판정 원천)
        self.fd_last: Dict[str, int] = {}
        self.rss_points: Dict[str, List[Tuple[float, float]]] = {}   # "host/proc" → [(t, rss_mb)] — 기울기(MB/h) 추정
        for hid, h in topology.hosts.items():
            if h.ssh is None:
                continue
            nodes = [n for n in topology.target.nodes.values() if n.host == hid]
            if not nodes:
                continue
            procs = sorted({p for n in nodes for p in (n.procs or [])})
            logs = sorted({g for n in nodes for g in (n.logs or [])})
            self.hosts[hid] = {'host': h, 'procs': procs, 'logs': logs, 'prev': None, 'base_files': None}

    @staticmethod
    def wanted(topology: Topology) -> bool:
        return any(h.ssh is not None and any(n.host == hid for n in topology.target.nodes.values()) for hid, h in topology.hosts.items())

    def stop(self) -> None:
        self._stop_ev.set()

    def cpu_now(self) -> Optional[float]:
        with self._lock:
            vals = [sum(v) / len(v) for v in self._recent.values() if v]
        return max(vals) if vals else None

    def rss_delta_mb(self) -> Dict[str, float]:
        return {k: round(self.rss_last[k] - self.rss_first[k], 1) for k in self.rss_first if k in self.rss_last}

    def fd_delta(self) -> Dict[str, int]:
        """프로세스별 열린 fd 수 처음↔끝 차 — 소크 누수 판정(fd_growth). fd 를 못 읽은 프로세스는 없다."""
        return {k: self.fd_last[k] - self.fd_first[k] for k in self.fd_first if k in self.fd_last}

    def rss_slope_mb_per_h(self) -> Dict[str, float]:
        """프로세스별 RSS 최소제곱 기울기(MB/h) — 처음↔끝 차보다 표본 잡음에 덜 흔들리는 누수 추정(표본 3개 이상·10 초 이상일 때만)."""
        out: Dict[str, float] = {}
        with self._lock:
            for k, pts in self.rss_points.items():
                if len(pts) < 3 or pts[-1][0] - pts[0][0] < 10:
                    continue
                n = len(pts)
                mt = sum(p[0] for p in pts) / n
                mr = sum(p[1] for p in pts) / n
                den = sum((p[0] - mt) ** 2 for p in pts)
                if den <= 0:
                    continue
                out[k] = round(3600.0 * sum((p[0] - mt) * (p[1] - mr) for p in pts) / den, 2)
        return out

    def _sample(self, hid: str, db) -> None:
        e = self.hosts[hid]
        cur = parse_sample(self._runner(e['host'], _sample_cmd(e['procs'], e['logs'])))
        if cur['t'] is None:
            raise RuntimeError('원격 출력에 시각(@T)이 없다 — sh/date/getconf 를 확인')
        if e['base_files'] is None:
            e['base_files'] = dict(cur['files'])
        prev = e['prev']
        e['prev'] = cur
        t = int(cur['t'])
        dt = (cur['t'] - prev['t']) if prev else 0.0
        # 프로세스 행은 첫 표본부터(RSS 의 처음값 — 누수 판정의 기점), CPU 는 차분이라 두 번째 표본부터
        for (proc, pid), (ticks, rss_kb, fds) in cur['procs'].items():
            key = f'{hid}/{proc}'
            rss_mb = rss_kb / 1024.0
            pcpu = None
            if prev and (proc, pid) in prev['procs'] and dt > 0:
                pcpu = max(0.0, 100.0 * (ticks - prev['procs'][(proc, pid)][0]) / max(1, cur['clk']) / dt)
            db.execute('INSERT INTO target_proc VALUES (?,?,?,?,?,?,?)', (t, hid, proc, pid, pcpu, rss_mb, fds))
            with self._lock:
                if pcpu is not None:
                    self.proc_peak[key] = max(self.proc_peak.get(key, 0.0), pcpu)
                self.rss_first.setdefault(key, rss_mb)
                self.rss_last[key] = rss_mb
                pts = self.rss_points.setdefault(key, [])
                pts.append((cur['t'], rss_mb))
                if len(pts) > 20000:      # 8 h 소크 × 3 s ≈ 9600 — 상한을 넘으면 절반으로 솎는다
                    del pts[::2]
                if fds is not None:
                    self.fd_first.setdefault(key, fds)
                    self.fd_last[key] = fds
            publish('target_proc', {'run_id': self.run_id, 't': t, 'host': hid, 'proc': proc, 'pid': pid, 'cpu_pct': pcpu, 'rss_mb': round(rss_mb, 1), 'fds': fds})
        if prev is None:
            return
        cpu = host_cpu_pct(prev, cur)
        mem = None
        if cur['mem_total'] and cur['mem_avail'] is not None:
            mem = 100.0 * (cur['mem_total'] - cur['mem_avail']) / cur['mem_total']
        if cpu is not None:
            db.execute('INSERT INTO target VALUES (?,?,?,?,?)', (t, hid, cpu, mem, cur['load']))
            with self._lock:
                r = self._recent.setdefault(hid, [])
                r.append(cpu)
                del r[:-3]
                self.samples += 1
                self.peak_cpu = cpu if self.peak_cpu is None else max(self.peak_cpu, cpu)
            publish('target', {'run_id': self.run_id, 't': t, 'agent': hid, 'cpu_pct': cpu, 'mem_pct': mem, 'source': 'ssh'})

    def run(self) -> None:
        if not self.hosts:
            self.note = '호스트 SSH 관측 불가 — ssh 가 있는 호스트에 대상 노드가 없다'
            return
        db = sqlite3.connect(self.db_path, check_same_thread=False)
        db.execute('CREATE TABLE IF NOT EXISTS target (t INTEGER, agent TEXT, cpu_pct REAL, mem_pct REAL, load REAL)')
        db.execute('CREATE INDEX IF NOT EXISTS target_t ON target (t)')
        db.execute('CREATE TABLE IF NOT EXISTS target_proc (t INTEGER, host TEXT, proc TEXT, pid INTEGER, cpu_pct REAL, rss_mb REAL, fds INTEGER)')
        db.execute('CREATE INDEX IF NOT EXISTS target_proc_t ON target_proc (t)')
        while not self._stop_ev.is_set():
            for hid in list(self.hosts):
                if hid in self.errors:
                    continue
                try:
                    self._sample(hid, db)
                except Exception as e:      # 호스트 하나의 실패는 그 호스트만 멈춘다(run 은 계속)
                    self.errors[hid] = str(e)
            db.commit()
            self._stop_ev.wait(self._poll)
        db.commit()
        db.close()
        if self.errors:
            self.note = '호스트 SSH 관측 실패 — ' + '; '.join(f'{h}: {m}' for h, m in self.errors.items())

    def log_errors(self) -> Optional[int]:
        """run 동안 nodes.*.logs 에 늘어난 ERROR/FATAL 줄 수(호스트 합). logs 를 둔 노드가 없거나 어느 호스트도 못 읽었으면 None(판정 불가)."""
        total, any_ok = 0, False
        for hid, e in self.hosts.items():
            if not e['logs'] or hid in self.errors or e['base_files'] is None:
                continue
            parts = []
            for g in e['logs']:
                parts.append(f'for f in {g}; do [ -f "$f" ] || continue; echo "@N $f"; done')
            try:
                listing = self._runner(e['host'], '; '.join(parts))
            except Exception as ex:
                self.errors[hid] = str(ex)
                continue
            files = [ln[3:].strip() for ln in listing.splitlines() if ln.startswith('@N ')]
            cmds = []
            for f in files:
                base = e['base_files'].get(f, 0)
                cmds.append(f'tail -c +{base + 1} {shlex.quote(f)} | grep -c -E {shlex.quote(_LOG_ERR_RE)}')
            if not cmds:
                any_ok = True
                continue
            try:
                out = self._runner(e['host'], '; '.join(c + ' || true' for c in cmds))
            except Exception as ex:
                self.errors[hid] = str(ex)
                continue
            for ln in out.splitlines():
                ln = ln.strip()
                if ln.isdigit():
                    total += int(ln)
            any_ok = True
        return total if any_ok else None
