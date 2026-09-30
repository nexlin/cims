// [무전] 한 화면 — 카드·타 채널 행 누름 = 채널 상세(버튼·✓ 위 누름은 그 컨트롤 몫), 칸 경계 끌기 저장(§3.3), 메시지 Enter 보내기,
// 이벤트 [따라가기] = 새 줄이 오면 맨 위로.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

/// <summary>내 채널 격자 끝의 [+ 개별 · 애드혹 열기] 타일(자료 없는 자리표시 항목).</summary>
public sealed class AddTile { }

public partial class PttModeView : UserControl
{
    public PttModeView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Hook();
    }

    private MainViewModel? Vm => DataContext as MainViewModel;
    private PttActivityViewModel? _events;

    private void Hook()
    {
        if (Vm is not { } vm) return;
        var seams = vm.Layout.File.Seams;
        TopRow.Height = new GridLength(seams.PttTop);
        ThreadCol.Width = new GridLength(seams.ThreadList);
        if (_events is not null) _events.RowInserted -= OnRowInserted;
        _events = vm.PttActivity;
        _events.RowInserted += OnRowInserted;
    }

    private void OnRowInserted(object? sender, EventArgs e) => Dispatcher.BeginInvoke(() => EventsScroll.ScrollToTop());

    /// <summary>누른 자리가 버튼(✓·음소거·[참여]·[청취] 등) 안이면 그 컨트롤 몫이다.</summary>
    private static bool FromButton(object sender, MouseButtonEventArgs e)
    {
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return true;
        return false;
    }

    private void Card_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ChannelCard c } || FromButton(sender, e)) return;
        Vm?.OpenChannel(c);
    }

    private void Other_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: ScopedCard o } || FromButton(sender, e)) return;
        Vm?.OpenOther(o);
    }

    private void TopSeam_DragCompleted(object sender, DragCompletedEventArgs e)
    {
        if (Vm is not { } vm) return;
        vm.Layout.File.Seams.PttTop = Math.Round(TopRow.ActualHeight);
        vm.Layout.Save();
    }

    private void ThreadSeam_DragCompleted(object sender, DragCompletedEventArgs e)
    {
        if (Vm is not { } vm) return;
        vm.Layout.File.Seams.ThreadList = Math.Round(ThreadCol.ActualWidth);
        vm.Layout.Save();
    }

    private void Input_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Keyboard.Modifiers != ModifierKeys.Shift && Vm?.McData is { } m && m.SendCommand.CanExecute(null))
        {
            m.SendCommand.Execute(null);
            e.Handled = true;
        }
    }

    /// <summary>왼쪽 칸 1040 고정(§3.1) — 창이 좁아 오른쪽 칸이 360 보다 작아질 때만 왼쪽을 줄인다(640 까지). 1920 폭에서는 패널을 열어도 1040 그대로다.</summary>
    private void Board_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = Math.Clamp(e.NewSize.Width - 360, 640, 1040);
        if (Math.Abs(LeftCol.Width.Value - w) > 0.5) LeftCol.Width = new GridLength(w);
    }
}
