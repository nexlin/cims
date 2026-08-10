import asyncio, os as _os, ssl as _ssl, uvicorn, threading
from fastapi import FastAPI, APIRouter
from fastapi.middleware.cors import CORSMiddleware
from typing import Optional, List, Tuple

from httpsrv.controller import HttpServerController
from httpsrv.handler import Server_Dynamic_Handler
from util.log_util import Logger

# TLS 인증서 파일 변경 감시 주기(초). 회전은 드문 사건이라 짧을 이유가 없다.
_CERT_WATCH_SEC = int(_os.environ.get('CIMS_CERT_WATCH_SEC') or 30)


class HttpServer:
    def __init__(self, host: str = "0.0.0.0", port: int = 8080, ssl_keyfile: str = None, ssl_certfile: str = None):
        self._logger = Logger()
        self._host = host
        self._port = port
        self._ssl_keyfile = ssl_keyfile
        self._ssl_certfile = ssl_certfile
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._ready_event = threading.Event()
        self._shutdown_event = threading.Event()
        self._thread = threading.Thread(target=self._start_event_loop, daemon=True)
        self._app = self._create_app()
        router = APIRouter()
        self._controller = HttpServerController(router)
        self._app.include_router(router)

    @staticmethod
    def _create_app() -> FastAPI:
        app = FastAPI(title="Dynamic URL Server")
        # Allow CORS for local frontend dev server
        app.add_middleware(
            CORSMiddleware,
            allow_origins=["*"],
            allow_credentials=True,
            allow_methods=["*"],
            allow_headers=["*"],
        )
        return app

    def _start_event_loop(self):
        self._logger.log_info(f"start http server on {self._host}:{self._port}")
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._bootstrap())
        self._shutdown_event.set()

    async def _bootstrap(self):
        config = uvicorn.Config(
            app=self._app, host=self._host, port=self._port, log_level="info", loop="asyncio",
            ssl_keyfile=self._ssl_keyfile, ssl_certfile=self._ssl_certfile
        )
        # ssl 컨텍스트를 여기서 확정해 두고(watch 대상) Server 에 넘긴다.
        # Server.serve() 는 config.loaded 를 보고 중복 load 하지 않는다.
        try:
            config.load()
            self._start_cert_watch(getattr(config, 'ssl', None))
        except Exception as e:
            self._logger.log_warning(f"TLS 인증서 감시 미기동({e}) — 회전 시 재기동 필요")
        self._server = uvicorn.Server(config)
        self._server_task = asyncio.create_task(self._server.serve())
        self._ready_event.set()
        try:
            await self._server_task
        finally:
            await self._cleanup_async()

    # ── TLS 인증서 핫리로드 ──────────────────────────────────────────────
    # 인증서는 lifecycle 엔진이 **모듈 기동 전**에 발급·재발급한다(oam_ha.md §5.2).
    # 그런데 SAN 이 바뀌는 사건(VIP 부여·접속 주소 변경)은 모듈이 이미 떠 있을 때도
    # 일어난다 — 그때 엔진이 파일을 갈아끼워도 프로세스가 기동 시 1회 읽은 컨텍스트를
    # 그대로 쓰면 **옛 인증서를 계속 서빙**한다(같은 노드에서 모듈마다 인증서가 갈린다).
    # uvicorn 의 SSLContext 를 잡아 두고 파일이 바뀌면 load_cert_chain 으로 교체한다 —
    # 이후 handshake 부터 새 인증서가 쓰이고 기존 연결은 영향받지 않는다.
    def _cert_sig(self):
        try:
            c = _os.stat(self._ssl_certfile)
            k = _os.stat(self._ssl_keyfile)
            return (c.st_mtime_ns, c.st_size, k.st_mtime_ns, k.st_size)
        except OSError:
            return None

    def _start_cert_watch(self, ssl_ctx):
        if ssl_ctx is None or not (self._ssl_certfile and self._ssl_keyfile):
            return
        state = {'sig': self._cert_sig()}

        def _loop():
            while not self._shutdown_event.wait(_CERT_WATCH_SEC):
                cur = self._cert_sig()
                if cur is None or cur == state['sig']:
                    continue
                try:
                    # **버리는 컨텍스트에 먼저 적재해 검증한다.** load_cert_chain 은 키가
                    # 인증서와 맞지 않으면 예외를 던지지만 그 전에 인증서를 이미 갈아
                    # 끼운다 — 살아있는 컨텍스트에 바로 적용하면 예외를 잡아도 컨텍스트가
                    # 망가져 **그 뒤 모든 handshake 가 실패**한다(실측). 키/인증서가 따로
                    # 기록되는 찰나에 반드시 걸리는 창이다.
                    _probe = _ssl.SSLContext(_ssl.PROTOCOL_TLS_SERVER)
                    _probe.load_cert_chain(self._ssl_certfile, self._ssl_keyfile)
                    ssl_ctx.load_cert_chain(self._ssl_certfile, self._ssl_keyfile)
                except Exception as e:
                    # sig 를 갱신하지 않고 다음 주기에 재시도 — 기존 인증서 유지.
                    self._logger.log_warning(f"TLS 인증서 재적용 보류({e}) — 다음 주기 재시도")
                    continue
                state['sig'] = cur
                self._logger.log_info(
                    f"TLS 인증서 교체 적용 — {self._ssl_certfile} (이후 handshake 부터)")

        threading.Thread(target=_loop, daemon=True, name='cert-watch').start()

    async def _cleanup_async(self):
        try:
            await self._server.shutdown()
        except Exception:
            pass

    def start(self):
        self._thread.start()
        self._ready_event.wait()

    def stop(self, timeout: float = 5.0):
        # stop uvicorn-server
        self._server.should_exit = True

        # stop event loop
        self._shutdown_event.wait(timeout)
        self._loop.call_soon_threadsafe(self._loop.stop)

        # stop thread
        self._thread.join(timeout)

        self._logger.log_info(f"stop http server on {self._host}:{self._port}")

    def add_dynamic_rules(self, rules: List[Tuple[str, Server_Dynamic_Handler, dict]]):
        for rule in rules:
            self.add_dynamic_rule(rule[0], rule[1], rule[2])

    def add_dynamic_rule(self, path: str, handler: Server_Dynamic_Handler, kwargs: dict = None):
        self._controller.add_dynamic_route(path, handler, kwargs)
        self._logger.log_info(f'http server add dynamic rule: {path}')

    def del_dynamic_rules(self, paths: List[str]):
        for path in paths:
            self.del_dynamic_rule(path)

    def del_dynamic_rule(self, path: str):
        self._controller.del_dynamic_route(path)
        self._logger.log_info(f'http server del dynamic rule: {path}')
