"""httpsrv.HttpServer 기동 계약 — **bind 에 성공해야 기동 성공이다**.

uvicorn 은 `EADDRINUSE` 를 잡아 로그만 남기고 `sys.exit(STARTUP_FAILURE)` 로 끝낸다.
준비 완료를 `serve()` 생성 직후에 알리면 **HTTP 없이 살아 있는 프로세스**가 된다 —
agent liveness 는 "process up" 으로 통과하고 readiness 만 계속 실패해서, 스스로 죽지도
남이 재기동하지도 못하는 상태로 굳는다(2026-09-11 oam-svc 실측: 죽은 인스턴스의 고아
소켓이 :4480 을 쥔 채 새 프로세스가 bind 에 실패했는데 "server started" 를 찍었고,
그 노드가 영구 FAILOVER_LATCHED 로 남았다).

base(oam_app)와 oam-svc(oam_svc_app)가 같은 HttpServer 를 쓰므로 여기 한 곳이 둘을 덮는다.
sys.path 는 ems/core/oam/{src,vendor} — test_stats_probe.py 와 동일.
"""
import os
import socket
import sys
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

for _m in [m for m in list(sys.modules)
           if m.split('.')[0] in ('services', 'handlers', 'httpsrv', 'util')]:
    del sys.modules[_m]
sys.path.insert(0, os.path.join(_REPO, "ems", "core", "oam", "src"))
sys.path.insert(1, os.path.join(_REPO, "ems", "core", "oam", "vendor"))

from httpsrv.server import HttpServer  # noqa: E402


def _free_port() -> int:
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


class HttpServerBindContractTest(unittest.TestCase):
    def test_점유된_포트면_start_가_예외를_올린다(self):
        """조용히 돌아오면 호출자가 떠 있는 줄 알고 계속 진행한다."""
        blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        blocker.bind(('127.0.0.1', 0))
        blocker.listen(8)
        port = blocker.getsockname()[1]
        srv = HttpServer('127.0.0.1', port)
        try:
            t0 = time.time()
            with self.assertRaises(RuntimeError) as cm:
                srv.start()
            # 기다리다 지치는 것이 아니라 즉시 실패해야 한다 — start() 는 무한 대기였다.
            self.assertLess(time.time() - t0, 10.0)
            msg = str(cm.exception)
            self.assertIn(str(port), msg)
            self.assertIn('포트 점유', msg, '사유가 메시지에 남아야 한다')
        finally:
            srv.stop(1)
            blocker.close()

    def test_기동_실패_뒤_stop_이_사유를_덮지_않는다(self):
        """아직 만들어지지 않은 것에 손대면 그 AttributeError 가 진짜 사유를 가린다."""
        blocker = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        blocker.bind(('127.0.0.1', 0))
        blocker.listen(8)
        srv = HttpServer('127.0.0.1', blocker.getsockname()[1])
        try:
            with self.assertRaises(RuntimeError):
                srv.start()
            srv.stop(1)          # 예외 없이 끝나야 한다
        finally:
            blocker.close()

    def test_한번도_기동하지_않은_서버의_stop(self):
        HttpServer('127.0.0.1', _free_port()).stop(0.1)

    def test_빈_포트면_정상_기동(self):
        port = _free_port()
        srv = HttpServer('127.0.0.1', port)
        srv.start()
        try:
            self.assertTrue(srv._server.started)
            self.assertIsNone(srv._start_error)
        finally:
            srv.stop(2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
