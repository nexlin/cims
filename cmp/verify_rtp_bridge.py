"""CMP 1:1 relay 스모크 — leg 별 전용 포트 모델 (docs/api/cmp_media_api.md §6).

한 세션에 peer0(A)/peer1(B)을 등록하고 양방향 relay 를 확인한다:
  A 는 자기 전용 포트(local_port)로, B 는 local_port_b 로 송신 —
  수신 포트가 곧 peer 신원이므로 소스 주소 매칭이 없다.
CMP 가 127.0.0.1 에서 기동 중이어야 한다 (기본 제어 포트 9000).
"""

import json
import socket
import sys

CMP_IP = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
CMP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 9000


def local_ip_towards(dest_ip):
    """dest 로 나갈 때 쓰이는 로컬 소스 IP — relay 의 선언 주소 검증에 필요."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((dest_ip, 9))
    ip = s.getsockname()[0]
    s.close()
    return ip


MY_IP = local_ip_towards(CMP_IP)

CLIENT_A_RTP_PORT = 20000
CLIENT_B_RTP_PORT = 20002

RTP_PAYLOAD = b"\x80\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01" + b"\x00" * 20


def cmp_request(cmp_sock, trans_id, cmd, payload):
    req = {
        "hdr": {"ver": 2, "trans_id": trans_id, "node": "verify", "cmd": cmd,
                "type": "request", "sesid": "verify::cmp::0::1", "service": "volte"},
        "payload": payload,
    }
    cmp_sock.sendto(json.dumps(req).encode(), (CMP_IP, CMP_PORT))
    data, _ = cmp_sock.recvfrom(4096)
    resp = json.loads(data)
    print(f"{cmd} resp:", resp)
    assert resp["hdr"]["status"] == "OK", f"{cmd} failed"
    return resp.get("payload") or {}


def recv_rtp(sock, label):
    sock.settimeout(3)
    try:
        data, addr = sock.recvfrom(1024)
        print(f"[{label}] Received {len(data)} bytes from {addr}")
        return True
    except socket.timeout:
        print(f"[{label}] Timeout waiting for RTP")
        return False


def test_cmp_bridge():
    cmp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    cmp_sock.settimeout(3)

    # 한 세션: peer0 = A, peer1 = B
    body = cmp_request(cmp_sock, 1, "RELAY_ADD", {
        "session_id": "verify_bridge",
        "remote_ip": MY_IP, "remote_port": CLIENT_A_RTP_PORT,
        "peer_index": 0,
    })
    port_a = int(body["local_port"])        # A 전용 수신 포트
    port_b = int(body["local_port_b"])      # B 전용 수신 포트

    cmp_request(cmp_sock, 2, "RELAY_MODIFY", {
        "session_id": "verify_bridge",
        "remote_ip": MY_IP, "remote_port": CLIENT_B_RTP_PORT,
        "peer_index": 1,
    })

    sock_a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_a.bind(("0.0.0.0", CLIENT_A_RTP_PORT))
    sock_b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock_b.bind(("0.0.0.0", CLIENT_B_RTP_PORT))

    ok = True

    print(f"Testing A(:{CLIENT_A_RTP_PORT}) -> CMP:{port_a} -> B(:{CLIENT_B_RTP_PORT})...")
    sock_a.sendto(RTP_PAYLOAD, (CMP_IP, port_a))
    if recv_rtp(sock_b, "Client B"):
        print("SUCCESS: A -> B bridged")
    else:
        print("FAIL: A -> B not bridged")
        ok = False

    print(f"Testing B(:{CLIENT_B_RTP_PORT}) -> CMP:{port_b} -> A(:{CLIENT_A_RTP_PORT})...")
    sock_b.sendto(RTP_PAYLOAD, (CMP_IP, port_b))
    if recv_rtp(sock_a, "Client A"):
        print("SUCCESS: B -> A bridged")
    else:
        print("FAIL: B -> A not bridged")
        ok = False

    cmp_request(cmp_sock, 3, "RELAY_REMOVE", {"session_id": "verify_bridge"})

    sock_a.close()
    sock_b.close()
    cmp_sock.close()
    return ok


if __name__ == "__main__":
    sys.exit(0 if test_cmp_bridge() else 1)
