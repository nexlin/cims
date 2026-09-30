// 배치(layout.json, §3.3) — 고정 배치(모드마다 한 화면)라 도킹·프리셋은 없고, 주 창·감청 창 위치와 운영자가 끈 칸 경계만 기억한다.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace DispatchDesktop.Services;

public sealed class WindowBounds
{
    /// <summary>null = 위치 미저장(OS 기본 위치).</summary>
    public double? Left { get; set; }
    public double? Top { get; set; }
    public double Width { get; set; } = 1920;
    public double Height { get; set; } = 1080;
    public bool Maximized { get; set; } = true;
}

/// <summary>칸 경계(§3.3) — 왼쪽 칸 1040 은 고정이고 끌 수 있는 것은 이 셋뿐이다. 0 이하 = 시안 기본값.</summary>
public sealed class SeamSizes
{
    /// <summary>[무전] 위 줄(내 채널 | 타 채널) 높이 — 기본 292. 배너가 서도 이 값은 그대로고 아래 줄이 준다.</summary>
    public double PttTop { get; set; } = LayoutStore.PttTopDefault;
    /// <summary>[무전] «메시지» 대화 목록 폭 — 기본 300.</summary>
    public double ThreadList { get; set; } = LayoutStore.ListDefault;
    /// <summary>[통화] «기록» 상대 목록 폭 — 기본 300.</summary>
    public double RecordList { get; set; } = LayoutStore.ListDefault;
}

public sealed class LayoutFile
{
    /// <summary>배치 구성 버전 — 다르면(옛 도킹 프리셋 파일) 칸 경계를 기본값으로 되돌린다. 창 위치는 옮겨 온다.</summary>
    public int Version { get; set; } = LayoutStore.CurrentVersion;
    public WindowBounds Window { get; set; } = new();
    /// <summary>감청 창 기본 위치(마지막 위치 기억).</summary>
    public WindowBounds Monitor { get; set; } = new() { Width = 440, Height = 260, Maximized = false };
    public SeamSizes Seams { get; set; } = new();
}

public sealed class LayoutStore
{
    /// <summary>4 = 모드마다 한 화면(고정 배치 + 칸 경계 끌기). 3 = 도킹 패널 6개(프리셋) — 읽으면 창 위치만 옮긴다.</summary>
    public const int CurrentVersion = 4;
    public const double PttTopDefault = 292, ListDefault = 300;
    public const double PttTopMin = 200, PttTopMax = 520, ListMin = 220, ListMax = 480;
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public LayoutFile File { get; private set; } = new();

    public void Load()
    {
        try
        {
            if (System.IO.File.Exists(AppPaths.Layout))
            {
                string text = System.IO.File.ReadAllText(AppPaths.Layout);
                var f = JsonSerializer.Deserialize<LayoutFile>(text, Json) ?? new LayoutFile();
                if (f.Version != CurrentVersion) f = Migrate(text);
                File = f;
            }
        }
        catch (Exception) { File = new LayoutFile(); }
        File.Seams.PttTop = Clamp(File.Seams.PttTop, PttTopDefault, PttTopMin, PttTopMax);
        File.Seams.ThreadList = Clamp(File.Seams.ThreadList, ListDefault, ListMin, ListMax);
        File.Seams.RecordList = Clamp(File.Seams.RecordList, ListDefault, ListMin, ListMax);
    }

    private static double Clamp(double v, double dflt, double min, double max) => v <= 0 || double.IsNaN(v) ? dflt : Math.Clamp(v, min, max);

    /// <summary>옛 도킹 프리셋 파일(버전 3 — presets[] 안에 창 위치) → 현재 프리셋의 창·감청 창 위치만 옮긴다.</summary>
    private static LayoutFile Migrate(string text)
    {
        var f = new LayoutFile();
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            string current = root.TryGetProperty("Current", out var c) ? c.GetString() ?? "" : "";
            if (root.TryGetProperty("Presets", out var presets) && presets.ValueKind == JsonValueKind.Array)
                foreach (var p in presets.EnumerateArray())
                {
                    if (!p.TryGetProperty("Name", out var n) || n.GetString() != current) continue;
                    if (p.TryGetProperty("Window", out var w)) f.Window = w.Deserialize<WindowBounds>(Json) ?? f.Window;
                    if (p.TryGetProperty("Monitor", out var m)) f.Monitor = m.Deserialize<WindowBounds>(Json) ?? f.Monitor;
                }
        }
        catch (JsonException) { }
        return f;
    }

    /// <summary>저장 실패(읽기 전용·잠김 %APPDATA%)는 삼킨다 — 배치는 메모리에 유효하고, 창 닫기마다 오류 창이 뜨면 안 된다.</summary>
    public void Save()
    {
        try
        {
            AppPaths.Ensure();
            System.IO.File.WriteAllText(AppPaths.Layout, JsonSerializer.Serialize(File, Json));
        }
        catch (Exception ex) when (ex is System.IO.IOException or UnauthorizedAccessException) { }
    }
}
