"""메시지 통계의 **인터페이스 축** — 네 인터페이스가 같은 집계 피라미드를 타는지.

sip 만 미리 세고 cmp/csc/https 는 조회 때마다 원본을 훑던 시절, 긴 구간 조회는 게이트웨이
상한(5초)에 걸려 504 가 됐다 — 실측(2026-09-21): **하루** https 5.9초, 7일 csc 5.3초로
둘 다 끊겼고 서버는 호출자가 떠난 뒤까지 긁었다. 이 파일은 그 경로를 집계로 옮긴 뒤의
계약을 고정한다.

핵심은 두 가지다.

1. **축이 서로 새지 않는다** — cmp 를 물었는데 sip 건수가 섞여 나오면 안 된다. 접기(1m→1h)
   와 조회 합산(aggregate) 양쪽에서 확인한다.

2. **세대가 모자란 레코드는 안 쓴다** — sip 만 세던 시절의 레코드에는 cmp/csc/https 칸이
   아예 없다. 그걸 그대로 읽으면 **0 건**이 나가고 커버리지는 "덮였다" 고 한다. 없는 것과
   0 건은 다르다(sip_statistics.md §2.1a) — 화면이 정상 조회한 얼굴로 거짓을 말하는 것이
   이 축에서 가장 위험한 고장이다. 그래서 레코드에 세대(`v`)를 적고, sip 외 조회는 세대가
   모자란 행을 건너뛴다.

sys.path 는 ems/core/oam/{src,vendor} — test_stats_rollup_range.py 와 동일.

  python3 tests/test_stats_msg_iface.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from services import stats_rollup as R  # noqa: E402
from services import stats_store as S  # noqa: E402

DAY = '2026-09-20'


def _rec(bucket, svc='unknown', v=R.RECORD_VERSION, sip=None, iface=None):
    """저장 레코드 하나. `v=1` 로 주면 인터페이스 축 이전 세대를 흉내낸다."""
    msg = {'in': dict(sip or {}), 'out': {}}
    if iface:
        msg['iface'] = {k: {'in': dict(c), 'out': {}} for k, c in iface.items()}
    r = {'bucket': bucket, 'unit': '1m', 'svc': svc, 'call': {}, 'msg': msg,
         'open': 0, 'late_dropped': 0}
    if v is not None:
        r['v'] = v
    return r


class TestIfaceAccessor(unittest.TestCase):
    """`msg_io` 하나가 축의 자리를 안다 — 읽는 자리마다 분기를 두면 한 군데를 빠뜨렸을 때
    조용히 0 이 된다."""

    def test_sip_은_루트_자리다(self):
        msg = {'in': {'INVITE': 3}, 'out': {'INVITE/200': 3}}
        self.assertEqual(R.msg_io(msg, 'sip').get('in'), {'INVITE': 3})

    def test_그_밖은_iface_아래다(self):
        msg = {'in': {'INVITE': 3}, 'iface': {'cmp': {'in': {'HEARTBEAT': 9}}}}
        self.assertEqual(R.msg_io(msg, 'cmp').get('in'), {'HEARTBEAT': 9})

    def test_없는_칸은_빈_dict(self):
        self.assertEqual(R.msg_io({'in': {'INVITE': 1}}, 'https'), {})
        self.assertEqual(R.msg_io(None, 'cmp'), {})

    def test_쓰기용_자리는_만들어_준다(self):
        msg = {'in': {}, 'out': {}}
        R.msg_io_slot(msg, 'csc')['in']['USER_CHANGED'] = 2
        self.assertEqual(msg['iface']['csc']['in'], {'USER_CHANGED': 2})
        # sip 은 루트 그 자체라 iface 칸을 만들지 않는다
        R.msg_io_slot(msg, 'sip')['in']['INVITE'] = 1
        self.assertEqual(msg['in'], {'INVITE': 1})


class TestGeneration(unittest.TestCase):
    """세대 판정 — 이게 틀리면 옛 구간이 0 건으로 위장된다."""

    def test_세대가_없으면_1(self):
        self.assertEqual(R.record_version({'bucket': 'x'}), 1)
        self.assertEqual(R.record_version({'v': 'zzz'}), 1)   # 깨진 값도 안전하게

    def test_sip_은_세대와_무관하다(self):
        """sip 은 이 축의 기저라 레코드가 있으면 언제나 센 것이다."""
        self.assertTrue(R.covers_iface({'v': 1}, 'sip'))

    def test_sip_외는_세대_2_부터(self):
        self.assertFalse(R.covers_iface({'v': 1}, 'cmp'))
        self.assertFalse(R.covers_iface({}, 'https'))
        self.assertTrue(R.covers_iface({'v': 2}, 'cmp'))


class TestFold(unittest.TestCase):
    """1m → 1h 접기."""

    def test_축이_서로_새지_않는다(self):
        rows = [
            _rec(f'{DAY} 10:00', sip={'INVITE': 2}, iface={'cmp': {'RELAY_ADD': 5}}),
            _rec(f'{DAY} 10:30', sip={'INVITE': 3}, iface={'cmp': {'RELAY_ADD': 7},
                                                           'https': {'GET': 11}}),
        ]
        out = R.fold_records(rows, '1h')
        self.assertEqual(len(out), 1)
        msg = out[0]['msg']
        self.assertEqual(R.msg_io(msg, 'sip').get('in'), {'INVITE': 5})
        self.assertEqual(R.msg_io(msg, 'cmp').get('in'), {'RELAY_ADD': 12})
        self.assertEqual(R.msg_io(msg, 'https').get('in'), {'GET': 11})
        self.assertEqual(R.msg_io(msg, 'csc'), {})     # 자료 없는 칸은 안 만든다

    def test_세대는_가장_낮은_것을_따른다(self):
        """세대 1 이 한 줄이라도 섞이면 그 합은 인터페이스 축을 온전히 덮지 못한다.
        높은 쪽으로 적으면 그 구멍이 0 으로 위장되고, 접힌 뒤에는 되물을 수 없다."""
        rows = [_rec(f'{DAY} 10:00', v=1, sip={'INVITE': 2}),
                _rec(f'{DAY} 10:30', v=2, sip={'INVITE': 3},
                     iface={'cmp': {'RELAY_ADD': 7}})]
        out = R.fold_records(rows, '1h')
        self.assertEqual(R.record_version(out[0]), 1)
        self.assertFalse(R.covers_iface(out[0], 'cmp'))

    def test_전부_새_세대면_유지된다(self):
        rows = [_rec(f'{DAY} 10:00'), _rec(f'{DAY} 10:30')]
        self.assertEqual(R.record_version(R.fold_records(rows, '1h')[0]), 2)


class TestEmptyRecord(unittest.TestCase):
    def test_인터페이스만_있어도_빈_레코드가_아니다(self):
        """SIP 이 조용한 분에도 콘솔 폴링(https)·CMP 제어는 오간다 — 여기서 빈 것으로
        보면 그 분이 통째로 안 적히고, 조회는 0 건을 낸다."""
        rec = R._empty(f'{DAY} 10:00', 'unknown')
        R.msg_io_slot(rec['msg'], 'https')['in']['GET'] = 4
        self.assertFalse(R._is_empty(rec))

    def test_정말_빈_것은_빈_것이다(self):
        self.assertTrue(R._is_empty(R._empty(f'{DAY} 10:00', 'unknown')))


class TestAggregate(unittest.TestCase):
    """조회 합산 — `include_msg` 가 인터페이스 축까지 싣는지."""

    def test_합산이_축을_보존한다(self):
        rows = [_rec(f'{DAY} 10:00', svc='volte', sip={'INVITE': 2},
                     iface={'cmp': {'RELAY_ADD': 5}}),
                _rec(f'{DAY} 10:01', svc='ptt', sip={'INVITE': 1},
                     iface={'cmp': {'RELAY_ADD': 3}})]
        _buckets, totals = R.aggregate(rows, '1h', 'all', include_msg=True)
        allm = totals['all']['msg']
        self.assertEqual(R.msg_io(allm, 'sip').get('in'), {'INVITE': 3})
        self.assertEqual(R.msg_io(allm, 'cmp').get('in'), {'RELAY_ADD': 8})
        # 서비스 칸도 자기 몫만
        self.assertEqual(R.msg_io(totals['volte']['msg'], 'cmp').get('in'),
                         {'RELAY_ADD': 5})


class TestReadRangeFiltersOldGeneration(unittest.TestCase):
    """**이 시험이 이 파일의 핵심이다.**

    세대 1 레코드만 있는 구간을 cmp 로 물으면, 그 구간은 `missing_days` 로 나와야 한다.
    0 건으로 나가면 화면이 "그 시간엔 CMP 통신이 한 건도 없었다" 고 말하는데 실제로는
    센 적이 없는 것이다."""

    def setUp(self):
        self.site = tempfile.mkdtemp(prefix='statsiface')
        self.roots = R.roots_of({'CimsSiteDir': self.site})
        S.reset_cache() if hasattr(S, 'reset_cache') else None

    def tearDown(self):
        shutil.rmtree(self.site, ignore_errors=True)

    def _seed(self, v):
        rows = [_rec(f'{DAY} {h:02d}:{m:02d}', v=v, sip={'INVITE': 1},
                     iface=({'cmp': {'RELAY_ADD': 1}} if v == 2 else None))
                for h in range(24) for m in range(60)]
        S.for_root(self.roots.stats).replace_day('1m', DAY, rows)

    def test_옛_세대는_sip_에는_쓰이고_cmp_에는_안_쓰인다(self):
        self._seed(v=1)
        # 원본 날 디렉터리를 두지 않는다 — 즉석 집계로 채울 수 없으니 '모름' 이 되어야 한다
        f, t = f'{DAY} 00:00:00', f'{DAY} 23:59:59'

        rows, cov = R.read_range_filled(self.roots, f, t, {}, gran='1h')
        self.assertTrue(rows, 'sip 조회는 옛 세대도 그대로 쓴다')
        self.assertEqual(cov['missing'], 0)

        rows, cov = R.read_range_filled(
            self.roots, f, t, {}, gran='1h',
            row_ok=lambda r: R.covers_iface(r, 'cmp'))
        self.assertEqual(rows, [], 'cmp 조회는 세대가 모자란 행을 안 쓴다')
        self.assertEqual(cov['missing_days'], [DAY],
                         '못 쓴 구간은 0 이 아니라 모름으로 신고돼야 한다')

    def test_새_세대는_cmp_에도_쓰인다(self):
        self._seed(v=2)
        rows, cov = R.read_range_filled(
            self.roots, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h',
            row_ok=lambda r: R.covers_iface(r, 'cmp'))
        self.assertEqual(len(rows), 1440)
        self.assertEqual(cov['missing'], 0)
        _b, totals = R.aggregate(rows, '1d', 'all', include_msg=True)
        self.assertEqual(R.msg_io(totals['all']['msg'], 'cmp').get('in'),
                         {'RELAY_ADD': 1440})


class TestHttpsScan(unittest.TestCase):
    """HTTPS 는 자기 원문 로그가 없어 flow 로그에서 센다."""

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='statshttps')     # SIP/Flow 5분 버킷 루트(<log>/sip) 자리
        self.hdir = os.path.join(self.root, '2026', '09', '20', '10')
        os.makedirs(self.hdir)

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _flow(self, name, entries):
        with open(os.path.join(self.hdir, name), 'w') as f:
            for e in entries:
                f.write(json.dumps(e) + '\n')

    def test_HTTPS_만_세고_응답은_동사까지_붙인다(self):
        self._flow('oam_01.flow.jsonl', [
            {'ts': '10:00:01', 'proto': 'HTTPS', 'method': 'GET /api/v1/x',
             'detail': 'status=200'},
            {'ts': '10:00:02', 'proto': 'HTTPS', 'method': 'POST /api/v1/y',
             'detail': 'status=200'},
            {'ts': '10:00:03', 'proto': 'SIP', 'method': 'INVITE'},
        ])
        got = R._scan_https_hour(self.root, '2026-09-20 10')
        cell = got['2026-09-20 10:00']['unknown']
        self.assertEqual(cell['in'], {'GET': 1, 'POST': 1})
        # 코드만 세면 조회 성공과 변경 성공이 한 칸에 합쳐져 세어도 쓸 수 없다(§2.4)
        self.assertEqual(cell['out'], {'GET/200': 1, 'POST/200': 1})

    def test_깨진_줄은_그_줄만_버린다(self):
        """한 줄의 손상에 그 시간 전체가 날아가면 watermark 가 영구히 멈춘다."""
        with open(os.path.join(self.hdir, 'oam_01.flow.jsonl'), 'w') as f:
            f.write('{"proto": "HTTPS" 깨짐\n')
            f.write(json.dumps({'ts': '10:00:05', 'proto': 'HTTPS',
                                'method': 'GET /z', 'detail': 'status=404'}) + '\n')
        got = R._scan_https_hour(self.root, '2026-09-20 10')
        self.assertEqual(got['2026-09-20 10:00']['unknown']['out'], {'GET/404': 1})


class TestIfaceList(unittest.TestCase):
    def test_네_인터페이스가_다_집계_대상이다(self):
        """화면이 고를 수 있는 것과 집계가 아는 것이 어긋나면 그 축만 조용히 옛 경로로
        떨어져 다시 504 가 된다."""
        self.assertEqual(set(R.MSG_IFACES), {'sip', 'cmp', 'csc', 'https'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
