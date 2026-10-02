// CimsUe — CSC 설정 평면 클라이언트 (cimsue/csc.h 1:1). IdMS PKCE 로그인 · /provisioning/me · GMS/CMS XCAP.
//
// Engine 과 독립·동기 호출(HTTP 가 끝날 때까지 블록 — 앱은 Task.Run/…Async 로 감싼다). 한 핸들의 산출은 다음 호출 전까지만
// 유효하므로 호출을 핸들 단위로 직렬화하고 받는 즉시 관리 객체로 복사한다.
using CimsUe.Native;
using static CimsUe.Native.NativeMethods;

namespace CimsUe;

public sealed class CscEndpoint
{
    public string Host { get; set; } = "";
    public int Port { get; set; } = 4430;
    /// <summary>null = 코어 기본 "MCPTT_UE".</summary>
    public string? ClientId { get; set; }
    public string? RedirectUri { get; set; }
    public string? Scope { get; set; }
    /// <summary>신뢰 앵커(PEM). null = 시스템 기본.</summary>
    public string? CaPem { get; set; }
    public bool VerifyServer { get; set; } = true;
}

public sealed record TokenSet(string AccessToken, string TokenType, string RefreshToken, string IdToken, string Scope, int ExpiresInSec);

public sealed record ServiceEndpoint(Transport Transport, int Port);

/// <summary>프로비저닝 프로파일의 서비스 1개(kind = volte | voip | ptt). UdpNoTcpSwitch = sip.udpNoTcpSwitch(엔진 전역 — 앱이 고른다),
/// SmsGateway = capabilities.smsGateway(외부망 SMS/LMS 게이트웨이 연결 — 외부 번호 [문자] 활성 조건). MaxPayloadSdsCplaneBytes = mcdata 블록(TS 24.484).</summary>
public sealed record ServiceProfile(
    string Kind, string SipHost, int SipPort, Transport Transport, IReadOnlyList<ServiceEndpoint> Transports, bool Enforced,
    MediaSecurity MediaSecurity, string Domain, string Msisdn, string Imsi, string AuthId, string SipHa1, string McpttId,
    AuthScheme AuthScheme, string AkaK, string AkaOpc, string AkaAmf, IReadOnlyList<string> SecMechanisms, int MaxPayloadSdsCplaneBytes,
    bool UdpNoTcpSwitch = false, bool SmsGateway = false)
{
    /// <summary>이 서비스로 등록할 계정 설정 — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). 규칙은 코어(toAccount).</summary>
    public AccountConfig ToAccountConfig(string? loginPw = null) => CscClient.ToAccountConfig(this, loginPw);
}

/// <summary>관제 그룹원(dispatch members[]) — dialog 구독·그룹원 띠 대상.</summary>
/// <summary>관제 그룹원(dispatch members[]) — GroupId = 그 가입자의 관제 그룹(무소속 ""). 그룹원 띠 = GroupId == Dispatch.GroupId, dialog 감시 = 전원.</summary>
public sealed record DispatchMember(string UserId, string Name, string VolteAor, string PttId, string Extension, string GroupId);
/// <summary>청취 대상 PTT 그룹(dispatch pttTargets[] — 서버가 ptt_listen 범위를 해석한 결과).</summary>
public sealed record DispatchTarget(string Id, string Uri, string Name);

/// <summary>관제 데스크(dispatch_center.md §8.4) — 없으면 Present=false. MonitorScope·PttListen = none|own|listed|all, ListenVisibility = hidden|visible,
/// DirectoryAdmin = none|own|all(관제 앱 조직/구성원/번호·PTT 그룹 관리 범위 — own 의 루트 = OrgCode). Members/PttTargets 는 서버가 주지 않으면 빈 목록.</summary>
public sealed record DispatchProfile(bool Present, string GroupId, string GroupName, string PilotId,
                                     string MonitorScope, string PttListen, string ListenVisibility,
                                     string DirectoryAdmin, string OrgCode,
                                     IReadOnlyList<DispatchMember> Members, IReadOnlyList<DispatchTarget> PttTargets)
{
    public static DispatchProfile None { get; } = new(false, "", "", "", "none", "none", "hidden", "none", "", Array.Empty<DispatchMember>(), Array.Empty<DispatchTarget>());
    /// <summary>관리 범위가 있는가 — 관제 앱 [관리] 창 노출.</summary>
    public bool CanAdminDirectory => DirectoryAdmin is "own" or "all";
}

public sealed record Profile(string DisplayName, string LoginId, string CountryCode, string CscHost, int CscPort,
                             IReadOnlyList<ServiceProfile> Services, DispatchProfile Dispatch, bool AllowGroupCreation)
{
    /// <summary>kind 로 서비스 찾기 — 없으면 null.</summary>
    public ServiceProfile? Service(string kind) => Services.FirstOrDefault(s => s.Kind == kind);
    /// <summary>전화 회선 — 유선 "voip" 우선, 없으면 이동 "volte"(android_ue_provisioning.md §3). 관제 앱의 전화 계정 선택 규칙.</summary>
    public ServiceProfile? PhoneService => Service("voip") ?? Service("volte");
}

/// <summary>GMS 목록 항목. IsOwner = 토큰 주체가 authorized user(편집·삭제 가능).</summary>
public sealed record GroupSummary(string Uri, string DisplayName, string ETag, int MemberCount, bool IsOwner);
public sealed record XcapDoc(string Body, string ETag, bool NotModified);
/// <summary>범용 요청 산출(csc.h HttpResult) — Status = HTTP 상태(0 전송 실패), Body 는 바이트 그대로(이진 가능). NotModified = 304.</summary>
public sealed record HttpResponse(int Status, string ContentType, string ETag, byte[] Body)
{
    public bool NotModified => Status == 304;
    /// <summary>본문을 UTF-8 문자열로(JSON·텍스트 응답).</summary>
    public string Text => System.Text.Encoding.UTF8.GetString(Body);
}
/// <summary>MCData FD 업로드 결과(csc.h FdUpload) — Url 을 FdFile.Url 로 넘겨 Account.SendGroupFd/SendFd 로 알린다.</summary>
public sealed record FdUpload(string Id, string Url, string Name, long Size);

/// <summary>그룹 문서 멤버 — Role = chair | participant.</summary>
public sealed class GroupMember
{
    public string Uri { get; set; } = "";
    public string Name { get; set; } = "";
    public string Role { get; set; } = "participant";
    public int Priority { get; set; } = 5;
    /// <summary>필수 멤버 &lt;on-network-required&gt;(TS 24.481 §7.2.4.2) — 읽은 값을 되돌려야 콘솔 설정이 남는다.</summary>
    public bool Required { get; set; }
    /// <summary>직함 &lt;cims:user-title&gt;(사이트 확장) — 읽기 전용(PUT 에 싣지 않는다).</summary>
    public string Title { get; set; } = "";
    /// <summary>MCVideo entry 의 MCVideo ID &lt;mcvideo-mcvideo-id&gt;(TS 24.481 §7.2.2) — 빈 값 = Uri 와 같다(MCVideo ID = MCPTT ID).</summary>
    public string McvideoId { get; set; } = "";
}

