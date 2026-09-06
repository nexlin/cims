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

/// <summary>프로비저닝 프로파일의 서비스 1개(kind = volte | ptt).</summary>
public sealed record ServiceProfile(
    string Kind, string SipHost, int SipPort, Transport Transport, IReadOnlyList<ServiceEndpoint> Transports, bool Enforced,
    MediaSecurity MediaSecurity, string Domain, string Msisdn, string Imsi, string AuthId, string SipHa1, string McpttId,
    AuthScheme AuthScheme, string AkaK, string AkaOpc, string AkaAmf, IReadOnlyList<string> SecMechanisms, int MaxPayloadSdsCplaneBytes)
{
    /// <summary>이 서비스로 등록할 계정 설정 — 프로파일 값 그대로(loginPw 는 sipHa1 부재 시 평문 폴백). 규칙은 코어(toAccount).</summary>
    public AccountConfig ToAccountConfig(string? loginPw = null) => CscClient.ToAccountConfig(this, loginPw);
}

/// <summary>관제 그룹원(dispatch members[]) — dialog 구독·그룹원 띠 대상.</summary>
public sealed record DispatchMember(string UserId, string Name, string VolteAor, string PttId, string Extension);
/// <summary>청취 대상 PTT 그룹(dispatch pttTargets[] — 서버가 ptt_listen 범위를 해석한 결과).</summary>
public sealed record DispatchTarget(string Id, string Uri, string Name);

/// <summary>관제 데스크(dispatch_center.md §8.4) — 없으면 Present=false. MonitorScope·PttListen = none|own|listed|all, ListenVisibility = hidden|visible.
/// Members/PttTargets 는 서버가 주지 않으면 빈 목록.</summary>
public sealed record DispatchProfile(bool Present, string GroupId, string GroupName, string PilotId,
                                     string MonitorScope, string PttListen, string ListenVisibility,
                                     IReadOnlyList<DispatchMember> Members, IReadOnlyList<DispatchTarget> PttTargets)
{
    public static DispatchProfile None { get; } = new(false, "", "", "", "none", "none", "hidden", Array.Empty<DispatchMember>(), Array.Empty<DispatchTarget>());
}

public sealed record Profile(string DisplayName, string LoginId, string CountryCode, string CscHost, int CscPort,
                             IReadOnlyList<ServiceProfile> Services, DispatchProfile Dispatch, bool AllowGroupCreation)
{
    /// <summary>kind 로 서비스 찾기 — 없으면 null.</summary>
    public ServiceProfile? Service(string kind) => Services.FirstOrDefault(s => s.Kind == kind);
}

/// <summary>GMS 목록 항목. IsOwner = 토큰 주체가 authorized user(편집·삭제 가능).</summary>
public sealed record GroupSummary(string Uri, string DisplayName, string ETag, int MemberCount, bool IsOwner);
public sealed record XcapDoc(string Body, string ETag, bool NotModified);

/// <summary>그룹 문서 멤버 — Role = chair | participant.</summary>
public sealed class GroupMember
{
    public string Uri { get; set; } = "";
    public string Name { get; set; } = "";
    public string Role { get; set; } = "participant";
    public int Priority { get; set; } = 5;
}

/// <summary>GMS 그룹 문서(OMA list-service + TS 24.481 mcpttgi) — GET 산출·PUT 입력 공용 모델(cimsue/csc.h GroupDoc 1:1). 편집 폼이 그대로 쓰도록 가변.</summary>
public sealed class GroupDoc
{
    public string Uri { get; set; } = "";
    public string DisplayName { get; set; } = "";
    /// <summary>서버 산출(GET/PUT 응답 ETag). 수정 PUT 의 If-Match 로 쓴다.</summary>
    public string ETag { get; set; } = "";
    public List<GroupMember> Members { get; set; } = new();
    /// <summary>prearranged | chat | broadcast</summary>
    public string SessionType { get; set; } = "prearranged";
    public bool VideoEnabled { get; set; }
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

