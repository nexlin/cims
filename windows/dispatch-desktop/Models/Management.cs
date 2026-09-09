// 관리 창(§4.5) 모델 — 서버 `/provisioning/directory/{admin,orgs,members,groups}`·`/provisioning/recordings` 응답의 앱 표현
// (계약 android_ue_provisioning.md §3-2/§3-3). 조직/구성원/번호는 서버가 관리 범위(dispatch.directoryAdmin)로 걸러 준 것만 온다.
namespace DispatchDesktop.Models;

/// <summary>관리 범위 — DirectoryAdmin = own|all, OrgCode = own 의 루트 조직.</summary>
public sealed record AdminScope(string GroupId, string DirectoryAdmin, string OrgCode);

/// <summary>접속서비스 후보(service_ref) — 번호 개설 폼의 콤보.</summary>
public sealed record ServiceRef(string Kind, string Name, string Domain)
{
    public string Label => Domain.Length > 0 ? $"{Name} ({Domain})" : Name;
}

/// <summary>가입(번호) — VoLTE 또는 PTT 회선 하나. Profile 은 PTT 회선의 자격 플래그(없으면 null).</summary>
public sealed record NumberInfo(string Msisdn, string Imsi, string ServiceRef, string SipTransport, string AuthScheme,
                                IReadOnlyDictionary<string, bool>? Profile);

/// <summary>구성원(person) + 회선 둘.</summary>
public sealed record MemberInfo(long UserId, string Name, string LoginId, string Org, string Title, NumberInfo? Volte, NumberInfo? Ptt);

/// <summary>관리 화면 한 벌 — GET /provisioning/directory/admin.</summary>
public sealed record AdminView(AdminScope Scope, IReadOnlyList<ServiceRef> Services, IReadOnlyList<OrgNode> Orgs,
                               IReadOnlyList<MemberInfo> Members, string ETag);

/// <summary>구성원 입력(생성·수정) — null 필드는 보내지 않는다.</summary>
public sealed class MemberInput
{
    public string? Name { get; set; }
    public string? Org { get; set; }
    public string? Title { get; set; }
    public string? LoginId { get; set; }
    public string? Password { get; set; }
    public NumberInput? Volte { get; set; }
    public NumberInput? Ptt { get; set; }
}

/// <summary>번호 입력 — Msisdn 필수, Imsi 비면 서버가 번호 숫자로(USIM 없는 관제 소프트폰 규약), Password 는 개설·번호 변경 시 필수.</summary>
public sealed class NumberInput
{
    public string Msisdn { get; set; } = "";
    public string Imsi { get; set; } = "";
    public string ServiceRef { get; set; } = "";
    public string SipTransport { get; set; } = "TLS";
    public string Password { get; set; } = "";
}

/// <summary>관리 범위 안 PTT 그룹(GET /provisioning/directory/groups) — 멤버가 아니어도 보인다.</summary>
/// <summary>관리 화면의 PTT 그룹 행(GET /provisioning/directory/groups) — 보이는 범위 = 관리 범위 ∪ 내 소유 ∪ 청취 범위 ∪ 멤버 그룹,
/// 편집·삭제는 CanManage(관리 범위 안 또는 내 소유 — 서버 GMS 게이트와 같은 판정) 인 행만.</summary>
public sealed record ManagedGroup(string Id, string Uri, string Name, int MemberCount, bool IsOwner, string OrgCode, string SessionType, string ETag,
                                  bool CanManage = true, bool InListenScope = false, bool IsMember = false);

/// <summary>슬롯 트랙 안의 화자 구간(세그먼트 시작 기준 offset).</summary>
public sealed record SpeakerSpan(string Id, int OffsetMs, int DurMs);

/// <summary>녹취 세그먼트의 슬롯 트랙(동시 발언·전이중 private call) — Kind audio|video, Slot = PTT 슬롯 번호(VoIP 는 -1).</summary>
public sealed record SegmentTrack(int Slot, string Kind, IReadOnlyList<SpeakerSpan> Speakers, bool HasVideo, string Status);

/// <summary>녹취 세그먼트(OAM handlers/recording.py 세그먼트 항목 중 앱이 쓰는 것).</summary>
public sealed record RecordingSegment(int Seq, string Type, string SpeakerId, DateTime? Start, DateTime? End, int DurationMs, bool HasVideo,
                                      string Status, IReadOnlyList<string> SpeakerIds, int TalkerCount)
{
    /// <summary>슬롯 트랙(발언 턴 = 화자 구간 — 콘솔 segTurns 와 같은 해석). 없으면 세그먼트 전체가 대표 화자의 한 턴.</summary>
    public IReadOnlyList<SegmentTrack> Tracks { get; init; } = Array.Empty<SegmentTrack>();
    public string Label => Type == "ptt" ? (SpeakerIds.Count > 1 ? $"#{Seq} {string.Join(", ", SpeakerIds)}" : $"#{Seq} {SpeakerId}") : $"#{Seq}";
    public string DurationText => TimeSpan.FromMilliseconds(Math.Max(0, DurationMs)).ToString(@"mm\:ss");
}

/// <summary>녹취 세션(GET /provisioning/recordings/{id}).</summary>
public sealed record RecordingInfo(string Id, string CallType, string Caller, string Callee, string GroupId, DateTime? Start, DateTime? End,
                                   int DurationSec, string Status, IReadOnlyList<RecordingSegment> Segments);
