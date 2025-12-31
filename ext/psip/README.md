# PSIP Linux Build Guide

This directory (`ext/psip`) contains the core SIP/Call processing libraries and various utility servers.
A master `CMakeLists.txt` has been added to support building these components on Linux (e.g., WSL).

## Prerequisites

Before building, ensure the following development tools and libraries are installed on your Linux system (e.g., Ubuntu/WSL).

### 1. Install System Dependencies
Run the following commands to install CMake, GCC/G++, OpenSSL, and build tools:

```bash
sudo apt update
sudo apt install -y cmake g++ libssl-dev autoconf automake libtool
```

- **CMake**: Build system configuration (Version 3.10+).
- **G++**: C++ Compiler (GCC 13 recommended).
- **OpenSSL**: Required for cryptographic functions (`libssl-dev`).
- **Autotools**: Required for building dependencies like ALSA (`autoconf`, `automake`, `libtool`).

### 2. Build ALSA Dependency (Required for SipClient)
The `SipClient` target depends on a static ALSA library (`libasound.a`). If it is not present in `ext/alsa-lib-build`, you must build it from source:

```bash
cd ../alsa-lib-1.2.10

# 1. Configure
autoreconf -fvi
./configure --prefix=$(pwd)/../alsa-lib-build --enable-static --disable-shared

# 2. Build and Install
make -j $(nproc)
make install

# Return to psip directory
cd ../psip
```

## Building (Master Build)

To build all portable libraries and tools at once:

```bash
# 1. Create a build directory (e.g., build_master)
cmake -S . -B build_master

# 2. Compile (parallel build)
cmake --build build_master -j $(nproc)
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
  - `SipCallDump`, `SipClientPcapRtp` (requires `libpcap-dev`) - *Can be enabled if dependencies installed*

## Integration
Projects like `csp` link against these libraries by adding this directory via `add_subdirectory`.
