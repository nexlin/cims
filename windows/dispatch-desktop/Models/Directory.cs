// 주소록 항목 — 관제 그룹원(내선)·조직 연락처(외부망)·PTT 사용자·그룹 (§4.1·§4.3, 소스는 §13).
namespace DispatchDesktop.Models;

public enum ContactKind { Extension, External, PttUser, PttGroup }

public sealed record Contact(ContactKind Kind, string Number, string Name, IReadOnlyList<string> Tags)
{
    /// <summary>관제 그룹원(BLF·띠 대상) — tags 에 member.</summary>
    public bool IsMember => Tags.Contains("member", StringComparer.OrdinalIgnoreCase);
    public bool IsExternal => Kind == ContactKind.External;
    public string Display => Name.Length > 0 ? $"{Name} {Number}" : Number;
    public string KindText => Kind switch
    {
        ContactKind.Extension => "내선", ContactKind.External => "외부", ContactKind.PttUser => "PTT", _ => "그룹",
    };
}
