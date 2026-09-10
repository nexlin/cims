// 서버 통합 이력 폴링(P3b) — `GET /provisioning/history?kind=call|ptt|message&since=<cursor>&limit=N` (CSC 4430, PKCE 토큰, ETag/304).
//
// 역할: 관제 범위 안에서 끝난 통화·PTT 세션·메시지를 수초 지연으로 ②④ 내역 패널에 합친다. 진행 중 상태는 dialog/conference 구독이
// 정본이라 이 클라이언트는 live 를 대체하지 않는다. 서버가 아직 이 API 를 내지 않으면(404/501) 첫 탐침에서 조용히 꺼진다 —
// 계약이 확정되면 파서(Parse)만 맞춘다. 요청 형태와 응답 기대치는 dispatch_desktop_ui.md §13.
//
// 응답 기대치(앱이 읽는 것 — 서버 확정 대기):
//   { "items": [ { "id": "...", "time": "2026-09-06T10:00:00+09:00", "kind": "call|ptt|message", "event": "call.answered|call.missed|...",
//                  "from": "tel:+82...", "to": "tel:+82...", "group": "tel:g003", "duration": 42, "emergency": false, "text": "...",
//                  "recordingId": "ptt/24/2026/09/07/10/S…_1", "hasRecording": true } ],
//     "next": "<since 커서 — 다음 폴링에 그대로>", "etag": "..." }
using System.Text.Json;
using CimsUe;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed class HistoryClient : IDisposable
{
    public const int DefaultIntervalMs = 2500;
    /// <summary>CSC 연결 실패 시 폴링 백오프 상한.</summary>
    public const int MaxBackoffMs = 30_000;
    private int _unreachable;
    public const int PageLimit = 200;

    private readonly CscClient _csc;
    private readonly Func<string?> _token;
    private readonly AppLog _log;
    private readonly SynchronizationContext? _ui = SynchronizationContext.Current;
    private readonly Dictionary<HistoryKind, (string Since, string ETag)> _cursor = new();
    private readonly HashSet<string> _seen = new(StringComparer.Ordinal);
    private CancellationTokenSource? _cts;

    /// <summary>탐침 결과 — null 이면 아직 안 봄, false 면 서버가 API 를 내지 않아 비활성.</summary>
    public bool? Available { get; private set; }
    public bool Running => _cts is { IsCancellationRequested: false };

    /// <summary>새 항목(중복 제거 후, 오래된 것부터). UI 스레드로 전달된다.</summary>
    public event EventHandler<HistoryEntry>? Received;

    public HistoryClient(CscClient csc, Func<string?> accessToken, AppLog log)
    {
        _csc = csc; _token = accessToken; _log = log;
    }

    /// <summary>API 존재 확인 — 200 이면 시작 가능, 404/501 이면 서버 미구현(비활성), 403 이면 범위 밖(비활성). 그 밖의 실패는 판단 보류(null).</summary>
    public async Task<bool?> ProbeAsync(CancellationToken ct = default)
    {
        string? token = _token();
        if (token is null) return null;
        var r = await Task.Run(() => _csc.XcapGet(token, "/provisioning/history?kind=call&limit=1", "application/json"), ct);
        if (r.Ok) { Available = true; _log.Info("history: available"); return true; }
        if (r.Code is 404 or 501 or 405) { Available = false; _log.Info($"history: server does not provide it ({r.Code}) — polling off"); return false; }
        if (r.Code == 403) { Available = false; _log.Warn($"history: forbidden ({r.Reason}) — polling off"); return false; }
        _log.Warn($"history probe: {r}");
        return null;
    }

    /// <summary>주기 폴링 시작(kind 별 커서 독립). 이미 돌고 있으면 무시.</summary>
    public void Start(IEnumerable<HistoryKind> kinds, int intervalMs = DefaultIntervalMs)
    {
        if (Running || Available != true) return;
        var list = kinds.Distinct().ToArray();
        if (list.Length == 0) return;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        _ = Task.Run(async () =>
        {
            while (!ct.IsCancellationRequested)
            {
                foreach (var k in list)
                {
                    try { await PollOnceAsync(k, ct); }
                    catch (OperationCanceledException) { return; }
                    catch (Exception ex) { _log.Error($"history {k}: poll failed", ex); }
                }
                // CSC 에 닿지 않으면(재배포·망 단절) 지수 백오프 — 최대 MaxBackoffMs. 닿으면 원래 주기로.
                int delay = _unreachable > 0 ? Math.Min(intervalMs << Math.Min(_unreachable, 6), MaxBackoffMs) : intervalMs;
                try { await Task.Delay(delay, ct); } catch (OperationCanceledException) { return; }
            }
        }, ct);
    }

    public void Stop()
    {
        _cts?.Cancel();
        _cts = null;
    }

    private async Task PollOnceAsync(HistoryKind kind, CancellationToken ct)
    {
        string? token = _token();
        if (token is null) return;
        var (since, etag) = _cursor.TryGetValue(kind, out var c) ? c : ("", "");
        string path = $"/provisioning/history?kind={KindName(kind)}&limit={PageLimit}" + (since.Length > 0 ? "&since=" + Uri.EscapeDataString(since) : "");
        var r = await Task.Run(() => _csc.XcapGet(token, path, "application/json", etag.Length > 0 ? etag : null), ct);
        if (!r.Ok)
        {
            if (r.Code is 404 or 501 or 403) { _log.Warn($"history {kind}: {r.Code} — polling off"); Available = false; Stop(); }
            else if (r.Code < 0) { if (_unreachable++ == 0) _log.Warn($"history: CSC unreachable ({r.Reason}) — backing off"); }
            else _log.Warn($"history {kind}: {r}");
            return;
        }
        if (_unreachable > 0) { _log.Info($"history: CSC reachable again after {_unreachable} failed polls"); _unreachable = 0; }
        if (r.Value.NotModified) return;
        var (items, next, _hours) = Parse(kind, r.Value.Body);
        _cursor[kind] = (next.Length > 0 ? next : since, r.Value.ETag);
        foreach (var e in items)
        {
            if (!_seen.Add(e.Id)) continue;
            if (_ui is not null) _ui.Post(_ => Received?.Invoke(this, e), null);
            else Received?.Invoke(this, e);
        }
        if (_seen.Count > 5000) _seen.Clear();          // 커서가 앞으로만 가므로 중복 키는 최근분만 기억하면 된다
    }

    internal static string KindName(HistoryKind k) => k switch { HistoryKind.Call => "call", HistoryKind.Ptt => "ptt", _ => "message" };

    /// <summary>응답 본문 → 항목(오래된 것부터)·다음 커서·시간대 분포. 모르는 필드는 무시, 필수(id·time)가 없는 항목은 건너뛴다.
    /// 종류별 확장 필드(§3-2 — 이력 화면용)는 있으면 읽고 없으면 기본값(""/0)이다.</summary>
    internal static (List<HistoryEntry> Items, string Next, Dictionary<string, int> Hours) Parse(HistoryKind kind, string json)
    {
        var items = new List<HistoryEntry>();
        var hours = new Dictionary<string, int>(StringComparer.Ordinal);
        string next = "";
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        if (root.TryGetProperty("next", out var n) && n.ValueKind == JsonValueKind.String) next = n.GetString() ?? "";
        if (root.TryGetProperty("hours", out var ho) && ho.ValueKind == JsonValueKind.Object)
            foreach (var p in ho.EnumerateObject()) if (p.Value.TryGetInt32(out int c)) hours[p.Name] = c;
        if (!root.TryGetProperty("items", out var arr) || arr.ValueKind != JsonValueKind.Array) return (items, next, hours);
        foreach (var it in arr.EnumerateArray())
        {
            string id = Str(it, "id");
            if (id.Length == 0 || !DateTime.TryParse(Str(it, "time"), null, System.Globalization.DateTimeStyles.RoundtripKind, out var t)) continue;
            var k = Str(it, "kind") switch { "call" => HistoryKind.Call, "ptt" => HistoryKind.Ptt, "message" => HistoryKind.Message, _ => kind };
            var people = new List<string>();
            if (it.TryGetProperty("people", out var pa) && pa.ValueKind == JsonValueKind.Array)
                foreach (var x in pa.EnumerateArray()) if (x.ValueKind == JsonValueKind.String && x.GetString() is { Length: > 0 } s) people.Add(s);
            items.Add(new HistoryEntry(id, t.ToLocalTime(), k, Str(it, "event"), Str(it, "from"), Str(it, "to"), Str(it, "group"),
                                       Int(it, "duration"), Bool(it, "emergency"), Str(it, "text"),
                                       Str(it, "recordingId"), Bool(it, "hasRecording"))
            {
                State = Str(it, "state"), CallType = Str(it, "callType"),
                InviteTime = Time(it, "inviteTime"), AnswerTime = Time(it, "answerTime"), EndTime = Time(it, "endTime"),
                EndReason = Str(it, "endReason"), SipStatus = Int(it, "sipStatus"),
                SessionKind = Str(it, "sessionKind"), StartTime = Time(it, "startTime"), GroupName = Str(it, "groupName"),
                MemberCount = Int(it, "memberCount"), TurnCount = Int(it, "turnCount"), SpeakerCount = Int(it, "speakerCount"),
                TotalSpeechMs = Int(it, "totalSpeechMs"), TalkMs = Int(it, "talkMs"), MaxConcurrent = Int(it, "maxConcurrent"),
                FloorControl = Str(it, "floorControl"), FloorPolicy = Str(it, "floorPolicy"), MaxTalkers = Int(it, "maxTalkers"),
                People = people,
            });
        }
        items.Sort((a, b) => a.Time.CompareTo(b.Time));
        return (items, next, hours);
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
    private static int Int(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) ? n : 0;
    private static bool Bool(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
    /// <summary>ISO8601(+offset 또는 naive-local) → 로컬 DateTime. 빈 값은 null.</summary>
    internal static DateTime? Time(JsonElement e, string name)
    {
        string s = Str(e, name);
        if (s.Length == 0 || !DateTime.TryParse(s, null, System.Globalization.DateTimeStyles.RoundtripKind, out var t)) return null;
        return t.Kind == DateTimeKind.Unspecified ? t : t.ToLocalTime();
    }

    public void Dispose() => Stop();
}
