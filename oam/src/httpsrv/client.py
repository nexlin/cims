import threading, asyncio, httpx, concurrent.futures
import weakref
from typing import Tuple, Optional

from util.log_util import Logger


class HttpClient:

    def __init__(self):
        self._logger = Logger()

        self._http_client_sync = httpx.Client()     # not thread-safe
        self._http_client_async = httpx.AsyncClient()
        self._sync_lock = threading.Lock()

        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._loop_ready = threading.Event()
        self._thread = threading.Thread(target=self._start_event_loop, daemon=True)
        self._thread.start()
        self._loop_ready.wait()

        self._stopped = False
        self._finalizer = weakref.finalize(self, self._finalize)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            self.stop()
        except Exception:
            pass

    def _finalize(self):
        try:
            self.stop()
        except Exception:
            pass

    def _start_event_loop(self):
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._loop_ready.set()
        try:
            self._loop.run_forever()
        finally:
            try:
                self._loop.run_until_complete(self._loop.shutdown_asyncgens())
            except RuntimeError:
                pass
            except Exception as exc:
                pass
            self._loop.close()

    @staticmethod
    def _getRsp4Exception(exc: httpx.RequestError) -> int:
        response_code = 500  # "Internal Server Error"
        if isinstance(exc, httpx.ConnectTimeout):
            response_code = 504  # "Gateway Timeout"
        elif isinstance(exc, httpx.ReadTimeout):
            response_code = 504  # "Gateway Timeout"
        elif isinstance(exc, httpx.NetworkError):
            response_code = 503  # "Service Unavailable"
        elif isinstance(exc, httpx.ConnectError):
            response_code = 502  # "Bad Gateway"
        return response_code

    async def _post_async(self, url: str, data: dict, timeout: int=5) -> Tuple[int, str]:
        try:
            response = await self._http_client_async.post(url, json=data, timeout=timeout)
            response_code = response.status_code
            body_str = response.text
            # self._logger.log_verbose(f"http-post(async): {url} : {response_code}")
        except httpx.RequestError as exc:
            response_code = self._getRsp4Exception(exc)
            body_str = ''
            self._logger.log_error(f"http-post(async): {url} : exception {exc} : {response_code}")
        except Exception as exc:
            response_code = 500
            body_str = ''
            self._logger.log_error(f"http-post(async): {url} : exception {exc} : {response_code}")
        return response_code, body_str

    async def _get_async(self, url: str, timeout: int=5, headers: dict=None) -> Tuple[int, str, bytes]:
        try:
            response = await self._http_client_async.get(url, headers=headers, timeout=timeout)
            response_code = response.status_code
            content_type = response.headers.get('content-type')
            body_data = response.content
            # self._logger.log_verbose(f"http-get(async): {url} : {response_code}")
        except httpx.RequestError as exc:
            response_code = self._getRsp4Exception(exc)
            content_type = ''
            body_data = ''
            self._logger.log_error(f"http-get(async): {url} : exception {exc} : {response_code}")
        except Exception as exc:
            response_code = 500
            content_type = ''
            body_data = ''
            self._logger.log_error(f"http-get(async): {url} : exception {exc} : {response_code}")
        return response_code, content_type, body_data

    # ----- async API (비동기 result) -----
    def post_async(self, url: str, data: dict, timeout: int=5, result_loop: Optional[asyncio.AbstractEventLoop]=None) -> asyncio.Future:
        future = asyncio.run_coroutine_threadsafe(self._post_async(url, data, timeout), self._loop)
        if result_loop is None:
            result_loop = asyncio.get_running_loop()
        return asyncio.wrap_future(future, loop=result_loop)

    def get_async(self, url: str, timeout: int=5, result_loop: Optional[asyncio.AbstractEventLoop]=None, headers: dict=None) -> asyncio.Future:
        future = asyncio.run_coroutine_threadsafe(self._get_async(url, timeout, headers), self._loop)
        if not result_loop:
            result_loop = asyncio.get_running_loop()
        return asyncio.wrap_future(future, loop=result_loop)

    # ----- async API (동기 result) -----
    def post_async_sync_result(self, url: str, data: dict, timeout: int=5) -> concurrent.futures.Future:
        future = asyncio.run_coroutine_threadsafe(self._post_async(url, data, timeout), self._loop)
        return future

    def get_async_sync_result(self, url: str, timeout: int=5, headers: dict=None) -> concurrent.futures.Future:
        future = asyncio.run_coroutine_threadsafe(self._get_async(url, timeout, headers), self._loop)
        return future

    # ----- sync API -----
    def post_sync(self, url: str, data: dict, timeout: int=5) -> Tuple[int, str]:
        try:
            with self._sync_lock:
                response = self._http_client_sync.post(url, json=data, timeout=timeout)
            return response.status_code, response.text
        except httpx.RequestError as exc:
            response_code = self._getRsp4Exception(exc)
            self._logger.log_error(f"http-post(sync): {url}-{data} : exception {exc} : {response_code}")
            return response_code, ''
        except Exception as exc:
            self._logger.log_error(f"http-post(sync): {url}-{data} : exception {exc} : {500}")
            return 500, ''

    def get_sync(self, url: str, timeout: int=5) -> Tuple[int, str, str]:
        try:
            with self._sync_lock:
                response = self._http_client_sync.get(url, timeout=timeout)
            return response.status_code, response.headers.get('content-type'), response.content.decode('utf-8')
        except httpx.RequestError as exc:
            response_code = self._getRsp4Exception(exc)
            self._logger.log_error(f"http-get(sync): {url} : exception {exc} : {response_code}")
            return response_code, '', ''
        except Exception as exc:
            self._logger.log_error(f"http-get(sync): {url} : exception {exc} : {500}")
            return 500, '', ''

    def stop(self):
        if self._stopped:
            return
        self._stopped = True

        async def async_stop():
            try:
                await self._http_client_async.aclose()
            except Exception as e:
                self._logger.log_error(f"async_client close error: {e}")

        try:
            if self._loop and self._loop.is_running():
                fut = asyncio.run_coroutine_threadsafe(async_stop(), self._loop)
                try:
                    fut.result(timeout=5)
                except Exception as exc:
                    self._logger.log_error(f"async_stop result error: {exc}")

                try:
                    self._loop.call_soon_threadsafe(self._loop.stop)
                except Exception:
                    pass

                if self._thread and self._thread.is_alive():
                    self._thread.join(timeout=5)
            else:
                pass
        finally:
            try:
                self._http_client_sync.close()
            except Exception as exc:
                self._logger.log_error(f"sync_client close error: {exc}")
