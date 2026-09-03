// 배치 프리셋(layout.json, §3.3) — 이름 → 도킹 배치 XML(AvalonDock 직렬화) + 잠금 + 주 창 위치. "기본 배치" 는 XML 없음(= XAML 기본).
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

public sealed class LayoutPreset
{
    public string Name { get; set; } = "";
    /// <summary>AvalonDock XmlLayoutSerializer 산출. 비면 기본 배치.</summary>
    public string DockXml { get; set; } = "";
    public bool Locked { get; set; } = true;
    public WindowBounds Window { get; set; } = new();
    /// <summary>감청 창 기본 위치(마지막 위치 기억).</summary>
    public WindowBounds Monitor { get; set; } = new() { Width = 440, Height = 260, Maximized = false };
}

public sealed class LayoutFile
{
    public string Current { get; set; } = LayoutStore.DefaultName;
    public List<LayoutPreset> Presets { get; set; } = new();
}

public sealed class LayoutStore
{
    public const string DefaultName = "기본 배치";
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public LayoutFile File { get; private set; } = new();

    public IReadOnlyList<string> Names => File.Presets.Select(p => p.Name).Prepend(DefaultName).Distinct().ToList();

    public LayoutPreset Current => Get(File.Current) ?? Get(DefaultName)!;

    public LayoutPreset? Get(string name)
    {
        var p = File.Presets.FirstOrDefault(x => x.Name == name);
        if (p is null && name == DefaultName) { p = new LayoutPreset { Name = DefaultName }; File.Presets.Insert(0, p); }
        return p;
    }

    public void Load()
    {
        try
        {
            if (System.IO.File.Exists(AppPaths.Layout))
                File = JsonSerializer.Deserialize<LayoutFile>(System.IO.File.ReadAllText(AppPaths.Layout), Json) ?? new LayoutFile();
        }
        catch (Exception) { File = new LayoutFile(); }
        _ = Get(DefaultName);
    }

    public void Save()
    {
        AppPaths.Ensure();
        System.IO.File.WriteAllText(AppPaths.Layout, JsonSerializer.Serialize(File, Json));
    }

    /// <summary>현재 배치를 이름으로 저장(같은 이름은 덮어씀) 후 그 프리셋을 현재로.</summary>
    public LayoutPreset SaveAs(string name, string dockXml, bool locked, WindowBounds window)
    {
        var p = Get(name) ?? new LayoutPreset { Name = name };
        if (!File.Presets.Contains(p)) File.Presets.Add(p);
        p.DockXml = name == DefaultName ? "" : dockXml;
        p.Locked = locked;
        p.Window = window;
        File.Current = name;
        Save();
        return p;
    }

    public void Delete(string name)
    {
        if (name == DefaultName) return;
        File.Presets.RemoveAll(p => p.Name == name);
        if (File.Current == name) File.Current = DefaultName;
        Save();
    }

    public void SetCurrent(string name) { File.Current = name; Save(); }
}
