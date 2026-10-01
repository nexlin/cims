// [이력] 화면 본문(§4.6) 코드비하인드 — 녹취 재생기(MediaElement)·재생 바 마우스·발언 타임라인 확대. 같은 VM 을 주 창과 별창이 번갈아 붙이므로
// 재생 이벤트는 Loaded~Unloaded 사이에만 구독한다(두 인스턴스가 동시에 재생하지 않게). 언로드되면 재생을 멈춘다.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using DispatchDesktop.ViewModels;

namespace DispatchDesktop.Views;

public partial class HistoryView : UserControl
{
    private SessionHistoryViewModel? Vm => DataContext as SessionHistoryViewModel;
    private SessionHistoryViewModel? _bound;
    /// <summary>재생 헤드 갱신 — 200 ms 마다 재생기 위치를 VM 에 알린다(말 없는 구간은 VM 이 시계로 흘린다).</summary>
    private readonly System.Windows.Threading.DispatcherTimer _tick = new() { Interval = TimeSpan.FromMilliseconds(200) };
    private MediaOpenArgs? _pendingOpen;

    public HistoryView()
    {
        InitializeComponent();
        _tick.Tick += (_, _) => _bound?.Tick(_pendingOpen is null && Player.Source is not null ? Player.Position.TotalSeconds : null);
        Loaded += (_, _) =>
        {
            if (Vm is null || _bound == Vm) return;
            _bound = Vm;
            _bound.OpenRequested += OnOpen; _bound.SeekRequested += OnSeek; _bound.PauseRequested += OnPause;
            _bound.SpeedRequested += OnSpeed; _bound.StopRequested += OnStop;
            _bound.PropertyChanged += OnVmChanged;
            _tick.Start();
            ApplyColumnWidths();
        };
        Unloaded += (_, _) =>
        {
            if (_bound is null) return;
            _bound.Stop();
            _bound.OpenRequested -= OnOpen; _bound.SeekRequested -= OnSeek; _bound.PauseRequested -= OnPause;
            _bound.SpeedRequested -= OnSpeed; _bound.StopRequested -= OnStop;
            _bound.PropertyChanged -= OnVmChanged;
            _bound = null;
            _tick.Stop(); _pendingOpen = null;
            Player.Stop(); Player.Source = null;
            SessionHistoryViewModel.CleanupTemp();
        };
    }

