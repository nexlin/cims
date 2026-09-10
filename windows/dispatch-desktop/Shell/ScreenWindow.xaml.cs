// 화면 별창(§3.4) 코드비하인드 — 화면 고정·제목·닫힘 통지 + 앱 핫키(§8) 를 주 창 규칙으로 라우팅.
// 관제 요약 띠는 별창에도 붙어 관제 상태를 놓치지 않는다.
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
        // 핫키는 앱의 것 — 별창에 포커스가 있어도 PTT 폴백·응답/종료·F1~F4 가 주 창과 같은 규칙으로 동작한다
        // (화면 전환 키의 창 활성화까지 주 창 쪽 처리에 있다).
        PreviewKeyDown += (_, e) => (Owner as MainWindow)?.RouteKeyDown(e);
        PreviewKeyUp += (_, e) => (Owner as MainWindow)?.RouteKeyUp(e);
    }

    public void BringToFront()
    {
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Activate();
    }
}
