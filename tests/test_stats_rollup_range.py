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
from datetime import datetime, timedelta
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


def _raw_day(root, day):
    """그 날의 **원본 날 디렉터리**를 심는다 — 즉석 집계가 0 을 낸 것이 사실인지 가르는
    근거(`_raw_day_exists`). 원본이 없는 날은 훑어도 빈손이고, 그 빈손은 0 이 아니라
    모름이라 조회에서 제외된다."""
    os.makedirs(os.path.join(root, day[0:4], day[5:7], day[8:10]), exist_ok=True)


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
        _raw_day(self.root, DAY)
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 12:42:59', {}, gran='1h')
        # 그 날 원본은 남아 있다 — 호가 없어 결과는 비지만 '훑었다' 는 사실이 남는다.
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
        _raw_day(self.root, DAY)
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {},
            gran='1h', deadline_sec=-1)
        self.assertFalse(cov['deadline_hit'])
        self.assertEqual(cov['scanned'], 1)

    def test_데드라인_없으면_상한_없이_채운다(self):
        _raw_day(self.root, DAY)
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


class RawSourceGoneTest(unittest.TestCase):
    """**집계도 원본도 없는 날은 0 이 아니라 모름이다** (F-48 잔여).

    구간 양 끝의 반쪽 날은 거친 계층(1h·1d)의 버킷이 구간 경계를 넘어 쓸 수 없다 —
    남은 조각은 1분 계층으로, 거기도 없으면 원본으로 내려간다. 그런데 1분 계층은 14일,
    원본은 그보다 먼저 지워질 수 있어 **둘 다 없는 조각**이 생긴다. 그때 즉석 집계는
    조용히 빈손으로 돌아오고, 조회는 성공한 얼굴로 작은 값을 낸다.

    운영자는 그 감소를 **실제 트래픽 변화로 읽는다.** 그래서 빠졌다고 말해야 한다.
    """

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='rollup_raw_')

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_원본이_없는_날은_빠진_날로_신고한다(self):
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h')
        self.assertEqual(cov['scanned'], 0, '훑을 원본이 없다')
        self.assertEqual(cov['missing'], 1)
        self.assertIn(DAY, cov['missing_days'])

    def test_원본이_있으면_빈_결과도_0_으로_받는다(self):
        """디렉터리는 있는데 호가 없던 날 — 이건 **진짜 0** 이라 신고 대상이 아니다."""
        _raw_day(self.root, DAY)
        rows, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h')
        self.assertEqual(cov['missing'], 0)
        self.assertEqual(cov['scanned'], 1)

    def test_호_기록만_남은_날도_원본으로_친다(self):
        """원문 로그가 지워져도 호 기록(volte/)이 남아 있으면 되짚을 수 있다."""
        os.makedirs(os.path.join(self.root, 'volte', DAY[0:4], DAY[5:7], DAY[8:10]))
        _, cov = R.read_range_filled(
            self.root, f'{DAY} 00:00:00', f'{DAY} 23:59:59', {}, gran='1h')
        self.assertEqual(cov['missing'], 0)

    def test_반쪽_날만_원본이_없어도_그_날이_신고된다(self):
        """가운데 날은 1시간 계층이 온전히 덮고, 양 끝 반쪽만 원본이 필요하다."""
        d0, d1 = '2026-09-10', '2026-09-11'
        _write(self.root, '1h', d1, _hours(d1))
        _raw_day(self.root, d1)
        _, cov = R.read_range_filled(
            self.root, f'{d0} 10:30:00', f'{d1} 23:59:59', {}, gran='1h')
        self.assertIn(d0, cov['missing_days'], '원본이 없는 반쪽 날')
        self.assertNotIn(d1, cov['missing_days'], '계층으로 덮인 날')


