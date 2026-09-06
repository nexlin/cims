// PTT 그룹 생성·편집 창 — GMS 그룹 문서(GroupDoc)를 폼으로(§4.1 [그룹] 탭 [새 그룹]/[편집]). 저장 = XCAP PUT(TS 24.481, 본인 소유).
// 멤버 후보 = PTT 주소록(서버 전화번호부 service=ptt). 편집은 열 때의 ETag 를 If-Match 로 보내 충돌(412)을 잡는다.
using System.Collections.ObjectModel;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class GroupMemberRow : ObservableObject
{
    public string Uri { get; }
    public string Name { get; }
    public string DisplayNumber { get; }
    public bool IsMe { get; }
    [ObservableProperty] private bool _isChair;
    public GroupMemberRow(string uri, string name, string displayNumber, bool isMe, bool isChair)
    {
        Uri = uri; Name = name; DisplayNumber = displayNumber; IsMe = isMe; _isChair = isChair;
    }
    public string Label => Name.Length > 0 ? Name : DisplayNumber;
    public string RoleText => IsChair ? "의장" : "참가자";
    partial void OnIsChairChanged(bool value) => OnPropertyChanged(nameof(RoleText));
}

public sealed partial class GroupCandidateRow
{
    public Contact Contact { get; }
    public string DisplayNumber { get; }
    public string OrgPath { get; }
    public GroupCandidateRow(Contact c, string displayNumber, string orgPath) { Contact = c; DisplayNumber = displayNumber; OrgPath = orgPath; }
    public string Name => Contact.Name.Length > 0 ? Contact.Name : DisplayNumber;
}

