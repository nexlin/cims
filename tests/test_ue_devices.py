"""단말 속성 file-store `ue_devices` — CSP 관측 줄을 단말 레코드로 접는 규칙 (mcptt_management_views.md §4.1).

지키는 것:
  ① User-Agent 형식 `CIMS-PTT/<버전> (<OS>; <모델>)` 을 앱·버전·OS·모델로 가른다, 형식 밖이면 product 만
  ② IMEI 칸은 `urn:gsma:imei:` 만 채운다(`urn:uuid:` 는 빈 칸), 화면 값은 가운데를 가린다
  ③ 같은 단말의 재등록은 같은 레코드(first_seen 유지·last_seen 갱신), 다른 단말이 오면 이전 단말은 등록 해제
  ④ 해제 줄(단말 식별 없음)은 그 번호의 등록 중 레코드를 모두 내린다
  ⑤ 커서 — 두 번째 fold 는 새 줄만 읽고, 쓰는 중인 마지막 줄(개행 없음)은 다음 차례로 미룬다

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

from services import lease, ue_devices as U  # noqa: E402

UA_PTT = 'CIMS-PTT/1.4.2 (Android 14; SM-G991N)'
IMEI = 'urn:gsma:imei:35123456-789012-0'
UUID = 'urn:uuid:0f3e2c44-1111-3aaa-8bbb-222233334444'


def _row(user, event='register', instance='', ua=UA_PTT, ts='2026-09-29T09:00:00'):
    return {'ts': ts, 'event': event, 'user': user, 'kind': 'ptt', 'instance': instance, 'user_agent': ua,
            'transport': 'TLS', 'addr': '10.0.0.5:40211', 'expires': 600, 'node': 'csp_01'}


class ParseTests(unittest.TestCase):
    def test_user_agent(self):
        self.assertEqual(U.parse_user_agent(UA_PTT),
                         {'app': 'CIMS-PTT', 'app_version': '1.4.2', 'os': 'Android 14', 'model': 'SM-G991N'})
        self.assertEqual(U.parse_user_agent('CIMS-UE/cimsue-cli')['app'], 'CIMS-UE')
        self.assertEqual(U.parse_user_agent('')['app'], '')

    def test_imei_only_for_gsma_urn(self):
        self.assertEqual(U.imei_of(IMEI), '351234567890120')
        self.assertEqual(U.imei_of(UUID), '')
        self.assertEqual(U.mask_imei('351234567890120'), '3512…0120')


class FoldTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.stats = os.path.join(self.tmp, 'stats')
        os.makedirs(os.path.join(self.stats, 'ue_devices'))
        self.cfg = {'CimsRuntimeDir': os.path.join(self.tmp, 'runtime')}
        self.src = os.path.join(self.stats, 'ue_devices', '20260929.jsonl')
        os.makedirs(U.owner_root(self.cfg), exist_ok=True)
        self.assertTrue(lease.acquire(U.owner_root(self.cfg)).get('active'))   # oam-svc 가 자기 서브트리에 잡는 리스

    def tearDown(self):
        lease.release()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write(self, rows, tail=''):
        with open(self.src, 'a', encoding='utf-8') as f:
            for r in rows:
                f.write(json.dumps(r) + '\n')
            f.write(tail)

    def _recs(self):
        return {U.device_key(r['subscription_id'], r.get('instance_id')): r for r in U.load_all(self.cfg)}

    def test_same_device_refresh_and_device_change(self):
        self._write([_row('+8250001', instance=IMEI, ts='2026-09-29T09:00:00'),
                     _row('+8250001', instance=IMEI, ts='2026-09-29T10:00:00')])
        self.assertEqual(U.fold(self.cfg, self.stats), 2)
        r = self._recs()[U.device_key('+8250001', IMEI)]
        self.assertEqual((r['first_seen'], r['last_seen'], r['registered']), ('2026-09-29T09:00:00', '2026-09-29T10:00:00', True))
        self.assertEqual((r['model'], r['imei']), ('SM-G991N', '351234567890120'))
        self._write([_row('+8250001', instance=UUID, ts='2026-09-29T11:00:00')])
        self.assertEqual(U.fold(self.cfg, self.stats), 1)          # ⑤ 새 줄만
        recs = self._recs()
        self.assertFalse(recs[U.device_key('+8250001', IMEI)]['registered'])
        self.assertTrue(recs[U.device_key('+8250001', UUID)]['registered'])
        self.assertEqual(recs[U.device_key('+8250001', UUID)]['imei'], '')
        self.assertEqual(U.by_user(list(recs.values()))['+8250001'][0]['instance_id'], UUID)

    def test_unregister_and_partial_line(self):
        self._write([_row('+8250002', instance=UUID)],
                    tail=json.dumps(_row('+8250002', event='unregister', instance='', ua=''))[:-5])
        self.assertEqual(U.fold(self.cfg, self.stats), 1)          # 개행 없는 줄은 미룬다
        self.assertTrue(self._recs()[U.device_key('+8250002', UUID)]['registered'])
        with open(self.src, 'r+', encoding='utf-8') as f:            # 쓰던 줄이 끝났다
            data = f.read()
            f.seek(0); f.truncate()
            f.write(data[:data.rfind('\n') + 1])
        self._write([dict(_row('+8250002', event='unregister', ua=''), ts='2026-09-29T12:00:00')])
        self.assertEqual(U.fold(self.cfg, self.stats), 1)
        r = self._recs()[U.device_key('+8250002', UUID)]
        self.assertEqual((r['registered'], r['last_unregister']), (False, '2026-09-29T12:00:00'))

    def test_view_masks_imei_unless_raw(self):
        self._write([_row('+8250003', instance=IMEI)])
        U.fold(self.cfg, self.stats)
        rec = self._recs()[U.device_key('+8250003', IMEI)]
        self.assertEqual(U.view(rec)['imei'], '3512…0120')
        self.assertEqual(U.view(rec, raw_imei=True)['imei'], '351234567890120')


if __name__ == '__main__':
    unittest.main()
