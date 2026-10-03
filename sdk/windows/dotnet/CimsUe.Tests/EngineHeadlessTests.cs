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
        Assert.True(SpinWait.SpinUntil(() => Volatile.Read(ref logs) > 0, 2000));    // 로그는 엔진 스레드에서 비동기로 온다

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
        Assert.False(e.GetCall(7).Quality.Valid);
        Assert.Equal(-1, e.GetCall(7).Quality.MosCq);
        // 서버 인증서 만료 관측(§8.6.2) — TLS 핸드셰이크가 없었으면 관측 없음(Valid=false·None)
        Assert.Same(TlsPeerExpiry.None, e.TlsPeerExpiry);
        Assert.Equal(0, e.TlsPeerExpiry.DaysLeft);
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

    /// <summary>W01 — MC 서비스 인가·송출 확인·규격형 구독 명령이 코어 결과를 그대로 돌려주고, 계정 칸이 왕복한다(c_api_test ServiceAuthAndCallOptionFields).</summary>
    [Fact]
    public void ServiceAuthAndAccountFields()
    {
        using var e = Inline();
        Assert.True(e.Start(new EngineConfig { LogLevel = 1, NullAudioDevice = true }).Ok);
        var cfg = CompleteAccount();
        cfg.McpttEnabled = true; cfg.McdataFd = true; cfg.AccessToken = "tok"; cfg.McpttServerUri = "sip:mcptt_psi@ptt.example.org";
        cfg.TcTimers = new McVideoTcTimers(T100Ms: 3000); cfg.ConfirmQueuedTransmission = true;
        var acc = e.AddAccount(cfg);
        Assert.True(acc.Ok, acc.Reason);
        var a = acc.Value;

        // 등록 전 — 보내지 않았다(Unauthorized, Code 0)
        var auth = a.ServiceAuth(McService.Mcptt);
        Assert.Equal(ServiceAuthState.Unauthorized, auth.State);
        Assert.Equal(a.Id, auth.AccountId);
        Assert.Equal(McService.Mcptt, auth.Service);
        Assert.Equal(0, auth.Code);
        Assert.True(a.SetAccessToken("tok2").Ok);
        Assert.Equal(-2, e.GetAccount(99).SetAccessToken("t").Code);        // 없는 계정 — 코어 결과 그대로
        Assert.True(a.SetTcTimers(new McVideoTcTimers(T100Ms: 2000)).Ok);
        Assert.Equal("no such account", e.GetAccount(99).SetMcVideoEnabled(true).Reason);
        Assert.Equal(-2, e.GetAccount(99).SubscribeXcapDiff("sip:gms@ptt", new[] { "doc" }, "tok", true).Code);
        Assert.Equal(-2, e.GetCall(7).ConfirmTransmission(true).Code);       // 없는 호
        Assert.Equal(-2, e.GetCall(7).RequestQueuePosition().Code);
        Assert.False(e.GetCall(7).TransmissionInfo.AwaitingConfirmation);

        // 계정 칸 왕복 — 코어 헬퍼가 같은 구조체를 본다
        Assert.True(cfg.IsComplete());
        e.Stop();
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

    /// <summary>Windows 엔진 = 영상 빌드(F3) — 프레임 렌더 장치·웹캠(DirectShow)·합성 캡처가 열거되고, 셀프뷰·카메라 선택이 받힌다.
    /// 창 렌더(SetVideoWindow)는 받지 않는다(프레임이 VideoFrameReceived 로 온다). 실제 프레임은 cimsue_test McvCall.VideoTransmitSelfViewAndReceiveFrames.</summary>
    [Fact]
    public void VideoEngineIsFrameSink()
    {
        using var e = Inline();
        Assert.True(e.Start(new EngineConfig { LogLevel = 1, NullAudioDevice = true }).Ok);
        var devs = e.VideoDevices;
        Assert.Contains(devs, d => d.Render && d.Driver == "CIMS");
        var cbar = Assert.Single(devs, d => d.Capture && d.Driver == "Colorbar" && d.Name == "Colorbar generator");
        Assert.False(cbar.IsCamera);
        Assert.True(e.SetVideoCaptureDevice(cbar.Id).Ok);
        Assert.False(e.SetVideoCaptureDevice(9999).Ok);
        Assert.True(e.SetVideoPreview(true).Ok);
        Assert.True(e.SetVideoPreview(false).Ok);
        Assert.False(e.SetVideoWindow(IntPtr.Zero).Ok);
        e.Stop();
        Assert.False(e.SetVideoPreview(true).Ok);    // 미기동
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
