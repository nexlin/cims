"""게이트웨이 SSE 통과 단위시험 (handlers.gateway — test_instrument.md §6.2 base 확장 ①).

Covers:
  - _wants_event_stream: Accept 헤더 대소문자 무관 판정
  - _is_event_stream: 업스트림 Content-Type 판정(charset 꼬리 포함)
  - _stream_passthrough: 청크가 그대로 흐르고, 소비 종료 시 업스트림 응답을 release/close
  - register_module_routes: requires_base_oam 이 라우트 레코드에 기록된다
"""
import asyncio
import os
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
for _m in [m for m in list(sys.modules) if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, 'ems', 'core', 'oam', 'src'))
sys.path.insert(1, os.path.join(_REPO, 'ems', 'core', 'oam', 'vendor'))

from handlers import gateway as G  # noqa: E402


class _FakeContent:
    def __init__(self, chunks): self._chunks = list(chunks)
    async def iter_any(self):
        for c in self._chunks:
            yield c


class _FakeResp:
    def __init__(self, chunks):
        self.content = _FakeContent(chunks)
        self.released = False
        self.closed = False
    def release(self): self.released = True
    def close(self): self.closed = True


class Detection(unittest.TestCase):
    def test_wants(self):
        self.assertTrue(G._wants_event_stream({'Accept': 'text/event-stream'}))
        self.assertTrue(G._wants_event_stream({'accept': 'TEXT/EVENT-STREAM, */*'}))
        self.assertFalse(G._wants_event_stream({'Accept': 'application/json'}))
        self.assertFalse(G._wants_event_stream({}))

    def test_is(self):
        self.assertTrue(G._is_event_stream('text/event-stream; charset=utf-8'))
        self.assertFalse(G._is_event_stream('application/json'))
        self.assertFalse(G._is_event_stream(''))


class Passthrough(unittest.TestCase):
    def test_chunks_and_release(self):
        resp = _FakeResp([b': connected\n\n', b'', b'data: {"a":1}\n\n'])
        r = G._stream_passthrough(resp, 200, {'etag': 'x'})
        self.assertIsNotNone(r.response)
        self.assertEqual(r.response.media_type, 'text/event-stream')
        self.assertEqual(r.response.headers.get('cache-control'), 'no-cache')

        async def drain():
            out = []
            async for c in r.response.body_iterator:
                out.append(c)
            return out
        out = asyncio.run(drain())
        self.assertEqual(out, [b': connected\n\n', b'data: {"a":1}\n\n'])   # 빈 청크는 건너뜀
        self.assertTrue(resp.released and resp.closed)


class RequiresBaseOam(unittest.TestCase):
    def test_recorded_on_route(self):
        tmp = tempfile.mkdtemp(prefix='gw-ut-')
        cfg = {'CimsRuntimeDir': tmp}
        from services import file_store, lease
        lease.acquire(file_store.runtime_root(cfg))   # 관리 store 는 단일 writer(리스 펜싱)
        n = G.register_module_routes(cfg, 'oam-cims-tester', '127.0.0.1', 4490, ['/api/v1/tester'],
                                     requires_base_oam='0.2.120')
        self.assertEqual(n, 1)
        rec = next(r for r in G.load_routes(cfg) if r['segment'] == '/api/v1/tester')
        self.assertEqual(rec['module'], 'oam-cims-tester')
        self.assertEqual(rec['upstream'], 'https://127.0.0.1:4490')
        self.assertEqual(rec['requires_base_oam'], '0.2.120')

    def test_ver_tuple(self):
        self.assertLess(G._ver_tuple('0.2.119'), G._ver_tuple('0.2.120'))
        self.assertLess(G._ver_tuple('0.9.9'), G._ver_tuple('0.10.0'))


if __name__ == '__main__':
    unittest.main()
