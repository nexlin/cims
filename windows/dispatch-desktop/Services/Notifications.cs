// 알림 — 토스트(명령 실패 사유·정보, §3.2)와 배너(착신·긴급). UI 스레드에서만 만진다.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public enum ToastLevel { Info, Warn, Error }

public sealed partial class Toast : ObservableObject
{
    public ToastLevel Level { get; init; }
    public string Text { get; init; } = "";
    /// <summary>원문 코드·사유 — ▸상세.</summary>
    public string Detail { get; init; } = "";
    public DateTime Time { get; } = DateTime.Now;
    [ObservableProperty] private bool _showDetail;
    public bool HasDetail => Detail.Length > 0;
    public bool AutoClose => Level != ToastLevel.Error;
    public bool IsError => Level == ToastLevel.Error;
    public bool IsWarn => Level == ToastLevel.Warn;
}

public enum BannerKind { PilotIncoming, DirectIncoming, PttPrivateIncoming, Emergency, ImminentPeril, Alert, ServerCert, Credential, Video }

/// <summary>착신 배너(세션 1개) · 긴급 배너(그룹 1개) · 서버 인증서 만료 배너(세션당 1개, sip_tls_signaling.md §8.6.2) — 스택(최신 위).
/// 배너 층은 상단 바 아래 공통이라 관제를 포함한 어느 화면에서나 보인다(§3.2). 긴급·임박은 꽉 찬 면·닫기 없음, 착신은 옅은 면 [응답][거절].</summary>
public sealed partial class Banner : ObservableObject
{
    public BannerKind Kind { get; init; }
    public string Title { get; init; } = "";
    public string Subtitle { get; init; } = "";
    public SessionItem? Session { get; init; }
    public string GroupId { get; init; } = "";
    /// <summary>긴급 경보 배너의 경보 발신자(bare MCPTT ID) — 같은 그룹·같은 발신자의 취소(TS 24.379 §12.1.1.3)로 내린다.</summary>
    public string AlertUser { get; init; } = "";
    /// <summary>«새 영상» 배너의 송출자 MCVideo ID(§10.3) — [받기] 의 AcceptReception 인자. 받거나·송출이 끝나거나·다른 송출을 보기 시작하면 내린다.</summary>
    public string Transmitter { get; init; } = "";
    /// <summary>배너의 [해제] — 긴급·임박 = 조건 하향(내가 올린 조건·내 소유 그룹, TS 24.379 §10.1.1.2.1.5), 경보 = 경보 취소(allow-cancel-emergency-alert,
    /// 남의 경보면 제3자 취소 §12.1.1.2 4)e)). 세션 조건·자격이 바뀌면 갱신된다.</summary>
    [ObservableProperty] private bool _canCancel;
    /// <summary>위험 단계(서버 인증서 잔여 ≤ 7일) — 연한 배경 대신 진한 배경.</summary>
    public bool Critical { get; init; }
    public DateTime Time { get; } = DateTime.Now;
    [ObservableProperty] private TimeSpan _elapsed;
    public bool IsIncoming => Kind is BannerKind.PilotIncoming or BannerKind.DirectIncoming or BannerKind.PttPrivateIncoming;
    public bool IsEmergency => Kind is BannerKind.Emergency or BannerKind.ImminentPeril or BannerKind.Alert;
    public bool IsPilot => Kind == BannerKind.PilotIncoming;
    public bool IsDirect => Kind == BannerKind.DirectIncoming;
    public bool IsPtt => Kind == BannerKind.PttPrivateIncoming;
    public bool IsEmg => Kind == BannerKind.Emergency;
    public bool IsPeril => Kind == BannerKind.ImminentPeril;
    public bool IsAlert => Kind == BannerKind.Alert;
    /// <summary>MCVideo 새 송출 알림(TS 24.581 §6.2.5.3.2) — 옅은 청록 면 [받기][닫기](§10.3).</summary>
    public bool IsVideo => Kind == BannerKind.Video;
    /// <summary>[닫기] — 경보(로컬 표시만 내림)·새 영상(배너만 내림, 송출 목록은 채널 상세에 남는다).</summary>
    public bool ShowDismiss => IsAlert || IsVideo;
    public string DismissTip => IsVideo ? "배너만 내린다 — 채널 상세 «영상 n» 목록에서 다시 [보기] 할 수 있다(TS 22.281 R-5.2.6.2.2-009)"
                              : "이 화면의 경보 표시만 내린다 — 서버의 경보는 그대로(취소 신호를 놓쳤을 때)";
    public string CancelText => IsAlert ? "경보 해제" : "긴급 해제";
    public bool IsServerCert => Kind == BannerKind.ServerCert;
    /// <summary>경고 계열(서버 인증서 만료·자격 갱신 실패) — 같은 시각 처리(경고 아이콘 + 빨강 계열, 경과 숨김)를 받는다.</summary>
    public bool IsWarning => Kind is BannerKind.ServerCert or BannerKind.Credential;
    /// <summary>경과 시간 표시 — 착신·긴급은 "언제부터" 가 뜻이 있고, 경고 계열은 제목이 이미 상태라 경과를 보이지 않는다.</summary>
    public bool ShowElapsed => !IsWarning;
    /// <summary>배너 두 줄(§3.2) — 윗줄(작게) = 무엇이 · 누가, 아랫줄(크게) = 채널·번호. 긴급·임박·경보는 제목의 "— 그룹명" 을 아랫줄로 뗀다.</summary>
    public string Line1 => IsEmergency
        ? (IsEmg ? "긴급" : IsPeril ? "임박 위험" : "긴급 경보") + (Subtitle.Length > 0 ? $" · 개시 {Subtitle}" : "")
        : IsIncoming ? Title : Title;
    public string Line2 => IsEmergency ? (Title.IndexOf(" — ", StringComparison.Ordinal) is int i and >= 0 ? Title[(i + 3)..] : Title) : IsIncoming ? Subtitle : Subtitle;
    public void Tick(DateTime now) => Elapsed = now - Time;
}

