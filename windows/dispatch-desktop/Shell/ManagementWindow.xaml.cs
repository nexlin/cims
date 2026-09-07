// 관리 창(§4.5) 코드비하인드 — 확인 대화상자·비밀번호 상자(PasswordBox 는 바인딩 불가)·그룹 편집 창·녹취 재생(MediaElement) 만 든다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Shell;

public partial class ManagementWindow : Window
{
    private readonly ManagementViewModel _vm;

    public ManagementWindow(ManagementViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        DataContext = vm;
        vm.Directory.Confirm = Confirm;
        vm.Groups.Confirm = Confirm;
        vm.Groups.EditRequested += (_, g) => { var w = new GroupEditWindow(g) { Owner = this }; w.ShowDialog(); };
        vm.History.PlayRequested += (_, path) => { Player.Stop(); Player.Source = new Uri(path); Player.Play(); };
        vm.History.StopRequested += (_, _) => { Player.Stop(); Player.Source = null; };
        vm.Directory.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(DirectoryAdminViewModel.MemberEditing) && vm.Directory.MemberEditing) { LoginPw.Password = ""; VoltePw.Password = ""; PttPw.Password = ""; }
        };
        Loaded += async (_, _) => await vm.LoadAsync();
        Closed += (_, _) => { vm.History.Stop(); SessionHistoryViewModel.CleanupTemp(); };
    }

    private bool Confirm(string title, string text) =>
        MessageBox.Show(this, text, title, MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes;

    private void LoginPw_Changed(object sender, RoutedEventArgs e) => _vm.Directory.EditPassword = LoginPw.Password;
    private void VoltePw_Changed(object sender, RoutedEventArgs e) => _vm.Directory.VoltePassword = VoltePw.Password;
    private void PttPw_Changed(object sender, RoutedEventArgs e) => _vm.Directory.PttPassword = PttPw.Password;

    private void Members_DoubleClick(object sender, MouseButtonEventArgs e) { if (_vm.Directory.SelectedMember is not null) _vm.Directory.EditMemberCommand.Execute(null); }
    private void Groups_DoubleClick(object sender, MouseButtonEventArgs e) { if (_vm.Groups.Selected is not null) _vm.Groups.EditGroupCommand.Execute(null); }
    private void Segments_DoubleClick(object sender, MouseButtonEventArgs e) { if (_vm.History.CanPlay) _vm.History.PlayCommand.Execute(null); }

    private void Player_MediaEnded(object sender, RoutedEventArgs e) => _vm.History.OnMediaEnded();
    private void Player_MediaFailed(object sender, ExceptionRoutedEventArgs e) => _vm.History.OnMediaFailed(e.ErrorException.Message);
}
