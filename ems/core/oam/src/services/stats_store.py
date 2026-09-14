"""집계 저장소 — 포트와 파일 어댑터.

**왜 갈라 두나.** 최종 목표는 "원본 로그를 주기 집계 → **DB** 적재 → 콘솔은 DB 조회" 다.
지금의 파일 구현은 그 자리에 끼워 둔 임시 저장소인데, 그게 공유 NFS 위에서 터졌다 —
두 배포본이 같은 일별 파일을 60초마다 tmp+rename 으로 갈아끼우다 NFSv4 위임 회수가 고착해
그 디렉터리 전체가 잠겼다(2026-09-11 실측). 집계 규칙(무엇을 세는가)과 저장 방식(어디에
어떻게 넣는가)을 갈라 두면 DB 가 정해질 때 **어댑터 하나만 더 쓰면 되고**, 집계·조회 로직은
손대지 않는다.

**주소 단위는 `(unit, bucket, svc)` 다.** 파일 어댑터가 그것을 날짜별 파일로 묶는 건 어댑터
사정이지 계약이 아니다 — 그래서 포트에는 파일 경로가 한 군데도 나오지 않는다. SQL 어댑터는
같은 세 값을 PK 로 두고 upsert 하면 된다. 계약을 이 모양으로 잡아야 이행이 교체로 끝난다.

**단일 writer 는 계약에 포함된다.** "누가 집계를 쓰는가" 를 백엔드가 스스로 정할 수 있어야
한다 — 파일 어댑터는 집계 트리에 flock 을 걸고, SQL 어댑터는 키 upsert 가 멱등이라 사실상
제한이 없다. 이 책임을 호출자에게 떠넘기면 배포본이 하나 늘 때마다 같은 사고가 난다.
"""
from __future__ import annotations

import fcntl
import json
import os
import tempfile
import threading

from util.log_util import Logger

logger = Logger()

# 자기 파일을 갖는 계층. 여기 없는 단위(5m·1w·1y)는 조회 때 접는다.
UNITS = ('1m', '1h', '1d', '1M')
# 날짜가 아니라 **연도 단위**로 묶이는 계층 — 날짜 기준 스위퍼로 다루지 못한다.
PERIOD_UNITS = ('1M',)

_STATE_NAME = '.rollup_state.json'
_LOCK_NAME = '.writer.lock'


def subdir(unit: str) -> str:
    """{root} 아래 그 계층의 상대 경로. 파일 어댑터 전용이지만 보존 스위퍼가 공유한다."""
    return os.path.join('stats', unit if unit in UNITS else '1m')


