// ① 오른쪽 위 — PTT 발신 [사설콜|애드혹] + PTT 주소록 [사용자|그룹] (§4.1).
using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class PttUserRow : ObservableObject
{
    public Contact Contact { get; }
    [ObservableProperty] private bool _checked;
    [ObservableProperty] private string _status = "";
    public PttUserRow(Contact c) { Contact = c; }
    public string Name => Contact.Name.Length > 0 ? Contact.Name : Contact.Number;
    public string NumberShort => Contact.Number.Length > 4 ? "…" + Contact.Number[^4..] : Contact.Number;
    public string Number => Contact.Number;
}

public sealed partial class PttOriginateViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly List<PttUserRow> _allUsers = new();

    /// <summary>private | adhoc</summary>
    [ObservableProperty] private string _mode = "private";
    /// <summary>users | groups</summary>
    [ObservableProperty] private string _book = "users";
    [ObservableProperty] private string _target = "";
    [ObservableProperty] private bool _fullDuplex;
    [ObservableProperty] private bool _emergency;
    [ObservableProperty] private string _search = "";
    public ObservableCollection<PttUserRow> Users { get; } = new();
    public ObservableCollection<PttUserRow> AdhocSelection { get; } = new();
    public ObservableCollection<GroupInfo> Groups { get; } = new();

    public event EventHandler<GroupInfo>? MessageGroupRequested;
    public event EventHandler<string>? MessageUserRequested;
    public event EventHandler<GroupInfo>? AddChannelRequested;

    public PttOriginateViewModel(DispatchSession s)
    {
        _s = s;
        s.Directory.Changed += (_, _) => Reload();
        s.Groups.CollectionChanged += (_, _) => ReloadGroups();
        s.RosterChanged += (_, _) => RefreshStatus();
        s.SessionAdded += (_, _) => RefreshStatus();
        s.SessionEnded += (_, _) => RefreshStatus();
        Reload();
    }

    public bool IsPrivate => Mode == "private";
    public bool IsAdhoc => Mode == "adhoc";
    public bool IsUsers => Book == "users";
    public string StartText => IsPrivate ? "사설콜 발신" : $"애드혹 발신 ({AdhocSelection.Count}명)";
    public bool CanStart => IsPrivate ? Target.Trim().Length > 0 : AdhocSelection.Count > 0;

    partial void OnModeChanged(string v) { OnPropertyChanged(nameof(IsPrivate)); OnPropertyChanged(nameof(IsAdhoc)); OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart)); }
    partial void OnBookChanged(string v) => OnPropertyChanged(nameof(IsUsers));
    partial void OnTargetChanged(string v) => OnPropertyChanged(nameof(CanStart));
    partial void OnSearchChanged(string v) => Filter();

    private void Reload()
    {
        _allUsers.Clear();
        foreach (var c in _s.Directory.PttUsers) _allUsers.Add(new PttUserRow(c));
        Filter();
        ReloadGroups();
        RefreshStatus();
    }

    private void ReloadGroups()
    {
        Groups.Clear();
        foreach (var g in _s.Groups.Where(g => g.IsMember)) Groups.Add(g);
    }

    private void Filter()
    {
        string q = Search.Trim();
        Users.Clear();
        foreach (var u in _allUsers.Where(u => q.Length == 0 || u.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || u.Number.Contains(q, StringComparison.OrdinalIgnoreCase)))
            Users.Add(u);
    }

    /// <summary>사용자 현재 상태 — 어느 채널에 참여/발언 중인지(로스터·세션에서 파생).</summary>
    private void RefreshStatus()
    {
        foreach (var u in _allUsers)
        {
            string st = "";
            foreach (var g in _s.Groups)
            {
                var e = g.Roster.FirstOrDefault(r => Converters.UserPartConverter.UserPart(r.Uri) == u.Number);
                if (e is null) continue;
                var sess = _s.SessionOfGroup(g.Id);
                st = sess is not null && sess.Speaker == u.Name ? $"{g.Name} 발언" : $"{g.Name} 참여";
                break;
            }
            u.Status = st;
        }
    }

    [RelayCommand] private void SetMode(string mode) => Mode = mode;
    [RelayCommand] private void SetBook(string book) => Book = book;

    [RelayCommand]
    private void Start()
    {
        if (IsPrivate)
        {
            string t = Target.Trim();
            var byName = _allUsers.FirstOrDefault(u => u.Name.Equals(t, StringComparison.OrdinalIgnoreCase));
            var r = _s.StartPrivateCall(byName?.Number ?? t, FullDuplex, Emergency);
            if (r.Ok) { Target = ""; Emergency = false; }
        }
        else
        {
            var r = _s.StartAdhoc(AdhocSelection.Select(u => u.Number).ToList(), Emergency);
            if (r.Ok) { foreach (var u in AdhocSelection.ToList()) u.Checked = false; AdhocSelection.Clear(); Emergency = false; OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart)); }
        }
    }

    [RelayCommand] private void PrivateTo(PttUserRow u) { Mode = "private"; Target = u.Name; Start(); }
    [RelayCommand]
    private void ToggleAdhoc(PttUserRow u)
    {
        Mode = "adhoc";
        if (AdhocSelection.Contains(u)) { AdhocSelection.Remove(u); u.Checked = false; }
        else { AdhocSelection.Add(u); u.Checked = true; }
        OnPropertyChanged(nameof(StartText)); OnPropertyChanged(nameof(CanStart));
    }
    [RelayCommand] private void RemoveAdhoc(PttUserRow u) { if (u.Checked) ToggleAdhoc(u); }
    [RelayCommand] private void MessageUser(PttUserRow u) => MessageUserRequested?.Invoke(this, u.Number);
    [RelayCommand] private void MessageGroup(GroupInfo g) => MessageGroupRequested?.Invoke(this, g);
    [RelayCommand] private void AddChannel(GroupInfo g) => AddChannelRequested?.Invoke(this, g);
}
