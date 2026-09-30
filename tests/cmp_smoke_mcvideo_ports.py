#!/usr/bin/env python3
# CMP MCVideo 그룹 호 스모크 — cmp_media_api.md §7.9 (계약 K6) · mcvideo_dev_plan.md B3
#   HEARTBEAT resource.mcvideo → ADD 거절(max_transmitters 없음·floor 필드·group_type·tc_timers 범위) → ADD chat g101(멤버 포트 6포트 블록)
#   → 같은 id 의 MCPTT 그룹과 동시(자원 키 (service, group_id)) → JOIN ① 선할당(tc_ssrc 멱등) ② 주소 등록 → 로스터 밖 멤버·풀 소진 NO_RESOURCE
#   → implicit_request(chat 거절·prearranged granted 0) → 보호 키 거절 → PTT_FLOOR_TIER 거절 → 수신 판정(선언 소스 = 허가 없는 미디어 드롭 ·
#   전송 제어 MCV0 수신 · 미선언 소스 드롭 · 주소 전 드롭) → STATS mcvideo_groups → MCVideo 해제가 MCPTT 그룹에 무영향 → 멱등 LEAVE/REMOVE
#   → MODIFY NOT_FOUND → 자원 0 복귀
# 사용법: python3 tests/cmp_smoke_mcvideo_ports.py [--cmp build/bin/cmp]     — 시험용 CMP 를 빈 포트 창에 직접 띄우고 끝나면 내린다
#         python3 tests/cmp_smoke_mcvideo_ports.py --target IP:PORT            — 이미 띄운 시험용 CMP(McVideoMemberPoolSize 4, 같은 호스트)
#   **라이브 CMP 에 돌리지 않는다** — 이벤트 회신처가 마지막 요청 소켓이라 CSP 의 이벤트를 가로챈다.
import argparse, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--cmp", default=os.path.join(ROOT, "build", "bin", "cmp"))
ap.add_argument("--target", default="")
args = ap.parse_args()

IP = "127.0.0.1"
POOL = 4


def free_window(n=60, lo=33000, hi=60000, step=100):
    """n 포트가 모두 비어 있는 창의 시작 포트 (UDP bind 로 확인)."""
    for base in range(lo, hi, step):
        socks, ok = [], True
        try:
            for p in range(base, base + n):
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.bind((IP, p))
                socks.append(s)
        except OSError:
            ok = False
        for s in socks:
            s.close()
        if ok:
            return base
    raise SystemExit("free port window not found")


proc, tmp = None, None
if args.target:
    h, p = args.target.rsplit(":", 1)
    CMP = (h, int(p))
else:
    if not os.access(args.cmp, os.X_OK):
        raise SystemExit(f"cmp binary not found: {args.cmp} (make cmp)")
    B = free_window()
    tmp = tempfile.mkdtemp(prefix="cmp_smoke_mcv_")
    cfg = {"ServerIp": IP, "ServerPort": B + 50, "RtpIp": IP,
           "RtpStartPort": B, "RtpPoolSize": 1, "TranscodeSlots": 0, "TapPoolSize": 0, "AnnPlayers": 0,
           "PttRtpStartPort": B + 10, "PttMemberPoolSize": 1, "PttVideoStartPort": B + 12,
           "PttFloorStartPort": B + 14, "PttRtpPoolSize": 1, "PttMediaBufferMs": 0,
           "McVideoStartPort": B + 20, "McVideoMemberPoolSize": POOL,
           "RtpWorkerCount": 1, "LogDir": tmp, "SystemId": "cmp_smoke",
           "ServiceLogging": {"Dir": ""}, "Fm": {"Enable": False}}
    with open(os.path.join(tmp, "cmp.json"), "w") as f:
        json.dump(cfg, f)
    CMP = (IP, B + 50)
    proc = subprocess.Popen([args.cmp, os.path.join(tmp, "cmp.json")], cwd=tmp,
                            stdout=open(os.path.join(tmp, "stdout.log"), "w"), stderr=subprocess.STDOUT)

ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ctrl.bind((IP, 0))
_tid = [7920000]
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


