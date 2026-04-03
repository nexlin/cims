"""
CIMS 검증 공통 설정 및 유틸리티
"""
import json
import socket
import time
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# ── 서버 설정 ──────────────────────────────────────────────────
CSC_BASE = "https://127.0.0.1:4420"
CSP_NOTIFY_IP = "127.0.0.1"
CSP_NOTIFY_PORT = 4421
CMP_IP = "127.0.0.1"
CMP_PORT = 9000

# 관리자 계정
ADMIN_LOGIN = "admin"
ADMIN_PW = "1234"

# 테스트용 데이터 접두어 (정리 용이)
TEST_PREFIX = "_vtest_"
TEST_VOIP_MSISDN = "+8299990001"
TEST_PTT_MSISDN = "+8299990002"
TEST_GROUP_ID = "+8299991000"


# ── HTTP 헬퍼 ──────────────────────────────────────────────────
import requests

class CscClient:
    """CSC REST API 클라이언트"""

    def __init__(self, base=CSC_BASE):
        self.base = base
        self.token = None
        self.session = requests.Session()
        self.session.verify = False

    def login(self, login_id=ADMIN_LOGIN, password=ADMIN_PW):
        r = self.post("/api/v1/auth/login", {"login_id": login_id, "password": password})
        if r.get("token"):
            self.token = r["token"]
        return r

    def _headers(self):
        h = {"Content-Type": "application/json"}
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        return h

    def get(self, path, params=None):
        resp = self.session.get(f"{self.base}{path}", headers=self._headers(), params=params)
        try:
            return {"_status": resp.status_code, **resp.json()}
        except Exception:
            return {"_status": resp.status_code}

    def post(self, path, data=None):
        resp = self.session.post(f"{self.base}{path}", headers=self._headers(), json=data)
        try:
            return {"_status": resp.status_code, **resp.json()}
        except Exception:
            return {"_status": resp.status_code}

    def put(self, path, data=None):
        resp = self.session.put(f"{self.base}{path}", headers=self._headers(), json=data)
        try:
            return {"_status": resp.status_code, **resp.json()}
        except Exception:
            return {"_status": resp.status_code}

    def delete(self, path):
        resp = self.session.delete(f"{self.base}{path}", headers=self._headers())
        try:
            return {"_status": resp.status_code, **resp.json()}
        except Exception:
            return {"_status": resp.status_code}


# ── UDP 헬퍼 ──────────────────────────────────────────────────
def udp_request(ip, port, data, timeout=3.0):
    """UDP JSON 요청/응답"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    msg = json.dumps(data).encode("utf-8")
    sock.sendto(msg, (ip, port))
    try:
        resp, _ = sock.recvfrom(8192)
        sock.close()
        return json.loads(resp.decode("utf-8"))
    except socket.timeout:
        sock.close()
        return None


def cmp_request(payload, timeout=3.0):
    """CMP에 명령 전송"""
    data = {
        "trans_id": int(time.time() * 1000) % 1000000,
        "payload": payload,
    }
    r = udp_request(CMP_IP, CMP_PORT, data, timeout)
    if r and isinstance(r.get("response"), str):
        try:
            r["response"] = json.loads(r["response"])
        except (json.JSONDecodeError, TypeError):
            pass
    return r


def csp_request(event, uri="", action="", etag=""):
    """CSP CscInterface에 이벤트 전송"""
    data = {"event": event, "uri": uri, "action": action, "etag": etag}
    return udp_request(CSP_NOTIFY_IP, CSP_NOTIFY_PORT, data, timeout=3.0)


# ── 결과 수집 ──────────────────────────────────────────────────
class TestResult:
    def __init__(self, test_id, name):
        self.id = test_id
        self.name = name
        self.status = "SKIP"
        self.detail = ""
        self.elapsed_ms = 0

    def to_dict(self):
        return {
            "id": self.id,
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
            "elapsed_ms": self.elapsed_ms,
        }


class TestRunner:
    def __init__(self, module_name):
        self.module = module_name
        self.results = []

    def run(self, test_id, name, func):
        """테스트 함수 실행 및 결과 수집"""
        r = TestResult(test_id, name)
        t0 = time.time()
        try:
            ok, detail = func()
            r.status = "PASS" if ok else "FAIL"
            r.detail = detail
        except Exception as e:
            r.status = "FAIL"
            r.detail = f"Exception: {e}"
        r.elapsed_ms = int((time.time() - t0) * 1000)
        self.results.append(r)

        icon = {"PASS": "\033[32mPASS\033[0m", "FAIL": "\033[31mFAIL\033[0m", "SKIP": "\033[33mSKIP\033[0m"}
        print(f"  [{icon[r.status]}] {r.id} {r.name} ({r.elapsed_ms}ms)")
        if r.status == "FAIL":
            print(f"         {r.detail}")

    def summary(self):
        total = len(self.results)
        passed = sum(1 for r in self.results if r.status == "PASS")
        failed = sum(1 for r in self.results if r.status == "FAIL")
        skipped = sum(1 for r in self.results if r.status == "SKIP")
        return {"module": self.module, "total": total, "pass": passed, "fail": failed, "skip": skipped,
                "results": [r.to_dict() for r in self.results]}
