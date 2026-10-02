"""csc/src/services/mcdata_fd.py — MCData FD 콘텐츠 서버(media storage function) 단위 시험 (오프라인, DB 없음).

TS 24.282 §10.2.2(업로드 — multipart/mixed: mcdata-info + octet-stream, 201 Created + Location, 전송 제어 403, 크기 413) ·
§10.2.3(다운로드 — 수신 제어 403) · §6.7.3(HEAD 존재 확인 — 200 / 404, 제어 기능은 내부 토큰) + 간이형(query) 업로드.

  python3 -m unittest tests.test_csc_mcdata_fd
"""
from __future__ import annotations

import asyncio
import os
import shutil
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(_REPO_ROOT, "csc", "src"))
for _v in (os.path.join(_REPO_ROOT, "csc", "vendor"), "/opt/cims-agent/modules/csc/current/csc/vendor"):
    if os.path.isdir(_v) and _v not in sys.path:
        sys.path.append(_v)
        break

import services.mcptt as m  # noqa: E402
from services import mcdata_fd as fd  # noqa: E402
from services.auc import auc  # noqa: E402
from httpsrv.handler import HandlerArgs  # noqa: E402

ALICE, BOB, CAROL = "tel:+82500000001", "tel:+82500000002", "tel:+82500000003"
GROUP, GROUP_NOFD = "tel:g-fd", "tel:g-nofd"
BOUNDARY = "b0undary"
PUBLIC = "https://csc.example:4430"


def _info(req_type, calling=ALICE, group=None):
    g = f'<mcdata-request-uri type="Normal"><mcdataURI>{group}</mcdataURI></mcdata-request-uri>' if group else ''
    return (f'<?xml version="1.0" encoding="UTF-8"?><mcdatainfo xmlns="urn:3gpp:ns:mcdataInfo:1.0"><mcdata-Params>'
            f'<request-type>{req_type}</request-type>{g}'
            f'<mcdata-calling-user-id type="Normal"><mcdataURI>{calling}</mcdataURI></mcdata-calling-user-id>'
            f'</mcdata-Params></mcdatainfo>').encode()


def _mixed(info: bytes, data: bytes, extra_file_headers: str = "", nl: bytes = b"\r\n") -> bytes:
    out = b"--" + BOUNDARY.encode() + nl
    out += b"Content-Type: application/vnd.3gpp.mcdata-info+xml" + nl + nl + info + nl
    out += b"--" + BOUNDARY.encode() + nl
    out += b"Content-Type: application/octet-stream" + nl
    out += (f"Content-Length: {len(data)}").encode() + nl + extra_file_headers.encode() + nl + data + nl
    out += b"--" + BOUNDARY.encode() + b"--" + nl
    return out


class McDataFdTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="csc-fd-")
        self._keep = (fd._FD_DIR, fd._MAX_BYTES, dict(m.GROUPS), m.extract_token, m._MCPTT_PUBLIC_URL, m.SCOPE_ENFORCEMENT,
                      auc._TOKEN)
        fd._FD_DIR, fd._MAX_BYTES = self.tmp, 1024
        m._MCPTT_PUBLIC_URL, m.SCOPE_ENFORCEMENT, auc._TOKEN = PUBLIC, "enforce", "internal-tok"
        m.GROUPS.clear()
        m.GROUPS[GROUP] = {"display_name": "fd", "allow_fd": True,
                           "members": [{"uri": ALICE, "name": "a"}, {"uri": BOB, "name": "b"}]}
        m.GROUPS[GROUP_NOFD] = {"display_name": "nofd", "allow_fd": False, "members": [{"uri": ALICE, "name": "a"}]}
        m.extract_token = lambda hdr: ({"mcdata_id": hdr.split()[-1], "mcptt_id": hdr.split()[-1],
                                        "scope": m.SCOPE_DATA_SERVICE}
                                       if hdr and hdr.split()[-1].startswith("tel:") else None)

    def tearDown(self):
        (fd._FD_DIR, fd._MAX_BYTES, groups, m.extract_token, m._MCPTT_PUBLIC_URL, m.SCOPE_ENFORCEMENT, auc._TOKEN) = self._keep
        m.GROUPS.clear(); m.GROUPS.update(groups)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _call(self, method, path="/mcdata/fd", who=ALICE, body=None, ctype=None, query=None, auth=None):
        headers = {"host": "10.0.0.9:4430"}
        if auth is not None:
            headers["authorization"] = auth
        elif who:
            headers["authorization"] = f"Bearer {who}"
        if ctype:
            headers["content-type"] = ctype
        return asyncio.run(fd.handle_mcdata_fd(
            HandlerArgs(method, path, "127.0.0.1", 0, query_params=query or {}, headers=headers, body=body), {}))

    def _upload_group(self, data=b"hello-fd", who=ALICE, group="sip:g-fd@ptt.example", **kw):
        return self._call("POST", who=who, body=_mixed(_info("group-fd", who, group), data, **kw),
                          ctype=f'multipart/mixed; boundary="{BOUNDARY}"')

    # ── FD-2 · FD-3: 규격형 업로드 → 201 + Location ──
    def test_group_upload_201_location_is_public_base(self):
        r = self._upload_group(extra_file_headers='Content-Disposition: attachment; filename="보고서.pdf"\r\n')
        self.assertEqual(r.status, 201, r.body)
        self.assertEqual(r.headers["Location"], r.body["url"])
        self.assertTrue(r.body["url"].startswith(PUBLIC + "/mcdata/fd/"), "요청 Host 가 아니라 공개 base URL")
        self.assertEqual((r.body["size"], r.body["name"]), (8, "보고서.pdf"))
        g = self._call("GET", path="/mcdata/fd/" + r.body["id"], who=BOB)
        self.assertEqual(g.status, 200)
        with open(g.headers["X-File-Path"], "rb") as f:
            self.assertEqual(f.read(), b"hello-fd")
        self.assertIn("filename*=UTF-8''%EB%B3%B4%EA%B3%A0%EC%84%9C.pdf", g.headers["Content-Disposition"])
        g.headers["Content-Disposition"].encode("latin-1")          # HTTP 헤더로 실을 수 있다

    def test_binary_body_with_newlines_survives(self):
        data = b"\r\n--not-the-boundary\r\n\x00\xff\n\n--" + bytes(range(256))
        for nl in (b"\r\n", b"\n"):
            r = self._call("POST", body=_mixed(_info("one-to-one-fd"), data, nl=nl), ctype=f"multipart/mixed; boundary={BOUNDARY}")
            self.assertEqual(r.status, 201, r.body)
            with open(self._call("GET", path="/mcdata/fd/" + r.body["id"]).headers["X-File-Path"], "rb") as f:
                self.assertEqual(f.read(), data)

    def test_upload_rejections(self):
        mixed = f"multipart/mixed; boundary={BOUNDARY}"
        cases = [
            (self._upload_group(who=CAROL), 403, "그룹 비멤버"),
            (self._upload_group(group="tel:g-nofd"), 403, "FD 를 끈 그룹"),
            (self._upload_group(group="tel:g-none"), 404, "없는 그룹"),
            (self._call("POST", body=_mixed(_info("group-fd", ALICE), b"x"), ctype=mixed), 400, "group-fd 인데 그룹 없음"),
            (self._call("POST", body=_mixed(_info("one-to-one-fd", BOB), b"x"), ctype=mixed), 403, "calling-user-id ≠ 토큰"),
            (self._call("POST", body=_mixed(_info("one-to-one-sds"), b"x"), ctype=mixed), 400, "request-type"),
            (self._upload_group(data=b"x" * 1025), 413, "그룹 FD 상한"),
            (self._call("POST", body=_mixed(_info("one-to-one-fd"), b"x" * 1025), ctype=mixed), 413, "1:1 FD 상한"),
            (self._call("POST", body=b"--" + BOUNDARY.encode() + b"\r\nContent-Type: application/octet-stream\r\n\r\nx\r\n--"
                        + BOUNDARY.encode() + b"--\r\n", ctype=mixed), 400, "mcdata-info 없음"),
            (self._call("POST", body=b"garbage", ctype=mixed), 400, "경계선 없음"),
            (self._call("POST", body=_mixed(b"<!DOCTYPE x><a/>", b"x"), ctype=mixed), 400, "DTD"),
            (self._call("POST", who=None, body=b"x", ctype="application/octet-stream"), 403, "토큰 없음"),
            (self._call("POST", auth="Bearer junk", body=b"x", ctype="application/octet-stream"), 401, "무효 토큰"),
        ]
        for r, status, why in cases:
            self.assertEqual(r.status, status, f"{why}: {r.body}")
        self.assertEqual(os.listdir(self.tmp), [], "거절한 업로드는 저장하지 않는다")

    def test_legacy_query_upload(self):
        r = self._call("POST", body=b"abc", ctype="application/octet-stream", query={"name": "a.txt", "group": "g-fd", "type": "text/plain"})
        self.assertEqual((r.status, r.headers["Location"]), (201, r.body["url"]))
        g = self._call("GET", path="/mcdata/fd/" + r.body["id"], who=BOB)
        self.assertEqual((g.status, g.media_type), (200, "text/plain"))
        self.assertEqual(self._call("POST", who=CAROL, body=b"abc", ctype="application/octet-stream", query={"group": "g-fd"}).status, 403)

    # ── FD-6: 수신 제어 ──
    def test_download_reception_control(self):
        gid = self._upload_group().body["id"]
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + gid, who=CAROL).status, 403, "그룹 밖")
        m.GROUPS[GROUP]["members"] = [{"uri": BOB, "name": "b"}]                       # 올린 사람이 그룹을 떠나도 자기 파일은 받는다
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + gid, who=ALICE).status, 200)
        del m.GROUPS[GROUP]                                               # 그룹이 없어지면 올린 사람만
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + gid, who=BOB).status, 403)
        one = self._call("POST", body=_mixed(_info("one-to-one-fd"), b"x"), ctype=f"multipart/mixed; boundary={BOUNDARY}").body["id"]
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + one, who=CAROL).status, 200, "1:1 파일은 URL 을 받은 MCData 사용자")
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + one, who=None).status, 403)
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + "0" * 32).status, 404)
        self.assertEqual(self._call("GET", path="/mcdata/fd/../etc").status, 400)

    # ── §6.7.3: HEAD 존재 확인 ──
    def test_head_availability(self):
        gid = self._upload_group().body["id"]
        internal = "Bearer internal-tok"
        r = self._call("HEAD", path="/mcdata/fd/" + gid, auth=internal)
        self.assertEqual((r.status, r.headers["X-Cims-Fd-Group"], r.headers["X-Cims-Fd-Uploader"]), (200, GROUP, ALICE))
        self.assertEqual(self._call("HEAD", path="/mcdata/fd/" + "0" * 32, auth=internal).status, 404)
        self.assertEqual(self._call("HEAD", path="/mcdata/fd/" + gid, who=BOB).status, 200)
        self.assertNotIn("X-Cims-Fd-Group", self._call("HEAD", path="/mcdata/fd/" + gid, who=BOB).headers)
        self.assertEqual(self._call("HEAD", path="/mcdata/fd/" + gid, who=CAROL).status, 403)
        self.assertEqual(self._call("HEAD", path="/mcdata/fd/" + gid, auth="Bearer wrong-internal").status, 401)
        self.assertEqual(self._call("GET", path="/mcdata/fd/" + gid, auth=internal).status, 401, "내부 토큰은 HEAD 만")
        auc._TOKEN = ""                                                   # 내부 토큰 미설정이면 빈 Bearer 로 통하지 않는다
        self.assertNotEqual(self._call("HEAD", path="/mcdata/fd/" + gid, auth="Bearer ").status, 200)

    # ── FD-5: 그룹 문서의 FD 상한 = 콘텐츠 서버 상한 ──
    def test_group_document_carries_fd_limit(self):
        m.GROUPS[GROUP].update({"priority": 5, "group_type": "prearranged"})
        xml = m.get_group_xml(GROUP)[0]
        self.assertIn("<mcpttgi:mcdata-on-network-max-data-size-for-FD>1024</mcpttgi:mcdata-on-network-max-data-size-for-FD>", xml)
        m.GROUPS[GROUP_NOFD].update({"priority": 5, "group_type": "prearranged"})
        self.assertNotIn("max-data-size-for-FD", m.get_group_xml(GROUP_NOFD)[0])


if __name__ == "__main__":
    unittest.main()
