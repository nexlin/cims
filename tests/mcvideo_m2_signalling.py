#!/usr/bin/env python3
# MCVideo M2 신호 시험 실행기 — cimsue-cli 두세 대(.45) → .48 CSP·CMP·CSC. 절차 정본 = docs/dev/server45_handoff.md §11 «M2 신호 시험 절차 제안»
# (T1~T9) · .48 쪽 준비 = docs/dev/mcvideo_m2_runbook.md.
#
# **실서버에 MCVideo REGISTER·PUBLISH·INVITE 를 보낸다** — 사용자 결정 셋(.48 배포 · 공유 DB migrate_mcvideo.sql · Roles.MCVIDEO) 과 .48 의
# «준비 끝» 알림 뒤에만 `--confirm` 으로 돌린다. `--confirm` 이 없으면 계획만 찍고 아무것도 보내지 않는다.
#
# 신원 = 계측기 PTT 신원 A/B/C(기본 test023·024·025). 자격은 creds 파일(jsonl: user·login·loginPw)을 **그 자리에서** 읽어 환경변수로만
# cimsue-cli 에 넘긴다(`--pw-env` — 명령행·로그에 비밀이 남지 않는다). 계정 설정은 CSC 프로파일(`--from-profile ptt`)이 채운다 —
# MCVideo PSI 도 ue-init-config 의 MCVideo-Service-Details 에서.
#
# 사용: python3 tests/mcvideo_m2_signalling.py [--only T1,T3] [--csc-host 121.161.164.48] [--csc-ca FILE] [--out DIR] --confirm
#   종료 코드 0 = 고른 항목 전부 PASS, 1 = FAIL 있음, 2 = 인자·준비 오류(또는 --confirm 없음)
import argparse, json, os, subprocess, sys, tempfile, threading, time, uuid

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument("--cli", default=os.path.join(ROOT, "build", "bin", "cimsue-cli"))
ap.add_argument("--csc-host", default="121.161.164.48")
ap.add_argument("--csc-port", type=int, default=4430)
ap.add_argument("--csc-ca", default="", help="CSC TLS 신뢰 앵커(PEM). 비우면 --no-tls-verify")
ap.add_argument("--creds", default="/mnt/cims/test48/tester/scenarios/creds/ptt.jsonl")
ap.add_argument("--ids", default="+82500000023,+82500000024,+82500000025", help="A,B,C 의 MSISDN")
ap.add_argument("--chat", default="gmv1", help="MCPTT + MCVideo chat 그룹(max_transmitters 1)")
ap.add_argument("--pre", default="gmv2", help="MCPTT + MCVideo prearranged 그룹")
ap.add_argument("--only", default="", help="쉼표 구분 항목(T1..T9, T7b) — 비우면 전부")
ap.add_argument("--out", default="", help="결과·로그 디렉터리(기본 임시)")
ap.add_argument("--confirm", action="store_true", help="실서버로 실제로 보낸다")
args = ap.parse_args()

PLAN = [
    ("T1", "등록 태그 — A --mcvideo register"),
    ("T2", "affiliation — A --affiliate-mcvideo chat 그룹 register(200 Expires 2^32-1, ptt_affiliations 무변화는 .48 DB 로 확인)"),
    ("T3", "chat 합류·송출·수신 — B video-call --accept, A video-call --transmit-at"),
    ("T8", "상한 초과 — chat(max 1) A 송출 중 C 송출 요청 → Rejected(또는 Queue Position)"),
    ("T4", "prearranged 팬아웃·암묵 요청 — B·C 제휴 + video-answer, A 제휴 + video-call --prearranged --implicit"),
    ("T5", "재합류 — B·C 가 남은 세션에 A 가 --rejoin <session_uri>"),
    ("T6", "거절 — chat→prearranged 404 117 · prearranged→chat 404 118 · 미제휴 prearranged 403 120"),
    ("T9", "해제 — prearranged 에서 B 가 나가면 참가자 1명 → CSP 가 A 를 끊는다"),
    ("T7", "MCPTT 회귀 — 같은 신원 MCPTT group-call(floor)"),
    ("T7b", "MCPTT·MCVideo 동시 — 같은 chat 그룹에서 MCPTT 그룹 호와 MCVideo 그룹 호를 함께"),
]
only = [x.strip() for x in args.only.split(",") if x.strip()]
plan = [p for p in PLAN if not only or p[0] in only]
print("MCVideo M2 신호 시험 — 대상 CSC %s:%d · 그룹 chat=%s prearranged=%s" % (args.csc_host, args.csc_port, args.chat, args.pre))
for t, d in plan:
    print("  %-4s %s" % (t, d))
if not args.confirm:
    print("--confirm 이 없어 보내지 않는다(사용자 결정 셋 + .48 준비 알림 뒤에 --confirm).")
    sys.exit(2)
if not os.access(args.cli, os.X_OK):
    print("cimsue-cli 없음: %s (make cimsue-cli)" % args.cli)
    sys.exit(2)

ids = args.ids.split(",")
if len(ids) != 3:
    print("--ids 는 A,B,C 셋")
    sys.exit(2)
