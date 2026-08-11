"""FM 자기보고 클라이언트 (docs/design/alarm_self_reporting.md) — csp/FmReporter 의
Python 대칭 구현. OAM FM ingest(UDP)로 envelope v2 의 FM_REGISTER/FM_ALARM/FM_EVENT/
FM_SYNC 를 push 한다.

- 활성 알람의 SoT 는 모듈(여기), OAM 은 미러 — 유실·재기동은 주기 FM_SYNC 가 수렴.
- 신뢰성: 전 메시지 type:"event" — ack(동일 trans_id 의 response) 미수신 시 1s 간격
  최대 5회 재전송. ERROR code=UNREGISTERED 수신 시 FM_REGISTER 부터 다시.
- 스레드 1개: recv(1s 타임아웃)가 타이머 틱을 겸한다 (fm_ingest 수신측과 동형).
- stdlib 전용 (csc/vendor 고정 의존성 — 신규 의존성 금지).
"""

import json
import os
import socket
import threading
import time

_RETRY_SEC = 1            # ack 미수신 재전송 간격 (cmp_media_api.md §8)
_MAX_ATTEMPTS = 5         # 재전송 상한 — 초과 시 폐기 (FM_SYNC 가 수렴)
_REGISTER_RETRY_SEC = 5   # 미등록 상태의 FM_REGISTER 재시도 간격
_MAX_PACKET = 32768       # FM 채널 상한 — FM_REGISTER 카탈로그 성장 대비 (FmReporter.h 와 동일)
_EVENT_QUEUE_MAX = 32     # 미등록 구간 이벤트 버퍼 상한 (등록 시 flush)

_INSTANCE = None          # init() 이 채우는 프로세스 싱글턴 (mcptt audit 등에서 get())


def _now_iso() -> str:
    from datetime import datetime
    return datetime.now().isoformat(timespec='seconds')


