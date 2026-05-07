"""SIP msg.jsonl / flow.jsonl 파싱 helper.

dist 빌드의 service log 는 `<dist>/ext_mnt/service_log/YYYY/MM/DD/HH/`
아래 두 종류 jsonl 파일을 둔다.

- `*_sip.msg.jsonl`: 원본 SIP 메시지 단위 라인.
  형식: `{ts,dir,peer,caller,callee,sesid,proto,msg}` — `msg` 가 raw SIP
  텍스트(헤더 + 본문, CRLF 구분).
- `*.flow.jsonl`: method 단위 흐름 라인.
  형식: `{ts,service,sesid,node,from,to,proto,method,mid,seq,iface}` —
  본문 없음.

옛 빌드(`<repo>/ext_mnt/msg_log/csp/sip/**/sip.jsonl`)도 라인 구조가 동일
하므로 fallback 으로 함께 검색한다.
"""
from __future__ import annotations

import json
import os
from glob import glob
from typing import Iterator, Optional


def _msg_roots(dist_dir: str) -> list:
    primary = os.path.join(dist_dir, "ext_mnt", "service_log")
    legacy = os.path.join(os.path.dirname(dist_dir), "ext_mnt",
                          "msg_log", "csp", "sip")
    return [p for p in (primary, legacy) if os.path.isdir(p)]


def iter_sip_msgs(dist_dir: str, *, since: float = 0.0,
                  method: Optional[str] = None) -> Iterator[dict]:
    """SIP msg.jsonl 라인 iter.

    - `since`: 파일 mtime 이 since 미만이면 skip.
    - `method`: 지정 시 raw msg 의 첫 토큰(REGISTER/NOTIFY/INVITE/...) 매칭.
      응답(SIP/2.0 ...) 라인은 매칭 X — request 만.
    """
    method_u = method.upper() if method else None
    method_prefix = (method_u + " ") if method_u else None
    method_b = method_prefix.encode() if method_prefix else None
    for root in _msg_roots(dist_dir):
        for p in glob(os.path.join(root, "**", "*sip*.jsonl"), recursive=True):
            try:
                if os.path.getmtime(p) < since:
                    continue
            except OSError:
                continue
            try:
                with open(p, "rb") as f:
                    for raw in f:
                        if method_b and method_b not in raw:
                            continue
                        try:
                            d = json.loads(raw.decode("utf-8", errors="replace"))
                        except Exception:
                            continue
                        msg = d.get("msg", "") or ""
                        if method_prefix and not msg.startswith(method_prefix):
                            continue
                        yield d
            except OSError:
                continue


def iter_flow_lines(dist_dir: str, *, node: Optional[str] = None,
                    proto: Optional[str] = None,
                    since: float = 0.0) -> Iterator[dict]:
    """flow.jsonl 라인 iter.

    - `node`: cmp / csp / csc — 지정 시 파일명 prefix 로 1차 필터.
    - `proto`: 라인의 `proto` 필드 매칭 (MCPTT/SIP/JSON ...).
    - `since`: 파일 mtime 이 since 미만이면 skip.
    """
    primary = os.path.join(dist_dir, "ext_mnt", "service_log")
    if not os.path.isdir(primary):
        return
    file_pat = f"{node}_*.flow.jsonl" if node else "*.flow.jsonl"
    for p in glob(os.path.join(primary, "**", file_pat), recursive=True):
        try:
            if os.path.getmtime(p) < since:
                continue
        except OSError:
            continue
        try:
            with open(p, "rb") as f:
                for raw in f:
                    try:
                        d = json.loads(raw.decode("utf-8", errors="replace"))
                    except Exception:
                        continue
                    if proto and d.get("proto") != proto:
                        continue
                    yield d
        except OSError:
            continue


def parse_sip_body(raw: str) -> tuple:
    """raw SIP 텍스트 → (headers_dict, body_str).

    헤더는 첫 빈 줄(`\\r\\n\\r\\n` 또는 `\\n\\n`) 까지. 시작줄(start-line)은
    헤더 dict 에 포함하지 않음. 중복 헤더는 마지막 값 유지 (간단화).
    body 끝에 msg jsonl 에서 따라 붙는 trailing `]` 는 정리.
    """
    if not raw:
        return ({}, "")
    sep = raw.find("\r\n\r\n")
    if sep >= 0:
        head, body = raw[:sep], raw[sep + 4:]
    else:
        sep = raw.find("\n\n")
        if sep < 0:
            return ({}, "")
        head, body = raw[:sep], raw[sep + 2:]
    headers: dict = {}
    for line in head.splitlines()[1:]:
        if ":" not in line:
            continue
        k, _, v = line.partition(":")
        headers[k.strip()] = v.strip()
    body = body.rstrip()
    if body.endswith("]"):
        body = body[:-1].rstrip()
    return (headers, body)
