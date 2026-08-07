import copy
import os

from readerwriterlock import rwlock
from typing import Dict, Tuple, Optional
from fastapi import APIRouter, Request
from starlette.responses import JSONResponse, PlainTextResponse, Response, FileResponse

from httpsrv.util import HttpUtil, HttpException
from httpsrv.handler import BodyData, Server_Dynamic_Handler, HandlerResult, HandlerArgs
from util.log_util import Logger


def _http_response(accept_format: str, result: HandlerResult) -> Response:
    status, body, headers, media = result.status, result.body, result.headers, result.media_type
    if body is None:
        return Response(status_code=status, headers=headers)

    if isinstance(body, dict):
        return JSONResponse(content=body, status_code=status, headers=headers)
    # Phase 4 vendor 정리: numpy/pandas DataFrame body 처리 제거 (사용 안 함).
    if isinstance(body, list):
        if accept_format == "text/csv":
            return Response(content=HttpUtil.get_csv_content(body), status_code=status, headers=headers, media_type=media or "text/csv")
        else:
            if len(body) > 0 and isinstance(body[0], dict):
                keys = list(body[0].keys())
                two_dim_list = [ [ row.get(k) for k in keys ] for row in body ]
            else:
                two_dim_list = body
            return JSONResponse(content=two_dim_list, status_code=status, headers=headers)
    if isinstance(body, (bytes, bytearray, memoryview)):
        return Response(content=body, status_code=status, headers=headers, media_type=media or "application/octet-stream")
    if isinstance(body, str):
        # X-File-Path 헤더가 있으면 파일 스트리밍
        file_path = headers.pop('X-File-Path', None) if headers else None
        if file_path and os.path.isfile(file_path):
            ct = headers.pop('Content-Type', None) if headers else None
            return FileResponse(path=file_path, media_type=ct or media or 'application/octet-stream', headers=headers)
        return PlainTextResponse(content=body, status_code=status, headers=headers, media_type=media or "text/plain")
    return Response(status_code=status, headers=headers)

async def _get_body_from_request(req: Request) -> Optional[BodyData]:
    content_type = req.headers.get("content-type", "")
    # content_encoding = req.headers.get("content-encoding", "").lower()
    media_type, charset = HttpUtil.parse_content_type_header(content_type)
    try:
        if req.method in ("POST", "PUT", "PATCH"):
            if media_type == "application/json":
                body_data = await req.json()
            elif media_type == "application/x-www-form-urlencoded":
                form_data = await req.form()
                body_data = dict(form_data)
            elif media_type == "multipart/form-data":
                # 이진 업로드 지원 — UploadFile 은 bytes 로 읽어서 dict 에 담음
                import time as _t
                _t0 = _t.monotonic()
                form_data = await req.form()
                _t1 = _t.monotonic()
                body_data = {}
                total_bytes = 0
                for k, v in form_data.multi_items():
                    if hasattr(v, "read") and hasattr(v, "filename"):
                        body_data[k] = await v.read()
                        body_data[f"{k}__filename"] = v.filename
                        total_bytes += len(body_data[k])
                    else:
                        body_data[k] = v
                _t2 = _t.monotonic()
                if total_bytes > 1024*1024:   # 1MB 이상만 로그
                    Logger().log_info(
                        f"[multipart] body={total_bytes/1024/1024:.1f}MB "
                        f"form_parse={int((_t1-_t0)*1000)}ms "
                        f"read={int((_t2-_t1)*1000)}ms "
                        f"rate={total_bytes/max(_t1-_t0,1e-6)/1024/1024:.1f}MB/s"
                    )
            elif media_type == "application/octet-stream":
                # 원시 바이너리 — stream 으로 통째 읽음 (multipart 오버헤드 제거)
                import time as _t
                _t0 = _t.monotonic()
                chunks = []
                total = 0
                async for chunk in req.stream():
                    if chunk:
                        chunks.append(chunk)
                        total += len(chunk)
                body_data = b"".join(chunks)
                _t1 = _t.monotonic()
                if total > 1024*1024:
                    Logger().log_info(
                        f"[raw-upload] body={total/1024/1024:.1f}MB "
                        f"read={int((_t1-_t0)*1000)}ms "
                        f"rate={total/max(_t1-_t0,1e-6)/1024/1024:.1f}MB/s"
                    )
            elif media_type == "text/csv":
                body_data = []
                async for csv_row in HttpUtil.iter_csv_rows_from_stream(req.stream(), encoding=charset):
                    body_data.append(csv_row)
                # suffix = ".csv.gz" if "gzip" in content_encoding else ".csv"
                # path = await HttpUtil.spool_body_to_temp(req.stream(), suffix=suffix)
                # body_data = HttpUtil.iter_csv_rows_from_path(path, encoding=charset)
            else:
                raise HttpException(f"Unsupported media type: {media_type}", 415)
            return body_data
        else:
            return None
    except Exception as e:
        raise HttpException(f"exception : {e}", 500)

async def _encode_reqHandler_args(request: Request) -> HandlerArgs:
    body_data = await _get_body_from_request(request)
    return HandlerArgs(request.method,
                       request.path_params['full_path'],
                       request.client.host,
                       request.client.port,
                       dict(request.query_params),
                       dict(request.headers),
                       request.cookies,
                       body_data)


