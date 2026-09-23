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


def _ledger(stats, rows):
    """CSP 가 쓰는 장부를 흉내낸다 — `{Stats.Dir}/ptt_attempts/YYYYMMDD.jsonl` (sip_statistics.md §3)."""
    d = os.path.join(stats, 'ptt_attempts')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, DAY.replace('-', '') + '.jsonl'), 'w', encoding='utf-8') as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + '\n')


def _att(ts, outcome, reason='', status=0, caller='u1'):
    return {'ts': ts, 'group': 'g1', 'group_key': '7', 'caller': caller,
            'outcome': outcome, 'reason': reason, 'status': status, 'sesid': 's1'}


class PttAttemptLedgerTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='ptt-att-')      # 통계 영역(Stats.Dir) 자리
        # 스캐너는 **통계 영역을 인자로** 받는다 — 모듈 전역은 집계 주체(oam-svc)만 설정하므로
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
        # 종료 사유는 **개시 결말과 다른 축**이다 — 한 맵에 넣으면 `오류` 칸이 "붙지도 못함"
        #   과 "붙었다가 회수됨" 을 함께 세어 `시도 = 성립 + 거부 + 오류 + 무응답` 이 깨진다.
        self.assertEqual(agg['call']['reasons'], {})
        self.assertEqual(agg['call']['end_reasons'], {'error': 1})
        out = R.with_rates(dict(agg['call'], attempts=1, sessions=1))
        self.assertEqual(out['completion_rate'], 0.0)   # 종료 사유를 알고, 정상이 아니다
        self.assertEqual(out['drop_rate'], 100.0)

    def test_장부가_없으면_성공률을_비운다(self):
        """구 CSP 노드 — 분모가 0 인데 성립이 있으면 0% 가 아니라 빈칸이다."""
        self.assertEqual(R._scan_ptt_attempts_day(self.root, DAY), [])
        out = R.with_rates({'attempts': 0, 'sessions': 2, 'talked': 1, 'completed': 2})
        self.assertIsNone(out['success_rate'])
        self.assertIsNone(out['talk_rate'])
        self.assertIsNone(out['ner'])
        self.assertEqual(out['rate_gap'], ['attempts_unknown'])
        self.assertIsNone(out['attempts'])          # 0 이 아니라 모름이다
        self.assertEqual(out['sessions'], 2)        # 개수는 사실이므로 그대로
        self.assertEqual(out['sessions'], 2)    # 개수는 사실이므로 그대로

    def test_반쪽_줄은_건너뛴다(self):
        """CSP 가 쓰는 중인 줄을 만나도 그 구간 전체를 버리지 않는다."""
        d = os.path.join(self.root, 'ptt_attempts')
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, DAY.replace('-', '') + '.jsonl'), 'w', encoding='utf-8') as f:
            f.write(json.dumps(_att(f'{DAY}T09:00:01', 'established')) + '\n')
            f.write('{"ts":"2026-09-15T09:00:0')
        self.assertEqual(len(R._scan_ptt_attempts_day(self.root, DAY)), 1)


    def test_옛_버킷이_섞인_구간은_성공률을_비운다(self):
        """실측 회귀(2026-09-16): 장부 이전에 집계된 날과 함께 조회하면 **성공률 350%**.

        결손 버킷(성립 4·시도 0)과 정상 버킷(성립 1·시도 2)을 더하면 성립 5·시도 2 가 되어
        합쳐진 칸에서는 `attempts != 0` 이라 결손의 증거가 사라진다. 버킷 행은 각자 빈칸인데
        **합계 행만 틀리는** 형태였다 — 판정을 1분 레코드에서 걷어야 한다.
        """
        rows = [
            # 장부 이전 — 세션 기록만 있어 성립은 있고 시도가 없다
            {'bucket': '2026-09-09 09:00', 'svc': 'ptt',
             'call': {'attempts': 0, 'sessions': 4, 'talked': 4}},
            # 장부 이후 — 시도 2 · 성립 1
            {'bucket': '2026-09-15 18:00', 'svc': 'ptt',
             'call': {'attempts': 2, 'sessions': 1, 'talked': 1, 'completed': 1}},
        ]
        buckets, totals = R.aggregate(rows, '1d', svc='ptt')
        for ax in ('ptt', 'all'):
            # **잰 것끼리** 나눈다: 성립 5 중 4 는 미측정 구간에서 왔으므로 1 / 시도 2 = 50%
            self.assertEqual(totals[ax]['success_rate'], 50.0, ax)
            self.assertEqual(totals[ax]['attempts'], 2, ax)    # 잰 시도
            self.assertEqual(totals[ax]['sessions'], 5, ax)    # 개수는 사실이므로 그대로
            self.assertEqual(totals[ax]['rate_gap'], ['ptt'], ax)   # 일부가 미측정임을 알린다
            # 완료율은 시도와 무관하다(정상종료/성립 — 둘 다 세션 기록) → 비우지 않는다
            self.assertEqual(totals[ax]['completion_rate'], 20.0, ax)
        # 못 잰 날의 행은 여전히 빈칸이다 — 그 날은 잰 시도가 0 이다
        b09 = next(b for b in buckets if b['bucket'] == '2026-09-09')
        self.assertIsNone(b09['ptt']['attempts'])
        self.assertIsNone(b09['ptt']['success_rate'])
        self.assertEqual(b09['ptt']['completion_rate'], 0.0)
        # 장부 이후만 보면 비율이 그대로 나온다 — 빈칸은 섞였을 때만이다
        _, only_new = R.aggregate(rows[1:], '1d', svc='ptt')
        self.assertEqual(only_new['ptt']['success_rate'], 50.0)
        self.assertIsNone(only_new['ptt'].get('rate_gap'))

    def test_장부가_없는_날은_세션_기록이_성립을_센다(self):
        """재집계가 옛 날의 세션 수를 0 으로 지우지 않게 — 그날은 그게 유일한 원천이다."""
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        row = {'start': f'{DAY}T09:00:01', 'end': f'{DAY}T09:00:31', 'turns': 1,
               'state': 'ended', 'mcptt_group_id': 'g1', 'member_count': 2, 'people': ['a', 'b']}
        R._fold_ptt(row, agg, count_session=True)
        self.assertEqual(agg['call']['sessions'], 1)
        self.assertEqual(agg['call']['attempts'], 0)          # 시도는 그래도 없다
        # 장부가 있는 날은 장부가 세므로 여기서 세지 않는다 (두 번 세기 방지)
        agg2 = R._empty(f'{DAY} 09:00', 'ptt')
        R._fold_ptt(row, agg2)
        self.assertEqual(agg2['call']['sessions'], 0)


    def test_원인이_사유_두_칸보다_잘게_남는다(self):
        """사유(denied/error)·응답코드로는 못 가리는 것 — 같은 488 둘, 코드 없는 실패 둘."""
        _ledger(self.root, [
            _att(f'{DAY}T09:00:01', 'failed', 'error', 488) | {'cause': 'codec_mismatch'},
            _att(f'{DAY}T09:00:05', 'failed', 'error', 488) | {'cause': 'srtp_failed'},
            _att(f'{DAY}T09:00:09', 'failed', 'error', 0) | {'cause': 'accept_failed'},
            _att(f'{DAY}T09:00:12', 'failed', 'denied', 0) | {'cause': 'session_expired'},
        ])
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        for _mi, row in R._scan_ptt_attempts_day(self.root, DAY):
            R._fold_ptt_attempt(row, agg)
        c = agg['call']
        self.assertEqual(c['reasons'], {'error': 3, 'denied': 1})
        self.assertEqual(c['statuses'], {'488': 2})          # 코드 없는 둘은 안 담긴다
        self.assertEqual(c['causes'], {'codec_mismatch': 1, 'srtp_failed': 1,
                                       'accept_failed': 1, 'session_expired': 1})
        # 합산·정렬을 거쳐도 원인이 남는다 (건수 내림차순)
        rows = [{'bucket': f'{DAY} 09:00', 'svc': 'ptt', 'call': c}]
        _b, totals = R.aggregate(rows, '1h', svc='ptt')
        self.assertEqual(sum(totals['ptt']['causes'].values()), 4)

    def test_옛_장부_줄은_원인을_지어내지_않는다(self):
        """cause 없는 줄(csp 구 판본)을 unknown 으로 채우면 원인 불명과 구별이 안 된다."""
        _ledger(self.root, [_att(f'{DAY}T09:00:01', 'failed', 'error', 488)])
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        for _mi, row in R._scan_ptt_attempts_day(self.root, DAY):
            R._fold_ptt_attempt(row, agg)
        self.assertEqual(agg['call']['causes'], {})
        self.assertEqual(agg['call']['reasons'], {'error': 1})


    def test_이미_합산된_월_레코드에서도_결손이_잡힌다(self):
        """저장 계층(1h·1d·1M)은 미리 접혀 있어 "시도 0" 조건이 안 보인다.

        실측 회귀(2026-09-16): 1d 를 고친 뒤에도 **월 레코드가 350% 를 그대로 냈다** —
        월 한 줄에 시도 2·성립 7 이 이미 합쳐져 있어 조건이 거짓이기 때문이다. 그래서
        결손을 조건이 아니라 **레코드에 실린 카운터**로 판정한다.
        """
        rec = {'bucket': '2026-09', 'svc': 'ptt',
               'call': {'attempts': 2, 'sessions': 7, 'talked': 7, 'completed': 7,
                        'attempts_unknown': 2, 'sessions_unmeasured': 6, 'talked_unmeasured': 6}}
        _b, totals = R.aggregate([rec], '1M', svc='ptt')
        for ax in ('ptt', 'all'):
            self.assertEqual(totals[ax]['success_rate'], 50.0, ax)   # (7−6) / 2
            self.assertEqual(totals[ax]['attempts'], 2, ax)
            self.assertEqual(totals[ax]['rate_gap'], ['ptt'], ax)
            self.assertEqual(totals[ax]['completion_rate'], 100.0, ax)   # 시도와 무관
        # 카운터가 0 이면 정상 구간이다 — 같은 숫자라도 값을 낸다
        ok = dict(rec['call'], attempts_unknown=0, sessions_unmeasured=0,
                  talked_unmeasured=0, sessions=2)
        _b2, t2 = R.aggregate([{'bucket': '2026-09', 'svc': 'ptt', 'call': ok}], '1M', svc='ptt')
        self.assertEqual(t2['ptt']['success_rate'], 100.0)

    def test_카운터가_상위_계층_합산을_타고_올라간다(self):
        """1m → 1h·1d·1M 접기(fold_records)에서 카운터·원인이 빠지면 판정이 무력해진다."""
        mins = [
            {'bucket': '2026-09-09 09:00', 'unit': '1m', 'svc': 'ptt',
             'call': {'attempts': 0, 'sessions': 4, 'attempts_unknown': 1,
                      'sessions_unmeasured': 4, 'causes': {}}},
            {'bucket': '2026-09-09 09:01', 'unit': '1m', 'svc': 'ptt',
             'call': {'attempts': 2, 'sessions': 1, 'attempts_unknown': 0,
                      'causes': {'codec_mismatch': 1}}},
        ]
        hours = R.fold_records(mins, '1h')
        self.assertEqual(len(hours), 1)
        c = hours[0]['call']
        self.assertEqual(c['attempts'], 2)
        self.assertEqual(c['sessions'], 5)
        self.assertEqual(c['attempts_unknown'], 1)         # 증거가 살아 올라간다
        self.assertEqual(c['sessions_unmeasured'], 4)      # 양도 함께 (분자에서 덜어내려면)
        self.assertEqual(c['causes'], {'codec_mismatch': 1})   # 원인도 함께
        # 접힌 시간 레코드로 조회해도 잰 것끼리 나온다: (5−4) / 2 = 50%
        _b, totals = R.aggregate(hours, '1d', svc='ptt')
        self.assertEqual(totals['ptt']['attempts'], 2)
        self.assertEqual(totals['ptt']['success_rate'], 50.0)

    def test_시도_0_과_시도_모름을_가른다(self):
        """"호를 한 건도 안 걸었는데 통화가 4건 성립했다" 는 모순을 만들지 않는다.

        장부가 없는 날의 시도는 **0건이 아니라 모름**이다. 0 으로 적으면 합산에서 분자만
        자라 성공률이 100% 를 넘고(실측 350%), 그 날 행 자체가 앞뒤가 안 맞는다.
        """
        # ① 잰 시도가 하나도 없는 구간 — 시도·성공률은 빈칸, 개수와 완료율은 그대로
        out = R.with_rates({'attempts': 0, 'sessions': 4, 'talked': 4, 'completed': 4,
                            'attempts_unknown': 1, 'sessions_unmeasured': 4,
                            'talked_unmeasured': 4})
        self.assertIsNone(out['attempts'])
        self.assertIsNone(out['success_rate'])
        self.assertEqual(out['sessions'], 4)
        self.assertEqual(out['completion_rate'], 100.0)
        self.assertEqual(out['rate_gap'], ['attempts_unknown'])
        # ② 정말 한 건도 없던 구간 — 0 은 사실이다. 빈칸이 아니다
        out0 = R.with_rates({'attempts': 0, 'sessions': 0, 'talked': 0, 'completed': 0,
                             'attempts_unknown': 0})
        self.assertEqual(out0['attempts'], 0)
        self.assertNotIn('rate_gap', out0)


    def test_사설콜_상대_꺼짐은_NER_이_면제한다(self):
        """상대 단말 사정은 우리 구성 문제가 아니다 — VoLTE 와 같은 `no_answer`(480).

        `denied`(정책 거부)에 넣으면 NER 이 면제하지 않아 상대 사정이 망 책임으로 잡히고,
        실패 사유 분포에서 우리 결함과 섞인다. 그래서 어휘를 공용 `no_answer` 로 쓴다.
        """
        _ledger(self.root, [
            _att(f'{DAY}T09:00:01', 'established'),
            _att(f'{DAY}T09:00:05', 'failed', 'denied', 403) | {'cause': 'not_member'},
            _att(f'{DAY}T09:00:09', 'failed', 'no_answer', 480) | {'cause': 'private_callee_offline'},
        ])
        agg = R._empty(f'{DAY} 09:00', 'ptt')
        for _mi, row in R._scan_ptt_attempts_day(self.root, DAY):
            R._fold_ptt_attempt(row, agg)
        out = R.with_rates(agg['call'])
        self.assertEqual(out['attempts'], 3)
        self.assertEqual(out['sessions'], 1)
        self.assertEqual(out['success_rate'], 33.3)          # 성립 1 / 시도 3
        # NER 은 상대 단말 사정 1건을 분자에 넣는다 → 성공률보다 높다
        self.assertEqual(out['ner_ok'], 2)                   # 성립 1 + no_answer 1
        self.assertEqual(out['ner'], 66.7)
        self.assertEqual(out['reasons'], {'denied': 1, 'no_answer': 1})
        self.assertEqual(out['causes'], {'not_member': 1, 'private_callee_offline': 1})
        self.assertEqual(out['statuses'], {'403': 1, '480': 1})
        # 정책 거부는 면제되지 않는다 — 그게 우리 결함을 가리지 않는 근거다
        self.assertLess(out['ner'], 100.0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
