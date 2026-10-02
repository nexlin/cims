// 등록에 묶인 서버 상태(제휴·구독)의 유지 (dispatch_desktop_ui.md §6 «등록에 묶인 것»).
//   서버는 등록이 사라지면 그 가입자의 제휴(affiliation)를 전부 내린다(TS 24.379 §9 — 제휴는 등록에 묶인다). 등록이 사라지는 것은 해지 REGISTER
//   만이 아니다 — 등록 만료, 그리고 TCP/TLS 연결이 끊긴 바인딩의 회수(registration_binding_set.md §4 계기 2)도 같다. 망이 끊겼다 돌아오면 코어가
//   다시 등록하지만(Engine.HandleNetworkChange) 제휴 PUBLISH·구독 SUBSCRIBE 는 코어가 한 번 보낼 뿐 유지하지 않는다(ue_sdk.md §4.2 — 목표 집합·
//   재시도는 앱). 다시 싣지 않으면 편성 그룹 [참여] 가 403(Warning 120 — 미제휴)으로 거절되고 로스터·회선 감시·그룹 변경 통지도 오지 않는다.
//   그 둘의 수명은 3600 초다(코어 요청 값 = 서버 상한) — 끊기지 않아도 한 시간 뒤에는 사라진다. 수명 절반마다 다시 싣는다(RFC 3903 §4.1 · RFC 6665 §4.1.2.2).
//   계기 넷: ① 등록이 끊겼다 다시 섬 ② 망이 바뀐 뒤의 첫 등록 성공(등록 상태는 줄곧 «등록됨» 이어도) ③ 수명 절반(1분 틱) ④ [참여] 403 — 제휴를 다시
//   싣고 한 번 더. 태블릿 `UpkeepPlane.kt` 와 같은 규칙·같은 값이다.
using CimsUe;
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

/// <summary>유지 규칙 — 순수 판정(태블릿 `UpkeepRules` 와 같은 값).</summary>
public static class UpkeepRules
{
    /// <summary>제휴 PUBLISH·구독 SUBSCRIBE 의 수명(초) — 코어가 요청하는 값이자 서버 상한(CSP SUBSCRIBE_MAX_EXPIRES_SEC).</summary>
    public const int LifetimeSec = 3600;
    /// <summary>갱신 시각을 보는 주기.</summary>
    public const long TickMs = 60_000;
    /// <summary>제휴 PUBLISH 의 최종 응답을 기다리는 시한 — 넘기면 응답 없음으로 보고 물러났다 다시 싣는다.</summary>
    public const long ConfirmTimeoutMs = 8_000;
    /// <summary>같은 그룹의 [참여] 를 제휴 재적재 뒤 다시 거는 최소 간격 — 다시 건 것까지 403 이면 그대로 알린다.</summary>
    public const long RejoinGapMs = 10_000;
    private const long RetryBaseMs = 60_000, RetryMaxMs = 30 * 60_000;

    /// <summary>다시 실을 때인가 — 실은 적이 없거나(0) 수명 절반이 지났다.</summary>
    public static bool Due(long atMs, long nowMs, int lifetimeSec = LifetimeSec) => atMs <= 0 || nowMs - atMs >= lifetimeSec * 500L;

    /// <summary>이 등록 이벤트로 등록에 묶인 상태를 다시 세워야 하나. was = 이 계정의 앞선 등록 이벤트가 «등록됨» 이었나(null = 처음 — 로그인 절차가 건다),
    /// networkChanged = 망이 바뀐 뒤 이 계정의 등록 결과를 아직 보지 못했다.</summary>
    public static bool Renewed(bool? was, bool registered, bool networkChanged) => registered && (was == false || networkChanged);

    /// <summary>거절·응답 없음 뒤 다음 시도까지 — 1분부터 배로, 최대 30분.</summary>
    public static long RetryDelayMs(int failures) => Math.Min(RetryBaseMs << Math.Clamp(failures - 1, 0, 5), RetryMaxMs);