class FileStatsStore:
    """일별/연도별 JSONL 파일 어댑터.

    `root` 는 `ServiceLogging.Dir` 이고, 집계는 그 아래 `stats/{unit}/...` 에 놓인다.
    쓰기는 전부 원자적 교체(tmp + rename)다 — 읽는 쪽이 반쪽 파일을 보지 않게.
    """

    def __init__(self, root: str):
        self._root = root or ''
        self._lock_fh = None
        self._writer = False
        self._reason = 'not_acquired'

    # ── 경로 (어댑터 내부) ─────────────────────────────────────
    @property
    def root(self) -> str:
        return self._root

    def stats_root(self) -> str:
        return os.path.join(self._root, 'stats') if self._root else ''

    def _day_path(self, unit: str, day: str) -> str:
        if not self._root or len(day) < 10:
            return ''
        return os.path.join(self._root, subdir(unit), day[0:4], day[5:7], day[8:10] + '.jsonl')

    def _period_path(self, unit: str, year: str) -> str:
        if not self._root or unit not in PERIOD_UNITS or len(year) < 4:
            return ''
        return os.path.join(self._root, subdir(unit), year[0:4] + '.jsonl')

    def _state_path(self) -> str:
        r = self.stats_root()
        return os.path.join(r, _STATE_NAME) if r else ''

    # ── 단일 writer ───────────────────────────────────────────
    def acquire_writer(self) -> tuple:
        """집계를 쓸 권한을 잡는다 → (획득 여부, 사유).

        **집계 트리(`{root}/stats`)에** 건다 — 런타임 store 의 소유권 리스와는 다른 축이다.
        런타임 store 는 배포본마다 따로라 배포본 경계를 넘어 중재하지 못한다. 실제로 두
        배포본이 `ServiceLogging.Dir` 만 공유하는 구성에서 양쪽이 같은 일별 파일을 갈아끼우다
        사고가 났다 — 잠금은 **집계 대상 트리 자체**에 걸어야 누가 붙든 한 명만 쓴다.

        flock 은 프로세스가 죽으면 커널이 풀어 준다. 죽은 보유자 때문에 영영 못 잡는 일이
        없어서, 별도의 시한·하트비트가 필요 없다.

        잠금이 강제되지 않는 파일시스템이면 **막지 않고 경고만** 한다 — 거기서 집계를 통째로
        끄면 단일 노드 구성까지 통계를 잃는데, 그건 더 나쁜 실패다.
        """
        if self._writer:
            return True, 'ok'
        r = self.stats_root()
        if not r:
            self._reason = 'no_root'
            return False, self._reason
        try:
            os.makedirs(r, exist_ok=True)
            p = os.path.join(r, _LOCK_NAME)
            fh = open(p, 'a+')
            try:
                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                fh.close()
                self._reason = 'held_by_other'
                return False, self._reason
            try:
                from services import lease
                enforced = lease.locking_enforced(p)
            except Exception:
                enforced = True          # 판정 불가 — 잠금을 믿고 진행한다
            self._lock_fh = fh           # 프로세스 수명 동안 보유
            self._writer = True
            self._reason = 'ok' if enforced else 'locking_not_enforced'
            return True, self._reason
        except OSError as e:
            self._reason = f'acquire_failed:{e}'
            return False, self._reason

    def release_writer(self) -> None:
        if self._lock_fh is not None:
            try:
                fcntl.flock(self._lock_fh.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                self._lock_fh.close()
            except OSError:
                pass
        self._lock_fh = None
        self._writer = False
        self._reason = 'released'

    def is_writer(self) -> bool:
        return self._writer

    # ── 읽기 ──────────────────────────────────────────────────
    def has_day(self, unit: str, day: str) -> bool:
        """그 단위·날에 집계가 **기록된 적이 있는가**.

        행이 0개인 것과 구분해야 한다 — 보존기간에 지워진 날을 되짚기가 되살리면 purge 가
        무의미해진다. SQL 어댑터는 커버리지 행으로 같은 뜻을 낸다.
        """
        p = self._day_path(unit, day)
        return bool(p) and os.path.isfile(p)

    def read_day(self, unit: str, day: str) -> list:
        return self._read_rows(self._day_path(unit, day))

    def read_year(self, unit: str, year: str) -> list:
        return self._read_rows(self._period_path(unit, year))

    # ── 쓰기 ──────────────────────────────────────────────────
    def upsert_day(self, unit: str, day: str, rows: list,
                   replaced_buckets: set, allow_create: bool) -> str:
        """그 날에서 `replaced_buckets` 에 해당하는 기존 행을 지우고 `rows` 를 넣는다.

        반환 `'written'` | `'late'` | `'empty'`.
          late  — 그 날 집계가 없는데 새로 만들면 안 되는 경우(보존기간에 지워진 날).
          empty — 넣을 것도 없고 기존 것도 없어 아무것도 만들지 않음. 빈 날에 0바이트 파일을
                  만들면 "집계된 날" 과 구분이 안 된다.
        """
        p = self._day_path(unit, day)
        if not p:
            return 'empty'
        exists = os.path.isfile(p)
        if not exists and not allow_create:
            return 'late'
        kept = [r for r in self._read_rows(p) if r.get('bucket') not in replaced_buckets]
        merged = kept + list(rows)
        if not merged and not exists:
            return 'empty'
        return 'written' if self._write_rows(p, merged) else 'empty'

    def replace_day(self, unit: str, day: str, rows: list) -> bool:
        """그 날을 통째로 교체. 빈 목록이면 지운다 — 근거 없는 파생 계층을 남기지 않는다."""
        return self._replace(self._day_path(unit, day), rows)

    def replace_year(self, unit: str, year: str, rows: list) -> bool:
        return self._replace(self._period_path(unit, year), rows)

    def purge(self, unit: str, days: int) -> int:
        """그 계층에서 `days` 일을 넘긴 날을 지운다 → 지운 수. 0 이하는 무제한(no-op)."""
        if days <= 0 or not self._root:
            return 0
        from services import daily_jsonl
        return daily_jsonl.purge_old(self._root, subdir(unit), days)

    # ── 상태 (watermark + 미결 호) ────────────────────────────
    def load_state(self) -> dict:
        p = self._state_path()
        if p and os.path.isfile(p):
            try:
                with open(p, 'r', encoding='utf-8', errors='replace') as f:
                    st = json.load(f)
                if isinstance(st, dict):
                    st.setdefault('watermark', '')
                    st.setdefault('open', {})
                    st.setdefault('late_dropped_total', 0)
                    return st
            except (OSError, ValueError):
                pass
        return {'watermark': '', 'open': {}, 'late_dropped_total': 0}

    def save_state(self, st: dict) -> None:
        p = self._state_path()
        if not p:
            return
        try:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix='.tmp.', dir=os.path.dirname(p))
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                json.dump(st, f, ensure_ascii=False)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, p)
        except OSError as e:
            logger.log_error(f"[stats-store] 상태 기록 실패: {e}")

    # ── 파일 입출력 ───────────────────────────────────────────
    @staticmethod
    def _read_rows(p: str) -> list:
        if not p or not os.path.isfile(p):
            return []
        out = []
        try:
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        out.append(json.loads(line))
                    except ValueError:
                        continue
        except OSError:
            return []
        return out

    @staticmethod
    def _write_rows(p: str, rows: list) -> bool:
        try:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix='.tmp.', dir=os.path.dirname(p))
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                for r in sorted(rows, key=lambda x: (x.get('bucket', ''), x.get('svc', ''))):
                    f.write(json.dumps(r, ensure_ascii=False) + '\n')
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, p)
            return True
        except OSError as e:
            logger.log_error(f"[stats-store] {p} 기록 실패: {e}")
            return False

    def _replace(self, p: str, rows: list) -> bool:
        if not p:
            return False
        if not rows:
            if os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
            return True
        return self._write_rows(p, rows)


# ──────────────────────────────────────────────────────────────
#  루트별 인스턴스
# ──────────────────────────────────────────────────────────────
#  같은 루트는 같은 인스턴스를 돌려준다 — writer 잠금이 인스턴스에 매여 있어서, 루트마다
#  하나로 묶지 않으면 한 프로세스가 자기 자신과 잠금을 다투게 된다.
_INSTANCES: dict = {}
_INST_LOCK = threading.Lock()


def for_root(root: str) -> FileStatsStore:
    key = os.path.normpath(root) if root else ''
    with _INST_LOCK:
        st = _INSTANCES.get(key)
        if st is None:
            st = FileStatsStore(root)
            _INSTANCES[key] = st
        return st


def _reset_for_test() -> None:
    """시험 전용 — 루트별 인스턴스 캐시를 비운다(잠금도 함께 푼다)."""
    with _INST_LOCK:
        for st in _INSTANCES.values():
            st.release_writer()
        _INSTANCES.clear()
