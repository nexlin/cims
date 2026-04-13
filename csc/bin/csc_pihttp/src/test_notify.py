import socket
import json
import time

def notify_csp(event_type, uri, action, etag=""):
    try:
        data = {
            "event": event_type,
            "uri": uri,
            "action": action,
            "etag": etag
        }
        msg = json.dumps(data)
        print(f"Sending: {msg}")
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(1.0) 
        sock.connect(('127.0.0.1', 4421))
        sock.sendall(msg.encode('utf-8'))
        sock.close()
        print("Sent successfully")
    except Exception as e:
        print(f"Failed: {e}")

notify_csp("GROUP_CHANGED", "tel:+8888", "PUT", "test_script")
