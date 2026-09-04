using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class PttOriginateView : UserControl
{
    public PttOriginateView() { InitializeComponent(); }

    /// <summary>대상 필드 Enter = 사설콜 발신(입력창 옆 버튼 없음 — 패드 📞·제안 행 버튼과 같은 동작).</summary>
    private void Target_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && DataContext is PttOriginateViewModel vm && vm.CanStart) { vm.StartCommand.Execute(null); e.Handled = true; }
    }
}