    private static ServiceProfile ToManaged(cimsue_service_profile_t* s)
    {
        var eps = new ServiceEndpoint[Math.Max(0, s->transport_count)];
        for (int i = 0; i < eps.Length; ++i) eps[i] = new ServiceEndpoint((Transport)s->transports[i].transport, s->transports[i].port);
        var sec = new string[Math.Max(0, s->sec_mechanism_count)];
        for (int i = 0; i < sec.Length; ++i) sec[i] = Utf8.Str(s->sec_mechanisms[i]);
        return new ServiceProfile(Utf8.Str(s->kind), Utf8.Str(s->sip_host), s->sip_port, (Transport)s->transport, eps, s->enforced != 0,
                                  (MediaSecurity)s->media_security, Utf8.Str(s->domain), Utf8.Str(s->msisdn), Utf8.Str(s->imsi),
                                  Utf8.Str(s->auth_id), Utf8.Str(s->sip_ha1), Utf8.Str(s->mcptt_id), (AuthScheme)s->auth_scheme,
                                  Utf8.Str(s->aka_k), Utf8.Str(s->aka_opc), Utf8.Str(s->aka_amf), sec, s->max_payload_sds_cplane_bytes);
    }

    private static Profile ToManaged(cimsue_profile_t* p)
    {
        var svc = new ServiceProfile[Math.Max(0, p->service_count)];
        for (int i = 0; i < svc.Length; ++i) svc[i] = ToManaged(&p->services[i]);
        ref cimsue_dispatch_profile_t d = ref p->dispatch;
        var members = new DispatchMember[Math.Max(0, d.member_count)];
        for (int i = 0; i < members.Length; ++i)
            members[i] = new DispatchMember(Utf8.Str(d.members[i].user_id), Utf8.Str(d.members[i].name), Utf8.Str(d.members[i].volte_aor),
                                            Utf8.Str(d.members[i].ptt_id), Utf8.Str(d.members[i].extension));
        var targets = new DispatchTarget[Math.Max(0, d.ptt_target_count)];
        for (int i = 0; i < targets.Length; ++i)
            targets[i] = new DispatchTarget(Utf8.Str(d.ptt_targets[i].id), Utf8.Str(d.ptt_targets[i].uri), Utf8.Str(d.ptt_targets[i].name));
        var dispatch = new DispatchProfile(d.present != 0, Utf8.Str(d.group_id), Utf8.Str(d.group_name), Utf8.Str(d.pilot_id),
                                           Utf8.Str(d.monitor_scope), Utf8.Str(d.ptt_listen), Utf8.Str(d.listen_visibility), members, targets);
        return new Profile(Utf8.Str(p->display_name), Utf8.Str(p->login_id), Utf8.Str(p->country_code), Utf8.Str(p->csc_host),
                           p->csc_port, svc, dispatch, p->allow_group_creation != 0);
    }

    private static GroupDoc ToManaged(cimsue_group_doc_t* d)
    {
        var g = new GroupDoc
        {
            Uri = Utf8.Str(d->uri), DisplayName = Utf8.Str(d->display_name), ETag = Utf8.Str(d->etag),
            SessionType = Utf8.Str(d->session_type) is { Length: > 0 } st ? st : "prearranged",
            VideoEnabled = d->video_enabled != 0, Encryption = d->encryption != 0,
            EmergencyCall = d->emergency_call != 0, EmergencyAlert = d->emergency_alert != 0,
            AllowSds = d->allow_sds != 0, AllowFd = d->allow_fd != 0, RequireAffiliation = d->require_affiliation != 0,
            Priority = d->priority, MaxParticipants = d->max_participants,
            OrgCode = Utf8.Str(d->org_code), AuthorizedUser = Utf8.Str(d->authorized_user),
        };
        for (int i = 0; i < d->member_count; ++i)
            g.Members.Add(new GroupMember
            {
                Uri = Utf8.Str(d->members[i].uri), Name = Utf8.Str(d->members[i].display_name),
                Role = Utf8.Str(d->members[i].role) is { Length: > 0 } r ? r : "participant", Priority = d->members[i].priority,
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
            }
        }
        n.session_type = s.Add(g.SessionType);
        n.video_enabled = Engine.B(g.VideoEnabled); n.encryption = Engine.B(g.Encryption);
        n.emergency_call = Engine.B(g.EmergencyCall); n.emergency_alert = Engine.B(g.EmergencyAlert);
        n.allow_sds = Engine.B(g.AllowSds); n.allow_fd = Engine.B(g.AllowFd); n.require_affiliation = Engine.B(g.RequireAffiliation);
        n.priority = g.Priority; n.max_participants = g.MaxParticipants;
        n.org_code = s.Add(g.OrgCode); n.authorized_user = s.Add(g.AuthorizedUser);
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
        return n;
    }
}
