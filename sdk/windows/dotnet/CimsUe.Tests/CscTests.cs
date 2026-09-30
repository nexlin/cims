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
      "dispatch": { "groupId": "dg-1", "groupName": "관제1", "pilotId": "+8215001000", "monitorScope": "all", "pttListen": "listed", "listenVisibility": "hidden",
                    "directoryAdmin": "own", "orgCode": "DIV1" }
    }
    """;

    [Fact]
    public void PhoneServicePrefersVoipOverVolte()
    {
        // 유선 voip 회선과 이동 volte 회선이 함께 오면 관제 앱의 전화 계정은 voip(android_ue_provisioning.md §3)
        string json = ProfileJson.Replace("\"services\": [", "\"services\": [ { \"kind\": \"voip\", \"sip\": { \"host\": \"10.0.0.1\", \"port\": 5060, \"transport\": \"TLS\", \"domain\": \"voip.example.org\" }, \"account\": { \"msisdn\": \"+821310001001\", \"imsi\": \"45033821310001001\" } },");
        var r = CscClient.ParseProfile(json);
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(3, r.Value.Services.Count);
        Assert.Equal("voip", r.Value.PhoneService!.Kind);
        Assert.Equal("voip.example.org", r.Value.PhoneService!.Domain);
        Assert.Equal("volte", r.Value.Service("volte")!.Kind);
    }

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
        Assert.Same(v, p.PhoneService);                       // voip 없음 → volte 폴백
        Assert.True(p.Dispatch.Present);
        Assert.Equal("dg-1", p.Dispatch.GroupId);
        Assert.Equal("all", p.Dispatch.MonitorScope);
        Assert.Equal("hidden", p.Dispatch.ListenVisibility);
        Assert.Equal("own", p.Dispatch.DirectoryAdmin);
        Assert.Equal("DIV1", p.Dispatch.OrgCode);
        Assert.True(p.Dispatch.CanAdminDirectory);

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
    public void ParseProfileDispatchDiscoveryAndGroupCreation()
    {
        var r = CscClient.ParseProfile("""
        { "services": [], "ptt": { "allowCreateGroup": true },
          "dispatch": { "groupId": "dg-1", "monitorScope": "listed", "pttListen": "listed",
            "members": [ { "userId": 12, "name": "관제2석", "volteAor": "tel:+821310001002", "pttId": "sip:+82510001002@ptt.example.org", "extension": "1002", "groupId": "dg-1" } ],
            "pttTargets": [ { "id": "g002", "uri": "sip:g002@ptt.example.org", "name": "음성그룹2" } ] } }
        """);
        Assert.True(r.Ok, r.Reason);
        Assert.True(r.Value!.AllowGroupCreation);
        var m = Assert.Single(r.Value.Dispatch.Members);
        Assert.Equal("1002", m.Extension); Assert.Equal("관제2석", m.Name); Assert.Equal("tel:+821310001002", m.VolteAor); Assert.Equal("dg-1", m.GroupId);
        var t = Assert.Single(r.Value.Dispatch.PttTargets);
        Assert.Equal("g002", t.Id); Assert.Equal("음성그룹2", t.Name);
        var old = CscClient.ParseProfile(ProfileJson).Value!;          // 확장 없는 응답 = 빈 목록·false
        Assert.Empty(old.Dispatch.Members); Assert.Empty(old.Dispatch.PttTargets); Assert.False(old.AllowGroupCreation);
    }

    [Fact]
    public void GroupDocRoundTripsThroughCore()
    {
        var doc = new GroupDoc
        {
            Uri = "sip:g-1234abcd@ptt.example.org", DisplayName = "순찰 & 지원", SessionType = "chat", VideoEnabled = true,
            MaxParticipants = 16, RequireAffiliation = false, EmergencyCall = false, OrgCode = "ORG1",
            Members = { new GroupMember { Uri = "tel:+82510001001", Name = "관제1석", Role = "chair", Priority = 7 },
                        new GroupMember { Uri = "tel:+82510001002" } },
        };
        string xml = doc.ToXml();
        Assert.Contains("<list-service uri=\"sip:g-1234abcd@ptt.example.org\">", xml);
        Assert.Contains("순찰 &amp; 지원", xml);
        // 그룹 종류는 규격 요소로만(TS 24.481 §7.2.2 a — chat = false) — session-type 은 싣지 않는다
        Assert.Contains("<mcpttgi:on-network-invite-members>false</mcpttgi:on-network-invite-members>", xml);
        Assert.DoesNotContain("session-type", xml);
        var back = GroupDoc.Parse(xml);
        Assert.True(back.Ok, back.Reason);
        var b = back.Value!;
        Assert.Equal(doc.Uri, b.Uri); Assert.Equal("순찰 & 지원", b.DisplayName); Assert.Equal("chat", b.SessionType);
        Assert.True(b.VideoEnabled); Assert.Equal(16, b.MaxParticipants); Assert.False(b.RequireAffiliation); Assert.False(b.EmergencyCall);
        Assert.True(b.EmergencyAlert); Assert.Equal("ORG1", b.OrgCode);
        Assert.Equal(2, b.Members.Count);
        Assert.Equal("chair", b.Members[0].Role); Assert.Equal(7, b.Members[0].Priority); Assert.Equal("관제1석", b.Members[0].Name);
        Assert.Equal("participant", b.Members[1].Role); Assert.Equal("", b.Members[1].Name);
        Assert.False(GroupDoc.Parse("<other/>").Ok);
    }

    /// <summary>
    /// 그룹 호 타이머·참가자 정보·MCData 크기 한도 — <b>null = 미기재</b>면 싣지 않는다. 폼이 이 칸을 다루지 않고
    /// 새 문서를 지어 저장해도 콘솔이 정한 값을 덮지 않아야 한다. 0 은 값이다(미사용·무제한).
    /// </summary>
    [Fact]
    public void GroupDocCallTimersAreOptional()
    {
        var fresh = new GroupDoc { Uri = "sip:g1@ptt.example.org", DisplayName = "g1" };
        string fx = fresh.ToXml();
        foreach (var tag in new[] { "on-network-hang-timer", "on-network-maximum-duration", "on-network-allow-conference-state",
                                    "max-data-size-for-SDS", "max-data-size-auto-recv" })
            Assert.DoesNotContain(tag, fx);
        var none = GroupDoc.Parse(fx).Value!;
        Assert.Null(none.HangTimerSec); Assert.Null(none.MaxDurationSec); Assert.Null(none.AllowConferenceState);
        Assert.Null(none.MaxSdsSize); Assert.Null(none.MaxAutoRecv);

        var set = new GroupDoc
        {
            Uri = "sip:g2@ptt.example.org", DisplayName = "g2",
            HangTimerSec = 0, MaxDurationSec = 3600, AllowConferenceState = false, MaxSdsSize = 1000, MaxAutoRecv = 0,
            Members = { new GroupMember { Uri = "tel:+82510001001", Role = "chair", Priority = 9 },
                        new GroupMember { Uri = "tel:+82510001002", Priority = 2 } },
        };
        string sx = set.ToXml();
        Assert.Contains("<mcpttgi:on-network-hang-timer>PT0S</mcpttgi:on-network-hang-timer>", sx);
        var b = GroupDoc.Parse(sx).Value!;
        Assert.Equal(0, b.HangTimerSec); Assert.Equal(3600, b.MaxDurationSec); Assert.False(b.AllowConferenceState);
        Assert.Equal(1000, b.MaxSdsSize); Assert.Equal(0, b.MaxAutoRecv);
        // 멤버별 우선순위는 코어가 보존한다 — 앱이 폼으로 새로 지을 때 버리지 않아야 한다
        Assert.Equal(9, b.Members[0].Priority); Assert.Equal(2, b.Members[1].Priority);
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
        // 서버 인증서 만료 관측(§8.6.2) — 요청 전엔 관측 없음
        Assert.Same(TlsPeerExpiry.None, c.TlsPeerExpiry);
    }

    [Fact]
    public void FdGuardsRunBeforeNetwork()
    {
        // FD 경로가 아닌 URL 은 요청하지 않는다(Bearer 누설 방지) · 빈 파일은 올리지 않는다 — 둘 다 코어가 -2 로 끊는다
        using var c = new CscClient(new CscEndpoint { Host = "127.0.0.1", Port = 1 });
        var d = c.DownloadFd("tok", "https://127.0.0.1/provisioning/me");
        Assert.False(d.Ok); Assert.Equal(-2, d.Code);
        var u = c.UploadFd("tok", Array.Empty<byte>(), "a.txt", null, null);
        Assert.False(u.Ok); Assert.Equal(-2, u.Code);
    }

    [Fact]
    public void ProfileCapabilitiesAndMcDataLimitReachAccount()
    {
        // 서버 산출 모양 — capabilities.smsGateway · sip.udpNoTcpSwitch · PTT 의 mcdata.maxPayloadSdsCplaneBytes → AccountConfig.MaxSdsCplaneBytes
        var r = CscClient.ParseProfile("""
        { "user": { "loginId": "d1" }, "services": [
          { "kind": "voip", "capabilities": { "smsGateway": true },
            "sip": { "host": "h", "port": 5061, "transport": "TLS", "domain": "ims.example.org", "udpNoTcpSwitch": true },
            "account": { "msisdn": "+821310001001", "imsi": "1", "sipHa1": "0123456789abcdef0123456789abcdef" } },
          { "kind": "ptt", "sip": { "host": "h", "port": 5060, "domain": "ptt.example.org" },
            "account": { "msisdn": "+82500000001", "imsi": "2", "sipHa1": "0123456789abcdef0123456789abcdef" },
            "mcdata": { "maxPayloadSdsCplaneBytes": 1500 } } ] }
        """);
        Assert.True(r.Ok, r.Reason);
        var voip = r.Value.Service("voip")!;
        var ptt = r.Value.Service("ptt")!;
        Assert.True(voip.SmsGateway);
        Assert.True(voip.UdpNoTcpSwitch);
        Assert.False(ptt.SmsGateway);
        Assert.Equal(1500, ptt.MaxPayloadSdsCplaneBytes);
        var a = ptt.ToAccountConfig();
        Assert.Equal(1500, a.MaxSdsCplaneBytes);                  // 넘는 그룹 SDS 는 media plane(MSRP)
        Assert.Equal("mcpttp.15", a.RpEmergency);                 // 코어 기본값이 to_account 로 온다
        a.McdataMsrp = true;
        Assert.True(a.IsComplete());
    }

    [Fact]
    public void CmsDocsAndCapabilitiesFollowCore()
    {
        var up = UserProfileDoc.Parse("""
            <mcptt-user-profile XUI-URI="tel:+82500000001"><ruleset><actions>
            <allow-cancel-emergency-alert>false</allow-cancel-emergency-alert>
            <allow-cancel-group-emergency>false</allow-cancel-group-emergency></actions></ruleset>
            <Common><MCPTT-group-call><EmergencyAlert><entry entry-info="DedicatedGroup"><uri-entry>sip:g002@ptt</uri-entry></entry></EmergencyAlert></MCPTT-group-call></Common>
            <OnNetwork><MCPTTGroupInfo><entry><uri-entry>sip:g1@ptt</uri-entry></entry></MCPTTGroupInfo></OnNetwork></mcptt-user-profile>
            """);
        Assert.True(up.Ok, up.Reason);
        Assert.Equal("tel:+82500000001", up.Value.UserUri);
        Assert.Equal(new[] { "sip:g1@ptt" }, up.Value.Groups);
        Assert.Equal("sip:g002@ptt", up.Value.EmergencyAlertGroup.Uri);
        Assert.Equal("DedicatedGroup", up.Value.EmergencyAlertGroup.Mode);
        Assert.False(up.Value.AllowCancelEmergencyAlert);
        Assert.True(up.Value.AllowActivateEmergencyAlert);          // 요소 없음 = 허용
        Assert.False(up.Value.AllowCancelGroupEmergency);
        Assert.True(up.Value.AllowCancelImminentPeril);
        var k = Capabilities.Of(up.Value, null);
        Assert.True(k.UserProfileKnown);
        Assert.False(k.ServiceConfigKnown);
        Assert.False(k.CancelEmergencyAlert);
        Assert.True(k.EmergencyAlert);
        Assert.False(k.CancelGroupEmergency);                       // 앱은 «내가 올린 조건» 과 OR (TS 24.379 §6.3.3.1.13.4)
        Assert.True(k.CancelImminentPeril);
        var none = Capabilities.Of(null, null);
        Assert.False(none.UserProfileKnown);
        Assert.True(none.CancelEmergencyAlert);                     // 못 받은 문서는 허용
        Assert.False(UserProfileDoc.Parse("<group/>").Ok);

        var sc = ServiceConfigDoc.Parse("""
            <service-configuration-info><service-configuration-params domain="ptt.example.org"><on-network>
            <emergency-resource-priority><resource-priority-namespace>mcpttp</resource-priority-namespace><resource-priority-priority>14</resource-priority-priority></emergency-resource-priority>
            </on-network></service-configuration-params></service-configuration-info>
            """);
        Assert.True(sc.Ok, sc.Reason);
        Assert.Equal("ptt.example.org", sc.Value.Domain);
        Assert.Equal("mcpttp.14", sc.Value.RpEmergency);
        Assert.Equal("", sc.Value.RpNormal);
        Assert.True(Capabilities.Of(up.Value, sc.Value).ServiceConfigKnown);

        var ui = UeInitConfigDoc.Parse("""
            <mcptt-UE-initial-configuration domain="ptt.example.org"><on-network><anyExt>
            <MCPTT-Service-Details><Server-URI>sip:mcptt_psi@ptt.example.org</Server-URI></MCPTT-Service-Details>
            <MCData-Service-Details><Server-URI>sip:mcdata_psi@ptt.example.org</Server-URI></MCData-Service-Details>
            </anyExt></on-network></mcptt-UE-initial-configuration>
            """);
        Assert.True(ui.Ok, ui.Reason);
        Assert.Equal("sip:mcptt_psi@ptt.example.org", ui.Value.McpttServerUri);
        Assert.Equal("sip:mcdata_psi@ptt.example.org", ui.Value.McdataServerUri);
        Assert.False(UeInitConfigDoc.Parse("<mcptt-user-profile/>").Ok);
        Assert.False(string.IsNullOrEmpty(Engine.ToText(ConditionCause.Denied)));
    }
}
