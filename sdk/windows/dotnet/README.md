# sdk/windows/dotnet — .NET 파사드 `CimsUe` + Windows 접점 (F2)

설계 정본은 [docs/design/features/ue_sdk.md](../../../docs/design/features/ue_sdk.md) §6.4 이다. Android 의
SWIG Java 바인딩 ↔ Kotlin 파사드에 대응하는 Windows 의 두 층을 한 C# 클래스 라이브러리(`CimsUe.dll`, `net10.0-windows`)로 둔다.
코어(`sdk/core`)가 모르는 Windows 전용 책임만 두며, 프로토콜·SIP·RTP 는 이 층에 없다(ue_sdk.md §1 경계 규칙 3).

```
dotnet/
  CimsUe/
    CimsUe.csproj             net10.0-windows, AllowUnsafeBlocks. WPF 참조 없음(UI 프레임워크 無)
    Native/NativeMethods.cs   internal — cimsue_c.h 의 P/Invoke(LibraryImport), SafeHandle, 콜백 델리게이트 고정
    Engine.cs Account.cs Call.cs Group.cs Subscriptions.cs CscClient.cs   공개 파사드 — Kotlin 파사드와 같은 모델
    Platform/                 Windows 접점(아래 표)
  CimsUe.Tests/               파사드 단위시험 (P/Invoke 마샬링·콜백 수명 — S1-UE-UNIT 의 .NET 축)
```

| 접점 모듈 | 책임 | 코어 API 대응 |
|---|---|---|
| `AudioEndpoints` | `IMMDeviceEnumerator` COM interop 으로 재생/캡처 엔드포인트 이름·기본 장치 표시, `IMMNotificationClient` 핫플러그 통지 → 코어 재열거 | `Engine.AudioDevices` · `RefreshAudioDevices` · `SetAudioDevices` · `AddPlaybackRoute` |
| `HotKeys` | `RegisterHotKey` 전역 PTT/응답/끊기 키(메시지 전용 HWND), 키 down/up → floor request/release | `FloorRequest` · `FloorRelease` · `Answer` · `Hangup` |
| `CredentialStore` | DPAPI(`ProtectedData`) 로 PKCE 토큰·H(A1) 저장 | `CscClient` 토큰 · `AccountConfig.Ha1` |
| `SingleInstance` | 명명 Mutex + 두 번째 실행 시 창 활성화 | — |
| `AutoStart` | `HKCU\...\Run` 등록 | — |

콜백은 코어 **이벤트 스레드**에서 온다(`cimsue_listener_t` — ue_sdk.md §6.4, 콜백 인자 문자열은 콜백 동안만 유효하므로 파사드가
관리 문자열로 복사한다). 파사드가 `SynchronizationContext.Post` 로 앱 스레드에 넘기며, WPF `Dispatcher` 는 앱(`windows/dispatch-desktop`)만
안다. 네이티브 표면은 `sdk/core/include/cimsue/cimsue_c.h`(구현 완료 — `cimsue.dll` export, `cimsue_test` 로 검증) 하나다.

**네이티브 DLL 배치**: 관리 어셈블리 `CimsUe.dll` 과 네이티브 `cimsue.dll` 은 Windows 에서 같은 이름이라 한 디렉터리에 둘 수 없다(P/Invoke 가 관리
DLL 을 열어 `EntryPointNotFound`). 빌드 출력·NuGet 패키지 모두 `runtimes/win-x64/native/`(cimsue.dll + OpenSSL 런타임 둘)에 두고, `NativeLoader` 가
`CIMSUE_NATIVE_DIR` → `runtimes/win-x64/native` → 앱 디렉터리 순으로 찾는다. 원본은 `Directory.Build.props` 의 `CimsUeNativeDir`(기본 `build-win/sdk/bin`).

**시험**: `dotnet test CimsUe.Tests` — 50건(ABI 레이아웃 27 구조체 대조 `cimsue_struct_size`·헤드리스 엔진 수명·SynchronizationContext 마샬링·프로파일 파싱·
접점). xunit 은 `SynchronizationContext.Current` 를 두므로 "이벤트 스레드 직접 수신" 시험은 컨텍스트를 명시적으로 비운다.
