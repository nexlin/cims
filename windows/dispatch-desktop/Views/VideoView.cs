// 영상 칸의 그림(dispatch_desktop_ui.md §10.3) — VideoFeed 의 최신 장을 WriteableBitmap 하나에 그린다. 크기가 바뀌면(상대 카메라 회전·
// 인코더 크기 변경) 비트맵을 새로 만든다. 회전은 바깥의 LayoutTransform 이 한다(보내는 사람마다 기억 — ChannelDetailViewModel.VideoRotation).
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DispatchDesktop.Services;

namespace DispatchDesktop.Views;

public sealed class VideoView : Image
{
    public static readonly DependencyProperty FeedProperty = DependencyProperty.Register(
        nameof(Feed), typeof(VideoFeed), typeof(VideoView), new PropertyMetadata(null, (d, e) => ((VideoView)d).OnFeedChanged((VideoFeed?)e.OldValue)));
    private static readonly DependencyPropertyKey HasPictureKey = DependencyProperty.RegisterReadOnly(
        nameof(HasPicture), typeof(bool), typeof(VideoView), new PropertyMetadata(false));
    public static readonly DependencyProperty HasPictureProperty = HasPictureKey.DependencyProperty;

    private WriteableBitmap? _bmp;
    private bool _live;

    public VideoView()
    {
        Stretch = Stretch.Uniform;
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.HighQuality);
        Loaded += (_, _) => Attach(true);
        Unloaded += (_, _) => Attach(false);
    }

    /// <summary>그릴 우편함 — null 이면 비운다.</summary>
    public VideoFeed? Feed { get => (VideoFeed?)GetValue(FeedProperty); set => SetValue(FeedProperty, value); }
    /// <summary>한 장이라도 그렸다 — 자리 표시 글(«영상 기다리는 중…»)을 거두는 데 쓴다.</summary>
    public bool HasPicture => (bool)GetValue(HasPictureProperty);

    private void OnFeedChanged(VideoFeed? old)
    {
        if (old is not null) old.FrameReady -= OnFrameReady;
        Clear();
        if (_live && Feed is { } f) { f.FrameReady += OnFrameReady; OnFrameReady(f, EventArgs.Empty); }
    }

    /// <summary>화면에 붙은 동안만 받는다 — 떨어진 칸(패널 닫힘·탭 전환)이 우편함을 붙잡지 않게.</summary>
    private void Attach(bool on)
    {
        if (_live == on) return;
        _live = on;
        if (Feed is not { } f) return;
        if (on) { f.FrameReady += OnFrameReady; OnFrameReady(f, EventArgs.Empty); }
        else f.FrameReady -= OnFrameReady;
    }

    private void OnFrameReady(object? sender, EventArgs e)
    {
        if (sender is not VideoFeed f || f != Feed) return;
        bool drew = f.Draw((px, w, h, stride) =>
        {
            if (_bmp is null || _bmp.PixelWidth != w || _bmp.PixelHeight != h)
            {
                _bmp = new WriteableBitmap(w, h, 96, 96, PixelFormats.Bgr32, null);
                Source = _bmp;
            }
            _bmp.WritePixels(new Int32Rect(0, 0, w, h), px, stride, 0);
        });
        if (!drew) Clear();
        else if (!HasPicture) SetValue(HasPictureKey, true);
    }

    private void Clear()
    {
        _bmp = null;
        Source = null;
        SetValue(HasPictureKey, false);
    }
}
