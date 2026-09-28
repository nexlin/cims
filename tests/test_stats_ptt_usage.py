"""PTT 이용 축 — 발언 수·발언 시간·긴급·영상·사용자 축(mcptt_management_views.md §5.2).

지키는 것:
  ① 세션 1건이 루트·by_group·by_user 에 같은 값으로 접힌다(사용자 축 = 참여자 전원 + 화자별 발언)
  ② 1m → 1h → 1d 로 접어도(fold_records) 조회 합산(aggregate)에서도 합이 보존된다
  ③ 화자별 발언이 없는 인덱스 행(축 이전 세션)은 0 이 아니라 **모름** — `talk_measured` 에 안 들어간다

sys.path 는 ems/core/oam/{src,vendor} — test_stats_ptt_attempts.py 와 동일.
"""
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from services import stats_rollup as R  # noqa: E402

DAY = '2026-09-29'


def _row(gid, people, spk, emergency=False, video=False, start='09:00:01'):
    turns = sum(v['turns'] for v in spk.values())
    return {'start': f'{DAY}T{start}', 'end': f'{DAY}T09:03:01', 'state': 'ended', 'end_reason': 'normal',
            'mcptt_group_id': gid, 'member_count': len(people), 'people': people, 'turns': turns,
            'talk_ms': sum(v['talk_ms'] for v in spk.values()), 'by_speaker': spk,
            'emergency': emergency, 'video_sent': video}


class PttUsageAxisTest(unittest.TestCase):
    def test_세션이_세_축에_같은_값으로_접힌다(self):
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        R._fold_ptt(_row('g1', ['a', 'b', 'c'], {'a': {'turns': 2, 'talk_ms': 4000}, 'b': {'turns': 1, 'talk_ms': 1500}},
                         emergency=True, video=True), agg)
        c = agg['call']
        self.assertEqual((c['turns'], c['talk_sum_sec'], c['emergency'], c['video'], c['talk_measured']), (3, 6, 1, 1, 1))
        g = c['by_group']['g1']
        self.assertEqual((g['sessions'], g['turns'], g['talk_sum_sec'], g['emergency'], g['video']), (1, 3, 6, 1, 1))
        u = c['by_user']
        self.assertEqual(set(u), {'a', 'b', 'c'})                       # 발언 없이 참여한 c 도 사용자 축에 있다
        self.assertEqual((u['a']['turns'], u['a']['talk_sum_sec'], u['a']['sessions'], u['a']['emergency']), (2, 4, 1, 1))
        self.assertEqual((u['c']['turns'], u['c']['sessions']), (0, 1))

    def test_축_이전_세션은_모름이다(self):
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        row = _row('g1', ['a'], {'a': {'turns': 1, 'talk_ms': 1000}})
        del row['by_speaker']                                           # 옛 인덱스 행
        R._fold_ptt(row, agg)
        c = agg['call']
        self.assertEqual((c['talk_measured'], c['turns'], c['by_user']), (0, 0, {}))
        self.assertEqual(c['by_group']['g1']['sessions'], 1)            # 세션 수는 사실이므로 센다

    def test_계층을_접어도_합이_보존된다(self):
        recs = []
        for i, (gid, spk) in enumerate([('g1', {'a': {'turns': 2, 'talk_ms': 3000}}),
                                        ('g1', {'b': {'turns': 1, 'talk_ms': 2000}}),
                                        ('g2', {'a': {'turns': 4, 'talk_ms': 9000}})]):
            agg = R._empty(f'{DAY} 09:0{i}', 'ptt')
            R._fold_ptt(_row(gid, list(spk), spk, start=f'09:0{i}:01'), agg)
            recs.append(agg)
        hour = R.fold_records(recs, '1h')
        day = R.fold_records(hour, '1d')
        for rows in (recs, hour, day):
            _b, totals = R.aggregate(rows, '1d', svc='ptt')
            t = totals['ptt']
            self.assertEqual((t['turns'], t['talk_sum_sec'], t['talk_measured']), (7, 14, 3))
            self.assertEqual((t['by_group']['g1']['turns'], t['by_group']['g2']['talk_sum_sec']), (3, 9))
            self.assertEqual((t['by_user']['a']['turns'], t['by_user']['a']['sessions'], t['by_user']['b']['talk_sum_sec']),
                             (6, 2, 2))
            self.assertEqual(list(t['by_user'])[0], 'a')                # 발언 시간 내림차순


if __name__ == '__main__':
    unittest.main()
