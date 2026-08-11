"""FM ingest — 모듈 알람/이벤트 자기보고 수신 (alarm_self_reporting.md).

서비스 모듈(csp/cmp/cmdp/csc)이 UDP JSON envelope v2 로 push 하는 FM_* 를 수신한다:
  FM_REGISTER  카탈로그 등록 + boot_id (+초기 활성목록 = 첫 sync 겸용)
  FM_ALARM     알람 open/close 실시간 통지
  FM_EVENT     정상 동작 이벤트(stateChange/audit) → event_log
  FM_SYNC      활성 알람 전량 동기화 (주기 reconcile — 32.111 alarm sync 대응)

소유는 oam-svc (서비스 관측 소유 — oam_base_service_split §4), --role all 단일
프로세스에서는 base(oam_app)가 대행 — sweeper 소유 규약과 동일.

활성 알람의 SoT 는 모듈, 여기는 미러 — 통지 유실·양측 재기동은 FM_SYNC 가 수렴시킨다.
활성키/alarm_id/재통지 의미는 alarm_standardization.md §3.4 그대로 (transition 코어 재사용).
detected_by = 'self' (주체 클래스만 — 발신 노드는 envelope hdr.node 와 mo_instance
(<서버명>/<module>[/<component>], 서버명 = hdr.node)가 보유. 표준화 §3.4(b)).
구 wire(코드 CIMS-*, mo cims/<module>/<node>[/...])는 수신 시 현행 정의 코드·서버명
루트로 정규화해 흡수한다 (배포 스큐 — 구 모듈 + 신 OAM 구간).

신뢰성 (cmp_media_api.md §8 과 동일 계약):
  - ack = 동일 trans_id 의 type:"response" (발신측 재전송 1s×5).
  - 재전송 중복은 (node, trans_id) LRU 로 흡수 — 캐시된 응답 재송.
  - 순서는 payload.seq(boot 당 단조증가) — 활성키별 더 작은 seq 의 open/close 폐기.
  - 미등록 node/boot_id 의 통지는 ERROR code=UNREGISTERED → 발신측이 FM_REGISTER 재송.
"""

import json
import socket
import threading
import time
from collections import OrderedDict
from datetime import datetime

_MAX_DGRAM = 4096          # envelope v2 §1.2 — 데이터그램 최대 4KB
_DEDUP_MAX = 1024          # (node, trans_id) 응답 캐시 크기
_STALE_SYNC_MISSES = 3     # sync 연속 누락 임계 — 초과 시 판정 불가 종결 (표준화 §3.4(d))

# 카탈로그 보존 — {ServiceLogDir}/fm_catalog/<node>.json. alert/event 스트림과 같은
# 서비스 로그 영역(공유 스토리지)에 둔다: 관리 store(file_store)는 소유권 리스가 있는
# base 단일 writer 라 oam-svc(FM ingest 소유자)가 쓸 수 없고, 여기는 oam-svc 가
# 이미 쓰기 소유자다. 모듈 다운/OAM 절체 중에도 카탈로그 조회·판정이 유지된다.
_CATALOG_SUBDIR = 'fm_catalog'

FM_CMDS = ('FM_REGISTER', 'FM_ALARM', 'FM_EVENT', 'FM_SYNC')


def _now_iso() -> str:
    return datetime.now().isoformat(timespec='seconds')


def _current_code(code):
    """구 wire 코드(CIMS-*) → 현행 정의 코드 alias (표준화 §3.4(a) — 배포 스큐 흡수)."""
    if not code:
        return code
    from services import service_registry
    return service_registry.current_code(code)


def _normalize_mo(mo):
    """구 wire mo(cims/<module>/<node>[/<comp>]) → 현행 서버명 루트
    (<node>/<module>[/<comp>], 표준화 §3.4(b)). 현행 형식은 그대로 통과."""
    if not mo or not mo.startswith('cims/'):
        return mo
    seg = mo.split('/')
    if len(seg) >= 3:
        return '/'.join([seg[2], seg[1]] + seg[3:])
    return mo