public sealed class Notifications
{
    public const int ToastSeconds = 6;

    public ObservableCollection<Toast> Toasts { get; } = new();
    public ObservableCollection<Banner> Banners { get; } = new();

    public void Info(string text, string detail = "") => Push(ToastLevel.Info, text, detail);
    public void Warn(string text, string detail = "") => Push(ToastLevel.Warn, text, detail);
    public void Error(string text, string detail = "") => Push(ToastLevel.Error, text, detail);

    private void Push(ToastLevel level, string text, string detail)
    {
        Toasts.Insert(0, new Toast { Level = level, Text = text, Detail = detail });
        while (Toasts.Count > 6) Toasts.RemoveAt(Toasts.Count - 1);
    }

    public void Dismiss(Toast t) => Toasts.Remove(t);

    public void ShowBanner(Banner b) => Banners.Insert(0, b);
    public void RemoveBanner(Banner b) => Banners.Remove(b);
    public Banner? BannerOf(SessionItem s) => Banners.FirstOrDefault(b => b.Session == s);
    /// <summary>그룹 세션의 긴급·임박 배너(그룹당 하나) — 경보 배너(발신자별)는 따로 센다(BannerOfAlert).</summary>
    public Banner? BannerOfGroup(string groupId) => Banners.FirstOrDefault(b => b.Kind is BannerKind.Emergency or BannerKind.ImminentPeril && b.GroupId == groupId);
    public Banner? BannerOfAlert(string groupId, string user) => Banners.FirstOrDefault(b => b.IsAlert && b.GroupId == groupId && b.AlertUser == user);
    public Banner? BannerOfKind(BannerKind kind) => Banners.FirstOrDefault(b => b.Kind == kind);
    /// <summary>그룹 영상 호의 «새 영상» 배너(호당 하나).</summary>
    public Banner? VideoBannerOf(SessionItem s) => Banners.FirstOrDefault(b => b.IsVideo && b.Session == s);
    /// <summary>응답 핫키 대상 = 최상단 착신.</summary>
    public Banner? TopIncoming => Banners.FirstOrDefault(b => b.IsIncoming);

    public void Tick(DateTime now)
    {
        for (int i = Toasts.Count - 1; i >= 0; --i)
            if (Toasts[i].AutoClose && (now - Toasts[i].Time).TotalSeconds > ToastSeconds) Toasts.RemoveAt(i);
        foreach (var b in Banners) b.Tick(now);
    }
}