class MarkMissingBucketsTest(unittest.TestCase):
    """**자료가 없는 날의 행은 0 이 아니라 빈칸이다.**

    시간축 표는 빈 버킷을 0 으로 그린다(§2.1c). 그 규약은 "읽었고 호가 없었다" 일 때만
    참이고, 철거·보존기간 경과로 자료가 없는 날에는 거짓말이 된다 — 실측(2026-09-17):
    9/1~9/3 행이 9/17(자료 있고 호 0건) 행과 **화면에서 똑같이 0** 으로 나왔다.
    """

    def _buckets(self, labels):
        return [{'bucket': b, 'bucket_start': b} for b in labels]

    def test_빠진_날의_행만_표시된다(self):
        bs = R.mark_missing_buckets(
            self._buckets(['2026-09-01', '2026-09-02', '2026-09-03']), '1d',
            '2026-09-01 00:00:00', '2026-09-03 23:59:59', ['2026-09-02'])
        self.assertEqual([b.get('missing') for b in bs], [None, True, None])

    def test_빠진_날이_없으면_아무것도_안_단다(self):
        bs = R.mark_missing_buckets(self._buckets(['2026-09-01']), '1d',
                                    '2026-09-01 00:00:00', '2026-09-01 23:59:59', [])
        self.assertNotIn('missing', bs[0])

    def test_시간_버킷은_그_날을_따른다(self):
        bs = R.mark_missing_buckets(
            self._buckets(['2026-09-02 03:00', '2026-09-03 03:00']), '1h',
            '2026-09-02 00:00:00', '2026-09-03 23:59:59', ['2026-09-02'])
        self.assertEqual([b.get('missing') for b in bs], [True, None])

    def test_월_버킷은_덮는_날이_전부_빠졌을_때만(self):
        """일부만 빠진 달을 통째로 비우면 **있는 자료를 숨긴다.**"""
        days = R._days_of_month('2026-09')
        part = R.mark_missing_buckets(self._buckets(['2026-09']), '1M',
                                      '2026-09-01 00:00:00', '2026-09-30 23:59:59',
                                      days[:5])
        self.assertNotIn('missing', part[0], '5일만 빠진 달은 통째로 비우지 않는다')
        whole = R.mark_missing_buckets(self._buckets(['2026-09']), '1M',
                                       '2026-09-01 00:00:00', '2026-09-30 23:59:59', days)
        self.assertTrue(whole[0].get('missing'))

    def test_조회_구간_밖의_날은_판정에서_뺀다(self):
        """9/2 하루만 조회했는데 그 달의 다른 날까지 요구하면 영원히 표시되지 않는다."""
        bs = R.mark_missing_buckets(self._buckets(['2026-09-02']), '1d',
                                    '2026-09-02 00:00:00', '2026-09-02 23:59:59',
                                    ['2026-09-02'])
        self.assertTrue(bs[0].get('missing'))

    def test_구간_조회가_빠진_날_목록을_자르지_않는다(self):
        """40개로 자르면 41번째 날부터 다시 0 으로 그려진다(표가 이 목록으로 판정한다)."""
        root = tempfile.mkdtemp(prefix='rollup_miss_')
        try:
            _, cov = R.read_range_filled(root, '2026-06-01 00:00:00',
                                         '2026-09-01 23:59:59', {}, gran='1d')
            self.assertEqual(cov['missing'], len(cov['missing_days']),
                             '신고한 수와 목록 길이가 같아야 한다')
            self.assertGreater(cov['missing'], 40)
        finally:
            shutil.rmtree(root, ignore_errors=True)


