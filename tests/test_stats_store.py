"""집계 저장소(services/stats_store) 계약 시험.

가장 중요한 건 **단일 writer** 다. 같은 집계 대상을 둘이 쓰면 결과가 서로를 덮고, 공유
파일시스템에서는 그 경합이 노드를 통째로 멈춰 세운다 — 두 배포본이 `ServiceLogging.Dir`
만 공유하는 구성에서 실제로 그렇게 됐다(2026-09-11: NFSv4 위임 회수 고착 → 디렉터리 잠김
→ oam-svc 전 요청 504). 그 회귀를 막는 것이 이 파일의 목적이다.

나머지는 포트 계약 — 파일이든 DB든 같은 답을 내야 하는 규칙들이다.
sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
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

from services import stats_store as S      # noqa: E402
from services import stats_rollup as R     # noqa: E402

DAY = '2026-09-11'


def _rows(*buckets):
    return [{'bucket': b, 'svc': 'volte', 'call': {'attempts': 1}} for b in buckets]


class StoreBase(unittest.TestCase):
    def setUp(self):
        S._reset_for_test()
        self.root = tempfile.mkdtemp(prefix='stats_store_')
        self.st = S.for_root(self.root)

    def tearDown(self):
        S._reset_for_test()
        shutil.rmtree(self.root, ignore_errors=True)


class SingleWriterTest(StoreBase):
    def test_한_명만_잡는다(self):
        ok1, why1 = self.st.acquire_writer()
        self.assertTrue(ok1)
        self.assertEqual(why1, 'ok', 'tmpfs 에서 flock 이 강제되어야 한다')

        other = S.FileStatsStore(self.root)      # 다른 프로세스를 흉내낸 별도 인스턴스
        ok2, why2 = other.acquire_writer()
        self.assertFalse(ok2)
        self.assertEqual(why2, 'held_by_other')

    def test_놓으면_다음이_잡는다(self):
        self.assertTrue(self.st.acquire_writer()[0])
        self.st.release_writer()
        other = S.FileStatsStore(self.root)
        self.assertTrue(other.acquire_writer()[0], '놓았는데 못 잡으면 죽은 보유자가 영구 점유')
        other.release_writer()

    def test_같은_루트는_같은_인스턴스(self):
        self.assertIs(S.for_root(self.root), S.for_root(self.root),
                      '루트마다 하나로 묶지 않으면 자기 자신과 잠금을 다툰다')

    def test_두_번_잡아도_된다(self):
        self.assertTrue(self.st.acquire_writer()[0])
        self.assertEqual(self.st.acquire_writer(), (True, 'ok'), '재진입은 무해해야 한다')


class RollupWriterGateTest(StoreBase):
    def test_권한_없으면_집계를_건너뛴다(self):
        holder = S.FileStatsStore(self.root)     # 다른 배포본이 먼저 쥔 상황
        self.assertTrue(holder.acquire_writer()[0])
        try:
            R._WRITER_NOTED['reason'] = None
            R.init(self.root, {}, enabled=True)
            out = R.run_once()
            self.assertIn('skipped', out)
            self.assertTrue(out['skipped'].startswith('not_writer'), out)
        finally:
            holder.release_writer()
            R.init('', {}, enabled=False)

    def test_권한_없으면_수동_재생성도_안_한다(self):
        holder = S.FileStatsStore(self.root)
        self.assertTrue(holder.acquire_writer()[0])
        try:
            R._WRITER_NOTED['reason'] = None
            R.init(self.root, {}, enabled=True)
            self.assertEqual(R.rebuild_range(DAY, DAY), 0)
        finally:
            holder.release_writer()
            R.init('', {}, enabled=False)

    def test_권한이_없어도_조회는_된다(self):
        """집계를 안 쓰는 쪽도 상대가 만든 값을 그대로 읽어야 한다."""
        self.st.replace_day('1h', DAY, _rows(f'{DAY} 00:00', f'{DAY} 01:00'))
        holder = S.FileStatsStore(self.root)
        self.assertTrue(holder.acquire_writer()[0])
        try:
            rows, cov = R.read_range_filled(
                self.root, f'{DAY} 00:00:00', f'{DAY} 01:59:59', {}, gran='1h')
            self.assertEqual(len(rows), 2)
            self.assertEqual(cov['by_unit'].get('1h'), 1)
        finally:
            holder.release_writer()


class UpsertContractTest(StoreBase):
    def test_지정한_버킷만_갈아끼운다(self):
        self.st.replace_day('1m', DAY, _rows(f'{DAY} 00:00', f'{DAY} 00:01'))
        r = self.st.upsert_day('1m', DAY,
                               _rows(f'{DAY} 00:01'), {f'{DAY} 00:01'}, allow_create=True)
        self.assertEqual(r, 'written')
        got = sorted(x['bucket'] for x in self.st.read_day('1m', DAY))
        self.assertEqual(got, [f'{DAY} 00:00', f'{DAY} 00:01'], '건드리지 않은 버킷은 남아야 한다')

    def test_결과가_비면_그_버킷은_사라진다(self):
        """되짚기로 값이 줄어든 경우 옛 행이 남으면 이중 계산된다."""
        self.st.replace_day('1m', DAY, _rows(f'{DAY} 00:00', f'{DAY} 00:01'))
        self.st.upsert_day('1m', DAY, [], {f'{DAY} 00:01'}, allow_create=True)
        got = [x['bucket'] for x in self.st.read_day('1m', DAY)]
        self.assertEqual(got, [f'{DAY} 00:00'])

    def test_없는_날은_만들지_않는다(self):
        """보존기간에 지워진 날을 되짚기가 되살리면 purge 가 무의미해진다."""
        r = self.st.upsert_day('1m', DAY, _rows(f'{DAY} 00:00'), {f'{DAY} 00:00'},
                               allow_create=False)
        self.assertEqual(r, 'late')
        self.assertFalse(self.st.has_day('1m', DAY))

    def test_빈_날에_빈_집계를_만들지_않는다(self):
        r = self.st.upsert_day('1m', DAY, [], {f'{DAY} 00:00'}, allow_create=True)
        self.assertEqual(r, 'empty')
        self.assertFalse(self.st.has_day('1m', DAY), '있는 것 자체가 "집계된 날" 이라는 뜻이다')

    def test_has_day_는_집계_유무를_가른다(self):
        self.assertFalse(self.st.has_day('1m', DAY))
        self.st.replace_day('1m', DAY, _rows(f'{DAY} 00:00'))
        self.assertTrue(self.st.has_day('1m', DAY))
        self.st.replace_day('1m', DAY, [])
        self.assertFalse(self.st.has_day('1m', DAY), '빈 교체는 그 날을 지운다')

    def test_계층은_서로_독립이다(self):
        self.st.replace_day('1m', DAY, _rows(f'{DAY} 00:00'))
        self.assertTrue(self.st.has_day('1m', DAY))
        self.assertFalse(self.st.has_day('1h', DAY))

    def test_연도_계층_왕복(self):
        self.st.replace_year('1M', '2026', _rows('2026-09', '2026-08'))
        self.assertEqual(len(self.st.read_year('1M', '2026')), 2)
        self.st.replace_year('1M', '2026', [])
        self.assertEqual(self.st.read_year('1M', '2026'), [])


class StateTest(StoreBase):
    def test_상태_왕복(self):
        self.assertEqual(self.st.load_state(),
                         {'watermark': '', 'open': {}, 'late_dropped_total': 0})
        self.st.save_state({'watermark': f'{DAY} 10:00', 'open': {'k': 1},
                            'late_dropped_total': 3})
        got = self.st.load_state()
        self.assertEqual(got['watermark'], f'{DAY} 10:00')
        self.assertEqual(got['late_dropped_total'], 3)

    def test_깨진_상태는_빈_상태로(self):
        os.makedirs(self.st.stats_root(), exist_ok=True)
        with open(os.path.join(self.st.stats_root(), '.rollup_state.json'), 'w') as f:
            f.write('{깨짐')
        self.assertEqual(self.st.load_state()['watermark'], '')


class EndToEndTest(StoreBase):
    """원본 → 집계 → 저장소 까지 실제로 도는지. 포트를 끼운 뒤 이 경로가 끊기면
    단위 시험은 다 통과해도 운영에서 통계가 빈다."""

    def _seed_call(self, day, hour, minute):
        d = os.path.join(self.root, 'volte', day[0:4], day[5:7], day[8:10],
                         f'{hour:02d}', 'p', 'caller', 'c1.d')
        os.makedirs(d, exist_ok=True)
        import json as _j
        with open(os.path.join(d, 'call.json'), 'w', encoding='utf-8') as f:
            _j.dump({'call_id': 'c1',
                     'invite_time': f'{day}T{hour:02d}:{minute:02d}:03',
                     'answer_time': f'{day}T{hour:02d}:{minute:02d}:06',
                     'duration': 30, 'state': 'ended', 'end_reason': 'normal'}, f)

    def test_원본에서_집계가_저장소까지_들어간다(self):
        self._seed_call(DAY, 10, 5)
        R._WRITER_NOTED['reason'] = None
        R.init(self.root, {}, enabled=True)
        try:
            n = R.rebuild_range(DAY, DAY)
            self.assertGreater(n, 0, '버킷이 하나도 안 만들어졌다')

            rows = self.st.read_day('1m', DAY)
            bucket = f'{DAY} 10:05'
            hit = [r for r in rows if r.get('bucket') == bucket and r.get('svc') == 'volte']
            self.assertEqual(len(hit), 1, f'{bucket} 버킷이 없다: {[r.get("bucket") for r in rows]}')
            self.assertEqual(hit[0]['call']['attempts'], 1)
            self.assertEqual(hit[0]['call']['sessions'], 1, 'answer_time 이 있으면 성립')

            # 파생 계층도 같이 만들어져야 한다 — 조회가 계층을 골라 읽는다
            self.assertTrue(self.st.has_day('1h', DAY))
            self.assertTrue(self.st.has_day('1d', DAY))
            self.assertTrue(self.st.read_year('1M', DAY[:4]), '월 계층도 접혀야 한다')

            # 조회가 그 값을 돌려주는가
            got, cov = R.read_range_filled(
                self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h')
            _b, totals = R.aggregate(got, '1h', 'volte')
            self.assertEqual(totals['volte']['attempts'], 1)
        finally:
            R.init('', {}, enabled=False)


class NoRootTest(unittest.TestCase):
    def test_루트가_없으면_전부_무해하게_no_op(self):
        S._reset_for_test()
        st = S.FileStatsStore('')
        self.assertFalse(st.acquire_writer()[0])
        self.assertFalse(st.has_day('1m', DAY))
        self.assertEqual(st.read_day('1m', DAY), [])
        self.assertEqual(st.upsert_day('1m', DAY, [], set(), True), 'empty')
        self.assertEqual(st.load_state()['watermark'], '')
        st.save_state({'watermark': 'x'})        # 예외 없이 무시


if __name__ == '__main__':
    unittest.main(verbosity=2)
