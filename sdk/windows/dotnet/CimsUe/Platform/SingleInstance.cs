// CimsUe.Platform — 단일 인스턴스 (명명 Mutex + 활성화 이벤트)
//
// 둘째 실행은 첫 인스턴스에 "활성화" 신호만 보내고 끝난다(IsFirst=false). 첫 인스턴스는 신호를 받아 ActivationRequested 를
// (생성 시점 컨텍스트로) 올린다 — 앱이 창을 앞으로 가져온다.
namespace CimsUe.Platform;

public sealed class SingleInstance : IDisposable
{
    private readonly Mutex _mutex;
    private readonly EventWaitHandle _activate;
    private readonly ManualResetEvent _stop = new(false);
    private readonly SynchronizationContext? _sync;
    private readonly Thread? _waiter;

    /// <summary>이 프로세스가 첫 인스턴스인가.</summary>
    public bool IsFirst { get; }
    public event EventHandler? ActivationRequested;

    /// <param name="name">인스턴스 이름(사용자 세션 범위 — Local\ 접두).</param>
    public SingleInstance(string name, SynchronizationContext? context = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        _sync = context ?? SynchronizationContext.Current;
        _mutex = new Mutex(true, @"Local\" + name, out bool createdNew);
        _activate = new EventWaitHandle(false, EventResetMode.AutoReset, @"Local\" + name + ".activate");
        IsFirst = createdNew;
        if (!createdNew)
        {
            _activate.Set();                 // 기존 인스턴스에 창 활성화 요청
            return;
        }
        _waiter = new Thread(Wait) { IsBackground = true, Name = "CimsUe.SingleInstance" };
        _waiter.Start();
    }

    private void Wait()
    {
        var handles = new WaitHandle[] { _activate, _stop };
        while (WaitHandle.WaitAny(handles) == 0)
        {
            if (_sync is not null) _sync.Post(_ => ActivationRequested?.Invoke(this, EventArgs.Empty), null);
            else ActivationRequested?.Invoke(this, EventArgs.Empty);
        }
    }

    public void Dispose()
    {
        _stop.Set();
        if (IsFirst) { try { _mutex.ReleaseMutex(); } catch (ApplicationException) { } }
        _mutex.Dispose();
        _activate.Dispose();
        _stop.Dispose();
    }
}
