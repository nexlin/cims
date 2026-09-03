// CimsUe.Platform — 전역 핫키 (RegisterHotKey + 메시지 전용 HWND, dispatch_desktop_ui.md §8)
//
// PTT(hold)·응답·종료·그룹 픽업 같은 전역 키. WM_HOTKEY 는 key-down 만 오므로 hold 형 키는 GetAsyncKeyState 를 20ms 로
// 폴링해 key-up(Released)을 만든다 — 포커스가 바뀌어도 release 를 놓치지 않는다. 메시지 전용 창은 생성 스레드의
// 메시지 루프가 필요하다 → UI 스레드에서 만든다. 이벤트는 생성 시점의 SynchronizationContext(=UI)로 전달한다.
using System.Runtime.InteropServices;

namespace CimsUe.Platform;

[Flags]
public enum HotKeyModifiers { None = 0, Alt = 0x1, Control = 0x2, Shift = 0x4, Win = 0x8 }

/// <summary>키 조합. 문자열 표기 "Ctrl+Space"·"F9"·"Ctrl+Shift+1".</summary>
public readonly record struct HotKey(HotKeyModifiers Modifiers, int VirtualKey)
{
    public static HotKey None => default;
    public bool IsEmpty => VirtualKey == 0;

    public static bool TryParse(string? text, out HotKey key)
    {
        key = default;
        if (string.IsNullOrWhiteSpace(text)) return false;
        var mods = HotKeyModifiers.None;
        int vk = 0;
        foreach (string raw in text.Split('+'))
        {
            string tok = raw.Trim();
            switch (tok.ToLowerInvariant())
            {
                case "ctrl": case "control": mods |= HotKeyModifiers.Control; continue;
                case "alt": mods |= HotKeyModifiers.Alt; continue;
                case "shift": mods |= HotKeyModifiers.Shift; continue;
                case "win": case "windows": mods |= HotKeyModifiers.Win; continue;
            }
            if (vk != 0 || !TryKeyCode(tok, out vk)) return false;
        }
        if (vk == 0) return false;
        key = new HotKey(mods, vk);
        return true;
    }

    public static HotKey Parse(string text) =>
        TryParse(text, out var k) ? k : throw new FormatException($"핫키 표기가 아니다: '{text}'");

    public override string ToString()
    {
        if (IsEmpty) return "";
        var parts = new List<string>(4);
        if (Modifiers.HasFlag(HotKeyModifiers.Control)) parts.Add("Ctrl");
        if (Modifiers.HasFlag(HotKeyModifiers.Alt)) parts.Add("Alt");
        if (Modifiers.HasFlag(HotKeyModifiers.Shift)) parts.Add("Shift");
        if (Modifiers.HasFlag(HotKeyModifiers.Win)) parts.Add("Win");
        parts.Add(KeyName(VirtualKey));
        return string.Join("+", parts);
    }

    private static readonly (string Name, int Vk)[] Named =
    {
        ("Space", 0x20), ("Enter", 0x0D), ("Esc", 0x1B), ("Tab", 0x09), ("Backspace", 0x08), ("Pause", 0x13),
        ("PageUp", 0x21), ("PageDown", 0x22), ("End", 0x23), ("Home", 0x24), ("Left", 0x25), ("Up", 0x26), ("Right", 0x27), ("Down", 0x28),
        ("Insert", 0x2D), ("Delete", 0x2E), ("ScrollLock", 0x91), ("NumLock", 0x90), ("PrintScreen", 0x2C),
        ("Multiply", 0x6A), ("Add", 0x6B), ("Subtract", 0x6D), ("Decimal", 0x6E), ("Divide", 0x6F),
    };

    private static bool TryKeyCode(string tok, out int vk)
    {
        vk = 0;
        if (tok.Length == 0) return false;
        if (tok.Length == 1)
        {
            char c = char.ToUpperInvariant(tok[0]);
            if (c is >= '0' and <= '9' or >= 'A' and <= 'Z') { vk = c; return true; }
            return false;
        }
        if ((tok[0] == 'F' || tok[0] == 'f') && int.TryParse(tok.AsSpan(1), out int f) && f is >= 1 and <= 24) { vk = 0x70 + f - 1; return true; }
        if (tok.StartsWith("NumPad", StringComparison.OrdinalIgnoreCase) && tok.Length == 7 && tok[6] is >= '0' and <= '9') { vk = 0x60 + (tok[6] - '0'); return true; }
        foreach (var (name, code) in Named)
            if (string.Equals(name, tok, StringComparison.OrdinalIgnoreCase)) { vk = code; return true; }
        if (string.Equals(tok, "Escape", StringComparison.OrdinalIgnoreCase)) { vk = 0x1B; return true; }
        if (string.Equals(tok, "Return", StringComparison.OrdinalIgnoreCase)) { vk = 0x0D; return true; }
        return false;
    }

    private static string KeyName(int vk)
    {
        if (vk is >= '0' and <= '9' or >= 'A' and <= 'Z') return ((char)vk).ToString();
        if (vk is >= 0x70 and <= 0x87) return "F" + (vk - 0x70 + 1);
        if (vk is >= 0x60 and <= 0x69) return "NumPad" + (vk - 0x60);
        foreach (var (name, code) in Named) if (code == vk) return name;
        return "VK" + vk.ToString("X2");
    }
}

public sealed class HotKeyEventArgs : EventArgs
{
    public string Name { get; }
    public HotKey Key { get; }
    public HotKeyEventArgs(string name, HotKey key) { Name = name; Key = key; }
}