def req(cmd, payload, service="mcvideo", sesid="mcv-smoke::1", timeout=3.0):
    _tid[0] += 1
    msg = {"hdr": {"ver": 2, "trans_id": _tid[0], "node": "csp-smoke", "cmd": cmd, "type": "request",
                   "sesid": sesid, "service": service}, "payload": payload}
    ctrl.sendto(json.dumps(msg).encode(), CMP)
    ctrl.settimeout(timeout)
    while True:
        d, _ = ctrl.recvfrom(8192)
        r = json.loads(d.decode())
        if r["hdr"].get("type") == "event":
            ctrl.sendto(json.dumps({"hdr": {"ver": 2, "trans_id": r["hdr"]["trans_id"], "node": "csp-smoke",
                                            "cmd": r["hdr"]["cmd"], "type": "response", "status": "OK"}}).encode(), CMP)
            continue
        if r["hdr"].get("trans_id") == _tid[0]:
            return r


def st(r):
    return r["hdr"].get("status"), r["hdr"].get("code", "")


def pl(r):
    return r.get("payload", {}) or {}


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((IP, 0))
    return s


def rtp(pt=96, ssrc=0x11110001, seq=1):
    return struct.pack("!BBHII", 0x80, pt, seq, 1000, ssrc) + b"\0" * 20


def mcv0(subtype, ssrc):
    return bytes([0x80 | subtype, 204]) + struct.pack("!H", 2) + struct.pack("!I", ssrc) + b"MCV0"


def stats():
    return pl(req("STATS", {}, service="system")).get("detail", {})


def mcv_group(gid):
    for g in stats().get("mcvideo_groups", []):
        if g.get("group_id") == gid:
            return g
    return None