class FutureDaysTest(unittest.TestCase):
    """**아직 오지 않은 날은 "못 본 날" 이 아니다.**

    자료가 없는 것은 같지만 성질이 다르다 — 못 본 날은 조치가 있고(보존기간을 늘려 재집계)
    미래는 없다. 한데 세면 `이번 달` 처럼 달 끝까지 잡는 조회에서 남은 날이 전부 "자료 없음"
    으로 신고되고 재집계 권고까지 붙는다(실측 2026-09-17: 9/10~9/25 조회에 9/18~9/25 8일이
    경고 띠에 나열됐다 — 내일 것을 재집계할 수는 없다).
    """

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='rollup_future_')
        self.today = datetime.now().strftime('%Y-%m-%d')
        self.tomorrow = (datetime.now() + timedelta(days=1)).strftime('%Y-%m-%d')
        self.next_week = (datetime.now() + timedelta(days=7)).strftime('%Y-%m-%d')

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_미래_날은_빠진_날로_세지_않는다(self):
        _, cov = R.read_range_filled(self.root, f'{self.tomorrow} 00:00:00',
                                     f'{self.next_week} 23:59:59', {}, gran='1d')
        self.assertEqual(cov['missing'], 0, '경고 대상이 아니다')
        self.assertEqual(cov['missing_days'], [])
        self.assertGreater(cov['future'], 0, '미래 날은 따로 센다')
        self.assertIn(self.tomorrow, cov['future_days'])

    def test_과거의_결손은_그대로_신고한다(self):
        """미래를 가른다고 진짜 구멍까지 조용해지면 안 된다."""
        past = '2026-01-02'
        _, cov = R.read_range_filled(self.root, f'{past} 00:00:00',
                                     f'{past} 23:59:59', {}, gran='1d')
        self.assertEqual(cov['missing'], 1)
        self.assertEqual(cov['future'], 0)

    def test_섞이면_각자의_칸으로_간다(self):
        _raw_day(self.root, self.today)
        _, cov = R.read_range_filled(self.root, '2026-01-02 00:00:00',
                                     f'{self.tomorrow} 23:59:59', {}, gran='1d')
        self.assertIn('2026-01-02', cov['missing_days'])
        self.assertNotIn(self.tomorrow, cov['missing_days'])
        self.assertIn(self.tomorrow, cov['future_days'])

    def test_오늘은_미래가_아니다(self):
        """진행 중인 날은 남은 시간이 비어 있어도 '읽은 날' 이다(원본이 있다)."""
        _raw_day(self.root, self.today)
        _, cov = R.read_range_filled(self.root, f'{self.today} 00:00:00',
                                     f'{self.today} 23:59:59', {}, gran='1d')
        self.assertEqual(cov['future'], 0)
        self.assertEqual(cov['missing'], 0)


class UnknownReasonTest(unittest.TestCase):
    """**아무것도 특정할 수 없는 실패는 `unknown` 으로 따로 센다.**

    `error` 는 이름이 붙은 사유처럼 보이지만 실제로는 "200 이 아니었다" 는 뜻뿐이다
    (CallDir.h — 착신까지 나간 호는 `200 아니면 error`). 응답코드마저 없으면 나중에 사유를
    세분화해도 **가를 근거가 없다.** 코드가 있는 것과 없는 것을 미리 갈라 두면 세분화
    대상이 좁혀지고, 이 칸이 줄어드는 것이 곧 진척도가 된다.
    """

    def _fold(self, rec):
        agg = {'call': R._zero_call(), 'msg': {'in': {}, 'out': {}}}
        R._fold_volte(rec, agg)
        return agg

    def _rec(self, **kw):
        base = {'state': 'ended', 'invite_time': '2026-09-15 10:00:00',
                'answer_time': None, 'duration': 0, 'end_reason': 'error', 'end_status': 0}
        base.update(kw)
        return base

    def test_오류인데_코드가_없으면_모름(self):
        agg = self._fold(self._rec())
        self.assertEqual(agg['call']['reasons'].get('unknown'), 1)
        self.assertIsNone(agg['call']['reasons'].get('error'))

    def test_오류에_코드가_있으면_그대로_오류(self):
        """지금 쌓는 방식을 바꾸지 않는다 — 가를 근거가 있는 건은 건드리지 않는다."""
        agg = self._fold(self._rec(end_status=503))
        self.assertEqual(agg['call']['reasons'].get('error'), 1)
        self.assertIsNone(agg['call']['reasons'].get('unknown'))
        self.assertEqual(agg['call']['statuses'].get('503'), 1)

    def test_이름이_붙은_사유는_코드가_없어도_그대로(self):
        """거절·통화중·무응답은 CSP 가 응답코드로 판정해 붙인 이름이다."""
        for rs in ('rejected', 'busy', 'no_answer'):
            agg = self._fold(self._rec(end_reason=rs))
            self.assertEqual(agg['call']['reasons'].get(rs), 1, rs)
            self.assertIsNone(agg['call']['reasons'].get('unknown'), rs)

    def test_정상종료는_모름으로_새지_않는다(self):
        agg = self._fold(self._rec(end_reason='normal', answer_time='2026-09-15 10:00:03',
                                   duration=5, end_status=200))
        self.assertEqual(agg['call']['reasons'].get('normal'), 1)
        self.assertEqual(agg['call']['completed'], 1)
        self.assertIsNone(agg['call']['reasons'].get('unknown'))

    def test_안_끝난_호는_미결이지_모름이_아니다(self):
        """미결 판정은 집계와 되짚기가 **같은 규칙**(사유 유무)을 쓴다 — 여기만 바꾸면 갈린다."""
        agg = self._fold(self._rec(state='ringing', end_reason=None))
        self.assertEqual(agg.get('open'), 1)
        self.assertEqual(agg['call']['reasons'], {})

    def test_모름은_NER_분자에_안_들어간다(self):
        """NER 은 '상대 사정' 만 면제한다 — 모르는 것을 면제하면 망 책임이 지워진다."""
        out = R.with_rates({'attempts': 2, 'sessions': 1, 'talked': 1,
                            'reasons': {'unknown': 1}})
        self.assertEqual(out['ner_ok'], 1)
        self.assertEqual(out['ner'], 50.0)

    def test_PTT_장부에_사유가_없으면_모름(self):
        agg = {'call': R._zero_call(), 'msg': {'in': {}, 'out': {}}}
        R._fold_ptt_attempt({'outcome': 'failed', 'reason': '', 'cause': '', 'status': 0}, agg)
        self.assertEqual(agg['call']['reasons'].get('unknown'), 1)
        self.assertIsNone(agg['call']['reasons'].get('error'))


