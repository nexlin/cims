// 설정 창 — 오디오(§7)·핫키(§8)·관제·표시·채널 선택. 저장 = settings.json + 즉시 적용(오디오 재적용·핫키 재등록·테마).
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
    partial void OnTextChanged(string value) => OnPropertyChanged(nameof(IsValid));
}

public sealed partial class ChannelChoice : ObservableObject
{
    public GroupInfo Group { get; }
    [ObservableProperty] private bool _selected;
    public ChannelChoice(GroupInfo g, bool sel) { Group = g; _selected = sel; }
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
    public ObservableCollection<HotKeyRow> HotKeys { get; } = new();
    [ObservableProperty] private string _pickupFeatureCode;
    [ObservableProperty] private bool _autoHoldOnAnswer;
    [ObservableProperty] private bool _confirmCloseMonitor;
    [ObservableProperty] private int _maxMonitorWindows;
    [ObservableProperty] private bool _followChannelThread;
    [ObservableProperty] private bool _minimizeToTray;
    [ObservableProperty] private int _messageRetentionDays;
    [ObservableProperty] private string _theme;
    [ObservableProperty] private string _directoryCsv;
    [ObservableProperty] private int _logLevel;
    [ObservableProperty] private bool _autoStart;
    public ObservableCollection<ChannelChoice> Channels { get; } = new();
    public string DirectoryLoadedFrom => _s.Directory.LoadedFrom ?? "(없음)";
    public string LogsPath => AppPaths.Logs;

    public event EventHandler? Saved;

    public SettingsViewModel(DispatchSession s, HotKeyMap hotKeys)
    {
        _s = s; _hotKeys = hotKeys;
        var c = s.Settings.Current;
        _captureDevice = c.CaptureDevice; _headsetDevice = c.HeadsetDevice; _speakerDevice = c.SpeakerDevice; _speakerRouteEnabled = c.SpeakerRouteEnabled;
        _autoReturnToPreferredDevice = c.AutoReturnToPreferredDevice; _pickupFeatureCode = c.PickupFeatureCode; _autoHoldOnAnswer = c.AutoHoldOnAnswer;
        _confirmCloseMonitor = c.ConfirmCloseMonitor; _maxMonitorWindows = c.MaxMonitorWindows; _followChannelThread = c.FollowChannelThread;
        _minimizeToTray = c.MinimizeToTray; _messageRetentionDays = c.MessageRetentionDays; _theme = c.Theme; _directoryCsv = c.DirectoryCsv; _logLevel = c.LogLevel;
        _autoStart = CimsUe.Platform.AutoStart.IsEnabled(AppPaths.InstanceName);
        LoadDevices();
        foreach (var (name, label, global) in new[] { ("ptt", "PTT (누르는 동안)", true), ("answer", "응답", true), ("hangup", "종료", true), ("pickup", "그룹 픽업", true), ("hold", "보류/재개", false), ("mute", "음소거", false) })
            HotKeys.Add(new HotKeyRow(name, label, global, c.HotKeys.TryGetValue(name, out var t) ? t : "") { Conflict = hotKeys.Conflicts.Contains(name) });
        foreach (var g in s.Groups.Where(g => g.IsMember)) Channels.Add(new ChannelChoice(g, c.SelectedChannels.Count == 0 || c.SelectedChannels.Contains(g.Id)));
    }

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
        _s.Settings.Update(c =>
        {
            c.CaptureDevice = CaptureDevice; c.HeadsetDevice = HeadsetDevice; c.SpeakerDevice = SpeakerDevice; c.SpeakerRouteEnabled = SpeakerRouteEnabled;
            c.AutoReturnToPreferredDevice = AutoReturnToPreferredDevice; c.PickupFeatureCode = PickupFeatureCode.Trim(); c.AutoHoldOnAnswer = AutoHoldOnAnswer;
            c.ConfirmCloseMonitor = ConfirmCloseMonitor; c.MaxMonitorWindows = Math.Clamp(MaxMonitorWindows, 1, 16); c.FollowChannelThread = FollowChannelThread;
            c.MinimizeToTray = MinimizeToTray; c.MessageRetentionDays = Math.Clamp(MessageRetentionDays, 1, 365); c.Theme = Theme; c.DirectoryCsv = DirectoryCsv.Trim(); c.LogLevel = LogLevel;
            foreach (var h in HotKeys) c.HotKeys[h.Name] = h.Text.Trim();
            c.SelectedChannels = Channels.All(x => x.Selected) ? new List<string>() : Channels.Where(x => x.Selected).Select(x => x.Group.Id).ToList();
        });
        var conflicts = _hotKeys.Apply(_s.Settings.Current.HotKeys);
        foreach (var h in HotKeys) h.Conflict = conflicts.Contains(h.Name);
        try { CimsUe.Platform.AutoStart.SetEnabled(AppPaths.InstanceName, AutoStart); } catch (Exception ex) { _s.Log.Warn("autostart: " + ex.Message); }
        _s.Log.MinLevel = LogLevel;
        _s.Directory.Load(_s.Settings.Current.DirectoryCsv.Length > 0 ? _s.Settings.Current.DirectoryCsv : null);
        _s.ApplyAudioSettings();
        Saved?.Invoke(this, EventArgs.Empty);
    }
}
