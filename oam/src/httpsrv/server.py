import asyncio, uvicorn, threading
from fastapi import FastAPI, APIRouter
from fastapi.middleware.cors import CORSMiddleware
from typing import Optional, List, Tuple

from httpsrv.controller import HttpServerController
from httpsrv.handler import Server_Dynamic_Handler
from util.log_util import Logger


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
        self._server = uvicorn.Server(config)
        self._server_task = asyncio.create_task(self._server.serve())
        self._ready_event.set()
        try:
            await self._server_task
        finally:
            await self._cleanup_async()

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