class EnsureSvcCellTest(unittest.TestCase):
    """**읽은 구간의 0 건은 0 으로 낸다** (F-49).

    집계는 들어온 행에 있는 서비스로만 칸을 만든다. 그래서 그 서비스의 호가 한 건도 없는
    날은 `totals` 에 칸이 아예 없고, 화면은 없는 경로를 `—`(자료 없음)으로 그린다 —
    조용한 주말·PTT 만 쓰는 현장처럼 **정상적으로 0 건인 날이 통계 고장과 구분되지 않는다**
    (실측 2026-09-17: VoLTE 지표 타일 6개가 전부 `—`).
    """

    def _rows(self, svc='ptt'):
        return [{'bucket': f'{DAY} 09:00', 'svc': svc,
                 'call': {'attempts': 2, 'sessions': 2, 'talked': 2}}]

    def test_끄면_없는_서비스_칸이_안_생긴다(self):
        """기본 동작 — 구간을 읽었는지 모르는 호출자는 0 을 지어내면 안 된다."""
        _, totals = R.aggregate(self._rows(), '1h', 'volte')
        self.assertNotIn('volte', totals)

    def test_켜면_0_으로_칸이_생긴다(self):
        _, totals = R.aggregate(self._rows(), '1h', 'volte', ensure_svc=True)
        self.assertIn('volte', totals)
        self.assertEqual(totals['volte']['attempts'], 0)
        self.assertEqual(totals['volte']['sessions'], 0)

    def test_0_인_칸의_비율은_0_이_아니라_빈칸이다(self):
        """분모가 0 이면 비율은 정의되지 않는다 — 0% 로 내면 '전부 실패' 와 같아진다."""
        _, totals = R.aggregate(self._rows(), '1h', 'volte', ensure_svc=True)
        for k in ('success_rate', 'talk_rate', 'completion_rate', 'ner'):
            self.assertIsNone(totals['volte'][k], k)

    def test_자료가_있으면_켜도_값이_그대로다(self):
        _, totals = R.aggregate(self._rows('volte'), '1h', 'volte', ensure_svc=True)
        self.assertEqual(totals['volte']['attempts'], 2)
        self.assertEqual(totals['volte']['success_rate'], 100.0)

    def test_행이_하나도_없어도_칸을_만든다(self):
        """구간을 읽었는데 아무 서비스도 없던 경우 — `all` 까지 0 으로."""
        _, totals = R.aggregate([], '1h', 'volte', ensure_svc=True)
        self.assertEqual(totals['volte']['attempts'], 0)
        self.assertEqual(totals['all']['attempts'], 0)

    def test_all_축은_필터와_무관하게_전체_합계다(self):
        _, totals = R.aggregate(self._rows(), '1h', 'volte', ensure_svc=True)
        self.assertEqual(totals['all']['attempts'], 2, 'PTT 2건이 all 에 남아야 한다')


if __name__ == '__main__':
    unittest.main(verbosity=2)
