using System.Windows;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class LoginWindow : Window
{
    private readonly LoginViewModel _vm;

    /// <param name="autoResume">저장된 로그인으로 이어 접속한다 — **이 창을 띄운 채로** 한다(«저장된 로그인으로 접속 중…»). 창 없이 하면 서버가 느릴 때
    /// 바로가기를 눌러도 한참 아무것도 보이지 않는다. 성공하면 창이 스스로 닫히고, 실패하면 사유를 적고 그대로 수동 로그인을 받는다.</param>
    public LoginWindow(LoginViewModel vm, bool autoResume = false)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;
        vm.Succeeded += (_, _) => { DialogResult = true; Close(); };
        Loaded += async (_, _) =>
        {
            if (autoResume)
            {
                try { if (await vm.ResumeAsync()) return; }
                catch (Exception ex) { vm.Busy = false; vm.Status = ""; vm.Error = "자동 로그인 실패 — " + ex.Message; }
            }
            if (vm.LoginId.Length > 0) Pw.Focus();
        };
    }

    private void Pw_Changed(object sender, RoutedEventArgs e) => _vm.Password = Pw.Password;
    private void Pw_KeyDown(object sender, KeyEventArgs e) { if (e.Key == Key.Enter && _vm.CanLogin) _vm.LoginCommand.Execute(null); }
}
