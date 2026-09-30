"""관제 앱 관리 API — PTT 프로파일 PUT 의 잠긴 자격(allowAmbientListening) 단위시험.

dispatch_center.md §3.4 / mcptt_authorization.md §2.4: 원격 청취 자격은 콘솔(역할)에서 부여하고 관제 앱의
관리 범위로는 바꿀 수 없다 — 현재값과 다른 값이 실려 오면 400 not_editable, 같은 값은 무시(구 앱 호환),
다른 자격(allowCreateGroup 등)은 종전대로 반영된다. 미응답 멤버 알림 자격(allowNonAckUsersInfo — TS 24.484 anyExt
allow-to-receive-non-acknowledged-users-information, TS 24.379 §6.3.3.3)은 편집 가능한 선택 컬럼 — 실리면 반영, 없으면 현재값 유지.
해제 인가 셋(allowCancelGroupEmergency·allowCancelImminentPeril·allowCancelEmergencyAlert — TS 24.484 ruleset)도 편집 가능한
선택 컬럼이다 — 부재 시 값(0 · 1 · 발령 인가)이 0 이 아닌 것이 있어 "현재값 유지" 는 현재값이 부재 시 값과 다를 때만 싣는다.
"""
import asyncio
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from handlers import dispatch_directory as dd                            # noqa: E402
from httpsrv.handler import HandlerResult                                # noqa: E402


class _Cur:
    """_member_write 가 profile 분기 전에 하는 두 질의(users 행, 가입 행)만 흉내낸다."""

    def __init__(self):
        self._rows = []

    def execute(self, sql, params=None):
        if sql.startswith("SELECT id, org_id, name FROM users"):
            self._rows = [{'id': 5020, 'org_id': 'TEAM01', 'name': '관제1석'}]
        else:
            self._rows = [{'id': '+82510001001', 'service_ref': 'mcptt', 'sip_transport': 'TLS'}]

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def fetchall(self):
        return list(self._rows)


def _run(body, current_ambient: bool, current_non_ack: bool = False, cancel=None, absent=()):
    """profile PUT 을 한 번 실행하고 (status, body, _put_ptt_profile 에 넘어간 pb) 를 돌려준다.
    cancel = 현재 프로파일에 덧붙일 값(allow_cancel_* 등), absent = DB 에 없는 선택 컬럼."""
    prof = {'allow_emergency_call': 1, 'allow_emergency_alert': 1, 'allow_adhoc_call': 1,
            'allow_emergency_private_call': 1, 'allow_ambient_listening': 1 if current_ambient else 0,
            'allow_create_group': 0, 'allow_non_ack_users_info': 1 if current_non_ack else 0}
    prof.update(cancel or {})
    captured = {}

    async def fake_put(user_id, msisdn, pb, config):
        captured['pb'] = dict(pb)
        return HandlerResult(status=200, body=dict(pb), media_type='application/json')

    scope = {'directoryAdmin': 'all', 'codes': None, 'root': ''}
    with mock.patch.object(dd, 'in_scope', return_value=True), \
         mock.patch.object(dd._m, 'get_user_profile', return_value=prof), \
         mock.patch.object(dd._admin, '_put_ptt_profile', side_effect=fake_put), \
         mock.patch.object(dd._admin, '_opt_col_present', side_effect=lambda cur, c: c not in absent), \
         mock.patch.object(dd, '_audit'):
        r = asyncio.run(dd._member_write(_Cur(), {}, scope, 'PUT', ('5020', 'ptt', 'profile'), body,
                                         'user:5020', '127.0.0.1', 9999))
    return r.status, r.body, captured.get('pb')


class TestLockedAmbientListening(unittest.TestCase):
    def test_change_attempt_is_rejected(self):
        status, body, pb = _run({'allowCreateGroup': True, 'allowAmbientListening': True}, current_ambient=False)
        self.assertEqual(status, 400)
        self.assertEqual(body.get('error'), 'not_editable')
        self.assertEqual(body.get('key'), 'allowAmbientListening')
        self.assertIsNone(pb, '거절 시 프로파일 쓰기가 일어나면 안 된다')

    def test_revoke_attempt_is_rejected_too(self):
        status, body, _ = _run({'allowAmbientListening': False}, current_ambient=True)
        self.assertEqual(status, 400)
        self.assertEqual(body.get('error'), 'not_editable')

    def test_same_value_is_ignored_and_other_flags_apply(self):
        # 구 앱은 두 플래그를 항상 함께 보낸다 — 청취 자격이 현재값과 같으면 무시하고 나머지는 반영.
        status, body, pb = _run({'allowCreateGroup': True, 'allowAmbientListening': False}, current_ambient=False)
        self.assertEqual(status, 200)
        self.assertTrue(pb['allow_create_group'])
        self.assertNotIn('allow_ambient_listening', pb, '0 인 선택 컬럼은 싣지 않는다(컬럼 미적용 DB 호환)')
        self.assertFalse(body['profile']['allowAmbientListening'])

    def test_current_true_is_preserved_when_same_value_sent(self):
        status, _, pb = _run({'allowCreateGroup': False, 'allowAmbientListening': True}, current_ambient=True)
        self.assertEqual(status, 200)
        self.assertTrue(pb['allow_ambient_listening'], '현재값 1 은 그대로 유지된다')

    def test_omitted_key_keeps_current(self):
        status, _, pb = _run({'allowCreateGroup': True}, current_ambient=True)
        self.assertEqual(status, 200)
        self.assertTrue(pb['allow_ambient_listening'])


