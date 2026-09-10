// AvalonDock 테마 — VS2013 Light/Dark 를 바탕으로 제목줄(AnchorablePaneTitle)을 우리 패널 머리로 바꾼 사전(Themes/DockLight.xaml·DockDark.xaml).
using System;
using AvalonDock.Themes;

namespace DispatchDesktop.Themes;

public sealed class DockTheme : Theme
{
    private readonly bool _dark;
    public DockTheme(bool dark) => _dark = dark;
    public override Uri GetResourceUri() => new($"pack://application:,,,/CimsDispatch;component/Themes/Dock{(_dark ? "Dark" : "Light")}.xaml");
}
