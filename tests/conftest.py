"""
CIMS 검증 공통 설정 및 유틸리티

서버 IP/포트/도메인 같은 환경 상수는 `tests/test_env.json` (configure.sh 가
현재 배포 값으로 생성) 에서 읽어 중앙화한다. 파일이 없으면 csp.json 에서
자동 감지, 그것도 실패하면 127.0.0.1 fallback.
"""
import json
import os
import socket
import time
import urllib3

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# ── 환경 로더 ─────────────────────────────────────────────────

def _load_test_env() -> dict:
    """tests/test_env.json → 없으면 csp.json fallback → 127.0.0.1 최종 fallback."""
    here = os.path.dirname(os.path.abspath(__file__))
    env_file = os.path.join(here, "test_env.json")
    if os.path.exists(env_file):
        try:
            with open(env_file, encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            pass
    # csp.json fallback — 실제 배포의 LocalIp 자동 감지
    src_root = os.path.abspath(os.path.join(here, ".."))
    candidates = [
        os.path.join(src_root, "build", "dist", "csp", "config", "csp.json"),
        os.path.join(src_root, "dist", "csp", "config", "csp.json"),
    ]
    csp_ip = "127.0.0.1"
    volte_domain = "ims.mnc001.mcc001.3gppnetwork.org"
    ptt_domain   = "ptt.mnc001.mcc001.3gppnetwork.org"
    for p in candidates:
        if not os.path.exists(p): continue
        try:
            with open(p, encoding="utf-8") as f:
                cfg = json.load(f)
            setup = cfg.get("Setup", {})
            ip = setup.get("Sip", {}).get("LocalIp")
            if ip and ip != "0.0.0.0": csp_ip = ip
            for r in setup.get("Realm", []):
                svc = r.get("service"); doms = r.get("domains") or []
                if svc == "volte" and doms: volte_domain = doms[0]
                if svc == "mcptt" and doms: ptt_domain   = doms[0]
            break
        except Exception:
            continue
    return {
        "csp": {"ip": csp_ip, "sip_udp_port": 5060, "notify_udp_port": 4421,
                 "monitor_tcp_port": 16000},
        "cmp": {"ip": csp_ip, "control_port": 9000},
        "csc": {"host": "127.0.0.1", "admin_port": 4420, "mcptt_port": 4430},
        "domains": {"volte": volte_domain, "mcptt": ptt_domain,
                    "csp_realm": volte_domain},
        "db": {"host": "127.0.0.1", "port": 3306, "user": "cims",
               "password": "cims1234", "database": "cims"},
        "admin": {"login_id": "admin", "password": "1234"},
        "test_users": {"voip_msisdn": "+8299990001",
                        "ptt_msisdn": "+8299990002",
                        "group_id": "+8299991000",
                        "prefix": "_vtest_"},
    }


_ENV = _load_test_env()

# ── 서버 설정 (env 파일 또는 자동감지) ─────────────────────────
CSP_IP = _ENV["csp"]["ip"]
CSP_SIP_PORT = _ENV["csp"]["sip_udp_port"]
CSP_NOTIFY_IP = _ENV["csp"]["ip"]
CSP_NOTIFY_PORT = _ENV["csp"]["notify_udp_port"]
CMP_IP = _ENV["cmp"]["ip"]
CMP_PORT = _ENV["cmp"]["control_port"]
CSC_HOST = _ENV["csc"]["host"]
CSC_ADMIN_PORT = _ENV["csc"]["admin_port"]
CSC_MCPTT_PORT = _ENV["csc"]["mcptt_port"]
CSC_BASE = f"https://{CSC_HOST}:{CSC_ADMIN_PORT}"

# 도메인
VOLTE_DOMAIN = _ENV["domains"]["volte"]
PTT_DOMAIN = _ENV["domains"]["mcptt"]
CSP_REALM = _ENV["domains"]["csp_realm"]

# DB
DB_HOST = _ENV["db"]["host"]
DB_PORT = _ENV["db"]["port"]
DB_USER = _ENV["db"]["user"]
DB_PASSWORD = _ENV["db"]["password"]
DB_NAME = _ENV["db"]["database"]

# 관리자 계정
ADMIN_LOGIN = _ENV["admin"]["login_id"]
ADMIN_PW = _ENV["admin"]["password"]

# 테스트용 데이터 접두어 (정리 용이)
TEST_PREFIX = _ENV["test_users"]["prefix"]
TEST_VOIP_MSISDN = _ENV["test_users"]["voip_msisdn"]
TEST_PTT_MSISDN = _ENV["test_users"]["ptt_msisdn"]
TEST_GROUP_ID = _ENV["test_users"]["group_id"]


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
