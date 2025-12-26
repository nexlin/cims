# PSIP Linux Build Guide

This directory (`ext/psip`) contains the core SIP/Call processing libraries and various utility servers.
A master `CMakeLists.txt` has been added to support building these components on Linux (e.g., WSL).

## Prerequisites

- CMake 3.10+
- GCC/G++ (supporting C++11)
- OpenSSL Development Libraries (`libssl-dev`)
- Pthreads (usually standard)

## Building (Master Build)

To build all portable libraries and tools at once:

```bash
# 1. Create a build directory (e.g., build_master)
wsl cmake -S . -B build_master

# 2. Compile (parallel build)
wsl cmake --build build_master -j 8
```

## Build Targets

### Core Libraries
- `SipStack`: Main SIP protocol stack
- `SipPlatform`: Platform abstraction layer (shim)
- `SipParser`, `SdpParser`, `StunParser`, `HttpParser`, `XmlParser`
- `TcpStack`, `HttpStack`, `ServerPlatform`
- `opensrtp`: Secure RTP library (built internally with specific exclusions)
- `SipUserAgent`: User Agent library

### Portable Executables
- `SimpleSipServer`: Basic SIP server implementation
- `EchoSipServer`: SIP Echo server for testing
- `SipLoadBalancer`: SIP Load Balancer
- `SipSpeedLinux`: Performance testing tool (Linux ported)
- `TestWebRtcVideo`: WebRTC Video test tool (requires `openh264`)

### Excluded Targets
The following are currently excluded from the default Linux build:
- **MFC Applications**: `SipSend`, `ServerMonitor` (Windows GUI dependent)
- **System Dependencies**: 
  - `SipClient` (requires `libasound2-dev` / ALSA) - *Can be enabled if dependencies installed*
  - `SipCallDump`, `SipClientPcapRtp` (requires `libpcap-dev`) - *Can be enabled if dependencies installed*

## Integration
Projects like `csp` link against these libraries by adding this directory via `add_subdirectory`.
