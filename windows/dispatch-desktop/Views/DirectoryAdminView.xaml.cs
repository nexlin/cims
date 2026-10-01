// [관리] 화면 본문(§4.5) 코드비하인드 — 확인 대화상자(소유 창 기준)·비밀번호 상자(PasswordBox 는 바인딩 불가)·더블클릭·좁은 구성원 표(IsNarrow)만 든다.
// 같은 VM 을 주 창과 별창이 번갈아 붙이므로 Confirm 은 Loaded 때 자기 창으로 다시 건다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class DirectoryAdminView : UserControl
{
    private DirectoryAdminViewModel? Vm => DataContext as DirectoryAdminViewModel;

    /// <summary>구성원 표가 좁다 — 작은 창(1366 등)에서 번호·자격 열(540)이 이름 열을 없앤다. 좁으면 그 열들을 접고 이름 아래 한 줄로 보인다.</summary>
    public static readonly DependencyProperty IsNarrowProperty = DependencyProperty.Register(nameof(IsNarrow), typeof(bool), typeof(DirectoryAdminView), new PropertyMetadata(false));
    public bool IsNarrow { get => (bool)GetValue(IsNarrowProperty); private set => SetValue(IsNarrowProperty, value); }
    public const double NarrowWidth = 760;

    public DirectoryAdminView()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            if (Vm is null) return;
            Vm.Confirm = Confirm;
            Vm.PropertyChanged -= OnVmChanged; Vm.PropertyChanged += OnVmChanged;
        };
        Unloaded += (_, _) => { if (Vm is not null) Vm.PropertyChanged -= OnVmChanged; };
    }

    private void OnVmChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(DirectoryAdminViewModel.MemberEditing) && Vm?.MemberEditing == true) { LoginPw.Password = ""; VoltePw.Password = ""; VoipPw.Password = ""; PttPw.Password = ""; }
    }

    private bool Confirm(string title, string text) =>
        Shell.ConfirmWindow.Ask(Window.GetWindow(this), title, text, title, danger: title.EndsWith("삭제", StringComparison.Ordinal));   // 삭제 확인 = 빨강·Enter 취소

    private void LoginPw_Changed(object sender, RoutedEventArgs e) { if (Vm is not null) Vm.EditPassword = LoginPw.Password; }
    private void VoltePw_Changed(object sender, RoutedEventArgs e) { if (Vm is not null) Vm.VoltePassword = VoltePw.Password; }
    private void VoipPw_Changed(object sender, RoutedEventArgs e) { if (Vm is not null) Vm.VoipPassword = VoipPw.Password; }
    private void PttPw_Changed(object sender, RoutedEventArgs e) { if (Vm is not null) Vm.PttPassword = PttPw.Password; }
    private void MemberCard_SizeChanged(object sender, SizeChangedEventArgs e) => IsNarrow = e.NewSize.Width < NarrowWidth;
    private void Members_DoubleClick(object sender, MouseButtonEventArgs e) { if (Vm?.SelectedMember is not null) Vm.EditMemberCommand.Execute(null); }
}
