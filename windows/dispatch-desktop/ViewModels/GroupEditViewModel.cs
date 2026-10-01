// PTT 그룹 생성·편집 폼 — GMS 그룹 문서(GroupDoc)를 폼으로. [PTT 그룹] 화면(§4.7) 상세 카드 자리에 인라인으로 뜬다(GroupAdminViewModel.Editor,
// 뷰 = Views/GroupEditView). 저장 = XCAP PUT(TS 24.481, 본인 소유 또는 관리 범위). 멤버 후보 = PTT 주소록(서버 전화번호부 service=ptt).
// 편집은 열 때의 ETag 를 If-Match 로 보내 충돌(412)을 잡는다.
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
    /// <summary>필수 멤버 &lt;on-network-required&gt;(TS 24.481 §7.2.4.2) — 개시자 응답 전에 이 멤버의 200 을 기다린다(TNG1, TS 24.379 §6.3.3.3).</summary>
    [ObservableProperty] private bool _required;
    private readonly bool _readChair;
    private readonly int _readPriority;
    public GroupMemberRow(string uri, string name, string displayNumber, bool isMe, bool isChair, bool required = false, int? priority = null)
    {
        Uri = uri; Name = name; DisplayNumber = displayNumber; IsMe = isMe; _isChair = isChair; _required = required;
        _readChair = isChair; _readPriority = priority ?? DefaultPriority(isChair);
    }
    public string Label => Name.Length > 0 ? Name : DisplayNumber;
    public string RoleText => IsChair ? "의장" : "참가자";
    public string RequiredText => Required ? "필수" : "선택";
    /// <summary>저장할 멤버 우선순위(mcpttgi user-priority) — 역할을 바꾸지 않았으면 읽은 값 그대로(콘솔이 준 멤버별 값을 지우지 않는다 —
    /// PUT 은 멤버 목록 전체 교체), 바꿨으면 역할 기본값(의장 7 · 참가자 5).</summary>
    public int Priority => IsChair == _readChair ? _readPriority : DefaultPriority(IsChair);
    private static int DefaultPriority(bool chair) => chair ? 7 : 5;
    partial void OnIsChairChanged(bool value) => OnPropertyChanged(nameof(RoleText));
    partial void OnRequiredChanged(bool value) => OnPropertyChanged(nameof(RequiredText));
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

    /// <summary>그룹 종류(TS 24.481 on-network-invite-members) — 일제 통화는 그룹 종류가 아니라 호 속성이라 없다(mcptt_broadcast_group_call.md §3.1).
    /// 인스턴스 프로퍼티 — WPF 바인딩은 static 멤버를 경로로 풀지 못한다.</summary>
    public IReadOnlyList<string> SessionTypes { get; } = new[] { "prearranged", "chat" };
    /// <summary>TNG1 만료 동작(on-network-action-upon-expiration-of-timeout-for-acknowledgement-of-required-members) — 값, 표시.</summary>
    public IReadOnlyList<KeyValuePair<string, string>> AckActions { get; } = new[]
    {
        new KeyValuePair<string, string>("abandon", "통화 포기"), new KeyValuePair<string, string>("proceed", "없이 진행"),
    };

    public const int HangTimerDefault = 30, HangTimerMax = 3600;
    public const int MaxDurationDefault = 3600, MaxDurationMax = 86400;
    public const int MaxSdsSizeDefault = 10000, MaxAutoRecvDefault = 1048576;
    public const int MinNumberToStartMax = 65535;
    public const int AckTimeoutDefault = 5, AckTimeoutMax = 300;
    // MCVideo 속성(TS 24.481 §7.2.2) — 범위·기본값은 서버(csc services/mcvideo.py GROUP_ATTR_DEFAULTS·validate_attrs)와 같다.
    public const int McvMaxTransmittersDefault = 2, McvMaxTransmittersMax = 16;
    public const int McvMaxDurationDefault = 3600, McvReceptionHangDefault = 30, McvReceptionHangMax = 3600;
    /// <summary>MCVideo 호 방식(mcvideo-on-network-invite-members) — 값, 표시. 기본 chat(mcvideo.md D5).</summary>
    public IReadOnlyList<KeyValuePair<string, string>> McVideoTypes { get; } = new[]
    {
        new KeyValuePair<string, string>("chat", "chat — 원하는 사람이 합류"), new KeyValuePair<string, string>("prearranged", "편성 — 제휴 멤버를 초대"),
    };
    /// <summary>열 때 받은 문서(신규 = null) — 미기재 칸 판정.</summary>
    private GroupDoc? _read;

    [ObservableProperty] private string _name = "";
    /// <summary>그룹 id(uri user part) — 신규만 편집 가능.</summary>
    [ObservableProperty] private string _groupId = "";
    [ObservableProperty] private string _sessionType = "prearranged";
    [ObservableProperty] private bool _allowSds = true;
    [ObservableProperty] private bool _allowFd;
    [ObservableProperty] private bool _emergencyCall = true;
    [ObservableProperty] private bool _emergencyAlert = true;
    [ObservableProperty] private bool _requireAffiliation = true;
    [ObservableProperty] private bool _encryption;
    [ObservableProperty] private int _priority = 5;
    [ObservableProperty] private int _maxParticipants;
    // 그룹 호 타이머·참가자 정보·MCData 한도·확인 통화 설정(TS 24.481 §7.2.2·§7.2.4.2) — 기본값·범위는 콘솔 그룹 편집과 같다.
    //   문서에 없던(미기재) 칸은 기본값 그대로면 저장해도 미기재로 둔다(서버 값·기본값을 덮지 않는다 — WithUnset).
    [ObservableProperty] private int _hangTimerSec = HangTimerDefault;
    [ObservableProperty] private int _maxDurationSec = MaxDurationDefault;
    [ObservableProperty] private bool _allowConferenceState = true;
    [ObservableProperty] private int _maxSdsSize = MaxSdsSizeDefault;
    [ObservableProperty] private int _maxAutoRecv = MaxAutoRecvDefault;
    [ObservableProperty] private int _minNumberToStart;
    [ObservableProperty] private int _ackTimeoutSec = AckTimeoutDefault;
    [ObservableProperty] private string _ackAction = "abandon";
    // MCVideo 그룹 영상(§10.6) — 켜기 = GroupDoc.Mcvideo 객체, 끄기는 이 앱에서 못 한다(서버가 MCVideo <service> 없는 PUT 을 «그대로 둠» 으로 읽는 전환기, mcvideo.md §5.1).
    [ObservableProperty] private bool _mcVideo;
    /// <summary>읽은 문서에서 이미 켜져 있었다 — 스위치를 잠근다(끄기는 콘솔).</summary>
    [ObservableProperty] private bool _mcVideoLocked;
    [ObservableProperty] private string _mcVideoType = "chat";
    [ObservableProperty] private int _mcVideoMaxTransmitters = McvMaxTransmittersDefault;
    [ObservableProperty] private int _mcVideoMaxDurationSec = McvMaxDurationDefault;
    [ObservableProperty] private int _mcVideoReceptionHangSec = McvReceptionHangDefault;
    [ObservableProperty] private int _mcVideoMinNumberToStart;
    /// <summary>mcvideo-on-network-group-priority 0~255 — 빈 값 = 미기재(가장 낮음).</summary>
    [ObservableProperty] private string _mcVideoGroupPriority = "";
    [ObservableProperty] private bool _mcVideoAllowConferenceState = true;
    /// <summary>열 때 받은 MCVideo 몫(없으면 null) — 폼에 없는 값(코덱·해상도·실시간 모드·규칙 action)을 되돌린다.</summary>
    private McVideoGroupAttrs? _readMcVideo;
    public bool CanToggleMcVideo => !McVideoLocked;
    public string McVideoTip => McVideoLocked
        ? "끄기는 운영 콘솔에서 — 이 앱의 저장(XCAP PUT)은 MCVideo 를 끄지 못합니다(서버 전환기 — MCVideo 서비스가 없는 PUT 은 그대로 둔다)"
        : "켜면 이 그룹이 MCVideo 그룹이 된다(TS 24.481 §7.2.2) — 멤버는 같은 그룹에서 음성 무전과 따로 영상 호를 연다";
    partial void OnMcVideoLockedChanged(bool value) { OnPropertyChanged(nameof(CanToggleMcVideo)); OnPropertyChanged(nameof(McVideoTip)); }
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private bool _busy;
    /// <summary>편집 문서를 받는 중(신규는 즉시 true).</summary>
    [ObservableProperty] private bool _loaded;

    public ObservableCollection<GroupMemberRow> Members { get; } = new();
    public ObservableCollection<GroupCandidateRow> Candidates { get; } = new();

    public event EventHandler? Saved;
    /// <summary>[취소] — 호스트(GroupAdminViewModel)가 폼을 닫는다.</summary>
    public event EventHandler? Cancelled;

    public GroupEditViewModel(DispatchSession s, GroupInfo? existing)
    {
        _s = s; _existing = existing;
        if (existing is null)
        {
            GroupId = UserPartConverter.UserPart(s.NewGroupUri());
            // 관리 범위가 있으면 새 그룹을 데스크 소속 조직(dispatch.orgCode)에 귀속 — 같은 범위의 다른 관제사 관리 창에도 보인다(§4.5).
            if (s.CanManageDirectory) _orgCode = s.Dispatch.OrgCode;
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
        AllowSds = d.AllowSds; AllowFd = d.AllowFd; EmergencyCall = d.EmergencyCall; EmergencyAlert = d.EmergencyAlert;
        RequireAffiliation = d.RequireAffiliation; Encryption = d.Encryption; Priority = d.Priority; MaxParticipants = d.MaxParticipants;
        _read = d;
        HangTimerSec = d.HangTimerSec ?? HangTimerDefault; MaxDurationSec = d.MaxDurationSec ?? MaxDurationDefault;
        AllowConferenceState = d.AllowConferenceState ?? true;
        MaxSdsSize = d.MaxSdsSize ?? MaxSdsSizeDefault; MaxAutoRecv = d.MaxAutoRecv ?? MaxAutoRecvDefault;
        MinNumberToStart = d.MinNumberToStart ?? 0; AckTimeoutSec = d.AckTimeoutSec ?? AckTimeoutDefault;
        AckAction = d.AckAction is "proceed" ? "proceed" : "abandon";
        _readMcVideo = d.Mcvideo;
        McVideo = McVideoLocked = d.Mcvideo is not null;
        if (d.Mcvideo is { } mv)
        {
            McVideoType = mv.InviteMembers ? "prearranged" : "chat";
            McVideoMaxTransmitters = mv.MaxTransmitters ?? McvMaxTransmittersDefault; McVideoMaxDurationSec = mv.MaxDurationSec ?? McvMaxDurationDefault;
            McVideoReceptionHangSec = mv.ReceptionHangTimerSec ?? McvReceptionHangDefault; McVideoMinNumberToStart = mv.MinNumberToStart ?? 0;
            McVideoGroupPriority = mv.GroupPriority?.ToString() ?? ""; McVideoAllowConferenceState = mv.AllowConferenceState ?? true;
        }
        Members.Clear();
        foreach (var m in d.Members) AddMember(m.Uri, m.Name, m.Role == "chair", m.Required, m.Priority);
        Loaded = true;
        Filter();
    }

    /// <summary>[사용자] 패널에서 고른 사람을 새 그룹 멤버로(참가자) — 패널 «새 PTT 그룹».</summary>
    public void AddMembers(IEnumerable<(string Number, string Name)> people)
    {
        foreach (var (number, name) in people) AddMember(_s.ToTelUri(number), name, chair: false);
        Filter();
    }

    /// <summary>패널 «새 PTT 그룹» 의 세션 종류 알약.</summary>
    public bool IsPrearranged => SessionType == "prearranged";
    public bool IsChat => SessionType == "chat";
    [RelayCommand] private void SetSessionType(string t) { if (SessionTypes.Contains(t)) SessionType = t; }
    partial void OnSessionTypeChanged(string value) { OnPropertyChanged(nameof(IsPrearranged)); OnPropertyChanged(nameof(IsChat)); }

    private void AddMember(string uri, string name, bool chair, bool required = false, int? priority = null)
    {
        string number = UserPartConverter.UserPart(uri);
        if (Members.Any(m => DirectoryService.Normalize(UserPartConverter.UserPart(m.Uri)) == DirectoryService.Normalize(number))) return;
        string n = name.Length > 0 ? name : _s.Directory.NameOf(number);
        Members.Add(new GroupMemberRow(uri, n, _s.Directory.DisplayNumber(number), _s.IsMe(uri), chair, required, priority));
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
    [RelayCommand] private void ToggleRequired(GroupMemberRow m) => m.Required = !m.Required;

    /// <summary>MCVideo 몫 — 읽은 것(없으면 새것)에 폼 값을 얹는다. 보호 둘은 false 명시(요소가 없으면 true 로 읽힌다 — TS 24.481 §7.2.8, mcvideo.md D7).
    /// 새로 켜는 그룹의 코덱은 서버 기본값(AMR-WB · H264)을 싣는다.</summary>
    private McVideoGroupAttrs BuildMcVideo()
    {
        var r = _readMcVideo;
        var a = r ?? new McVideoGroupAttrs { AudioEncodings = new() { "AMR-WB" }, VideoEncodings = new() { "H264" } };
        a.ProtectMedia = false; a.ProtectTransmissionControl = false;
        a.InviteMembers = McVideoType == "prearranged";
        a.MaxTransmitters = WithUnset(r?.MaxTransmitters, Math.Clamp(McVideoMaxTransmitters, 1, McvMaxTransmittersMax), McvMaxTransmittersDefault);
        a.MaxDurationSec = WithUnset(r?.MaxDurationSec, Math.Clamp(McVideoMaxDurationSec, 0, MaxDurationMax), McvMaxDurationDefault);
        a.ReceptionHangTimerSec = WithUnset(r?.ReceptionHangTimerSec, Math.Clamp(McVideoReceptionHangSec, 0, McvReceptionHangMax), McvReceptionHangDefault);
        a.MinNumberToStart = WithUnset(r?.MinNumberToStart, Math.Clamp(McVideoMinNumberToStart, 0, MinNumberToStartMax), 0);
        a.GroupPriority = int.TryParse(McVideoGroupPriority.Trim(), out int gp) ? Math.Clamp(gp, 0, 255) : null;
        a.AllowConferenceState = r?.AllowConferenceState is null && McVideoAllowConferenceState ? null : McVideoAllowConferenceState;
        return a;
    }

    /// <summary>문서에 없던 칸(read = null)이 기본값 그대로면 미기재(null) — 폼을 연 것만으로 서버 값을 명시값으로 굳히지 않는다.</summary>
    private static int? WithUnset(int? read, int value, int dflt) => read is null && value == dflt ? null : value;
    [RelayCommand] private void Cancel() => Cancelled?.Invoke(this, EventArgs.Empty);

    [RelayCommand]
    private async Task Save()
    {
        if (!CanSave) return;
        Error = "";
        var doc = new GroupDoc
        {
            Uri = Uri, DisplayName = Name.Trim(), SessionType = SessionType, AllowSds = AllowSds, AllowFd = AllowFd,
            EmergencyCall = EmergencyCall, EmergencyAlert = EmergencyAlert, RequireAffiliation = RequireAffiliation, Encryption = Encryption,
            Priority = Math.Clamp(Priority, 0, 15), MaxParticipants = Math.Max(0, MaxParticipants), OrgCode = _orgCode,
            HangTimerSec = WithUnset(_read?.HangTimerSec, Math.Clamp(HangTimerSec, 0, HangTimerMax), HangTimerDefault),
            MaxDurationSec = WithUnset(_read?.MaxDurationSec, Math.Clamp(MaxDurationSec, 0, MaxDurationMax), MaxDurationDefault),
            AllowConferenceState = _read?.AllowConferenceState is null && AllowConferenceState ? null : AllowConferenceState,
            MaxSdsSize = WithUnset(_read?.MaxSdsSize, Math.Max(0, MaxSdsSize), MaxSdsSizeDefault),
            MaxAutoRecv = WithUnset(_read?.MaxAutoRecv, Math.Max(0, MaxAutoRecv), MaxAutoRecvDefault),
            MinNumberToStart = WithUnset(_read?.MinNumberToStart, Math.Clamp(MinNumberToStart, 0, MinNumberToStartMax), 0),
            AckTimeoutSec = WithUnset(_read?.AckTimeoutSec, Math.Clamp(AckTimeoutSec, 1, AckTimeoutMax), AckTimeoutDefault),
            AckAction = _read?.AckAction is null && AckAction == "abandon" ? null : AckAction,
            Mcvideo = McVideo ? BuildMcVideo() : null,
        };
        foreach (var m in Members)
            doc.Members.Add(new GroupMember { Uri = m.Uri, Name = m.Name, Role = m.IsChair ? "chair" : "participant", Priority = m.Priority,
                                              Required = m.Required });
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
