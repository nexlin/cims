"""검증 회차 webhook 발행.

job 종료 시 verdict + 요약 메타를 외부 webhook URL 로 POST. fire-and-forget —
실패해도 검증 자체에 영향 X. Slack incoming webhook / 사내 Hook 서비스 / CI
gate 알림 등 연동 용도.

** 환경 설정 **
  CIMS_VERIFY_WEBHOOK_URL    : POST 대상 URL (필수). 미설정 시 webhook 미발행.
  CIMS_VERIFY_WEBHOOK_FILTER : verdict 필터 — comma-separated. 예 "FAIL" 면
                                FAIL 만 알림. 기본은 모두 발행.
  CIMS_VERIFY_WEBHOOK_TIMEOUT: HTTP timeout 초 (기본 5).

** Payload schema (POST body, application/json) **
  {
    "run_id": int,
    "verdict": "PASS"|"FAIL"|"UNKNOWN",
    "scope": str,                         # "stage5" / "preset:..." / "items"
    "totals": {total, pass, fail, skip, blocked},
    "elapsed_ms": int,
    "started_at": str (ISO),
    "finished_at": str (ISO),
    "git_branch": str, "git_sha": str, "host": str,
    "trigger": str,                       # "user" / "cli" / "ci"
    "report_path": str,
    "pkg_manifest_hash": str,
  }

호출자가 record (write_run 결과) 를 그대로 넘기면 본 모듈이 적합한 subset 만
추출해 발송. raise 안 함 — 실패는 stderr 한 줄.
"""
from __future__ import annotations

import json
import os
import ssl
import sys
import urllib.request
import urllib.error
from typing import Optional


_ENV_URL     = "CIMS_VERIFY_WEBHOOK_URL"
_ENV_FILTER  = "CIMS_VERIFY_WEBHOOK_FILTER"
_ENV_TIMEOUT = "CIMS_VERIFY_WEBHOOK_TIMEOUT"

_INSECURE_CTX = ssl.create_default_context()
_INSECURE_CTX.check_hostname = False
_INSECURE_CTX.verify_mode = ssl.CERT_NONE


def _config() -> dict:
    """env → 설정 dict. URL 미설정 시 url=None."""
    url = (os.environ.get(_ENV_URL) or "").strip()
    flt = (os.environ.get(_ENV_FILTER) or "").strip()
    try:
        to = int(os.environ.get(_ENV_TIMEOUT) or "5")
    except (TypeError, ValueError):
        to = 5
    verdicts = {x.strip().upper() for x in flt.split(",") if x.strip()} if flt else None
    return {"url": url or None, "verdicts": verdicts, "timeout": max(1, to)}


def _build_payload(record: dict) -> dict:
    """write_run record 에서 webhook subset 추출."""
    totals = record.get("totals") or {}
    return {
        "run_id":            record.get("id"),
        "verdict":           record.get("verdict") or "UNKNOWN",
        "scope":             record.get("scope") or "",
        "totals": {
            "total":   int(totals.get("total", 0)),
            "pass":    int(totals.get("pass", 0)),
            "fail":    int(totals.get("fail", 0)),
            "skip":    int(totals.get("skip", 0)),
            "blocked": int(totals.get("blocked", 0)),
        },
        "elapsed_ms":        int(record.get("elapsed_ms") or 0),
        "started_at":        record.get("started_at"),
        "finished_at":       record.get("finished_at"),
        "git_branch":        record.get("git_branch") or "",
        "git_sha":           record.get("git_sha") or "",
        "host":              record.get("host") or "",
        "trigger":           record.get("trigger") or "user",
        "report_path":       record.get("report_path") or "",
        "pkg_manifest_hash": record.get("pkg_manifest_hash") or "",
    }


def publish(record: dict, *, dry_run: bool = False) -> Optional[dict]:
    """record 를 webhook 으로 POST. 발송 시 payload 반환, 미발송/실패 시 None.

    Args:
      record: write_run 의 record dict (id/verdict/scope/totals/...).
      dry_run: True 면 payload 만 반환 + 실 HTTP 호출 안 함 (테스트/CLI 미리보기).

    환경 변수:
      CIMS_VERIFY_WEBHOOK_URL: 대상 URL.
      CIMS_VERIFY_WEBHOOK_FILTER: verdict 필터 (comma-separated).
      CIMS_VERIFY_WEBHOOK_TIMEOUT: 초.
    """
    cfg = _config()
    if not cfg["url"]:
        return None
    payload = _build_payload(record)
    if cfg["verdicts"] and payload["verdict"] not in cfg["verdicts"]:
        return None     # filter 통과 X
    if dry_run:
        return payload

    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        cfg["url"], data=body, method="POST",
        headers={"Content-Type": "application/json", "User-Agent": "cims-verify/1"},
    )
    try:
        ctx = _INSECURE_CTX if cfg["url"].startswith("https://") else None
        with urllib.request.urlopen(req, context=ctx, timeout=cfg["timeout"]) as r:
            _ = r.read(256)    # drain
        return payload
    except urllib.error.HTTPError as e:
        print(
            f"[verify-webhook] HTTP {e.code} from {cfg['url']}: {e.reason}",
            file=sys.stderr,
        )
    except Exception as e:
        print(
            f"[verify-webhook] 발송 실패 {type(e).__name__}: {e}",
            file=sys.stderr,
        )
    return None
