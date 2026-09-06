// 서버 통합 이력 폴링(P3b) — `GET /provisioning/history?kind=call|ptt|message&since=<cursor>&limit=N` (CSC 4430, PKCE 토큰, ETag/304).
//
// 역할: 관제 범위 안에서 끝난 통화·PTT 세션·메시지를 수초 지연으로 ②④ 내역 패널에 합친다. 진행 중 상태는 dialog/conference 구독이
// 정본이라 이 클라이언트는 live 를 대체하지 않는다. 서버가 아직 이 API 를 내지 않으면(404/501) 첫 탐침에서 조용히 꺼진다 —
// 계약이 확정되면 파서(Parse)만 맞춘다. 요청 형태와 응답 기대치는 dispatch_desktop_ui.md §13.
//
// 응답 기대치(앱이 읽는 것 — 서버 확정 대기):
//   { "items": [ { "id": "...", "time": "2026-09-06T10:00:00+09:00", "kind": "call|ptt|message", "event": "call.answered|call.missed|...",
//                  "from": "tel:+82...", "to": "tel:+82...", "group": "tel:g003", "duration": 42, "emergency": false, "text": "..." } ],
//     "next": "<since 커서 — 다음 폴링에 그대로>", "etag": "..." }
using System.Text.Json;
using CimsUe;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public sealed class HistoryClient : IDisposable
{
    public const int DefaultIntervalMs = 2500;
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
                try { await Task.Delay(intervalMs, ct); } catch (OperationCanceledException) { return; }
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
            else _log.Warn($"history {kind}: {r}");
            return;
        }
        if (r.Value.NotModified) return;
        var (items, next) = Parse(kind, r.Value.Body);
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

    /// <summary>응답 본문 → 항목(오래된 것부터)·다음 커서. 모르는 필드는 무시, 필수(id·time)가 없는 항목은 건너뛴다.</summary>
    internal static (List<HistoryEntry> Items, string Next) Parse(HistoryKind kind, string json)
    {
        var items = new List<HistoryEntry>();
        string next = "";
        using var doc = JsonDocument.Parse(json);
        var root = doc.RootElement;
        if (root.TryGetProperty("next", out var n) && n.ValueKind == JsonValueKind.String) next = n.GetString() ?? "";
        if (!root.TryGetProperty("items", out var arr) || arr.ValueKind != JsonValueKind.Array) return (items, next);
        foreach (var it in arr.EnumerateArray())
        {
            string id = Str(it, "id");
            if (id.Length == 0 || !DateTime.TryParse(Str(it, "time"), null, System.Globalization.DateTimeStyles.RoundtripKind, out var t)) continue;
            var k = Str(it, "kind") switch { "call" => HistoryKind.Call, "ptt" => HistoryKind.Ptt, "message" => HistoryKind.Message, _ => kind };
            items.Add(new HistoryEntry(id, t.ToLocalTime(), k, Str(it, "event"), Str(it, "from"), Str(it, "to"), Str(it, "group"),
                                       it.TryGetProperty("duration", out var d) && d.TryGetInt32(out int ds) ? ds : 0,
                                       it.TryGetProperty("emergency", out var em) && em.ValueKind == JsonValueKind.True, Str(it, "text")));
        }
        items.Sort((a, b) => a.Time.CompareTo(b.Time));
        return (items, next);
    }

    private static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    public void Dispose() => Stop();
}
