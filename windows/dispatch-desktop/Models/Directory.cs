// 주소록 항목 — 서버 회사 전화번호부(`/provisioning/directory` 조직 트리 + 가입자, android_ue_provisioning.md §3-1)를 정본으로 하고
// 로컬 CSV(외부망 번호·관제 그룹원 표시)를 보탠다 (§4.1·§4.3, 소스 규약은 §13).
namespace DispatchDesktop.Models;

public enum ContactKind { Extension, External, PttUser, PttGroup }

/// <summary>조직 노드 — 코드/이름/상위 코드/정렬 (organizations 트리).</summary>
public sealed record OrgNode(string Code, string Name, string Parent, int Sort);

public sealed record Contact(ContactKind Kind, string Number, string Name, IReadOnlyList<string> Tags, string OrgCode = "")
{
    /// <summary>관제 그룹원(BLF·띠 대상) — tags 에 member.</summary>
    public bool IsMember => Tags.Contains("member", StringComparer.OrdinalIgnoreCase);
    public bool IsExternal => Kind == ContactKind.External;
    public bool IsServer => Tags.Contains("server", StringComparer.OrdinalIgnoreCase);
    public string Display => Name.Length > 0 ? $"{Name} {Number}" : Number;
    public string KindText => Kind switch
    {
        ContactKind.Extension => "내선", ContactKind.External => "외부", ContactKind.PttUser => "PTT", _ => "그룹",
    };
}
