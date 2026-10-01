// 영상 그림의 우편함(dispatch_desktop_ui.md §10.3 — 엔진 = 창 없는 프레임 렌더, ue_sdk.md §4.5).
//   엔진 VideoFrameReceived 는 영상 스레드에서 프레임마다 온다 — 화면이 보고 있는 호(Feed 를 만든 호)만 최신 한 장을 복사해 두고
//   UI 스레드에 «새 장» 을 한 번만 알린다. 앞 장을 그리기 전에 온 장은 덮어쓴다(밀리면 버린다 — 지연이 쌓이지 않는다).
//   그리기(WriteableBitmap)는 Views/VideoView 의 몫 — 이 층은 WPF 를 모른다.
using System.Collections.Concurrent;
using CimsUe;

namespace DispatchDesktop.Services;

/// <summary>호 하나(또는 셀프뷰 -1)의 최신 프레임 — BGRA 32 bpp, 위 줄부터.</summary>
public sealed class VideoFeed
{
    private readonly object _gate = new();
    private readonly SynchronizationContext? _ui;
    private byte[] _px = Array.Empty<byte>();
    private int _w, _h, _stride;
    private bool _posted, _has;
    public int CallId { get; }
    /// <summary>받은 장 수(그린 장이 아니다).</summary>
    public long Frames { get; private set; }
    public DateTime LastFrameAt { get; private set; }
    /// <summary>UI 스레드 — 새 장이 있다(또는 Reset). 받는 쪽이 <see cref="Draw"/> 로 꺼내 그린다.</summary>
    public event EventHandler? FrameReady;

    internal VideoFeed(int callId, SynchronizationContext? ui) { CallId = callId; _ui = ui; }

    /// <summary>영상 스레드 — 복사하고 곧 돌아간다.</summary>
    internal void Push(in VideoFrame f)
    {
        if (f.Width <= 0 || f.Height <= 0 || f.Pixels.Length < f.Stride * f.Height) return;
        bool post;
        lock (_gate)
        {
            int n = f.Stride * f.Height;
            if (_px.Length != n) _px = new byte[n];
            f.Pixels[..n].CopyTo(_px);
            _w = f.Width; _h = f.Height; _stride = f.Stride; _has = true;
            Frames++; LastFrameAt = DateTime.Now;
            post = !_posted;
            _posted = true;
        }
        if (post) Raise();
    }

    /// <summary>그림을 비운다 — 보던 송출을 그만 보거나 바꿀 때(앞 사람의 마지막 장이 남지 않게).</summary>
    public void Reset()
    {
        lock (_gate) { _has = false; _posted = true; }
        Raise();
    }

    /// <summary>UI 스레드 — 최신 장을 잠금 안에서 그린다(그리는 동안 다음 장이 덮어쓰지 않게). 장이 없으면 false.</summary>
    public bool Draw(Action<byte[], int, int, int> draw)
    {
        lock (_gate)
        {
            _posted = false;
            if (!_has) return false;
            draw(_px, _w, _h, _stride);
            return true;
        }
    }

    private void Raise()
    {
        if (_ui is null) { FrameReady?.Invoke(this, EventArgs.Empty); return; }
        _ui.Post(static o => { var f = (VideoFeed)o!; f.FrameReady?.Invoke(f, EventArgs.Empty); }, this);
    }
}

/// <summary>호별 영상 우편함 — 화면이 <see cref="Feed"/> 로 만든 것만 받는다(보지 않는 호의 프레임은 복사하지 않는다).</summary>
public sealed class VideoFrames
{
    /// <summary>셀프뷰(내 카메라) — 코어 VideoFrame.CallId -1.</summary>
    public const int SelfView = -1;
    private readonly ConcurrentDictionary<int, VideoFeed> _feeds = new();
    private readonly SynchronizationContext? _ui;

    public VideoFrames(SynchronizationContext? ui) { _ui = ui; }

    public VideoFeed Feed(int callId) => _feeds.GetOrAdd(callId, id => new VideoFeed(id, _ui));
    public void Remove(int callId) { if (_feeds.TryRemove(callId, out var f)) f.Reset(); }

    /// <summary>Engine.VideoFrameReceived — 영상 스레드.</summary>
    public void OnFrame(Engine sender, in VideoFrame f)
    {
        if (_feeds.TryGetValue(f.CallId, out var feed)) feed.Push(f);
    }
}
