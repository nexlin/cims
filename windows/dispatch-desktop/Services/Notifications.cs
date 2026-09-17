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

public enum BannerKind { PilotIncoming, DirectIncoming, PttPrivateIncoming, Emergency, ImminentPeril, Alert, ServerCert, Credential }

/// <summary>착신 배너(세션 1개) · 긴급 배너(그룹 1개) · 서버 인증서 만료 배너(세션당 1개, sip_tls_signaling.md §8.6.2) — 스택(최신 위).
/// 배너 레이어는 상단 바 아래 공통이라 관제 캔버스를 포함한 어느 화면에서나 보인다(§3.4).</summary>
public sealed partial class Banner : ObservableObject
{
    public BannerKind Kind { get; init; }
    public string Title { get; init; } = "";
    public string Subtitle { get; init; } = "";
    public SessionItem? Session { get; init; }
    public string GroupId { get; init; } = "";
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
    public bool IsServerCert => Kind == BannerKind.ServerCert;
    /// <summary>경고 계열(서버 인증서 만료·자격 갱신 실패) — 같은 시각 처리(경고 아이콘 + 빨강 계열, 경과 숨김)를 받는다.</summary>
    public bool IsWarning => Kind is BannerKind.ServerCert or BannerKind.Credential;
    /// <summary>경과 시간 표시 — 착신·긴급은 "언제부터" 가 뜻이 있고, 경고 계열은 제목이 이미 상태라 경과를 보이지 않는다.</summary>
    public bool ShowElapsed => !IsWarning;
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
    public Banner? BannerOfGroup(string groupId) => Banners.FirstOrDefault(b => b.IsEmergency && b.GroupId == groupId);
    public Banner? BannerOfKind(BannerKind kind) => Banners.FirstOrDefault(b => b.Kind == kind);
    /// <summary>응답 핫키 대상 = 최상단 착신.</summary>
    public Banner? TopIncoming => Banners.FirstOrDefault(b => b.IsIncoming);

    public void Tick(DateTime now)
    {
        for (int i = Toasts.Count - 1; i >= 0; --i)
            if (Toasts[i].AutoClose && (now - Toasts[i].Time).TotalSeconds > ToastSeconds) Toasts.RemoveAt(i);
        foreach (var b in Banners) b.Tick(now);
    }
}
