// ③ 오른쪽 위 — 일반통화 발신: 번호 필드·[발신]·[픽업 **] + 세그먼트 [다이얼패드|주소록|최근] 중 하나 (§4.3). 다이얼패드는 통화 중 DTMF 겸용.
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class BookRow : ObservableObject
{
    public Contact Contact { get; }
    [ObservableProperty] private string _status = "";
    public BookRow(Contact c) { Contact = c; }
    public string Name => Contact.Name;
    public string Number => Contact.Number;
    public string KindText => Contact.KindText;
    public bool IsExternal => Contact.IsExternal;
    public string SmsTip => IsExternal ? "문자 게이트웨이 미구성" : "문자";
}

public sealed partial class CallOriginateViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<BookRow> _all = new();

    [ObservableProperty] private string _number = "";
    /// <summary>pad | book | recent</summary>
    [ObservableProperty] private string _mode = "book";
    [ObservableProperty] private string _search = "";
    public ObservableCollection<BookRow> Book { get; } = new();
    public ObservableCollection<ActivityRow> Recent { get; } = new();
    public string[] PadKeys { get; } = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#" };

    public event EventHandler<string>? SmsRequested;

    public CallOriginateViewModel(DispatchSession s)
    {
        _s = s;
        _mode = s.Settings.Current.OriginateMode;
        s.Directory.Changed += (_, _) => Reload();
        s.Activity.Call.CollectionChanged += (_, _) => ReloadRecent();
        s.DialogChanged += (_, _) => RefreshStatus();
        s.DialogEnded += (_, _) => RefreshStatus();
        Reload();
    }

    public bool IsPad => Mode == "pad";
    public bool IsBook => Mode == "book";
    public bool IsRecent => Mode == "recent";
    public string PickupCode => _s.Settings.Current.PickupFeatureCode;
    /// <summary>활성 통화가 있고 필드가 비어 있으면 패드는 DTMF.</summary>
    public bool PadIsDtmf => Number.Length == 0 && _s.ActiveVolteCall is not null;
    public string PadHint => PadIsDtmf ? "통화 중 — DTMF 로 전송" : "번호를 입력";

    partial void OnModeChanged(string v) { OnPropertyChanged(nameof(IsPad)); OnPropertyChanged(nameof(IsBook)); OnPropertyChanged(nameof(IsRecent)); _s.Settings.Update(x => x.OriginateMode = v); }
    partial void OnNumberChanged(string v) { OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); }
    partial void OnSearchChanged(string v) => Filter();

    public void RefreshPad() { OnPropertyChanged(nameof(PadIsDtmf)); OnPropertyChanged(nameof(PadHint)); }

    private void Reload()
    {
        _all.Clear();
        foreach (var c in _s.Directory.CallBook.Where(c => c.Number != _s.MyExtension)) _all.Add(new BookRow(c));
        Filter();
        ReloadRecent();
        RefreshStatus();
    }

    private void Filter()
    {
        string q = Search.Trim();
        Book.Clear();
        foreach (var r in _all.Where(r => q.Length == 0 || r.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || r.Number.Contains(q, StringComparison.OrdinalIgnoreCase))) Book.Add(r);
    }

    private void ReloadRecent()
    {
        Recent.Clear();
        foreach (var r in _s.Activity.Call.Where(r => r.CanRedial).Take(50)) Recent.Add(r);
    }

    private void RefreshStatus()
    {
        foreach (var r in _all)
        {
            var d = _s.Dialogs.Where(x => x.WatchedNumber == r.Number).OrderByDescending(x => x.IsConfirmed).FirstOrDefault();
            r.Status = d is null ? "" : d.IsConfirmed ? "통화중" : d.IsEarly ? "링잉" : "";
        }
    }

    [RelayCommand] private void SetMode(string m) => Mode = m;
    [RelayCommand] private void Dial() { string n = Number.Trim(); if (n.Length == 0) return; if (Resolve(n) is { } t && _s.Dial(t).Ok) Number = ""; }
    [RelayCommand] private void Pickup() => _s.Pickup();
    [RelayCommand]
    private void Pad(string key)
    {
        if (PadIsDtmf) { var c = _s.ActiveVolteCall; if (c is not null) _s.Dtmf(c, key); return; }
        Number += key;
    }
    [RelayCommand] private void Backspace() { if (Number.Length > 0) Number = Number[..^1]; }
    [RelayCommand] private void Clear() => Number = "";
    [RelayCommand] private void Call(BookRow r) => _s.Dial(r.Number);
    [RelayCommand] private void Sms(BookRow r) { if (!r.IsExternal) SmsRequested?.Invoke(this, r.Number); }
    [RelayCommand] private void Redial(ActivityRow r) { if (r.Number.Length > 0) _s.Dial(r.Number); }
    [RelayCommand] private void SmsRecent(ActivityRow r) { if (r.Number.Length > 0 && !_s.Directory.IsExternal(r.Number)) SmsRequested?.Invoke(this, r.Number); }
    public void Fill(string number) => Number = number;

    /// <summary>이름 입력이면 주소록에서 번호로.</summary>
    private string? Resolve(string input)
    {
        var byName = _all.FirstOrDefault(r => r.Name.Equals(input, StringComparison.OrdinalIgnoreCase));
        return byName?.Number ?? input;
    }
}