    /// <summary>거절된 [참여] 를 제휴 재적재 뒤 다시 걸 것인가 — 403 이고 내가 멤버인 그룹이며, 방금 그렇게 다시 건 호가 아닐 때. 편성 그룹의 미제휴 거절은
    /// 403 + Warning 120(TS 24.379 §10.1.1.4.2)인데 비멤버 거절도 403 이다 — 코어가 Warning 을 올리지 않으므로 목록(GMS)의 멤버십으로 가른다.</summary>
    public static bool Rejoin(int code, bool member, long lastRejoinAtMs, long nowMs) =>
        code == 403 && member && (lastRejoinAtMs <= 0 || nowMs - lastRejoinAtMs >= RejoinGapMs);
}

public sealed partial class DispatchSession
{
    /// <summary>그룹 하나의 제휴 — 서버가 2xx 로 받은 시각(0 = 확인 없음)·응답 대기·거절/무응답 뒤의 물러남.</summary>
    private sealed class AffState { public long At; public bool Pending; public int Failures; public long RetryAt; }

    private readonly Dictionary<string, AffState> _aff = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>제휴 PUBLISH 의 token → (그룹, 보낸 시각). 최종 응답(RequestCompleted)으로 맞춘다.</summary>
    private readonly Dictionary<long, (string Group, long Since)> _affTokens = new();
    /// <summary>제휴가 서면 다시 걸 [참여] — 그룹 → 처음의 거절(제휴도 서지 않으면 이것을 그대로 알린다).</summary>
    private readonly Dictionary<string, (int Code, string Reason)> _rejoinPending = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, long> _rejoinAt = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>계정별 — 마지막 등록 이벤트가 «등록됨» 이었나.</summary>
    private readonly Dictionary<int, bool> _regSeen = new();
    /// <summary>망이 바뀐 뒤 등록 결과를 아직 보지 못한 계정.</summary>
    private readonly HashSet<int> _netChanged = new();
    /// <summary>등록이 새로 섰다 — 다시 세울 것이 남았다(계정이 등록된 때의 맞춤이 거둔다).</summary>
    private bool _pttOwed, _volteOwed;
    /// <summary>conference·xcap-diff 구독을 건 시각 / 회선 감시(dialog) 구독을 건 시각 / 다음 맞춤.</summary>
    private long _subsAt, _watchAt, _nextUpkeep = long.MaxValue;

    private static long UpkeepNow => Environment.TickCount64;

    /// <summary>로그인 절차가 처음 구독을 건다 — 그 시각을 갱신의 기준으로 잡는다(곧바로 한 번 더 걸지 않게).</summary>
    private void BeginUpkeep()
    {
        _subsAt = _watchAt = UpkeepNow;
        _nextUpkeep = UpkeepNow + UpkeepRules.TickMs;
    }

    private void ResetUpkeep()
    {
        _aff.Clear(); _affTokens.Clear(); _rejoinPending.Clear(); _rejoinAt.Clear(); _regSeen.Clear(); _netChanged.Clear();
        _pttOwed = _volteOwed = false; _subsAt = _watchAt = 0; _nextUpkeep = long.MaxValue;
    }

    /// <summary>망이 바뀌었다(HandleNetworkChange 직전) — 다음 등록 성공에 등록에 묶인 상태를 다시 세운다.</summary>
    private void NoteUpkeepNetworkChange()
    {
        if (Ptt is { } p) _netChanged.Add(p.Id);
        if (Volte is { } v) _netChanged.Add(v.Id);
    }

    /// <summary>등록 이벤트(REGISTER 응답마다 — 갱신 포함) → 다시 세울 것이 생겼나.</summary>
    private void NoteUpkeepRegistration(RegInfo r, AccountKind kind)
    {
        bool registered = r.State == RegState.Registered;
        bool? was = _regSeen.TryGetValue(r.AccountId, out bool w) ? w : null;
        _regSeen[r.AccountId] = registered;
        bool changed = registered && _netChanged.Remove(r.AccountId);
        if (!UpkeepRules.Renewed(was, registered, changed)) return;
        if (kind == AccountKind.Ptt) _pttOwed = true; else _volteOwed = true;
        _nextUpkeep = 0;                                         // 다음 틱에 곧바로
    }

