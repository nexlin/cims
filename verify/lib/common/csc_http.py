"""TB-CSC / 배포본 csc API 호출 helper.

S5 native step 들이 사용하는 최소 HTTP client.
- urllib 기반 (별도 의존 X), TLS verify skip (self-signed cert).
- JSON / multipart upload 지원.
- 응답이 비-JSON 이거나 status>=400 인 경우 명시적으로 예외 발생.
"""
from __future__ import annotations

import json
import os
import ssl
import urllib.request
import urllib.error
import urllib.parse
import uuid
from typing import Any, Optional


class CscHttpError(Exception):
    """CSC API 호출 실패 (HTTP status>=400 또는 응답 파싱 실패)."""
    def __init__(self, msg: str, *, status: int = 0, body: str = "") -> None:
        super().__init__(msg)
        self.status = status
        self.body = body


_INSECURE_CTX = ssl.create_default_context()
_INSECURE_CTX.check_hostname = False
_INSECURE_CTX.verify_mode = ssl.CERT_NONE


def _request(method: str, url: str, *, headers: Optional[dict] = None,
             data: Optional[bytes] = None, timeout: int = 10) -> tuple:
    """단일 HTTP 호출. (status:int, body:str) 반환. 네트워크 오류 시 예외."""
    req = urllib.request.Request(url, data=data, method=method)
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, context=_INSECURE_CTX, timeout=timeout) as r:
            return (int(r.status), r.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        body = ""
        try:
            body = e.read().decode("utf-8", errors="replace")
        except Exception:
            pass
        return (int(e.code), body)


def get_json(url: str, token: Optional[str] = None, timeout: int = 10) -> Any:
    """GET → JSON. 4xx/5xx 면 CscHttpError."""
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    status, body = _request("GET", url, headers=headers, timeout=timeout)
    if status >= 400:
        raise CscHttpError(f"GET {url} → {status}", status=status, body=body[:400])
    try:
        return json.loads(body) if body else None
    except json.JSONDecodeError as e:
        raise CscHttpError(f"GET {url} non-JSON: {e}", status=status, body=body[:400])


def post_json(url: str, payload: dict, token: Optional[str] = None,
              timeout: int = 15) -> tuple:
    """POST JSON. (status, parsed_json_or_text) 반환. 4xx/5xx 도 그대로 반환
    (호출자가 409 등 분기 처리 가능). 5xx network error 만 예외.
    """
    headers = {"Content-Type": "application/json", "Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = json.dumps(payload).encode("utf-8")
    status, body = _request("POST", url, headers=headers, data=data, timeout=timeout)
    parsed: Any = body
    if body:
        try:
            parsed = json.loads(body)
        except json.JSONDecodeError:
            pass
    return (status, parsed)


def delete(url: str, token: Optional[str] = None, timeout: int = 10) -> int:
    """DELETE. status code 반환. 4xx/5xx 도 그대로 반환."""
    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    status, _ = _request("DELETE", url, headers=headers, timeout=timeout)
    return status


def post_multipart(url: str, *, file_path: str, file_field: str = "file",
                   filename: Optional[str] = None,
                   form_fields: Optional[dict] = None,
                   token: Optional[str] = None, timeout: int = 60) -> tuple:
    """multipart/form-data POST (파일 업로드). (status, parsed_json_or_text) 반환."""
    boundary = uuid.uuid4().hex
    fname = filename or os.path.basename(file_path)
    parts: list = []
    for k, v in (form_fields or {}).items():
        parts.append(f"--{boundary}\r\n".encode())
        parts.append(f'Content-Disposition: form-data; name="{k}"\r\n\r\n'.encode())
        parts.append(str(v).encode("utf-8"))
        parts.append(b"\r\n")
    parts.append(f"--{boundary}\r\n".encode())
    parts.append(
        f'Content-Disposition: form-data; name="{file_field}"; filename="{fname}"\r\n'.encode()
    )
    parts.append(b"Content-Type: application/octet-stream\r\n\r\n")
    with open(file_path, "rb") as f:
        parts.append(f.read())
    parts.append(b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)

    headers = {"Content-Type": f"multipart/form-data; boundary={boundary}"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    status, raw = _request("POST", url, headers=headers, data=body, timeout=timeout)
    parsed: Any = raw
    if raw:
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            pass
    return (status, parsed)


# ── 도메인 헬퍼 ──────────────────────────────────────────────────
def admin_login(base: str, login_id: str, password: str, timeout: int = 5) -> str:
    """POST /api/v1/auth/login → JWT token. 실패 시 빈 문자열."""
    try:
        status, body = post_json(
            f"{base}/api/v1/auth/login",
            {"login_id": login_id, "password": password},
            timeout=timeout,
        )
    except Exception:
        return ""
    if status != 200:
        return ""
    if isinstance(body, dict):
        return str(body.get("token") or "")
    return ""


def list_agents(base: str, token: str, timeout: int = 5) -> list:
    """GET /api/v1/agents → list. 형식 (list / dict.items / dict.agents) 흡수."""
    try:
        d = get_json(f"{base}/api/v1/agents", token=token, timeout=timeout)
    except CscHttpError:
        return []
    if isinstance(d, list):
        return d
    if isinstance(d, dict):
        return list(d.get("items") or d.get("agents") or [])
    return []


def find_agent_id_by_name(base: str, token: str, name: str) -> Optional[int]:
    """이름으로 agent id 검색."""
    for r in list_agents(base, token):
        if isinstance(r, dict) and r.get("name") == name:
            v = r.get("id")
            if v is not None:
                try:
                    return int(v)
                except (TypeError, ValueError):
                    return None
    return None