/// <summary>그룹 문서의 MCVideo 몫(csc.h McVideoGroupAttrs — TS 24.481 §7.2.2·§7.2.8). <see cref="GroupDoc.Mcvideo"/> 가 null 이면 MCVideo 그룹이
/// 아니고 PUT 에도 싣지 않는다(서버는 MCVideo &lt;service&gt; 가 없는 PUT 으로 그 그룹의 MCVideo 설정을 바꾸지 않는다). null 속성 = 미기재.</summary>
public sealed class McVideoGroupAttrs
{
    /// <summary>mcvideo-on-network-invite-members — true = prearranged, false = chat(TS 24.281 §6.3.5.2 호 종류 검사).</summary>
    public bool InviteMembers { get; set; }
    public int? MaxDurationSec { get; set; }
    /// <summary>mcvideo-protect-media — 요소가 없으면 true(GMK 보호). CIMS 는 false 를 명시한다.</summary>
    public bool ProtectMedia { get; set; } = true;
    public bool ProtectTransmissionControl { get; set; } = true;
    public List<string> AudioEncodings { get; set; } = new();
    public List<string> VideoEncodings { get; set; } = new();
    public string VideoResolutions { get; set; } = "";
    public string VideoFrameRate { get; set; } = "";
    public bool? UrgentRealTimeVideoMode { get; set; }
    public bool? NonUrgentRealTimeVideoMode { get; set; }
    public bool? NonRealTimeVideoMode { get; set; }
    public string ActiveRealTimeVideoMode { get; set; } = "";
    /// <summary>mcvideo-maximum-simultaneous-mcvideo-transmitting-group-members — 동시 송출 상한.</summary>
    public int? MaxTransmitters { get; set; }
    public int? MinNumberToStart { get; set; }
    public int? GroupPriority { get; set; }
    /// <summary>on-network-reception-hang-timer (T5, TS 24.581 §11.1.3).</summary>
    public int? ReceptionHangTimerSec { get; set; }
    public bool? AllowConferenceState { get; set; }
    public bool? AllowEmergencyCall { get; set; }
    public bool? AllowEmergencyAlert { get; set; }
    public bool? AllowImminentPerilCall { get; set; }
}

/// <summary>CMS 대상 항목(TS 24.484 §8.3.2.7 EntryType) — Mode = entry-info(DedicatedGroup | UseCurrentlySelectedGroup | UsePreConfigured | LocallyDetermined).</summary>
public sealed record CmsEntry(string Uri, string Mode);

/// <summary>MCPTT user profile(TS 24.484 §8.3.2, csc.h UserProfileDoc) — 코어가 해석하는 요소만. 인가 Allow* 는 요소가 없으면 허용이다 —
/// 서버가 최종 판정하므로 앱은 UX 선차단만 한다. NotModified = 304(나머지는 비어 있다 — 가진 사본을 유지). MaxAffiliationsN2 -1 = 미기재.</summary>
public sealed record UserProfileDoc(
    string ETag, bool NotModified, string UserUri, CmsEntry EmergencyGroup, CmsEntry ImminentPerilGroup, CmsEntry EmergencyAlertGroup,
    CmsEntry EmergencyPrivateRecipient, IReadOnlyList<string> Groups, IReadOnlyList<string> ImplicitAffiliations, int MaxAffiliationsN2,
    bool AllowPrivateCall, bool AllowEmergencyGroupCall, bool AllowImminentPerilCall, bool AllowActivateEmergencyAlert,
    bool AllowCancelEmergencyAlert, bool AllowEmergencyPrivateCall, bool AllowAdhocGroupCall,
    bool AllowCancelGroupEmergency = true, bool AllowCancelImminentPeril = true)
{
    /// <summary>XML → 문서(코어 파서). 루트가 mcptt-user-profile 이 아니면 실패.</summary>
    public static Result<UserProfileDoc> Parse(string xml) => CscClient.ParseUserProfile(xml);
}

/// <summary>MCPTT service configuration(TS 24.484 §8.4, csc.h ServiceConfigDoc) — 인가 요소는 없다. Rp* = Resource-Priority r-value(빈 값 = 미기재 —
/// 계정 기본값 유지). 요소가 없으면 빈 값/-1.</summary>
public sealed record ServiceConfigDoc(string ETag, bool NotModified, string Domain, int NumLevelsGroupHierarchy, int NumLevelsUserHierarchy,
                                      string RpEmergency, string RpImminentPeril, string RpNormal)
{
    public static Result<ServiceConfigDoc> Parse(string xml) => CscClient.ParseServiceConfig(xml);
}

/// <summary>MCVideo user profile(TS 24.484 §9.3, csc.h McVideoUserProfileDoc) — 문서가 있으면 MCVideo 이용 자격이 있다(Fetch 404 = 자격 없음).
/// Groups = MCVideo 로 affiliate 할 수 있는 그룹. 인가 Allow* 는 요소가 없으면 허용. 정수 -1 = 미기재.</summary>
public sealed record McVideoUserProfileDoc(
    string ETag, bool NotModified, string UserUri, string McvideoId, IReadOnlyList<string> Groups, IReadOnlyList<string> ImplicitAffiliations,
    int MaxAffiliationsN2, int MaxSimultaneousVideoStreams, int MaxSimultaneousCallsN6, CmsEntry EmergencyGroup, CmsEntry ImminentPerilGroup,
    CmsEntry EmergencyAlertGroup, bool AllowPrivateCall, bool AllowEmergencyGroupCall, bool AllowEmergencyPrivateCall, bool AllowImminentPerilCall,
    bool AllowActivateEmergencyAlert, bool AllowRevokeTransmit, bool AllowRemoteAmbientViewing, bool AllowLocalAmbientViewing,
    bool AllowAdhocGroupCall)
{
    public static Result<McVideoUserProfileDoc> Parse(string xml) => CscClient.ParseMcVideoUserProfile(xml);
}

/// <summary>MCVideo service configuration(TS 24.484 §9.4, csc.h McVideoServiceConfigDoc) — 참여자 전송 제어 타이머 T100~T104(초, -1 = 미기재 → 코어 기본값)·
/// RP·신호 보호(요소가 없으면 켜짐 — TS 24.281 §6.6.2.1).</summary>
public sealed record McVideoServiceConfigDoc(string ETag, bool NotModified, string Domain, string RpEmergency, string RpImminentPeril, string RpNormal,
                                             bool ConfidentialityProtection, bool IntegrityProtection,
                                             int T100Sec, int T101Sec, int T102Sec, int T103Sec, int T104Sec)
{
    public static Result<McVideoServiceConfigDoc> Parse(string xml) => CscClient.ParseMcVideoServiceConfig(xml);
}

/// <summary>MCS UE initial configuration(TS 24.484 §7.2, csc.h UeInitConfigDoc) — 참여 기능 PSI(`&lt;anyExt&gt;` 의 *-Service-Details/Server-URI).
/// 광고하지 않은 서비스는 빈 값 — 계정의 해당 PSI 도 비워 둔다(<see cref="AccountConfig.McpttServerUri"/>·<see cref="AccountConfig.McdataServerUri"/>).</summary>
public sealed record UeInitConfigDoc(string ETag, bool NotModified, string Domain, string McpttServerUri, string McdataServerUri,
                                     string McvideoServerUri = "", FloorTimers? FloorTimers = null)
{
    public static Result<UeInitConfigDoc> Parse(string xml) => CscClient.ParseUeInitConfig(xml);
}