    /// <summary>1초 틱 — 응답이 없는 제휴 PUBLISH 를 시한으로 마감하고, 때가 되면 한 번 맞춘다.</summary>
    private void TickUpkeep()
    {
        long now = UpkeepNow;
        foreach (var (token, w) in _affTokens.Where(kv => now - kv.Value.Since >= UpkeepRules.ConfirmTimeoutMs).ToList())
        {
            _affTokens.Remove(token);
            ApplyAffiliationResult(w.Group, 0, "no answer");
        }
        if (!IsReady || now < _nextUpkeep) return;
        _nextUpkeep = now + UpkeepRules.TickMs;
        UpkeepPass(now);
    }

    /// <summary>한 번의 맞춤 — 등록된 계정마다 낡은 것만 다시 싣는다: 멤버 그룹의 제휴(확인 없음·수명 절반), conference·xcap-diff 구독, 회선 감시 구독.
    /// 등록이 새로 선 계정은 전부 낡은 것으로 본다. 등록 전이면 남겨 둔다 — 등록 이벤트가 다시 부른다.</summary>
    private void UpkeepPass(long now)
    {
        if (Ptt is { } ptt && PttReg.State == RegState.Registered)
        {
            if (_pttOwed)
            {
                _pttOwed = false;
                foreach (var a in _aff.Values) { a.At = 0; a.RetryAt = 0; }
                _subsAt = 0;
                // MCVideo 제휴도 등록에 묶여 있다(TS 24.281 §8.2.2.2) — 다시 싣지 않으면 편성 영상 호의 초대가 오지 않는다(코어가 관심 그룹 전부를 한 PUBLISH 로)
                if (Groups.Any(g => g.McVideoAffiliated))
                {
                    foreach (var g in Groups.Where(g => g.McVideoAffiliated)) g.McVideoAffiliated = false;
                    ApplyMcVideoGroups();
                }
                Log.Info("upkeep: ptt registration renewed — re-affiliating, re-subscribing");
            }
            foreach (var g in Groups.Where(g => g.IsMember).ToList())
            {
                var a = AffOf(g.Id);
                if (a.Pending || !UpkeepRules.Due(a.At, now) || a.RetryAt > now) continue;
                AffiliateTracked(g.Id);
            }
            if (UpkeepRules.Due(_subsAt, now))
            {
                foreach (var g in Groups.ToList())
                {
                    var sc = ptt.SubscribeConference(g.Id, true);
                    if (!sc.Ok) Log.Warn($"upkeep: conference subscribe {g.Id}: {sc}");
                }
                if (PttDomain.Length > 0)
                {
                    var x = ptt.SubscribeXcapDiff($"sip:gms_psi@{PttDomain}", true);
                    if (!x.Ok) Log.Warn($"upkeep: xcap-diff subscribe: {x}");
                }
                _subsAt = now;
            }
        }
        if (Volte is not null && VolteReg.State == RegState.Registered)
        {
            if (_volteOwed) { _volteOwed = false; _watchAt = 0; Log.Info("upkeep: volte registration renewed — re-subscribing dialog watch"); }
            if (HasDesk && UpkeepRules.Due(_watchAt, now))
            {
                _watched.Clear();                                // 이미 구독한 것도 다시 건다(Watch 는 구독 중이면 건너뛴다)
                WatchAll();
                _watchAt = now;
            }
        }
    }

    private AffState AffOf(string groupId)
    {
        if (!_aff.TryGetValue(groupId, out var a)) _aff[groupId] = a = new AffState();
        return a;
    }

    /// <summary>제휴 PUBLISH 한 건 — 명령이 받아들여진 것은 제휴가 선 것이 아니다(서버가 거절하면 그룹콜이 오지 않고 [참여] 가 403 이다). 최종 응답
    /// 2xx 에서만 제휴로 적는다(<see cref="OnAffiliationResult"/>). 보냈으면 true.</summary>
    private bool AffiliateTracked(string groupId)
    {
        if (Ptt is not { } ptt) return false;
        var a = AffOf(groupId);
        var r = ptt.Affiliate(groupId, true);
        if (!r.Ok) { ApplyAffiliationResult(groupId, -1, r.Reason); return false; }
        a.Pending = true;
        _affTokens[r.Value] = (groupId, UpkeepNow);
        return true;
    }