creds = {}
with open(args.creds) as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        o = json.loads(line)
        if o.get("user") in ids:
            creds[o["user"]] = o
missing = [i for i in ids if i not in creds or not creds[i].get("login") or not creds[i].get("loginPw")]
if missing:
    print("creds 에 login·loginPw 가 없는 신원: %s" % ",".join(missing))
    sys.exit(2)
A, B, C = ids
NAME = {A: "A", B: "B", C: "C"}
out = args.out or tempfile.mkdtemp(prefix="mcv_m2_")
os.makedirs(out, exist_ok=True)
print("결과·로그: %s" % out)


def run(who, cli_args, tag):
    """cimsue-cli 한 번 — 마지막 JSON 줄을 결과로. 비밀은 환경변수(CIMS_M2_PW)로만."""
    o = creds[who]
    env = dict(os.environ, CIMS_M2_PW=o["loginPw"])
    inst = "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, "cims-m2-" + who))
    cmd = [args.cli, "--csc-host", args.csc_host, "--csc-port", str(args.csc_port), "--user", o["login"], "--pw-env", "CIMS_M2_PW",
           "--from-profile", "ptt", "--instance-id", inst, "--mcvideo", "--json"]
    cmd += ["--csc-ca", args.csc_ca] if args.csc_ca else ["--no-tls-verify"]
    cmd += cli_args
    t0 = time.time()
    with open(os.path.join(out, "%s_%s.err" % (tag, NAME[who])), "w") as ef:
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=ef, text=True, timeout=180)
    res = {}
    for line in p.stdout.splitlines()[::-1]:
        line = line.strip()
        if line.startswith("{"):
            try:
                res = json.loads(line)
                break
            except ValueError:
                pass
    res["_rc"], res["_secs"] = p.returncode, round(time.time() - t0, 1)
    with open(os.path.join(out, "%s_%s.json" % (tag, NAME[who])), "w") as jf:
        json.dump(res, jf, ensure_ascii=False)
    return res


def parallel(jobs):
    """[(delay_s, who, args, tag)] — 각자 delay 뒤 시작, 전부 끝날 때까지. 결과 = {이름: res}."""
    res, ths = {}, []
    for delay, who, a, tag in jobs:
        def go(delay=delay, who=who, a=a, tag=tag):
            time.sleep(delay)
            res[NAME[who]] = run(who, a, tag)
        th = threading.Thread(target=go)
        th.start()
        ths.append(th)
    for th in ths:
        th.join()
    return res


def stderr_has(tag, who, text):
    try:
        return text in open(os.path.join(out, "%s_%s.err" % (tag, NAME[who]))).read()
    except OSError:
        return False


verdict = []


def check(t, name, ok, detail=""):
    verdict.append((t, bool(ok)))
    print("%s  %-4s %s  %s" % ("PASS" if ok else "FAIL", t, name, detail))


