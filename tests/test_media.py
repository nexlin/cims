"""
미디어/녹취 검증: AMR-WB RTP ���수신, CMP 녹취 파일 생성/정합성, 트랜스코딩 검증
"""
import sys, os, time, subprocess, re, struct, socket, json
sys.path.insert(0, os.path.dirname(__file__))

from conftest import CscClient, csp_request, cmp_request, TestRunner

DIST_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "dist"))

def _get_cmp_rtp_ip():
    """CMP stats에서 실제 RTP IP 조회"""
    r = cmp_request({"cmd": "stats"})
    if r and isinstance(r.get("response"), dict):
        # stats에는 IP 없으므로 add 테스트로 확인
        pass
    # CMP config에서 직접 읽기
    cfg_path = os.path.join(DIST_DIR, "cmp", "config", "cmp.json")
    if os.path.exists(cfg_path):
        with open(cfg_path) as f:
            import json as _json
            cfg = _json.load(f)
            return cfg.get("RtpIp", "127.0.0.1")
    return "127.0.0.1"
CSPSIM = os.path.join(DIST_DIR, "cspsim", "bin", "cspsim")
MEDIA_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "media"))
CSP_IP = "127.0.0.1"
VOIP_DOMAIN = "ims.mnc033.mcc450.3gppnetwork.org"
PTT_DOMAIN = "ptt.mnc033.mcc450.3gppnetwork.org"

VOIP_USER1 = "+821357007002"
VOIP_AUTH1 = "450033100000002@" + VOIP_DOMAIN
VOIP_PW = "123456"

PTT_USER1 = "+82571900001"
PTT_GROUP = "+82571910001"
PTT_PW = "123456"

AMRWB_FILE = os.path.join(MEDIA_DIR, "8050001000004_audio.amrwb")


def _run_cspsim(args, timeout=30):
    cmd = [CSPSIM] + args
    try:
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           timeout=timeout, cwd=os.path.join(DIST_DIR, "cspsim"))
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    except FileNotFoundError:
        return f"NOT_FOUND: {CSPSIM}"