    /// <summary>제휴 PUBLISH 의 최종 응답(token 상관).</summary>
    private void OnAffiliationResult(RequestResult r)
    {
        if (_affTokens.Remove(r.Token, out var w)) ApplyAffiliationResult(w.Group, r.Code, r.Reason);
    }

    private void ApplyAffiliationResult(string groupId, int code, string reason)
    {
        bool ok = code is >= 200 and < 300;
        long now = UpkeepNow;
        if (_aff.TryGetValue(groupId, out var a))               // 그사이 내 채널에서 빠진 그룹은 적지 않는다
        {
            a.Pending = false;
            if (ok) { a.At = now; a.Failures = 0; a.RetryAt = 0; }
            else
            {
                a.At = 0; a.Failures++; a.RetryAt = now + UpkeepRules.RetryDelayMs(a.Failures);
                Log.Warn($"upkeep: affiliate {groupId}: {code} {reason} — retry in {UpkeepRules.RetryDelayMs(a.Failures) / 1000}s");
            }
        }
        var g = Groups.FirstOrDefault(x => x.Id == groupId && x.IsMember);
        if (g is not null) g.Affiliated = ok;
        if (!_rejoinPending.Remove(groupId, out var first)) return;
        // 제휴도 서지 않았다 — 정말 멤버가 아니거나 서버에 닿지 않는다. 처음의 거절을 그대로 알린다.
        if (!ok || g is null) { Notify.Error(ResponseText.Describe(ResponseText.Area.PttJoin, first.Code, first.Reason), $"{first.Code} {first.Reason}"); return; }
        Log.Info($"upkeep: join {groupId} 403 — re-affiliated, joining again");
        JoinChannel(g);
    }

    /// <summary>내 채널에서 빠진 그룹 — 유지 대상에서 뺀다.</summary>
    private void ForgetUpkeep(string groupId) { _aff.Remove(groupId); _rejoinAt.Remove(groupId); _rejoinPending.Remove(groupId); }

    /// <summary>연결되지 못하고 403 으로 끝난 [참여] — 멤버 그룹이면 제휴를 다시 싣고 **한 번 더** 건다. 서버가 등록을 새것으로 보는 사이(연결이 끊긴 바인딩의
    /// 회수·등록 만료) 제휴가 내려갔는데 단말은 등록이 이어진 것으로만 보는 경우가 있다 — 등록 이벤트로는 알 수 없고 이 거절이 유일한 신호다. 다시 걸
    /// 것이면 true — 이 끝은 실패로 알리지 않는다(다시 건 호의 결과가 알린다). 일제 통화 개시는 누르는 동안만 유효한 호라 다시 걸지 않고 제휴만 다시
    /// 싣는다(다음 누름이 성립한다). 긴급 참여는 서버가 암묵적으로 제휴시키므로(TS 24.379 §9.2.2.3.7) 대상이 아니다.</summary>
    private bool RejoinsAfterAffiliation(SessionItem s)
    {
        var ci = s.Info;
        if (s.ConnectedAt is not null || ci.Dir != CallDir.Outgoing || s.Operation is not (Operation.PttJoin or Operation.Broadcast)) return false;
        string gid = ci.GroupId;
        long now = UpkeepNow;
        bool member = Groups.Any(g => g.Id == gid && g.IsMember);
        if (!UpkeepRules.Rejoin(ci.LastCode, member, _rejoinAt.GetValueOrDefault(gid), now) || Ptt is null) return false;
        if (s.Operation == Operation.Broadcast) { AffiliateTracked(gid); return false; }
        _rejoinAt[gid] = now;
        if (!AffiliateTracked(gid)) return false;
        _rejoinPending[gid] = (ci.LastCode, ci.LastReason);
        return true;
    }
}