class FmReporter:
    def __init__(self, oam_ip: str, oam_port: int, node: str, module: str,
                 catalog: dict, sync_sec: int = 60, log=None):
        self.oam = (oam_ip, int(oam_port))
        self.node = node
        self.module = module
        self.catalog = catalog or {}
        self.sync_sec = max(int(sync_sec or 60), 1)
        self.log = log
        self.boot_id = int(time.time())
        self._lock = threading.Lock()
        self._active: dict = {}      # akey(code@mo) -> {code, mo_instance, open_ts, params}
        self._pending: dict = {}     # trans_id -> {packet, cmd, attempts, next_send}
        self._event_queue: list = []  # 미등록 구간 이벤트 버퍼 (등록 시 flush)
        # trans_id 시드 = 부팅 ms (재기동 직후 구 프로세스 앞 지연 응답 오매칭 창 제거)
        self._trans_id = (int(time.time() * 1000) & 0x3FFFFFFF) | 1
        self._seq = 0
        self._registered = False
        self._last_register = 0.0
        self._last_sync = 0.0
        self._unreg_logged = False   # OAM 미기동 시 5s 재시도 로그 노이즈 억제
        self._sock = None
        self._thread = None
        self._stop = threading.Event()

    # ── lifecycle ────────────────────────────────────────────────────────

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(1.0)
        self._thread = threading.Thread(target=self._loop, name='fm-reporter', daemon=True)
        self._thread.start()
        self._log_info(f"[fm] reporter started (oam={self.oam[0]}:{self.oam[1]}, "
                       f"node={self.node}, boot_id={self.boot_id}, sync={self.sync_sec}s)")
        self._send_register()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        self._log_info("[fm] reporter stopped")

    # ── 공개 API — 알람은 (code, mo) 전이 시에만 통지 (멱등) ─────────────

    def alarm_open(self, code: str, mo: str, params: dict = None):
        akey = f"{code}@{mo}"
        with self._lock:
            if akey in self._active:
                return
            self._active[akey] = {'code': code, 'mo_instance': mo,
                                  'open_ts': _now_iso(), 'params': params or {}}
        payload = {'action': 'open', 'code': code, 'mo_instance': mo, 'ts': _now_iso()}
        if params:
            payload['params'] = params
        self._send('FM_ALARM', payload)
        self._log_info(f"[fm] ALARM OPEN {akey}")

    def alarm_close(self, code: str, mo: str):
        akey = f"{code}@{mo}"
        with self._lock:
            if self._active.pop(akey, None) is None:
                return
        self._send('FM_ALARM', {'action': 'close', 'code': code, 'mo_instance': mo,
                                'ts': _now_iso()})
        self._log_info(f"[fm] ALARM CLOSE {akey}")

    def send_event(self, etype: str, kind: str = 'stateChange', mo: str = None,
                   params: dict = None, message: str = None):
        payload = {'type': etype, 'kind': kind, 'ts': _now_iso()}
        if mo:
            payload['mo_instance'] = mo
        if params:
            payload['params'] = params
        if message:
            payload['message'] = message
        # 미등록 상태(OAM 미기동/재기동)의 이벤트는 재전송 5회로도 유실 — 기동 순서상
        # 모듈이 OAM 보다 먼저 뜨는 부트에서 process_started 가 사라지는 것을 막기 위해
        # 등록 성공 시까지 버퍼링 후 flush (상한 초과 시 오래된 것부터 폐기).
        if not self._registered:
            with self._lock:
                if len(self._event_queue) >= _EVENT_QUEUE_MAX:
                    self._event_queue.pop(0)
                self._event_queue.append(payload)
            return
        self._send('FM_EVENT', payload)

    # ── 송신 코어 ────────────────────────────────────────────────────────

    def _active_list(self) -> list:
        return [dict(v) for v in self._active.values()]

    def _send_register(self):
        with self._lock:
            active = self._active_list()
            self._last_register = time.time()
        self._send('FM_REGISTER', {'module': self.module, 'catalog': self.catalog,
                                   'active': active})

    def _send_sync(self):
        with self._lock:
            active = self._active_list()
            self._last_sync = time.time()
        self._send('FM_SYNC', {'active': active})

    def _send(self, cmd: str, payload: dict):
        if self._sock is None:
            return
        with self._lock:
            tid = self._trans_id
            self._trans_id += 1
            self._seq += 1
            seq = self._seq
        payload = {**payload, 'boot_id': self.boot_id, 'seq': seq}
        packet = json.dumps({'hdr': {'ver': 2, 'trans_id': tid, 'node': self.node,
                                     'cmd': cmd, 'type': 'event', 'service': 'cims'},
                             'payload': payload}, ensure_ascii=False).encode('utf-8')
        if len(packet) > _MAX_PACKET:
            self._log_error(f"[fm] {cmd} packet too large ({len(packet)}) — 미전송")
            return
        with self._lock:
            self._pending[tid] = {'packet': packet, 'cmd': cmd, 'attempts': 1,
                                  'next_send': time.time() + _RETRY_SEC}
        try:
            self._sock.sendto(packet, self.oam)
        except Exception:
            pass    # 재전송 루프가 처리

    # ── 수신/타이머 루프 ─────────────────────────────────────────────────

    def _loop(self):
        while not self._stop.is_set():
            try:
                data, _addr = self._sock.recvfrom(_MAX_PACKET)
                self._handle(data)
            except socket.timeout:
                pass
            except OSError:
                break
            self._tick()

    def _handle(self, data: bytes):
        try:
            msg = json.loads(data.decode('utf-8')) or {}
            hdr = msg.get('hdr') or {}
        except Exception:
            return
        if hdr.get('type') != 'response':
            return
        tid = hdr.get('trans_id')
        with self._lock:
            if self._pending.pop(tid, None) is None:
                return    # 미상/지연 응답
        if hdr.get('status') == 'OK':
            if hdr.get('cmd') == 'FM_REGISTER':
                self._registered = True
                self._unreg_logged = False
                self._apply_sync_interval(msg.get('payload'))
                self._log_info(f"[fm] registered (sync={self.sync_sec}s)")
                with self._lock:
                    queued, self._event_queue = self._event_queue, []
                for payload in queued:      # 미등록 구간에 쌓인 이벤트 송신
                    self._send('FM_EVENT', payload)
            return
        code = hdr.get('code')
        if code == 'UNREGISTERED':
            # OAM 재기동/최초 접속 — 루프가 FM_REGISTER 재시도
            if self._registered:
                self._log_info("[fm] OAM 미등록 응답 — 재등록 예정")
            self._registered = False
        else:
            self._log_error(f"[fm] {hdr.get('cmd')} ERROR {code} ({hdr.get('reason')})")

    def _tick(self):
        now = time.time()
        if not self._registered:
            if now - self._last_register >= _REGISTER_RETRY_SEC:
                if not self._unreg_logged:
                    self._log_info("[fm] 미등록 — FM_REGISTER 재시도 중 (OAM FM ingest 응답 대기)")
                    self._unreg_logged = True
                self._send_register()
        elif now - self._last_sync >= self.sync_sec:
            self._send_sync()
        # pending 재전송 (1s×5) — 초과분 폐기. 활성 알람은 FM_SYNC 가 복구.
        resend = []
        with self._lock:
            for tid in list(self._pending):
                p = self._pending[tid]
                if p['next_send'] > now:
                    continue
                if p['attempts'] >= _MAX_ATTEMPTS:
                    del self._pending[tid]
                    continue
                p['attempts'] += 1
                p['next_send'] = now + _RETRY_SEC
                resend.append(p['packet'])
        for packet in resend:
            try:
                self._sock.sendto(packet, self.oam)
            except Exception:
                pass

    # FM_REGISTER 응답의 sync_interval_sec 수용 — _handle 에서 payload 접근이 필요해
    # 별도 처리 대신 등록 시 OAM 지시값을 반영한다.
    def _apply_sync_interval(self, payload: dict):
        try:
            v = int((payload or {}).get('sync_interval_sec') or 0)
            if v > 0:
                self.sync_sec = v
        except Exception:
            pass

    def _log_info(self, m):
        if self.log:
            self.log.log_info(m)

    def _log_error(self, m):
        if self.log:
            self.log.log_error(m)