try:
    # CMP 기동 대기 — HEARTBEAT 응답까지
    hb = None
    for _ in range(50):
        try:
            hb = req("HEARTBEAT", {}, service="system", timeout=0.2)
            break
        except (socket.timeout, ConnectionRefusedError, OSError):
            time.sleep(0.1)
    if hb is None:
        raise SystemExit("CMP did not answer HEARTBEAT")
    mcv = pl(hb).get("resource", {}).get("mcvideo")
    check("HEARTBEAT resource.mcvideo", mcv and mcv.get("member_total") == POOL and mcv.get("member_used") == 0, f"{mcv}")

    A, B_, C, D, E = "+82510002001", "+82510002002", "+82510002003", "+82510002004", "+82510002005"
    ROSTER = f"{A}:5:participant,{B_}:3:participant"

    # ── ADD 거절 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER})
    check("ADD without max_transmitters → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "floor_control": "on"})
    check("ADD with floor field → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "group_type": "private"})
    check("ADD group_type private → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 2, "tc_timers": {"c7": 0}})
    check("ADD tc_timers.c7 0 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "max_transmitters": 17})
    check("ADD max_transmitters 17 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_GROUP_MODIFY", {"group_id": "g-none", "members": ROSTER, "max_transmitters": 2})
    check("MODIFY unknown group → NOT_FOUND", st(r) == ("ERROR", "NOT_FOUND"), r["hdr"].get("reason"))
    check("rejected ADD leaves no resource", pl(req("HEARTBEAT", {}, service="system"))["resource"]["mcvideo"]["member_used"] == 0)

    # ── ADD chat g101 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": ROSTER, "subid": "1", "max_transmitters": 2,
                              "group_type": "chat", "reception_mode": "manual",
                              "tc_timers": {"t1_ms": 30000, "t5_ms": 30000, "c7": 2, "c11": 4}})
    mp = pl(r).get("member_ports", {})
    check("ADD chat g101 OK", st(r)[0] == "OK" and pl(r).get("ip") == IP and "floor_port" not in pl(r), f"{pl(r)}")
    pa, pb = mp.get(A, {}), mp.get(B_, {})
    blocks_ok = all(p and p["video_port"] == p["port"] + 2 and p["control_port"] == p["port"] + 4 for p in (pa, pb))
    check("member_ports = 6-port blocks {port, video_port, control_port}", blocks_ok and pa["port"] != pb["port"]
          and abs(pa["port"] - pb["port"]) % 6 == 0, f"A={pa} B={pb}")

    # ── 같은 group id 의 MCPTT 그룹 — 동시에 선다 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g101", "members": f"{A}:5:participant", "subid": "1"}, service="mcptt",
            sesid="ptt-smoke::1")
    check("MCPTT g101 alongside MCVideo g101", st(r)[0] == "OK" and pl(r).get("floor_port")
          and pl(r).get("member_ports", {}).get(A, {}).get("port") not in (pa.get("port"), pb.get("port")), f"{pl(r)}")

    # ── JOIN ① 선할당 · 멱등 ──
    r1 = req("PTT_JOIN", {"group_id": "g101", "session_id": A})
    r2 = req("PTT_JOIN", {"group_id": "g101", "session_id": A})
    tc = pl(r1).get("tc_ssrc", 0)
    check("JOIN pre-alloc = roster ports + tc_ssrc", st(r1)[0] == "OK" and pl(r1).get("port") == pa["port"]
          and pl(r1).get("control_port") == pa["control_port"] and tc > 0, f"{pl(r1)}")
    check("JOIN pre-alloc idempotent (same tc_ssrc)", pl(r2).get("tc_ssrc") == tc and pl(r2).get("port") == pa["port"])

    # ── JOIN ② 주소 등록 ──
    ua, uv, uc = udp(), udp(), udp()
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": A, "user_ip": IP, "user_port": ua.getsockname()[1],
                         "user_video_port": uv.getsockname()[1], "user_control_port": uc.getsockname()[1],
                         "user_uri": "sip:+82510002001@ptt.cims.example.kr", "user_tc_ssrc": 0x0A0A0A0A,
                         "user_pt": 96, "user_video_pt": 98, "max_priority": 5, "max_rx_streams": 1, "role": "participant"})
    check("JOIN address → same ports + tc_ssrc", st(r)[0] == "OK" and pl(r).get("tc_ssrc") == tc
          and pl(r).get("port") == pa["port"], f"{pl(r)}")
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "max_rx_streams": 0})
    check("JOIN max_rx_streams 0 → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "implicit_request": 1})
    check("JOIN implicit_request on chat → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": B_, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "media_crypto": {"alg": "AES_CM_128_HMAC_SHA1_80", "rx_key": "x", "tx_key": "y"}})
    check("JOIN media_crypto → BAD_REQUEST (fail-fast)", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))
    r = req("PTT_JOIN", {"group_id": "g-none", "session_id": A})
    check("JOIN unknown group → NOT_FOUND", st(r) == ("ERROR", "NOT_FOUND"))

    # ── 로스터 밖 멤버 · 풀 소진 ──
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": C})
    check("JOIN off-roster member → new unit", st(r)[0] == "OK" and pl(r).get("port") not in (pa["port"], pb["port"]))
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": D})
    check("JOIN 4th member (pool 4)", st(r)[0] == "OK")
    r = req("PTT_JOIN", {"group_id": "g101", "session_id": E})
    check("JOIN 5th member → NO_RESOURCE", st(r) == ("ERROR", "NO_RESOURCE"), r["hdr"].get("reason"))
    r = req("PTT_LEAVE", {"group_id": "g101", "session_id": D})
    check("LEAVE frees unit", st(r)[0] == "OK" and pl(req("HEARTBEAT", {}, service="system"))["resource"]["mcvideo"]["member_used"] == 3)

    # ── prearranged + 암묵적 송출 요청 ──
    r = req("PTT_GROUP_ADD", {"group_id": "g103", "members": f"{E}:5:participant", "max_transmitters": 1,
                              "group_type": "prearranged"}, sesid="mcv-smoke::2")
    check("ADD prearranged g103", st(r)[0] == "OK" and E in pl(r).get("member_ports", {}))
    r = req("PTT_JOIN", {"group_id": "g103", "session_id": E, "user_ip": IP, "user_port": udp().getsockname()[1],
                         "implicit_request": 1}, sesid="mcv-smoke::2")
    check("JOIN implicit_request (prearranged) → granted 0", st(r)[0] == "OK" and pl(r).get("granted") == 0
          and "audio_ssrc" not in pl(r), f"{pl(r)}")
    check("tc_ssrc unique across groups", pl(r).get("tc_ssrc") not in (0, tc))

    # ── floor 명령은 MCVideo 에 없다 ──
    r = req("PTT_FLOOR_TIER", {"group_id": "g101", "session_id": A, "tier": "emergency"})
    check("PTT_FLOOR_TIER mcvideo → BAD_REQUEST", st(r) == ("ERROR", "BAD_REQUEST"), r["hdr"].get("reason"))

    # ── 수신 판정 ──
    before = stats().get("rtp_src_drop", 0)
    for i in range(3):
        ua.sendto(rtp(seq=i + 1), (IP, pa["port"]))
        uv.sendto(rtp(pt=98, seq=i + 1), (IP, pa["video_port"]))
    uc.sendto(mcv0(0, tc), (IP, pa["control_port"]))          # Transmission Request (헤더 = tc_ssrc)
    uc.sendto(mcv0(4, 0x12345678), (IP, pa["control_port"]))  # Receive Media Request (다른 SSRC — 받고 기록)
    rr = struct.pack("!BBHI", 0x80, 201, 1, tc)                 # 빈 RR keepalive (RFC 3550 §6.4.2) — 드롭으로 세지 않는다
    uc.sendto(rr, (IP, pa["control_port"]))
    uc.sendto(rr + mcv0(2, tc), (IP, pa["control_port"]))     # compound RR + Transmission Release → APP 만 푼다
    uc.sendto(mcv0(1, tc), (IP, pa["control_port"]))          # MCV0 subtype 1 = 정의 없음 → 메시지 전체 버림 (§9.1.4 1)
    stranger = udp()
    stranger.sendto(rtp(), (IP, pa["port"]))                   # 미선언 소스 (nat 아님)
    udp().sendto(rtp(), (IP, pb["port"]))                      # 주소 등록 전 멤버 B
    time.sleep(0.3)
    g = mcv_group("g101") or {}
    after = stats().get("rtp_src_drop", 0)
    check("declared media before grant → no_grant_drop", g.get("no_grant_drop", 0) == 6, f"{g}")
    check("control MCV0 decoded (compound split, RR keepalive ignored)", g.get("control_rx", 0) == 3, f"{g}")
    check("undeclared source + pre-join + unknown subtype dropped", after - before == 3, f"src_drop {before} -> {after}")
    check("STATS mcvideo_groups", g.get("members") == 1 and g.get("reserved") == 3 and g.get("group_type") == "chat"
          and g.get("max_transmitters") == 2, f"{g}")

    # ── MCVideo 해제가 같은 id 의 MCPTT 그룹에 무영향 ──
    r = req("PTT_GROUP_REMOVE", {"group_id": "g101"})
    det = stats()
    ptt_ids = [x.get("group_id") for x in det.get("groups", [])]
    mcv_ids = [x.get("group_id") for x in det.get("mcvideo_groups", [])]
    check("REMOVE mcvideo g101 keeps MCPTT g101", st(r)[0] == "OK" and "g101" in ptt_ids and "g101" not in mcv_ids,
          f"ptt={ptt_ids} mcv={mcv_ids}")
    r = req("PTT_GROUP_REMOVE", {"group_id": "g101"})
    check("REMOVE idempotent", st(r)[0] == "OK")
    r = req("PTT_LEAVE", {"group_id": "g101", "session_id": A})
    check("LEAVE on removed group idempotent", st(r)[0] == "OK")
    ua.sendto(rtp(), (IP, pa["port"]))                         # 해제 뒤 늦은 패킷 — 크래시 없이 버린다
    time.sleep(0.1)
    req("PTT_GROUP_REMOVE", {"group_id": "g103"}, sesid="mcv-smoke::2")
    req("PTT_GROUP_REMOVE", {"group_id": "g101"}, service="mcptt", sesid="ptt-smoke::1")
    res = pl(req("HEARTBEAT", {}, service="system"))["resource"]
    check("all released", res["mcvideo"]["member_used"] == 0 and res["mcvideo"]["groups"] == 0
          and res["ptt"]["groups"] == 0, f"{res['mcvideo']} ptt={res['ptt']}")
finally:
    if proc:
        alive = proc.poll() is None
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        if not alive:
            print("FAIL  CMP exited during test (see log)")
            results.append(False)
        if all(results) and tmp:
            shutil.rmtree(tmp, ignore_errors=True)
        elif tmp:
            print(f"CMP log kept: {tmp}")

n_ok = sum(results)
print(f"\n{n_ok}/{len(results)} PASS")
sys.exit(0 if results and all(results) else 1)