class TestNonAckUsersInfo(unittest.TestCase):
    def test_editable_from_dispatch_app(self):
        status, body, pb = _run({'allowNonAckUsersInfo': True}, current_ambient=False)
        self.assertEqual(status, 200)
        self.assertTrue(pb['allow_non_ack_users_info'])
        self.assertTrue(body['profile']['allowNonAckUsersInfo'])

    def test_revoke(self):
        status, body, pb = _run({'allowNonAckUsersInfo': False}, current_ambient=False, current_non_ack=True)
        self.assertEqual(status, 200)
        self.assertFalse(pb['allow_non_ack_users_info'])
        self.assertFalse(body['profile']['allowNonAckUsersInfo'])

    def test_omitted_keeps_current(self):
        _, _, pb = _run({'allowCreateGroup': True}, current_ambient=False, current_non_ack=True)
        self.assertTrue(pb['allow_non_ack_users_info'], '현재값 1 은 유지된다')
        _, _, pb = _run({'allowCreateGroup': True}, current_ambient=False, current_non_ack=False)
        self.assertNotIn('allow_non_ack_users_info', pb, '0 인 선택 컬럼은 싣지 않는다(컬럼 미적용 DB 호환)')


class TestCancelAuthz(unittest.TestCase):
    CUR_DEFAULT = {'allow_cancel_group_emergency': 0, 'allow_cancel_imminent_peril': 1, 'allow_cancel_emergency_alert': 1}

    def test_editable_from_dispatch_app(self):
        status, body, pb = _run({'allowCancelGroupEmergency': True, 'allowCancelImminentPeril': False,
                                 'allowCancelEmergencyAlert': False}, current_ambient=False, cancel=self.CUR_DEFAULT)
        self.assertEqual(status, 200)
        self.assertEqual((pb['allow_cancel_group_emergency'], pb['allow_cancel_imminent_peril'],
                          pb['allow_cancel_emergency_alert']), (True, False, False))
        self.assertEqual((body['profile']['allowCancelGroupEmergency'], body['profile']['allowCancelImminentPeril'],
                          body['profile']['allowCancelEmergencyAlert']), (True, False, False))

    def test_omitted_equal_to_absent_value_is_not_sent(self):
        # 현재값 = 부재 시 값(0 · 1 · 발령 인가) → 싣지 않아도 admin PUT 이 같은 값을 쓴다(컬럼 미적용 DB 호환)
        _, body, pb = _run({'allowCreateGroup': True}, current_ambient=False, cancel=self.CUR_DEFAULT)
        for c in self.CUR_DEFAULT:
            self.assertNotIn(c, pb)
        self.assertEqual((body['profile']['allowCancelGroupEmergency'], body['profile']['allowCancelImminentPeril'],
                          body['profile']['allowCancelEmergencyAlert']), (False, True, True), '응답 = 부재 시 값')

    def test_omitted_different_from_absent_value_keeps_current(self):
        _, _, pb = _run({'allowCreateGroup': True}, current_ambient=False,
                        cancel={'allow_cancel_group_emergency': 1, 'allow_cancel_imminent_peril': 0,
                                'allow_cancel_emergency_alert': 0})
        self.assertEqual((pb['allow_cancel_group_emergency'], pb['allow_cancel_imminent_peril'],
                          pb['allow_cancel_emergency_alert']), (True, False, False), '현재값 유지')

    def test_alert_change_keeps_cancel_alert(self):
        # 발령 인가를 끄면 부재 시 값(= 발령 인가)이 바뀐다 — 컬럼이 있으면 취소 인가 현재값(1)을 실어 유지한다
        _, _, pb = _run({'allowEmergencyAlert': False}, current_ambient=False, cancel=self.CUR_DEFAULT)
        self.assertFalse(pb['allow_emergency_alert'])
        self.assertTrue(pb['allow_cancel_emergency_alert'], '발령만 끄고 취소 인가는 그대로')

    def test_alert_change_on_unmigrated_db_sends_nothing(self):
        # 컬럼 미적용 DB — 취소 인가는 저장값이 없고 발령 인가를 따른다: 싣지 않는다(실으면 admin PUT 400)
        absent = ('allow_cancel_group_emergency', 'allow_cancel_imminent_peril', 'allow_cancel_emergency_alert')
        status, _, pb = _run({'allowEmergencyAlert': False}, current_ambient=False, cancel=self.CUR_DEFAULT, absent=absent)
        self.assertEqual(status, 200)
        for c in absent:
            self.assertNotIn(c, pb)

    def test_old_cache_entry_without_keys(self):
        # 키 없는 프로파일(옛 캐시) — 부재 시 값으로 보고 싣지 않는다
        _, body, pb = _run({'allowCreateGroup': True}, current_ambient=False)
        for c in self.CUR_DEFAULT:
            self.assertNotIn(c, pb)
        self.assertTrue(body['profile']['allowCancelEmergencyAlert'])


if __name__ == '__main__':
    unittest.main()