public sealed partial class GroupEditViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private readonly GroupInfo? _existing;
    private string _ifMatch = "";
    private string _orgCode = "";

    /// <summary>인스턴스 프로퍼티 — WPF 바인딩은 static 멤버를 경로로 풀지 못한다.</summary>
    public IReadOnlyList<string> SessionTypes { get; } = new[] { "prearranged", "chat", "broadcast" };

    [ObservableProperty] private string _name = "";
    /// <summary>그룹 id(uri user part) — 신규만 편집 가능.</summary>
    [ObservableProperty] private string _groupId = "";
    [ObservableProperty] private string _sessionType = "prearranged";
    [ObservableProperty] private bool _videoEnabled;
    [ObservableProperty] private bool _allowSds = true;
    [ObservableProperty] private bool _allowFd;
    [ObservableProperty] private bool _emergencyCall = true;
    [ObservableProperty] private bool _emergencyAlert = true;
    [ObservableProperty] private bool _requireAffiliation = true;
    [ObservableProperty] private bool _encryption;
    [ObservableProperty] private int _priority = 5;
    [ObservableProperty] private int _maxParticipants;
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private bool _busy;
    /// <summary>편집 문서를 받는 중(신규는 즉시 true).</summary>
    [ObservableProperty] private bool _loaded;

    public ObservableCollection<GroupMemberRow> Members { get; } = new();
    public ObservableCollection<GroupCandidateRow> Candidates { get; } = new();

    public event EventHandler? Saved;

    public GroupEditViewModel(DispatchSession s, GroupInfo? existing)
    {
        _s = s; _existing = existing;
        if (existing is null)
        {
            GroupId = UserPartConverter.UserPart(s.NewGroupUri());
            AddMember(s.ToTelUri(s.MyPttNumber), s.DisplayName, chair: true);
            Loaded = true;
        }
        else _ = LoadAsync();
        Filter();
    }

    public bool IsNew => _existing is null;
    public string Title => IsNew ? "새 PTT 그룹" : $"그룹 편집 — {_existing!.Name}";
    /// <summary>그룹 uri 정규형 = `tel:&lt;id&gt;`(mcptt_api.md §2). 기존 그룹은 목록의 uri 그대로.</summary>
    public string Uri => IsNew ? $"tel:{GroupId.Trim()}" : _existing!.Uri;
    public bool HasError => Error.Length > 0;
    public string MemberCountText => $"{Members.Count}명";
    public bool CanSave => Loaded && !Busy && Name.Trim().Length > 0 && Members.Count > 0 && (!IsNew || GroupId.Trim().Length > 0);

    partial void OnNameChanged(string value) => OnPropertyChanged(nameof(CanSave));
    partial void OnGroupIdChanged(string value) { OnPropertyChanged(nameof(Uri)); OnPropertyChanged(nameof(CanSave)); }
    partial void OnBusyChanged(bool value) => OnPropertyChanged(nameof(CanSave));
    partial void OnLoadedChanged(bool value) => OnPropertyChanged(nameof(CanSave));
    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();

    private async Task LoadAsync()
    {
        Busy = true;
        var r = await _s.GetGroupAsync(_existing!);
        Busy = false;
        if (!r.Ok) { Error = ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason); return; }
        var d = r.Value;
        _ifMatch = d.ETag.Length > 0 ? d.ETag : _existing!.Etag;
        _orgCode = d.OrgCode;
        Name = d.DisplayName; GroupId = UserPartConverter.UserPart(d.Uri.Length > 0 ? d.Uri : _existing!.Uri);
        SessionType = SessionTypes.Contains(d.SessionType) ? d.SessionType : "prearranged";
        VideoEnabled = d.VideoEnabled; AllowSds = d.AllowSds; AllowFd = d.AllowFd; EmergencyCall = d.EmergencyCall; EmergencyAlert = d.EmergencyAlert;
        RequireAffiliation = d.RequireAffiliation; Encryption = d.Encryption; Priority = d.Priority; MaxParticipants = d.MaxParticipants;
        Members.Clear();
        foreach (var m in d.Members) AddMember(m.Uri, m.Name, m.Role == "chair");
        Loaded = true;
        Filter();
    }

    private void AddMember(string uri, string name, bool chair)
    {
        string number = UserPartConverter.UserPart(uri);
        if (Members.Any(m => DirectoryService.Normalize(UserPartConverter.UserPart(m.Uri)) == DirectoryService.Normalize(number))) return;
        string n = name.Length > 0 ? name : _s.Directory.NameOf(number);
        Members.Add(new GroupMemberRow(uri, n, _s.Directory.DisplayNumber(number), _s.IsMe(uri), chair));
        OnPropertyChanged(nameof(MemberCountText)); OnPropertyChanged(nameof(CanSave));
    }

    private void Filter()
    {
        Candidates.Clear();
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        var taken = Members.Select(m => DirectoryService.Normalize(UserPartConverter.UserPart(m.Uri))).ToHashSet();
        var d = _s.Directory;
        foreach (var c in d.PttUsers)
        {
            if (taken.Contains(DirectoryService.Normalize(c.Number))) continue;
            string disp = d.DisplayNumber(c.Number);
            if (q.Length > 0 && !c.Name.Contains(q, StringComparison.OrdinalIgnoreCase)
                && !(qn.Length > 0 && (DirectoryService.Normalize(c.Number).Contains(qn) || DirectoryService.Normalize(disp).Contains(qn)))) continue;
            Candidates.Add(new GroupCandidateRow(c, disp, d.OrgPath(c.OrgCode)));
            if (Candidates.Count >= 200) break;
        }
    }

    [RelayCommand] private void Add(GroupCandidateRow c) { AddMember(_s.ToTelUri(c.Contact.Number), c.Contact.Name, chair: false); Filter(); }
    [RelayCommand] private void AddAllShown() { foreach (var c in Candidates.ToList()) AddMember(_s.ToTelUri(c.Contact.Number), c.Contact.Name, chair: false); Filter(); }
    [RelayCommand] private void Remove(GroupMemberRow m) { Members.Remove(m); OnPropertyChanged(nameof(MemberCountText)); OnPropertyChanged(nameof(CanSave)); Filter(); }
    [RelayCommand] private void ToggleChair(GroupMemberRow m) => m.IsChair = !m.IsChair;

    [RelayCommand]
    private async Task Save()
    {
        if (!CanSave) return;
        Error = "";
        var doc = new GroupDoc
        {
            Uri = Uri, DisplayName = Name.Trim(), SessionType = SessionType, VideoEnabled = VideoEnabled, AllowSds = AllowSds, AllowFd = AllowFd,
            EmergencyCall = EmergencyCall, EmergencyAlert = EmergencyAlert, RequireAffiliation = RequireAffiliation, Encryption = Encryption,
            Priority = Math.Clamp(Priority, 0, 15), MaxParticipants = Math.Max(0, MaxParticipants), OrgCode = _orgCode,
        };
        foreach (var m in Members)
            doc.Members.Add(new GroupMember { Uri = m.Uri, Name = m.Name, Role = m.IsChair ? "chair" : "participant", Priority = m.IsChair ? 7 : 5 });
        Busy = true;
        var r = await _s.SaveGroupAsync(doc, IsNew ? null : _ifMatch);       // 409 uri_taken 재시도는 세션이 처리
        Busy = false;
        if (!r.Ok)
        {
            Error = ResponseText.Describe(ResponseText.Area.Group, r.Code, r.Reason);
            if (r.Code == 412 && !IsNew) _ = LoadAsync();                     // 타인이 먼저 갱신 — 최신 문서로 다시 편집
            return;
        }
        if (IsNew && r.Value.Uri.Length > 0) GroupId = UserPartConverter.UserPart(r.Value.Uri);   // 응답 문서 uri 가 정본(재시도로 바뀔 수 있다)
        Saved?.Invoke(this, EventArgs.Empty);
    }
}
