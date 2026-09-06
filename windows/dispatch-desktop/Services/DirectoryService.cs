// 주소록 — 서버 회사 전화번호부 + 로컬 CSV (§4.1·§4.3, android_ue_provisioning.md §3-1).
//
//  서버: `GET /provisioning/directory?service=volte|ptt` → {orgs:[{code,name,parent,sort}], entries:[{org,name,msisdn}]}. ETag 로 버전 동기화
//        (304 = 변경 없음), 본문은 %APPDATA% directory-cache.json 에 캐시해 오프라인·재기동 시 먼저 그린다. Android 와 같은 소스.
//  CSV:  kind,number,name,tags — kind = ext | external | ptt, tags 는 ';' 구분(member = 관제 그룹원). 외부망 번호와 관제 그룹원 표시처럼
//        서버가 아직 주지 않는 것(§13)을 보탠다. 같은 번호가 양쪽에 있으면 서버 이름 + CSV 태그를 합친다.
//  번호 표기: 프로비저닝 countryCode 와 같은 국가는 로컬 표기(+821300000001 → 01300000001) — 표시 전용, 키는 원본.
using System.Globalization;
using System.IO;
using System.Text.Json;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed class DirectoryService
{
    private readonly List<Contact> _csv = new();
    private readonly Dictionary<string, List<Contact>> _server = new(StringComparer.OrdinalIgnoreCase);   // service → contacts
    private readonly Dictionary<string, string> _etags = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, string> _bodies = new(StringComparer.OrdinalIgnoreCase);
    private List<OrgNode> _orgs = new();
    private List<Contact> _merged = new();
    private readonly Dictionary<string, string> _names = new(StringComparer.OrdinalIgnoreCase);

    public IReadOnlyList<Contact> Contacts => _merged;
    public IReadOnlyList<OrgNode> Orgs => _orgs;
    public string? LoadedFrom { get; private set; }
    /// <summary>홈 국가코드(프로비저닝 countryCode, 예 "82") — 로컬 표기용.</summary>
    public string CountryCode { get; set; } = "";
    public DateTime? ServerSyncedAt { get; private set; }
    public event EventHandler? Changed;

    private static string CachePath => Path.Combine(AppPaths.Root, "directory-cache.json");

    /// <summary>CSV(설정 경로 → %APPDATA% directory.csv → 앱 옆 directory.sample.csv) + 서버 캐시.</summary>
    public void Load(string? overridePath)
    {
        string[] candidates = { overridePath ?? "", AppPaths.DirectoryCsv, Path.Combine(AppContext.BaseDirectory, "directory.sample.csv") };
        _csv.Clear();
        LoadedFrom = null;
        foreach (string p in candidates)
        {
            if (p.Length == 0 || !File.Exists(p)) continue;
            try { LoadCsv(p); }
            catch (IOException) { _csv.Clear(); continue; }        // Excel 등이 열어 잠근 파일 — 다음 후보로(기동을 막지 않는다)
            catch (UnauthorizedAccessException) { _csv.Clear(); continue; }
            LoadedFrom = p;
            break;
        }
        LoadCache();
        Rebuild();
    }

    private void LoadCsv(string path)
    {
        foreach (string raw in File.ReadLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith('#') || line.StartsWith("kind,", StringComparison.OrdinalIgnoreCase)) continue;
            string[] f = SplitCsv(line);
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
            _csv.RemoveAll(x => x.Kind == kind && Normalize(x.Number) == Normalize(number));
            _csv.Add(new Contact(kind, number, name, tags));
        }
    }

    /// <summary>CSV 한 줄 분리 — 큰따옴표 필드 안의 쉼표("김철수, 팀장")와 "" 이스케이프를 지킨다(RFC 4180).</summary>
    internal static string[] SplitCsv(string line)
    {
        var fields = new List<string>();
        var sb = new System.Text.StringBuilder();
        bool quoted = false;
        for (int i = 0; i < line.Length; ++i)
        {
            char c = line[i];
            if (quoted)
            {
                if (c == '"') { if (i + 1 < line.Length && line[i + 1] == '"') { sb.Append('"'); ++i; } else quoted = false; }
                else sb.Append(c);
            }
            else if (c == '"') quoted = true;
            else if (c == ',') { fields.Add(sb.ToString()); sb.Clear(); }
            else sb.Append(c);
        }
        fields.Add(sb.ToString());
        return fields.ToArray();
    }

    // ── 서버 전화번호부 ──
    public string? Etag(string service) => _etags.TryGetValue(service, out var e) && e.Length > 0 ? e : null;

    /// <summary>서버 응답 적용(200). 파싱 실패면 false.</summary>
    public bool ApplyServer(string service, string json, string etag)
    {
        if (!Parse(service, json)) return false;
        _etags[service] = etag;
        _bodies[service] = json;
        ServerSyncedAt = DateTime.Now;
        SaveCache();
        Rebuild();
        return true;
    }

    /// <summary>304 — 내용 유지, 동기화 시각만.</summary>
    public void TouchServer() => ServerSyncedAt = DateTime.Now;

    private bool Parse(string service, string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var orgs = new List<OrgNode>();
            if (root.TryGetProperty("orgs", out var oa) && oa.ValueKind == JsonValueKind.Array)
                foreach (var o in oa.EnumerateArray())
                    orgs.Add(new OrgNode(Str(o, "code"), Str(o, "name"), Str(o, "parent"), o.TryGetProperty("sort", out var s) && s.ValueKind == JsonValueKind.Number && s.TryGetInt32(out int n) ? n : 0));
            var list = new List<Contact>();
            var kind = service == "ptt" ? ContactKind.PttUser : ContactKind.Extension;
            if (root.TryGetProperty("entries", out var ea) && ea.ValueKind == JsonValueKind.Array)
                foreach (var e in ea.EnumerateArray())
                {
                    string num = Str(e, "msisdn");
                    if (num.Length == 0) num = Str(e, "number");
                    if (num.Length == 0) continue;
                    list.Add(new Contact(kind, num, Str(e, "name"), new[] { "server" }, Str(e, "org")));
                }
            if (orgs.Count > 0 || _orgs.Count == 0) _orgs = orgs.OrderBy(x => x.Sort).ThenBy(x => x.Name, StringComparer.CurrentCulture).ToList();
            _server[service] = list;
            return true;
        }
        catch (JsonException) { return false; }
    }

    private static string Str(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private void LoadCache()
    {
        try
        {
            if (!File.Exists(CachePath)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(CachePath));
            foreach (var p in doc.RootElement.EnumerateObject())
            {
                string etag = Str(p.Value, "etag"), body = Str(p.Value, "body");
                if (body.Length > 0 && Parse(p.Name, body)) { _etags[p.Name] = etag; _bodies[p.Name] = body; }
            }
        }
        catch (Exception) { /* 캐시 손상은 무시 — 서버 동기화가 다시 채운다 */ }
    }

    private void SaveCache()
    {
        try
        {
            AppPaths.Ensure();
            var obj = _bodies.ToDictionary(kv => kv.Key, kv => new { etag = _etags.TryGetValue(kv.Key, out var e) ? e : "", body = kv.Value });
            File.WriteAllText(CachePath, JsonSerializer.Serialize(obj));
        }
        catch (Exception) { }
    }

    // ── 병합 ──
    private void Rebuild()
    {
        var merged = new List<Contact>();
        var csvByKey = _csv.ToDictionary(c => Key(c.Kind, c.Number), c => c);
        foreach (var list in _server.Values)
            foreach (var s in list)
            {
                string k = Key(s.Kind, s.Number);
                if (csvByKey.Remove(k, out var c))
                    merged.Add(s with { Name = s.Name.Length > 0 ? s.Name : c.Name, Tags = s.Tags.Concat(c.Tags).Distinct(StringComparer.OrdinalIgnoreCase).ToList() });
                else merged.Add(s);
            }
        merged.AddRange(csvByKey.Values);
        _merged = merged;
        _names.Clear();
        foreach (var c in merged) if (c.Name.Length > 0) _names[Canonical(c.Number)] = c.Name;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    private string Key(ContactKind k, string number) => (k == ContactKind.PttUser ? "p:" : "v:") + Canonical(number);

    /// <summary>비교·병합 정규형 — 숫자·+ 만 남기고, 홈 국가의 로컬 표기(010…)는 E.164(+8210…)로 올린다. 내선처럼 짧은 번호(≤6자리)는 그대로.
    /// CSV `01012345678` 과 서버 `+821012345678` 이 한 연락처가 되고, IsExternal 이 서버 가입자를 외부망으로 오판하지 않게.</summary>
    public string Canonical(string number)
    {
        string n = Normalize(number);
        if (CountryCode.Length > 0 && n.Length > 6 && n.StartsWith('0') && !n.StartsWith("00", StringComparison.Ordinal))
            return "+" + CountryCode + n[1..];
        return n;
    }

    /// <summary>GMS 그룹 목록 → 그룹 항목 갱신(멤버 수 포함).</summary>
    public void SetGroups(IEnumerable<GroupInfo> groups)
    {
        _csv.RemoveAll(x => x.Kind == ContactKind.PttGroup);
        foreach (var g in groups) _csv.Add(new Contact(ContactKind.PttGroup, g.Id, g.Name, new[] { g.MemberCount.ToString(CultureInfo.InvariantCulture) }));
        Rebuild();
    }

    private readonly List<Contact> _serverMembers = new();        // 감시(dialog watch) 대상 전원 — 서버가 monitorScope 를 해석한 목록
    private readonly List<Contact> _serverDeskMembers = new();    // 그중 자기 관제 그룹원(groupId == dispatch.groupId) — ③ 띠
    /// <summary>프로비저닝 `dispatch.members[]`(정본, android_ue_provisioning.md §3) → 감시 대상 전원 + 자기 그룹원. 비면 CSV member 태그 폴백.
    /// 번호는 망 주소(volteAor)로 두고 내선 라벨(extension)은 이름에 병기한다 — dialog watch·발신이 곧바로 다이얼되게.</summary>
    public void SetMembers(IEnumerable<CimsUe.DispatchMember> members, string deskGroupId)
    {
        _serverMembers.Clear(); _serverDeskMembers.Clear();
        var all = members.ToList();
        bool hasGroupIds = all.Any(m => m.GroupId.Length > 0);          // 구 서버(groupId 없음)면 전원을 그룹원으로 본다
        foreach (var m in all)
        {
            string aor = Converters.UserPartConverter.UserPart(m.VolteAor);
            string number = aor.Length > 0 ? aor : m.Extension;
            if (number.Length == 0) continue;
            string name = m.Name.Length > 0 && m.Extension.Length > 0 && aor.Length > 0 ? $"{m.Name} {m.Extension}" : m.Name;
            bool desk = !hasGroupIds || (deskGroupId.Length > 0 && string.Equals(m.GroupId, deskGroupId, StringComparison.Ordinal));
            var c = new Contact(ContactKind.Extension, number, name, desk ? new[] { "server", "member" } : new[] { "server", "watch" });
            _serverMembers.Add(c);
            if (desk) _serverDeskMembers.Add(c);
        }
        Rebuild();
    }

    /// <summary>dialog 감시 대상 — 서버 목록이 있으면 전원(범위 밖 그룹원 포함), 없으면 CSV 그룹원.</summary>
    public IReadOnlyList<Contact> WatchTargets => _serverMembers.Count > 0
        ? _serverMembers.Select(m => m.Name.Length > 0 ? m : m with { Name = NameOf(m.Number) }).ToList()
        : Members;
    public bool HasServerMembers => _serverMembers.Count > 0;

    // ── 조회 ──
    /// <summary>관제 그룹원(③ 띠) — 프로비저닝 members[] 중 자기 관제 그룹(groupId == dispatch.groupId), 이름은 전화번호부로 보강. 없으면 CSV member 태그.</summary>
    public IReadOnlyList<Contact> Members => _serverMembers.Count > 0
        ? _serverDeskMembers.Select(m => m.Name.Length > 0 ? m : m with { Name = NameOf(m.Number) }).ToList()
        : _merged.Where(c => c.Kind == ContactKind.Extension && c.IsMember).ToList();
    public IReadOnlyList<Contact> PttUsers => _merged.Where(c => c.Kind == ContactKind.PttUser).ToList();
    public IReadOnlyList<Contact> Groups => _merged.Where(c => c.Kind == ContactKind.PttGroup).ToList();
    public IReadOnlyList<Contact> CallBook => _merged.Where(c => c.Kind is ContactKind.Extension or ContactKind.External).ToList();

    public string OrgName(string code)
    {
        if (code.Length == 0) return "미지정";
        return _orgs.FirstOrDefault(o => o.Code == code)?.Name ?? code;
    }

    /// <summary>"CIMS › 제1본부 › 팀01" — 루트부터 경로.</summary>
    public string OrgPath(string code)
    {
        if (code.Length == 0) return "미지정";
        var parts = new List<string>();
        var seen = new HashSet<string>();
        string cur = code;
        while (cur.Length > 0 && seen.Add(cur))
        {
            var o = _orgs.FirstOrDefault(x => x.Code == cur);
            if (o is null) { parts.Add(cur); break; }
            parts.Add(o.Name);
            cur = o.Parent;
        }
        parts.Reverse();
        return string.Join(" › ", parts);
    }

    /// <summary>code 와 그 하위 조직 코드 전부(빈 code = 전체 → null).</summary>
    public HashSet<string>? OrgScope(string code)
    {
        if (code.Length == 0) return null;
        var set = new HashSet<string> { code };
        bool grew = true;
        while (grew) { grew = false; foreach (var o in _orgs) if (set.Contains(o.Parent) && set.Add(o.Code)) grew = true; }
        return set;
    }

    /// <summary>조직 트리를 깊이 순으로 평탄화(콤보 표시용) — (code, 들여쓴 이름, 인원).</summary>
    public IReadOnlyList<(string Code, string Label, int Count)> OrgTree(ContactKind kind)
    {
        var result = new List<(string, string, int)>();
        void Walk(string parent, int depth)
        {
            foreach (var o in _orgs.Where(x => x.Parent == parent).OrderBy(x => x.Sort))
            {
                int n = _merged.Count(c => c.Kind == kind && OrgScope(o.Code)!.Contains(c.OrgCode));
                result.Add((o.Code, new string(' ', depth * 3) + o.Name, n));
                Walk(o.Code, depth + 1);
            }
        }
        Walk("", 0);
        return result;
    }

    public int OrgDepth(string code)
    {
        int d = 0; var seen = new HashSet<string>(); string cur = code;
        while (cur.Length > 0 && seen.Add(cur)) { var o = _orgs.FirstOrDefault(x => x.Code == cur); if (o is null || o.Parent.Length == 0) break; cur = o.Parent; ++d; }
        return d;
    }

    /// <summary>번호/URI → 표시 이름. 없으면 빈 문자열.</summary>
    public string NameOf(string? numberOrUri)
    {
        if (string.IsNullOrEmpty(numberOrUri)) return "";
        return _names.TryGetValue(Canonical(Converters.UserPartConverter.UserPart(numberOrUri)), out string? n) ? n : "";
    }

    /// <summary>"1003 이순경" 형태 병기(§3.2 신원 표시). 이름 없으면 번호만.</summary>
    public string Label(string? numberOrUri)
    {
        string raw = Converters.UserPartConverter.UserPart(numberOrUri);
        string num = DisplayNumber(raw);
        string name = NameOf(raw);
        return name.Length > 0 ? $"{num} {name}" : num;
    }

    /// <summary>홈 국가 번호는 로컬 표기(+82 10… → 010…). 그 외는 원본.</summary>
    public string DisplayNumber(string? number)
    {
        if (string.IsNullOrEmpty(number)) return "";
        string n = number;
        if (CountryCode.Length > 0 && n.StartsWith("+" + CountryCode, StringComparison.Ordinal) && n.Length > CountryCode.Length + 2)
            return "0" + n[(CountryCode.Length + 1)..];
        return n;
    }

    public bool IsExternal(string number)
    {
        string key = Canonical(number);
        var c = _merged.FirstOrDefault(x => Canonical(x.Number) == key);
        if (c is not null) return c.Kind == ContactKind.External;
        string digits = key;
        return digits.Length > 6 && !digits.StartsWith('+');       // 내선 규약(짧은 번호)·E.164 가입자 밖이면 외부망으로 본다
    }

    /// <summary>번호 비교 키 — 숫자·+ 만.</summary>
    public static string Normalize(string s)
    {
        var sb = new System.Text.StringBuilder(s.Length);
        foreach (char c in s) if (char.IsLetterOrDigit(c) || c == '+') sb.Append(c);
        return sb.ToString();
    }
}
