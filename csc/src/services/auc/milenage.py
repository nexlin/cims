"""Milenage (3GPP TS 35.205/35.206) — f1, f1*, f2, f3, f4, f5, f5*.

시험 벡터: TS 35.207/35.208 (test_milenage.py). 모든 입력/출력은 bytes.
  K   16B   OPc 16B   RAND 16B   SQN 6B   AMF 2B
  f1 → MAC-A 8B      f1* → MAC-S 8B
  f2 → RES 8B        f3 → CK 16B    f4 → IK 16B
  f5 → AK 6B         f5* → AK* 6B
회전 상수 r1..r5 = 64,0,32,64,96 비트, c1..c5 = 0,1,2,4,8 (TS 35.206 §4.1).
"""
from __future__ import annotations

from .aes128 import AES128


def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def _rot(b: bytes, bits: int) -> bytes:
    """128-bit 좌회전 (바이트 단위 — r 은 모두 8 의 배수)."""
    n = (bits // 8) % 16
    return b[n:] + b[:n]


def opc_from_op(k: bytes, op: bytes) -> bytes:
    """OPc = E_K(OP) XOR OP (TS 35.206 §4.1)."""
    return _xor(AES128(k).encrypt_block(op), op)


class Milenage:
    def __init__(self, k: bytes, opc: bytes) -> None:
        if len(k) != 16 or len(opc) != 16:
            raise ValueError("K and OPc must be 16 bytes")
        self._aes = AES128(k)
        self._opc = opc

    def _temp(self, rand: bytes) -> bytes:
        return self._aes.encrypt_block(_xor(rand, self._opc))

    def f1_f1star(self, rand: bytes, sqn: bytes, amf: bytes) -> tuple:
        """(MAC-A, MAC-S)."""
        temp = self._temp(rand)
        in1 = sqn + amf + sqn + amf
        # OUT1 = E_K( TEMP XOR rot(IN1 XOR OPc, r1) XOR c1 ) XOR OPc,  r1=64, c1=0
        out1 = _xor(self._aes.encrypt_block(_xor(temp, _rot(_xor(in1, self._opc), 64))), self._opc)
        return out1[:8], out1[8:]

    def f2_f5(self, rand: bytes) -> tuple:
        """(RES, AK)."""
        temp = self._temp(rand)
        # OUT2 = E_K( rot(TEMP XOR OPc, r2) XOR c2 ) XOR OPc,  r2=0, c2=1
        x = bytearray(_rot(_xor(temp, self._opc), 0)); x[15] ^= 1
        out2 = _xor(self._aes.encrypt_block(bytes(x)), self._opc)
        return out2[8:], out2[:6]

    def f3(self, rand: bytes) -> bytes:
        temp = self._temp(rand)
        x = bytearray(_rot(_xor(temp, self._opc), 32)); x[15] ^= 2
        return _xor(self._aes.encrypt_block(bytes(x)), self._opc)

    def f4(self, rand: bytes) -> bytes:
        temp = self._temp(rand)
        x = bytearray(_rot(_xor(temp, self._opc), 64)); x[15] ^= 4
        return _xor(self._aes.encrypt_block(bytes(x)), self._opc)

    def f5star(self, rand: bytes) -> bytes:
        temp = self._temp(rand)
        x = bytearray(_rot(_xor(temp, self._opc), 96)); x[15] ^= 8
        return _xor(self._aes.encrypt_block(bytes(x)), self._opc)[:6]


def generate_av(k: bytes, opc: bytes, rand: bytes, sqn: bytes, amf: bytes) -> dict:
    """TS 33.102 §6.3.2 — AV = RAND ‖ XRES ‖ CK ‖ IK ‖ AUTN,  AUTN = (SQN⊕AK) ‖ AMF ‖ MAC-A."""
    m = Milenage(k, opc)
    mac_a, _ = m.f1_f1star(rand, sqn, amf)
    xres, ak = m.f2_f5(rand)
    return {
        "rand": rand,
        "xres": xres,
        "ck": m.f3(rand),
        "ik": m.f4(rand),
        "autn": _xor(sqn, ak) + amf + mac_a,
    }


def resync_sqn(k: bytes, opc: bytes, rand: bytes, auts: bytes):
    """AUTS = (SQN_MS ⊕ AK*) ‖ MAC-S 를 검증하고 SQN_MS 를 돌려준다 (TS 33.102 §6.3.5).

    MAC-S 는 AMF* = 0000 으로 계산된다(TS 33.102 §6.3.3). 검증 실패면 None.
    """
    if len(auts) != 14:
        return None
    m = Milenage(k, opc)
    ak_s = m.f5star(rand)
    sqn_ms = _xor(auts[:6], ak_s)
    _, mac_s = m.f1_f1star(rand, sqn_ms, b"\x00\x00")
    return sqn_ms if mac_s == auts[6:] else None


def sqn_to_bytes(sqn: int) -> bytes:
    return (sqn & 0xFFFFFFFFFFFF).to_bytes(6, "big")


def sqn_from_bytes(b: bytes) -> int:
    return int.from_bytes(b, "big")
