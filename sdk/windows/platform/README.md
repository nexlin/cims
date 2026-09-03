# sdk/windows/platform — Windows 접점 (F2)

코어(`sdk/core`)가 모르는 Windows 전용 책임만 둔다. 프로토콜·SIP·RTP 는 이 층에 없다(ue_sdk.md §1 경계 규칙 3).

| 모듈(예정) | 책임 | 코어 API 대응 |
|---|---|---|
| `AudioEndpoints` | `IMMDeviceEnumerator` 로 재생/캡처 엔드포인트 이름·기본 장치 표시, `IMMNotificationClient` 핫플러그 통지 → 코어 재열거 | `Engine::audioDevices` · `refreshAudioDevices` · `setAudioDevices` · `addPlaybackRoute` |
| `HotKeys` | `RegisterHotKey` 전역 PTT/응답/끊기 키, 키 down/up → floor request/release | `floorRequest` · `floorRelease` · `answer` · `hangup` |
| `CredentialStore` | DPAPI(`CryptProtectData`) 로 PKCE 토큰·H(A1) 저장 | `CscClient` 토큰 · `AccountConfig.ha1` |
| `SingleInstance` | 명명 mutex + 두 번째 실행 시 창 활성화 | — |
| `AutoStart` | `HKCU\...\Run` 등록 | — |

UI 프레임워크 종속 코드는 앱(`windows/dispatch-desktop`) 에 둔다.