class HealthRouteProc:

    def __init__(self):
        pass

    async def health(self):
        return {"status": "ok"}


class DynamicRouteProc:

    # 전역 pre/post 훅 (호출자가 set_request_hooks 로 등록).
    # pre(handler_args, base_path) → None
    # post(handler_args, base_path, handler_result) → None
    _pre_hook = None
    _post_hook = None

    @classmethod
    def set_request_hooks(cls, pre=None, post=None):
        """앱 초기화 시 호출. pre 는 handler 실행 직전, post 는 직후 실행."""
        cls._pre_hook = pre
        cls._post_hook = post

    def __init__(self):
        self._logger = Logger()
        self._routes: Dict[str, Tuple[Server_Dynamic_Handler, dict]] = {}
        self._routeRWLock = rwlock.RWLockFairD()

    def add_route(self, path: str, handler: Server_Dynamic_Handler, kwargs: dict = None):
        with self._routeRWLock.gen_wlock():
            self._routes[path] = (handler, kwargs)

    def remove_route(self, path: str):
        with self._routeRWLock.gen_wlock():
            del self._routes[path]

    async def route(self, req: Request, full_path: str) -> Response:
        # encode args
        try:
            handler_args = await _encode_reqHandler_args(req)
        except HttpException as e:
            Logger().log_error(f"HttpServer : route({full_path}) : fail : HttpException : {e}")
            return PlainTextResponse(status_code=e.error_code, content=e.message)

        # find handler
        with self._routeRWLock.gen_rlock():
            match_base_path = ""
            for base_path in self._routes.keys():
                if HttpUtil.is_match_url(base_path, full_path) and len(base_path) > len(match_base_path):
                    match_base_path = base_path
            if len(match_base_path) == 0:
                self._logger.log_error(f"HttpServer : route({full_path}) : fail : No route")
                return PlainTextResponse(status_code=404, content=f"No route for '{full_path}'")
            handler_info = self._routes.get(match_base_path)
            if handler_info is None:
                self._logger.log_error(f"HttpServer : route({full_path}) : fail : No handler")
                return PlainTextResponse(status_code=500, content=f"No handler for '{full_path}'")
            handler, kwargs = handler_info
            if kwargs is None:
                kwargs = {}
            else:
                kwargs = copy.deepcopy(kwargs)

        # execute handler (pre/post 훅 실행 — 예외 발생해도 핸들러 진행 방해하지 않음)
        try:
            if DynamicRouteProc._pre_hook:
                try: DynamicRouteProc._pre_hook(handler_args, match_base_path)
                except Exception as eh: self._logger.log_error(f"pre_hook error: {eh}")
            handler_result = await handler(handler_args, kwargs)
            self._logger.log_verbose(f"HttpServer : route({full_path}) : rsp={handler_result.status}")
            if DynamicRouteProc._post_hook:
                try: DynamicRouteProc._post_hook(handler_args, match_base_path, handler_result)
                except Exception as eh: self._logger.log_error(f"post_hook error: {eh}")
            accept_format = req.headers.get("accept", "")
            res = _http_response(accept_format, handler_result)
            return res
        except Exception as e:
            # 관리 store 소유권 상실(LeaseLostError)은 서버 오류가 아니라 **거부**다 —
            # 409 로 매핑해 콘솔이 read-only 상태를 그대로 표시할 수 있게 한다
            # (500 이면 "OAM 이 고장" 으로 오해된다). 상세: oam_ha.md §4.4
            if type(e).__name__ == 'LeaseLostError':
                self._logger.log_warning(f"HttpServer : route({full_path}) : 409 not_lease_owner : {e}")
                return JSONResponse(status_code=409,
                                    content={"error": "not_lease_owner", "detail": str(e)})
            import traceback
            tb_str = traceback.format_exc()
            self._logger.log_error(f"HttpServer : route({full_path}) : fail : exception : {e} : {tb_str}")
            return PlainTextResponse(status_code=500, content=f"exception : {e}")


class HttpServerController:
    SUB_URL_HEALTH = "/health"
    SUB_URL_DYNAMIC = "{full_path:path}"

    def __init__(self, router: APIRouter):
        self._logger = Logger()
        self._dynamic_controller = DynamicRouteProc()
        self._health_controller = HealthRouteProc()
        self._controller_mapping = {
            HttpServerController.SUB_URL_HEALTH:    (self._health_controller.health, ["GET"]),
            HttpServerController.SUB_URL_DYNAMIC:   (self._dynamic_controller.route, ["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS", "HEAD"])
        }
        self._controller_register(router)

    def _controller_register(self, router: APIRouter):
        for url, (controller, method_list) in self._controller_mapping.items():
            router.add_api_route(url, controller, methods=method_list)

    def add_dynamic_route(self, path: str, handler: Server_Dynamic_Handler, kwargs: dict = None):
        self._dynamic_controller.add_route(path, handler, kwargs)

    def del_dynamic_route(self, path: str):
        self._dynamic_controller.remove_route(path)
