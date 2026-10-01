// 오른쪽 패널 겹침(dispatch_desktop_ui.md §3.6) — 본문 위에 오른쪽 끝에서 왼쪽으로 밀려 들어오고(열기) 다시 오른쪽으로 밀려 나간다(닫기).
//   본문 배치는 그대로 둔다(겹칠 뿐 좁히지 않는다). 같은 패널 안에서 내용만 바뀌면(다른 대상 = 교체) 움직이지 않는다.
//   닫기는 패널 VM 이 곧바로 내용을 비우므로(View = None) 닫는 순간의 그림을 떠 두고 그것을 밀어낸다 — 빈 패널이 미끄러지지 않게.
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;

namespace DispatchDesktop.Views;

public sealed class SlideOverPanel : Grid
{
    public static readonly DependencyProperty IsOpenProperty = DependencyProperty.Register(
        nameof(IsOpen), typeof(bool), typeof(SlideOverPanel), new PropertyMetadata(false, (d, e) => ((SlideOverPanel)d).Apply((bool)e.NewValue)));

    private static readonly Duration InTime = TimeSpan.FromMilliseconds(220);
    private static readonly Duration OutTime = TimeSpan.FromMilliseconds(180);
    private readonly TranslateTransform _shift = new();
    private readonly Image _snapshot = new()
    {
        Stretch = Stretch.Fill, IsHitTestVisible = false, Visibility = Visibility.Collapsed,
        HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
    };
    private int _slides;

    public SlideOverPanel()
    {
        RenderTransform = _shift;
        Visibility = Visibility.Collapsed;
        ClipToBounds = false;
        Loaded += (_, _) =>
        {
            if (!Children.Contains(_snapshot)) { Children.Add(_snapshot); SetZIndex(_snapshot, 1); }
            if (IsOpen) { Visibility = Visibility.Visible; _shift.X = 0; }
        };
    }

    /// <summary>열림 — 패널 VM 의 IsOpen.</summary>
    public bool IsOpen { get => (bool)GetValue(IsOpenProperty); set => SetValue(IsOpenProperty, value); }

    /// <summary>밀어낼 거리 — 패널 폭(아직 배치 전이면 지정 폭, 그것도 없으면 440).</summary>
    private double Travel => ActualWidth > 0 ? ActualWidth : !double.IsNaN(Width) ? Width : 440;

    private void Apply(bool open)
    {
        double from = _shift.X;                                   // 진행 중 애니메이션의 지금 값에서 이어 간다
        _shift.BeginAnimation(TranslateTransform.XProperty, null);
        ++_slides;
        _shift.X = from;
        bool animate = IsLoaded && SystemParameters.ClientAreaAnimation;   // 시스템 «창 안의 애니메이션» 을 끈 사용자는 움직이지 않는다
        if (open)
        {
            ShowLive();
            if (Visibility != Visibility.Visible) { Visibility = Visibility.Visible; _shift.X = from = Travel; }
            if (!animate) { _shift.X = 0; return; }
            if (from <= 0) return;
            Slide(0, InTime, new CubicEase { EasingMode = EasingMode.EaseOut }, null);
        }
        else
        {
            if (Visibility != Visibility.Visible) return;
            if (!animate) { Visibility = Visibility.Collapsed; return; }
            Freeze();
            Slide(Travel, OutTime, new CubicEase { EasingMode = EasingMode.EaseIn }, () =>
            {
                if (IsOpen) return;                               // 그사이 다시 열렸다
                Visibility = Visibility.Collapsed;
                ShowLive();
            });
        }
    }

    private void Slide(double to, Duration d, IEasingFunction ease, Action? done)
    {
        double from = _shift.X;
        int gen = ++_slides;                                      // 뒤에 시작한 이동이 있으면 앞 이동의 끝맺음은 버린다
        var a = new DoubleAnimation(from, to, d) { EasingFunction = ease, FillBehavior = FillBehavior.Stop };
        a.Completed += (_, _) => { if (gen == _slides) done?.Invoke(); };
        _shift.X = to;                                            // 애니메이션이 끝나 풀린 뒤의 값
        _shift.BeginAnimation(TranslateTransform.XProperty, a);
    }

    /// <summary>닫는 순간의 그림을 떠 두고 살아 있는 내용은 가린다(VM 이 곧 비운다).</summary>
    private void Freeze()
    {
        if (ActualWidth <= 0 || ActualHeight <= 0 || Children.OfType<FrameworkElement>().FirstOrDefault(c => c != _snapshot) is not { } content) return;
        var dpi = VisualTreeHelper.GetDpi(this);
        var rtb = new RenderTargetBitmap((int)Math.Ceiling(ActualWidth * dpi.DpiScaleX), (int)Math.Ceiling(ActualHeight * dpi.DpiScaleY),
                                         dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        var shot = new DrawingVisual();                           // 내용만(이 패널의 이동 변환은 빼고) 제 자리에 그린다
        var box = new Rect(0, 0, content.ActualWidth, content.ActualHeight);
        // Viewbox 를 배치 사각형으로 못박는다 — 기본(경계 상자)은 그림자 효과만큼 넓어 그림이 그만큼 밀린다
        using (var dc = shot.RenderOpen())
            dc.DrawRectangle(new VisualBrush(content) { Stretch = Stretch.None, Viewbox = box, ViewboxUnits = BrushMappingMode.Absolute }, null, box);
        rtb.Render(shot);
        rtb.Freeze();
        _snapshot.Source = rtb;
        _snapshot.Width = ActualWidth;                            // 크기를 못박는다 — Stretch=Fill 인 Image 는 주는 폭을 다 달라고 해 패널이 본문 폭으로 늘어난다
        _snapshot.Height = ActualHeight;
        _snapshot.Visibility = Visibility.Visible;
        foreach (UIElement c in Children) if (c != _snapshot) c.Opacity = 0;
    }

    private void ShowLive()
    {
        _snapshot.Visibility = Visibility.Collapsed;
        _snapshot.Source = null;
        foreach (UIElement c in Children) if (c != _snapshot) c.Opacity = 1;
    }
}
