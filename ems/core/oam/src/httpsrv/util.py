import csv, tempfile, gzip, io
from typing import AsyncIterator, Iterable, Tuple, Dict
from urllib.parse import urlparse
from pathlib import PurePath
# Phase 4d2: Python 3.10+ 의 builtin anext 우선. asyncstdlib 의존 제거 — csc
# 단독 install 환경 (vendor 에 asyncstdlib 없음) 에서도 동작.
try:
    anext  # type: ignore[name-defined]  # Python 3.10+ builtin
except NameError:
    from asyncstdlib import anext  # legacy fallback

from httpsrv.handler import CSVLike, CSVRow


class HttpException(Exception):
    def __init__(self, message: str, error_code: int):
        self.message = message
        self.error_code = error_code
        super().__init__(f"HttpException: {error_code}, {message}")


class HttpUtil:

    @staticmethod
    def get_full_url(host: str, port: int, sub_url: str) -> str:
        return f"http://{host}:{port}{sub_url}"

    @staticmethod
    def parse_content_type_header(ct: str) -> Tuple[str, str]:
        """returns (media_type, charset)"""
        media_type = "application/octet-stream"
        charset = "utf-8"
        if ct:
            parts = [p.strip() for p in ct.split(";")]
            if parts:
                media_type = parts[0].lower()
            for p in parts[1:]:
                if "=" in p:
                    k, v = p.split("=", 1)
                    if k.strip().lower() == "charset":
                        charset = v.strip().lower()
        return media_type, charset

    @staticmethod
    async def spool_body_to_temp(stream: AsyncIterator[bytes], suffix: str) -> str:
        tmp = tempfile.NamedTemporaryFile(delete=False, suffix=suffix)
        async for chunk in stream:
            tmp.write(chunk)
        tmp.flush()
        tmp.close()
        return tmp.name

    @staticmethod
    def iter_csv_rows_from_path(path: str, encoding: str) -> Iterable[Dict[str, str]]:
        open_fn = open
        if path.endswith(".gz"):
            open_fn = gzip.open
            mode = "rt"
        else:
            mode = "rt"
        with open_fn(path, mode=mode, encoding=encoding, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                yield row

    @staticmethod
    async def stream_to_lines(stream: AsyncIterator[bytes], encoding: str = "utf-8") \
            -> AsyncIterator[str]:
        buffer = b""
        async for chunk in stream:
            buffer += chunk
            lines = buffer.split(b'\n')
            buffer = lines.pop()
            for line in lines:
                yield line.decode(encoding)
        if buffer:
            yield buffer.decode(encoding)

    @staticmethod
    async def iter_csv_rows_from_stream(stream: AsyncIterator[bytes], encoding: str = "utf-8") \
            -> AsyncIterator[CSVRow]:
        line_iterator = HttpUtil.stream_to_lines(stream, encoding)
        try:
            header_line = await anext(line_iterator)
            fieldnames = next(csv.reader([header_line]))
        except StopAsyncIteration as e:
            raise e

        async for row_line in line_iterator:
            if not row_line.strip():
                continue
            values = next(csv.reader([row_line]))
            yield dict(zip(fieldnames, values))

    @staticmethod
    def get_csv_content(csv_like_data: CSVLike) -> str:
        csv_content = ""
        with io.StringIO() as string_buffer:
            csv_headers = csv_like_data[0].keys()
            writer = csv.DictWriter(string_buffer, fieldnames=csv_headers)
            writer.writeheader()
            writer.writerows(csv_like_data)
            csv_content = string_buffer.getvalue()
        return csv_content

    @staticmethod
    def is_match_url(base_path: str, full_url: str) -> bool:
        try:
            full_url_path = urlparse(full_url).path
            full_url_path_obj = PurePath(full_url_path)
            base_path_obj = PurePath(base_path)
            return full_url_path_obj == base_path_obj or base_path_obj in full_url_path_obj.parents
        except Exception as e:
            return False

    @staticmethod
    def get_first_sub_path(base_path: str, full_url: str) -> str:
        try:
            full_url_path = urlparse(full_url).path
            full_url_path_obj = PurePath(full_url_path)
            base_path_obj = PurePath(base_path)
            relative_path = full_url_path_obj.relative_to(base_path_obj)
            if relative_path.parts:
                return relative_path.parts[0]
            else:
                return ''
        except Exception as e:
            return ''


if __name__ == '__main__':
    print(HttpUtil.is_match_url('/api/v1/test', 'http://localhost:8080/api/v1/test?b=1'))
    print(HttpUtil.is_match_url('/api/v1', 'http://localhost:8080/api/v1/test/?a=1'))
    print(HttpUtil.is_match_url('/api', 'http://localhost:8080/api/v1/test/?a=1'))
    print(HttpUtil.is_match_url('/api/v1/tes', 'http://localhost:8080/api/v1/test/?a=1'))

    print('1 '+HttpUtil.get_first_sub_path('/api/v1', 'http://localhost:8080/api/v1/test?b=1'))