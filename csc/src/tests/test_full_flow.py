import socket
import time
import requests
import uuid
import threading
import sys

# Configuration
CSP_IP = "192.168.0.73"
CSP_SIP_PORT = 5060
CSC_HTTP_URL = "https://127.0.0.1:4420"

# SIP Client Config
CLIENT_IP = "192.168.0.73"
CLIENT_PORT = 5090
USER_ID = "1000"
USER_URI = f"sip:{USER_ID}@{CSP_IP}:{CSP_SIP_PORT}"
GROUP_URI = "tel:+9000"

sip_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sip_sock.bind((CLIENT_IP, CLIENT_PORT))

def generate_call_id():
    return str(uuid.uuid4())

def current_time_ms():
    return int(time.time() * 1000)

def send_sip(msg):
    # print(f"\n[SIP SEND] >>>>>>>>>>\n{msg}\n<<<<<<<<<<")
    sip_sock.sendto(msg.encode('utf-8'), (CSP_IP, CSP_SIP_PORT))

def listener():
    print(f"[*] SIP Client Listening on {CLIENT_IP}:{CLIENT_PORT}...")
    while True:
        try:
            data, addr = sip_sock.recvfrom(65535)
            msg = data.decode('utf-8')
            # print(f"\n[SIP RECV] <<<<<<<<<<\n{msg}\n>>>>>>>>>>")
            
            if "NOTIFY" in msg:
                print("\n[SUCCESS] Received SIP NOTIFY!")
                print("-" * 40)
                print(msg)
                print("-" * 40)
                # Send 200 OK
                cseq = extract_header(msg, "CSeq")
                call_id = extract_header(msg, "Call-ID")
                from_h = extract_header(msg, "From")
                to_h = extract_header(msg, "To")
                via = extract_header(msg, "Via")
                
                resp = f"SIP/2.0 200 OK\r\nVia: {via}\r\nFrom: {from_h}\r\nTo: {to_h}\r\nCall-ID: {call_id}\r\nCSeq: {cseq}\r\nContent-Length: 0\r\n\r\n"
                send_sip(resp)
                
            elif "200 OK" in msg:
                cseq = extract_header(msg, "CSeq")
                if "REGISTER" in cseq:
                    print("[*] Registration Successful (200 OK)")
                elif "SUBSCRIBE" in cseq:
                    print("[*] Subscription Successful (200 OK)")

        except Exception as e:
            print(f"Printer Error: {e}")

def extract_header(msg, header):
    for line in msg.split("\r\n"):
        if line.lower().startswith(header.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return ""

def register():
    call_id = generate_call_id()
    msg = f"""REGISTER sip:{CSP_IP} SIP/2.0
Via: SIP/2.0/UDP {CLIENT_IP}:{CLIENT_PORT};branch=z9hG4bK-{current_time_ms()}
Max-Forwards: 70
To: <{USER_URI}>
From: <{USER_URI}>;tag={current_time_ms()}
Call-ID: {call_id}
CSeq: 1 REGISTER
Contact: <sip:{USER_ID}@{CLIENT_IP}:{CLIENT_PORT}>
Expires: 3600
Content-Length: 0

"""
    print(f"[*] Sending REGISTER for {USER_URI}...")
    # Normalize line endings
    msg = msg.replace("\n", "\r\n")
    send_sip(msg)

def subscribe():
    call_id = generate_call_id()
    msg = f"""SUBSCRIBE {GROUP_URI} SIP/2.0
Via: SIP/2.0/UDP {CLIENT_IP}:{CLIENT_PORT};branch=z9hG4bK-{current_time_ms()}
Max-Forwards: 70
To: <{GROUP_URI}>
From: <{USER_URI}>;tag={current_time_ms()}
Call-ID: {call_id}
CSeq: 2 SUBSCRIBE
Event: xcap-diff
Expires: 3600
Contact: <sip:{USER_ID}@{CLIENT_IP}:{CLIENT_PORT}>
Accept: application/xcap-diff+xml
Content-Length: 0

"""
    print(f"[*] Sending SUBSCRIBE for {GROUP_URI}...")
    msg = msg.replace("\n", "\r\n")
    send_sip(msg)

def trigger_csc_change():
    print(f"[*] Triggering CSC Group Change (PUT {GROUP_URI})...")
    # Need ID and Access Token.
    # We will simulate the same requests as test_csc_http.py but just the group PUT part
    # Assuming the server allows it or we reuse the logic.
    # Actually, simpler to just run the test_csc_http.py script or reuse its token logic?
    # Let's perform a raw request if possible, or just call the python script subprocess?
    # Subprocess is easier to ensure Auth is handled if we don't want to duplicate token logic.
    
    import subprocess
    # We'll rely on the user running test_csc_http.py separately or we invoke it.
    # Let's try to invoke it.
    print("[*] invoking test_csc_http.py to trigger change...")
    subprocess.run(["python3", "test_csc_http.py"], check=False, stdout=subprocess.DEVNULL)
    print("[*] CSC Triggered.")

if __name__ == "__main__":
    t = threading.Thread(target=listener)
    t.daemon = True
    t.start()

    # 1. Register
    register()
    time.sleep(1)

    # 2. Subscribe
    subscribe()
    time.sleep(1)

    # 3. Trigger Change
    trigger_csc_change()

    # Wait for Notify
    time.sleep(5)
