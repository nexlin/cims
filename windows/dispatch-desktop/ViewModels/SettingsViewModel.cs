// 설정 창 — 오디오(§7)·영상(§10)·핫키(§8)·관제·표시·주소록. 저장 = settings.json + 즉시 적용(오디오·영상 재적용·핫키 재등록·테마).
using System.Collections.ObjectModel;
using CimsUe.Platform;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class HotKeyRow : ObservableObject
{
    public string Name { get; }
    public string Label { get; }
    public bool IsGlobal { get; }
    [ObservableProperty] private string _text;
    [ObservableProperty] private bool _conflict;
    public HotKeyRow(string name, string label, bool global, string text) { Name = name; Label = label; IsGlobal = global; _text = text; }
    public bool IsValid => Text.Trim().Length == 0 || HotKey.TryParse(Text, out _);
    /// <summary>전역 이름인데 글자 키(Space 등) — 전역 등록하지 않고 앱이 앞에 있을 때(입력칸 밖)만 받는다.</summary>
    public bool IsFocusOnly => IsGlobal && HotKey.TryParse(Text, out var k) && DispatchDesktop.Services.HotKeyMap.IsTypingKey(k);
    partial void OnTextChanged(string value) { OnPropertyChanged(nameof(IsValid)); OnPropertyChanged(nameof(IsFocusOnly)); }
}

