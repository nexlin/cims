// [이력] 화면 본문(§4.6) 코드비하인드 — 녹취 재생(MediaElement)·더블클릭만. 같은 VM 을 주 창과 별창이 번갈아 붙이므로
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

    public HistoryView()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            if (Vm is null || _bound == Vm) return;
            _bound = Vm;
            _bound.PlayRequested += OnPlay; _bound.StopRequested += OnStop;
        };
        Unloaded += (_, _) =>
        {
            if (_bound is null) return;
            _bound.Stop();
            _bound.PlayRequested -= OnPlay; _bound.StopRequested -= OnStop;
            _bound = null;
            Player.Stop(); Player.Source = null;
            SessionHistoryViewModel.CleanupTemp();
        };
    }

    private void OnPlay(object? sender, string path) { Player.Stop(); Player.Source = new Uri(path); Player.Play(); }
    private void OnStop(object? sender, EventArgs e) { Player.Stop(); Player.Source = null; }

    private void Segments_DoubleClick(object sender, MouseButtonEventArgs e) { if (Vm?.CanPlay == true) Vm.PlayCommand.Execute(null); }
    private void Player_MediaEnded(object sender, RoutedEventArgs e) => Vm?.OnMediaEnded();
    private void Player_MediaFailed(object sender, ExceptionRoutedEventArgs e) => Vm?.OnMediaFailed(e.ErrorException.Message);
}
