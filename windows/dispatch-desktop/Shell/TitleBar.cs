// 창 제목 표시줄 색 — 테마(Light/Dark.xaml 의 Color.Caption·Color.CaptionText)를 따른다(dispatch_desktop_ui.md §3.2 «색»).
// WPF 기본 제목 표시줄은 시스템 색이라 어두운 테마에서 창 위에 흰 띠가 남는다. DWM 속성: 20 = 어두운 모드(Windows 10 20H1+),
// 35/36 = 제목 표시줄 면·글자 색(Windows 11). 지원하지 않는 OS 는 호출이 실패할 뿐 동작은 그대로다.
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace DispatchDesktop.Shell;

public static class TitleBar
{
    private const int DwmwaUseImmersiveDarkMode = 20, DwmwaCaptionColor = 35, DwmwaTextColor = 36;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    /// <summary>지금 테마가 어둡게인가 — App.ApplyTheme 이 정한다.</summary>
    public static bool Dark { get; set; }

    /// <summary>모든 창의 Loaded 에 건다 — 나중에 뜨는 창(감청 창·설정·로그인·화면 별창)도 같은 색으로 선다.</summary>
    public static void Register() =>
        EventManager.RegisterClassHandler(typeof(Window), FrameworkElement.LoadedEvent, new RoutedEventHandler((s, _) => { if (s is Window w) Apply(w); }));

    /// <summary>테마를 바꾼 뒤 — 이미 떠 있는 창 전부.</summary>
    public static void ApplyAll()
    {
        if (Application.Current is null) return;
        foreach (Window w in Application.Current.Windows) Apply(w);
    }

    public static void Apply(Window w)
    {
        var hwnd = new WindowInteropHelper(w).Handle;
        if (hwnd == IntPtr.Zero) return;
        try
        {
            int dark = Dark ? 1 : 0;
            DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));
            if (Application.Current.TryFindResource("Color.Caption") is Color cap) { int v = ColorRef(cap); DwmSetWindowAttribute(hwnd, DwmwaCaptionColor, ref v, sizeof(int)); }
            if (Application.Current.TryFindResource("Color.CaptionText") is Color txt) { int v = ColorRef(txt); DwmSetWindowAttribute(hwnd, DwmwaTextColor, ref v, sizeof(int)); }
        }
        catch (DllNotFoundException) { }
        catch (EntryPointNotFoundException) { }
    }

    private static int ColorRef(Color c) => c.R | (c.G << 8) | (c.B << 16);   // COLORREF = 0x00BBGGRR
}