public sealed partial class SettingsViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly HotKeyMap _hotKeys;

    public ObservableCollection<string> CaptureDevices { get; } = new();
    public ObservableCollection<string> RenderDevices { get; } = new();
    [ObservableProperty] private string _captureDevice;
    [ObservableProperty] private string _headsetDevice;
    [ObservableProperty] private string _speakerDevice;
    [ObservableProperty] private bool _speakerRouteEnabled;
    [ObservableProperty] private bool _autoReturnToPreferredDevice;
    /// <summary>카메라 이름 목록(엔진 영상 장치 — 첫 줄 "" = 첫 카메라). 엔진이 서기 전(로그인 전)엔 비어 있다.</summary>
    public ObservableCollection<string> Cameras { get; } = new();
    [ObservableProperty] private string _videoCaptureDevice;
    /// <summary>voice | video — «영상 보내는 중 무전»(D12).</summary>
    [ObservableProperty] private string _videoMicPolicy;
    public string CameraNote => _s.Engine.IsRunning
        ? (Cameras.Count > 1 ? "카메라는 이름으로 기억합니다. 빈 값 = 첫 카메라. 카메라를 새로 꽂았으면 다시 로그인하면 목록에 나옵니다." : "이 PC 에서 카메라를 찾지 못했습니다 — [영상 보내기] 가 꺼져 있습니다(영상 보기는 됩니다).")
        : "로그인한 뒤 카메라 목록이 나옵니다.";
    public ObservableCollection<HotKeyRow> HotKeys { get; } = new();
    [ObservableProperty] private string _pickupFeatureCode;
    [ObservableProperty] private bool _autoHoldOnAnswer;
    [ObservableProperty] private bool _confirmCloseMonitor;
    [ObservableProperty] private int _maxMonitorWindows;
    [ObservableProperty] private bool _followChannelThread;
    [ObservableProperty] private bool _lockTalk;
    [ObservableProperty] private bool _minimizeToTray;
    [ObservableProperty] private int _messageRetentionDays;
    [ObservableProperty] private string _theme;
    [ObservableProperty] private string _directoryCsv;
    [ObservableProperty] private int _logLevel;
    [ObservableProperty] private bool _autoStart;
    public string DirectoryLoadedFrom => _s.Directory.LoadedFrom ?? "(없음)";
    public string LogsPath => AppPaths.Logs;

    public event EventHandler? Saved;

    public SettingsViewModel(DispatchSession s, HotKeyMap hotKeys)
    {
        _s = s; _hotKeys = hotKeys;
        var c = s.Settings.Current;
        _captureDevice = c.CaptureDevice; _headsetDevice = c.HeadsetDevice; _speakerDevice = c.SpeakerDevice; _speakerRouteEnabled = c.SpeakerRouteEnabled;
        _autoReturnToPreferredDevice = c.AutoReturnToPreferredDevice; _pickupFeatureCode = c.PickupFeatureCode; _autoHoldOnAnswer = c.AutoHoldOnAnswer;
        _confirmCloseMonitor = c.ConfirmCloseMonitor; _maxMonitorWindows = c.MaxMonitorWindows; _followChannelThread = c.FollowChannelThread; _lockTalk = c.LockTalk;
        _minimizeToTray = c.MinimizeToTray; _messageRetentionDays = c.MessageRetentionDays; _theme = c.Theme; _directoryCsv = c.DirectoryCsv; _logLevel = c.LogLevel;
        _autoStart = CimsUe.Platform.AutoStart.IsEnabled(AppPaths.InstanceName);
        _videoCaptureDevice = c.VideoCaptureDevice; _videoMicPolicy = c.VideoMicPolicy == "video" ? "video" : "voice";
        LoadDevices();
        foreach (var (name, label, global) in new[] { ("ptt", "PTT (누르는 동안)", true), ("answer", "응답", true), ("hangup", "종료", true), ("pickup", "그룹 픽업", true), ("hold", "보류/재개", false), ("mute", "음소거", false) })
        {
            var row = new HotKeyRow(name, label, global, c.HotKeys.TryGetValue(name, out var t) ? t : "") { Conflict = hotKeys.Conflicts.Contains(name) };
            row.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(HotKeyRow.IsValid)) { OnPropertyChanged(nameof(CanSave)); OnPropertyChanged(nameof(HotKeyError)); } };
            HotKeys.Add(row);
        }
    }

    /// <summary>해석되지 않는 핫키 문자열이 있으면 저장하지 않는다 — 조용히 등록만 빠지던 것을 막는다.</summary>
    public bool CanSave => HotKeys.All(h => h.IsValid);
    public string HotKeyError => CanSave ? "" : "핫키 형식 오류: " + string.Join(", ", HotKeys.Where(h => !h.IsValid).Select(h => h.Label)) + " — 예: Space, Ctrl+Space, F9, Alt+Shift+P";

    [RelayCommand]
    private void LoadDevices()
    {
        CaptureDevices.Clear(); RenderDevices.Clear();
        CaptureDevices.Add(""); RenderDevices.Add("");
        try
        {
            foreach (var d in _s.Endpoints.List(AudioFlow.Capture)) CaptureDevices.Add(d.Name);
            foreach (var d in _s.Endpoints.List(AudioFlow.Render)) RenderDevices.Add(d.Name);
        }
        catch (Exception ex) { _s.Log.Warn("endpoint list: " + ex.Message); }
        Cameras.Clear();
        Cameras.Add("");
        foreach (var d in _s.Cameras()) Cameras.Add(d.Name);
        OnPropertyChanged(nameof(CameraNote));
    }

    [RelayCommand]
    private void BrowseDirectory()
    {
        var dlg = new Microsoft.Win32.OpenFileDialog { Filter = "CSV|*.csv|모든 파일|*.*" };
        if (dlg.ShowDialog() == true) DirectoryCsv = dlg.FileName;
    }

    [RelayCommand]
    private void Save()
    {
        if (!CanSave) return;
        _s.Settings.Update(c =>
        {
            c.CaptureDevice = CaptureDevice; c.HeadsetDevice = HeadsetDevice; c.SpeakerDevice = SpeakerDevice; c.SpeakerRouteEnabled = SpeakerRouteEnabled;
            c.AutoReturnToPreferredDevice = AutoReturnToPreferredDevice; c.PickupFeatureCode = PickupFeatureCode.Trim(); c.AutoHoldOnAnswer = AutoHoldOnAnswer;
            c.ConfirmCloseMonitor = ConfirmCloseMonitor; c.MaxMonitorWindows = Math.Clamp(MaxMonitorWindows, 1, 16); c.FollowChannelThread = FollowChannelThread; c.LockTalk = LockTalk;
            c.MinimizeToTray = MinimizeToTray; c.MessageRetentionDays = Math.Clamp(MessageRetentionDays, 1, 365); c.Theme = Theme; c.DirectoryCsv = DirectoryCsv.Trim(); c.LogLevel = LogLevel;
            foreach (var h in HotKeys) c.HotKeys[h.Name] = h.Text.Trim();
            c.VideoCaptureDevice = VideoCaptureDevice.Trim(); c.VideoMicPolicy = VideoMicPolicy;
        });
        var conflicts = _hotKeys.Apply(_s.Settings.Current.HotKeys);
        foreach (var h in HotKeys) h.Conflict = conflicts.Contains(h.Name);
        try { CimsUe.Platform.AutoStart.SetEnabled(AppPaths.InstanceName, AutoStart); } catch (Exception ex) { _s.Log.Warn("autostart: " + ex.Message); }
        _s.Log.MinLevel = LogLevel;
        _s.Directory.Load(_s.Settings.Current.DirectoryCsv.Length > 0 ? _s.Settings.Current.DirectoryCsv : null);
        _s.ApplyAudioSettings();
        _s.ApplyVideoSettings();
        Saved?.Invoke(this, EventArgs.Empty);
    }
}
