// 화면 별창(§3.4) 코드비하인드 — 화면 고정·제목·닫힘 통지만. 관제 요약 띠는 별창에도 붙어 관제 상태를 놓치지 않는다.
using System.Windows;
using DispatchDesktop.Models;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class ScreenWindow : Window
{
    public AppScreen Screen { get; }

    public ScreenWindow(MainViewModel vm, AppScreen screen)
    {
        InitializeComponent();
        Screen = screen;
        DataContext = vm;
        Host.Screen = screen;
        Title = $"{AppScreens.Title(screen)} — {vm.Desk.DisplayName} · {vm.Desk.GroupName}";
        Closed += (_, _) => vm.OnScreenWindowClosed(screen);
    }

    public void BringToFront()
    {
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Activate();
    }
}
