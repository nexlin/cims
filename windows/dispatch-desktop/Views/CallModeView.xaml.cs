// [통화] 한 화면 — 번호칸 Enter = 발신 · Esc = 제안 접기, 그룹원 칸 누름 = 번호칸에 채움 · 오른쪽 = 사람 메뉴, 기록 목록 폭 끌기 저장(§3.3),
// 오른쪽 칸이 좁으면(작은 창 — IsNarrow) «기록» 은 목록만(한 줄기는 넓어지면 돌아온다), 문자 Enter = 보내기, 새 사건이 붙으면 한 줄기 맨 아래로.
// 오른쪽 패널은 이 화면 위에 겹치므로(§3.6) 배치를 바꾸지 않는다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class CallModeView : UserControl
{
    /// <summary>오른쪽 칸이 좁다 — 창이 작아 오른쪽 칸이 640 에 못 미친다. 좁으면 «기록» 목록만 · 제목·CSV 숨김.</summary>
    public static readonly DependencyProperty IsNarrowProperty = DependencyProperty.Register(nameof(IsNarrow), typeof(bool), typeof(CallModeView), new PropertyMetadata(false));
    public bool IsNarrow { get => (bool)GetValue(IsNarrowProperty); private set => SetValue(IsNarrowProperty, value); }
    public const double NarrowWidth = 640;

    private MainViewModel? _vm;

    public CallModeView()
    {
        InitializeComponent();
        DataContextChanged += (_, _) => Hook();
    }

    private void Hook()
    {
        if (_vm is not null) _vm.Records.ItemsGrew -= OnItemsGrew;
        _vm = DataContext as MainViewModel;
        if (_vm is null) return;
        _vm.Records.ItemsGrew += OnItemsGrew;
        ApplyNarrow();
    }

    private bool RightNarrow => Board.ActualWidth > 0 && Board.ActualWidth - LeftCol.Width.Value < NarrowWidth;
    private void OnItemsGrew(object? sender, EventArgs e) => Dispatcher.BeginInvoke(() => RecordScroll.ScrollToEnd());

    /// <summary>오른쪽 칸이 좁으면(작은 창) 목록만 남기고, 넓어지면 끈 폭으로 돌아온다.</summary>
    private void ApplyNarrow()
    {
        if (_vm is null) return;
        bool narrow = RightNarrow;
        IsNarrow = narrow;
        RecordCol.Width = narrow ? new GridLength(1, GridUnitType.Star) : new GridLength(_vm.Layout.File.Seams.RecordList);
        TimelineCol.Width = narrow ? new GridLength(0) : new GridLength(1, GridUnitType.Star);
        Timeline.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
        RecordSeam.Visibility = narrow ? Visibility.Collapsed : Visibility.Visible;
    }

    private void RecordSeam_DragCompleted(object sender, DragCompletedEventArgs e)
    {
        if (_vm is null || IsNarrow) return;
        _vm.Layout.File.Seams.RecordList = Math.Round(RecordCol.ActualWidth);
        _vm.Layout.Save();
    }

    private void Number_KeyDown(object sender, KeyEventArgs e)
    {
        if (_vm is null) return;
        if (e.Key == Key.Enter) { _vm.CallOriginate.DialCommand.Execute(null); e.Handled = true; }
        else if (e.Key == Key.Escape && _vm.CallOriginate.HasSuggestions) { _vm.CallOriginate.ClearSuggestions(); e.Handled = true; }
    }
    private void Number_LostFocus(object sender, KeyboardFocusChangedEventArgs e) => _vm?.CallOriginate.ClearSuggestions();

    private static bool FromButton(object sender, MouseButtonEventArgs e)
    {
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != sender; d = System.Windows.Media.VisualTreeHelper.GetParent(d))
            if (d is ButtonBase) return true;
        return false;
    }

    private void Member_Click(object sender, MouseButtonEventArgs e)
    {
        if (sender is not FrameworkElement { DataContext: MemberChip chip } || FromButton(sender, e) || chip.IsMe) return;
        chip.FillCommand.Execute(null);
        NumberBox.Focus();
    }
    private void Member_RightClick(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: MemberChip chip } && !chip.IsMe) { chip.MenuCommand.Execute(null); e.Handled = true; }
    }

    private void Sms_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && Keyboard.Modifiers != ModifierKeys.Shift && _vm?.Records is { } r && r.SendCommand.CanExecute(null)) { r.SendCommand.Execute(null); e.Handled = true; }
    }

    /// <summary>왼쪽 칸 1040 고정(§3.1) — 창이 좁아 오른쪽 칸이 360 보다 작아질 때만 왼쪽을 줄인다(640 까지). 1920 폭에서는 패널을 열어도 1040 그대로다.</summary>
    private void Board_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        double w = Math.Clamp(e.NewSize.Width - 360, 640, 1040);
        if (Math.Abs(LeftCol.Width.Value - w) > 0.5) LeftCol.Width = new GridLength(w);
        ApplyNarrow();
    }
}
