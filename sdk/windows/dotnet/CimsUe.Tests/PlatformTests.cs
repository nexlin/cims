// Windows 접점 — 핫키 표기·자격 저장 왕복·엔드포인트 열거·이름 대응. 레지스트리(AutoStart)는 시험에서 쓰지 않는다.
using CimsUe.Platform;
using Xunit;

namespace CimsUe.Tests;

public class PlatformTests
{
    [Theory]
    [InlineData("Ctrl+Space", HotKeyModifiers.Control, 0x20)]
    [InlineData("F9", HotKeyModifiers.None, 0x78)]
    [InlineData("ctrl+shift+1", HotKeyModifiers.Control | HotKeyModifiers.Shift, '1')]
    [InlineData("Alt+F12", HotKeyModifiers.Alt, 0x7B)]
    [InlineData("Win+NumPad5", HotKeyModifiers.Win, 0x65)]
    public void HotKeyParseAndRoundTrip(string text, HotKeyModifiers mods, int vk)
    {
        Assert.True(HotKey.TryParse(text, out var k));
        Assert.Equal(mods, k.Modifiers);
        Assert.Equal(vk, k.VirtualKey);
        Assert.True(HotKey.TryParse(k.ToString(), out var again));
        Assert.Equal(k, again);
    }

    [Theory]
    [InlineData("")]
    [InlineData("Ctrl+")]
    [InlineData("Ctrl+Space+F1")]
    [InlineData("Foo")]
    public void HotKeyRejectsBadText(string text) => Assert.False(HotKey.TryParse(text, out _));

    [Fact]
    public void CredentialStoreRoundTrip()
    {
        string root = Path.Combine(Path.GetTempPath(), "cimsue-tests-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new CredentialStore("dispatch-desktop", root);
            Assert.Null(store.Load("refresh"));
            store.Save("refresh", "tok-한글-🔒");
            Assert.Equal("tok-한글-🔒", store.Load("refresh"));
            Assert.NotEqual("tok-한글-🔒", File.ReadAllText(Path.Combine(store.Directory, "refresh.bin")));   // 평문이 아니다
            store.Delete("refresh");
            Assert.Null(store.Load("refresh"));
            store.Delete("refresh");                                    // 없는 키 삭제 무해
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }

    [Fact]
    public void AudioEndpointsEnumerate()
    {
        using var ep = new AudioEndpoints(context: null);
        var render = ep.List(AudioFlow.Render);                       // 장치가 없는 머신이면 빈 목록
        foreach (var d in render) { Assert.False(string.IsNullOrEmpty(d.Id)); Assert.Equal(AudioFlow.Render, d.Flow); }
        Assert.True(render.Count(d => d.IsDefault) <= 1);
        _ = ep.Default(AudioFlow.Capture);
    }

    [Fact]
    public void MatchEngineDeviceUsesWmmeTruncatedPrefix()
    {
        var devices = new[]
        {
            new AudioDeviceInfo(0, "Speakers (Realtek(R) Audio)", "WMME", 0, 2),
            new AudioDeviceInfo(1, "Microphone Array (Realtek(R) Au", "WMME", 2, 0),  // 31자 절단(MAXPNAMELEN 32)
            new AudioDeviceInfo(2, "Headset Earphone (Jabra Evolve2", "WMME", 0, 2),
        };
        Assert.Equal(0, AudioEndpoints.MatchEngineDevice(devices, "Speakers (Realtek(R) Audio)", AudioFlow.Render)!.Id);
        Assert.Equal(1, AudioEndpoints.MatchEngineDevice(devices, "Microphone Array (Realtek(R) Audio)", AudioFlow.Capture)!.Id);
        Assert.Equal(2, AudioEndpoints.MatchEngineDevice(devices, "Headset Earphone (Jabra Evolve2 65)", AudioFlow.Render)!.Id);
        Assert.Null(AudioEndpoints.MatchEngineDevice(devices, "Speakers (Realtek(R) Audio)", AudioFlow.Capture));   // 방향 불일치
        Assert.Null(AudioEndpoints.MatchEngineDevice(devices, "Unknown", AudioFlow.Render));
    }

    [Fact]
    public void SingleInstanceSecondIsNotFirst()
    {
        string name = "CimsUe.Tests." + Guid.NewGuid().ToString("N");
        using var first = new SingleInstance(name, context: null);
        Assert.True(first.IsFirst);
        var activated = new ManualResetEventSlim(false);
        first.ActivationRequested += (_, _) => activated.Set();
        using var second = new SingleInstance(name, context: null);
        Assert.False(second.IsFirst);
        Assert.True(activated.Wait(TimeSpan.FromSeconds(5)));
    }
}
