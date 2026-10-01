// 핫키 매핑 (dispatch_desktop_ui.md §8) — 전역(RegisterHotKey): ptt(hold)·answer·hangup·pickup / 앱 포커스: hold·mute·Ctrl+1..9.
// 전역 이름이라도 글자 키(수식키 없는 Space·글자·숫자 — PTT 기본 Space)는 전역으로 등록하지 않는다 — 다른 프로그램의 타이핑을 삼키기 때문.
// 그런 키는 앱이 앞에 있을 때, 입력칸 밖에서만 받는다(FocusOnly — 창의 키 처리가 맡는다).
using CimsUe.Platform;

namespace DispatchDesktop.Services;

public sealed class HotKeyMap : IDisposable
{
    public static readonly string[] GlobalNames = { "ptt", "answer", "hangup", "pickup" };
    public static readonly string[] LocalNames = { "hold", "mute" };

    private readonly HotKeys _hotKeys;
    /// <summary>등록 실패(충돌)한 키 이름 — 설정 화면에 빨강 표시.</summary>
    public HashSet<string> Conflicts { get; } = new();
    /// <summary>글자 키라 전역 등록하지 않은 이름 — 앱이 앞에 있을 때(입력칸 밖)만 받는다. 충돌이 아니다.</summary>
    public HashSet<string> FocusOnly { get; } = new();
    /// <summary>창의 키 처리가 맡을 전역 이름인가 — 전역 등록 실패(충돌) 폴백 또는 글자 키.</summary>
    public bool HandledInApp(string name) => Conflicts.Contains(name) || FocusOnly.Contains(name);

    /// <summary>글자를 넣는 키인가 — Ctrl/Alt/Win 없이 누르는 Space·Enter·Tab·Backspace·글자·숫자·숫자패드·문장부호. F키·Pause·ScrollLock 등은 아니다(전역 등록).</summary>
    public static bool IsTypingKey(HotKey k) =>
        (k.Modifiers & (HotKeyModifiers.Control | HotKeyModifiers.Alt | HotKeyModifiers.Win)) == 0
        && k.VirtualKey is 0x08 or 0x09 or 0x0D or 0x20 or (>= 0x30 and <= 0x39) or (>= 0x41 and <= 0x5A) or (>= 0x60 and <= 0x6F) or (>= 0xBA and <= 0xE2);

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
        Conflicts.Clear(); FocusOnly.Clear();
        foreach (string name in GlobalNames)
        {
            if (!map.TryGetValue(name, out string? text) || !HotKey.TryParse(text, out HotKey key))
            {
                _hotKeys.Unregister(name);
                continue;
            }
            if (IsTypingKey(key)) { _hotKeys.Unregister(name); FocusOnly.Add(name); continue; }
            if (!_hotKeys.Register(name, key, trackRelease: name == "ptt")) Conflicts.Add(name);
        }
        return Conflicts;
    }

    public static string DisplayOf(IReadOnlyDictionary<string, string> map, string name) =>
        map.TryGetValue(name, out string? t) ? t : "";

    public void Dispose() => _hotKeys.Dispose();
}
