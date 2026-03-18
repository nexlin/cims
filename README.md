# CIMS Project Build Guide

## Overview
This project uses CMake for build configuration. External dependencies such as FFmpeg, oneTBB, and AMR codecs are configured to be automatically downloaded and built using CMake's `ExternalProject_Add`.

## Prerequisites
Ensure the following tools are installed on your build server:
- **CMake** (version 2.8 or higher)
- **GCC/G++** (support for C++11)
- **Git** (for fetching external dependencies)
- **Make**

## Build Instructions
0. **Install Prerequisites**
   ```bash
   sudo apt-get update
   sudo apt-get install -y cmake build-essential libssl-dev
   ```

1. **Clone the repository**
   ```bash
   git clone <repository_url>
   cd cims
   ```

2. **Create a build directory** (build 디렉토리 내에서 빌드)
   It is recommended to do an out-of-source build.
   ```bash
   mkdir build
   cd build
   ```

3. **Configure the project**(build 디렉토리 내에서 빌드)
   Run cmake to generate the makefiles. This step configures the external projects but does not yet build them.
   ```bash
   cmake ..
   ```

4. **Build**(build 디렉토리 내에서 빌드)
   Run make to start the build process. This will automatically download and compile the external dependencies (FFmpeg, etc.) before building the main project.
   ```bash
   make -j$(nproc)
   ```
   *(Note: The first build may take some time as it downloads and compiles external libraries.)*

## Deployment (build 디렉토리 내 배포용 디렉토리 생성 및 복사)
After a successful build, a distribution package can be created using the `dist` target:
```bash
make dist
```
This will create a `dist/` directory in your build folder containing the binaries and configuration files ready for deployment.


5. ** 실행 **  (build/dist/)
5.1 ** 호처리서버(csp) **
 * foreground 실행 
``` bash
./csp ../config/csp.json -n 
```
 * background 실행/종료/상태조회
``` bash
 ./csp.sh start 
```
 * 종료
``` bash
 ./csp.sh stop
```
 * 상태조회
 ``` bash
./csp.sh status
```

5.2 ** 미디어처리서버(cmp)
``` bash
 ./cmp ../config/cmp.json
```


6. 기타 설정
6.1 git 설정
``` bash
 git config --global user.name "nexlin"
 git config --global user.email "jcryu74@gmail.com"

# github 에서 Token 생성 후 아래와 같이 입력
``` bash
 git config --global credential.helper store
 git push 
 # 비밀번호에 token 입력

```