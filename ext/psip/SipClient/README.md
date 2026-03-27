# SipClient

A SIP Client application for testing Call and MCPTT (PTT) scenarios.

## Build

The application is built using CMake as part of the `psip` project or the main `cims` build.

```bash
cd /home/nex/work/cims/build
cmake ..
make SipClient
```

The executable will be located in `bin/SipClient` (or `ext/psip/SipClient/SipClient.exe` depending on build context, usually `bin/` in top level).

## Usage

Run the application with command-line arguments. XML configuration is no longer used.

```bash
./SipClient [-local_ip <LOCAL_IP>] [-local_port <LOCAL_PORT>] [-server_ip <SERVER_IP>] [-server_port <SERVER_PORT>] [-user <USER_ID>] [-mode <call|ptt>]
```

### Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `-local_ip` | IP address to bind SIP/RTP to. | Auto-detected (UDP connect) |
| `-local_port` | Local SIP listening port. | Random (10000-30000) |
| `-server_ip` | SIP Server (CSP) IP address. | `127.0.0.1` |
| `-server_port` | SIP Server (CSP) port. | `5060` |
| `-user` | SIP User ID / Extension. | `1000` |
| `-domain` | SIP Domain. | `csp` |
| `-password` | SIP Password. | `1234` |
| `-mode` | Operation mode: `call` (default) or `ptt`. | `call` |

---

## Modes

### 1. Call Mode (Default)
Standard VoIP call mode. Supports placing and receiving calls.

**Command Line:**
```bash
./SipClient -local_ip 192.168.1.10 -user 1001 -server_ip 192.168.0.9
./SipClient -user 1001 -server_ip 192.168.0.9 
./SipClient -user 1001 -server_ip 192.168.0.9 -server_port 5060
./SipClient -user 1002 -server_ip 192.168.0.9 -server_port 5060


```

**Console Commands:**
- `c <dest_user>` : Call a user (e.g., `c 1002`).
- `a` : Accept incoming call (Manual answer).
- `e` or `s` : End call / Stop ringing.
- `m <dest_user>` : Send MESSAGE (SMS).
- `i` : Print SIP Stack info.
- `q` : Quit.

### 2. PTT Mode (Auto-Answer & Floor Control)
MCPTT Client simulation mode.
- **Auto-Answer**: Automatically answers incoming calls (INVITE) with 200 OK immediately.
- **Floor Control**: Sends RTCP APP packets for Floor Request/Release.

**Command Line:**
```bash
./SipClient -local_ip 192.168.1.10 -user 2000 -server_ip 192.168.1.5 -mode ptt
```

**Console Commands:**
- `t` : **Talk Request**. Sends RTCP Floor Request (OpCode 1).
- `r` : **Release**. Sends RTCP Floor Release (OpCode 4).
- `e` or `s` : End call.
- `i` : Print SIP Stack info.
- `q` : Quit.

## Troubleshooting
- **ALSA Error**: The client is built with `NO_ALSA` defined, so audio device errors are suppressed, and dummy audio is used.
- **Library Load Error**: Ensure `LD_LIBRARY_PATH` includes the directory containing `libSipStack.so` etc., if built as shared. (Currently built as static, so single binary).
