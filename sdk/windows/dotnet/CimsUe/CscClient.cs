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

/// <summary>관제 데스크(dispatch_center.md §8.4) — 없으면 Present=false. MonitorScope·PttListen = none|own|listed|all, ListenVisibility = hidden|visible.</summary>
public sealed record DispatchProfile(bool Present, string GroupId, string GroupName, string PilotId,
                                     string MonitorScope, string PttListen, string ListenVisibility)
{
    public static DispatchProfile None { get; } = new(false, "", "", "", "none", "none", "hidden");
}

public sealed record Profile(string DisplayName, string LoginId, string CountryCode, string CscHost, int CscPort,
                             IReadOnlyList<ServiceProfile> Services, DispatchProfile Dispatch)
{
    /// <summary>kind 로 서비스 찾기 — 없으면 null.</summary>
    public ServiceProfile? Service(string kind) => Services.FirstOrDefault(s => s.Kind == kind);
}

public sealed record GroupSummary(string Uri, string DisplayName, string ETag, int MemberCount);
public sealed record XcapDoc(string Body, string ETag, bool NotModified);

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
                list[i] = new GroupSummary(Utf8.Str(g[i].uri), Utf8.Str(g[i].display_name), Utf8.Str(g[i].etag), g[i].member_count);
            return Result<IReadOnlyList<GroupSummary>>.Success(list);
        }
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
        var dispatch = new DispatchProfile(d.present != 0, Utf8.Str(d.group_id), Utf8.Str(d.group_name), Utf8.Str(d.pilot_id),
                                           Utf8.Str(d.monitor_scope), Utf8.Str(d.ptt_listen), Utf8.Str(d.listen_visibility));
        return new Profile(Utf8.Str(p->display_name), Utf8.Str(p->login_id), Utf8.Str(p->country_code), Utf8.Str(p->csc_host),
                           p->csc_port, svc, dispatch);
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
