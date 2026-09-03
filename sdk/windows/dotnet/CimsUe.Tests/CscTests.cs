// CSC 평면·설정 헬퍼 — 프로파일 평탄화(중첩 배열·dispatch)·to_account·인라인 멤버 규칙이 코어와 같은 답을 내는지(네트워크 없이).
using Xunit;

namespace CimsUe.Tests;

public class CscTests
{
    private const string ProfileJson = """
    {
      "user": { "displayName": "테스트001", "loginId": "test001" },
      "csc": { "host": "121.161.164.48", "port": 4430 },
      "countryCode": "82",
      "services": [
        { "kind": "volte",
          "sip": { "host": "121.161.164.48", "port": 5060, "transport": "UDP",
                   "transports": [ { "transport": "UDP", "port": 5060 }, { "transport": "TLS", "port": 5061 } ],
                   "default": "UDP", "domain": "ims.example.org", "mediaSecurity": "optional", "security": ["tls"] },
          "account": { "msisdn": "+821300000001", "imsi": "45033821300000001", "sipHa1": "0123456789abcdef0123456789abcdef" } },
        { "kind": "ptt",
          "sip": { "host": "121.161.164.48", "port": 5061, "transport": "TLS", "transports": [ { "transport": "TLS", "port": 5061 } ],
                   "default": "TLS", "enforced": true, "domain": "ptt.example.org" },
          "account": { "msisdn": "+82500000001", "imsi": "4503382500000001", "mcpttId": "tel:+82500000001",
                       "authScheme": "aka", "aka": { "k": "00112233", "opc": "44556677", "amf": "8000" } } }
      ],
      "dispatch": { "groupId": "dg-1", "groupName": "관제1", "pilotId": "+8215001000", "monitorScope": "all", "pttListen": "listed", "listenVisibility": "hidden" }
    }
    """;

    [Fact]
    public void ParseProfileFlattensNestedArraysAndDispatch()
    {
        var r = CscClient.ParseProfile(ProfileJson);
        Assert.True(r.Ok, r.Reason);
        var p = r.Value;
        Assert.Equal("테스트001", p.DisplayName);
        Assert.Equal(4430, p.CscPort);
        Assert.Equal(2, p.Services.Count);
        var v = p.Service("volte");
        Assert.NotNull(v);
        Assert.Equal(5060, v!.SipPort);
        Assert.Equal(2, v.Transports.Count);
        Assert.Equal(Transport.Tls, v.Transports[1].Transport);
        Assert.Equal(MediaSecurity.Optional, v.MediaSecurity);
        Assert.Equal(new[] { "tls" }, v.SecMechanisms);
        Assert.Null(p.Service("video"));
        Assert.True(p.Dispatch.Present);
        Assert.Equal("dg-1", p.Dispatch.GroupId);
        Assert.Equal("all", p.Dispatch.MonitorScope);
        Assert.Equal("hidden", p.Dispatch.ListenVisibility);

        var a = v.ToAccountConfig();
        Assert.Equal("45033821300000001@ims.example.org", a.DigestUsername());
        Assert.Equal("0123456789abcdef0123456789abcdef", a.Ha1);
        Assert.Equal(new[] { "tls" }, a.SecMechanisms);
        Assert.True(a.IsComplete());
        Assert.Equal("sip:+821300000001@ims.example.org", a.Aor());

        var t = p.Service("ptt")!;
        Assert.Equal(AuthScheme.Aka, t.AuthScheme);
        var ta = t.ToAccountConfig();
        Assert.Equal("00112233", ta.AkaK);
        Assert.Equal("tel:+82500000001", ta.EffectiveMcpttId());
        Assert.True(ta.IsComplete());                    // AKA K 로 완성
    }

    [Fact]
    public void ParseProfileFailureCarriesReason()
    {
        var r = CscClient.ParseProfile("{not json");
        Assert.False(r.Ok);
        Assert.False(string.IsNullOrEmpty(r.Reason));
        Assert.Null(r.Value);
    }

    [Fact]
    public void AccountConfigHelpersFollowCore()
    {
        var c = new AccountConfig
        {
            ServerHost = "csp.example.org", Domain = "ims.example.org", Msisdn = "+821300000001",
            Imsi = "45033821300000001", Ha1 = "0123456789abcdef0123456789abcdef",
        };
        Assert.Equal("sip:+821300000001@ims.example.org", c.Aor());
        Assert.Equal("45033821300000001@ims.example.org", c.DigestUsername());
        Assert.Equal("tel:+821300000001", c.EffectiveMcpttId());   // 비면 tel:+msisdn
        Assert.True(c.IsComplete());
        c.Ha1 = ""; c.Password = null;                              // 빈 문자열은 지운다 → 자격 없음
        Assert.False(c.IsComplete());
        c.AuthId = "impi@ims.example.org";
        Assert.Equal("impi@ims.example.org", c.DigestUsername());
    }

    [Fact]
    public void DialogJoinHeaderFollowsCore()
    {
        var d = new DialogInfo(0, "sip:1003@d", "d1", "abc@host", "L1", "R1", "initiator", "confirmed", "sip:1004@d", true);
        Assert.Equal("abc@host;to-tag=R1;from-tag=L1", d.JoinHeader());
    }

    [Fact]
    public void CscHandleAndEncode()
    {
        using var c = new CscClient(new CscEndpoint { Host = "127.0.0.1" });
        Assert.Equal(4430, c.Endpoint.Port);
        Assert.Equal("tel%3A%2B82%201", CscClient.Encode("tel:+82 1"));
    }
}
