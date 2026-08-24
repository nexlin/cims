"""AuC 서비스 — AV 발급·AUTS 재동기 (sip_access_security.md §8.2).

Cx MAR/MAA 상당의 논리 계약(§4.4 aka 항)을 물리화한다. SQN_HE 는 이 모듈만 갱신한다:
  · 발급  : SQN_HE := SQN_HE + 1  → AV(RAND 신선, AUTN = (SQN⊕AK)‖AMF‖MAC-A)   (TS 33.102 §6.3.2, Annex C.3)
  · 재동기: AUTS 검증(MAC-S, AMF*=0000) → SQN_HE := SQN_MS  → 이어서 발급 (§6.3.5)
행 잠금(SELECT … FOR UPDATE) 트랜잭션 하나로 묶어 동시 REGISTER 가 같은 SQN 을 받지 않게 한다.
K/OPc 는 복호 즉시 쓰고 버린다 — 로그·응답 어디에도 남기지 않는다.
"""
from __future__ import annotations

import os
from typing import Optional

from . import keystore
from .milenage import generate_av, resync_sqn, sqn_to_bytes, sqn_from_bytes, opc_from_op

_KEK: Optional[bytes] = None
_TOKEN: str = ""
_HAS_AKA_COLS: Optional[bool] = None

SQN_MAX = (1 << 48) - 1
_TABLES = {"volte": "volte_subscriptions", "ptt": "ptt_subscriptions"}


class AucError(Exception):
    """API 응답 코드로 사상되는 오류 — status/code 를 함께 든다."""

    def __init__(self, status: int, code: str, detail: str = "") -> None:
        super().__init__(detail or code)
        self.status, self.code, self.detail = status, code, detail


def init(config: dict) -> None:
    """csc.json AuC.Kek / InternalApi.Token 적용 (기동·SIGUSR1 재로드)."""
    global _KEK, _TOKEN
    auc = config.get("AuC") or {}
    raw = str(auc.get("Kek") or "").strip()
    try:
        _KEK = keystore.normalize_kek(raw) if raw else None
    except ValueError:
        _KEK = None
    _TOKEN = str((config.get("InternalApi") or {}).get("Token") or "").strip()


def enabled() -> bool:
    return _KEK is not None


def internal_token() -> str:
    return _TOKEN


def has_aka_columns(cur) -> bool:
    """migrate_subscription_aka.sql 적용 여부 — 1회 프로브 캐시(프로세스 수명)."""
    global _HAS_AKA_COLS
    if _HAS_AKA_COLS is None:
        cur.execute("SHOW COLUMNS FROM volte_subscriptions LIKE 'auth_scheme'")
        _HAS_AKA_COLS = cur.fetchone() is not None
    return _HAS_AKA_COLS


def reset_schema_probe() -> None:
    global _HAS_AKA_COLS
    _HAS_AKA_COLS = None


# ── 프로비저닝 (admin.py 가 호출) ───────────────────────────────────────────

def _hex16(name: str, v) -> bytes:
    s = str(v or "").strip()
    try:
        b = bytes.fromhex(s)
    except ValueError:
        b = b""
    if len(b) != 16:
        raise AucError(400, "bad_key_material", f"{name} must be 32 hex chars (128-bit)")
    return b


def provision_keys(k_hex: str, opc_hex: str = "", op_hex: str = "") -> tuple:
    """(k_enc, opc_enc). opc 우선, 없으면 op → OPc 유도(TS 35.206 §4.1). KEK 미설정이면 503."""
    if _KEK is None:
        raise AucError(503, "auc_disabled", "AuC.Kek not configured")
    k = _hex16("k", k_hex)
    if str(opc_hex or "").strip():
        opc = _hex16("opc", opc_hex)
    elif str(op_hex or "").strip():
        opc = opc_from_op(k, _hex16("op", op_hex))
    else:
        raise AucError(400, "bad_key_material", "opc or op required")
    return keystore.encrypt(_KEK, k), keystore.encrypt(_KEK, opc)


