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

콜백은 코어 스레드(`ue-ctl`)에서 오며 파사드가 `SynchronizationContext.Post` 로 앱 스레드에 넘긴다. WPF `Dispatcher` 는
앱(`windows/dispatch-desktop`)만 안다.