    /// <summary>종류(통화/PTT)에 맞는 기본 열 폭 — 통화는 표가 주역(목록 * · 녹취 380), PTT 는 세션 패널이 주역(1 : 3). 경계 드래그 값은 종류를 바꾸면 기본으로 돌아간다.</summary>
    private void ApplyColumnWidths()
    {
        if (Vm is null) return;
        ListCol.Width = Vm.ListWidth; PaneCol.Width = Vm.PaneWidth;
    }
    private void OnVmChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e) { if (e.PropertyName == nameof(SessionHistoryViewModel.KindIndex)) ApplyColumnWidths(); }
    private void Splitter_DoubleClick(object sender, MouseButtonEventArgs e) => ApplyColumnWidths();

    // ── 발언 타임라인: Ctrl+휠 = 커서 기준 확대·축소, Shift+휠 = 가로 이동, 빈 곳 드래그 = 가로 이동 (콘솔 LaneTimebar 와 같은 조작) ──
    private Point? _panStart; private double _panOffset;

    private void LaneScroll_PreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (Vm is null) return;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            double x = e.GetPosition(LaneScroll).X;
            double extent = Math.Max(1, LaneScroll.ExtentWidth);
            double anchor = (LaneScroll.HorizontalOffset + x) / extent;               // 커서가 가리키는 콘텐츠 비율
            Vm.TalkZoomBy(e.Delta > 0 ? 1.25 : 1 / 1.25);
            LaneScroll.Dispatcher.BeginInvoke(() =>
            {
                LaneScroll.UpdateLayout();
                LaneScroll.ScrollToHorizontalOffset(Math.Max(0, anchor * LaneScroll.ExtentWidth - x));
            }, System.Windows.Threading.DispatcherPriority.Loaded);
            e.Handled = true;
        }
        else if (Keyboard.Modifiers.HasFlag(ModifierKeys.Shift))                   // 맨휠은 패널 세로 스크롤에 양보
        {
            LaneScroll.ScrollToHorizontalOffset(LaneScroll.HorizontalOffset - e.Delta);
            e.Handled = true;
        }
    }

    private void LaneScroll_MouseDown(object sender, MouseButtonEventArgs e)
    {
        // 막대(Tag=turn) 위는 클릭 = 재생이므로 이동을 시작하지 않는다
        for (var d = e.OriginalSource as DependencyObject; d is not null && d != LaneScroll; d = System.Windows.Media.VisualTreeHelper.GetParent(d))
            if (d is Border { Tag: "turn" }) return;
        if (Vm is null || !Vm.IsTalkZoomed) return;
        _panStart = e.GetPosition(LaneScroll); _panOffset = LaneScroll.HorizontalOffset;
        LaneScroll.CaptureMouse(); LaneScroll.Cursor = Cursors.SizeWE;
    }
    private void LaneScroll_MouseMove(object sender, MouseEventArgs e)
    {
        if (_panStart is not { } p0) return;
        LaneScroll.ScrollToHorizontalOffset(_panOffset - (e.GetPosition(LaneScroll).X - p0.X));
    }
    private void LaneScroll_MouseUp(object sender, MouseButtonEventArgs e)
    {
        if (_panStart is null) return;
        _panStart = null; LaneScroll.ReleaseMouseCapture(); LaneScroll.Cursor = null;
    }

    // ── 녹취 재생기 — 열기는 MediaOpened 뒤에 위치·속도·재생/멈춤을 건다(열리기 전 Position 은 무시된다) ──
    private void OnOpen(object? sender, MediaOpenArgs a)
    {
        _pendingOpen = a;
        Player.Stop(); Player.Source = new Uri(a.Path);
        Player.Play();                                                            // Manual 재생기는 Play 로 연다 — 멈춤이면 MediaOpened 에서 바로 Pause
    }
    private void Player_MediaOpened(object sender, RoutedEventArgs e)
    {
        if (_pendingOpen is not { } a) return;
        _pendingOpen = null;
        Player.SpeedRatio = Vm?.Speed ?? 1.0;
        Player.Position = TimeSpan.FromSeconds(Math.Max(0, a.OffsetSec));
        if (!a.Play) Player.Pause();
    }
    private void OnSeek(object? sender, double sec) => Player.Position = TimeSpan.FromSeconds(Math.Max(0, sec));
    private void OnPause(object? sender, bool pause) { if (pause) Player.Pause(); else { Player.SpeedRatio = Vm?.Speed ?? 1.0; Player.Play(); } }
    private void OnSpeed(object? sender, double ratio) => Player.SpeedRatio = ratio;
    private void OnStop(object? sender, EventArgs e) { _pendingOpen = null; Player.Stop(); Player.Source = null; }

    // ── 재생 바 — 누른 곳부터 재생, 마우스를 올리면 그 지점 시각 ──
    private static double RatioIn(object sender, MouseEventArgs e) =>
        sender is FrameworkElement f && f.ActualWidth > 0 ? e.GetPosition(f).X / f.ActualWidth : 0;
    private void Bar_MouseDown(object sender, MouseButtonEventArgs e) { Vm?.SeekRatio(RatioIn(sender, e)); e.Handled = true; }
    private void Bar_MouseMove(object sender, MouseEventArgs e) => Vm?.HoverAt(RatioIn(sender, e));
    private void Bar_MouseLeave(object sender, MouseEventArgs e) => Vm?.HoverEnd();

    /// <summary>본문 3구획의 높이 = 보이는 높이, 단 DetailMinHeight 아래로는 줄이지 않는다(그때는 바깥이 세로로 넘긴다) — ScrollViewer 는 안을 무한 높이로 재므로 * 행이 칸을 채우도록 높이를 직접 준다.</summary>
    private const double DetailMinHeight = 420;
    private void DetailScroll_SizeChanged(object sender, SizeChangedEventArgs e) =>
        DetailBody.Height = Math.Max(DetailMinHeight, e.NewSize.Height - DetailBody.Margin.Top - DetailBody.Margin.Bottom);

    private void Player_MediaEnded(object sender, RoutedEventArgs e) => Vm?.OnMediaEnded();
    private void Player_MediaFailed(object sender, ExceptionRoutedEventArgs e) => Vm?.OnMediaFailed(e.ErrorException.Message);
}
