"""MCVideo 통계 서비스 축 — 시도 장부·세션 기록을 MCPTT 와 같은 길로 받되 서비스 값으로 가른다(sip_statistics.md §3).

MCPTT 그룹 호와 MCVideo 그룹 호는 같은 시도 장부(`ptt_attempts/<일>.jsonl`)와 같은 녹취 폴더를 쓴다 — 접속환경 kind 가
둘 다 `ptt` 라 kind 로는 못 가르고, 장부 줄·인덱스 행의 `service` 로 가른다. 이 파일이 지키는 것:

  ① 장부 줄·세션 행이 서비스마다 다른 레코드(`svc`)로 접힌다 — MCVideo 가 MCPTT 지표에 섞이지 않는다
  ② 성립하지 못한 MCVideo 세션(`end_reason` setup_failed)은 세션으로 세지 않는다
  ③ 장부가 그 서비스를 남기기 전(서비스 값 이전 CSP)의 날은 성립을 세션 기록이 세고 시도는 모름이다
  ④ MCVideo 송출 축은 슬롯마다 한 번 — 영상·음성 두 트랙에 같은 송출이 실려도 두 번 세지 않는다

sys.path 는 ems/core/oam/{src,vendor} — test_stats_ptt_attempts.py 와 동일.
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

from services import ptt_index  # noqa: E402
from services import stats_rollup as R  # noqa: E402

DAY = '2026-10-01'
MIN = f'{DAY} 10:00'


def _att(ts, outcome, service=None, reason='', cause='', status=0):
    r = {'ts': ts, 'group': 'gmv1', 'group_key': '41', 'caller': 'u1', 'outcome': outcome,
         'reason': reason, 'cause': cause, 'status': status, 'sesid': 's1'}
    if service:
        r['service'] = service
    return r


def _row(service, end_reason='normal', key='S1', turns=1):
    return {'key': key, 'service': service, 'start': f'{DAY}T10:00:05', 'end': f'{DAY}T10:01:05',
            'state': 'ended', 'end_reason': end_reason, 'turns': turns, 'people': ['u1', 'u2'],
            'member_count': 3, 'mcptt_group_id': 'gmv1', 'talk_ms': 20000,
            'by_speaker': {'u1': {'turns': turns, 'talk_ms': 20000}}, 'video_sent': service == 'mcvideo'}


class McVideoRollupTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='mcv-stats-')
        self.stats = os.path.join(self.root, 'stats')
        self.roots = R.Roots(sip=os.path.join(self.root, 'sip'), recordings=os.path.join(self.root, 'rec'),
                             stats=self.stats, state=os.path.join(self.root, 'state'))
        self._orig_day = ptt_index.day
        self.rows = []
        ptt_index.day = lambda d, force=False: list(self.rows)

    def tearDown(self):
        ptt_index.day = self._orig_day
        shutil.rmtree(self.root, ignore_errors=True)

    def _ledger(self, lines):
        d = os.path.join(self.stats, 'ptt_attempts')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, DAY.replace('-', '') + '.jsonl'), 'w', encoding='utf-8') as f:
            for r in lines:
                f.write(json.dumps(r, ensure_ascii=False) + '\n')

    def _build(self):
        return R.build_minutes(self.roots, {MIN}, config={})

    def test_서비스마다_따로_접힌다(self):
        self._ledger([
            _att(f'{DAY}T10:00:01', 'established'),                       # 서비스 값 이전 줄 = ptt
            _att(f'{DAY}T10:00:02', 'established', 'mcvideo'),
            _att(f'{DAY}T10:00:03', 'failed', 'mcvideo', 'denied', 'not_entitled', 403),
        ])
        self.rows = [_row('ptt', key='S1'), _row('mcvideo', key='S2', turns=3)]
        out = self._build()
        p, v = out[(MIN, 'ptt')]['call'], out[(MIN, 'mcvideo')]['call']
        self.assertEqual((p['attempts'], p['sessions'], p['turns']), (1, 1, 1))
        self.assertEqual((v['attempts'], v['sessions'], v['turns']), (2, 1, 3))
        self.assertEqual(v['causes'], {'not_entitled': 1})
        self.assertEqual(v['video'], 1)
        self.assertNotIn('not_entitled', p.get('causes') or {})
        _b, totals = R.aggregate([out[(MIN, 'ptt')], out[(MIN, 'mcvideo')]], '1h', svc='mcvideo')
        self.assertEqual(totals['mcvideo']['success_rate'], 50.0)
        self.assertNotIn('ptt', totals)                  # 필터한 서비스 칸만
        self.assertEqual(totals['all']['attempts'], 3)   # all = 전 서비스 합계

    def test_성립하지_못한_세션은_세지_않는다(self):
        self._ledger([_att(f'{DAY}T10:00:01', 'failed', 'mcvideo', 'no_answer', 'no_member_answered', 480)])
        self.rows = [_row('mcvideo', end_reason='setup_failed', turns=0)]
        v = self._build()[(MIN, 'mcvideo')]['call']
        self.assertEqual((v['attempts'], v['sessions'], v['legs_invited']), (1, 0, 0))
        self.assertEqual(v['end_reasons'], {})           # 강제 회수(error)로도 세지 않는다
        self.assertEqual(v['by_group'], {})

    def test_장부가_그_서비스를_남기기_전의_날(self):
        """서비스 값 이전 CSP — 장부 파일은 있어도(MCPTT 줄) MCVideo 시도는 없다. 성립은 세션 기록이, 시도는 모름."""
        self._ledger([_att(f'{DAY}T10:00:01', 'established')])
        self.rows = [_row('ptt', key='S1'), _row('mcvideo', key='S2')]
        out = self._build()
        v = out[(MIN, 'mcvideo')]['call']
        self.assertEqual((v['sessions'], v['attempts'], v['attempts_unknown']), (1, 0, 1))
        self.assertEqual(out[(MIN, 'ptt')]['call'].get('attempts_unknown', 0), 0)
        _b, totals = R.aggregate([out[(MIN, 'mcvideo')]], '1h', svc='mcvideo')
        self.assertIsNone(totals['mcvideo']['success_rate'])     # 0% 가 아니라 빈칸
        self.assertIsNone(totals['mcvideo']['attempts'])

    def test_장부_대조는_서비스마다(self):
        self._ledger([_att(f'{DAY}T10:00:02', 'established', 'mcvideo')])
        self.rows = [_row('mcvideo', key='S2')]
        with _NoWarn(self):
            self._build()


class _NoWarn:
    """성립 수 불일치 경고가 나지 않아야 한다(장부 1 = 세션 1)."""
    def __init__(self, tc):
        self.tc, self.seen = tc, []

    def __enter__(self):
        self._orig = R.logger.log_warning
        R.logger.log_warning = lambda msg, *a, **k: self.seen.append(msg)
        return self

    def __exit__(self, *exc):
        R.logger.log_warning = self._orig
        self.tc.assertFalse([m for m in self.seen if '불일치' in m], self.seen)
        return False


class McVideoTxTracksTest(unittest.TestCase):
    def test_송출은_슬롯마다_한_번(self):
        seg = {'type': 'mcvideo', 'tracks': [
            {'kind': 'audio', 'slot': 0, 'file': 'a0.rtp', 'speakers': [{'id': 'u1', 'offset_ms': 0, 'dur_ms': 5000}]},
            {'kind': 'video', 'slot': 0, 'file': 'v0.rtp', 'speakers': [{'id': 'u1', 'offset_ms': 0, 'dur_ms': 5000}]},
            # 영상만 보낸 송출(송출 중 무전 — mcvideo.md D12)
            {'kind': 'video', 'slot': 1, 'file': 'v1.rtp', 'speakers': [{'id': 'u2', 'offset_ms': 1000, 'dur_ms': 2000}]},
        ]}
        tx = ptt_index.seg_tx_tracks(seg)
        self.assertEqual([t['slot'] for t in tx], [0, 1])
        self.assertEqual(sum(len(t['speakers']) for t in tx), 2)
        self.assertEqual(len(ptt_index.seg_audio_tracks(seg)), 1)     # 음성만 세면 u2 송출이 빠진다
        self.assertEqual(ptt_index.seg_max_concurrent(tx), 2)


if __name__ == '__main__':
    unittest.main()
