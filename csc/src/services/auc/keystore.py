"""K/OPc 보관 형식 — AES-128-CTR + HMAC-SHA256 (encrypt-then-MAC).

  v1:<iv hex32><ct hex32><tag hex64>   (132 자, 컬럼 VARCHAR(160))
KEK 는 csc.json `AuC.Kek`(hex32 또는 base64 16B 이상 — SHA-256 으로 16B 로 정규화). KEK 가 없으면
AKA 가입자 프로비저닝·AV 발급 모두 거부한다(평문 보관 fallback 을 두지 않는다).
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import os

from .aes128 import AES128

_VERSION = "v1"


def normalize_kek(raw: str) -> bytes:
    """설정 문자열 → 16B KEK. hex32 는 그대로, 그 외는 base64/원문을 SHA-256 해 앞 16B."""
    s = (raw or "").strip()
    if not s:
        raise ValueError("AuC.Kek not configured")
    if len(s) == 32:
        try:
            return bytes.fromhex(s)
        except ValueError:
            pass
    try:
        b = base64.b64decode(s, validate=True)
    except Exception:
        b = s.encode()
    return hashlib.sha256(b).digest()[:16]


def _ctr(aes: AES128, iv: bytes, data: bytes) -> bytes:
    out = bytearray()
    ctr = int.from_bytes(iv, "big")
    for i in range(0, len(data), 16):
        ks = aes.encrypt_block(((ctr + i // 16) & ((1 << 128) - 1)).to_bytes(16, "big"))
        out += bytes(x ^ y for x, y in zip(data[i:i + 16], ks))
    return bytes(out)


def encrypt(kek: bytes, plain: bytes, iv: bytes | None = None) -> str:
    if len(kek) != 16:
        raise ValueError("KEK must be 16 bytes")
    iv = iv or os.urandom(16)
    ct = _ctr(AES128(kek), iv, plain)
    tag = hmac.new(kek, iv + ct, hashlib.sha256).digest()
    return f"{_VERSION}:{iv.hex()}{ct.hex()}{tag.hex()}"


def decrypt(kek: bytes, stored: str) -> bytes:
    if not stored:
        raise ValueError("empty key material")
    ver, _, body = stored.partition(":")
    if ver != _VERSION or len(body) < 32 + 64:
        raise ValueError("unknown key material format")
    iv = bytes.fromhex(body[:32])
    tag = bytes.fromhex(body[-64:])
    ct = bytes.fromhex(body[32:-64])
    if not hmac.compare_digest(hmac.new(kek, iv + ct, hashlib.sha256).digest(), tag):
        raise ValueError("key material MAC mismatch (KEK changed or data corrupted)")
    return _ctr(AES128(kek), iv, ct)
