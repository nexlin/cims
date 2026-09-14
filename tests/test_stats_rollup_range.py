"""stats_rollup 구간 조회 — 계층 선택이 **버킷 단위**인지 확인하는 단위 테스트.

성능 화면은 늘 "오늘 00:00 ~ 지금" 을 조회한다. 계층 선택을 **날 단위**로 하면 그 날은
마지막 1분이 빠졌다는 이유로 거친 계층에서 통째로 밀려나, 이미 만들어 둔 1시간 집계를
버리고 원본을 전량 훑는다 — 같은 하루가 `date=` 로는 0.03초, `from`·`to` 로는 125초
무응답이었고 게이트웨이가 504 를 냈다(2026-09-11 실측). 이 파일은 그 회귀를 막는다.

sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
import unittest.mock

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from services import stats_rollup as R  # noqa: E402
from services import stats_store as S  # noqa: E402

DAY = '2026-09-11'


def _write(root, unit, day, buckets):
    """그 날의 버킷 레코드를 심는다. **저장소 API 로** 넣는다 — 시험이 파일 배치를 알면
    백엔드를 바꿀 때 같이 깨진다(최종 목표는 DB 적재다)."""
    S.for_root(root).replace_day(
        unit, day,
        [{'bucket': b, 'svc': 'volte', 'call': {'attempts': 1}} for b in buckets])


def _hours(day, n=24):
    return [f'{day} {h:02d}:00' for h in range(n)]


def _minutes(day, h0, m0, h1, m1):
    out = []
    for h in range(h0, h1 + 1):
        for m in range(60):
            if (h, m) < (h0, m0) or (h, m) > (h1, m1):
                continue
            out.append(f'{day} {h:02d}:{m:02d}')
    return out


class ReadRangeBucketCoverageTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='rollup_test_')

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _buckets_of(self, rows):
        return sorted(r['bucket'] for r in rows)

    # ── 핵심 회귀 ────────────────────────────────────────────────
    def test_진행중인_날도_1시간_롤업을_쓴다(self):
        """to 가 하루 중간이어도 그때까지의 온전한 시간 버킷은 롤업에서 나와야 한다."""
        _write(self.root, '1h', DAY, _hours(DAY))
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')

        self.assertEqual(cov['by_unit'].get('1h'), 1, '1시간 계층을 써야 한다')
        self.assertEqual(cov['rollup'], 1)
        # 00:00~11:00 버킷 12개는 12:42 안에 온전히 들어간다.
        self.assertEqual(self._buckets_of(rows), sorted(_hours(DAY, 12)))

    def test_경계에_걸친_버킷은_거친_계층에서_빠진다(self):
        """12:00 버킷은 12:59 까지라 12:42 를 넘는다 — 통째로 세면 총계가 부푼다."""
        _write(self.root, '1h', DAY, _hours(DAY))
        rows, _ = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')
        self.assertNotIn(f'{DAY} 12:00', self._buckets_of(rows))

    def test_경계_구간은_잔_계층이_채운다(self):
        """1분 계층이 있으면 12:00~12:42 는 거기서 나온다 — 즉석 집계로 내려가지 않는다."""
        _write(self.root, '1h', DAY, _hours(DAY))
        _write(self.root, '1m', DAY, _minutes(DAY, 0, 0, 23, 59))
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')

        self.assertEqual(cov['scanned'], 0, '원본 즉석 집계로 떨어지면 안 된다')
        self.assertEqual(cov['missing'], 0)
        got = self._buckets_of(rows)
        # 1h 12개 + 1m 43개(12:00~12:42). 겹치는 분이 두 번 들어가면 총계가 부푼다.
        self.assertEqual(len(got), 12 + 43)
        self.assertEqual(len(got), len(set(got)))
        self.assertIn(f'{DAY} 12:42', got)
        self.assertNotIn(f'{DAY} 12:43', got)

    def test_계층이_섞여도_총계가_부풀지_않는다(self):
        """1h + 1m 을 함께 읽어도 같은 분이 두 번 세어지면 안 된다 — 합산까지 확인한다."""
        _write(self.root, '1h', DAY, _hours(DAY))
        _write(self.root, '1m', DAY, _minutes(DAY, 0, 0, 23, 59))
        rows, _ = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')
        buckets, totals = R.aggregate(rows, '1h', 'volte')
        # 레코드 1건 = attempts 1. 12:42 까지면 1h 12건 + 1m 43건 = 55.
        self.assertEqual(totals['volte']['attempts'], 55)
        # 12:00 칸은 1분 레코드 43개로만 만들어진다(시간 버킷을 겹쳐 넣지 않았다).
        cell = next(b for b in buckets if b['bucket'] == f'{DAY} 12:00')
        self.assertEqual(cell['volte']['attempts'], 43)

    def test_온전히_덮인_날은_가장_거친_계층(self):
        _write(self.root, '1h', DAY, _hours(DAY))
        _write(self.root, '1m', DAY, _minutes(DAY, 0, 0, 23, 59))
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h')
        self.assertEqual(cov['by_unit'].get('1h'), 1)
        self.assertIsNone(cov['by_unit'].get('1m'), '1분까지 내려갈 이유가 없다')
        self.assertEqual(len(rows), 24)

    def test_롤업이_없으면_그_구간만_즉석_집계_대상(self):
        """1h 가 덮은 뒤 남은 구간만 원본으로 간다 — 하루 전체가 아니라."""
        _write(self.root, '1h', DAY, _hours(DAY))
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')
        # 원본 트리가 없으니 즉석 집계 결과는 비지만, '시도했다' 는 사실이 남는다.
        self.assertEqual(cov['scanned'], 1)
        self.assertEqual(cov['by_unit'].get('1h'), 1, '롤업은 그대로 쓰였어야 한다')

    def test_구간_밖_버킷은_들어오지_않는다(self):
        _write(self.root, '1h', DAY, _hours(DAY))
        rows, _ = R.read_range_filled(
            self.root, f'{DAY} 03:00:00', f'{DAY} 06:59:59', {}, gran='1h')
        self.assertEqual(self._buckets_of(rows),
                         [f'{DAY} 03:00', f'{DAY} 04:00', f'{DAY} 05:00', f'{DAY} 06:00'])

    def test_여러_날_양끝만_부분_덮임(self):
        d0, d1, d2 = '2026-09-09', '2026-09-10', '2026-09-11'
        for d in (d0, d1, d2):
            _write(self.root, '1h', d, _hours(d))
        rows, cov = R.read_range_filled(
            self.root, f'{d0} 10:00:00', f'{d2} 05:30:59', {}, gran='1h')
        got = self._buckets_of(rows)
        self.assertEqual(cov['by_unit'].get('1h'), 3, '세 날 모두 1시간 계층')
        self.assertIn(f'{d0} 10:00', got)
        self.assertNotIn(f'{d0} 09:00', got)
        self.assertIn(f'{d1} 00:00', got)          # 가운데 날은 온전히
        self.assertIn(f'{d1} 23:00', got)
        self.assertIn(f'{d2} 04:00', got)
        self.assertNotIn(f'{d2} 05:00', got)       # 05:59 까지라 05:30 을 넘는다

    # ── 즉석 집계 시간 상한 ──────────────────────────────────────
    def test_데드라인을_넘기면_빠진_구간으로_알린다(self):
        """호출자가 이미 떠난 뒤까지 원본을 긁지 않는다.

        시계를 고정해 재현한다 — 짧은 실제 상한(0.01초)에 기대면 장비 속도에 따라
        결과가 갈린다. 첫 호출(기준시각) 뒤부터 이미 지난 것으로 보이게 한다."""
        clock = iter([0.0] + [1e9] * 10000)
        with unittest.mock.patch.object(R.time, 'monotonic', lambda: next(clock)):
            rows, cov = R.read_range_filled(
                self.root, '2026-09-01 00:00:00', '2026-09-11 23:59:59', {},
                gran='1h', deadline_sec=3.5)
        self.assertTrue(cov['deadline_hit'])
        self.assertGreater(cov['missing'], 0)
        self.assertEqual(cov['scanned'], 0, '한 날도 긁지 않고 멈춰야 한다')

    def test_음수_상한은_상한_없음(self):
        """0 과 음수는 모두 '상한 없음' — 재집계·검증 경로가 쓴다."""
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {},
            gran='1h', deadline_sec=-1)
        self.assertFalse(cov['deadline_hit'])
        self.assertEqual(cov['scanned'], 1)

    def test_데드라인_없으면_상한_없이_채운다(self):
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {},
            gran='1h', deadline_sec=0)           # 0 = 상한 없음
        self.assertFalse(cov['deadline_hit'])
        self.assertEqual(cov['scanned'], 1)

    # ── 헬퍼 ────────────────────────────────────────────────────
    def test_버킷_오프셋(self):
        self.assertEqual(R._bucket_offsets(f'{DAY} 13:00', '1h', DAY), (780, 839))
        self.assertEqual(R._bucket_offsets(f'{DAY} 13:07', '1m', DAY), (787, 787))
        self.assertEqual(R._bucket_offsets(DAY, '1d', DAY), (0, 1439))
        self.assertIsNone(R._bucket_offsets('2026-09-10 13:00', '1h', DAY), '다른 날')

    def test_필요_구간_오프셋(self):
        self.assertEqual(min(R._need_offsets(DAY, f'{DAY} 00:00', f'{DAY} 12:42')), 0)
        self.assertEqual(max(R._need_offsets(DAY, f'{DAY} 00:00', f'{DAY} 12:42')), 762)
        self.assertEqual(R._need_offsets(DAY, '2026-09-12 00:00', '2026-09-12 10:00'), set())
        self.assertEqual(len(R._need_offsets(DAY, '2026-09-01 00:00', '2026-09-30 23:59')), 1440)


if __name__ == '__main__':
    unittest.main(verbosity=2)
