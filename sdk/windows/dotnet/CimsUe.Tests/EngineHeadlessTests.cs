// 엔진 수명·명령 결과·콜백 전달 — 헤드리스(null 장치) 기동. c_api_test.cpp 의 EngineLifecycleHeadless 와 같은 판정을 파사드로.
using Xunit;

namespace CimsUe.Tests;

public class EngineHeadlessTests
{
    private static AccountConfig CompleteAccount() => new()
    {
        ServerHost = "127.0.0.1", ServerPort = 65000, Domain = "ims.example.org",
        Msisdn = "+821300000001", Imsi = "45033821300000001", Ha1 = "0123456789abcdef0123456789abcdef",
    };

    /// <summary>이벤트를 코어 이벤트 스레드에서 직접 받는 엔진 — xunit 은 SynchronizationContext.Current 를 두므로 명시적으로 비운다.</summary>
    private static Engine Inline()
    {
        SynchronizationContext.SetSynchronizationContext(null);
        return new Engine(eventContext: null);
    }

    /// <summary>Post 를 모아 두고 Drain 으로 실행하는 컨텍스트 — 이벤트가 앱 스레드로 넘어오는 경로를 시험한다.</summary>
    private sealed class QueueContext : SynchronizationContext
    {
        private readonly System.Collections.Concurrent.ConcurrentQueue<(SendOrPostCallback, object?)> _q = new();
        public int Posted;
        public override void Post(SendOrPostCallback d, object? state) { Interlocked.Increment(ref Posted); _q.Enqueue((d, state)); }
        public void Drain() { while (_q.TryDequeue(out var item)) item.Item1(item.Item2); }
    }

    [Fact]
    public void LifecycleAndSnapshots()
    {
        using var e = Inline();
        Assert.False(e.IsRunning);

        // 미기동 상태 명령 — C++ Result::fail(-1, "not running") 이 코드·사유로 그대로 온다
        var r = e.GetCall(0).Hangup();
        Assert.False(r.Ok);
        Assert.Equal(-1, r.Code);
        Assert.Equal("not running", r.Reason);
        var dial = e.GetAccount(0).Dial("1000");
        Assert.False(dial.Ok);
        Assert.Null(dial.Value);

        int logs = 0, stopped = 0;
        e.Log += (_, _) => Interlocked.Increment(ref logs);
        e.Stopped += (_, _) => Interlocked.Increment(ref stopped);

        var st = e.Start(new EngineConfig { LogLevel = 3, NullAudioDevice = true });
        Assert.True(st.Ok, st.Reason);
        Assert.True(e.IsRunning);
        Assert.False(e.Start(new EngineConfig { NullAudioDevice = true }).Ok);      // already running
        Assert.True(logs > 0);

        // 계정 — 미완성 설정은 실패, 완성 설정은 id 발급 + 조회 스냅샷
        Assert.False(e.AddAccount(new AccountConfig()).Ok);
        var acc = e.AddAccount(CompleteAccount());
        Assert.True(acc.Ok, acc.Reason);
        Assert.Contains(acc.Value, e.Accounts);
        Assert.Same(acc.Value, e.GetAccount(acc.Value.Id));
        var ri = acc.Value.RegInfo;
        Assert.Equal(acc.Value.Id, ri.AccountId);
        Assert.Equal(RegState.Unregistered, ri.State);
        Assert.Equal(-1, e.GetAccount(99).RegInfo.AccountId);                       // 없는 계정 → 기본 RegInfo

        // 호 조회 — 없는 호는 기본 CallInfo(-1), 배열은 빈 목록
        var ci = e.GetCall(7).Info;
        Assert.Equal(-1, ci.CallId);
        Assert.Empty(ci.Sources);
        Assert.Equal("", ci.RemoteUri);
        Assert.Empty(e.Calls);
        Assert.Equal(FloorState.Idle, e.GetCall(7).FloorInfo.State);
        Assert.False(e.GetCall(7).StreamStats.Valid);
        var route = e.GetCall(0).SetRoute(99);
        Assert.False(route.Ok);
        Assert.Equal("no such route", route.Reason);

        // 장치 — 헤드리스는 null 장치 하나(또는 0개)
        foreach (var d in e.AudioDevices) Assert.NotNull(d.Name);
        Assert.True(e.RefreshAudioDevices().Ok);

        Assert.True(acc.Value.Remove().Ok);
        e.Stop();
        Assert.False(e.IsRunning);
        Assert.Equal(1, stopped);
    }

    [Fact]
    public void EventsAreMarshalledThroughSynchronizationContext()
    {
        var ctx = new QueueContext();
        using var e = new Engine(ctx);
        int stoppedOnDrain = 0;
        e.Stopped += (_, _) => stoppedOnDrain++;
        Assert.True(e.Start(new EngineConfig { LogLevel = 1, NullAudioDevice = true }).Ok);
        e.Stop();
        Assert.Equal(0, stoppedOnDrain);            // 아직 컨텍스트 큐에만 있다
        Assert.True(ctx.Posted > 0);
        ctx.Drain();
        Assert.Equal(1, stoppedOnDrain);
    }

    [Fact]
    public void DisposeWhileRunningIsSafe()
    {
        var e = Inline();
        Assert.True(e.Start(new EngineConfig { LogLevel = 1, NullAudioDevice = true }).Ok);
        e.Dispose();
        Assert.False(e.IsRunning);
        e.Dispose();                                 // 재-Dispose 무해
        Assert.Throws<ObjectDisposedException>(() => e.Calls);
    }
}