class FmIngest:
    """UDP 수신 스레드 1개 — 수신 루프 안에서 소켓 타임아웃 주기로 stale 점검도 수행."""

    def __init__(self, config: dict, service_log_dir: str, log=None):
        self.config = config
        self.dir = service_log_dir
        self.log = log
        fm = config.get('FmIngest') or {}
        self.ip = fm.get('Ip', '0.0.0.0')
        self.port = int(fm.get('Port', 9010))
        self.sync_sec = int(fm.get('SyncSec', 60))
        self.node_id = config.get('SystemId', 'oam')
        # nodes: node -> {'boot_id', 'module', 'akeys': set, 'seq': {akey: int},
        #                 'last_sync': epoch}
        self.nodes: dict = {}
        self.catalogs: dict = {}          # node -> {'module', 'alarms': {code: rule}, 'events': {type: def}}
        self._responses: OrderedDict = OrderedDict()   # (node, trans_id) -> bytes
        self.state: dict = {}             # akey -> alarm_id (자기보고 계열만)
        self._sock = None
        self._thread = None
        self._stop = threading.Event()

    # ── lifecycle ────────────────────────────────────────────────────────

    def start(self):
        self._restore()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.ip, self.port))
        self._sock.settimeout(1.0)
        self._thread = threading.Thread(target=self._serve, name='fm-ingest', daemon=True)
        self._thread.start()
        self._log_info(f"[fm] ingest listening on {self.ip}:{self.port} "
                       f"(sync={self.sync_sec}s)")

    def stop(self):
        self._stop.set()
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass

    def _restore(self):
        """기동 시 복원 — alert_log replay 에서 자기보고(detected_by=self) 계열만.
        node 별 akey 집합도 재구성해 이후 FM_SYNC reconcile 이 이어받는다."""
        from services import alert_log
        try:
            meta = alert_log.compute_open_state(self.dir, with_meta=True)
        except Exception as e:
            self._log_error(f"[fm] restore failed: {e}")
            meta = {}
        now = time.time()
        for akey, m in meta.items():
            db = m.get('detected_by') or ''
            if db != 'self' and not db.startswith('self:'):
                continue
            # 발신 노드 — 구 레코드는 detected_by 접미(self:<node>), 현행 mo 루트는
            # 서버명(= envelope hdr.node) 이므로 첫 세그먼트. 구 mo 형식
            # (cims/<module>/<node>[/...])만 3번째 세그먼트. 도출 불가 시 mo 자체를
            # 노드 키로 두면 sync 부재 → stale 종결로 자가 정리된다.
            mo = akey.split('@', 1)[1] if '@' in akey else ''
            if db.startswith('self:'):
                node = db.split(':', 1)[1]
            else:
                seg = mo.split('/')
                if seg and seg[0] == 'cims':
                    node = seg[2] if len(seg) >= 3 else (mo or akey)
                else:
                    node = seg[0] if seg and seg[0] else (mo or akey)
            self.state[akey] = {'alarm_id': m['alarm_id'],
                                'severity': m.get('perceived_severity')}
            ent = self.nodes.setdefault(node, {'boot_id': None, 'module': '',
                                               'akeys': set(), 'seq': {}, 'last_sync': now})
            ent['akeys'].add(akey)
        if self.state:
            self._log_info(f"[fm] restored open state (self): {sorted(self.state.keys())}")
        # 카탈로그 복원 (모듈 다운 중에도 조회/판정 가능)
        for row in module_catalogs(self.dir):
            self.catalogs[row['node']] = self._index_catalog(row)

    # ── 수신 루프 ────────────────────────────────────────────────────────

    def _serve(self):
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(_MAX_DGRAM)
            except socket.timeout:
                self._check_stale()
                continue
            except OSError:
                break
            try:
                resp = self._handle(data)
            except Exception as e:
                self._log_error(f"[fm] handle error from {addr}: {e}")
                resp = None
            if resp:
                try:
                    self._sock.sendto(resp, addr)
                except Exception:
                    pass

    def _handle(self, data: bytes):
        try:
            msg = json.loads(data.decode('utf-8'))
            hdr = msg.get('hdr') or {}
        except Exception:
            return None    # JSON 아님 — 응답 불가(trans_id 미상)
        cmd, node, tid = hdr.get('cmd'), hdr.get('node'), hdr.get('trans_id')
        if hdr.get('ver') != 2 or hdr.get('type') != 'event' or cmd not in FM_CMDS \
                or not node or tid is None:
            return self._resp_err(hdr, 'BAD_REQUEST', 'invalid FM envelope')
        # 재전송 중복 — 캐시된 응답 재송 (처리 재실행 없음)
        cached = self._responses.get((node, tid))
        if cached is not None:
            return cached
        payload = msg.get('payload') or {}
        if cmd == 'FM_REGISTER':
            resp = self._on_register(node, payload)
        else:
            ent = self.nodes.get(node)
            if ent is None or ent.get('boot_id') != payload.get('boot_id'):
                # 미등록/재기동 후 미등록 — 발신측이 FM_REGISTER 부터 다시
                resp = self._resp_err(hdr, 'UNREGISTERED', 'register first')
                return self._cache_resp(node, tid, resp)
            if cmd == 'FM_ALARM':
                resp = self._on_alarm(node, ent, hdr, payload)
            elif cmd == 'FM_EVENT':
                resp = self._on_event(node, ent, hdr, payload)
            else:
                resp = self._on_sync(node, ent, payload) or self._resp_ok(hdr)
        if not isinstance(resp, bytes):
            resp = self._resp_ok(hdr, resp if isinstance(resp, dict) else None)
        return self._cache_resp(node, tid, resp)

    # ── cmd 처리 ─────────────────────────────────────────────────────────

    def _on_register(self, node: str, payload: dict):
        boot_id = payload.get('boot_id')
        module = payload.get('module') or node
        catalog = payload.get('catalog') or {}
        rebooted = node in self.nodes and self.nodes[node].get('boot_id') not in (None, boot_id)
        ent = self.nodes.setdefault(node, {'akeys': set(), 'seq': {}})
        ent.update({'boot_id': boot_id, 'module': module, 'last_sync': time.time()})
        if rebooted:
            ent['seq'] = {}
        self.catalogs[node] = self._index_catalog({'node': node, 'module': module, **catalog})
        try:
            self._persist_catalog(node, {'node': node, 'module': module,
                                         'updated_at': _now_iso(),
                                         'alarms': catalog.get('alarms') or [],
                                         'events': catalog.get('events') or []})
        except Exception as e:
            self._log_error(f"[fm] catalog persist failed ({node}): {e}")
        self._log_info(f"[fm] REGISTER {node} (module={module}, boot_id={boot_id}, "
                       f"alarms={len(catalog.get('alarms') or [])}, "
                       f"events={len(catalog.get('events') or [])})"
                       + (' — reboot' if rebooted else ''))
        # 초기 활성목록 = 첫 sync 겸용 (재기동 stale 정리 포함)
        self._reconcile(node, ent, payload.get('active') or [])
        return {'sync_interval_sec': self.sync_sec}

    def _on_alarm(self, node: str, ent: dict, hdr: dict, payload: dict):
        action = payload.get('action')
        code = _current_code(payload.get('code'))
        mo = _normalize_mo(payload.get('mo_instance'))
        if action not in ('open', 'close') or not code or not mo:
            return self._resp_err(hdr, 'BAD_REQUEST', 'action/code/mo_instance required')
        rule = (self.catalogs.get(node) or {}).get('alarms', {}).get(code)
        if not rule:
            return self._resp_err(hdr, 'UNKNOWN_CODE', f'code {code} not in catalog')
        akey = f"{code}@{mo}"
        seq = payload.get('seq')
        if isinstance(seq, int):
            last = ent['seq'].get(akey)
            if last is not None and seq <= last:
                return None    # UDP 역전 — 폐기(ack 는 OK)
            ent['seq'][akey] = seq
        self._transition(node, ent, rule, mo, action == 'open',
                         params=payload.get('params') or {},
                         severity=payload.get('perceived_severity'),
                         message=payload.get('message'))
        return None

    def _on_event(self, node: str, ent: dict, hdr: dict, payload: dict):
        etype = payload.get('type')
        if not etype:
            return self._resp_err(hdr, 'BAD_REQUEST', 'type required')
        from services import event_log
        cat = (self.catalogs.get(node) or {}).get('events', {}).get(etype) or {}
        # mo 규칙: <서버명>/<module>[/<component>] — 서버명 = hdr.node (자기보고 §4).
        mo = _normalize_mo(payload.get('mo_instance')) \
            or f"{node}/{ent.get('module') or node}"
        params = payload.get('params') or {}
        from services.alarm_sweeper import fmt
        message = payload.get('message') or fmt(cat.get('msg', ''), **{**params, 'mo': mo}) or etype
        rec = {
            'ts': payload.get('ts') or _now_iso(),
            'type': etype,
            'kind': payload.get('kind') or cat.get('kind') or 'stateChange',
            'source': {'mo_class': cat.get('mo_class') or 'software',
                       'mo_instance': mo, 'detected_by': 'self'},
            'message': message,
            'params': params,
        }
        if cat.get('code'):
            rec['code'] = cat['code']   # 이벤트 정의 코드 (표준화 §3.6 — type 슬러그는 유지)
        event_log.record_event(self.dir, rec)
        return None

    def _on_sync(self, node: str, ent: dict, payload: dict):
        ent['last_sync'] = time.time()
        self._reconcile(node, ent, payload.get('active') or [])
        return None

    # ── 알람 전이/수렴 ───────────────────────────────────────────────────

    def _transition(self, node: str, ent: dict, rule: dict, mo: str, is_open: bool,
                    params: dict = None, severity: str = None, message: str = None):
        from services import alarm_sweeper
        r = dict(rule)
        if severity:
            r['perceived_severity'] = severity
        if not r.get('perceived_severity'):
            # X.733: 심각도 미지정은 indeterminate — 통지·카탈로그 둘 다 없을 때 warning
            # 으로 임의 판정하지 않는다 (emit_alarm 기본값 대체).
            r['perceived_severity'] = 'indeterminate'
        kw = {**(params or {}), 'mo': mo}
        msg_open = message or alarm_sweeper.fmt(r.get('msg_open', ''), **kw) \
            or f"{mo} {r.get('type')}"
        msg_close = message or alarm_sweeper.fmt(r.get('msg_close', ''), **kw) \
            or f"{mo} 정상화"
        akey = f"{r.get('code')}@{mo}"
        was = akey in self.state
        alarm_sweeper.transition(self.state, self.dir, r, mo, 'self',
                                 is_open, msg_open, msg_close, log=self.log)
        if is_open and not was:
            ent['akeys'].add(akey)
        elif not is_open and was:
            ent['akeys'].discard(akey)

    def _reconcile(self, node: str, ent: dict, active: list):
        """FM_SYNC/REGISTER 의 활성목록으로 수렴 — 목록에 없는 열림은 close,
        목록에 있는데 안 열린 것은 open (유실/재기동 복구)."""
        cat = (self.catalogs.get(node) or {}).get('alarms', {})
        want = {}
        for a in active:
            code = _current_code(a.get('code'))
            mo = _normalize_mo(a.get('mo_instance'))
            if code and mo and code in cat:
                want[f"{code}@{mo}"] = {**a, 'code': code, 'mo_instance': mo}
        for akey in sorted(want.keys() - ent['akeys']):
            a = want[akey]
            rule = cat[a['code']]
            self._transition(node, ent, rule, a['mo_instance'], True,
                             params=a.get('params') or {})
        for akey in sorted(ent['akeys'] - want.keys()):
            code, mo = akey.split('@', 1)
            rule = cat.get(code)
            if rule:
                self._transition(node, ent, rule, mo, False)
            else:   # 카탈로그에서 사라진 클래스 — 최소 정보로 종결
                self._transition(node, ent,
                                 {'code': code, 'type': code, 'msg_close': '{mo} 정리'},
                                 mo, False)

    def _check_stale(self):
        """sync 연속 누락 노드의 자기보고 활성 알람을 판정 불가로 종결 (표준화 §3.4(d)).
        프로세스 생존은 L1(agent process_down)이, 응답성은 L3(service_unresponsive)가
        별도 판정한다 (표준화 §3.4(b))."""
        limit = self.sync_sec * _STALE_SYNC_MISSES
        now = time.time()
        for node, ent in self.nodes.items():
            if not ent['akeys'] or now - ent.get('last_sync', now) <= limit:
                continue
            self._log_info(f"[fm] {node} sync 두절 {int(now - ent['last_sync'])}s — "
                           f"활성 {len(ent['akeys'])}건 판정 불가 종결")
            cat = (self.catalogs.get(node) or {}).get('alarms', {})
            for akey in sorted(ent['akeys']):
                code, mo = akey.split('@', 1)
                rule = cat.get(code) or {'code': code, 'type': code}
                self._transition(node, ent, dict(rule), mo, False,
                                 message=f"{mo} 자기보고 두절 — 판정 불가 종결")
            ent['last_sync'] = now    # 반복 종결 방지

    # ── helpers ──────────────────────────────────────────────────────────

    def _persist_catalog(self, node: str, row: dict):
        import os
        import tempfile
        d = os.path.join(self.dir, _CATALOG_SUBDIR)
        os.makedirs(d, exist_ok=True)
        safe = ''.join(c if (c.isalnum() or c in '-_.') else '_' for c in node)
        path = os.path.join(d, f"{safe}.json")
        fd, tmp = tempfile.mkstemp(prefix='.tmp_', dir=d)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                json.dump(row, f, ensure_ascii=False, indent=2)
            os.replace(tmp, path)
        except Exception:
            try:
                os.unlink(tmp)
            except FileNotFoundError:
                pass
            raise

    @staticmethod
    def _index_catalog(row: dict) -> dict:
        alarms = {}
        for a in (row.get('alarms') or []):
            if isinstance(a, dict) and a.get('code'):
                a = dict(a)
                # 구 카탈로그(구 모듈/보존본) 흡수 — 코드 개정 alias + 클래스 개명.
                a['code'] = _current_code(a['code'])
                if a.get('type') == 'resource_failure':
                    a['type'] = 'storage_failure'
                alarms[a['code']] = a
        events = {}
        for e in (row.get('events') or []):
            if isinstance(e, dict) and e.get('type'):
                events[e['type']] = e
        return {'module': row.get('module') or '', 'alarms': alarms, 'events': events}

    def _resp_hdr(self, hdr: dict, status: str) -> dict:
        return {'ver': 2, 'trans_id': hdr.get('trans_id'), 'node': self.node_id,
                'cmd': hdr.get('cmd'), 'type': 'response', 'status': status}

    def _resp_ok(self, hdr: dict, payload: dict = None) -> bytes:
        msg = {'hdr': self._resp_hdr(hdr, 'OK')}
        if payload:
            msg['payload'] = payload
        return json.dumps(msg, ensure_ascii=False).encode('utf-8')

    def _resp_err(self, hdr: dict, code: str, reason: str) -> bytes:
        h = self._resp_hdr(hdr, 'ERROR')
        h.update({'code': code, 'reason': reason})
        return json.dumps({'hdr': h}, ensure_ascii=False).encode('utf-8')

    def _cache_resp(self, node, tid, resp: bytes) -> bytes:
        self._responses[(node, tid)] = resp
        while len(self._responses) > _DEDUP_MAX:
            self._responses.popitem(last=False)
        return resp

    def _log_info(self, m):
        if self.log:
            self.log.log_info(m)

    def _log_error(self, m):
        if self.log:
            self.log.log_error(m)


def module_catalogs(service_log_dir: str) -> list:
    """등록된 모듈 카탈로그 목록 ({ServiceLogDir}/fm_catalog/ 보존본) — /alerts/catalog 병합
    + ingest 재기동 복원용. 구 모듈이 남긴 보존본의 구 코드/클래스명은 read 시 현행으로
    정규화한다 (모듈 재등록 시 파일도 현행으로 교체됨)."""
    import glob
    import os
    out = []
    if not service_log_dir:
        return out
    for path in sorted(glob.glob(os.path.join(service_log_dir, _CATALOG_SUBDIR, '*.json'))):
        if os.path.basename(path).startswith('.'):
            continue
        try:
            with open(path, 'r', encoding='utf-8') as f:
                row = json.load(f)
            if isinstance(row, dict) and row.get('node'):
                for a in (row.get('alarms') or []):
                    if isinstance(a, dict) and a.get('code'):
                        a['code'] = _current_code(a['code'])
                        if a.get('type') == 'resource_failure':
                            a['type'] = 'storage_failure'
                out.append(row)
        except Exception:
            continue
    return out
