# sdk/windows — Windows SDK (cimsue.dll + 헤더)

설계 정본은 [docs/design/features/ue_sdk.md](../../docs/design/features/ue_sdk.md) §6 이다. 이 디렉터리는
**슈퍼빌드**(`CMakeLists.txt`)와 MSVC 용 의존성 래퍼(`deps/`)만 둔다 — 코어 소스는 `sdk/core`, 엔진은 `ext/pjproject`,
config_site 는 `sdk/engine/config_site/windows.h` 가 정본이며 여기에 복사본을 두지 않는다.

```
sdk/windows/
  CMakeLists.txt              슈퍼빌드: AMR-WB(deps) → pjproject(자체 CMake) → sdk/core(CIMSUE_SHARED) → sdk/ 레이아웃
  deps/opencore-amrwb/        ext/opencore-amr 의 MSVC CMake 래퍼 (디코더)
  deps/vo-amrwbenc/           ext/vo-amrwbenc-0.1.3 의 MSVC CMake 래퍼 (인코더)
  platform/                   Windows 접점 (F2 — 장치 열거/핫플러그 통지·전역 핫키·DPAPI 자격 저장·단일 인스턴스). UI 프레임워크 無
```

## 요구

- Visual Studio 2022 (MSVC v143, x64), CMake ≥ 3.28 (pjproject CMake 요구), Python 3 (floor_defs 생성)
- OpenSSL: vcpkg `openssl` (`-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake`) 또는 설치본을 `CMAKE_PREFIX_PATH` 로

## 빌드

```
cmake -S sdk/windows -B build-win -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build-win --config Release
build-win\sdk\bin\cimsue-cli.exe --server <CSP> --port 5060 --domain <D> --msisdn <M> --imsi <I> --ha1 <HEX> register
```

산출물 `build-win/sdk/{bin,lib,include}` 가 배포 zip 의 원본이다(`.dll`/`.lib` 은 커밋하지 않는다).

## 상태

F1(엔진 + `cimsue-cli` 실빌드) 전 — Linux 서버에서 작성한 설계 골격이다. F1 에서 확정할 항목은 슈퍼빌드 파일 서두와
ue_sdk.md §10 F 를 본다. pjproject CMake 경로가 막히면 폴백은 `pjproject-vs14.sln`(같은 트리·같은 config_site)이며,
그 경우 `PJ_LIBS`/`PJ_LIB_PATH` 를 `-D` 로 넘겨 코어만 이 CMake 로 빌드한다.
