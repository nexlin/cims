"""csc — 관제 데스크 통합 이력 조회 단위 시험 (오프라인, 임시 ServiceLogDir 트리).

dispatch_center.md §5.6/§8.4 · mcdata_messaging.md §4.3: `GET /provisioning/history?kind=call|ptt|message`
= 관제 그룹 범위(monitor_scope→members / ptt_listen→ptt_groups) 안의 지난 이력만 커서(since)로 준다.
`services/dispatch_history.py` 의 파일 스캔·범위 대조·커서, `mcptt.handle_provisioning_history` 의
토큰·범위 게이트(403)·감사.

  python3 -m unittest tests.test_csc_provisioning_history
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import types
import unittest
from datetime import datetime, timedelta

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))

import services.dispatch_history as dh  # noqa: E402
import services.mcptt as m  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402


def _bucket(sl, *parts):
    d = os.path.join(sl, *parts)
    os.makedirs(d, exist_ok=True)
    return d


def _now_parts(dt):
    return (f"{dt.year:04d}", f"{dt.month:02d}", f"{dt.day:02d}", f"{dt.hour:02d}")


class _Tree:
    """임시 ServiceLogDir 에 call.json/session.json/messages.jsonl 을 심는다. 'now' 시각 버킷 사용."""

    def __init__(self):
        self.sl = tempfile.mkdtemp(prefix="cims_hist_")
        self.now = datetime.now().replace(microsecond=0)
        self._ptt_seq = 0

    def ts(self, minutes_ago=0):
        return (self.now - timedelta(minutes=minutes_ago)).strftime("%Y-%m-%dT%H:%M:%S")

    def call(self, call_id, initiator, callee, minutes_ago=5, state="ended"):
        y, mo, d, h = _now_parts(self.now - timedelta(minutes=minutes_ago))
        # 실서버 레이아웃: {sl}/volte/{Y}/{M}/{D}/{H}/{prefix}/{caller}/{cid}.d/call.json
        dd = _bucket(self.sl, "volte", y, mo, d, h, initiator[:10], initiator, call_id + ".d")
        rec = {"call_id": call_id, "call_type": "volte", "initiator": initiator, "callee": callee,
               "state": state, "invite_time": self.ts(minutes_ago + 1), "answer_time": self.ts(minutes_ago),
               "end_time": self.ts(minutes_ago) if state == "ended" else None,
               "duration": 30, "end_reason": "normal"}
        with open(os.path.join(dd, "call.json"), "w") as f:
            json.dump(rec, f)

    def ptt(self, gid, sesid, initiator, minutes_ago=5):
        y, mo, d, h = _now_parts(self.now - timedelta(minutes=minutes_ago))
        self._ptt_seq += 1
        dd = _bucket(self.sl, "ptt", str(self._ptt_seq), y, mo, d, h, "S" + str(self._ptt_seq))
        rec = {"mcptt_group_id": gid, "name": gid, "sesid": sesid, "initiator": initiator,
               "call_id": "cid_" + sesid, "state": "ended", "start_time": self.ts(minutes_ago + 1),
               "end_time": self.ts(minutes_ago), "member_count": 3}
        with open(os.path.join(dd, "session.json"), "w") as f:
            json.dump(rec, f)

    def group_msg(self, gid, frm, text, minutes_ago=5, msg_id=None):
        y, mo, d, h = _now_parts(self.now - timedelta(minutes=minutes_ago))
        dd = _bucket(self.sl, "message", gid, y, mo, d, h)
        rec = {"ts": self.ts(minutes_ago), "group": gid, "from": frm, "msg_type": "sds",
               "conv_id": "c1", "msg_id": msg_id or ("m_" + gid + str(minutes_ago)), "text": text,
               "size": len(text), "disposition_req": 0, "fanout": 2}
        with open(os.path.join(dd, "messages.jsonl"), "a") as f:
            f.write(json.dumps(rec) + "\n")

    def direct_msg(self, frm, to, text, minutes_ago=5, msg_id=None):
        y, mo, d, h = _now_parts(self.now - timedelta(minutes=minutes_ago))
        dd = _bucket(self.sl, "message_direct", y, mo, d, h)
        rec = {"ts": self.ts(minutes_ago), "from": frm, "to": to, "msg_type": "text",
               "conv_id": "", "msg_id": msg_id or ("d_" + str(minutes_ago)), "text": text,
               "size": len(text), "disposition_req": 0}
        with open(os.path.join(dd, "messages.jsonl"), "a") as f:
            f.write(json.dumps(rec) + "\n")


class ReaderTests(unittest.TestCase):
    def setUp(self):
        self.t = _Tree()

    def _scope(self, members=(), ptt=()):
        return {"members": {dh.userpart(x) for x in members}, "ptt_groups": set(ptt),
                "groupId": "dg1", "monitorScope": "all", "pttListen": "all"}

    def test_call_scope_filter(self):
        self.t.call("call-A", "+821310002001", "+821310009999")   # 감시 멤버 발신
        self.t.call("call-B", "+821310007777", "+821310008888")   # 범위 밖
        items, nxt = dh.query(self.t.sl, "call", self._scope(members=["tel:+821310002001"]), None, 100)
        ids = [x["id"] for x in items]
        self.assertIn("call-A", ids)
        self.assertNotIn("call-B", ids)
        self.assertEqual(items[0]["kind"], "call")
        self.assertTrue(nxt)

    def test_call_matches_callee_too(self):
        self.t.call("call-C", "+821310007777", "+821310002002")   # 감시 멤버 수신
        items, _ = dh.query(self.t.sl, "call", self._scope(members=["+821310002002"]), None, 100)
        self.assertEqual([x["id"] for x in items], ["call-C"])

    def test_ptt_scope_filter(self):
        self.t.ptt("g002", "ses-1", "+82510002001")
        self.t.ptt("g009", "ses-2", "+82510009999")
        items, _ = dh.query(self.t.sl, "ptt", self._scope(ptt=["g002"]), None, 100)
        self.assertEqual([x["id"] for x in items], ["ses-1"])
        self.assertEqual(items[0]["groupId"], "g002")

    def test_message_group_scope(self):
        self.t.group_msg("g002", "+82510002001", "hi team")
        self.t.group_msg("g009", "+82510009999", "other group")
        items, _ = dh.query(self.t.sl, "message", self._scope(ptt=["g002"]), None, 100)
        self.assertEqual([x["text"] for x in items], ["hi team"])
        self.assertEqual(items[0]["scope"], "group")

    def test_message_direct_scope_sender_or_recipient(self):
        self.t.direct_msg("+821310002001", "+821310009999", "member sent")     # 멤버 발신
        self.t.direct_msg("+821310009999", "+821310002002", "member recv")     # 멤버 수신
        self.t.direct_msg("+821310007777", "+821310008888", "outsiders")       # 범위 밖
        items, _ = dh.query(self.t.sl, "message",
                            self._scope(members=["+821310002001", "+821310002002"]), None, 100)
        texts = sorted(x["text"] for x in items)
        self.assertEqual(texts, ["member recv", "member sent"])
        self.assertTrue(all(x["scope"] == "direct" for x in items))

    def test_since_cursor_excludes_older(self):
        self.t.group_msg("g002", "+82510002001", "old", minutes_ago=30, msg_id="old1")
        self.t.group_msg("g002", "+82510002001", "new", minutes_ago=2, msg_id="new1")
        cursor = (self.t.now - timedelta(minutes=10)).strftime("%Y-%m-%dT%H:%M:%S")
        items, nxt = dh.query(self.t.sl, "message", self._scope(ptt=["g002"]),
                              dh.parse_ts(cursor), 100)
        self.assertEqual([x["text"] for x in items], ["new"])
        # nextSince 로 다시 조회하면 빈 목록(그 이후 없음)
        items2, _ = dh.query(self.t.sl, "message", self._scope(ptt=["g002"]), dh.parse_ts(nxt), 100)
        self.assertEqual(items2, [])

    def test_limit_returns_most_recent(self):
        for i in range(5):
            self.t.group_msg("g002", "+82510002001", f"msg{i}", minutes_ago=20 - i * 2, msg_id=f"m{i}")
        items, _ = dh.query(self.t.sl, "message", self._scope(ptt=["g002"]), None, 3)
        self.assertEqual(len(items), 3)
        self.assertEqual([x["text"] for x in items], ["msg2", "msg3", "msg4"])   # 최근 3, 오름차순

    def test_empty_scope_returns_nothing(self):
        self.t.call("call-A", "+821310002001", "+821310009999")
        self.assertEqual(dh.query(self.t.sl, "call", self._scope(), None, 100)[0], [])

    def test_missing_service_log_dir(self):
        items, nxt = dh.query("/nonexistent/xyz", "call", self._scope(members=["+8213"]), None, 100)
        self.assertEqual(items, [])

    def test_parse_ts_forms(self):
        self.assertIsNotNone(dh.parse_ts("2026-09-06T19:05:00"))
        self.assertIsNotNone(dh.parse_ts("2026-09-06 19:05:00.123"))
        self.assertIsNotNone(dh.parse_ts("1788000000"))
        self.assertIsNone(dh.parse_ts(""))
        self.assertIsNone(dh.parse_ts("garbage"))

    def test_userpart_normalizes(self):
        self.assertEqual(dh.userpart("tel:+82510001001"), "+82510001001")
        self.assertEqual(dh.userpart("sip:+82510001001@ptt.example.org"), "+82510001001")
        self.assertEqual(dh.userpart("SIP:G002@X"), "g002")


class WireFormatTests(unittest.TestCase):
    """format_item — 내부 row → 앱 HistoryEntry 계약. 시각 offset ISO·event 이름표·group tel: uri."""

    def test_call_item(self):
        row = {"kind": "call", "ts": "2026-09-06T19:05:00", "id": "c1", "initiator": "+8210001",
               "callee": "+8210002", "answerTime": "2026-09-06T19:04:59", "duration": 42, "state": "ended"}
        w = dh.format_item(row)
        self.assertEqual((w["id"], w["kind"], w["event"], w["from"], w["to"], w["group"], w["duration"]),
                         ("c1", "call", "call.answered", "+8210001", "+8210002", "", 42))
        self.assertRegex(w["time"], r'^2026-09-06T19:05:00[+-]\d{2}:\d{2}$')
        self.assertFalse(w["emergency"]); self.assertEqual(w["text"], "")

    def test_call_missed_when_unanswered(self):
        w = dh.format_item({"kind": "call", "ts": "t", "id": "c2", "initiator": "a", "callee": "b",
                            "answerTime": None, "duration": 0})
        self.assertEqual(w["event"], "call.missed")

    def test_ptt_item_group_uri_and_duration(self):
        w = dh.format_item({"kind": "ptt", "ts": "2026-09-06T19:00:30", "id": "s1", "groupId": "g002",
                            "initiator": "+8250001", "state": "ended", "startTime": "2026-09-06T19:00:00",
                            "endTime": "2026-09-06T19:00:30"})
        self.assertEqual((w["kind"], w["event"], w["group"], w["from"], w["to"], w["duration"]),
                         ("ptt", "ptt.session.end", "tel:g002", "+8250001", "", 30))

    def test_ptt_active_is_session_start(self):
        w = dh.format_item({"kind": "ptt", "ts": "t", "id": "s2", "groupId": "g002", "state": "active"})
        self.assertEqual(w["event"], "ptt.session.start")

    def test_message_group_vs_direct_event(self):
        g = dh.format_item({"kind": "message", "ts": "t", "id": "m1", "scope": "group", "groupId": "g002",
                            "from": "+8250001", "text": "hi"})
        d = dh.format_item({"kind": "message", "ts": "t", "id": "m2", "scope": "direct", "from": "+8210001",
                            "to": "+8210002", "text": "dm"})
        self.assertEqual((g["event"], g["group"], g["text"]), ("message.sds", "tel:g002", "hi"))
        self.assertEqual((d["event"], d["group"], d["to"]), ("message.sms", "", "+8210002"))

    def test_bad_time_is_empty(self):
        self.assertEqual(dh.format_item({"kind": "call", "ts": None, "id": "c", "initiator": "a", "callee": "b"})["time"], "")


class HandlerTests(unittest.TestCase):
    """handle_provisioning_history — 토큰·kind·범위 게이트(403)·감사·응답 형태."""

    def setUp(self):
        self.t = _Tree()
        self._saved = (m._DB_CONFIG, m.extract_token, m._SERVICE_LOG_DIR, m.dispatch_discovery,
                       sys.modules.get("pymysql"), sys.modules.get("services.fm_reporter"))
        m._DB_CONFIG = {"Host": "127.0.0.1", "Port": 3306, "User": "cims", "Password": "", "Db": "cims"}
        m._SERVICE_LOG_DIR = self.t.sl
        self.token = {"sub": "disp01", "mcptt_id": "tel:+821310001001", "scope": [m.SCOPE_PROVISIONING]}
        m.extract_token = lambda hdr: self.token if hdr else None
        # DB: user_id 조회만 흉내
        cur = types.SimpleNamespace()
        cur.execute = lambda q, a=None: setattr(cur, "_r", (5020,) if "user_id FROM volte" in q else None)
        cur.fetchone = lambda: getattr(cur, "_r", None)
        conn = types.SimpleNamespace(cursor=lambda: cur, close=lambda: None)
        sys.modules["pymysql"] = types.SimpleNamespace(connect=lambda **kw: conn)
        # 범위 = disp01 관제 그룹 (dispatch_discovery 스텁)
        self._dispatcher = True
        def _disc(c, uid):
            if not self._dispatcher:
                return None
            return {"groupId": "dg-dispatch01", "monitorScope": "all", "pttListen": "all",
                    "members": [{"volteAor": "tel:+821310002001"}, {"volteAor": "tel:+821310002002"}],
                    "pttTargets": [{"id": "g002"}]}
        m.dispatch_discovery = _disc
        # 감사 캡처
        self.audits = []
        fake_r = types.SimpleNamespace(node="node1",
                                       send_event=lambda *a, **k: self.audits.append((a, k)))
        sys.modules["services.fm_reporter"] = types.SimpleNamespace(get=lambda: fake_r)

    def tearDown(self):
        (m._DB_CONFIG, m.extract_token, m._SERVICE_LOG_DIR, m.dispatch_discovery, pm, fm) = self._saved
        if pm is None:
            sys.modules.pop("pymysql", None)
        else:
            sys.modules["pymysql"] = pm
        if fm is None:
            sys.modules.pop("services.fm_reporter", None)
        else:
            sys.modules["services.fm_reporter"] = fm

    def _get(self, kind="call", since=None, limit=None, token="x"):
        qp = {"kind": kind}
        if since:
            qp["since"] = since
        if limit:
            qp["limit"] = str(limit)
        h = {"authorization": "Bearer " + token} if token else {}
        a = HandlerArgs("GET", "/provisioning/history", "127.0.0.1", 0, headers=h, query_params=qp)
        return asyncio.run(m.handle_provisioning_history(a, {}))

    def test_no_token_401(self):
        r = self._get(token="")
        self.assertEqual(r.status, 401)

    def test_bad_kind_400(self):
        r = self._get(kind="bogus")
        self.assertEqual(r.status, 400)

    def test_non_dispatcher_403(self):
        self._dispatcher = False
        r = self._get(kind="call")
        self.assertEqual(r.status, 403)
        self.assertEqual(r.body["error"], "no_monitor_scope")

    def test_call_history_wire_contract_and_audit(self):
        self.t.call("call-A", "+821310002001", "+821310009999")
        r = self._get(kind="call")
        self.assertEqual(r.status, 200)
        # 앱 HistoryClient 계약 — 최상위 items/next, 응답 ETag
        self.assertIn("items", r.body); self.assertIn("next", r.body)
        self.assertNotIn("nextSince", r.body); self.assertNotIn("kind", r.body)
        self.assertRegex(r.headers.get("ETag", ""), r'^"[0-9a-f]{32}"$')
        it = r.body["items"][0]
        self.assertEqual(it["id"], "call-A")
        self.assertEqual(it["kind"], "call")
        self.assertEqual(it["event"], "call.answered")           # answer_time 있음
        self.assertEqual((it["from"], it["to"]), ("+821310002001", "+821310009999"))
        self.assertRegex(it["time"], r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$')
        self.assertEqual(it["group"], ""); self.assertIsInstance(it["duration"], int)
        self.assertIn("emergency", it); self.assertIn("text", it)
        # 감사 E-AUD-016 (call_monitored, tap_mode=history) 1건
        self.assertEqual(len(self.audits), 1)
        _args, kw = self.audits[0]
        self.assertEqual((_args[0], kw["kind"], kw["params"]["tap_mode"], kw["params"]["hist_kind"]),
                         ("call_monitored", "audit", "history", "call"))

    def test_if_none_match_304_no_audit(self):
        self.t.call("call-A", "+821310002001", "+821310009999")
        first = self._get(kind="call")
        self.audits.clear()
        # 같은 ETag 로 재요청 → 304, 감사 없음
        a = HandlerArgs("GET", "/provisioning/history", "127.0.0.1", 0,
                        headers={"authorization": "Bearer x", "if-none-match": first.headers["ETag"]},
                        query_params={"kind": "call"})
        r304 = asyncio.run(m.handle_provisioning_history(a, {}))
        self.assertEqual(r304.status, 304)
        self.assertEqual(r304.headers["ETag"], first.headers["ETag"])
        self.assertEqual(len(self.audits), 0)

    def test_message_history_wire_group_and_direct(self):
        self.t.group_msg("g002", "+82510002001", "team msg")
        self.t.direct_msg("+821310002001", "+821310009999", "dm")
        r = self._get(kind="message")
        by_text = {x["text"]: x for x in r.body["items"]}
        self.assertEqual(by_text["team msg"]["event"], "message.sds")
        self.assertEqual(by_text["team msg"]["group"], "tel:g002")
        self.assertEqual(by_text["dm"]["event"], "message.sms")
        self.assertEqual(by_text["dm"]["group"], "")

    def test_mcptt_route_registered(self):
        paths = [p for (p, _h, _k) in m.CSC_HANDLER_LIST]
        self.assertIn("/provisioning/history", paths)


if __name__ == "__main__":
    unittest.main()


class ExtendedWireTests(unittest.TestCase):
    """이력 화면(§4.6) 확장 필드 — 콘솔 VoLTE/PTT 이력과 같은 열을 앱이 그릴 수 있게 종류별로 실린다. 공통 필드는 그대로."""

    def test_call_item_extended_fields(self):
        w = dh.format_item({"kind": "call", "ts": "2026-09-06T19:05:00", "id": "c1", "initiator": "a", "callee": "b",
                            "callType": "volte_video", "state": "ended", "inviteTime": "2026-09-06T19:04:00",
                            "answerTime": "2026-09-06T19:04:05", "endTime": "2026-09-06T19:05:00",
                            "duration": 55, "sipStatus": 200, "endReason": "normal"})
        self.assertEqual((w["callType"], w["state"], w["endReason"], w["sipStatus"]), ("volte_video", "ended", "normal", 200))
        self.assertRegex(w["inviteTime"], r'^2026-09-06T19:04:00[+-]\d{2}:\d{2}$')
        self.assertRegex(w["answerTime"], r'^2026-09-06T19:04:05[+-]\d{2}:\d{2}$')
        self.assertRegex(w["endTime"], r'^2026-09-06T19:05:00[+-]\d{2}:\d{2}$')
        # 값이 없으면 빈 문자열/0 — 앱은 필드 유무가 아니라 값으로 판단한다
        w2 = dh.format_item({"kind": "call", "ts": "t", "id": "c2", "initiator": "a", "callee": "b"})
        self.assertEqual((w2["callType"], w2["answerTime"], w2["sipStatus"], w2["endReason"]), ("volte", "", 0, ""))

    def test_ptt_item_extended_fields(self):
        w = dh.format_item({"kind": "ptt", "ts": "2026-09-06T19:00:30", "id": "s1", "groupId": "g002", "groupName": "1팀",
                            "sessionKind": "group", "state": "ended", "startTime": "2026-09-06T19:00:00",
                            "endTime": "2026-09-06T19:00:30", "memberCount": 5, "people": ["+8250001", "+8250002"],
                            "turnCount": 7, "speakerCount": 2, "totalSpeechMs": 12345, "talkMs": 13000, "maxConcurrent": 2,
                            "floorControl": "on", "floorPolicy": "dual", "maxTalkers": 2})
        self.assertEqual((w["sessionKind"], w["state"], w["groupName"], w["memberCount"]), ("group", "ended", "1팀", 5))
        self.assertEqual((w["turnCount"], w["speakerCount"], w["totalSpeechMs"], w["talkMs"], w["maxConcurrent"]), (7, 2, 12345, 13000, 2))
        self.assertEqual((w["floorControl"], w["floorPolicy"], w["maxTalkers"]), ("on", "dual", 2))
        self.assertEqual(w["people"], ["+8250001", "+8250002"])
        self.assertRegex(w["startTime"], r'^2026-09-06T19:00:00[+-]\d{2}:\d{2}$')
        # 파일 스캔 행(지표 없음) 도 같은 키를 0 으로 낸다
        w2 = dh.format_item({"kind": "ptt", "ts": "t", "id": "s2", "groupId": "g002", "state": "active"})
        self.assertEqual((w2["sessionKind"], w2["turnCount"], w2["people"], w2["floorControl"]), ("group", 0, [], ""))

    def test_hour_histogram_axis(self):
        rows = [{"kind": "call", "ts": "2026-09-06T19:05:00", "inviteTime": "2026-09-06T18:59:50"},     # INVITE 시각 기준
                {"kind": "ptt", "ts": "2026-09-06T19:30:00", "startTime": "2026-09-06T19:00:00"},        # 세션 시작 기준
                {"kind": "message", "ts": "2026-09-06T19:45:00"},                                        # 항목 시각
                {"kind": "call", "ts": None}]
        self.assertEqual(dh.hour_histogram(rows), {"18": 1, "19": 2})

    def test_query_ex_returns_hours_before_limit(self):
        t = _Tree()
        t.call("c1", "+821310002001", "+821310009999", minutes_ago=3)
        t.call("c2", "+821310002001", "+821310009998", minutes_ago=2)
        scope = {"members": {"+821310002001"}, "ptt_groups": set()}
        items, nxt, hours = dh.query_ex(t.sl, "call", scope, None, 1)
        self.assertEqual(len(items), 1)                          # limit 절삭
        self.assertEqual(sum(hours.values()), 2)                 # 분포는 절삭 전 전체
        self.assertEqual(dh.query(t.sl, "call", scope, None, 1)[0], items)


class OamIndexRowTests(unittest.TestCase):
    """OAM `/api/v1/ptt/sessions` 항목 → 내부 row (창 조회). 녹취 id 는 콘솔 recIdOf 와 같은 규칙."""

    def test_recording_id_rules(self):
        self.assertEqual(dh.ptt_recording_id("3", "S20260906190102000000_1", ["2026090619"]),
                         "ptt/3/2026/09/06/19/S20260906190102000000_1")
        self.assertEqual(dh.ptt_recording_id("3", "S20260906190102000000_1", None),          # windows 없어도 세션키에서 시각
                         "ptt/3/2026/09/06/19/S20260906190102000000_1")
        self.assertEqual(dh.ptt_recording_id("3", "2026090619", ["2026090619"]), "ptt/3/2026/09/06/19")   # 구 녹취 = 시간창
        self.assertEqual(dh.ptt_recording_id("", "S20260906190102000000_1", None), "")
        self.assertEqual(dh.ptt_recording_id("3", "x", None), "")

    def test_row_mapping(self):
        it = {"dir": "S20260906190102000000_1", "windows": ["2026090619"], "group_key": "3", "kind": "group",
              "mcptt_group_id": "g002", "group_name": "1팀", "group_type": "prearranged", "member_count": 4,
              "sesid": "ses-9", "call_id": "cid-9", "initiator": "+8250001", "people": ["+8250001", "+8250002"],
              "start_time": "2026-09-06T19:01:02", "end_time": "2026-09-06T19:03:00", "state": "ended",
              "turn_count": 5, "speaker_count": 2, "total_speech_ms": 40000, "talk_ms": 41000, "max_concurrent": 1,
              "segment_count": 3, "floor_control": "on", "floor_policy": "single", "max_talkers": 1}
        r = dh.ptt_row_from_oam(it, "")
        self.assertEqual((r["kind"], r["id"], r["groupId"], r["groupName"], r["sessionKind"]), ("ptt", "ses-9", "g002", "1팀", "group"))
        self.assertEqual(r["recordingId"], "ptt/3/2026/09/06/19/S20260906190102000000_1")
        self.assertTrue(r["hasRecording"])                        # sl_dir 없으면 segment_count 로
        self.assertEqual((r["ts"], r["startTime"], r["endTime"]), ("2026-09-06T19:03:00", "2026-09-06T19:01:02", "2026-09-06T19:03:00"))
        self.assertEqual((r["turnCount"], r["speakerCount"], r["totalSpeechMs"], r["maxConcurrent"]), (5, 2, 40000, 1))
        self.assertEqual(r["people"], ["+8250001", "+8250002"])
        w = dh.format_item(r)
        self.assertEqual((w["event"], w["group"], w["from"], w["duration"], w["turnCount"]), ("ptt.session.end", "tel:g002", "+8250001", 118, 5))
        # 진행중 → session.start, id 폴백(sesid 없으면 call_id → dir)
        live = dh.ptt_row_from_oam({"dir": "S1_1", "group_key": "3", "mcptt_group_id": "g002", "state": "active",
                                    "start_time": "2026-09-06T19:01:02", "end_time": None}, "")
        self.assertEqual((live["id"], dh.format_item(live)["event"]), ("S1_1", "ptt.session.start"))
        self.assertIsNone(dh.ptt_row_from_oam({"dir": "", "mcptt_group_id": "g002"}, ""))
        self.assertIsNone(dh.ptt_row_from_oam("junk", ""))

    def test_has_recording_prefers_file(self):
        t = _Tree()
        t.ptt("g002", "ses-1", "+82510002001")
        row = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}, "groupId": "dg1"}, None, 10)[0][0]
        parts = row["recordingId"].split("/")
        # 구 녹취형(세션키 아님) — 시간창 디렉터리가 곧 녹취 id. 세션키형은 test_recording_id_rules.
        it = {"dir": "".join(parts[2:6]), "windows": ["".join(parts[2:6])], "group_key": "1", "mcptt_group_id": "g002",
              "start_time": t.ts(6), "segment_count": 0}
        r = dh.ptt_row_from_oam(it, t.sl)
        self.assertEqual(r["recordingId"], "/".join(parts[:6]))
        self.assertFalse(r["hasRecording"])
        with open(os.path.join(t.sl, r["recordingId"], "segments.jsonl"), "w") as f:
            f.write("{}\n")
        self.assertTrue(dh.ptt_row_from_oam(it, t.sl)["hasRecording"])


class PttWindowHandlerTests(HandlerTests):
    """kind=ptt & until → OAM 세션 인덱스 프록시(범위 저장 키), 실패 시 파일 스캔 폴백. 응답에 hours."""

    def setUp(self):
        super().setUp()
        import handlers.dispatch_recordings as dr
        self._dr = dr
        self._saved_fetch = dr.fetch_ptt_sessions
        cur = sys.modules["pymysql"].connect().cursor()
        cur.fetchall = lambda: [(1, "g002"), (2, "g009")]

    def tearDown(self):
        self._dr.fetch_ptt_sessions = self._saved_fetch
        super().tearDown()

    def _window(self, kind="ptt"):
        since = (self.t.now - timedelta(hours=2)).strftime("%Y-%m-%dT%H:%M:%S")
        until = self.t.now.strftime("%Y-%m-%dT%H:%M:%S")
        a = HandlerArgs("GET", "/provisioning/history", "127.0.0.1", 0, headers={"authorization": "Bearer x"},
                        query_params={"kind": kind, "since": since, "until": until, "limit": "100"})
        return asyncio.run(m.handle_provisioning_history(a, {}))

    def test_window_proxies_oam_index_with_scope_keys(self):
        seen = []
        start = (self.t.now - timedelta(minutes=10)).strftime("%Y-%m-%dT%H:%M:%S")
        end = (self.t.now - timedelta(minutes=8)).strftime("%Y-%m-%dT%H:%M:%S")

        def fake_fetch(cfg, since_dt, until_dt, keys):
            seen.append((cfg, since_dt, until_dt, set(keys)))
            return [{"dir": "S20260906190102000000_1", "windows": ["2026090619"], "group_key": "1", "kind": "group",
                     "mcptt_group_id": "g002", "group_name": "1팀", "sesid": "ses-9", "initiator": "+8250001",
                     "people": ["+8250001"], "start_time": start, "end_time": end, "state": "ended",
                     "turn_count": 4, "speaker_count": 1, "total_speech_ms": 9000, "segment_count": 1},
                    {"dir": "S20260906190102000000_2", "group_key": "7", "mcptt_group_id": "g777",       # 범위 밖 그룹 → 제외
                     "start_time": start, "end_time": end, "state": "ended"}]
        self._dr.fetch_ptt_sessions = fake_fetch
        r = self._window()
        self.assertEqual(r.status, 200)
        self.assertEqual(seen[0][3], {"1"})                       # 청취 그룹(g002) 의 저장 키만
        self.assertEqual([x["id"] for x in r.body["items"]], ["ses-9"])
        it = r.body["items"][0]
        self.assertEqual((it["turnCount"], it["groupName"], it["hasRecording"]), (4, "1팀", True))
        self.assertEqual(it["recordingId"], "ptt/1/2026/09/06/19/S20260906190102000000_1")
        self.assertEqual(sum(r.body["hours"].values()), 1)
        self.assertEqual(len(self.audits), 1)

    def test_window_falls_back_to_scan_when_oam_unavailable(self):
        self._dr.fetch_ptt_sessions = lambda cfg, s, u, keys: None
        self.t.ptt("g002", "ses-1", "+82510002001")
        r = self._window()
        self.assertEqual(r.status, 200)
        self.assertEqual([x["id"] for x in r.body["items"]], ["ses-1"])
        self.assertEqual(r.body["items"][0]["turnCount"], 0)      # 스캔 경로 = 지표 없음
        self.assertIn("hours", r.body)

    def test_polling_does_not_touch_oam(self):
        self._dr.fetch_ptt_sessions = lambda *a: self.fail("polling must scan files")
        self.t.ptt("g002", "ses-1", "+82510002001")
        r = self._get(kind="ptt")
        self.assertEqual([x["id"] for x in r.body["items"]], ["ses-1"])
        self.assertIn("hours", r.body)


class PttLivenessTests(unittest.TestCase):
    """세션 시작 스냅샷(session.json 에 state/end_time 없음)은 라이브 상태 파일이 없으면 **종료**다 — 종료 시각은 events/segments 에서."""

    def _session(self, t, gid, sesid, minutes_ago, with_state=False, end_event=True):
        y, mo, d, h = _now_parts(t.now - timedelta(minutes=minutes_ago))
        t._ptt_seq += 1
        dd = _bucket(t.sl, "ptt", str(t._ptt_seq), y, mo, d, h, "S" + str(t._ptt_seq))
        with open(os.path.join(dd, "session.json"), "w") as f:
            json.dump({"mcptt_group_id": gid, "name": gid, "sesid": sesid, "initiator": "+8250001", "call_id": "cid_" + sesid,
                       "start_time": t.ts(minutes_ago)}, f)                       # state/end_time 없음 = 시작 스냅샷
        with open(os.path.join(dd, "events.jsonl"), "w") as f:
            f.write(json.dumps({"ts": t.ts(minutes_ago), "type": "session_start"}) + "\n")
            f.write(json.dumps({"ts": t.ts(minutes_ago - 1), "type": "member_join", "member": "+8250002"}) + "\n")
            if end_event:
                f.write(json.dumps({"ts": t.ts(minutes_ago - 3), "type": "session_end"}) + "\n")
        if with_state:
            sd = _bucket(t.sl, "state", "ptt")
            with open(os.path.join(sd, "+8250001.json"), "w") as f:
                json.dump({"kind": "ptt", "subscriber_id": "+8250001", "session_id": sesid, "call_id": "cid_" + sesid,
                           "group_id": gid, "role": "initiator", "state": "active", "started_at": t.ts(minutes_ago)}, f)

    def test_snapshot_without_end_is_ended_with_end_from_events(self):
        t = _Tree()
        self._session(t, "g002", "ses-old", 20)
        items, _ = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}}, None, 10)
        r = items[0]
        self.assertEqual((r["id"], r["state"]), ("ses-old", "ended"))
        self.assertEqual(r["endTime"], t.ts(17))                                  # session_end 시각
        self.assertEqual(dh.format_item(r)["event"], "ptt.session.end")
        self.assertEqual(dh.format_item(r)["duration"], 180)

    def test_snapshot_with_live_state_file_is_active(self):
        t = _Tree()
        self._session(t, "g002", "ses-live", 5, with_state=True, end_event=False)
        items, _ = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}}, None, 10)
        self.assertEqual([(x["id"], x["state"]) for x in items], [("ses-live", "active")])   # 라이브 스냅샷과 버킷 행이 한 건으로
        self.assertEqual(dh.format_item(items[0])["event"], "ptt.session.start")

    def test_no_end_event_falls_back_to_last_event(self):
        t = _Tree()
        self._session(t, "g002", "ses-x", 30, end_event=False)
        r = dh.query(t.sl, "ptt", {"members": set(), "ptt_groups": {"g002"}}, None, 10)[0][0]
        self.assertEqual((r["state"], r["endTime"]), ("ended", t.ts(29)))
