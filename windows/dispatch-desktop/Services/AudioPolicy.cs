// 오디오 배치 정책 (ue_sdk.md §6.3, dispatch_desktop_ui.md §1·§7) — 세션 종류별 기본 라우트.
//   라우트 0 = 헤드셋(기본 재생 장치): 통화·VoLTE 감청·사설콜
//   라우트 ≥1 = 데스크 스피커(추가 재생 라우트): PTT 그룹·애드혹·PTT 청취. 스피커 라우트가 없으면 헤드셋.
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed class AudioPolicy
{
    /// <summary>Engine.AddPlaybackRoute 가 준 스피커 라우트 id(없으면 0).</summary>
    public int SpeakerRoute { get; set; }

    public bool HasSpeaker => SpeakerRoute > 0;

    public int DefaultRouteFor(SessionKind kind) => kind switch
    {
        SessionKind.PttChannel or SessionKind.PttAdhoc or SessionKind.PttListen => HasSpeaker ? SpeakerRoute : 0,
        _ => 0,
    };

    public static string RouteLabel(int route) => route == 0 ? "🎧" : "🔊";
}