/// <summary>정책 게이트 스냅샷(csc.h Capabilities — ue_sdk.md §4.2) — user profile ruleset 인가. 받지 못한 문서는 허용(게이트 없음).
/// UX 선차단(버튼 비활성·안내)용이고 최종 판정은 서버다. MaxAffiliationsN2 0 = 미지정.</summary>
public sealed record Capabilities(bool UserProfileKnown, bool ServiceConfigKnown, bool PrivateCall, bool EmergencyGroupCall, bool ImminentPerilCall,
                                  bool EmergencyPrivateCall, bool EmergencyAlert, bool CancelEmergencyAlert, bool AdhocGroupCall, int MaxAffiliationsN2,
                                  bool CancelGroupEmergency = true, bool CancelImminentPeril = true)
{
    // CancelGroupEmergency = allow-cancel-group-emergency — 그룹 긴급 해제는 local policy(TS 24.379 §6.2.8.1.7), 서버 판정 = 개시자 ∨ 이 값
    //   (§6.3.3.1.13.4) → 앱은 «내가 올린 조건(McpttCondition.Mine)» 과 OR 해서 [긴급 해제] 를 연다. CancelImminentPeril 은 개시자 예외 없음(§6.2.8.1.10).
    /// <summary>규칙은 코어 한 곳(Capabilities::of). null = 그 문서를 아직 못 받음.</summary>
    public static Capabilities Of(UserProfileDoc? userProfile, ServiceConfigDoc? serviceConfig) => CscClient.CapabilitiesOf(userProfile, serviceConfig);
}

/// <summary>GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) — GET 산출·PUT 입력 공용 모델(cimsue/csc.h GroupDoc 1:1). 편집 폼이 그대로 쓰도록 가변.</summary>
public sealed class GroupDoc
{
    public string Uri { get; set; } = "";
    public string DisplayName { get; set; } = "";
    /// <summary>서버 산출(GET/PUT 응답 ETag). 수정 PUT 의 If-Match 로 쓴다.</summary>
    public string ETag { get; set; } = "";
    public List<GroupMember> Members { get; set; } = new();
    /// <summary>그룹 종류 prearranged | chat (문서의 on-network-invite-members)</summary>
    public string SessionType { get; set; } = "prearranged";
    public bool Encryption { get; set; }
    public bool EmergencyCall { get; set; } = true;
    public bool EmergencyAlert { get; set; } = true;
    public bool AllowSds { get; set; } = true;
    public bool AllowFd { get; set; }
    public bool RequireAffiliation { get; set; } = true;
    public int Priority { get; set; } = 5;
    /// <summary>0 = 미기재.</summary>
    public int MaxParticipants { get; set; }
    public string OrgCode { get; set; } = "";
    /// <summary>서버 산출 — 그룹 소유자(authorized user).</summary>
    public string AuthorizedUser { get; set; } = "";

    // 그룹 호 타이머·참가자 정보·MCData 크기 한도(TS 24.481) — **null = 미기재**. PUT 에 싣지 않아 서버가 기존값을
    //   유지한다. 폼이 이 칸을 다루지 않은 채 새 문서를 지어 저장해도 콘솔이 정한 값을 덮지 않게 하려는 것이다.
    //   0 은 값이다(hang 0 = 미사용, 크기 0 = 무제한).
    /// <summary>on-network-hang-timer (T4, 초) — 0 = 미사용. null = 미기재.</summary>
    public int? HangTimerSec { get; set; }
    /// <summary>on-network-maximum-duration (TNG3, 초) — 0 = 무제한. null = 미기재.</summary>
    public int? MaxDurationSec { get; set; }
    /// <summary>on-network-allow-conference-state — 멤버의 참가자 정보(conference 이벤트) 구독 허용. null = 미기재.</summary>
    public bool? AllowConferenceState { get; set; }
    /// <summary>mcdata-on-network-max-data-size-for-SDS (octet) — 0 = 무제한. null = 미기재.</summary>
    public int? MaxSdsSize { get; set; }
    /// <summary>mcdata-on-network-max-data-size-auto-recv (octet) — 0 = 무제한. null = 미기재.</summary>
    public int? MaxAutoRecv { get; set; }
    /// <summary>on-network-minimum-number-to-start — 개시자 200 OK 전 멤버 200 수(0 = 기다리지 않음). null = 미기재.</summary>
    public int? MinNumberToStart { get; set; }
    /// <summary>on-network-timeout-for-acknowledgement-of-required-members (TNG1, 초). null = 미기재.</summary>
    public int? AckTimeoutSec { get; set; }
    /// <summary>TNG1 만료 동작 proceed | abandon. null = 미기재.</summary>
    public string? AckAction { get; set; }
    /// <summary>MCVideo 몫 — null = MCVideo 그룹 아님(PUT 에 싣지 않는다). 서비스 집합 = MCPTT + MCVideo(TS 23.280 §3).</summary>
    public McVideoGroupAttrs? Mcvideo { get; set; }

    /// <summary>문서 → XML(PUT 본문) — 직렬화 규칙은 코어.</summary>
    public string ToXml() => CscClient.GroupDocToXml(this);
    /// <summary>XML → 문서(코어 파서). 실패면 Reason.</summary>
    public static Result<GroupDoc> Parse(string xml) => CscClient.ParseGroupDoc(xml);
}

public sealed unsafe class CscClient : IDisposable
{
    private IntPtr _h;
    private readonly object _gate = new();
    public CscEndpoint Endpoint { get; }

    public CscClient(CscEndpoint endpoint)
    {
        ArgumentNullException.ThrowIfNull(endpoint);
        Endpoint = endpoint;
        using var s = new NativeStrings();
        cimsue_csc_endpoint_t n;
        cimsue_csc_endpoint_default(&n);
        n.host = s.Add(endpoint.Host);
        n.port = endpoint.Port;
        n.client_id = s.Add(endpoint.ClientId);
        n.redirect_uri = s.Add(endpoint.RedirectUri);
        n.scope = s.Add(endpoint.Scope);
        n.ca_pem = s.Add(endpoint.CaPem);
        n.verify_server = Engine.B(endpoint.VerifyServer);
        _h = cimsue_csc_create(&n);
        if (_h == IntPtr.Zero) throw new CimsUeException("cimsue_csc_create 실패");
    }

    private IntPtr Handle => _h != IntPtr.Zero ? _h : throw new ObjectDisposedException(nameof(CscClient));

    public void Dispose()
    {
        IntPtr h = Interlocked.Exchange(ref _h, IntPtr.Zero);
        if (h != IntPtr.Zero) cimsue_csc_destroy(h);
    }

    /// <summary>HTTPS 서버 인증서 만료 관측 — 마지막 성공 TLS 요청에서 본 CSC 인증서(§8.6.2). 요청 전엔 Valid=false.</summary>
    public TlsPeerExpiry TlsPeerExpiry
    {
        get
        {
            lock (_gate)
            {
                cimsue_tls_peer_expiry_t t;
                cimsue_csc_tls_peer_expiry(Handle, &t);
                return Engine.ToManaged(&t);
            }
        }
    }

    /// <summary>IdMS PKCE(S256) 로그인 → 토큰.</summary>
    public Result<TokenSet> Login(string userName, string password)
    {
        lock (_gate)
        {
            cimsue_token_set_t t;
            int st = cimsue_csc_login(Handle, userName, password, &t);
            return st == 0 ? Result<TokenSet>.Success(ToManaged(&t)) : Result<TokenSet>.Fail(st, Engine.LastError());
        }
    }