def parse_amf(v) -> str:
    s = str(v if v is not None else "8000").strip().lower() or "8000"
    try:
        if len(bytes.fromhex(s)) != 2:
            raise ValueError
    except ValueError:
        raise AucError(400, "bad_key_material", "amf must be 4 hex chars")
    return s


def decrypt_keys(k_enc: str, opc_enc: str) -> tuple:
    """(K, OPc) bytes — 소프트-K 프로비저닝(/provisioning/me) 전용. KEK 불일치는 ValueError."""
    if _KEK is None:
        raise ValueError("AuC.Kek not configured")
    return keystore.decrypt(_KEK, k_enc), keystore.decrypt(_KEK, opc_enc)


# ── AV 발급 (내부 API) ───────────────────────────────────────────────────────

def _locate(cur, msisdn: str, service: str):
    """(table, row). service 가 비면 volte → ptt 순으로 찾는다 (CSP SelectUser 와 같은 순서)."""
    tables = [_TABLES[service]] if service in _TABLES else list(_TABLES.values())
    for t in tables:
        cur.execute(f"SELECT auth_scheme, k_enc, opc_enc, sqn, amf, imsi FROM {t} WHERE id=%s FOR UPDATE", (msisdn,))
        row = cur.fetchone()
        if row:
            return t, row
    return None, None


def issue(conn, msisdn: str, service: str = "", rand_hex: str = "", auts_hex: str = "") -> dict:
    """AV 1개 발급. auts 가 있으면 먼저 재동기(TS 33.102 §6.3.5)한 뒤 발급한다.

    반환: {"scheme":"aka","msisdn","service","av":{"rand","autn","xres","ck","ik"}(hex), "resynced":bool}
    오류: AucError(404 unknown_subscriber / 409 scheme_mismatch / 422 auts_invalid / 500 key_material)
    """
    if _KEK is None:
        raise AucError(503, "auc_disabled", "AuC.Kek not configured")
    conn.begin()
    try:
        with conn.cursor() as cur:
            if not has_aka_columns(cur):
                raise AucError(503, "schema_not_migrated", "migrate_subscription_aka.sql not applied")
            table, row = _locate(cur, msisdn, service)
            if row is None:
                raise AucError(404, "unknown_subscriber")
            scheme = (row.get("auth_scheme") if isinstance(row, dict) else row[0]) or "digest"
            k_enc, opc_enc, sqn_he, amf_hex = (
                (row["k_enc"], row["opc_enc"], int(row["sqn"] or 0), row["amf"]) if isinstance(row, dict)
                else (row[1], row[2], int(row[3] or 0), row[4]))
            if scheme != "aka":
                raise AucError(409, "scheme_mismatch", scheme)
            if not k_enc or not opc_enc:
                raise AucError(409, "keys_not_provisioned")
            try:
                k, opc = decrypt_keys(k_enc, opc_enc)
            except ValueError as e:
                raise AucError(500, "key_material", str(e))
            amf = bytes.fromhex(amf_hex or "8000")

            resynced = False
            if auts_hex:
                try:
                    rand_prev = bytes.fromhex(rand_hex or "")
                    auts = bytes.fromhex(auts_hex)
                except ValueError:
                    raise AucError(422, "auts_invalid", "rand/auts must be hex")
                if len(rand_prev) != 16:
                    raise AucError(422, "auts_invalid", "rand must be 16 bytes")
                sqn_ms = resync_sqn(k, opc, rand_prev, auts)
                if sqn_ms is None:
                    raise AucError(422, "auts_invalid", "MAC-S mismatch")
                sqn_he = sqn_from_bytes(sqn_ms)
                resynced = True

            sqn_next = (sqn_he + 1) & SQN_MAX
            rand = os.urandom(16)
            av = generate_av(k, opc, rand, sqn_to_bytes(sqn_next), amf)
            cur.execute(f"UPDATE {table} SET sqn=%s WHERE id=%s", (sqn_next, msisdn))
        conn.commit()
    except Exception:
        try:
            conn.rollback()
        except Exception:
            pass
        raise
    return {
        "scheme": "aka", "msisdn": msisdn,
        "service": "volte" if table == _TABLES["volte"] else "ptt",
        "resynced": resynced,
        "av": {k_: v.hex() for k_, v in av.items()},
    }
