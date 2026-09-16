"""PTT 시도 장부 — 분모가 생겼는지, 그리고 없을 때 거짓값을 내지 않는지.

PTT 는 실패한 그룹통화 시도가 원천에 없어 성공률을 낼 수 없었다(sip_statistics.md §8 Y6).
CSP 가 결말마다 한 줄을 남기는 **시도 장부**로 그 분모를 만든다. 이 파일이 지키는 것 셋:

  ① 장부가 있으면 시도·성립·실패 사유가 집계에 들어간다
  ② 성립을 **장부에서만** 센다 — 세션 기록에서도 세면 같은 통화를 두 번 센다
  ③ 장부가 없는 노드(구 CSP)에서는 성공률을 **0% 가 아니라 빈칸**으로 낸다
     — `_rate(x, 0) = 0` 이라 그대로 두면 멀쩡한 서비스가 0% 로 보이는 거짓 경보가 된다

sys.path 는 ems/core/oam/{src,vendor} — test_stats_rollup_range.py 와 동일.
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

DAY = '2026-09-15'


def _ledger(root, rows):
    """CSP 가 쓰는 장부를 흉내낸다 — 경로 규약은 sip_statistics.md §3."""
    d = os.path.join(root, 'ptt', 'attempts')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, DAY.replace('-', '') + '.jsonl'), 'w', encoding='utf-8') as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')


def _att(ts, outcome, reason='', status=0, caller='u1'):
    return {'ts': ts, 'group': 'g1', 'group_key': '7', 'caller': caller,
            'outcome': outcome, 'reason': reason, 'status': status, 'sesid': 's1'}


class PttAttemptLedgerTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='ptt-att-')
        # 스캐너는 **root 를 인자로** 받는다 — 모듈 전역은 집계 주체(oam-svc)만 설정하므로
        # 조회 프로세스(oam base)에서는 비어 있다. 그 회귀를 이 시험이 막는다.

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_장부가_시도와_성립을_센다(self):
        _ledger(self.root, [
            _att(f'{DAY}T09:00:01', 'established'),
            _att(f'{DAY}T09:00:20', 'failed', 'denied', 403),
            _att(f'{DAY}T09:00:40', 'failed', 'error', 488),
        ])
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        for _mi, row in R._scan_ptt_attempts_day(self.root, DAY):
            R._fold_ptt_attempt(row, agg)
        c = agg['call']
        self.assertEqual(c['attempts'], 3)
        self.assertEqual(c['sessions'], 1)
        self.assertEqual(c['reasons'], {'denied': 1, 'error': 1})
        self.assertEqual(c['statuses'], {'403': 1, '488': 1})

    def test_성공률과_NER_이_같다(self):
        """PTT 는 빼주는 항목이 없다 — denied 는 NER 분자에 들어가지 않는다(§2.1)."""
        c = {'attempts': 4, 'sessions': 3, 'talked': 2, 'completed': 3,
             'reasons': {'denied': 1}, 'statuses': {'403': 1}}
        out = R.with_rates(c)
        self.assertEqual(out['success_rate'], 75.0)
        self.assertEqual(out['ner'], 75.0)

    def test_세션_기록은_성립을_세지_않는다(self):
        """두 원천이 같은 통화를 두 번 세면 성립 수가 부푼다(§3)."""
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        R._fold_ptt({'start': f'{DAY}T09:00:01', 'end': f'{DAY}T09:03:01',
                     'turns': 2, 'people': ['a', 'b'], 'member_count': 4,
                     'state': 'ended', 'end_reason': 'normal',
                     'mcptt_group_id': 'g1'}, agg)
        c = agg['call']
        self.assertEqual(c['sessions'], 0)      # 성립은 장부 몫
        self.assertEqual(c['talked'], 1)        # 발언은 세션 기록 몫
        self.assertEqual(c['completed'], 1)     # 정상 종료(Y4)
        self.assertEqual(c['legs_invited'], 4)
        self.assertEqual(c['legs_joined'], 2)

    def test_강제회수는_완료로_세지_않는다(self):
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        R._fold_ptt({'start': f'{DAY}T09:00:01', 'end': f'{DAY}T09:01:01', 'turns': 1,
                     'state': 'ended', 'end_reason': 'error', 'mcptt_group_id': 'g1'}, agg)
        self.assertEqual(agg['call']['completed'], 0)
        self.assertEqual(agg['call']['reasons'], {'error': 1})

    def test_장부가_없으면_성공률을_비운다(self):
        """구 CSP 노드 — 분모가 0 인데 성립이 있으면 0% 가 아니라 빈칸이다."""
        self.assertEqual(R._scan_ptt_attempts_day(self.root, DAY), [])
        out = R.with_rates({'attempts': 0, 'sessions': 2, 'talked': 1, 'completed': 2})
        self.assertIsNone(out['success_rate'])
        self.assertIsNone(out['talk_rate'])
        self.assertIsNone(out['ner'])
        self.assertEqual(out['rate_gap'], ['no_attempts'])
        self.assertEqual(out['sessions'], 2)    # 개수는 사실이므로 그대로

    def test_반쪽_줄은_건너뛴다(self):
        """CSP 가 쓰는 중인 줄을 만나도 그 구간 전체를 버리지 않는다."""
        d = os.path.join(self.root, 'ptt', 'attempts')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, DAY.replace('-', '') + '.jsonl'), 'w', encoding='utf-8') as f:
            f.write(json.dumps(_att(f'{DAY}T09:00:01', 'established')) + '\n')
            f.write('{"ts":"2026-09-15T09:00:0')
        self.assertEqual(len(R._scan_ptt_attempts_day(self.root, DAY)), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