public sealed class HotKeys : IDisposable
{
    private const int WM_HOTKEY = 0x0312;
    private const uint MOD_NOREPEAT = 0x4000;
    private static readonly IntPtr HWND_MESSAGE = new(-3);

    private readonly SynchronizationContext? _sync;
    private readonly Dictionary<int, Entry> _byId = new();
    private readonly Dictionary<string, int> _byName = new();
    private readonly WndProc _proc;                       // 델리게이트를 창 수명 동안 붙잡는다
    private readonly ushort _atom;
    private IntPtr _hwnd;
    private int _nextId = 1;
    private Timer? _poll;
    private Entry? _held;

    private sealed record Entry(int Id, string Name, HotKey Key, bool TrackRelease);

    /// <summary>키 down. hold 형(TrackRelease)은 Released 가 뒤따른다.</summary>
    public event EventHandler<HotKeyEventArgs>? Pressed;
    public event EventHandler<HotKeyEventArgs>? Released;

    /// <summary>UI 스레드에서 만든다(메시지 루프 필요).</summary>
    public HotKeys(SynchronizationContext? context = null)
    {
        _sync = context ?? SynchronizationContext.Current;
        _proc = WindowProc;
        string cls = "CimsUe.HotKeys." + Guid.NewGuid().ToString("N");
        var wc = new WNDCLASSEX
        {
            cbSize = Marshal.SizeOf<WNDCLASSEX>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_proc),
            hInstance = GetModuleHandle(null),
            lpszClassName = cls,
        };
        _atom = RegisterClassEx(ref wc);
        if (_atom == 0) throw new InvalidOperationException("RegisterClassEx 실패: " + Marshal.GetLastWin32Error());
        _hwnd = CreateWindowEx(0, cls, "", 0, 0, 0, 0, 0, HWND_MESSAGE, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
        if (_hwnd == IntPtr.Zero) throw new InvalidOperationException("CreateWindowEx 실패: " + Marshal.GetLastWin32Error());
    }

    public IReadOnlyDictionary<string, HotKey> Registered
    {
        get { var d = new Dictionary<string, HotKey>(); foreach (var e in _byId.Values) d[e.Name] = e.Key; return d; }
    }

    /// <summary>등록. 같은 이름이 있으면 교체. false = RegisterHotKey 실패(다른 프로그램과 충돌) — 앱은 빨강 표시.</summary>
    public bool Register(string name, HotKey key, bool trackRelease = false)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        Unregister(name);
        if (key.IsEmpty) return false;
        int id = _nextId++;
        if (!RegisterHotKey(_hwnd, id, (uint)key.Modifiers | MOD_NOREPEAT, (uint)key.VirtualKey)) return false;
        var e = new Entry(id, name, key, trackRelease);
        _byId[id] = e;
        _byName[name] = id;
        return true;
    }

    public void Unregister(string name)
    {
        if (!_byName.Remove(name, out int id)) return;
        _byId.Remove(id);
        UnregisterHotKey(_hwnd, id);
    }

    private IntPtr WindowProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        if (msg == WM_HOTKEY && _byId.TryGetValue((int)wParam, out var e))
        {
            var args = new HotKeyEventArgs(e.Name, e.Key);
            Post(() => Pressed?.Invoke(this, args));
            if (e.TrackRelease) BeginTrackRelease(e);
            return IntPtr.Zero;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    private void BeginTrackRelease(Entry e)
    {
        _held = e;
        _poll ??= new Timer(_ => PollRelease(), null, Timeout.Infinite, Timeout.Infinite);
        _poll.Change(20, 20);
    }

    private void PollRelease()
    {
        var e = _held;
        if (e is null) { _poll?.Change(Timeout.Infinite, Timeout.Infinite); return; }
        if ((GetAsyncKeyState(e.Key.VirtualKey) & 0x8000) != 0) return;          // 아직 눌림
        _held = null;
        _poll?.Change(Timeout.Infinite, Timeout.Infinite);
        var args = new HotKeyEventArgs(e.Name, e.Key);
        Post(() => Released?.Invoke(this, args));
    }

    private void Post(Action a)
    {
        if (_sync is not null) _sync.Post(static o => ((Action)o!)(), a);
        else a();
    }

    public void Dispose()
    {
        _poll?.Dispose();
        foreach (int id in _byId.Keys) UnregisterHotKey(_hwnd, id);
        _byId.Clear(); _byName.Clear();
        if (_hwnd != IntPtr.Zero) { DestroyWindow(_hwnd); _hwnd = IntPtr.Zero; }
        if (_atom != 0) UnregisterClass((IntPtr)_atom, GetModuleHandle(null));
    }

    // ── Win32 ──
    private delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEX
    {
        public int cbSize; public uint style; public IntPtr lpfnWndProc; public int cbClsExtra; public int cbWndExtra;
        public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string? lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
        public IntPtr hIconSm;
    }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool UnregisterClass(IntPtr classAtom, IntPtr hInstance);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateWindowEx(uint exStyle, string className, string windowName, uint style, int x, int y, int w, int h,
                                                IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr DefWindowProc(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool RegisterHotKey(IntPtr hwnd, int id, uint modifiers, uint vk);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool UnregisterHotKey(IntPtr hwnd, int id);
    [DllImport("user32.dll")] private static extern short GetAsyncKeyState(int vk);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetModuleHandle(string? module);
}
