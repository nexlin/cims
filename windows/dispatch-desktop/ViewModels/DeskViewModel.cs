// 상단 바(§3.2) — 데스크 신원·등록 점등·감청 중 N 칩·배치 잠금/프리셋·오디오 요약·핫키·시각·설정.
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
    private readonly LayoutStore _layout;

    [ObservableProperty] private string _clock = DateTime.Now.ToString("HH:mm");
    [ObservableProperty] private bool _layoutLocked = true;
    [ObservableProperty] private string _currentPreset = LayoutStore.DefaultName;
    public ObservableCollection<string> Presets { get; } = new();
    /// <summary>열린 감청·청취 창의 세션(칩 목록).</summary>
    public ObservableCollection<SessionItem> Monitors { get; } = new();

    public event EventHandler? SettingsRequested;
    public event EventHandler? LogoutRequested;
    public event EventHandler? ExitRequested;
    public event EventHandler<string>? PresetApplyRequested;
    public event EventHandler<string>? PresetSaveRequested;
    public event EventHandler<SessionItem>? MonitorActivateRequested;

    public DeskViewModel(DispatchSession s, LayoutStore layout)
    {
        _s = s; _layout = layout;
        s.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(DispatchSession.VolteReg) or nameof(DispatchSession.PttReg))
            { OnPropertyChanged(nameof(VolteState)); OnPropertyChanged(nameof(PttState)); OnPropertyChanged(nameof(VolteTip)); OnPropertyChanged(nameof(PttTip)); }
            if (e.PropertyName is nameof(DispatchSession.Profile)) RefreshIdentity();
            if (e.PropertyName is nameof(DispatchSession.HeadsetName) or nameof(DispatchSession.SpeakerName) or nameof(DispatchSession.CaptureName))
            { OnPropertyChanged(nameof(AudioSummary)); OnPropertyChanged(nameof(AudioTip)); }
        };
        s.Settings.Changed += (_, _) => OnPropertyChanged(nameof(PttHotKey));
        RefreshPresets();
    }

    public void RefreshIdentity()
    {
        OnPropertyChanged(nameof(DisplayName)); OnPropertyChanged(nameof(Extension)); OnPropertyChanged(nameof(PttNumber)); OnPropertyChanged(nameof(PttNumberFull));
        OnPropertyChanged(nameof(GroupName)); OnPropertyChanged(nameof(Pilot)); OnPropertyChanged(nameof(HasDesk)); OnPropertyChanged(nameof(HasPtt)); OnPropertyChanged(nameof(HasVolte));
    }

    public string DisplayName => _s.DisplayName;
    public string Extension => _s.MyExtension;
    public string PttNumber => _s.MyPttNumber.Length > 4 ? "PTT …" + _s.MyPttNumber[^4..] : "PTT " + _s.MyPttNumber;
    public string PttNumberFull => _s.MyPttId;
    public string GroupName => _s.GroupName;
    public string Pilot => _s.PilotId.Length > 0 ? "대표 " + UserPartConverter.UserPart(_s.PilotId) : "";
    public bool HasDesk => _s.HasDesk;
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
    public string PttHotKey => "⌨ PTT " + HotKeyMap.DisplayOf(_s.Settings.Current.HotKeys, "ptt");

    public int MonitorCount => Monitors.Count;
    public bool HasMonitors => Monitors.Count > 0;

    public void SyncMonitors(IEnumerable<SessionItem> sessions)
    {
        Monitors.Clear();
        foreach (var s in sessions.Where(x => x.IsWindow)) Monitors.Add(s);
        OnPropertyChanged(nameof(MonitorCount)); OnPropertyChanged(nameof(HasMonitors));
    }

    public void Tick(DateTime now) => Clock = now.ToString("HH:mm");

    public void RefreshPresets()
    {
        Presets.Clear();
        foreach (var n in _layout.Names) Presets.Add(n);
        CurrentPreset = _layout.File.Current;
        LayoutLocked = _layout.Current.Locked;
    }

    [RelayCommand] private void ToggleLock() => LayoutLocked = !LayoutLocked;
    [RelayCommand] private void ApplyPreset(string name) => PresetApplyRequested?.Invoke(this, name);
    [RelayCommand] private void SavePreset(string name) => PresetSaveRequested?.Invoke(this, name);
    [RelayCommand] private void OpenSettings() => SettingsRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void Logout() => LogoutRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void Exit() => ExitRequested?.Invoke(this, EventArgs.Empty);
    [RelayCommand] private void ActivateMonitor(SessionItem s) => MonitorActivateRequested?.Invoke(this, s);
}