    public Result<TokenSet> Refresh(string refreshToken)
    {
        lock (_gate)
        {
            cimsue_token_set_t t;
            int st = cimsue_csc_refresh(Handle, refreshToken, &t);
            return st == 0 ? Result<TokenSet>.Success(ToManaged(&t)) : Result<TokenSet>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>GET /provisioning/me</summary>
    public Result<Profile> FetchProfile(string accessToken)
    {
        lock (_gate)
        {
            cimsue_profile_t p;
            int st = cimsue_csc_fetch_profile(Handle, accessToken, &p);
            return st == 0 ? Result<Profile>.Success(ToManaged(&p)) : Result<Profile>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>GMS 그룹 목록(userUri 예 tel:+8250…).</summary>
    public Result<IReadOnlyList<GroupSummary>> ListGroups(string accessToken, string userUri)
    {
        lock (_gate)
        {
            cimsue_group_summary_t* g;
            int n = cimsue_csc_list_groups(Handle, accessToken, userUri, &g);
            if (n < 0) return Result<IReadOnlyList<GroupSummary>>.Fail(-1, Engine.LastError());
            var list = new GroupSummary[n];
            for (int i = 0; i < n; ++i)
                list[i] = new GroupSummary(Utf8.Str(g[i].uri), Utf8.Str(g[i].display_name), Utf8.Str(g[i].etag), g[i].member_count, g[i].is_owner != 0);
            return Result<IReadOnlyList<GroupSummary>>.Success(list);
        }
    }

    // ── GMS 그룹 관리(TS 24.481 XCAP PUT/DELETE — authorized user = 토큰 주체, PKCE 토큰) ──
    /// <summary>그룹 문서 GET(ETag 포함). userUri = 자기 XCAP 트리(mcptt_id).</summary>
    public Result<GroupDoc> GetGroup(string accessToken, string userUri, string groupUri)
    {
        lock (_gate)
        {
            cimsue_group_doc_t d;
            int st = cimsue_csc_get_group(Handle, accessToken, userUri, groupUri, &d);
            return st == 0 ? Result<GroupDoc>.Success(ToManaged(&d)) : Result<GroupDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>그룹 생성(신규 Uri)/수정(기존 Uri) — ifMatch 로 조건부(412 = 충돌). 성공 시 서버가 확정한 문서(ETag·AuthorizedUser).
    /// 실패 Code = HTTP(403 자격/소유, 409 타인 소유, 412).</summary>
    public Result<GroupDoc> PutGroup(string accessToken, string userUri, GroupDoc doc, string? ifMatch = null)
    {
        ArgumentNullException.ThrowIfNull(doc);
        lock (_gate)
        {
            using var s = new NativeStrings();
            cimsue_group_doc_t n = ToNative(doc, s);
            cimsue_group_doc_t d;
            int st = cimsue_csc_put_group(Handle, accessToken, userUri, &n, ifMatch, &d);
            return st == 0 ? Result<GroupDoc>.Success(ToManaged(&d)) : Result<GroupDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>그룹 삭제 — 본인 소유만(403).</summary>
    public Result DeleteGroup(string accessToken, string userUri, string groupUri)
    {
        lock (_gate) return Engine.Status(cimsue_csc_delete_group(Handle, accessToken, userUri, groupUri));
    }

    public Task<Result<GroupDoc>> GetGroupAsync(string accessToken, string userUri, string groupUri, CancellationToken ct = default) =>
        Task.Run(() => GetGroup(accessToken, userUri, groupUri), ct);
    public Task<Result<GroupDoc>> PutGroupAsync(string accessToken, string userUri, GroupDoc doc, string? ifMatch = null, CancellationToken ct = default) =>
        Task.Run(() => PutGroup(accessToken, userUri, doc, ifMatch), ct);
    public Task<Result> DeleteGroupAsync(string accessToken, string userUri, string groupUri, CancellationToken ct = default) =>
        Task.Run(() => DeleteGroup(accessToken, userUri, groupUri), ct);

    internal static string GroupDocToXml(GroupDoc doc)
    {
        using var s = new NativeStrings();
        var n = (cimsue_group_doc_t*)s.Alloc(sizeof(cimsue_group_doc_t));   // 람다가 지역 주소를 못 잡으므로 임시 버퍼에 둔다
        *n = ToNative(doc, s);
        return Utf8.Call((buf, cap) => cimsue_group_doc_to_xml(n, buf, cap));
    }

    internal static Result<GroupDoc> ParseGroupDoc(string xml)
    {
        cimsue_group_doc_t d;
        int st = cimsue_group_doc_parse(xml, &d);
        return st == 0 ? Result<GroupDoc>.Success(ToManaged(&d)) : Result<GroupDoc>.Fail(st, Engine.LastError());
    }

    /// <summary>범용 요청(Bearer) — 코어가 모델링하지 않은 CSC 엔드포인트(관제 관리 API·녹취·이력 창 조회). 2xx·304 = Ok(Value.Status 로 구분),
    /// 그 밖의 HTTP 상태 = Fail(Code=상태)이되 Value 는 채워진다(오류 JSON 본문을 읽을 수 있게), 전송 실패 = -1.</summary>
    public Result<HttpResponse> Request(string accessToken, string method, string path, string? contentType = null, byte[]? body = null,
                                        string? accept = null, string? ifMatch = null, string? ifNoneMatch = null)
    {
        lock (_gate)
        {
            cimsue_http_result_t r;
            int st;
            fixed (byte* b = body)
                st = cimsue_csc_request(Handle, accessToken, method, path, contentType, b, body?.Length ?? 0, accept, ifMatch, ifNoneMatch, &r);
            var v = new HttpResponse(r.status, Utf8.Str(r.content_type), Utf8.Str(r.etag),
                                     r.body != null && r.body_len > 0 ? new ReadOnlySpan<byte>(r.body, r.body_len).ToArray() : Array.Empty<byte>());
            return st == 0 ? Result<HttpResponse>.Success(v) : new Result<HttpResponse>(st, Engine.LastError(), v);
        }
    }

    /// <summary>MCData FD 업로드(octet-stream, TS 24.282 §10.2). groupId 가 있으면 그룹 FD — 서버가 allow_fd·업로더 멤버십으로 게이트(403, 없는 그룹 404),
    /// null 이면 1:1. 413 = 서버 상한 초과. 실패 Code = HTTP 상태(전송 실패 -1).</summary>
    public Result<FdUpload> UploadFd(string accessToken, byte[] data, string name, string? mime, string? groupId)
    {
        ArgumentNullException.ThrowIfNull(data);
        lock (_gate)
        {
            cimsue_fd_upload_t u;
            int st;
            fixed (byte* d = data)
                st = cimsue_csc_upload_fd(Handle, accessToken, d, data.Length, name, mime, groupId, &u);
            return st == 0 ? Result<FdUpload>.Success(new FdUpload(Utf8.Str(u.id), Utf8.Str(u.url), Utf8.Str(u.name), u.size))
                           : Result<FdUpload>.Fail(st, Engine.LastError());
        }
    }
    public Task<Result<FdUpload>> UploadFdAsync(string accessToken, byte[] data, string name, string? mime, string? groupId, CancellationToken ct = default) =>
        Task.Run(() => UploadFd(accessToken, data, name, mime, groupId), ct);

    /// <summary>MCData FD 다운로드 — url = 받은 FILEURL. 경로(/mcdata/fd/{id})만 취해 이 CSC 에 요청한다(Bearer 를 다른 호스트로 보내지 않음).
    /// FD 경로가 아니면 Fail(-2). Value.Body = 파일 바이트. 실패 규약은 Request 와 같다.</summary>
    public Result<HttpResponse> DownloadFd(string accessToken, string url)
    {
        lock (_gate)
        {
            cimsue_http_result_t r;
            int st = cimsue_csc_download_fd(Handle, accessToken, url, &r);
            var v = new HttpResponse(r.status, Utf8.Str(r.content_type), Utf8.Str(r.etag),
                                     r.body != null && r.body_len > 0 ? new ReadOnlySpan<byte>(r.body, r.body_len).ToArray() : Array.Empty<byte>());
            return st == 0 ? Result<HttpResponse>.Success(v) : new Result<HttpResponse>(st, Engine.LastError(), v);
        }
    }
    public Task<Result<HttpResponse>> DownloadFdAsync(string accessToken, string url, CancellationToken ct = default) =>
        Task.Run(() => DownloadFd(accessToken, url), ct);

    /// <summary>JSON 본문 편의 — body 문자열은 UTF-8 로, Accept/Content-Type = application/json.</summary>
    public Result<HttpResponse> RequestJson(string accessToken, string method, string path, string? json = null, string? ifMatch = null, string? ifNoneMatch = null) =>
        Request(accessToken, method, path, "application/json", json is null ? null : System.Text.Encoding.UTF8.GetBytes(json),
                "application/json", ifMatch, ifNoneMatch);

    public Task<Result<HttpResponse>> RequestAsync(string accessToken, string method, string path, string? contentType = null, byte[]? body = null,
                                                   string? accept = null, string? ifMatch = null, string? ifNoneMatch = null, CancellationToken ct = default) =>
        Task.Run(() => Request(accessToken, method, path, contentType, body, accept, ifMatch, ifNoneMatch), ct);
    public Task<Result<HttpResponse>> RequestJsonAsync(string accessToken, string method, string path, string? json = null, string? ifMatch = null,
                                                       string? ifNoneMatch = null, CancellationToken ct = default) =>
        Task.Run(() => RequestJson(accessToken, method, path, json, ifMatch, ifNoneMatch), ct);

    /// <summary>XCAP GET — ifNoneMatch 로 304 캐시(NotModified).</summary>
    public Result<XcapDoc> XcapGet(string accessToken, string path, string accept, string? ifNoneMatch = null)
    {
        lock (_gate)
        {
            cimsue_xcap_doc_t d;
            int st = cimsue_csc_xcap_get(Handle, accessToken, path, accept, ifNoneMatch, &d);
            return st == 0 ? Result<XcapDoc>.Success(ToManaged(&d)) : Result<XcapDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>CMS user-profile 문서(TS 24.484).</summary>
    public Result<XcapDoc> GetUserProfile(string accessToken, string userUri, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_xcap_doc_t d;
            int st = cimsue_csc_get_user_profile(Handle, accessToken, userUri, etag, &d);
            return st == 0 ? Result<XcapDoc>.Success(ToManaged(&d)) : Result<XcapDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>CMS service-config 문서(TS 24.484).</summary>
    public Result<XcapDoc> GetServiceConfig(string accessToken, string userUri, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_xcap_doc_t d;
            int st = cimsue_csc_get_service_config(Handle, accessToken, userUri, etag, &d);
            return st == 0 ? Result<XcapDoc>.Success(ToManaged(&d)) : Result<XcapDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>CMS user-profile GET + 해석(If-None-Match = etag). 304 면 NotModified=true(나머지 비어 있음 — 가진 사본 유지). 해석 실패 = -2.</summary>
    public Result<UserProfileDoc> FetchUserProfile(string accessToken, string userUri, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_user_profile_doc_t d;
            int st = cimsue_csc_fetch_user_profile(Handle, accessToken, userUri, etag, &d);
            return st == 0 ? Result<UserProfileDoc>.Success(ToManaged(&d)) : Result<UserProfileDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>CMS service-config GET + 해석 — FetchUserProfile 과 같은 규약.</summary>
    public Result<ServiceConfigDoc> FetchServiceConfig(string accessToken, string userUri, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_service_config_doc_t d;
            int st = cimsue_csc_fetch_service_config(Handle, accessToken, userUri, etag, &d);
            return st == 0 ? Result<ServiceConfigDoc>.Success(ToManaged(&d)) : Result<ServiceConfigDoc>.Fail(st, Engine.LastError());
        }
    }
    /// <summary>UE initial configuration GET + 해석(TS 24.484 §7.2.1.1) — mcsUeId = 단말 instance ID(urn:uuid:…). 로그인 전 문서라 토큰 없이.
    /// NotModified·해석 실패 규약은 FetchUserProfile 과 같다.</summary>
    public Result<UeInitConfigDoc> FetchUeInitConfig(string mcsUeId, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_ue_init_config_doc_t d;
            int st = cimsue_csc_fetch_ue_init_config(Handle, mcsUeId, etag, &d);
            return st == 0 ? Result<UeInitConfigDoc>.Success(ToManaged(&d)) : Result<UeInitConfigDoc>.Fail(st, Engine.LastError());
        }
    }
    public Task<Result<UeInitConfigDoc>> FetchUeInitConfigAsync(string mcsUeId, string? etag = null, CancellationToken ct = default) =>
        Task.Run(() => FetchUeInitConfig(mcsUeId, etag), ct);
    public Task<Result<UserProfileDoc>> FetchUserProfileAsync(string accessToken, string userUri, string? etag = null, CancellationToken ct = default) =>
        Task.Run(() => FetchUserProfile(accessToken, userUri, etag), ct);
    public Task<Result<ServiceConfigDoc>> FetchServiceConfigAsync(string accessToken, string userUri, string? etag = null, CancellationToken ct = default) =>
        Task.Run(() => FetchServiceConfig(accessToken, userUri, etag), ct);

    /// <summary>MCVideo user profile GET + 해석(TS 24.484 §9.3) — mcvideoId = MCPTT ID 와 같은 값. 404 = MCVideo 이용 자격 없음.
    /// NotModified·해석 실패 규약은 FetchUserProfile 과 같다.</summary>
    public Result<McVideoUserProfileDoc> FetchMcVideoUserProfile(string accessToken, string mcvideoId, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_mcvideo_user_profile_doc_t d;
            int st = cimsue_csc_fetch_mcvideo_user_profile(Handle, accessToken, mcvideoId, etag, &d);
            return st == 0 ? Result<McVideoUserProfileDoc>.Success(ToManaged(&d)) : Result<McVideoUserProfileDoc>.Fail(st, Engine.LastError());
        }
    }

    /// <summary>MCVideo service configuration GET + 해석(TS 24.484 §9.4 — 전역 문서).</summary>
    public Result<McVideoServiceConfigDoc> FetchMcVideoServiceConfig(string accessToken, string? etag = null)
    {
        lock (_gate)
        {
            cimsue_mcvideo_service_config_doc_t d;
            int st = cimsue_csc_fetch_mcvideo_service_config(Handle, accessToken, etag, &d);
            return st == 0 ? Result<McVideoServiceConfigDoc>.Success(ToManaged(&d)) : Result<McVideoServiceConfigDoc>.Fail(st, Engine.LastError());
        }
    }
    public Task<Result<McVideoUserProfileDoc>> FetchMcVideoUserProfileAsync(string accessToken, string mcvideoId, string? etag = null, CancellationToken ct = default) =>
        Task.Run(() => FetchMcVideoUserProfile(accessToken, mcvideoId, etag), ct);
    public Task<Result<McVideoServiceConfigDoc>> FetchMcVideoServiceConfigAsync(string accessToken, string? etag = null, CancellationToken ct = default) =>
        Task.Run(() => FetchMcVideoServiceConfig(accessToken, etag), ct);

    internal static Result<McVideoUserProfileDoc> ParseMcVideoUserProfile(string xml)
    {
        cimsue_mcvideo_user_profile_doc_t d;
        int st = cimsue_mcvideo_user_profile_parse(xml, &d);
        return st == 0 ? Result<McVideoUserProfileDoc>.Success(ToManaged(&d)) : Result<McVideoUserProfileDoc>.Fail(st, Engine.LastError());
    }

    internal static Result<McVideoServiceConfigDoc> ParseMcVideoServiceConfig(string xml)
    {
        cimsue_mcvideo_service_config_doc_t d;
        int st = cimsue_mcvideo_service_config_parse(xml, &d);
        return st == 0 ? Result<McVideoServiceConfigDoc>.Success(ToManaged(&d)) : Result<McVideoServiceConfigDoc>.Fail(st, Engine.LastError());
    }

    internal static Result<UeInitConfigDoc> ParseUeInitConfig(string xml)
    {
        cimsue_ue_init_config_doc_t d;
        int st = cimsue_ue_init_config_parse(xml, &d);
        return st == 0 ? Result<UeInitConfigDoc>.Success(ToManaged(&d)) : Result<UeInitConfigDoc>.Fail(st, Engine.LastError());
    }

    internal static Result<UserProfileDoc> ParseUserProfile(string xml)
    {
        cimsue_user_profile_doc_t d;
        int st = cimsue_user_profile_parse(xml, &d);
        return st == 0 ? Result<UserProfileDoc>.Success(ToManaged(&d)) : Result<UserProfileDoc>.Fail(st, Engine.LastError());
    }

    internal static Result<ServiceConfigDoc> ParseServiceConfig(string xml)
    {
        cimsue_service_config_doc_t d;
        int st = cimsue_service_config_parse(xml, &d);
        return st == 0 ? Result<ServiceConfigDoc>.Success(ToManaged(&d)) : Result<ServiceConfigDoc>.Fail(st, Engine.LastError());
    }

    internal static Capabilities CapabilitiesOf(UserProfileDoc? up, ServiceConfigDoc? sc)
    {
        using var s = new NativeStrings();
        cimsue_user_profile_doc_t u = default;
        cimsue_service_config_doc_t c = default;
        if (up is not null)
        {
            u.max_affiliations_n2 = up.MaxAffiliationsN2;
            u.allow_private_call = Engine.B(up.AllowPrivateCall); u.allow_emergency_group_call = Engine.B(up.AllowEmergencyGroupCall);
            u.allow_imminent_peril_call = Engine.B(up.AllowImminentPerilCall); u.allow_activate_emergency_alert = Engine.B(up.AllowActivateEmergencyAlert);
            u.allow_cancel_emergency_alert = Engine.B(up.AllowCancelEmergencyAlert); u.allow_emergency_private_call = Engine.B(up.AllowEmergencyPrivateCall);
            u.allow_adhoc_group_call = Engine.B(up.AllowAdhocGroupCall);
            u.allow_cancel_group_emergency = Engine.B(up.AllowCancelGroupEmergency); u.allow_cancel_imminent_peril = Engine.B(up.AllowCancelImminentPeril);
        }
        if (sc is not null)
        {
            c.domain = s.Add(sc.Domain); c.num_levels_group_hierarchy = sc.NumLevelsGroupHierarchy; c.num_levels_user_hierarchy = sc.NumLevelsUserHierarchy;
            c.rp_emergency = s.Add(sc.RpEmergency); c.rp_imminent_peril = s.Add(sc.RpImminentPeril); c.rp_normal = s.Add(sc.RpNormal);
        }
        cimsue_capabilities_t k;
        cimsue_capabilities_of(up is null ? null : &u, sc is null ? null : &c, &k);
        return new Capabilities(k.user_profile_known != 0, k.service_config_known != 0, k.private_call != 0, k.emergency_group_call != 0,
                                k.imminent_peril_call != 0, k.emergency_private_call != 0, k.emergency_alert != 0, k.cancel_emergency_alert != 0,
                                k.adhoc_group_call != 0, k.max_affiliations_n2, k.cancel_group_emergency != 0, k.cancel_imminent_peril != 0);
    }

    // 비동기 편의 — 블록 호출을 스레드 풀로.
    public Task<Result<TokenSet>> LoginAsync(string userName, string password, CancellationToken ct = default) =>
        Task.Run(() => Login(userName, password), ct);
    public Task<Result<TokenSet>> RefreshAsync(string refreshToken, CancellationToken ct = default) =>
        Task.Run(() => Refresh(refreshToken), ct);
    public Task<Result<Profile>> FetchProfileAsync(string accessToken, CancellationToken ct = default) =>
        Task.Run(() => FetchProfile(accessToken), ct);
    public Task<Result<IReadOnlyList<GroupSummary>>> ListGroupsAsync(string accessToken, string userUri, CancellationToken ct = default) =>
        Task.Run(() => ListGroups(accessToken, userUri), ct);

    /// <summary>/provisioning/me 응답 JSON → Profile (시험·캐시 복원용).</summary>
    public static Result<Profile> ParseProfile(string json)
    {
        cimsue_profile_t p;
        int st = cimsue_csc_parse_profile(json, &p);
        return st == 0 ? Result<Profile>.Success(ToManaged(&p)) : Result<Profile>.Fail(st, Engine.LastError());
    }

    /// <summary>XCAP 경로용 percent-encoding(코어 규칙).</summary>
    public static string Encode(string s) => Utf8.Call((buf, cap) => cimsue_csc_enc(s, buf, cap));

    internal static AccountConfig ToAccountConfig(ServiceProfile sp, string? loginPw)
    {
        using var s = new NativeStrings();
        cimsue_service_profile_t n = ToNative(sp, s);
        cimsue_account_config_t a;
        cimsue_service_profile_to_account(&n, loginPw, &a);
        return Engine.FromNative(&a);
    }

    // ── 변환 ──

    private static TokenSet ToManaged(cimsue_token_set_t* t) =>
        new(Utf8.Str(t->access_token), Utf8.Str(t->token_type), Utf8.Str(t->refresh_token), Utf8.Str(t->id_token),
            Utf8.Str(t->scope), t->expires_in_sec);

    private static XcapDoc ToManaged(cimsue_xcap_doc_t* d) => new(Utf8.Str(d->body), Utf8.Str(d->etag), d->not_modified != 0);

    private static string[] StrArray(byte** v, int n)
    {
        var a = new string[Math.Max(0, n)];
        for (int i = 0; i < a.Length; ++i) a[i] = Utf8.Str(v[i]);
        return a;
    }

    private static CmsEntry ToManaged(in cimsue_cms_entry_t e) => new(Utf8.Str(e.uri), Utf8.Str(e.mode));

    private static UserProfileDoc ToManaged(cimsue_user_profile_doc_t* d) =>
        new(Utf8.Str(d->etag), d->not_modified != 0, Utf8.Str(d->user_uri), ToManaged(d->emergency_group), ToManaged(d->imminent_peril_group),
            ToManaged(d->emergency_alert_group), ToManaged(d->emergency_private_recipient), StrArray(d->groups, d->group_count),
            StrArray(d->implicit_affiliations, d->implicit_affiliation_count), d->max_affiliations_n2,
            d->allow_private_call != 0, d->allow_emergency_group_call != 0, d->allow_imminent_peril_call != 0,
            d->allow_activate_emergency_alert != 0, d->allow_cancel_emergency_alert != 0, d->allow_emergency_private_call != 0,
            d->allow_adhoc_group_call != 0, d->allow_cancel_group_emergency != 0, d->allow_cancel_imminent_peril != 0);

    private static ServiceConfigDoc ToManaged(cimsue_service_config_doc_t* d) =>
        new(Utf8.Str(d->etag), d->not_modified != 0, Utf8.Str(d->domain), d->num_levels_group_hierarchy, d->num_levels_user_hierarchy,
            Utf8.Str(d->rp_emergency), Utf8.Str(d->rp_imminent_peril), Utf8.Str(d->rp_normal));

    private static McVideoUserProfileDoc ToManaged(cimsue_mcvideo_user_profile_doc_t* d) =>
        new(Utf8.Str(d->etag), d->not_modified != 0, Utf8.Str(d->user_uri), Utf8.Str(d->mcvideo_id), StrArray(d->groups, d->group_count),
            StrArray(d->implicit_affiliations, d->implicit_affiliation_count), d->max_affiliations_n2, d->max_simultaneous_video_streams,
            d->max_simultaneous_calls_n6, ToManaged(d->emergency_group), ToManaged(d->imminent_peril_group), ToManaged(d->emergency_alert_group),
            d->allow_private_call != 0, d->allow_emergency_group_call != 0, d->allow_emergency_private_call != 0, d->allow_imminent_peril_call != 0,
            d->allow_activate_emergency_alert != 0, d->allow_revoke_transmit != 0, d->allow_remote_ambient_viewing != 0,
            d->allow_local_ambient_viewing != 0, d->allow_adhoc_group_call != 0);

    private static McVideoServiceConfigDoc ToManaged(cimsue_mcvideo_service_config_doc_t* d) =>
        new(Utf8.Str(d->etag), d->not_modified != 0, Utf8.Str(d->domain), Utf8.Str(d->rp_emergency), Utf8.Str(d->rp_imminent_peril),
            Utf8.Str(d->rp_normal), d->confidentiality_protection != 0, d->integrity_protection != 0,
            d->t100_sec, d->t101_sec, d->t102_sec, d->t103_sec, d->t104_sec);

    // MCVideo 몫 정수·삼중값: 코어 -1 = 미기재 ↔ null
    private static int? Opt(int v) => v >= 0 ? v : null;
    private static bool? Tri(int v) => v < 0 ? null : v != 0;
    private static int FromOpt(int? v) => v is int x && x >= 0 ? x : -1;
    private static int FromTri(bool? v) => v is bool b ? Engine.B(b) : -1;

    private static McVideoGroupAttrs? ToManaged(in cimsue_mcvideo_group_attrs_t v)
    {
        if (v.present == 0) return null;
        return new McVideoGroupAttrs
        {
            InviteMembers = v.invite_members != 0, MaxDurationSec = Opt(v.max_duration_sec),
            ProtectMedia = v.protect_media != 0, ProtectTransmissionControl = v.protect_transmission_control != 0,
            AudioEncodings = new List<string>(StrArray(v.audio_encodings, v.audio_encoding_count)),
            VideoEncodings = new List<string>(StrArray(v.video_encodings, v.video_encoding_count)),
            VideoResolutions = Utf8.Str(v.video_resolutions), VideoFrameRate = Utf8.Str(v.video_frame_rate),
            UrgentRealTimeVideoMode = Tri(v.urgent_real_time_video_mode), NonUrgentRealTimeVideoMode = Tri(v.non_urgent_real_time_video_mode),
            NonRealTimeVideoMode = Tri(v.non_real_time_video_mode), ActiveRealTimeVideoMode = Utf8.Str(v.active_real_time_video_mode),
            MaxTransmitters = Opt(v.max_transmitters), MinNumberToStart = Opt(v.min_number_to_start), GroupPriority = Opt(v.group_priority),
            ReceptionHangTimerSec = Opt(v.reception_hang_timer_sec), AllowConferenceState = Tri(v.allow_conference_state),
            AllowEmergencyCall = Tri(v.allow_emergency_call), AllowEmergencyAlert = Tri(v.allow_emergency_alert),
            AllowImminentPerilCall = Tri(v.allow_imminent_peril_call),
        };
    }

    private static void ToNative(McVideoGroupAttrs? a, NativeStrings s, cimsue_mcvideo_group_attrs_t* n)
    {
        cimsue_mcvideo_group_attrs_default(n);
        if (a is null) return;                                // present = 0 — PUT 에 MCVideo 를 싣지 않는다
        n->present = 1;
        n->invite_members = Engine.B(a.InviteMembers); n->max_duration_sec = FromOpt(a.MaxDurationSec);
        n->protect_media = Engine.B(a.ProtectMedia); n->protect_transmission_control = Engine.B(a.ProtectTransmissionControl);
        n->audio_encodings = s.AddArray(a.AudioEncodings, out n->audio_encoding_count);
        n->video_encodings = s.AddArray(a.VideoEncodings, out n->video_encoding_count);
        n->video_resolutions = s.Add(a.VideoResolutions); n->video_frame_rate = s.Add(a.VideoFrameRate);
        n->urgent_real_time_video_mode = FromTri(a.UrgentRealTimeVideoMode);
        n->non_urgent_real_time_video_mode = FromTri(a.NonUrgentRealTimeVideoMode);
        n->non_real_time_video_mode = FromTri(a.NonRealTimeVideoMode);
        n->active_real_time_video_mode = s.Add(a.ActiveRealTimeVideoMode);
        n->max_transmitters = FromOpt(a.MaxTransmitters); n->min_number_to_start = FromOpt(a.MinNumberToStart);
        n->group_priority = FromOpt(a.GroupPriority); n->reception_hang_timer_sec = FromOpt(a.ReceptionHangTimerSec);
        n->allow_conference_state = FromTri(a.AllowConferenceState); n->allow_emergency_call = FromTri(a.AllowEmergencyCall);
        n->allow_emergency_alert = FromTri(a.AllowEmergencyAlert); n->allow_imminent_peril_call = FromTri(a.AllowImminentPerilCall);
    }

    private static UeInitConfigDoc ToManaged(cimsue_ue_init_config_doc_t* d) =>
        new(Utf8.Str(d->etag), d->not_modified != 0, Utf8.Str(d->domain), Utf8.Str(d->mcptt_server_uri), Utf8.Str(d->mcdata_server_uri),
            Utf8.Str(d->mcvideo_server_uri), Engine.ToManaged(d->floor_timers));

    private static ServiceProfile ToManaged(cimsue_service_profile_t* s)
    {
        var eps = new ServiceEndpoint[Math.Max(0, s->transport_count)];
        for (int i = 0; i < eps.Length; ++i) eps[i] = new ServiceEndpoint((Transport)s->transports[i].transport, s->transports[i].port);
        var sec = new string[Math.Max(0, s->sec_mechanism_count)];
        for (int i = 0; i < sec.Length; ++i) sec[i] = Utf8.Str(s->sec_mechanisms[i]);
        return new ServiceProfile(Utf8.Str(s->kind), Utf8.Str(s->sip_host), s->sip_port, (Transport)s->transport, eps, s->enforced != 0,
                                  (MediaSecurity)s->media_security, Utf8.Str(s->domain), Utf8.Str(s->msisdn), Utf8.Str(s->imsi),
                                  Utf8.Str(s->auth_id), Utf8.Str(s->sip_ha1), Utf8.Str(s->mcptt_id), (AuthScheme)s->auth_scheme,
                                  Utf8.Str(s->aka_k), Utf8.Str(s->aka_opc), Utf8.Str(s->aka_amf), sec, s->max_payload_sds_cplane_bytes,
                                  s->udp_no_tcp_switch != 0, s->sms_gateway != 0);
    }

    private static Profile ToManaged(cimsue_profile_t* p)
    {
        var svc = new ServiceProfile[Math.Max(0, p->service_count)];
        for (int i = 0; i < svc.Length; ++i) svc[i] = ToManaged(&p->services[i]);
        ref cimsue_dispatch_profile_t d = ref p->dispatch;
        var members = new DispatchMember[Math.Max(0, d.member_count)];
        for (int i = 0; i < members.Length; ++i)
            members[i] = new DispatchMember(Utf8.Str(d.members[i].user_id), Utf8.Str(d.members[i].name), Utf8.Str(d.members[i].volte_aor),
                                            Utf8.Str(d.members[i].ptt_id), Utf8.Str(d.members[i].extension), Utf8.Str(d.members[i].group_id));
        var targets = new DispatchTarget[Math.Max(0, d.ptt_target_count)];
        for (int i = 0; i < targets.Length; ++i)
            targets[i] = new DispatchTarget(Utf8.Str(d.ptt_targets[i].id), Utf8.Str(d.ptt_targets[i].uri), Utf8.Str(d.ptt_targets[i].name));
        var dispatch = new DispatchProfile(d.present != 0, Utf8.Str(d.group_id), Utf8.Str(d.group_name), Utf8.Str(d.pilot_id),
                                           Utf8.Str(d.monitor_scope), Utf8.Str(d.ptt_listen), Utf8.Str(d.listen_visibility),
                                           Utf8.Str(d.directory_admin) is { Length: > 0 } da ? da : "none", Utf8.Str(d.org_code), members, targets);
        return new Profile(Utf8.Str(p->display_name), Utf8.Str(p->login_id), Utf8.Str(p->country_code), Utf8.Str(p->csc_host),
                           p->csc_port, svc, dispatch, p->allow_group_creation != 0);
    }

    private static GroupDoc ToManaged(cimsue_group_doc_t* d)
    {
        var g = new GroupDoc
        {
            Uri = Utf8.Str(d->uri), DisplayName = Utf8.Str(d->display_name), ETag = Utf8.Str(d->etag),
            SessionType = Utf8.Str(d->session_type) is { Length: > 0 } st ? st : "prearranged",
            Encryption = d->encryption != 0,
            EmergencyCall = d->emergency_call != 0, EmergencyAlert = d->emergency_alert != 0,
            AllowSds = d->allow_sds != 0, AllowFd = d->allow_fd != 0, RequireAffiliation = d->require_affiliation != 0,
            Priority = d->priority, MaxParticipants = d->max_participants,
            OrgCode = Utf8.Str(d->org_code), AuthorizedUser = Utf8.Str(d->authorized_user),
            HangTimerSec = d->has_hang_timer != 0 ? d->hang_timer_sec : null,
            MaxDurationSec = d->has_max_duration != 0 ? d->max_duration_sec : null,
            AllowConferenceState = d->has_conference_state != 0 ? d->allow_conference_state != 0 : null,
            MaxSdsSize = d->has_max_sds_size != 0 ? d->max_sds_size : null,
            MaxAutoRecv = d->has_max_auto_recv != 0 ? d->max_auto_recv : null,
            MinNumberToStart = d->has_min_number_to_start != 0 ? d->min_number_to_start : null,
            AckTimeoutSec = d->has_ack_timeout != 0 ? d->ack_timeout_sec : null,
            AckAction = Utf8.Str(d->ack_action) is { Length: > 0 } act ? act : null,
            Mcvideo = ToManaged(d->mcvideo),
        };
        for (int i = 0; i < d->member_count; ++i)
            g.Members.Add(new GroupMember
            {
                Uri = Utf8.Str(d->members[i].uri), Name = Utf8.Str(d->members[i].display_name),
                Role = Utf8.Str(d->members[i].role) is { Length: > 0 } r ? r : "participant", Priority = d->members[i].priority,
                Required = d->members[i].required != 0, Title = Utf8.Str(d->members[i].title),
                McvideoId = Utf8.Str(d->members[i].mcvideo_id),
            });
        return g;
    }

    private static cimsue_group_doc_t ToNative(GroupDoc g, NativeStrings s)
    {
        cimsue_group_doc_t n = default;
        n.uri = s.Add(g.Uri); n.display_name = s.Add(g.DisplayName); n.etag = s.Add(g.ETag);
        n.member_count = g.Members.Count;
        if (n.member_count > 0)
        {
            n.members = (cimsue_group_member_t*)s.Alloc(sizeof(cimsue_group_member_t) * n.member_count);
            for (int i = 0; i < n.member_count; ++i)
            {
                var m = g.Members[i];
                n.members[i].uri = s.Add(m.Uri); n.members[i].display_name = s.Add(m.Name);
                n.members[i].role = s.Add(m.Role); n.members[i].priority = m.Priority;
                n.members[i].required = Engine.B(m.Required);
                n.members[i].mcvideo_id = string.IsNullOrEmpty(m.McvideoId) ? null : s.Add(m.McvideoId);
            }
        }
        n.session_type = s.Add(g.SessionType);
        n.encryption = Engine.B(g.Encryption);
        n.emergency_call = Engine.B(g.EmergencyCall); n.emergency_alert = Engine.B(g.EmergencyAlert);
        n.allow_sds = Engine.B(g.AllowSds); n.allow_fd = Engine.B(g.AllowFd); n.require_affiliation = Engine.B(g.RequireAffiliation);
        n.priority = g.Priority; n.max_participants = g.MaxParticipants;
        n.org_code = s.Add(g.OrgCode); n.authorized_user = s.Add(g.AuthorizedUser);
        // null = 미기재 → has_* = 0(default). 음수는 받지 않는다 — 미기재로 둔다(서버도 범위 밖은 400).
        if (g.HangTimerSec is int hang && hang >= 0) { n.has_hang_timer = 1; n.hang_timer_sec = hang; }
        if (g.MaxDurationSec is int dur && dur >= 0) { n.has_max_duration = 1; n.max_duration_sec = dur; }
        if (g.AllowConferenceState is bool conf) { n.has_conference_state = 1; n.allow_conference_state = Engine.B(conf); }
        if (g.MaxSdsSize is int sds && sds >= 0) { n.has_max_sds_size = 1; n.max_sds_size = sds; }
        if (g.MaxAutoRecv is int auto && auto >= 0) { n.has_max_auto_recv = 1; n.max_auto_recv = auto; }
        if (g.MinNumberToStart is int min && min >= 0) { n.has_min_number_to_start = 1; n.min_number_to_start = min; }
        if (g.AckTimeoutSec is int tng1 && tng1 >= 0) { n.has_ack_timeout = 1; n.ack_timeout_sec = tng1; }
        if (!string.IsNullOrEmpty(g.AckAction)) n.ack_action = s.Add(g.AckAction);
        ToNative(g.Mcvideo, s, &n.mcvideo);
        return n;
    }

    private static cimsue_service_profile_t ToNative(ServiceProfile sp, NativeStrings s)
    {
        cimsue_service_profile_t n = default;
        n.kind = s.Add(sp.Kind);
        n.sip_host = s.Add(sp.SipHost);
        n.sip_port = sp.SipPort;
        n.transport = (int)sp.Transport;
        n.transport_count = sp.Transports.Count;
        if (n.transport_count > 0)
        {
            n.transports = (cimsue_service_endpoint_t*)s.Alloc(sizeof(cimsue_service_endpoint_t) * n.transport_count);
            for (int i = 0; i < n.transport_count; ++i)
            {
                n.transports[i].transport = (int)sp.Transports[i].Transport;
                n.transports[i].port = sp.Transports[i].Port;
            }
        }
        n.enforced = Engine.B(sp.Enforced);
        n.media_security = (int)sp.MediaSecurity;
        n.domain = s.Add(sp.Domain); n.msisdn = s.Add(sp.Msisdn); n.imsi = s.Add(sp.Imsi);
        n.auth_id = s.Add(sp.AuthId); n.sip_ha1 = s.Add(sp.SipHa1); n.mcptt_id = s.Add(sp.McpttId);
        n.auth_scheme = (int)sp.AuthScheme;
        n.aka_k = s.Add(sp.AkaK); n.aka_opc = s.Add(sp.AkaOpc); n.aka_amf = s.Add(sp.AkaAmf);
        n.sec_mechanisms = s.AddArray(sp.SecMechanisms, out n.sec_mechanism_count);
        n.max_payload_sds_cplane_bytes = sp.MaxPayloadSdsCplaneBytes;
        n.udp_no_tcp_switch = Engine.B(sp.UdpNoTcpSwitch);
        n.sms_gateway = Engine.B(sp.SmsGateway);
        return n;
    }
}