aff_pre = ["--affiliate-mcvideo", args.pre]
for t, _ in plan:
    if t == "T1":
        r = run(A, ["register", "--hold", "3"], "T1")
        check(t, "A 등록(MCVideo 태그)", r.get("outcome") == "registered", "code=%s" % r.get("code"))
    elif t == "T2":
        r = run(A, ["--affiliate-mcvideo", args.chat, "register", "--hold", "3"], "T2")
        check(t, "A MCVideo affiliation PUBLISH 2xx", r.get("outcome") == "registered" and not stderr_has("T2", A, "affiliate(mcvideo)"),
              "code=%s" % r.get("code"))
    elif t == "T3":
        rs = parallel([(0, B, ["video-call", args.chat, "--accept", "--duration", "20"], "T3"),
                       (3, A, ["video-call", args.chat, "--transmit-at", "2", "--transmit-len", "5", "--duration", "12"], "T3")])
        a, b = rs["A"], rs["B"]
        check(t, "A 송출 허가", a.get("tx_granted", 0) >= 1, "A=%s" % {k: a.get(k) for k in ("outcome", "code", "tx_granted", "session_uri")})
        check(t, "B 알림·[받기] 허가", b.get("rx_notified", 0) >= 1 and b.get("rx_granted", 0) >= 1,
              "B=%s" % {k: b.get(k) for k in ("outcome", "rx_notified", "rx_granted")})
    elif t == "T8":
        rs = parallel([(0, A, ["video-call", args.chat, "--transmit-at", "2", "--transmit-len", "8", "--duration", "14"], "T8"),
                       (0.5, C, ["video-call", args.chat, "--transmit-at", "4", "--transmit-len", "2", "--duration", "12"], "T8")])
        a, c = rs["A"], rs["C"]
        check(t, "A 허가 · C 상한 거절(또는 대기열)", a.get("tx_granted", 0) >= 1 and (c.get("tx_rejected", 0) >= 1 or stderr_has("T8", C, "Queue")),
              "A.tx_granted=%s C=%s" % (a.get("tx_granted"), {k: c.get(k) for k in ("outcome", "tx_granted", "tx_rejected")}))
    elif t == "T4":
        rs = parallel([(0, B, aff_pre + ["video-answer", "--accept", "--duration", "25"], "T4"),
                       (0, C, aff_pre + ["video-answer", "--accept", "--duration", "25"], "T4"),
                       (4, A, aff_pre + ["video-call", args.pre, "--prearranged", "--implicit", "--transmit-at", "1", "--transmit-len", "4",
                                         "--duration", "12"], "T4")])
        a, b, c = rs["A"], rs["B"], rs["C"]
        check(t, "A 암묵 요청 허가", a.get("tx_granted", 0) >= 1, "A=%s" % {k: a.get(k) for k in ("outcome", "code", "tx_granted")})
        check(t, "B·C 초대 수락·알림", b.get("rx_notified", 0) >= 1 and c.get("rx_notified", 0) >= 1,
              "B=%s C=%s" % (b.get("outcome"), c.get("outcome")))
    elif t == "T5":
        first = {}
        def a_first():
            time.sleep(4)
            first["A"] = run(A, aff_pre + ["video-call", args.pre, "--prearranged", "--duration", "5"], "T5a")
            sess = first["A"].get("session_uri", "")
            first["A2"] = run(A, aff_pre + ["video-call", args.pre, "--rejoin", sess, "--transmit-at", "1", "--transmit-len", "3",
                                            "--duration", "8"], "T5b") if sess else {"outcome": "no_session_uri"}
        th = threading.Thread(target=a_first)
        th.start()
        rs = parallel([(0, B, aff_pre + ["video-answer", "--accept", "--duration", "35"], "T5"),
                       (0, C, aff_pre + ["video-answer", "--accept", "--duration", "35"], "T5")])
        th.join()
        a2 = first.get("A2", {})
        check(t, "A 재합류 성립·송출 허가", a2.get("tx_granted", 0) >= 1, "sess=%s A2=%s" % (first.get("A", {}).get("session_uri"),
                                                                                     {k: a2.get(k) for k in ("outcome", "code", "tx_granted")}))
        check(t, "B 가 재합류자의 송출 알림", rs["B"].get("rx_notified", 0) >= 1, "B.rx_notified=%s" % rs["B"].get("rx_notified"))
    elif t == "T6":
        r1 = run(A, aff_pre + ["video-call", args.pre, "--duration", "3"], "T6a")                    # chat 호 → prearranged 그룹
        r2 = run(A, ["video-call", args.chat, "--prearranged", "--duration", "3"], "T6b")            # prearranged 호 → chat 그룹
        r3 = run(A, ["video-call", args.pre, "--prearranged", "--duration", "3"], "T6c")             # 제휴 없이 prearranged
        check(t, "chat→prearranged 404", r1.get("code") == 404, "code=%s" % r1.get("code"))
        check(t, "prearranged→chat 404", r2.get("code") == 404, "code=%s" % r2.get("code"))
        check(t, "미제휴 prearranged 403", r3.get("code") == 403, "code=%s" % r3.get("code"))
    elif t == "T9":
        rs = parallel([(0, B, aff_pre + ["video-answer", "--duration", "6"], "T9"),
                       (3, A, aff_pre + ["video-call", args.pre, "--prearranged", "--duration", "40"], "T9")])
        a = rs["A"]
        check(t, "B 이탈 뒤 A 가 서버 해제로 끝남(40 s 전에)", a.get("_secs", 99) < 30 and a.get("outcome") not in ("call_failed", "call_timeout"),
              "A secs=%s outcome=%s code=%s" % (a.get("_secs"), a.get("outcome"), a.get("code")))
    elif t == "T7":
        rs = parallel([(0, B, ["--affiliate", args.chat, "group-call", args.chat, "--duration", "12"], "T7"),
                       (2, A, ["--affiliate", args.chat, "group-call", args.chat, "--ptt-at", "2", "--ptt-len", "3", "--duration", "8"], "T7")])
        a = rs["A"]
        check(t, "MCPTT floor 허가", a.get("granted", 0) >= 1, "A=%s" % {k: a.get(k) for k in ("outcome", "code", "granted")})
    elif t == "T7b":
        rs = parallel([(0, B, ["--affiliate", args.chat, "group-call", args.chat, "--duration", "16"], "T7b"),
                       (1, C, ["video-call", args.chat, "--accept", "--duration", "14"], "T7b"),
                       (3, A, ["video-call", args.chat, "--transmit-at", "2", "--transmit-len", "4", "--duration", "8"], "T7b")])
        check(t, "MCPTT 호 유지 중 MCVideo 송출·수신", rs["A"].get("tx_granted", 0) >= 1 and rs["C"].get("rx_notified", 0) >= 1
              and rs["B"].get("outcome") not in ("call_failed", "call_timeout"),
              "A.tx=%s C.rx=%s B=%s" % (rs["A"].get("tx_granted"), rs["C"].get("rx_notified"), rs["B"].get("outcome")))

bad = [t for t, ok in verdict if not ok]
print("총 %d 확인 · FAIL %d%s" % (len(verdict), len(bad), (" (" + ",".join(sorted(set(bad))) + ")") if bad else ""))
sys.exit(1 if bad else 0)
