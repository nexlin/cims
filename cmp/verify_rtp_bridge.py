
import socket
import json
import time
import threading
import sys

# Configuration
CMP_IP = "127.0.0.1"
CMP_PORT = 9000
CSP_IP = "127.0.0.1"
CSP_PORT = 5060

# RTP Ports (Simulated Clients)
CLIENT_A_RTP_PORT = 20000
CLIENT_B_RTP_PORT = 20002

def create_rtp_socket(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    return sock

def send_rtp(sock, dest_ip, dest_port, payload=b'\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'):
    sock.sendto(payload, (dest_ip, dest_port))
    print(f"Sent RTP to {dest_ip}:{dest_port}")

def recv_rtp(sock, label):
    sock.settimeout(5)
    try:
        data, addr = sock.recvfrom(1024)
        print(f"[{label}] Received {len(data)} bytes from {addr}")
        return True
    except socket.timeout:
        print(f"[{label}] Timeout waiting for RTP")
        return False

def test_cmp_bridge():
    # 1. Simulate CMP control to allocate sessions (bypassing CSP for isolation test)
    # Actually, we should test CSP-CMP integration.
    # But let's assume we want to verify if CMP forwards packets if we manually set it up.
    
    cmp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # Add Session A (envelope v2 — docs/api/cmp_media_api.md)
    req_a = {
        "hdr": {"ver": 2, "trans_id": 1, "node": "verify", "cmd": "RELAY_ADD",
                "type": "request", "service": "volte"},
        "payload": {
            "session_id": "sess_a",
            "remote_ip": "127.0.0.1",
            "remote_port": CLIENT_A_RTP_PORT,
            "remote_video_port": 0,
            "peer_index": 0
        }
    }
    cmp_sock.sendto(json.dumps(req_a).encode(), (CMP_IP, CMP_PORT))
    data, _ = cmp_sock.recvfrom(4096)
    resp_a = json.loads(data)
    print("Session A Resp:", resp_a)
    port_a_loc = int(resp_a['payload']['local_port'])

    # Add Session B
    req_b = {
        "hdr": {"ver": 2, "trans_id": 2, "node": "verify", "cmd": "RELAY_ADD",
                "type": "request", "service": "volte"},
        "payload": {
            "session_id": "sess_b",
            "remote_ip": "127.0.0.1",
            "remote_port": CLIENT_B_RTP_PORT,
            "remote_video_port": 0,
            "peer_index": 0
        }
    }
    cmp_sock.sendto(json.dumps(req_b).encode(), (CMP_IP, CMP_PORT))
    data, _ = cmp_sock.recvfrom(4096)
    resp_b = json.loads(data)
    print("Session B Resp:", resp_b)
    port_b_loc = int(resp_b['payload']['local_port'])
    
    # Setup Sockets
    sock_a = create_rtp_socket(CLIENT_A_RTP_PORT)
    sock_b = create_rtp_socket(CLIENT_B_RTP_PORT)
    
    # Test A -> B (Should fail if no bridge)
    print(f"Testing A({port_a_loc}) -> B({port_b_loc})...")
    send_rtp(sock_a, CMP_IP, port_a_loc)
    if recv_rtp(sock_b, "Client B"):
        print("SUCCESS: A -> B Bridged")
    else:
        print("FAIL: A -> B Not Bridged")
        
    # Now Try "Bridge" command if we implement it.
    # ...
    
    sock_a.close()
    sock_b.close()

if __name__ == "__main__":
    test_cmp_bridge()
