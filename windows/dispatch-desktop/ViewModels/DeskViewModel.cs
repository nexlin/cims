// 상단 바(§3.2) — 데스크 신원(이름 · 소속 · 대표번호)·등록 점등·감청 중 N 칩·세션 메뉴(오디오 요약·설정·로그아웃·종료).
using System.Collections.ObjectModel;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class DeskViewModel : ObservableObject
{
    private readonly DispatchSession _s;

    /// <summary>진행 중인 감청·청취 세션(칩 목록 — 인라인·창 모두).</summary>
    public ObservableCollection<SessionItem> Monitors { get; } = new();

    public event EventHandler? SettingsRequested;
    public event EventHandler? LogoutRequested;
    public event EventHandler? ExitRequested;
    public event EventHandler<SessionItem>? MonitorActivateRequested;

    public DeskViewModel(DispatchSession s)
    {
        _s = s;
        s.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(DispatchSession.VolteReg) or nameof(DispatchSession.PttReg))
            { OnPropertyChanged(nameof(VolteState)); OnPropertyChanged(nameof(PttState)); OnPropertyChanged(nameof(VolteTip)); OnPropertyChanged(nameof(PttTip)); }
            if (e.PropertyName is nameof(DispatchSession.Profile)) RefreshIdentity();
            if (e.PropertyName is nameof(DispatchSession.HeadsetName) or nameof(DispatchSession.SpeakerName) or nameof(DispatchSession.CaptureName))
            { OnPropertyChanged(nameof(AudioSummary)); OnPropertyChanged(nameof(AudioTip)); }
        };
        s.Settings.Changed += (_, _) => OnPropertyChanged(nameof(PttHotKey));
    }

    public void RefreshIdentity()
    {
        OnPropertyChanged(nameof(DisplayName)); OnPropertyChanged(nameof(Extension)); OnPropertyChanged(nameof(PttNumber)); OnPropertyChanged(nameof(PttNumberFull));
        OnPropertyChanged(nameof(GroupName)); OnPropertyChanged(nameof(Pilot)); OnPropertyChanged(nameof(HasDesk)); OnPropertyChanged(nameof(HasPtt)); OnPropertyChanged(nameof(HasVolte));
        OnPropertyChanged(nameof(DeskLine));
    }

    public string DisplayName => _s.DisplayName;
    public string Extension => _s.MyExtension;
    public string PttNumber => _s.MyPttNumber.Length > 4 ? "PTT …" + _s.MyPttNumber[^4..] : "PTT " + _s.MyPttNumber;
    public string PttNumberFull => _s.MyPttId;
    public string GroupName => _s.GroupName;
    public string Pilot => _s.PilotId.Length > 0 ? "대표 " + UserPartConverter.UserPart(_s.PilotId) : "";
    public bool HasDesk => _s.HasDesk;
    /// <summary>상단 바 이름 옆 한 줄 — "관제1과 · 대표 7000"(관제 데스크가 없으면 PTT 번호).</summary>
    public string DeskLine => string.Join(" · ", new[] { GroupName, Pilot }.Where(x => x.Length > 0)) is { Length: > 0 } d ? d : PttNumber;
    public bool HasPtt => _s.PttService is not null;
    public bool HasVolte => _s.VolteService is not null;

    public RegState VolteState => _s.VolteReg.State;
    public RegState PttState => _s.PttReg.State;
    public string VolteTip => Tip("VoLTE", _s.VolteReg);
    public string PttTip => Tip("PTT", _s.PttReg);
    private static string Tip(string name, RegInfo r) => r.AccountId < 0 ? $"{name} 계정 없음" : $"{name} {Engine.ToText(r.State)} {r.Code} {r.Reason}".TrimEnd();

    public string AudioSummary => _s.SpeakerName.Length > 0 ? $"🎧 {Short(_s.HeadsetName)} · 🔊 {Short(_s.SpeakerName)}" : $"🎧 {Short(_s.HeadsetName)}";
    public string AudioTip => $"마이크: {_s.CaptureName}\n헤드셋(라우트 0): {_s.HeadsetName}\n데스크 스피커: {(_s.SpeakerName.Length > 0 ? _s.SpeakerName : "없음")}";
    private static string Short(string n) => n.Length > 18 ? n[..17] + "…" : n;
    public string PttHotKey => "PTT " + HotKeyMap.DisplayOf(_s.Settings.Current.HotKeys, "ptt");

    public int MonitorCount => Monitors.Count;
    public bool HasMonitors => Monitors.Count > 0;

    public void SyncMonitors(IEnumerable<SessionItem> sessions)
    {
        Monitors.Clear();
        foreach (var s in sessions.Where(x => x.IsListenLeg)) Monitors.Add(s);
        OnPropertyChanged(nameof(MonitorCount)); OnPropertyChanged(nameof(HasMonitors));
    }

    [RelayCommand] private void OpenSettings() => SettingsRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void Logout() => LogoutRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void Exit() => ExitRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void ActivateMonitor(SessionItem s) => MonitorActivateRequested?.Invoke(this, s);
}
