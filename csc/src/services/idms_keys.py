"""IdMS 토큰 서명 키 (TS 33.180 B.2.2.1 — access token 은 JSON web digital signature 프로파일(RFC 7515), OIDC Core §15.1 —
OP 는 ID token 을 RS256 으로 서명할 수 있어야 한다).

- 서명 키 = RSA 2048, runtime store `idms_keys/signing.pem`(PKCS#8, 0600). 없으면 처음 기동 때 만든다 — HA 쌍은 같은
  runtime store 를 보므로 같은 키로 서명한다. `IdMs.SigningKeyFile` 로 다른 경로의 키를 줄 수 있다.
- `kid` = RFC 7638 JWK thumbprint. 공개 키는 `GET /idms/jwks`(openid-configuration `jwks_uri`)로 낸다.
- 키 교체 = `signing.pem` 을 `previous-<이름>.pem` 으로 바꾸고 재기동한다. `previous-*.pem` 은 검증과 JWKS 에만 쓰이고,
  그 키로 서명한 토큰의 수명(`IdMs.AccessTokenTtl`)이 지나면 지운다.
"""
import base64
import glob
import hashlib
import json
import os
from typing import Optional

from services import file_store
from util.log_util import Logger

logger = Logger()

KEY_DOMAIN = 'idms_keys'
SIGNING_FILE = 'signing.pem'
PREVIOUS_GLOB = 'previous-*.pem'
RSA_BITS = 2048

_signing_key = None          # 서명 개인 키
_signing_kid: str = ''
_public_keys: dict = {}      # kid → 공개 키 (서명 키 + previous)
_jwks: dict = {"keys": []}


def _b64u(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b'=').decode('ascii')


def _jwk(public_key) -> dict:
    """공개 키 → JWK(RFC 7517) + kid(RFC 7638 thumbprint — 필수 멤버 e·kty·n 을 사전순·공백 없이 직렬화한 SHA-256)."""
    nums = public_key.public_numbers()
    n = _b64u(nums.n.to_bytes((nums.n.bit_length() + 7) // 8, 'big'))
    e = _b64u(nums.e.to_bytes((nums.e.bit_length() + 7) // 8, 'big'))
    canon = json.dumps({"e": e, "kty": "RSA", "n": n}, separators=(',', ':'), sort_keys=True)
    kid = _b64u(hashlib.sha256(canon.encode('ascii')).digest())
    return {"kty": "RSA", "use": "sig", "alg": "RS256", "kid": kid, "n": n, "e": e}


def _load_pem(path: str):
    """PEM → (개인 키 또는 None, 공개 키). previous 는 공개 키 PEM 만 남겨도 된다."""
    from cryptography.hazmat.primitives import serialization
    with open(path, 'rb') as f:
        data = f.read()
    if b'PRIVATE KEY' in data:
        priv = serialization.load_pem_private_key(data, password=None)
        return priv, priv.public_key()
    return None, serialization.load_pem_public_key(data)


def _create(path: str) -> None:
    """서명 키를 만들어 `path` 에 둔다. 다른 프로세스(HA 상대)가 먼저 만들었으면 그대로 둔다 — 임시 파일에 다 쓴 뒤 link 로
    올리므로 반쯤 쓰인 키가 보이지 않고, 이미 있으면 link 가 실패한다."""
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = rsa.generate_private_key(public_exponent=65537, key_size=RSA_BITS)
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    tmp = f"{path}.{os.getpid()}.tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'wb') as f:
            f.write(pem)
            f.flush()
            os.fsync(f.fileno())
        try:
            os.link(tmp, path)
            logger.log_info(f"[IdMS][key] signing key created: {path}")
        except FileExistsError:
            pass
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def init(config: dict) -> bool:
    """서명 키를 읽는다(없으면 만든다). 실패하면 False — 호출자가 서명 방식을 정한다."""
    global _signing_key, _signing_kid, _public_keys, _jwks
    _signing_key, _signing_kid, _public_keys, _jwks = None, '', {}, {"keys": []}
    try:
        explicit = ((config.get('IdMs') or {}).get('SigningKeyFile') or '').strip()
        if explicit:
            path, key_dir = explicit, os.path.dirname(explicit)
        else:
            key_dir = file_store.domain_dir(config, KEY_DOMAIN)
            path = os.path.join(key_dir, SIGNING_FILE)
            try:
                os.chmod(key_dir, 0o700)
            except OSError:
                pass
        if not os.path.exists(path):
            _create(path)
        priv, pub = _load_pem(path)
        if priv is None:
            raise ValueError(f"{path}: private key required")
        jwk = _jwk(pub)
        keys, jwks = {jwk['kid']: pub}, [jwk]
        for prev in sorted(glob.glob(os.path.join(key_dir, PREVIOUS_GLOB))):
            try:
                _, ppub = _load_pem(prev)
                pj = _jwk(ppub)
                if pj['kid'] not in keys:
                    keys[pj['kid']] = ppub
                    jwks.append(pj)
            except Exception as e:
                logger.log_error(f"[IdMS][key] previous key skipped: {prev}: {e}")
        _signing_key, _signing_kid, _public_keys, _jwks = priv, jwk['kid'], keys, {"keys": jwks}
        logger.log_info(f"[IdMS][key] signing kid={_signing_kid} verify-keys={len(keys)}")
        return True
    except Exception as e:
        logger.log_error(f"[IdMS][key] signing key unavailable: {e}")
        return False


def ready() -> bool:
    return _signing_key is not None


def signing_key():
    return _signing_key


def signing_kid() -> str:
    return _signing_kid


def public_key(kid: Optional[str]):
    """검증 키. kid 가 없는 토큰은 서명 키로 본다."""
    if not kid:
        return _public_keys.get(_signing_kid)
    return _public_keys.get(kid)


def jwks() -> dict:
    return _jwks