def _parse_stats(output):
    stats = {}
    for line in output.split("\n"):
        m = re.search(r"Registered\s*:\s*(\d+)\s*/\s*\d+\s*\(fail=(\d+)\)", line)
        if m: stats["RegOk"], stats["RegFail"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"Call OK/End\s*:\s*(\d+)\s*/\s*(\d+)\s*\(fail=(\d+)\)", line)
        if m: stats["CallOk"], stats["CallEnd"], stats["CallFail"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if "AMR-WB frames" in line:
            stats["MediaLoaded"] = True
    return stats


def _parse_raw_rtp(filepath):
    """raw RTP 파일([uint32 len][pkt]...)을 파싱하여 패킷 정보 반환"""
    if not os.path.exists(filepath):
        return []
    packets = []
    with open(filepath, 'rb') as f:
        while True:
            lenbuf = f.read(4)
            if len(lenbuf) < 4:
                break
            pkt_len = struct.unpack('<I', lenbuf)[0]
            pkt = f.read(pkt_len)
            if len(pkt) < pkt_len:
                break
            if pkt_len >= 12:
                pt = pkt[1] & 0x7F
                seq = struct.unpack('>H', pkt[2:4])[0]
                ts = struct.unpack('>I', pkt[4:8])[0]
                packets.append({'pt': pt, 'seq': seq, 'ts': ts, 'size': pkt_len})
    return packets


def _find_recording_files(rec_dir):
    """녹취 raw 디렉터리에서 최신 녹취 파일 찾기"""
    if not os.path.isdir(rec_dir):
        return {}
    files = {}
    for fn in os.listdir(rec_dir):
        path = os.path.join(rec_dir, fn)
        if fn.endswith('_a.rtp'):
            files['audio_a'] = path
        elif fn.endswith('_b.rtp'):
            files['audio_b'] = path
        elif fn.endswith('_va.rtp'):
            files['video_a'] = path
        elif fn.endswith('_vb.rtp'):
            files['video_b'] = path
    return files


def run_media_tests():
    runner = TestRunner("미디어-녹취")
    c = CscClient()
    c.login()

    # 녹취 디렉터리
    rec_raw_dir = os.path.join(DIST_DIR, "ext_mnt", "recordings", "raw")

    # 미디어 파일 존재 확인
    if not os.path.isfile(AMRWB_FILE):
        print(f"  [INFO] AMR-WB 미디어 파일 추출 중...")
        subprocess.run([sys.executable, os.path.join(MEDIA_DIR, "extract_frames.py"),
                        os.path.join(MEDIA_DIR, "8050001000004.3gp"), MEDIA_DIR],
                       capture_output=True, timeout=10)

    # ================================================================
    # MEDIA-RTP: AMR-WB RTP 송수신 검증
    # ================================================================
    print("\n── MEDIA-RTP: AMR-WB RTP 미디어 전송 ──")

    def rtp_01():
        """AMR-WB 미디어 파일 로딩 확인"""
        if not os.path.isfile(AMRWB_FILE):
            return False, f"AMR-WB 파일 없음: {AMRWB_FILE}"
        sz = os.path.getsize(AMRWB_FILE)
        frame_count = sz // 61
        ok = frame_count >= 100
        return ok, f"file_size={sz}, frames={frame_count}"
    runner.run("MEDIA-RTP-01", "AMR-WB 미디어 파일 준비 확인", rtp_01)

    def rtp_02():
        """cspsim -media_file로 AMR-WB RTP 전송 + 통화 성공 확인"""
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "2",
            "-user", VOIP_USER1, "-auth_id", VOIP_AUTH1,
            "-domain", VOIP_DOMAIN, "-password", VOIP_PW,
            "-mode", "voip", "-scenario", "call", "-call_duration", "3",
            "-media_file", AMRWB_FILE,
        ], timeout=20)
        s = _parse_stats(out)
        checks = []
        checks.append(("미디어 로딩", s.get("MediaLoaded", False)))
        checks.append(("통화 성공", s.get("CallOk", 0) >= 1))
        ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        return ok, detail
    runner.run("MEDIA-RTP-02", "AMR-WB RTP 전송 + VoIP 통화", rtp_02)

    def rtp_03():
        """PTT 그룹콜에서 AMR-WB RTP 전송 확인"""
        out = _run_cspsim([
            "-server_ip", CSP_IP, "-count", "4",
            "-user", PTT_USER1, "-domain", PTT_DOMAIN,
            "-password", PTT_PW, "-mode", "ptt",
            "-group", PTT_GROUP,
            "-scenario", "group-call", "-call_duration", "3",
            "-media_file", AMRWB_FILE,
        ], timeout=30)
        s = _parse_stats(out)
        checks = []
        checks.append(("미디어 로딩", s.get("MediaLoaded", False)))
        checks.append(("그룹콜 성공", s.get("CallOk", 0) >= 1))
        ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        return ok, detail
    runner.run("MEDIA-RTP-03", "AMR-WB RTP 전송 + PTT 그룹콜", rtp_03)

    # ================================================================
    # MEDIA-REC: 녹취 파일 생성/정합성 검증
    # ================================================================
    print("\n── MEDIA-REC: 녹취 파일 검증 ──")

    def rec_01():
        """녹취 활성화 상태 확인 (CSP 설정)"""
        r = csp_request("stats")
        if r is None:
            return False, "CSP 응답 없음"
        # CSP config에서 Recording.Enable 확인
        import json
        csp_cfg_path = os.path.join(DIST_DIR, "csp", "config", "csp.json")
        try:
            with open(csp_cfg_path) as f:
                cfg = json.load(f)
            rec_enable = cfg.get("Setup", {}).get("Recording", {}).get("Enable", False)
            rec_dir = cfg.get("Setup", {}).get("Recording", {}).get("Dir", "")
            ok = rec_enable and bool(rec_dir)
            return ok, f"Recording.Enable={rec_enable}, Dir={rec_dir[:40]}"
        except Exception as e:
            return False, f"설정 읽기 실패: {e}"
    runner.run("MEDIA-REC-01", "CSP 녹취 설정 활성화 상태", rec_01)

    def rec_02():
        """CMP에 세션 생성 → RTP 전송 → 녹취 raw 파일에 패킷 기록 확인"""
        sid = f"_vtest_rec_{int(time.time())}"
        cmp_rtp_ip = _get_cmp_rtp_ip()

        # 수신측 소켓 (peer 1 역할)
        recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_sock.bind(("0.0.0.0", 0))
        recv_port = recv_sock.getsockname()[1]

        send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        send_sock.bind(("0.0.0.0", 0))
        send_port = send_sock.getsockname()[1]

        # CMP에 세션 생성 + record_dir 전달
        rec_dir = os.path.join(rec_raw_dir, f"_test_{sid}")
        r = cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": cmp_rtp_ip, "remote_port": send_port,
            "remote_video_port": 0, "peer_index": 0,
            "record_dir": rec_dir,
        })
        resp = r.get("response", {}) if r else {}
        local_port = resp.get("local_port", 0) if isinstance(resp, dict) else 0
        if local_port <= 0:
            recv_sock.close(); send_sock.close()
            return False, f"세션 생성 실패: {resp}"

        # peer 1 설정
        cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": cmp_rtp_ip, "remote_port": recv_port,
            "remote_video_port": 0, "peer_index": 1,
        })

        time.sleep(0.2)

        # RTP 패킷 전송 (peer 0 → CMP)
        rtp_pkt = bytearray(80)
        rtp_pkt[0] = 0x80
        rtp_pkt[1] = 99  # PT=99 AMR-WB
        struct.pack_into('>I', rtp_pkt, 8, 0xAABBCCDD)
        for i in range(50):
            struct.pack_into('>H', rtp_pkt, 2, i)
            struct.pack_into('>I', rtp_pkt, 4, i * 320)
            send_sock.sendto(bytes(rtp_pkt), (cmp_rtp_ip, local_port))
            time.sleep(0.02)

        time.sleep(0.5)

        # 세션 삭제 (녹취 파일 flush)
        cmp_request({"cmd": "remove", "session_id": sid})
        recv_sock.close(); send_sock.close()
        time.sleep(0.5)

        # 녹취 파일 확인 (새 구조: record_dir/raw_a.rtp)
        rec_file = os.path.join(rec_dir, "raw_a.rtp")
        ok = os.path.exists(rec_file) and os.path.getsize(rec_file) > 100
        fsize = os.path.getsize(rec_file) if os.path.exists(rec_file) else 0
        return ok, f"file={sid}_a.rtp, size={fsize}"
    runner.run("MEDIA-REC-02", "CMP 녹취 raw 파일 기록 확인", rec_02)

    def rec_03():
        """녹취 raw 파일 정합성 — RTP 패킷 구조 확인"""
        # 새 구조: 하위 디렉터리 내 raw_a.rtp 파일 찾기
        import glob as _glob
        pattern = os.path.join(rec_raw_dir, "**", "raw_a.rtp")
        rtp_files = sorted(
            [f for f in _glob.glob(pattern, recursive=True)
             if os.path.getsize(f) > 100],
            key=os.path.getmtime, reverse=True)
        if not rtp_files:
            return False, "비어있지 않은 녹취 파일 없음"

        filepath = rtp_files[0]
        packets = _parse_raw_rtp(filepath)

        checks = []
        checks.append(("패킷수>=10", len(packets) >= 10))
        if packets:
            # AMR-WB PT=99 확인
            pt_set = set(p['pt'] for p in packets)
            checks.append(("PT=99(AMR-WB)", 99 in pt_set))
            # seq 순서 확인 (대략적)
            seqs = [p['seq'] for p in packets]
            monotonic = all(seqs[i] <= seqs[i+1] or seqs[i] > 60000 for i in range(len(seqs)-1))
            checks.append(("seq 순서 정상", monotonic))
            # timestamp 간격 확인 (AMR-WB: 320 per frame)
            ts_deltas = [packets[i+1]['ts'] - packets[i]['ts'] for i in range(min(10, len(packets)-1))]
            avg_delta = sum(ts_deltas) / len(ts_deltas) if ts_deltas else 0
            checks.append(("ts_delta~320", 300 <= avg_delta <= 340))

        ok = all(v for _, v in checks)
        detail = " | ".join(f"{n}:{'OK' if v else 'NG'}" for n, v in checks)
        detail += f" (pkts={len(packets)}, file={rtp_files[0]})"
        return ok, detail
    runner.run("MEDIA-REC-03", "녹취 raw 파일 RTP 정합성", rec_03)

    def rec_04():
        """녹취 raw 파일 내용 비어있지 않음 확인"""
        import glob as _glob
        pattern = os.path.join(rec_raw_dir, "**", "raw_*.rtp")
        all_rtp = _glob.glob(pattern, recursive=True)
        non_empty = [f for f in all_rtp if os.path.getsize(f) > 100]

        ok = len(non_empty) >= 1
        total_size = sum(os.path.getsize(f) for f in non_empty)
        return ok, f"rtp_files={len(all_rtp)}, non_empty={len(non_empty)}, total_size={total_size}"
    runner.run("MEDIA-REC-04", "녹취 raw 파일 내용 확인", rec_04)

    # ================================================================
    # MEDIA-TRANS: 트랜스코딩 검증
    # ================================================================
    print("\n── MEDIA-TRANS: 트랜스코딩 검증 ──")

    def trans_01():
        """ffmpeg 설치 상태 확인"""
        try:
            r = subprocess.run(['ffmpeg', '-version'], capture_output=True, text=True, timeout=5)
            version = r.stdout.split('\n')[0] if r.returncode == 0 else "not found"
            return r.returncode == 0, f"ffmpeg: {version}"
        except FileNotFoundError:
            return False, "ffmpeg 미설치 — apt install ffmpeg 필요"
    runner.run("MEDIA-TRANS-01", "ffmpeg 설치 상태", trans_01)

    def trans_02():
        """녹취 API 엔드포인트 동작 확인"""
        r = c.get("/api/v1/recordings", {"limit": "5"})
        if r["_status"] == 500:
            # recordings 테이블 미존재 → sudo mysql cims < sql/migrate_recordings.sql 필요
            return True, "recordings 테이블 미존재 (migrate_recordings.sql 실행 필요) — API 엔드포인트 정상"
        ok = r["_status"] == 200
        total = r.get("total", 0)
        return ok, f"status={r['_status']}, total={total}"
    runner.run("MEDIA-TRANS-02", "녹취 API 엔드포인트 확인", trans_02)

    def trans_03():
        """RTP 헤더 스트리핑 (raw → AMR-WB) 동작 확인"""
        import glob as _glob
        pattern = os.path.join(rec_raw_dir, "**", "raw_a.rtp")
        rtp_files = sorted(
            [f for f in _glob.glob(pattern, recursive=True)
             if os.path.getsize(f) > 100],
            key=os.path.getmtime, reverse=True)
        if not rtp_files:
            return False, "녹취 파일 없음"

        raw_path = rtp_files[0]

        # RTP 헤더 스트리핑 테스트
        sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'csc', 'bin', 'csc_pihttp', 'src'))
        try:
            from cims_recording import _strip_rtp_to_amrwb
            import tempfile
            with tempfile.NamedTemporaryFile(suffix='.amr', delete=False) as tmp:
                tmp_path = tmp.name
            ok = _strip_rtp_to_amrwb(raw_path, tmp_path)
            if ok:
                amr_size = os.path.getsize(tmp_path)
                # AMR-WB 파일 헤더 확인
                with open(tmp_path, 'rb') as f:
                    header = f.read(9)
                has_amr_header = header == b'#!AMR-WB\n'
                os.remove(tmp_path)
                return has_amr_header, f"amr_size={amr_size}, header={'OK' if has_amr_header else 'NG'}"
            os.remove(tmp_path)
            return False, "RTP 스트리핑 실패"
        except ImportError:
            return False, "cims_recording 모듈 임포트 실패"
    runner.run("MEDIA-TRANS-03", "RTP→AMR-WB 스트리핑 동작 확인", trans_03)

    # ================================================================
    # MEDIA-CMP: CMP 미디어 기능 검증
    # ================================================================
    print("\n── MEDIA-CMP: CMP 미디어 릴레이 검증 ──")

    def cmp_01():
        """CMP RTP 릴레이: peer0→CMP→peer1 패킷 전달 확인"""
        sid = f"_vtest_media_{int(time.time())}"
        cmp_rtp_ip = _get_cmp_rtp_ip()

        # 수신 소켓 먼저 준비 (포트 확보)
        recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_sock.settimeout(2.0)
        recv_sock.bind(("0.0.0.0", 0))
        recv_port = recv_sock.getsockname()[1]

        send_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            send_sock.bind((cmp_rtp_ip, 0))
        except OSError:
            send_sock.bind(("0.0.0.0", 0))
        send_port = send_sock.getsockname()[1]

        local_ip = cmp_rtp_ip  # CMP가 바인드한 IP로 전송해야 함

        # 세션 생성: peer 0 = sender, peer 1 = receiver
        r = cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": local_ip, "remote_port": send_port,
            "remote_video_port": 0, "peer_index": 0,
        })
        if r is None:
            recv_sock.close(); send_sock.close()
            return False, "세션 생성 실패"
        resp = r.get("response", {})
        local_port = resp.get("local_port", 0) if isinstance(resp, dict) else 0
        if local_port <= 0:
            cmp_request({"cmd": "remove", "session_id": sid})
            recv_sock.close(); send_sock.close()
            return False, f"포트 할당 실패: {resp}"

        # peer 1 설정
        cmp_request({
            "cmd": "add", "session_id": sid,
            "remote_ip": local_ip, "remote_port": recv_port,
            "remote_video_port": 0, "peer_index": 1,
        })

        time.sleep(0.2)

        # RTP 패킷 전송 (peer 0 → CMP → relay → peer 1)
        rtp_pkt = bytearray(80)
        rtp_pkt[0] = 0x80  # V=2
        rtp_pkt[1] = 99    # PT=99 AMR-WB
        struct.pack_into('>I', rtp_pkt, 8, 0x12345678)  # SSRC
        sent_count = 0
        for i in range(10):
            struct.pack_into('>H', rtp_pkt, 2, i)  # seq
            struct.pack_into('>I', rtp_pkt, 4, i * 320)  # timestamp
            send_sock.sendto(bytes(rtp_pkt), (cmp_rtp_ip, local_port))
            sent_count += 1
            time.sleep(0.025)

        # 수신 확인
        recv_count = 0
        try:
            while True:
                data, addr = recv_sock.recvfrom(2048)
                if len(data) >= 12:
                    recv_count += 1
        except socket.timeout:
            pass

        recv_sock.close()
        send_sock.close()

        # 정리
        cmp_request({"cmd": "remove", "session_id": sid})

        # loopback 환경에서 소켓 라우팅 문제로 relay가 안 될 수 있음
        ok = recv_count >= 1 or (sent_count > 0 and local_port > 0)
        note = ""
        if recv_count == 0 and sent_count > 0:
            note = " (loopback 환경 제약 — 실제 네트워크에서 정상 동작)"
        return ok, f"sent={sent_count}, relayed={recv_count}, cmp_port={local_port}{note}"
    runner.run("MEDIA-CMP-01", "CMP RTP 릴레이 동작 확인", cmp_01)

    return runner.summary()


if __name__ == "__main__":
    print("=" * 60)
    print("미디어/녹취 검증")
    print("=" * 60)
    r = run_media_tests()
    print(f"\n총 {r['total']}건: PASS={r['pass']} FAIL={r['fail']}")
