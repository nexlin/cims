# sdk/windows — Windows SDK (cimsue.dll + C API + .NET 파사드)

설계 정본은 [docs/design/features/ue_sdk.md](../../docs/design/features/ue_sdk.md) §6 이다. 이 디렉터리는
**슈퍼빌드**(`CMakeLists.txt`)·MSVC 용 의존성 래퍼(`deps/`)·**.NET 파사드**(`dotnet/`)만 둔다 — 코어 소스는 `sdk/core`,
엔진은 `ext/pjproject`, config_site 는 `sdk/engine/config_site/windows.h` 가 정본이며 여기에 복사본을 두지 않는다.

```
sdk/windows/
  CMakeLists.txt              슈퍼빌드: AMR-WB(deps) → pjproject(자체 CMake) → sdk/core(CIMSUE_SHARED) → sdk/ 레이아웃
  deps/opencore-amrwb/        ext/opencore-amr 의 MSVC CMake 래퍼 (디코더)
  deps/vo-amrwbenc/           ext/vo-amrwbenc-0.1.3 의 MSVC CMake 래퍼 (인코더)
  dotnet/                     .NET 파사드 CimsUe (C#, P/Invoke → cimsue_c.h) + Windows 접점(관리 코드) — F2. UI 프레임워크 無
```

앱(`windows/dispatch-desktop`, WPF) 은 `CimsUe.dll` 만 참조한다. 네이티브 `cimsue.dll` 은 C++ 클래스(`cimsue-cli`·단위시험용)와
C API(`cimsue_c.h`, 파사드용)를 함께 export 한다(ue_sdk.md §6.4).

## 요구 (개발 머신)

| 도구 | 용도 | 설치 |
|---|---|---|
| Visual Studio 2022 Build Tools — 워크로드 **C++ 데스크톱**(MSVC v143 x64, Windows SDK) + **.NET 데스크톱 빌드 도구** | 네이티브 슈퍼빌드 / .NET 파사드·WPF 앱 빌드 | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Workload.ManagedDesktopBuildTools --includeRecommended"` (IDE 가 필요하면 Community/Professional 에 같은 워크로드) |
| CMake ≥ 3.28 | pjproject CMake 요구 | `winget install Kitware.CMake` |
| Python 3 | `scripts/gen_floor_defs.py` (floor_defs.h 재생성). `find_package(Python3)` 로 잡으므로 `python3` 이름 불필요 | `winget install Python.Python.3.12 --scope user` |
| .NET SDK 10 (LTS) | `dotnet/CimsUe`·WPF 앱 (`net10.0-windows`) | `winget install Microsoft.DotNet.SDK.10` |
| vcpkg + `openssl:x64-windows` | SIP TLS·SRTP·코어 HTTPS 가 쓰는 OpenSSL (레포 vendoring 대상 아님) | `git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg && C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics`, 사용자 환경변수 `VCPKG_ROOT=C:\dev\vcpkg`, `vcpkg install openssl:x64-windows` |

OpenSSL 은 vcpkg 대신 설치본을 `-DCMAKE_PREFIX_PATH=<openssl 설치 경로>` 로 줘도 된다.

## 빌드

```
cmake -S sdk/windows -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build-win --config Release
build-win\sdk\bin\cimsue-cli.exe --server <CSP> --port 5060 --domain <D> --msisdn <M> --imsi <I> --ha1 <HEX> register
dotnet build sdk/windows/dotnet/CimsUe -c Release          (F2 — 파사드)
```

산출물 `build-win/sdk/{bin,lib,include}` 가 배포 zip 의 원본이다(`.dll`/`.lib` 은 커밋하지 않는다). 파사드 NuGet 패키지는
`cimsue.dll` 을 `runtimes/win-x64/native/` 로 동봉한다.

## 상태

F1 빌드 확정 — VS 2022 17.14(MSVC 14.44)·CMake 4.4·vcpkg openssl 3.6 에서 슈퍼빌드가 `cimsue.dll`·`cimsue-cli.exe` 까지
통과하고 `cimsue-cli --help` 가 DLL 을 로드해 실행된다. 확정된 사실은 슈퍼빌드 파일 서두와 ue_sdk.md §6.1 에 있다
(pjproject CMake 통과 → `pjproject-vs14.sln` 폴백 불필요). 남은 F1 항목은 S3 시나리오(등록·1:1 TLS+SRTP·그룹콜 floor·Join)의
Windows 실측과 WMME 장치 열거 실측. `dotnet/`(C API·.NET 파사드) 은 F2.
