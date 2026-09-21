"""ems/core/oam — 메시지 통계 **메서드 키** 판정 단위시험 (오프라인, DB/agent 없음).

`handlers.stats._parse_msg_method` 가 원문 한 줄에서 통계 키를 뽑는 규칙:

  요청       INVITE / REGISTER / …            (첫 토큰)
  응답       INVITE/401 · OPTIONS/200          (CSeq 메서드 + 상태코드)
  귀속 실패  401                               (CSeq 를 못 읽은 응답)
  JSON       HEARTBEAT · RELAY_ADD             (CMP/CSC 제어 — payload/hdr 의 cmd)
  쓰레기     unknown                           (SIP 도 JSON 도 아닌 바이트)

마지막 줄이 이 시험의 핵심이다 — 평문 포트로 들어온 TLS 레코드(0x16 …)가 메서드로 집계돼
`메서드 비중` 차트에 깨진 글자로 자리를 차지하던 결함(실측 2026-09-18)을 막는다. 화이트리스트
14종으로 자르면 CMP/CSC 명령(`RELAY_ADD` 등)이 함께 사라지므로 **토큰 꼴**로 거른다.

  python3 tests/test_oam_stats_method_key.py
"""
from __future__ import annotations

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

from handlers import stats  # noqa: E402

P = stats._parse_msg_method


def _req(method: str) -> str:
    return f"{method} sip:1000@cims SIP/2.0\r\nCSeq: 1 {method}\r\n\r\n"


def _resp(code: str, cseq_method: str | None) -> str:
    head = f"SIP/2.0 {code} X\r\n"
    if cseq_method:
        head += f"CSeq: 1 {cseq_method}\r\n"
    return head + "\r\n"


class MethodKey(unittest.TestCase):
    def test_requests(self):
        for m in ('INVITE', 'BYE', 'REGISTER', 'SUBSCRIBE', 'OPTIONS', 'ACK'):
            self.assertEqual(P(_req(m)), m)

    def test_response_is_attributed_to_its_request(self):
        # 상태코드만 세면 INVITE 의 200(호 성립)·BYE 의 200(호 해제)이 한 칸에 합쳐진다.
        self.assertEqual(P(_resp('200', 'INVITE')), 'INVITE/200')
        self.assertEqual(P(_resp('200', 'BYE')), 'BYE/200')
        self.assertEqual(P(_resp('401', 'REGISTER')), 'REGISTER/401')

    def test_response_without_cseq_keeps_bare_code(self):
        # 귀속은 못 해도 **응답이 있었다는 사실**은 남긴다 (unknown 으로 뭉개지 않는다).
        self.assertEqual(P(_resp('488', None)), '488')

    def test_negative_cseq_is_still_attributed(self):
        # 규격(RFC 3261 §20.16)상 음수 시퀀스는 없지만 스캐너가 보내고 psip 이 응답에 그대로
        # 복사해 되돌려 보낸다(F-59). 여기서 안 받으면 그 응답이 원 요청에 귀속되지 못하고
        # 생코드로 흩어져 표 맨 뒤에 `488`·`100` 으로 남는다 (실측 2026-09-18, 168건).
        self.assertEqual(P("SIP/2.0 488 X\r\nCSeq: -1 INVITE\r\n\r\n"), 'INVITE/488')
        self.assertEqual(P("SIP/2.0 100 X\r\nCSeq: -2147483648 INVITE\r\n\r\n"), 'INVITE/100')

    def test_json_control_commands_survive(self):
        # CMP/CSC 는 SIP 메서드가 아닌 이름을 쓴다 — 화이트리스트로 자르면 통째로 사라진다.
        self.assertEqual(P('{"hdr":{"cmd":"RELAY_ADD"},"payload":{}}'), 'RELAY_ADD')
        self.assertEqual(P('{"payload":{"cmd":"HEARTBEAT"}}'), 'HEARTBEAT')

    def test_extension_method_survives(self):
        # 표준 14종 밖이어도 토큰 꼴이면 살린다 (확장 메서드·평문 제어 명령).
        self.assertEqual(P(_req('USER_CHANGED')), 'USER_CHANGED')

    def test_binary_garbage_is_unknown(self):
        # 평문 포트로 들어온 TLS 레코드 — 0x16 = Handshake. 메서드 자리에 앉으면 안 된다.
        self.assertEqual(P('\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03'), 'unknown')
        self.assertEqual(P('\x16��]'), 'unknown')

    def test_empty_is_unknown(self):
        self.assertEqual(P(''), 'unknown')
        self.assertEqual(P('   \r\n'), 'unknown')


class SanitizeStoredKey(unittest.TestCase):
    """읽는 쪽 방어 — 이미 집계 저장본에 박힌 키까지 바로잡는다(재집계 없이)."""

    def test_valid_keys_pass_through(self):
        for k in ('INVITE', 'INVITE/200', 'BYE/200', 'OPTIONS/200', 'RELAY_ADD', 'USER_CHANGED'):
            self.assertEqual(stats.sanitize_method_key(k), k)

    def test_bare_status_code_is_kept(self):
        # 귀속 실패 응답 — "응답이 있었다"는 사실은 남겨야 한다.
        self.assertEqual(stats.sanitize_method_key('488'), '488')
        self.assertEqual(stats.sanitize_method_key('100'), '100')

    def test_garbage_becomes_unknown(self):
        self.assertEqual(stats.sanitize_method_key('\x16\ufffd\ufffd]'), 'unknown')
        self.assertEqual(stats.sanitize_method_key('\x16\x03\x01/200'), 'unknown')
        self.assertEqual(stats.sanitize_method_key(''), 'unknown')

    def test_garbage_keys_are_merged_not_dropped(self):
        # 쓰레기 키가 여럿이면 한 칸으로 **합산**된다 — 버리면 "이상 트래픽이 있다"는
        # 신호까지 사라진다.
        got = stats._sanitize_count_map(
            {'INVITE': 3, '\x16a': 6, '\x16b': 4, '488': 2})
        self.assertEqual(got, {'INVITE': 3, 'unknown': 10, '488': 2})

    def test_non_dict_is_empty(self):
        self.assertEqual(stats._sanitize_count_map(None), {})


if __name__ == '__main__':
    unittest.main(verbosity=2)