class DbHealthProbe(threading.Thread):
    """DB 연결 상태 probe — 요청 경로와 완전 분리된 전용 연결로 주기 ping.
    3연속 실패 시 down / 성공 시 up — 전이 시에만 콜백 (csp CDbManager 동형)."""

    PERIOD_SEC = 10
    MAX_FAIL = 3

    def __init__(self, get_db_cfg, on_change, log=None):
        super().__init__(name='fm-db-probe', daemon=True)
        self._get_cfg = get_db_cfg      # callable → CimsDatabase dict (reload 반영)
        self._on_change = on_change     # callable(bool up)
        self.log = log
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        import pymysql
        conn = None
        fail = 0
        up = True    # 기동 시 up 가정 — 3연속 실패 후 첫 down 전이
        while not self._stop.wait(self.PERIOD_SEC):
            cfg = self._get_cfg() or {}
            if not cfg.get('Host'):
                continue    # DB 미구성 — 판정 보류
            ok = False
            try:
                if conn is None:
                    conn = pymysql.connect(
                        host=cfg.get('Host'), port=int(cfg.get('Port', 3306)),
                        user=cfg.get('User'), password=cfg.get('Password', ''),
                        database=cfg.get('Db', cfg.get('Database', 'cims')),
                        connect_timeout=3, read_timeout=3, write_timeout=3)
                conn.ping(reconnect=False)
                ok = True
            except Exception:
                try:
                    if conn is not None:
                        conn.close()
                except Exception:
                    pass
                conn = None
            if ok:
                fail = 0
                if not up:
                    up = True
                    if self.log:
                        self.log.log_info("[fm] DB health probe RECOVERED")
                    self._on_change(True)
            else:
                fail += 1
                if up and fail >= self.MAX_FAIL:
                    up = False
                    if self.log:
                        self.log.log_error(f"[fm] DB health probe DOWN ({fail}연속 실패)")
                    self._on_change(False)
        try:
            if conn is not None:
                conn.close()
        except Exception:
            pass


def init(config: dict, node: str, catalog_file: str, module: str = 'csc',
         log=None):
    """config 의 Fm 섹션으로 리포터 기동. 비활성/실패 시 None (호출측 no-op)."""
    global _INSTANCE
    fm = config.get('Fm') or {}
    enable = fm.get('Enable')
    if not (enable is True or str(enable).lower() == 'true'):
        return None
    catalog = {}
    try:
        with open(catalog_file, 'r', encoding='utf-8') as f:
            catalog = json.load(f)
        if log:
            log.log_info(f"[fm] catalog loaded ({catalog_file})")
    except Exception as e:
        if log:
            log.log_error(f"[fm] catalog open failed ({catalog_file}): {e} — 빈 카탈로그로 등록")
    r = FmReporter(fm.get('OamIp', '127.0.0.1'), int(fm.get('OamPort', 9010)),
                   node, module, catalog, int(fm.get('SyncSec', 60)), log=log)
    r.start()
    _INSTANCE = r
    return r


def get():
    """프로세스 싱글턴 (미기동이면 None) — 감사 이벤트 발신 등 후킹 지점용."""
    return _INSTANCE
