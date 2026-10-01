using System.Windows;

namespace DispatchDesktop.Shell;

public partial class ConfirmWindow : Window
{
    public ConfirmWindow(string title, string text, string ok = "확인", bool danger = false)
    {
        InitializeComponent();
        Title = title; Heading.Text = title; Body.Text = text; Ok.Content = ok;
        if (danger) Ok.Style = (Style)FindResource("Btn.Danger");
        // 삭제처럼 되돌릴 수 없는 일은 Enter 가 취소 — 나머지는 Enter = 확인
        Ok.IsDefault = !danger; Cancel.IsDefault = danger;
        Loaded += (_, _) => (danger ? Cancel : Ok).Focus();
    }

    private void Ok_Click(object sender, RoutedEventArgs e) => DialogResult = true;

    /// <summary>예/아니오 — owner 가 보이면 그 가운데에 모달로 띄운다. 주 창이 숨은 때(트레이에서 종료)는 화면 가운데·작업 표시줄에 두고 앞으로 올린다.</summary>
    public static bool Ask(Window? owner, string title, string text, string ok = "확인", bool danger = false)
    {
        var w = new ConfirmWindow(title, text, ok, danger);
        if (owner is { IsVisible: true }) w.Owner = owner;
        else { w.WindowStartupLocation = WindowStartupLocation.CenterScreen; w.ShowInTaskbar = true; w.Topmost = true; }
        return w.ShowDialog() == true;
    }
}
