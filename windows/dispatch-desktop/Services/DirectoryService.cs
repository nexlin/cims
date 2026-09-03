// 주소록 — 관제 그룹원(내선)·조직 연락처(외부망)·PTT 사용자 (초기형 = 로컬 CSV + 프로비저닝 + GMS 그룹 목록, §13).
//
// CSV: kind,number,name,tags — kind = ext | external | ptt, tags 는 ';' 구분(member = 관제 그룹원). 그룹은 GMS listGroups 로 채운다.
using System.Globalization;
using DispatchDesktop.Models;

using System.IO;

namespace DispatchDesktop.Services;

public sealed class DirectoryService
{
    private readonly List<Contact> _contacts = new();
    private readonly Dictionary<string, string> _names = new(StringComparer.OrdinalIgnoreCase);

    public IReadOnlyList<Contact> Contacts => _contacts;
    public string? LoadedFrom { get; private set; }
    public event EventHandler? Changed;

    /// <summary>설정 경로 → %APPDATA% directory.csv → 앱 옆 directory.sample.csv.</summary>
    public void Load(string? overridePath)
    {
        string[] candidates =
        {
            overridePath ?? "",
            AppPaths.DirectoryCsv,
            Path.Combine(AppContext.BaseDirectory, "directory.sample.csv"),
        };
        foreach (string p in candidates)
        {
            if (p.Length == 0 || !File.Exists(p)) continue;
            LoadCsv(p);
            LoadedFrom = p;
            break;
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private void LoadCsv(string path)
    {
        _contacts.Clear();
        _names.Clear();
        foreach (string raw in File.ReadLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#') || line.StartsWith("kind,", StringComparison.OrdinalIgnoreCase)) continue;
            string[] f = line.Split(',');
            if (f.Length < 2) continue;
            var kind = f[0].Trim().ToLowerInvariant() switch
            {
                "ext" or "extension" => ContactKind.Extension,
                "external" or "ext-out" => ContactKind.External,
                "ptt" => ContactKind.PttUser,
                "group" => ContactKind.PttGroup,
                _ => ContactKind.External,
            };
            string number = f[1].Trim();
            string name = f.Length > 2 ? f[2].Trim() : "";
            var tags = f.Length > 3 ? f[3].Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries) : Array.Empty<string>();
            Add(new Contact(kind, number, name, tags));
        }
    }

    public void Add(Contact c)
    {
        _contacts.RemoveAll(x => x.Kind == c.Kind && x.Number == c.Number);
        _contacts.Add(c);
        if (c.Name.Length > 0) _names[Normalize(c.Number)] = c.Name;
    }

    /// <summary>GMS 그룹 목록 → 그룹 항목 갱신(멤버 수 포함).</summary>
    public void SetGroups(IEnumerable<GroupInfo> groups)
    {
        _contacts.RemoveAll(x => x.Kind == ContactKind.PttGroup);
        foreach (var g in groups) _contacts.Add(new Contact(ContactKind.PttGroup, g.Id, g.Name, new[] { g.MemberCount.ToString(CultureInfo.InvariantCulture) }));
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>관제 그룹원 내선(BLF 대상).</summary>
    public IReadOnlyList<Contact> Members => _contacts.Where(c => c.Kind == ContactKind.Extension && c.IsMember).ToList();
    public IReadOnlyList<Contact> PttUsers => _contacts.Where(c => c.Kind == ContactKind.PttUser).ToList();
    public IReadOnlyList<Contact> Groups => _contacts.Where(c => c.Kind == ContactKind.PttGroup).ToList();
    public IReadOnlyList<Contact> CallBook => _contacts.Where(c => c.Kind is ContactKind.Extension or ContactKind.External).ToList();

    /// <summary>번호/URI → 표시 이름. 없으면 빈 문자열.</summary>
    public string NameOf(string? numberOrUri)
    {
        if (string.IsNullOrEmpty(numberOrUri)) return "";
        return _names.TryGetValue(Normalize(Converters.UserPartConverter.UserPart(numberOrUri)), out string? n) ? n : "";
    }

    /// <summary>"1003 이순경" 형태 병기(§3.2 신원 표시). 이름 없으면 번호만.</summary>
    public string Label(string? numberOrUri)
    {
        string num = Converters.UserPartConverter.UserPart(numberOrUri);
        string name = NameOf(num);
        return name.Length > 0 ? $"{num} {name}" : num;
    }

    public bool IsExternal(string number)
    {
        var c = _contacts.FirstOrDefault(x => Normalize(x.Number) == Normalize(number));
        if (c is not null) return c.Kind == ContactKind.External;
        string digits = Normalize(number);
        return digits.Length > 6;                       // 내선 규약(짧은 번호) 밖이면 외부망으로 본다
    }

    private static string Normalize(string s)
    {
        var sb = new System.Text.StringBuilder(s.Length);
        foreach (char c in s) if (char.IsLetterOrDigit(c) || c == '+') sb.Append(c);
        return sb.ToString();
    }
}
