import asyncio, os as _os, ssl as _ssl, uvicorn, threading
from concurrent.futures import ThreadPoolExecutor
from fastapi import FastAPI, APIRouter
from fastapi.middleware.cors import CORSMiddleware
from typing import Optional, List, Tuple

# asyncio.to_thread 가 쓰는 기본 executor. 핸들러가 블로킹 I/O(NFS·DB·UDP probe)를 스레드로
# 오프로드하므로 파이썬 기본값 min(32, cpu+4) 로는 동시 요청 버스트에 고갈돼 head-of-line
# 블로킹이 되돌아온다. CMP 노드 probe 는 stats._PROBE_POOL(별도) 이라 서로 굶기지 않는다.
_IO_EXECUTOR_WORKERS = 48

# TLS 인증서 파일 변경 감시 주기(초). 회전은 드문 사건이라 짧을 이유가 없다.
_CERT_WATCH_SEC = int(_os.environ.get('CIMS_CERT_WATCH_SEC') or 30)

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
        self._io_executor: Optional[ThreadPoolExecutor] = None
        self._lag_task = None                 # 이벤트 루프 지연 감시
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
        self._io_executor = ThreadPoolExecutor(
            max_workers=_IO_EXECUTOR_WORKERS, thread_name_prefix='oam-io')
        asyncio.get_running_loop().set_default_executor(self._io_executor)
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
        self._lag_task = asyncio.create_task(self._watch_loop_lag())
        self._ready_event.set()
        try:
            await self._server_task
        finally:
            await self._cleanup_async()

    # ── 이벤트 루프 지연 감시 ───────────────────────────────────────────
    #
    # `/health` 는 `return {"status":"ok"}` 한 줄이라 **느려질 수 있는 이유가 하나뿐**이다:
    # 이벤트 루프가 막혀 코루틴이 제때 못 깨는 것. 그런데 그 사실이 지금까지 아무 데도
    # 남지 않았다 — agent 는 2초 안에 응답이 없으면 그 모듈을 죽은 것으로 판정하고
    # **한 번 만에 절체·영구 래치**까지 가는데(ha_service_model.md §8), 서버 쪽 기록은
    # "늦게라도 200 을 줬다"뿐이라 사후에 원인을 댈 수 없었다(실측 2026-09-10).
    #
    # 그래서 루프 지연을 직접 잰다. 판정 임계(2초)를 넘긴 구간은 **경고 + 그 순간의 전체
    # 스레드 스택**을 남긴다 — 무엇이 루프를 잡고 있었는지가 로그에 그대로 찍힌다
    # (py-spy 를 들고 현장에 있을 필요가 없다). 덤프는 쿨다운을 둬 폭주시키지 않는다.
    _LAG_WARN_SEC = 2.0            # agent readiness 판정 타임아웃과 같은 눈금
    _LAG_DEBUG_SEC = 0.2           # 이 이상은 debug 로만 (평시 소음 방지)
    _LAG_DUMP_COOLDOWN_SEC = 60

    async def _watch_loop_lag(self):
        """1초 주기 코루틴의 실제 깨어난 시각으로 루프 지연을 잰다."""
        import faulthandler
        import sys as _sys
        import time as _time
        last_dump = 0.0
        while True:
            t0 = _time.monotonic()
            try:
                await asyncio.sleep(1.0)
            except asyncio.CancelledError:
                return
            lag = _time.monotonic() - t0 - 1.0
            if lag >= self._LAG_WARN_SEC:
                self._logger.log_warning(
                    f"[loop] 이벤트 루프 지연 {lag:.1f}s — 이 시간만큼 모든 응답이 밀린다"
                    f" (health 판정 임계 {self._LAG_WARN_SEC:.0f}s)")
                now = _time.monotonic()
                if now - last_dump >= self._LAG_DUMP_COOLDOWN_SEC:
                    last_dump = now
                    try:
                        print(f"--- loop lag {lag:.1f}s: thread dump ---", file=_sys.stderr, flush=True)
                        faulthandler.dump_traceback(file=_sys.stderr)
                        _sys.stderr.flush()
                    except Exception:
                        pass
            elif lag >= self._LAG_DEBUG_SEC:
                self._logger.log_verbose(f"[loop] 지연 {lag * 1000:.0f}ms")

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
        if self._lag_task is not None:
            self._lag_task.cancel()
        try:
            await self._server.shutdown()
        except Exception:
            pass
        try:
            self._io_executor.shutdown(wait=False)
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
