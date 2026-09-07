// 관리 평면 클라이언트(§4.5) — 조직/구성원/번호·PTT 그룹 목록·이력 창 조회·녹취 재생을 코어 CscClient 의 범용 요청(Bearer) 위에 얇게 둔다.
//
//  서버 계약(android_ue_provisioning.md §3-2/§3-3, CSC 4430, PKCE provisioning 토큰):
//    GET    /provisioning/directory/admin                 → {scope, services{volte[],ptt[]}, orgs[], members[]} + ETag/304
//    POST   /provisioning/directory/orgs                  {code,name,parent,sort}
//    PUT    /provisioning/directory/orgs/{code}           {name?,parent?,sort?}      DELETE …/orgs/{code}
//    POST   /provisioning/directory/members               {name,org,title,loginId,password,volte{..},ptt{..}} → {userId}
//    PUT    /provisioning/directory/members/{id}          {name?,org?,title?,loginId?,password?}   DELETE …/members/{id}
//    PUT    /provisioning/directory/members/{id}/volte|ptt {msisdn,imsi,serviceRef,sipTransport,password}  DELETE 〃
//    PUT    /provisioning/directory/members/{id}/ptt/profile {allowCreateGroup,…}
//    GET    /provisioning/directory/groups                → {groups[]}
//    GET    /provisioning/history?kind=&since=&until=&limit=  (창 조회 — 하루 단위, 항목 recordingId/hasRecording)
//    GET    /provisioning/recordings/{id}                 → 세션·세그먼트 메타(OAM 녹취 API 응답 그대로)
//    GET    /provisioning/recordings/{id}/segments/{seq}/audio?slot=&retry=  → 200 MP4 · 202 변환 중(재시도) · 500 실패
//  앱은 경로와 JSON 만 알고 인증·전송은 SDK. 오류 본문 `error` 는 ResponseText(Area.Management/Recording) 사전이 문구로 바꾼다.
using System.IO;
using System.Text.Json;
using CimsUe;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed class ManagementClient
{
    private readonly CscClient _csc;
    private readonly Func<string?> _token;
    private readonly AppLog _log;

    public ManagementClient(CscClient csc, Func<string?> accessToken, AppLog log) { _csc = csc; _token = accessToken; _log = log; }

    private static string Enc(string s) => Uri.EscapeDataString(s);
    private static string Str(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
    private static int Int(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) ? n : 0;
    private static bool Bool(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
    private static DateTime? Time(JsonElement e, string name)
    {
        string s = Str(e, name);
        if (s.Length == 0) return null;
        return DateTime.TryParse(s, null, System.Globalization.DateTimeStyles.RoundtripKind, out var t) ? (t.Kind == DateTimeKind.Utc ? t.ToLocalTime() : t) : null;
    }

    /// <summary>요청 공통 — 토큰 없으면 -1, 실패는 (code, reason=오류 본문) 로 ResponseText 사전에 맞춘다.</summary>
    private async Task<Result<HttpResponse>> SendAsync(string method, string path, string? json = null, string? ifNoneMatch = null, CancellationToken ct = default)
    {
        string? token = _token();
        if (token is null) return Result<HttpResponse>.Fail(-1, "로그인 전");
        var r = await _csc.RequestJsonAsync(token, method, path, json, null, ifNoneMatch, ct);
        if (!r.Ok)
        {
            string body = r.Value?.Body is { Length: > 0 } b ? System.Text.Encoding.UTF8.GetString(b) : "";
            _log.Warn($"mgmt {method} {path}: {r.Code} {body}");
            return new Result<HttpResponse>(r.Code, body.Length > 0 ? body : r.Reason, r.Value ?? new HttpResponse(0, "", "", Array.Empty<byte>()));
        }
        return r;
    }

    private static Result<T> Map<T>(Result<HttpResponse> r, Func<JsonElement, T> parse)
    {
        if (!r.Ok) return Result<T>.Fail(r.Code, r.Reason);
        try
        {
            using var doc = JsonDocument.Parse(r.Value.Body.Length > 0 ? r.Value.Body : "{}"u8.ToArray());
            return Result<T>.Success(parse(doc.RootElement));
        }
        catch (Exception ex) { return Result<T>.Fail(-2, "응답 해석 실패: " + ex.Message); }
    }

    // ── 조직/구성원/번호 ──

    /// <summary>관리 화면 한 벌. etag 가 같으면 Value=null(304).</summary>
    public async Task<Result<AdminView?>> GetAdminViewAsync(string? etag = null, CancellationToken ct = default)
    {
        var r = await SendAsync("GET", "/provisioning/directory/admin", null, etag, ct);
        if (r.Ok && r.Value.NotModified) return Result<AdminView?>.Success(null);
        return Map<AdminView?>(r, root => ParseAdminView(root, r.Value.ETag));
    }

    internal static AdminView ParseAdminView(JsonElement root, string etag)
    {
        var sc = root.TryGetProperty("scope", out var s) ? s : default;
        var scope = new AdminScope(Str(sc, "groupId"), Str(sc, "directoryAdmin"), Str(sc, "orgCode"));
        var services = new List<ServiceRef>();
        if (root.TryGetProperty("services", out var sv) && sv.ValueKind == JsonValueKind.Object)
            foreach (var kind in new[] { "volte", "ptt" })
                if (sv.TryGetProperty(kind, out var arr) && arr.ValueKind == JsonValueKind.Array)
                    foreach (var x in arr.EnumerateArray()) services.Add(new ServiceRef(kind, Str(x, "name"), Str(x, "domain")));
        var orgs = new List<OrgNode>();
        if (root.TryGetProperty("orgs", out var oa) && oa.ValueKind == JsonValueKind.Array)
            foreach (var o in oa.EnumerateArray()) orgs.Add(new OrgNode(Str(o, "code"), Str(o, "name"), Str(o, "parent"), Int(o, "sort")));
        var members = new List<MemberInfo>();
        if (root.TryGetProperty("members", out var ma) && ma.ValueKind == JsonValueKind.Array)
            foreach (var m in ma.EnumerateArray())
            {
                long uid = m.TryGetProperty("userId", out var u) && u.ValueKind == JsonValueKind.Number && u.TryGetInt64(out long l) ? l : 0;
                members.Add(new MemberInfo(uid, Str(m, "name"), Str(m, "loginId"), Str(m, "org"), Str(m, "title"), ParseNumber(m, "volte"), ParseNumber(m, "ptt")));
            }
        return new AdminView(scope, services, orgs, members, etag);
    }

    private static NumberInfo? ParseNumber(JsonElement m, string kind)
    {
        if (!m.TryGetProperty(kind, out var n) || n.ValueKind != JsonValueKind.Object) return null;
        Dictionary<string, bool>? prof = null;
        if (n.TryGetProperty("profile", out var p) && p.ValueKind == JsonValueKind.Object)
        {
            prof = new Dictionary<string, bool>(StringComparer.Ordinal);
            foreach (var kv in p.EnumerateObject()) prof[kv.Name] = kv.Value.ValueKind == JsonValueKind.True;
        }
        return new NumberInfo(Str(n, "msisdn"), Str(n, "imsi"), Str(n, "serviceRef"), Str(n, "sipTransport"), Str(n, "authScheme"), prof);
    }

    public Task<Result<HttpResponse>> CreateOrgAsync(string code, string name, string parent, int sort, CancellationToken ct = default) =>
        SendAsync("POST", "/provisioning/directory/orgs", JsonSerializer.Serialize(new { code, name, parent, sort }), null, ct);
    public Task<Result<HttpResponse>> UpdateOrgAsync(string code, string? name, string? parent, int? sort, CancellationToken ct = default)
    {
        var body = new Dictionary<string, object>();
        if (name is not null) body["name"] = name;
        if (parent is not null) body["parent"] = parent;
        if (sort is not null) body["sort"] = sort.Value;
        return SendAsync("PUT", $"/provisioning/directory/orgs/{Enc(code)}", JsonSerializer.Serialize(body), null, ct);
    }
    public Task<Result<HttpResponse>> DeleteOrgAsync(string code, CancellationToken ct = default) =>
        SendAsync("DELETE", $"/provisioning/directory/orgs/{Enc(code)}", null, null, ct);

    private static Dictionary<string, object> NumberBody(NumberInput n)
    {
        var d = new Dictionary<string, object> { ["msisdn"] = n.Msisdn.Trim() };
        if (n.Imsi.Trim().Length > 0) d["imsi"] = n.Imsi.Trim();
        if (n.ServiceRef.Length > 0) d["serviceRef"] = n.ServiceRef;
        if (n.SipTransport.Length > 0) d["sipTransport"] = n.SipTransport;
        if (n.Password.Length > 0) d["password"] = n.Password;
        return d;
    }

    private static Dictionary<string, object> MemberBody(MemberInput m)
    {
        var d = new Dictionary<string, object>();
        if (m.Name is not null) d["name"] = m.Name;
        if (m.Org is not null) d["org"] = m.Org;
        if (m.Title is not null) d["title"] = m.Title;
        if (m.LoginId is not null) d["loginId"] = m.LoginId;
        if (!string.IsNullOrEmpty(m.Password)) d["password"] = m.Password;
        if (m.Volte is not null && m.Volte.Msisdn.Trim().Length > 0) d["volte"] = NumberBody(m.Volte);
        if (m.Ptt is not null && m.Ptt.Msisdn.Trim().Length > 0) d["ptt"] = NumberBody(m.Ptt);
        return d;
    }

    /// <summary>구성원 생성 → userId.</summary>
    public async Task<Result<long>> CreateMemberAsync(MemberInput m, CancellationToken ct = default)
    {
        var r = await SendAsync("POST", "/provisioning/directory/members", JsonSerializer.Serialize(MemberBody(m)), null, ct);
        return Map(r, root => root.TryGetProperty("userId", out var u) && u.TryGetInt64(out long id) ? id : 0L);
    }
    public Task<Result<HttpResponse>> UpdateMemberAsync(long userId, MemberInput m, CancellationToken ct = default) =>
        SendAsync("PUT", $"/provisioning/directory/members/{userId}", JsonSerializer.Serialize(MemberBody(m)), null, ct);
    public Task<Result<HttpResponse>> DeleteMemberAsync(long userId, CancellationToken ct = default) =>
        SendAsync("DELETE", $"/provisioning/directory/members/{userId}", null, null, ct);
    public Task<Result<HttpResponse>> PutNumberAsync(long userId, string kind, NumberInput n, CancellationToken ct = default) =>
        SendAsync("PUT", $"/provisioning/directory/members/{userId}/{kind}", JsonSerializer.Serialize(NumberBody(n)), null, ct);
    public Task<Result<HttpResponse>> DeleteNumberAsync(long userId, string kind, CancellationToken ct = default) =>
        SendAsync("DELETE", $"/provisioning/directory/members/{userId}/{kind}", null, null, ct);
    public Task<Result<HttpResponse>> PutPttProfileAsync(long userId, IReadOnlyDictionary<string, bool> flags, CancellationToken ct = default) =>
        SendAsync("PUT", $"/provisioning/directory/members/{userId}/ptt/profile", JsonSerializer.Serialize(flags), null, ct);

    // ── PTT 그룹(관리 범위) ──
    public async Task<Result<IReadOnlyList<ManagedGroup>>> ListGroupsAsync(CancellationToken ct = default)
    {
        var r = await SendAsync("GET", "/provisioning/directory/groups", null, null, ct);
        return Map<IReadOnlyList<ManagedGroup>>(r, root =>
        {
            var list = new List<ManagedGroup>();
            if (root.TryGetProperty("groups", out var ga) && ga.ValueKind == JsonValueKind.Array)
                foreach (var g in ga.EnumerateArray())
                    list.Add(new ManagedGroup(Str(g, "id"), Str(g, "uri"), Str(g, "name"), Int(g, "memberCount"), Bool(g, "isOwner"),
                                              Str(g, "orgCode"), Str(g, "sessionType"), Str(g, "etag")));
            return list;
        });
    }

    // ── 이력 창 조회 ──
    /// <summary>[from, to] 창의 이력(시각 오름차순). 서버 스캔은 48 시간 버킷 상한이라 호출자가 하루 단위로 나눈다.</summary>
    public async Task<Result<IReadOnlyList<HistoryEntry>>> QueryHistoryAsync(HistoryKind kind, DateTime from, DateTime to, int limit = 1000, CancellationToken ct = default)
    {
        string path = $"/provisioning/history?kind={HistoryClient.KindName(kind)}&since={Enc(from.ToString("yyyy-MM-ddTHH:mm:ss"))}" +
                      $"&until={Enc(to.ToString("yyyy-MM-ddTHH:mm:ss"))}&limit={Math.Clamp(limit, 1, 1000)}";
        var r = await SendAsync("GET", path, null, null, ct);
        if (!r.Ok) return Result<IReadOnlyList<HistoryEntry>>.Fail(r.Code, r.Reason);
        try { return Result<IReadOnlyList<HistoryEntry>>.Success(HistoryClient.Parse(kind, r.Value.Text).Items); }
        catch (JsonException ex) { return Result<IReadOnlyList<HistoryEntry>>.Fail(-2, "응답 해석 실패: " + ex.Message); }
    }

    // ── 녹취 ──
    private static string RecPath(string id) => "/provisioning/recordings/" + string.Join('/', id.Split('/').Select(Enc));

    public async Task<Result<RecordingInfo>> GetRecordingAsync(string id, CancellationToken ct = default)
    {
        var r = await SendAsync("GET", RecPath(id), null, null, ct);
        return Map(r, root => ParseRecording(root, id));
    }

    internal static RecordingInfo ParseRecording(JsonElement root, string id)
    {
        var segs = new List<RecordingSegment>();
        if (root.TryGetProperty("segments", out var sa) && sa.ValueKind == JsonValueKind.Array)
            foreach (var s in sa.EnumerateArray())
            {
                var ids = new List<string>();
                if (s.TryGetProperty("speaker_ids", out var si) && si.ValueKind == JsonValueKind.Array)
                    foreach (var x in si.EnumerateArray()) if (x.ValueKind == JsonValueKind.String) ids.Add(x.GetString() ?? "");
                segs.Add(new RecordingSegment(Int(s, "seq"), Str(s, "type"), Str(s, "speaker_id"), Time(s, "start_time"), Time(s, "end_time"),
                                              Int(s, "duration_ms"), Bool(s, "has_video"), Str(s, "status"), ids, Int(s, "talker_count")));
            }
        return new RecordingInfo(Str(root, "id").Length > 0 ? Str(root, "id") : id, Str(root, "call_type"), Str(root, "caller"), Str(root, "callee"),
                                 Str(root, "group_id"), Time(root, "start_time"), Time(root, "end_time"), Int(root, "duration"), Str(root, "status"), segs);
    }

    /// <summary>세그먼트 오디오(MP4/AAC)를 받아 로컬 파일로 — 202(변환 중)는 700ms→1.5s 간격으로 최대 120초 재시도(콘솔 SegmentPlayer 와 같은 규약).
    /// slot = 단독 발언자 트랙(null = 믹스). 진행 상황은 status 콜백("변환 중…").</summary>
    public async Task<Result<string>> FetchSegmentAudioAsync(string id, int seq, int? slot, bool retry, Action<string>? status, CancellationToken ct = default)
    {
        string? token = _token();
        if (token is null) return Result<string>.Fail(-1, "로그인 전");
        string path = RecPath(id) + $"/segments/{seq}/audio";
        var q = new List<string>();
        if (slot is not null) q.Add("slot=" + slot.Value);
        if (retry) q.Add("retry=1");
        string full = q.Count > 0 ? path + "?" + string.Join('&', q) : path;
        var deadline = DateTime.UtcNow.AddSeconds(120);
        int delay = 700;
        while (true)
        {
            var r = await _csc.RequestAsync(token, "GET", full, null, null, "*/*", null, null, ct);
            if (r.Ok && r.Value.Status == 200)
            {
                string dir = Path.Combine(Path.GetTempPath(), "CIMS", AppPaths.AppName, "rec");
                Directory.CreateDirectory(dir);
                string file = Path.Combine(dir, $"{Sanitize(id)}_{seq}_{(slot?.ToString() ?? "mix")}.mp4");
                await File.WriteAllBytesAsync(file, r.Value.Body, ct);
                return Result<string>.Success(file);
            }
            if (r.Ok && r.Value.Status == 202)
            {
                if (DateTime.UtcNow > deadline) return Result<string>.Fail(202, "변환 대기 시간 초과 — 다시 시도하세요");
                status?.Invoke(RecordingStatusText(r.Value.Text));
                await Task.Delay(delay, ct);
                delay = 1500;
                full = q.Count > 0 && retry ? path + "?" + string.Join('&', q.Where(x => !x.StartsWith("retry", StringComparison.Ordinal))) : full;
                continue;
            }
            string body = r.Value?.Body is { Length: > 0 } b ? System.Text.Encoding.UTF8.GetString(b) : r.Reason;
            _log.Warn($"recording audio {id} seq={seq}: {r.Code} {body}");
            return Result<string>.Fail(r.Code == 0 ? r.Value?.Status ?? -1 : r.Code, body);
        }
    }

    private static string RecordingStatusText(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json.Length > 0 ? json : "{}");
            string st = Str(doc.RootElement, "status");
            return st == "recording" ? "녹음 진행 중 — 세그먼트가 닫히면 재생됩니다" : "서버가 변환 중입니다…";
        }
        catch (JsonException) { return "서버가 변환 중입니다…"; }
    }

    private static string Sanitize(string id)
    {
        var sb = new System.Text.StringBuilder(id.Length);
        foreach (char c in id) sb.Append(char.IsLetterOrDigit(c) ? c : '_');
        return sb.Length > 120 ? sb.ToString(sb.Length - 120, 120) : sb.ToString();
    }
}
