// 핫키 매핑 (dispatch_desktop_ui.md §8) — 전역(RegisterHotKey): ptt(hold)·answer·hangup·pickup / 앱 포커스: hold·mute·Ctrl+1..9.
using CimsUe.Platform;

namespace DispatchDesktop.Services;

public sealed class HotKeyMap : IDisposable
{
    public static readonly string[] GlobalNames = { "ptt", "answer", "hangup", "pickup" };
    public static readonly string[] LocalNames = { "hold", "mute" };

    private readonly HotKeys _hotKeys;
    /// <summary>등록 실패(충돌)한 키 이름 — 설정 화면에 빨강 표시.</summary>
    public HashSet<string> Conflicts { get; } = new();

    public event EventHandler<HotKeyEventArgs>? Pressed;
    public event EventHandler<HotKeyEventArgs>? Released;

    /// <summary>UI 스레드에서 만든다.</summary>
    public HotKeyMap()
    {
        _hotKeys = new HotKeys();
        _hotKeys.Pressed += (s, e) => Pressed?.Invoke(this, e);
        _hotKeys.Released += (s, e) => Released?.Invoke(this, e);
    }

    /// <summary>설정의 전역 키를 (재)등록한다. 반환 = 충돌 목록.</summary>
    public IReadOnlySet<string> Apply(IReadOnlyDictionary<string, string> map)
    {
        Conflicts.Clear();
        foreach (string name in GlobalNames)
        {
            if (!map.TryGetValue(name, out string? text) || !HotKey.TryParse(text, out HotKey key))
            {
                _hotKeys.Unregister(name);
                continue;
            }
            if (!_hotKeys.Register(name, key, trackRelease: name == "ptt")) Conflicts.Add(name);
        }
        return Conflicts;
    }

    public static string DisplayOf(IReadOnlyDictionary<string, string> map, string name) =>
        map.TryGetValue(name, out string? t) ? t : "";

    public void Dispose() => _hotKeys.Dispose();
}
