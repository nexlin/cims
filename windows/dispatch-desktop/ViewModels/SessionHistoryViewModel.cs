// [이력] 화면(§4.6) — 서버 통합 이력을 **창 조회**(`/provisioning/history?since=&until=`, 하루 단위)로 받아 콘솔 `/service/history/volte`·
// `/service/history/ptt` 와 같은 구성으로 보인다: 도구줄 아래 **시간대 밴드**(그날의 분포이자 필터) · 통화는 콘솔과 같은 열의 표(유형·발신→착신·
// 상태·시작/응답/종료·통화시간·종료사유·녹취) · PTT 는 좌 세션 요약 카드(종류·상태·대상·시각·턴/화자/발화/동시·개시) + 우 선택 세션 패널
// (지표 → 참여자 → 발언 타임라인(화자 레인, 막대 클릭 = 그 턴 재생) → 이벤트 타임라인(floor 중재 + 입퇴장) → 녹취 재생).
// 발언 지표·참여자·floor 는 서버가 콘솔 PTT 이력의 읽기 모델(OAM 세션 인덱스·세션 상세)을 범위 게이트 뒤에서 프록시한 값이고,
// 발언 턴은 녹취 세그먼트의 슬롯 트랙에서 만든다(콘솔 segTurns 와 같은 해석). 범위·감사는 서버(monitor_scope/ptt_listen, E-AUD-016).
// 녹취 재생은 MP4(AAC) 를 받아(`/provisioning/recordings/{id}/segments/{seq}/audio?slot=`) 창의 MediaElement 로 튼다. ② ④ 실시간 내역과 달리
// 이 화면은 지난 기록 열람 전용이다(폴링 없음).
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Data;
using CimsUe;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

/// <summary>시간대 밴드 한 칸 — 값·상대 농도(0 = 없음, 0.08~0.45 = 많을수록 진하게 — 글자가 읽히는 연한 남색 범위)·선택.
/// Tip = 무엇을 센 건지(통화 시도 / 무전 세션 / 발언). Partial = 목록 상한으로 덜 센 칸(값 뒤 "+").</summary>
public sealed partial class HourCell : ObservableObject
{
    public string Hour { get; }
    public int Count { get; }
    public double Ratio { get; }
    public bool Partial { get; }
    public string CountText => Count > 0 || Partial ? $"{Count}{(Partial ? "+" : "")}" : "";
    public string Tip { get; }
    [ObservableProperty] private bool _isSelected;
    public HourCell(string hour, int count, int max, string tip, bool partial = false)
    {
        Hour = hour; Count = count; Partial = partial; Ratio = count > 0 && max > 0 ? 0.08 + 0.37 * count / max : 0;
        Tip = tip;
    }
}

/// <summary>목록 한 행 — 통화(표 열)·PTT(요약 카드) 두 종류의 표시값을 서버 항목에서 미리 만든다. 이름은 주소록, 그룹은 GMS 목록 이름.
/// 무전 목록은 빈 세션 묶음(§4.6)의 머리 행(IsBundle)과 펼친 묶음 안의 한 줄 행(IsBundleMember)도 이 행으로 그린다.</summary>
public sealed partial class HistoryRow : ObservableObject
{
    public HistoryEntry E { get; }
    public string TimeText => E.Time.ToString("HH:mm:ss");
    public string KindText { get; }
    public string Parties { get; }
    public string DurationText { get; }
    public string EventText { get; }
    public bool HasRecording => E.HasRecording;
    public bool IsEmergency => E.Emergency;
    public bool IsLive { get; }
    public string StateText { get; }

    // 통화 표 열 (콘솔 VoLTE 이력과 같은 열)
    public bool IsVideo => E.CallType == "volte_video";
    public string TypeText => IsVideo ? "영상" : "음성";
    public string CallerLabel { get; }
    public string CalleeLabel { get; }
    public string InviteText => Clock(E.InviteTime ?? (E.Kind == HistoryKind.Call ? E.Time : null));
    public string AnswerText => Clock(E.AnswerTime);
    public string EndText => Clock(E.EndTime);
    public string EndReasonText { get; }

    // PTT 요약 카드 (콘솔 SessionCard 와 같은 항목)
    public string SessionKindText { get; }
    public bool IsDuplex => E.FloorControl == "off";
    public string TargetText { get; }
    public string RangeText { get; private set; }
    public string GroupIdText { get; }
    /// <summary>카드 둘째 줄 — 시각 범위(·길이)(· 그룹 id).</summary>
    public string SubText => GroupIdText.Length > 0 ? $"{RangeText} · {GroupIdText}" : RangeText;
    public int TurnCount => E.TurnCount;
    public int SpeakerCount => E.SpeakerCount > 0 ? E.SpeakerCount : E.People.Count;
    public string SpeechText => SessionHistoryViewModel.FmtSpeech(E.TotalSpeechMs);
    public int MaxConcurrent => E.MaxConcurrent;
    public bool ShowConcurrent => E.MaxConcurrent > 1;
    public string InitiatorLabel { get; }
    public bool HasInitiator => E.From.Length > 0;
    public string FloorPolicyText { get; }
    public bool HasFloorPolicy => E.FloorControl == "on" && E.FloorPolicy.Length > 0;
    // PTT 카드 — 관제 채널 카드(§4 공통)와 같은 줄 구성: 1줄 점·이름·라벨·시각 · 2줄 누가·몇 명 · 3줄 길이·발언
    public bool IsPrivate => E.SessionKind == "private";
    public bool IsAdhoc => E.SessionKind == "adhoc";
    /// <summary>MCVideo 그룹 호(영상 세션) — 무전 목록에 같이 서되 «영상» 라벨, 세는 말은 발언이 아니라 송출.</summary>
    public bool IsMcVideo => E.IsMcVideo;
    /// <summary>발언(영상 세션은 송출) 없이 끝난 무전 세션 — 서버가 발언 수 0 을 실어 왔고 녹취도 없다(스캔 폴백은 발언 수가 늘 0 이라 녹취로 한 번 더 가른다). 진행 중이면 아니다.</summary>
    public bool IsSilent => E.Kind == HistoryKind.Ptt && !IsLive && E.HasTurnCount && E.TurnCount == 0 && !E.HasRecording;
    /// <summary>영상 세션 속성 한 줄 — "chat · 동시 송출 2"(없으면 "").</summary>
    public string McvText => !E.IsMcVideo ? "" : string.Join(" · ", new[]
        { E.McvSessionType switch { "chat" => "chat", "prearranged" => "편성", _ => "" }, E.McvMaxTransmitters > 0 ? $"동시 송출 {E.McvMaxTransmitters}" : "" }.Where(x => x.Length > 0));
    public bool HasMcvText => McvText.Length > 0;
    /// <summary>목록 묶음 열쇠 — 시간대 밴드와 같은 축(AxisTime)의 "HH시".</summary>
    public string HourKey => E.AxisTime.ToString("HH") + "시";
    public string StartClock => E.AxisTime.ToString("HH:mm");
    public int PeopleCount => E.People.Count > 0 ? E.People.Count : E.MemberCount;
    public string WhoLine { get; }
    public string StatLine { get; private set; }
    /// <summary>세션 길이(초) — 시작~종료, 없으면 서버 duration.</summary>
    public int DurSec { get; }

    // ── 빈 세션 묶음(§4.6) — 목록에서 연달아 나오는 같은 그룹·같은 개시자의 빈 세션(IsSilent) 2건 이상을 한 장으로.
    //    머리 행 = 가장 최근 세션의 행(고르면 그 세션이 열린다) + Members(최근이 앞), 펼치면 머리 아래에 한 줄 행(IsBundleMember)이 붙는다.
    public bool IsBundle { get; private init; }
    public IReadOnlyList<HistoryRow> Members { get; private init; } = Array.Empty<HistoryRow>();
    /// <summary>펼침 상태의 열쇠 — 시간대 + 가장 이른 세션(새 세션이 위에 붙어도 이어진다).</summary>
    public string BundleKey { get; private init; } = "";
    public string BundleTagText => $"빈 세션 {Members.Count}건";
    [ObservableProperty] private bool _isBundleMember;
    [ObservableProperty] private bool _bundleExpanded;
    public string BundleToggleText => BundleExpanded ? "접기" : $"{Members.Count}건 펼치기";
    public bool IsCard => !IsBundleMember;
    /// <summary>펼친 묶음 안 한 줄의 오른쪽 — "발언 0회"(영상 세션은 송출).</summary>
    public string MemberStat => $"{(E.IsMcVideo ? "송출" : "발언")} {E.TurnCount}회";
    partial void OnBundleExpandedChanged(bool value) => OnPropertyChanged(nameof(BundleToggleText));
    partial void OnIsBundleMemberChanged(bool value) => OnPropertyChanged(nameof(IsCard));
    private string _bundleRange = "";
    /// <summary>카드 오른쪽 위 시각 — 묶음은 가장 이른 ~ 가장 늦은 시작 시각.</summary>
    public string ClockText => IsBundle ? _bundleRange : StartClock;
    /// <summary>시간대 머리 건수에 더하는 세션 수 — 묶음 머리 = 묶은 수, 펼친 한 줄 = 0(머리가 셌다), 나머지 1.</summary>
    public int Weight => IsBundle ? Members.Count : IsBundleMember ? 0 : 1;

    /// <summary>빈 세션 묶음의 머리 행 — members 는 목록 순서(최근이 앞), 2건 이상.</summary>
    public static HistoryRow Bundle(IReadOnlyList<HistoryRow> members, DispatchSession s, string key)
    {
        var top = members[0]; var first = members[^1];
        int lo = members.Min(m => m.DurSec), hi = members.Max(m => m.DurSec);
        string each = lo == hi ? SessionHistoryViewModel.FmtDur(lo) : hi < 60 ? $"{lo}~{hi}초" : $"{SessionHistoryViewModel.FmtDur(lo)} ~ {SessionHistoryViewModel.FmtDur(hi)}";
        var head = new HistoryRow(top.E, s) { IsBundle = true, Members = members, BundleKey = key };
        head._bundleRange = $"{first.StartClock} ~ {top.StartClock}";
        head.StatLine = $"각 {each} · {(top.E.IsMcVideo ? "송출" : "발언")} 0회";
        head.RangeText = $"{first.E.StartTime ?? first.E.AxisTime:HH:mm:ss} ~ {top.E.EndTime ?? top.E.AxisTime:HH:mm:ss} · 빈 세션 {members.Count}건 — 고르면 가장 최근 세션";
        return head;
    }

    // 통화 카드·상세(§4.6 — 무전과 같은 짜임: 왼쪽 두 줄 카드 + 오른쪽 상세). 결과 = 응답/통화 중/부재/실패/거절/취소/오류,
    // 톤 = 태그 색(neutral 회색 · talk 초록 · warn 주황 · bad 빨강) — 관제 «기록»·채널 카드와 같은 상태색
    public string ResultText { get; } = "";
    public string ResultTone { get; } = "neutral";
    public string CallSub { get; } = "";
    public string RingText { get; } = "—";
    /// <summary>진행 막대 — 울림(호출~응답) : 통화(응답~종료) 비율. 응답이 없으면 울림만.</summary>
    public GridLength RingStar { get; } = new(1, GridUnitType.Star);
    public GridLength TalkStar { get; } = new(0, GridUnitType.Star);
    public string CallerInitial { get; } = "";
    public string CalleeInitial { get; } = "";
    /// <summary>이름이 있으면 그 아래 둘째 줄에 번호 — 이름이 없으면(번호가 곧 표시) 비운다.</summary>
    public string CallerNumber { get; } = "";
    public string CalleeNumber { get; } = "";
    public bool HasCallerNumber => CallerNumber.Length > 0;
    public bool HasCalleeNumber => CalleeNumber.Length > 0;
    public string DayClock => E.AxisTime.ToString("yyyy-MM-dd HH:mm:ss");

    private static string Clock(DateTime? t) => t is { } d ? d.ToString("HH:mm:ss") : "—";

    public HistoryRow(HistoryEntry e, DispatchSession s)
    {
        E = e;
        KindText = e.Kind == HistoryKind.Ptt ? "PTT" : "통화";
        string who(string u) => SessionHistoryViewModel.Who(s, u);
        IsLive = e.State is "active" or "ringing" || e.Event == "ptt.session.start";
        if (e.Kind == HistoryKind.Ptt)
        {
            string g = s.Groups.FirstOrDefault(x => string.Equals(x.Uri, e.Group, StringComparison.OrdinalIgnoreCase))?.Name
                       ?? (e.GroupName.Length > 0 ? e.GroupName : UserPartConverter.UserPart(e.Group));
            var peers = e.People.Where(p => !SessionHistoryViewModel.SameUser(p, e.From)).ToList();
            SessionKindText = e.SessionKind switch { "private" => "1:1", "adhoc" => "애드혹", "" or "group" => "그룹", _ => "미상" };
            TargetText = e.SessionKind switch
            {
                "private" => peers.Count > 0 ? $"{who(e.From)} ↔ {string.Join(", ", peers.Select(who))}" : who(e.From),
                "adhoc" => $"{who(e.From)} 외 {peers.Count}명",
                _ => g,
            };
            Parties = e.From.Length > 0 ? $"{TargetText} · 개시 {who(e.From)}" : TargetText;
            var st = e.StartTime ?? e.Time;
            int dur = e.EndTime is { } et && e.StartTime is { } s0 ? Math.Max(0, (int)(et - s0).TotalSeconds) : e.DurationSec;
            DurSec = dur;
            RangeText = $"{st:HH:mm:ss} ~ {(IsLive ? "진행중" : Clock(e.EndTime))}" + (dur > 0 ? $" · {SessionHistoryViewModel.FmtDur(dur)}" : "");
            DurationText = dur > 0 ? SessionHistoryViewModel.FmtDur(dur) : "";
            GroupIdText = e.SessionKind is "" or "group" ? UserPartConverter.UserPart(e.Group) : "";
            InitiatorLabel = who(e.From);
            StateText = IsLive ? "진행중" : "종료";
            FloorPolicyText = e.FloorPolicy switch { "multi" => $"multi · 최대 {(e.MaxTalkers > 0 ? e.MaxTalkers.ToString() : "?")}명", "dual" => "dual · 2명", _ => "single" };
            int people = PeopleCount;
            WhoLine = string.Join(" · ", new[] { e.From.Length > 0 ? $"개시 {InitiatorLabel}" : "", people > 0 ? $"참여 {people}명" : "" }.Where(x => x.Length > 0));
            string len = IsLive ? "진행 중" : dur > 0 ? SessionHistoryViewModel.FmtDur(dur) : "";
            StatLine = string.Join(" · ", new[] { len, $"{(e.IsMcVideo ? "송출" : "발언")} {e.TurnCount}회",
                                                   e.TotalSpeechMs > 0 ? $"{(e.IsMcVideo ? "보낸 시간" : "말한 시간")} {SpeechText}" : "" }.Where(x => x.Length > 0));
            CallerLabel = CalleeLabel = EndReasonText = "";
        }
        else
        {
            CallerLabel = who(e.From); CalleeLabel = e.To.Length > 0 ? who(e.To) : "—";
            Parties = $"{CallerLabel} → {CalleeLabel}";
            DurationText = e.DurationSec > 0 ? SessionHistoryViewModel.FmtDur(e.DurationSec) : "—";
            string num(string u) { if (u.Length == 0) return ""; string d = s.Directory.DisplayNumber(UserPartConverter.UserPart(u)); return d == who(u) ? "" : d; }
            CallerNumber = num(e.From); CalleeNumber = num(e.To);
            CallerInitial = SessionHistoryViewModel.InitialOf(CallerLabel); CalleeInitial = SessionHistoryViewModel.InitialOf(CalleeLabel);
            (ResultText, ResultTone) = e.State switch
            {
                "active" => ("통화 중", "talk"),
                "ringing" => ("호출 중", "talk"),
                _ when e.AnswerTime is not null => ("응답", "neutral"),
                _ => e.EndReason switch
                {
                    "no_answer" or "timeout" => ("부재", "warn"),
                    "busy" => ("실패", "warn"),
                    "rejected" => ("거절", "bad"),
                    "error" or "incomplete" => ("오류", "bad"),
                    _ => ("취소", "warn"),                                     // 응답 전에 발신자가 끊음
                },
            };
            // 울림 = 호출(INVITE)~응답, 응답이 없으면 호출~종료. 통화 = 응답~종료(진행 중이면 지금까지 — 서버 duration)
            double ringSec = e.InviteTime is { } inv ? Math.Max(0, ((e.AnswerTime ?? e.EndTime ?? inv) - inv).TotalSeconds) : 0;
            double talkSec = e.AnswerTime is { } ans ? Math.Max(e.DurationSec, ((e.EndTime ?? ans) - ans).TotalSeconds) : 0;
            RingText = ringSec >= 1 ? SessionHistoryViewModel.FmtDur((int)Math.Round(ringSec)) : "—";
            RingStar = new GridLength(Math.Max(ringSec, e.AnswerTime is null ? 1 : 0.0001), GridUnitType.Star);
            TalkStar = new GridLength(talkSec, GridUnitType.Star);
            CallSub = ResultTone == "talk" ? (IsLive && e.AnswerTime is not null ? $"통화 중 · 울림 {RingText}" : "호출 중")
                    : e.AnswerTime is not null ? $"{DurationText} · 울림 {RingText}"
                    : $"{SessionHistoryViewModel.EndReasonText(e.EndReason)} · 울림 {RingText}";
            StateText = e.State switch { "active" => "통화중", "ringing" => "호출중", "ended" or "" => "종료", var x => x };
            EndReasonText = SessionHistoryViewModel.EndReasonText(e.EndReason);
            SessionKindText = TargetText = RangeText = GroupIdText = InitiatorLabel = FloorPolicyText = WhoLine = StatLine = "";
        }
        EventText = e.Event switch
        {
            "call.answered" => "응답", "call.missed" => "부재", "ptt.session.start" => "진행 중", "ptt.session.end" => "종료",
            _ => e.Event,
        };
    }
}

/// <summary>시간대 묶음 머리의 건수 — 행 수가 아니라 세션 수(묶음 머리 = 묶은 수, 펼친 한 줄 = 0). [0] = 그룹 Items, [1] = ItemCount(바뀜 알림용).</summary>
public sealed class HourGroupCountConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type t, object? p, CultureInfo c) =>
        values.Length > 0 && values[0] is System.Collections.IEnumerable items ? $"{items.Cast<object>().Sum(x => x is HistoryRow r ? r.Weight : 1)}건" : "";
    public object[] ConvertBack(object v, Type[] t, object? p, CultureInfo c) => throw new NotSupportedException();
}

/// <summary>선택 세션의 참여자 한 줄 — 입퇴장 기록 ∪ 화자(녹취 턴). 발언 통계는 턴에서 센다(참가만 한 사람은 0).</summary>
public sealed record ParticipantRow(string Id, string Label, bool IsInitiator, string RangeText, int Turns, string SpeechText, string Color, bool HasSpoken)
{
    /// <summary>세는 말 — 무전 "발언" · 영상 세션 "송출".</summary>
    public string Word { get; init; } = "발언";
    /// <summary>이름표 머리글자 — 이름이면 첫 글자, 번호면 비운다(관제 «기록» 아바타와 같은 규칙).</summary>
    public string Initial => SessionHistoryViewModel.InitialOf(Label);
    public string Tip => (RangeText.Length > 0 ? $"{RangeText} · " : "") + $"{Word} {Turns}회 · {(Word == "송출" ? "보낸 시간" : "말한 시간")} {SpeechText}";
}

/// <summary>발언 턴 막대 — 한 화자가 한 슬롯을 점유한 구간. Multi = 세그먼트에 턴이 여럿(동시 발언·슬롯 재사용) → 단독 트랙(slot) 재생.</summary>
public sealed record TurnBar(int Seq, int Slot, bool Multi, string Speaker, string Color, double LeftRatio, double WidthRatio, string Tooltip, bool Playable);

public sealed record SpeakerLane(string Speaker, string Label, string Color, IReadOnlyList<TurnBar> Bars);

/// <summary>이벤트 타임라인 한 줄 — floor 중재(TS 24.380 op) 또는 입퇴장(events.jsonl). Detail 은 op 별 부가 정보(사유·대기 순번·회수 유예).</summary>
public sealed record TimelineItem(DateTime Ts, bool IsFloor, string Color, string Who, string Text, string Detail)
{
    public string TimeText => Ts.ToString("HH:mm:ss");
    public bool HasWho => Who.Length > 0;
    public bool HasDetail => Detail.Length > 0;
}

public sealed record MetricItem(string Key, string Value, string Suffix, string Hint);

/// <summary>무전 녹취 재생 바의 발언 막대 — 바 폭 대비 위치·폭(0~1), 화자 색, 줄(겹치면 0/1 두 줄).</summary>
public sealed record PlayerBlock(double LeftRatio, double WidthRatio, string Color, int Row, string Tip);

/// <summary>재생기에 파일을 열라는 요청 — 파일 안 오프셋(초)에서 시작, Play = 열고 바로 재생(아니면 그 자리에 멈춘 채).</summary>
public sealed record MediaOpenArgs(string Path, double OffsetSec, bool Play);

/// <summary>발언 타임라인 눈금 — 트랙 폭 대비 위치(0~1)와 시각 표기.</summary>
public sealed record AxisTick(double Ratio, string Label);

/// <summary>발언 타임라인의 줄인 틈 — 발언이 없어 짧게 접은 구간(트랙 폭 대비 위치·폭)과 실제 길이.</summary>
public sealed record AxisGap(double LeftRatio, double WidthRatio, string Tip);

public sealed partial class SessionHistoryViewModel : ObservableObject
{
    private readonly DispatchSession _s;
    private CancellationTokenSource? _playCts;
    private int _detailSeq;

    public SessionHistoryViewModel(DispatchSession s) { _s = s; }

    public IReadOnlyList<string> Kinds { get; } = new[] { "통화", "무전" };

    /// <summary>0 = 통화, 1 = 무전. 처음은 무전 — 관제 화면의 [무전|통화] 와 같은 순서·같은 첫 모드.</summary>
    [ObservableProperty] private int _kindIndex = 1;
    [ObservableProperty] private DateTime _date = DateTime.Today;
    [ObservableProperty] private string _search = "";
    [ObservableProperty] private bool _busy;
    [ObservableProperty] private string _error = "";
    [ObservableProperty] private string _summary = "";
    [ObservableProperty] private HistoryRow? _selected;
    [ObservableProperty] private RecordingInfo? _recording;
    [ObservableProperty] private RecordingSegment? _selectedSegment;
    [ObservableProperty] private string _recordingStatus = "";
    [ObservableProperty] private string _mediaSource = "";
    [ObservableProperty] private bool _loadingAudio;
    /// <summary>지금 트는 녹취에 영상이 있다 — 영상 칸(재생기)이 열린다. 영상 통화 녹취·MCVideo 송출 구간.</summary>
    [ObservableProperty] private bool _playingHasVideo;
    /// <summary>무전 목록의 서비스 거르기 — all | ptt(음성 무전) | mcvideo(영상). 서버가 서비스 축을 실어 줄 때만 칩이 보인다.</summary>
    [ObservableProperty] private string _serviceFilter = "all";
    /// <summary>무전 목록의 빈 세션 묶기(기본 켬) — 연달아 나오는 같은 그룹·같은 개시자의 빈 세션(HistoryRow.IsSilent)을 한 장으로. 끄면 한 장씩.</summary>
    [ObservableProperty] private bool _groupSilent = true;
    /// <summary>시간대 필터("" = 전체). 밴드 칸 클릭으로 토글, 날짜·종류가 바뀌면 해제.</summary>
    [ObservableProperty] private string _selectedHour = "";
    [ObservableProperty] private bool _detailLoading;
    [ObservableProperty] private string _detailStatus = "";
    [ObservableProperty] private bool _showFloorLayer = true;
    [ObservableProperty] private bool _showMemberLayer = true;
    [ObservableProperty] private string _axisStartText = "";
    [ObservableProperty] private string _axisEndText = "";
    /// <summary>발언 타임라인 확대 배율(1 = 세션 전체가 트랙 폭). 콘솔 LaneTimebar 와 같은 ×1.25 단계, 상한 64.</summary>
    [ObservableProperty] private double _talkZoom = 1.0;
    /// <summary>발언 타임라인의 «틈 줄임» — 발언(송출)이 없는 구간을 짧게 접어 발언 구간이 넓게 보이게 한다(기본 켬). 끄면 시간에 비례한 축.</summary>
    [ObservableProperty] private bool _compactGaps = true;
    private DateTime _axisT0;
    private double _axisSpanMs = 1000;
    // 시간축 사상 — 실제 구간(세션 시작 기준 ms) → 표시 구간. 틈 줄임이 꺼져 있으면 한 구간(선형)이다
    private readonly List<(double R0, double R1, double D0, double D1, bool Gap)> _axisMap = new();
    private double _axisDispMs = 1000;
    private HistoryRow? _paneRow; private PttSessionDetail? _paneDetail;

    private IReadOnlyList<HistoryEntry> _all = Array.Empty<HistoryEntry>();
    private bool _suppressQuery;                                          // 표본 심기 중 종류 전환이 서버 조회를 부르지 않게
    private IReadOnlyDictionary<string, int> _hours = new Dictionary<string, int>();
    private readonly List<TimelineItem> _timelineAll = new();

    public ObservableCollection<HistoryRow> Rows { get; } = new();
    public ObservableCollection<HourCell> Hours { get; } = new();
    public ObservableCollection<RecordingSegment> Segments { get; } = new();
    public ObservableCollection<ParticipantRow> Participants { get; } = new();
    public ObservableCollection<SpeakerLane> Lanes { get; } = new();
    public ObservableCollection<TimelineItem> Timeline { get; } = new();
    public ObservableCollection<MetricItem> Metrics { get; } = new();
    public ObservableCollection<AxisTick> AxisTicks { get; } = new();
    public ObservableCollection<AxisGap> AxisGaps { get; } = new();
    public string TalkZoomText => TalkZoom > 1.001 ? $"×{TalkZoom:0.#}" : "";
    public bool IsTalkZoomed => TalkZoom > 1.001;
    public const double TalkZoomMax = 64;

    public bool HasError => Error.Length > 0;
    public bool HasRecording => Recording is not null && Segments.Count > 0;
    public bool CanPlay => _segs.Count > 0 && !LoadingAudio;
    public bool IsPlaying => MediaSource.Length > 0;
    public bool IsPtt => KindIndex == 1;
    public bool IsCall => KindIndex != 1;
    public bool HasSelection => Selected is not null;
    /// <summary>받은 무전 항목에 서비스 축이 실려 있다 — [전체|무전|영상] 칩을 보인다.</summary>
    public bool HasServiceAxis => IsPtt && _all.Any(e => e.Service.Length > 0);
    public bool ShowVideo => IsPlaying && PlayingHasVideo;
    /// <summary>고른 세션이 MCVideo 영상 세션 — 이력 항목의 service, 없으면 녹취 메타(service·세그먼트 type)로 안다.</summary>
    public bool IsVideoSession => Selected?.IsMcVideo == true || (IsPtt && Recording?.IsMcVideo == true);
    /// <summary>세션 패널의 낱말 — 무전 "발언" / 영상 세션 "송출"(TS 24.581 전송 제어 — 발언권이 아니라 송출 허가).</summary>
    public string TurnWord => IsVideoSession ? "송출" : "발언";
    public string TurnsHint => IsVideoSession ? "  막대를 누르면 그 송출 재생(영상) · Ctrl+휠 확대" : "  막대를 누르면 그 발언 재생 · Ctrl+휠 확대";
    public string NoTurnsText => IsVideoSession ? "이 세션에는 녹취된 송출이 없습니다" : "이 세션에는 녹취된 발언이 없습니다";
    public bool HasLanes => Lanes.Count > 0;
    public bool HasAxisGaps => AxisGaps.Count > 0;
    public bool HasTimeline => Timeline.Count > 0;
    public bool HasParticipants => Participants.Count > 0;
    public int FloorCount { get; private set; }
    public int MemberEventCount { get; private set; }
    public bool HasHourFilter => SelectedHour.Length > 0;
    /// <summary>무전 시간대 밴드가 세는 것 — 0 = 성립한 세션 수(서버 hours), 1 = 발언 수(세션별 발언 턴의 합, 세션 시작 시간대에 넣는다).</summary>
    [ObservableProperty] private int _bandMode;
    public bool BandShowsTurns => IsPtt && BandMode == 1;
    /// <summary>시간대 밴드 제목 — 무엇을 센 건지(통화 = 통화 시도 INVITE, PTT = 무전 세션 시작 / 발언).</summary>
    /// <summary>밴드 제목 — 무전은 [세션 수 | 발언 수] 전환이 제목 바로 뒤라 제목 글자를 고정한다(전환을 눌러도 전환 자리가 움직이지 않게).</summary>
    public string BandTitle => !IsPtt ? "시간대별 통화 횟수" : "시간대별 무전";
    /// <summary>밴드 칸 클릭의 뜻 — [발언 수] 에서는 그 시간의 발언 있는 세션만.</summary>
    public string BandHint => BandShowsTurns ? "칸을 누르면 그 시간의 발언 있는 세션만 · 다시 누르면 전체" : "칸을 누르면 그 시간만 · 다시 누르면 전체";
    /// <summary>좌 목록 : 우 패널 폭 — 통화·무전 모두 상세 패널이 주역이라 1 : 3(카드 목록은 요약이면 충분하다).
    /// 화면은 이 값을 기본 폭으로 놓고 사용자가 경계를 끌어 바꿀 수 있다(더블클릭 = 기본 폭).</summary>
    public GridLength ListWidth => new GridLength(1, GridUnitType.Star);
    public GridLength PaneWidth => new GridLength(3, GridUnitType.Star);
    /// <summary>고른 행에 녹취가 있는가 — 통화 표 아래 녹취 띠의 표시 조건(세그먼트 로딩 중에도 띠는 보인다).</summary>
    public bool SelectedHasRecording => Selected?.HasRecording == true;


    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSearchChanged(string value) => Filter();
    partial void OnKindIndexChanged(int value)
    {
        OnPropertyChanged(nameof(IsPtt)); OnPropertyChanged(nameof(IsCall)); OnPropertyChanged(nameof(PaneWidth)); OnPropertyChanged(nameof(BandTitle)); OnPropertyChanged(nameof(BandShowsTurns));
        OnPropertyChanged(nameof(HasServiceAxis)); OnPropertyChanged(nameof(BandHint));
        SelectedHour = ""; if (!_suppressQuery) _ = QueryAsync();
    }
    partial void OnDateChanged(DateTime value) { SelectedHour = ""; _ = QueryAsync(); }
    partial void OnSelectedChanged(HistoryRow? value)
    {
        OnPropertyChanged(nameof(HasSelection)); OnPropertyChanged(nameof(SelectedHasRecording)); RaiseVideoSessionChanged();
        if (!_rebuilding) _ = LoadSelectionAsync(value);                  // 목록을 다시 채우는 동안(Filter)의 선택 흔들림은 패널을 다시 읽지 않는다
    }
    partial void OnSelectedSegmentChanged(RecordingSegment? value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnLoadingAudioChanged(bool value) => OnPropertyChanged(nameof(CanPlay));
    partial void OnMediaSourceChanged(string value) { OnPropertyChanged(nameof(IsPlaying)); OnPropertyChanged(nameof(ShowVideo)); }
    partial void OnPlayingHasVideoChanged(bool value) => OnPropertyChanged(nameof(ShowVideo));
    partial void OnServiceFilterChanged(string value) => Filter();
    partial void OnGroupSilentChanged(bool value) => Filter();
    /// <summary>묶음 머리의 [n건 펼치기]/[접기] — 그 묶음만 펼친다(펼친 묶음은 날짜를 다시 조회해도 열쇠가 같으면 펼친 채).</summary>
    [RelayCommand] private void ToggleBundle(HistoryRow? row)
    {
        if (row is not { IsBundle: true }) return;
        if (!_openBundles.Remove(row.BundleKey)) _openBundles.Add(row.BundleKey);
        Filter();
    }
    /// <summary>--ui-preview-bundles=open — 표본의 빈 세션 묶음을 전부 펼친다.</summary>
    public void PreviewOpenBundles() { foreach (var r in Rows.Where(r => r.IsBundle).ToList()) _openBundles.Add(r.BundleKey); Filter(); }
    partial void OnRecordingChanged(RecordingInfo? value) { OnPropertyChanged(nameof(HasRecording)); RaiseVideoSessionChanged(); }
    private void RaiseVideoSessionChanged()
    {
        OnPropertyChanged(nameof(IsVideoSession)); OnPropertyChanged(nameof(TurnWord)); OnPropertyChanged(nameof(TurnsHint)); OnPropertyChanged(nameof(NoTurnsText));
    }
    [RelayCommand] private void SetServiceFilter(string value) => ServiceFilter = value;
    partial void OnSelectedHourChanged(string value) { OnPropertyChanged(nameof(HasHourFilter)); foreach (var c in Hours) c.IsSelected = c.Hour == value; Filter(); }
    partial void OnTalkZoomChanged(double value) { OnPropertyChanged(nameof(TalkZoomText)); OnPropertyChanged(nameof(IsTalkZoomed)); RebuildAxisTicks(); }
    partial void OnBandModeChanged(int value)
    {
        OnPropertyChanged(nameof(BandTitle)); OnPropertyChanged(nameof(BandShowsTurns)); OnPropertyChanged(nameof(BandHint)); RebuildHours();
        if (SelectedHour.Length > 0) Filter();                            // 고른 시간대의 거르기 뜻이 바뀐다(전체 ↔ 발언 있는 세션만)
    }
    /// <summary>틈 줄임을 켜고 끄면 고른 세션의 패널을 같은 자료로 다시 그린다(막대 위치·눈금이 축에 달려 있다).</summary>
    partial void OnCompactGapsChanged(bool value)
    {
        if (_paneRow is null || !ReferenceEquals(_paneRow, Selected)) return;
        Participants.Clear(); Lanes.Clear(); Timeline.Clear(); Metrics.Clear(); _timelineAll.Clear();
        BuildPttPane(_paneRow, _paneDetail);
    }
    partial void OnShowFloorLayerChanged(bool value) => ApplyTimelineLayers();
    partial void OnShowMemberLayerChanged(bool value) => ApplyTimelineLayers();

    private HistoryKind Kind => KindIndex == 1 ? HistoryKind.Ptt : HistoryKind.Call;

    // ── 조회 ──

    /// <summary>날짜 창 이동 — -1 전날 · +1 다음 날 · 0 오늘(§4.6). 값이 바뀌면 OnDateChanged 가 조회한다.</summary>
    [RelayCommand] private void ShiftDate(string delta)
    {
        int d = int.TryParse(delta, out var n) ? n : 0;
        var next = d == 0 ? DateTime.Today : Date.Date.AddDays(d);
        if (next > DateTime.Today) next = DateTime.Today;
        if (next != Date.Date) Date = next;
    }

    [RelayCommand] public async Task QueryAsync()
    {
        var m = _s.Management; if (m is null) { Error = "로그인 전"; return; }
        Busy = true; Error = "";
        var from = Date.Date; var to = Date.Date.AddDays(1).AddSeconds(-1);
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var r = await m.QueryHistoryAsync(Kind, from, to);
        long queryMs = sw.ElapsedMilliseconds;
        Busy = false;
        if (!r.Ok)
        {
            Error = ResponseText.Describe(ResponseText.Area.Management, r.Code, r.Reason);
            _s.Log.Warn($"history window {HistoryClient.KindName(Kind)} {from:yyyy-MM-dd}: {r.Code} {r.Reason}");
            _all = Array.Empty<HistoryEntry>(); _hours = new Dictionary<string, int>(); ApplyLoaded(); return;
        }
        _all = r.Value.Items.OrderByDescending(e => e.Time).ToList();      // 표시는 최근이 위
        _hours = r.Value.Hours.Count > 0 ? r.Value.Hours : CountHours(_all);
        // 받은 것의 구성을 남긴다 — "콘솔에는 있는데 앱에는 없다" 를 서버 응답에서 가른다(영상 통화 = callType volte_video · 영상 세션 = service mcvideo)
        string what = $"history window {HistoryClient.KindName(Kind)} {from:yyyy-MM-dd}: {_all.Count} items"
                      + (Kind == HistoryKind.Call ? $" (video {_all.Count(e => e.CallType == "volte_video")})"
                                                  : $" (mcvideo {_all.Count(e => e.IsMcVideo)}, service axis {(_all.Any(e => e.Service.Length > 0) ? "yes" : "no")})");
        sw.Restart();
        ApplyLoaded();
        LogTimings(what, queryMs, sw);
    }

    /// <summary>창 조회 한 번의 시간을 로그 한 줄로 — 서버 응답(query) · 목록 만들기(list) · 화면 그리기(render, 다음 유휴 때까지).
    /// 조회가 느릴 때 서버 탓인지 화면 탓인지를 가른다.</summary>
    private void LogTimings(string what, long queryMs, System.Diagnostics.Stopwatch sinceList)
    {
        long listMs = sinceList.ElapsedMilliseconds;
        if (Application.Current?.Dispatcher is not { } d) { _s.Log.Info($"{what} · query {queryMs} ms · list {listMs} ms"); return; }
        d.BeginInvoke(System.Windows.Threading.DispatcherPriority.ApplicationIdle,
                      () => _s.Log.Info($"{what} · query {queryMs} ms · list {listMs} ms · render {sinceList.ElapsedMilliseconds - listMs} ms"));
    }

    /// <summary>받은 항목을 화면에 — 서비스 축이 없으면 거르기를 풀고(칩이 사라진다) 밴드·목록을 다시 만든다.</summary>
    private void ApplyLoaded()
    {
        OnPropertyChanged(nameof(HasServiceAxis));
        if (!HasServiceAxis && ServiceFilter != "all") { ServiceFilter = "all"; RebuildHours(); return; }   // 값이 바뀌면 Filter 는 OnServiceFilterChanged 가 돈다
        RebuildHours();
        Filter();
    }

    private static Dictionary<string, int> CountHours(IEnumerable<HistoryEntry> items)
    {
        var d = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var e in items) { string h = e.AxisTime.ToString("HH"); d[h] = d.TryGetValue(h, out int n) ? n + 1 : 1; }
        return d;
    }

    private void RebuildHours()
    {
        Hours.Clear();
        if (BandShowsTurns)
        {
            // 발언 수 — 받은 세션들의 발언 턴을 세션 시작 시간대(밴드·목록 묶음과 같은 AxisTime 축)에 더한다. 서버 hours 는 limit 절삭 전 세션 수라,
            // 받은 세션이 그보다 적은 시간대(하루 상한을 넘은 날의 이른 시간)는 덜 센 값이다 → 칸에 "+" 와 툴팁.
            var turns = new Dictionary<string, int>(StringComparer.Ordinal);
            var loaded = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (var e in _all)
            {
                string h = e.AxisTime.ToString("HH");
                turns[h] = (turns.TryGetValue(h, out int t) ? t : 0) + e.TurnCount;
                loaded[h] = (loaded.TryGetValue(h, out int c) ? c : 0) + 1;
            }
            int tmax = turns.Count > 0 ? turns.Values.Max() : 0;
            for (int h = 0; h < 24; h++)
            {
                string k = h.ToString("00");
                int n = turns.TryGetValue(k, out int tn) ? tn : 0;
                int sessions = _hours.TryGetValue(k, out int sn) ? sn : 0, got = loaded.TryGetValue(k, out int gn) ? gn : 0;
                bool partial = got < sessions;
                string tip = $"{k}시에 시작한 무전 세션 {sessions}건의 발언 {n}회"
                             + (partial ? $" — 목록이 하루 상한을 넘어 {sessions - got}건은 세지 못했습니다" : "");
                Hours.Add(new HourCell(k, n, tmax, tip, partial) { IsSelected = k == SelectedHour });
            }
            return;
        }
        int max = _hours.Count > 0 ? _hours.Values.Max() : 0;
        string unit = IsPtt ? "무전 세션" : "통화";
        for (int h = 0; h < 24; h++)
        {
            string k = h.ToString("00");
            int n = _hours.TryGetValue(k, out int v) ? v : 0;
            Hours.Add(new HourCell(k, n, max, $"{k}시에 시작한 {unit} {n}건") { IsSelected = k == SelectedHour });
        }
    }

    /// <summary>밴드 칸 클릭 — 그 시간대만(다시 누르면 해제). 건수 0 인 칸은 무시.</summary>
    [RelayCommand] private void SelectHour(string hour)
    {
        int n = BandShowsTurns ? Hours.FirstOrDefault(c => c.Hour == hour)?.Count ?? 0 : _hours.TryGetValue(hour, out int v) ? v : 0;
        if (n == 0) { if (SelectedHour == hour) SelectedHour = ""; return; }
        SelectedHour = SelectedHour == hour ? "" : hour;
    }

    // 목록 행 재사용 — 같은 항목이면 같은 행 객체(거르기·펼치기를 바꿔도 고른 행이 그대로 남는다). 받은 목록(_all)이 바뀌면 비운다
    private readonly Dictionary<HistoryEntry, HistoryRow> _rowCache = new(ReferenceEqualityComparer.Instance);
    private readonly Dictionary<string, HistoryRow> _bundleCache = new(StringComparer.Ordinal);
    private readonly HashSet<string> _openBundles = new(StringComparer.Ordinal);
    private IReadOnlyList<HistoryEntry>? _cacheOf;
    private bool _rebuilding;

    private void Filter()
    {
        if (!ReferenceEquals(_cacheOf, _all)) { _rowCache.Clear(); _bundleCache.Clear(); _cacheOf = _all; }
        string q = Search.Trim();
        string qn = DirectoryService.Normalize(q);
        bool speechOnly = BandShowsTurns && SelectedHour.Length > 0;     // [발언 수] 칸을 눌렀다 — 그 시간의 발언 있는 세션만
        var shown = new List<HistoryRow>();
        foreach (var e in _all)
        {
            if (SelectedHour.Length > 0 && e.AxisTime.ToString("HH") != SelectedHour) continue;
            if (IsPtt && ServiceFilter != "all" && (ServiceFilter == "mcvideo") != e.IsMcVideo) continue;
            if (speechOnly && e.TurnCount == 0 && !e.HasRecording) continue;
            if (!_rowCache.TryGetValue(e, out var row)) _rowCache[e] = row = new HistoryRow(e, _s);
            if (q.Length > 0 && !row.Parties.Contains(q, StringComparison.OrdinalIgnoreCase)
                && !(qn.Length > 0 && (DirectoryService.Normalize(e.From).Contains(qn) || DirectoryService.Normalize(e.To).Contains(qn)
                                       || e.People.Any(p => DirectoryService.Normalize(p).Contains(qn))))) continue;
            shown.Add(row);
        }

        // 목록 채우기 — 빈 세션 묶기가 켜져 있으면 연달아 나오는 같은 그룹·같은 개시자의 빈 세션 2건 이상을 머리 한 장(+ 펼치면 한 줄씩)으로
        var keep = Selected;
        int silent = 0, bundles = 0;
        _rebuilding = true;
        Rows.Clear();
        for (int i = 0; i < shown.Count; i++)
        {
            var row = shown[i];
            if (row.IsSilent) silent++;
            int j = i;
            if (IsPtt && GroupSilent && row.IsSilent)
                while (j + 1 < shown.Count && SameSilentRun(row, shown[j + 1])) j++;
            if (j == i) { row.IsBundleMember = false; Rows.Add(row); continue; }
            var members = shown.GetRange(i, j - i + 1);
            silent += members.Count - 1;
            string key = $"{row.HourKey}|{members[^1].E.Id}";
            string ck = $"{key}|{row.E.Id}|{members.Count}";
            if (!_bundleCache.TryGetValue(ck, out var head)) _bundleCache[ck] = head = HistoryRow.Bundle(members, _s, key);
            bool open = _openBundles.Contains(key);
            head.BundleExpanded = open;
            Rows.Add(head); bundles++;
            foreach (var m in members) { m.IsBundleMember = true; if (open) Rows.Add(m); }
            i = j;
        }
        _rebuilding = false;

        int live = shown.Count(x => x.IsLive);
        Summary = $"{Date:yyyy-MM-dd}{(SelectedHour.Length > 0 ? $" {SelectedHour}시" : "")}{(speechOnly ? " 발언 있는 세션" : "")} · {shown.Count}건"
                  + (live > 0 ? $" · 진행중 {live}" : "")
                  + (shown.Count(x => x.HasRecording) is > 0 and var n ? $" · 녹취 {n}건" : "")
                  + (IsPtt && shown.Count(x => x.IsMcVideo) is > 0 and var nv ? $" · 영상 {nv}건" : "")
                  + (IsPtt && shown.Where(x => !x.IsMcVideo).Sum(x => x.E.TotalSpeechMs) is > 0 and var sp ? $" · 발화 합 {FmtSpeech(sp)}" : "")
                  + (IsPtt && silent > 0 ? $" · 빈 세션 {silent}건{(bundles > 0 ? $" → {bundles}묶음" : "")}" : "");

        // 고른 행을 되짚는다 — 목록을 다시 채우면 ListBox 가 선택을 놓는다. 그대로 있으면 다시 짚고, 묶음에 접혀 들어갔으면 그 묶음 머리를 고른다
        if (keep is null) return;
        HistoryRow? again = Rows.Contains(keep) ? keep
            : Rows.FirstOrDefault(r => ReferenceEquals(r.E, keep.E))
              ?? Rows.FirstOrDefault(r => r.IsBundle && r.Members.Any(m => ReferenceEquals(m.E, keep.E)));
        if (again is null) Selected = null;
        else if (ReferenceEquals(Selected, again)) OnPropertyChanged(nameof(Selected));
        else { _rebuilding = ReferenceEquals(again.E, keep.E); Selected = again; _rebuilding = false; }   // 같은 세션이면 패널을 다시 읽지 않는다
    }

    /// <summary>빈 세션 묶음에 이어 붙는가 — 같은 시간대 · 같은 서비스 · 같은 그룹 · 같은 개시자의 빈 세션.</summary>
    private static bool SameSilentRun(HistoryRow a, HistoryRow b) =>
        b.IsSilent && b.HourKey == a.HourKey && b.E.IsMcVideo == a.E.IsMcVideo
        && string.Equals(b.E.Group, a.E.Group, StringComparison.OrdinalIgnoreCase) && SameUser(b.E.From, a.E.From);

    // ── 선택 세션 ──

    private async Task LoadSelectionAsync(HistoryRow? row)
    {
        int seq = ++_detailSeq;
        ResetPlayer();
        Recording = null; Segments.Clear(); RecordingStatus = "";
        Participants.Clear(); Lanes.Clear(); Timeline.Clear(); Metrics.Clear(); _timelineAll.Clear(); AxisGaps.Clear();
        _paneRow = null; _paneDetail = null;
        FloorCount = MemberEventCount = 0; AxisStartText = AxisEndText = ""; DetailStatus = "";
        RaiseDetailChanged();
        if (row is null) return;
        if (_previewSegments is not null)                                 // --ui-preview 표본: 서버 없이 패널만 그린다
        {
            if (row.HasRecording)
            {
                var segs = row.IsMcVideo && _previewVideoSegments is not null ? _previewVideoSegments : _previewSegments;
                foreach (var s in segs) Segments.Add(s);
                Recording = new RecordingInfo(row.E.RecordingId, row.E.Kind == HistoryKind.Ptt ? "ptt" : "volte", row.E.From, row.E.To, row.E.Group, row.E.StartTime, row.E.EndTime, row.E.DurationSec, "ready", segs)
                { Service = row.E.Kind == HistoryKind.Ptt ? (row.IsMcVideo ? "mcvideo" : "ptt") : "" };
                RecordingStatus = $"세그먼트 {Segments.Count}개 · ready (표본)"; SelectedSegment = Segments.FirstOrDefault();
            }
            else RecordingStatus = row.IsLive ? "진행 중 — 끝나면 녹취가 잡힙니다" : "녹취 없음";
            if (row.E.Kind == HistoryKind.Ptt) BuildPttPane(row, _previewDetail is { } pd && pd.RecordingId == row.E.RecordingId ? pd : null);
            SetupBar(row);
            return;
        }
        var m = _s.Management; if (m is null) return;

        Task<Result<PttSessionDetail>>? detailTask = null;
        Task<Result<RecordingInfo>>? recTask = null;
        if (row.E.Kind == HistoryKind.Ptt && row.E.RecordingId.Length > 0)
        {
            DetailLoading = true; DetailStatus = "세션 상세 조회 중…";
            detailTask = m.GetPttSessionDetailAsync(row.E.RecordingId);
        }
        if (row.E.RecordingId.Length > 0 && row.HasRecording) { RecordingStatus = "녹취 정보 조회 중…"; recTask = m.GetRecordingAsync(row.E.RecordingId); }
        else RecordingStatus = row.IsLive ? "진행 중 — 끝나면 녹취가 잡힙니다" : "녹취 없음";

        PttSessionDetail? detail = null;
        if (detailTask is not null)
        {
            var d = await detailTask;
            if (seq != _detailSeq) return;
            if (d.Ok) { detail = d.Value; DetailStatus = ""; }
            else DetailStatus = ResponseText.Describe(ResponseText.Area.Management, d.Code, d.Reason);
        }
        if (recTask is not null)
        {
            var r = await recTask;
            if (seq != _detailSeq) return;
            if (!r.Ok) RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason);
            else
            {
                Recording = r.Value;
                foreach (var s in r.Value.Segments) Segments.Add(s);
                OnPropertyChanged(nameof(HasRecording));
                RecordingStatus = Segments.Count > 0 ? $"세그먼트 {Segments.Count}개 · {r.Value.Status}" : "세그먼트 없음(녹음 진행 중이거나 미디어 없음)";
                SelectedSegment = Segments.FirstOrDefault();
            }
        }
        DetailLoading = false;
        if (row.E.Kind == HistoryKind.Ptt) BuildPttPane(row, detail);
        SetupBar(row);
    }

    /// <summary>발언 턴(세그먼트 슬롯 트랙) · 참여자 · 이벤트 타임라인 · 지표를 만든다. 콘솔 PanelDetail 과 같은 구성.</summary>
    private void BuildPttPane(HistoryRow row, PttSessionDetail? detail)
    {
        _paneRow = row; _paneDetail = detail;
        var e = row.E;
        // 발언 턴 — 세그먼트 → 슬롯 트랙의 화자 구간(콘솔 segTurns). 트랙이 없으면 세그먼트 전체가 대표 화자의 한 턴.
        var turns = new List<(int Seq, int Slot, string Spk, DateTime Start, DateTime End, int DurMs, bool Playable, bool Multi)>();
        foreach (var seg in Segments)
        {
            if (seg.Start is not { } b) continue;
            bool playable = seg.Status != "recording";
            // 화자 구간은 음성 트랙에서 — 음성 없이 영상만 있는 송출 구간(MCVideo — 송출 중 무전으로 마이크를 넘긴 동안)은 영상 트랙에서
            var audio = seg.Tracks.Where(t => t.Kind == "audio").ToList();
            if (audio.Count == 0) audio = seg.Tracks.Where(t => t.Kind == "video").ToList();
            var mine = new List<(int, int, string, DateTime, DateTime, int, bool, bool)>();
            if (audio.Count == 0)
                mine.Add((seg.Seq, 0, seg.SpeakerId, b, b.AddMilliseconds(seg.DurationMs), seg.DurationMs, playable, false));
            else
                foreach (var t in audio)
                {
                    var spans = t.Speakers.Count > 0 ? t.Speakers : new[] { new SpeakerSpan(seg.SpeakerId, 0, seg.DurationMs) };
                    foreach (var sp in spans)
                        mine.Add((seg.Seq, t.Slot, sp.Id.Length > 0 ? sp.Id : seg.SpeakerId, b.AddMilliseconds(sp.OffsetMs),
                                  b.AddMilliseconds(sp.OffsetMs + sp.DurMs), sp.DurMs, playable && t.Status != "recording", false));
                }
            bool multi = mine.Count > 1;
            foreach (var t in mine) turns.Add((t.Item1, t.Item2, t.Item3, t.Item4, t.Item5, t.Item6, t.Item7, multi));
        }
        turns.Sort((a, b) => a.Start != b.Start ? a.Start.CompareTo(b.Start) : a.Slot.CompareTo(b.Slot));
        _turnSpans.Clear();
        foreach (var t in turns) _turnSpans.Add((t.Start, t.End, t.Spk, t.Seq));
        var order = new List<string>();
        foreach (var t in turns) if (t.Spk.Length > 0 && !order.Contains(t.Spk)) order.Add(t.Spk);
        string colorOf(string id) { int i = order.IndexOf(id); return SpkColors[(i < 0 ? 0 : i) % SpkColors.Length]; }

        var parts = detail?.Participants ?? Array.Empty<PttParticipant>();
        var events = detail?.Events ?? Array.Empty<PttEvent>();
        var floor = detail?.Floor ?? Array.Empty<PttFloorEvent>();

        // 시간축 = 세션 시작~종료(항목) ∪ 턴·이벤트가 실제 걸친 범위
        var times = new List<DateTime>();
        if (e.StartTime is { } st) times.Add(st);
        if (e.EndTime is { } et) times.Add(et);
        times.AddRange(turns.Select(t => t.Start)); times.AddRange(turns.Select(t => t.End));
        times.AddRange(events.Where(x => x.Ts is not null).Select(x => x.Ts!.Value));
        times.AddRange(floor.Where(x => x.Ts is not null).Select(x => x.Ts!.Value));
        DateTime t0 = times.Count > 0 ? times.Min() : e.Time, t1 = times.Count > 0 ? times.Max() : e.Time;
        double span = Math.Max(1000, (t1 - t0).TotalMilliseconds);
        AxisStartText = t0.ToString("HH:mm:ss"); AxisEndText = t1.ToString("HH:mm:ss");
        _axisT0 = t0; _axisSpanMs = span;
        BuildAxisMap(turns.Select(t => ((t.Start - t0).TotalMilliseconds, (t.End - t0).TotalMilliseconds)), span);
        TalkZoom = 1.0;                                                    // 눈금은 레인을 다 만든 뒤(아래) 다시 센다

        _turnColors.Clear();
        foreach (var spk in order) _turnColors[spk] = colorOf(spk);
        // 화자 레인
        foreach (var spk in order)
        {
            var bars = turns.Where(t => t.Spk == spk).Select(t => new TurnBar(t.Seq, t.Slot, t.Multi, spk, colorOf(spk),
                AxisRatio(t.Start), Math.Max(0.002, AxisRatio(t.End) - AxisRatio(t.Start)),
                $"#{t.Seq}{(t.Multi ? $" 슬롯 {t.Slot}" : "")} · {Who(_s, spk)} · {t.Start:HH:mm:ss} · {FmtSpeech(t.DurMs)}{(t.Playable ? " · 클릭 = 재생" : " · 녹음 중")}",
                t.Playable)).ToList();
            Lanes.Add(new SpeakerLane(spk, Who(_s, spk), colorOf(spk), bars));
        }

        // 참여자 = 입퇴장 기록 ∪ 화자
        var ids = parts.Select(p => p.Msisdn).ToList();
        foreach (var spk in order) if (!ids.Any(x => SameUser(x, spk))) ids.Add(spk);
        foreach (var id in ids)
        {
            var p = parts.FirstOrDefault(x => x.Msisdn == id);
            var mine = turns.Where(t => SameUser(t.Spk, id)).ToList();
            string range = p is { } && (p.Join is not null || p.Leave is not null)
                ? $"{(p.Join is { } j ? j.ToString("HH:mm:ss") : "—")} ~ {(p.Leave is { } l ? l.ToString("HH:mm:ss") : (row.IsLive ? "참여중" : "—"))}" : "";
            bool spoke = order.Any(o => SameUser(o, id));
            Participants.Add(new ParticipantRow(id, Who(_s, id), p?.Role == "initiator" || (p is null && SameUser(id, e.From)), range,
                                                mine.Count, FmtSpeech(mine.Sum(t => t.DurMs)), spoke ? colorOf(order.First(o => SameUser(o, id))) : "", spoke) { Word = TurnWord });
        }

        // 이벤트 타임라인 — floor 중재 + 입퇴장, 시간순
        foreach (var f in floor.Where(x => x.Ts is not null))
            _timelineAll.Add(new TimelineItem(f.Ts!.Value, true, FloorColor(f.Op), f.User.Length > 0 ? Who(_s, f.User) : "", FloorLabel(f.Op), FloorDetail(f)));
        foreach (var ev in events.Where(x => x.Ts is not null))
        {
            var (label, color) = EventDisplay(ev.Type);
            string detailTxt = (ev.Type == "member_join" && ev.Role == "initiator" ? "개시자" : "")
                               + (ev.DurationSec is { } d ? $"{(ev.Role == "initiator" && ev.Type == "member_join" ? " · " : "")}{FmtDur(d)}" : "");
            _timelineAll.Add(new TimelineItem(ev.Ts!.Value, false, color, ev.Member.Length > 0 ? Who(_s, ev.Member) : "", label, detailTxt));
        }
        _timelineAll.Sort((a, b) => a.Ts.CompareTo(b.Ts));
        FloorCount = floor.Count; MemberEventCount = events.Count;
        ApplyTimelineLayers();

        // 지표 — 관제 사람이 먼저 묻는 넷(얼마나·몇 명·몇 번·얼마 동안) + 있을 때만 동시 발언·녹취
        int talkMs = e.TalkMs > 0 ? e.TalkMs : turns.Sum(t => t.DurMs);
        int durSec = e.EndTime is { } de && e.StartTime is { } ds ? Math.Max(0, (int)(de - ds).TotalSeconds) : e.DurationSec;
        Metrics.Add(new MetricItem("길이", row.IsLive ? "진행 중" : FmtDur(durSec), "", "세션 시작~종료"));
        bool mcv = IsVideoSession;
        Metrics.Add(new MetricItem("참여", Participants.Count.ToString(), "명", mcv ? "입퇴장 기록과 영상을 보낸 사람을 합친 수" : "입퇴장 기록과 말한 사람을 합친 수"));
        if (mcv)
        {
            Metrics.Add(new MetricItem("송출", (e.TurnCount > 0 ? e.TurnCount : turns.Count).ToString(), "회", "영상을 보낸 구간 수 — 동시에 보낸 사람은 따로 센다 (TS 24.581 전송 제어)"));
            Metrics.Add(new MetricItem("보낸 시간", FmtSpeech(e.TotalSpeechMs > 0 ? e.TotalSpeechMs : talkMs), "", $"겹친 구간은 한 번으로 센 송출 시간 · 사람별 합 {FmtSpeech(talkMs)}"));
            if (e.MaxConcurrent > 1) Metrics.Add(new MetricItem("최대 동시 송출", e.MaxConcurrent.ToString(), "명", ""));
        }
        else
        {
            Metrics.Add(new MetricItem("발언", (e.TurnCount > 0 ? e.TurnCount : turns.Count).ToString(), "회", "말한 구간 수 — 동시 발언은 사람마다 따로 센다"));
            Metrics.Add(new MetricItem("말한 시간", FmtSpeech(e.TotalSpeechMs), "", $"겹친 구간은 한 번으로 센 무전 점유 시간 · 사람별 합 {FmtSpeech(talkMs)}"));
            if (e.MaxConcurrent > 1) Metrics.Add(new MetricItem("최대 동시 발언", e.MaxConcurrent.ToString(), "명", ""));
        }
        if (Segments.Count > 0) Metrics.Add(new MetricItem("녹취", Segments.Count.ToString(), "개", "녹취 세그먼트 수"));
        if (detail is null && DetailStatus.Length == 0 && e.RecordingId.Length == 0) DetailStatus = "세션 기록 없음";
        RebuildAxisTicks();
        RaiseDetailChanged();
    }

    // ── 발언 타임라인 확대·축소 ──
    [RelayCommand] private void TalkZoomIn() => TalkZoom = Math.Min(TalkZoomMax, TalkZoom * 1.25);
    [RelayCommand] private void TalkZoomOut() => TalkZoom = Math.Max(1.0, TalkZoom / 1.25);
    [RelayCommand] private void TalkZoomReset() => TalkZoom = 1.0;
    /// <summary>배율을 factor 배 — Ctrl+휠(코드비하인드가 커서 기준으로 스크롤 위치를 맞춘다).</summary>
    public void TalkZoomBy(double factor) => TalkZoom = Math.Clamp(TalkZoom * factor, 1.0, TalkZoomMax);

    // ── 발언 타임라인의 시간축 ──

    /// <summary>발언 구간(세션 시작 기준 ms)으로 시간축 사상을 만든다. 틈 줄임이 켜져 있으면 발언이 없는 구간(세션 머리·꼬리 포함)을 상한 길이로 접는다 —
    /// 상한 = 발언 시간 합의 5 %(1.5~15초). 그보다 짧은 틈과 발언 구간은 시간에 비례한 그대로다(막대 길이끼리는 늘 비교된다).</summary>
    private void BuildAxisMap(IEnumerable<(double S, double E)> turns, double span)
    {
        _axisMap.Clear(); AxisGaps.Clear();
        var blocks = new List<(double S, double E)>();
        foreach (var (s, en) in turns.Select(t => (Math.Clamp(t.S, 0, span), Math.Clamp(t.E, 0, span))).Where(t => t.Item2 > t.Item1).OrderBy(t => t.Item1))
        {
            if (blocks.Count > 0 && s <= blocks[^1].E + 300) blocks[^1] = (blocks[^1].S, Math.Max(blocks[^1].E, en));   // 겹치거나 0.3초 안에 이어진 발언은 한 덩어리
            else blocks.Add((s, en));
        }
        if (!CompactGaps || blocks.Count == 0) { _axisMap.Add((0, span, 0, span, false)); _axisDispMs = span; return; }
        double cap = Math.Clamp(blocks.Sum(b => b.E - b.S) * 0.05, 1500, 15_000);
        double cur = 0, disp = 0;
        void gap(double until)
        {
            double g = until - cur;
            if (g <= 0) return;
            double shown = Math.Min(g, cap);
            _axisMap.Add((cur, until, disp, disp + shown, g > cap));
            disp += shown; cur = until;
        }
        foreach (var b in blocks)
        {
            gap(b.S);
            _axisMap.Add((b.S, b.E, disp, disp + (b.E - b.S), false));
            disp += b.E - b.S; cur = b.E;
        }
        gap(span);
        _axisDispMs = Math.Max(1, disp);
        foreach (var m in _axisMap.Where(m => m.Gap))
            AxisGaps.Add(new AxisGap(m.D0 / _axisDispMs, (m.D1 - m.D0) / _axisDispMs,
                                     $"{TurnWord} 없음 {_axisT0.AddMilliseconds(m.R0):HH:mm:ss} ~ {_axisT0.AddMilliseconds(m.R1):HH:mm:ss} · {FmtSpeech((int)(m.R1 - m.R0))} — 줄여서 표시"));
    }

    /// <summary>시각 → 트랙 폭 대비 위치(0~1).</summary>
    private double AxisRatio(DateTime t)
    {
        double ms = Math.Clamp((t - _axisT0).TotalMilliseconds, 0, _axisSpanMs);
        foreach (var m in _axisMap)
            if (ms <= m.R1) return (m.D0 + (m.R1 > m.R0 ? (ms - m.R0) / (m.R1 - m.R0) * (m.D1 - m.D0) : 0)) / _axisDispMs;
        return 1;
    }

    /// <summary>트랙 폭 대비 위치(0~1) → 시각 — AxisRatio 의 역(줄인 틈 안이면 그 틈의 실제 구간에 비례).</summary>
    private DateTime AxisTimeAt(double ratio)
    {
        double d = Math.Clamp(ratio, 0, 1) * _axisDispMs;
        foreach (var m in _axisMap)
            if (d <= m.D1) return _axisT0.AddMilliseconds(m.R0 + (m.D1 > m.D0 ? (d - m.D0) / (m.D1 - m.D0) * (m.R1 - m.R0) : 0));
        return _axisT0.AddMilliseconds(_axisSpanMs);
    }

    /// <summary>눈금 — 배율에 따라 6~8 개가 보이도록 간격을 1·2·5·10·15·30초·1·2·5·10·30분 중에서 고른다.
    /// 틈을 줄인 축은 시간에 비례하지 않으므로 고른 간격 대신 발언 덩어리가 시작하는 시각을 적는다(너무 붙은 것은 건너뛴다).</summary>
    private void RebuildAxisTicks()
    {
        AxisTicks.Clear();
        if (Lanes.Count == 0 || _axisSpanMs <= 0) return;
        if (_axisMap.Any(m => m.Gap))
        {
            double last = -1, minStep = 0.11 / TalkZoom;
            foreach (var m in _axisMap.Where(m => !m.Gap && m.R1 - m.R0 > 0))
            {
                double r = m.D0 / _axisDispMs;
                bool afterGap = _axisMap.Any(g => g.Gap && Math.Abs(g.R1 - m.R0) < 0.5);
                if (!afterGap && last >= 0) continue;                                   // 덩어리의 첫 눈금만 — 줄인 틈 바로 뒤(또는 맨 처음)
                if (last >= 0 && r - last < minStep) continue;
                AxisTicks.Add(new AxisTick(r, _axisT0.AddMilliseconds(m.R0).ToString("HH:mm:ss")));
                last = r;
            }
            return;
        }
        double targetMs = _axisSpanMs / (6.0 * TalkZoom);
        int[] steps = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
        int stepSec = steps.FirstOrDefault(x => x * 1000.0 >= targetMs);
        if (stepSec == 0) stepSec = steps[^1];
        var first = new DateTime((_axisT0.Ticks / TimeSpan.TicksPerSecond / stepSec + 1) * stepSec * TimeSpan.TicksPerSecond, _axisT0.Kind);
        for (var t = first; (t - _axisT0).TotalMilliseconds < _axisSpanMs && AxisTicks.Count < 400; t = t.AddSeconds(stepSec))
            AxisTicks.Add(new AxisTick((t - _axisT0).TotalMilliseconds / _axisSpanMs, stepSec >= 60 ? t.ToString("HH:mm") : t.ToString("HH:mm:ss")));
    }

    private void ApplyTimelineLayers()
    {
        Timeline.Clear();
        // 같은 초에 같은 입퇴장이 몰리면(그룹 통화 개시·해제) 한 줄로 — "테스트003 외 3명 입장"
        TimelineItem? run = null; int more = 0;
        void flush() { if (run is null) return; Timeline.Add(more > 0 ? run with { Who = $"{run.Who} 외 {more}명" } : run); run = null; more = 0; }
        foreach (var it in _timelineAll)
        {
            if (!(it.IsFloor ? ShowFloorLayer : ShowMemberLayer)) continue;
            if (run is { } r && !it.IsFloor && !r.IsFloor && it.Text == r.Text && it.Detail.Length == 0 && r.Detail.Length == 0 && it.HasWho && r.HasWho
                && it.Ts.ToString("HH:mm:ss") == r.TimeText) { more++; continue; }
            flush(); run = it;
        }
        flush();
        OnPropertyChanged(nameof(HasTimeline));
    }

    private void RaiseDetailChanged()
    {
        OnPropertyChanged(nameof(HasAxisGaps));
        OnPropertyChanged(nameof(HasLanes)); OnPropertyChanged(nameof(HasTimeline)); OnPropertyChanged(nameof(HasParticipants));
        OnPropertyChanged(nameof(FloorCount)); OnPropertyChanged(nameof(MemberEventCount)); OnPropertyChanged(nameof(HasRecording));
    }

    // ── 재생 — 녹취 재생 바(§4.6) ──
    // 막대 하나 = 녹취 전체의 벽시계 구간(통화 = 첫 세그먼트 시작~마지막 끝, 무전 = 세션 시간축 — 발언 레인과 같은 축).
    // 막대의 한 점(벽시계 시각)을 그 시각을 담은 세그먼트 + 파일 안 오프셋으로 풀어 튼다. 세그먼트 사이(말 없는 구간)는 건너뛰기면 다음
    // 세그먼트로 뛰고, 아니면 재생 헤드만 시계로 흘려 다음 세그먼트에서 이어 튼다. 파일은 ManagementClient 가 로컬에 캐시한다(옮겨도 다시 받지 않음).

    private readonly List<(RecordingSegment Seg, DateTime Start, DateTime End)> _segs = new();
    private readonly List<(DateTime Start, DateTime End, string Spk, int Seq)> _turnSpans = new();
    private readonly Dictionary<string, string> _turnColors = new();
    private DateTime _barT0 = DateTime.Today;
    private double _barSpanSec = 1;
    private RecordingSegment? _curSeg;                     // 지금 재생기에 열린 파일의 세그먼트(null = 말 없는 구간·정지)
    private DateTime _head;                                // 재생 헤드(벽시계)
    private int _openSeq;
    private bool _logged;

    /// <summary>사용자 의도 = 재생 중(파일 받는 중·말 없는 구간도 포함). 일시정지 = false.</summary>
    [ObservableProperty] private bool _playing;
    [ObservableProperty] private double _headRatio;
    [ObservableProperty] private string _headPosText = "00:00";
    [ObservableProperty] private string _headWallText = "";
    [ObservableProperty] private string _barLenText = "00:00";
    [ObservableProperty] private string _nowSpeakerText = "";
    [ObservableProperty] private bool _nowSpeaking;
    [ObservableProperty] private bool _skipGaps = true;
    [ObservableProperty] private double _hoverRatio = -1;
    [ObservableProperty] private string _hoverText = "";
    /// <summary>재생 속도 0 = 1× · 1 = 1.5× · 2 = 2×.</summary>
    [ObservableProperty] private int _speedIndex;
    public double Speed => SpeedIndex switch { 1 => 1.5, 2 => 2.0, _ => 1.0 };
    public ObservableCollection<PlayerBlock> PlayerBlocks { get; } = new();
    public bool HasPlayer => _segs.Count > 0;
    public bool HasHover => HoverRatio >= 0;
    public string PlayLabel => Playing ? "일시정지" : "재생";

    /// <summary>재생기 조작 — MediaElement 는 창 코드비하인드가 든다.</summary>
    public event EventHandler<MediaOpenArgs>? OpenRequested;
    public event EventHandler<double>? SeekRequested;
    /// <summary>true = 일시정지, false = 이어 재생.</summary>
    public event EventHandler<bool>? PauseRequested;
    public event EventHandler<double>? SpeedRequested;
    public event EventHandler? StopRequested;

    partial void OnPlayingChanged(bool value) => OnPropertyChanged(nameof(PlayLabel));
    partial void OnHoverRatioChanged(double value) => OnPropertyChanged(nameof(HasHover));
    partial void OnSpeedIndexChanged(int value) { OnPropertyChanged(nameof(Speed)); SpeedRequested?.Invoke(this, Speed); }

    private static string Mmss(double sec) { int t = (int)Math.Round(Math.Max(0, sec)); return $"{t / 60:00}:{t % 60:00}"; }

    private void ResetPlayer()
    {
        StopMedia(); _curSeg = null; Playing = false; _segs.Clear(); _turnSpans.Clear(); _turnColors.Clear(); PlayerBlocks.Clear();
        HoverRatio = -1; _logged = false; OnPropertyChanged(nameof(HasPlayer)); OnPropertyChanged(nameof(CanPlay));
    }

    /// <summary>고른 통화·세션의 막대 구간과 세그먼트 배치를 잡는다(세그먼트·발언 레인을 다 만든 뒤).</summary>
    private void SetupBar(HistoryRow row)
    {
        _segs.Clear(); PlayerBlocks.Clear();
        DateTime cursor = Recording?.Start ?? row.E.AnswerTime ?? row.E.StartTime ?? row.E.Time;
        foreach (var sg in Segments.OrderBy(x => x.Seq))
        {
            var st = sg.Start ?? cursor; var en = st.AddMilliseconds(Math.Max(0, sg.DurationMs));
            _segs.Add((sg, st, en)); cursor = en;
        }
        _segs.Sort((a, b) => a.Start.CompareTo(b.Start));
        if (row.E.Kind == HistoryKind.Ptt && Lanes.Count > 0) { _barT0 = _axisT0; _barSpanSec = Math.Max(1, _axisSpanMs / 1000); }
        else if (_segs.Count > 0) { _barT0 = _segs[0].Start; _barSpanSec = Math.Max(1, (_segs.Max(x => x.End) - _barT0).TotalSeconds); }
        if (row.E.Kind == HistoryKind.Ptt)
        {
            // 발언 막대 — 겹치면(동시 발언) 두 번째 줄
            DateTime lane0End = DateTime.MinValue;
            foreach (var t in _turnSpans)
            {
                int r = t.Start < lane0End ? 1 : 0;
                if (r == 0) lane0End = t.End;
                PlayerBlocks.Add(new PlayerBlock((t.Start - _barT0).TotalSeconds / _barSpanSec, Math.Max(0.004, (t.End - t.Start).TotalSeconds / _barSpanSec),
                    _turnColors.TryGetValue(t.Spk, out var c) ? c : SpkColors[0], r, $"#{t.Seq} {Who(_s, t.Spk)} · {t.Start:HH:mm:ss} · {FmtSpeech((int)(t.End - t.Start).TotalMilliseconds)}"));
            }
        }
        BarLenText = Mmss(_barSpanSec);
        _head = _segs.Count > 0 ? _segs[0].Start : _barT0;
        UpdateHead();
        OnPropertyChanged(nameof(HasPlayer)); OnPropertyChanged(nameof(CanPlay));
    }

    private void UpdateHead()
    {
        double t = (_head - _barT0).TotalSeconds;
        HeadRatio = Math.Clamp(t / _barSpanSec, 0, 1);
        HeadPosText = Mmss(t); HeadWallText = _head.ToString("HH:mm:ss");
        var now = _turnSpans.Where(x => _head >= x.Start && _head < x.End).ToList();
        NowSpeaking = now.Count > 0;
        NowSpeakerText = now.Count > 0 ? $"지금 {string.Join(" + ", now.Select(x => Who(_s, x.Spk)).Distinct())} · 발언 #{now[0].Seq}" : "말 없는 구간";
    }

    /// <summary>막대의 한 점으로 — 그 시각을 담은 세그먼트를 그 오프셋에서 연다(같은 파일이면 위치만 옮긴다). play = null 이면 지금 상태 유지.</summary>
    private void SeekTo(DateTime g, bool? play = null)
    {
        if (_segs.Count == 0) return;
        bool want = play ?? Playing;
        var end = _barT0.AddSeconds(_barSpanSec);
        if (g < _barT0) g = _barT0; if (g > end) g = end;
        var hit = _segs.FirstOrDefault(x => g >= x.Start && g < x.End);
        if (hit.Seg is null && want && SkipGaps && _segs.FirstOrDefault(x => x.Start > g) is { Seg: not null } nx) { g = nx.Start; hit = nx; }
        _head = g; UpdateHead();
        if (hit.Seg is null)                                                   // 말 없는 구간·끝 — 파일을 닫고 시계만(Tick)
        {
            if (_curSeg is not null) { _curSeg = null; StopMedia(); }
            Playing = want && _segs.Any(x => x.Start > g);
            return;
        }
        double off = (g - hit.Start).TotalSeconds;
        if (_curSeg == hit.Seg && MediaSource.Length > 0)
        {
            SeekRequested?.Invoke(this, off);
            if (want != Playing) PauseRequested?.Invoke(this, !want);
            Playing = want;
            return;
        }
        Playing = want;
        _ = OpenAsync(hit.Seg, off, want, retry: false);
    }

    private async Task OpenAsync(RecordingSegment seg, double off, bool play, bool retry)
    {
        var m = _s.Management; var rec = Recording;
        if (m is null || rec is null) { Playing = false; return; }
        int seq = ++_openSeq;
        _playCts?.Cancel(); _playCts = new CancellationTokenSource();
        var ct = _playCts.Token;
        bool video = seg.HasVideo || seg.Tracks.Any(t => t.Kind == "video");
        LoadingAudio = true; RecordingStatus = video ? "영상 받는 중…" : "오디오 받는 중…";
        Result<string> r;
        try { r = await m.FetchSegmentAudioAsync(rec.Id, seg.Seq, null, retry, st => RecordingStatus = st, ct); }
        catch (OperationCanceledException) { if (seq == _openSeq) LoadingAudio = false; return; }
        catch (Exception ex)                                                     // 임시 파일·전송 오류는 상태 줄로 — 명령 예외로 새면 앱 오류 대화상자가 뜬다
        {
            _s.Log.Warn($"recording play {rec.Id} #{seg.Seq}: {ex.Message}");
            if (seq == _openSeq) { LoadingAudio = false; Playing = false; RecordingStatus = "재생 실패 — " + ex.Message; }
            return;
        }
        if (seq != _openSeq || ct.IsCancellationRequested) return;
        LoadingAudio = false;
        if (!r.Ok) { RecordingStatus = ResponseText.Describe(ResponseText.Area.Recording, r.Code, r.Reason); Playing = false; return; }
        _curSeg = seg; SelectedSegment = seg; PlayingHasVideo = video; MediaSource = r.Value;
        RecordingStatus = $"녹취 {Segments.Count}개 중 #{seg.Seq}";
        OpenRequested?.Invoke(this, new MediaOpenArgs(r.Value, off, play));
        if (!_logged) { _logged = true; _s.Activity.Add(ActivityPanel.Call, ActivityKind.Note, $"녹취 재생 {Parties()} #{seg.Seq}"); }
    }

    /// <summary>창의 200 ms 틱 — 재생기 위치(초, 파일이 열려 있으면)로 헤드를 옮기고, 말 없는 구간이면 시계로 흘려 다음 세그먼트를 연다.</summary>
    public void Tick(double? mediaPosSec)
    {
        if (!Playing || LoadingAudio || _segs.Count == 0) return;
        if (_curSeg is not null)
        {
            if (mediaPosSec is { } p && _segs.FirstOrDefault(x => x.Seg == _curSeg) is { Seg: not null } cur) { _head = cur.Start.AddSeconds(p); UpdateHead(); }
            return;
        }
        _head = _head.AddSeconds(0.2 * Speed);
        if (_segs.FirstOrDefault(x => _head >= x.Start && _head < x.End) is { Seg: not null }) { SeekTo(_head, true); return; }
        if (_head >= _barT0.AddSeconds(_barSpanSec)) { _head = _barT0.AddSeconds(_barSpanSec); Playing = false; RecordingStatus = "재생 끝"; }
        UpdateHead();
    }

    /// <summary>파일 하나 끝(MediaEnded) — 다음 세그먼트로(건너뛰기면 바로, 아니면 말 없는 구간을 시계로 지나서).</summary>
    public void OnMediaEnded()
    {
        var cur = _segs.FirstOrDefault(x => x.Seg == _curSeg);
        _curSeg = null; MediaSource = "";
        if (cur.Seg is null) return;
        var next = _segs.FirstOrDefault(x => x.Start > cur.Start && x.Seg != cur.Seg);
        if (next.Seg is null) { _head = cur.End; Playing = false; RecordingStatus = "재생 끝"; UpdateHead(); return; }
        if (SkipGaps || next.Start <= cur.End) SeekTo(next.Start, true);
        else { _head = cur.End; UpdateHead(); }                               // 시계가 말 없는 구간을 지나 Tick 이 다음 세그먼트를 연다
    }
    public void OnMediaFailed(string reason) { _curSeg = null; MediaSource = ""; PlayingHasVideo = false; Playing = false; RecordingStatus = "재생 실패 — " + reason; }

    // 조작 — 막대(창이 비율을 넘긴다)·버튼
    public void SeekRatio(double ratio) => SeekTo(_barT0.AddSeconds(Math.Clamp(ratio, 0, 1) * _barSpanSec));
    public void HoverAt(double ratio)
    {
        HoverRatio = Math.Clamp(ratio, 0, 1);
        var at = _barT0.AddSeconds(HoverRatio * _barSpanSec);
        string who = IsPtt ? (_turnSpans.Where(x => at >= x.Start && at < x.End).Select(x => Who(_s, x.Spk)).Distinct().ToList() is { Count: > 0 } w ? " · " + string.Join(", ", w) : " · 말 없음") : "";
        HoverText = $"{Mmss(HoverRatio * _barSpanSec)} · {at:HH:mm:ss}{who}";
    }
    public void HoverEnd() => HoverRatio = -1;

    [RelayCommand] private void TogglePlay()
    {
        if (_segs.Count == 0) return;
        if (Playing) { Playing = false; if (MediaSource.Length > 0) PauseRequested?.Invoke(this, true); return; }
        if (_head >= _barT0.AddSeconds(_barSpanSec - 0.5)) _head = _segs[0].Start;     // 끝에서 누르면 처음부터
        if (_curSeg is not null && MediaSource.Length > 0) { Playing = true; PauseRequested?.Invoke(this, false); return; }
        SeekTo(_head, true);
    }
    [RelayCommand] private void Back10() => SeekTo(_head.AddSeconds(-10));
    [RelayCommand] private void Fwd10() => SeekTo(_head.AddSeconds(10));
    /// <summary>이전·다음 발언 — 무전 발언 막대의 시작으로.</summary>
    [RelayCommand] private void PrevTurn()
    {
        var t = _turnSpans.Select(x => x.Start).Where(x => x < _head.AddSeconds(-1)).DefaultIfEmpty(_barT0).Max();
        SeekTo(t);
    }
    [RelayCommand] private void NextTurn()
    {
        if (_turnSpans.Select(x => x.Start).Where(x => x > _head.AddSeconds(0.5)).OrderBy(x => x).FirstOrDefault() is var t && t != default) SeekTo(t);
    }
    /// <summary>세션 전체 — 처음 세그먼트부터 끝까지.</summary>
    [RelayCommand] private void PlayAll() { if (_segs.Count > 0) SeekTo(_segs[0].Start, true); }
    /// <summary>발언 레인의 턴 막대 클릭 — 그 발언 시작으로 옮겨 재생.</summary>
    [RelayCommand] private void PlayTurn(TurnBar bar) => SeekTo(AxisTimeAt(bar.LeftRatio), true);
    /// <summary>다시 변환 — 지금(없으면 헤드 위치의) 세그먼트의 변환 실패 표식을 지우고 다시 받아 그 자리에서 튼다.</summary>
    [RelayCommand] private Task RetryTranscode()
    {
        var hit = _curSeg is not null ? _segs.FirstOrDefault(x => x.Seg == _curSeg) : _segs.FirstOrDefault(x => _head >= x.Start && _head < x.End);
        if (hit.Seg is null && _segs.Count > 0) hit = _segs[0];
        if (hit.Seg is null) return Task.CompletedTask;
        Playing = true;
        return OpenAsync(hit.Seg, Math.Max(0, (_head - hit.Start).TotalSeconds), true, retry: true);
    }

    private string Parties() => Selected?.Parties ?? "";

    /// <summary>정지 — 파일을 닫고 헤드는 그 자리.</summary>
    [RelayCommand] public void Stop() { Playing = false; _curSeg = null; StopMedia(); }

    private void StopMedia()
    {
        _playCts?.Cancel(); _playCts = null; _openSeq++;
        if (MediaSource.Length > 0) { StopRequested?.Invoke(this, EventArgs.Empty); MediaSource = ""; }
        LoadingAudio = false; PlayingHasVideo = false;
    }

    // ── --ui-preview 표본 (서버 없이 화면 배치·바인딩 점검 — App.xaml.cs 개발 스위치 전용) ──
    private PttSessionDetail? _previewDetail;
    private IReadOnlyList<RecordingSegment>? _previewSegments;
    private IReadOnlyList<RecordingSegment>? _previewVideoSegments;

    /// <summary>표본 이력 한 날(통화 또는 PTT)을 심고 첫 행을 고른다. 서버 조회 없이 §4.6 화면을 그려 보는 개발 스위치용.
    /// video = 무전 표본의 영상 세션(MCVideo)을 고르고 영상 칸을 열어 둔다(재생은 하지 않는다 — 칸 배치 점검).</summary>
    public void SeedPreview(HistoryKind kind, bool video = false, int rows = 0)
    {
        var day = Date.Date;
        DateTime at(int h, int m, int s = 0) => day.AddHours(h).AddMinutes(m).AddSeconds(s);
        var list = new List<HistoryEntry>();
        if (kind == HistoryKind.Ptt)
        {
            _suppressQuery = true; KindIndex = 1; _suppressQuery = false;
            list.Add(new HistoryEntry("ses-1", at(9, 12, 40), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002001", "", "tel:g002", 160, false, "", "ptt/1/2026/09/08/09/S20260908091000000000_1", true)
            { State = "ended", SessionKind = "group", StartTime = at(9, 10), EndTime = at(9, 12, 40), GroupName = "1팀 무전", MemberCount = 6, TurnCount = 7, SpeakerCount = 3,
              TotalSpeechMs = 61_000, TalkMs = 66_000, MaxConcurrent = 2, FloorControl = "on", FloorPolicy = "dual", MaxTalkers = 2,
              People = new[] { "+821310002001", "+821310002002", "+821310002003", "+821310002004" } });
            list.Add(new HistoryEntry("ses-2", at(11, 3, 5), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002002", "", "tel:g002", 25, true, "", "ptt/1/2026/09/08/11/S20260908110240000000_1", true)
            { State = "ended", SessionKind = "group", StartTime = at(11, 2, 40), EndTime = at(11, 3, 5), GroupName = "1팀 무전", TurnCount = 2, SpeakerCount = 1, TotalSpeechMs = 9_000, FloorControl = "on", FloorPolicy = "single" });
            list.Add(new HistoryEntry("ses-3", at(11, 40), HistoryKind.Ptt, "ptt.session.start", "tel:+821310002003", "", "tel:g003", 0, false, "", "", false)
            { State = "active", SessionKind = "group", StartTime = at(11, 40), GroupName = "야간 순찰", TurnCount = 1, SpeakerCount = 1, TotalSpeechMs = 3_000, FloorControl = "on" });
            list.Add(new HistoryEntry("ses-4", at(14, 20, 30), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002001", "", "tel:priv-1", 30, false, "", "", false)
            { State = "ended", SessionKind = "private", StartTime = at(14, 20), EndTime = at(14, 20, 30), FloorControl = "off", People = new[] { "+821310002001", "+821310002004" } });
            // 영상 세션(MCVideo 그룹 호) — 같은 그룹의 영상 호. 세는 것은 송출, 재생하면 영상 칸
            list.Add(new HistoryEntry("mcv-1", at(15, 8, 20), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002002", "", "tel:g002", 200, false, "", "ptt/1/2026/09/08/15/S20260908150500000000_2", true)
            { State = "ended", SessionKind = "group", Service = "mcvideo", McvSessionType = "chat", McvMaxTransmitters = 2, StartTime = at(15, 5), EndTime = at(15, 8, 20),
              GroupName = "1팀 무전", MemberCount = 6, TurnCount = 2, SpeakerCount = 2, TotalSpeechMs = 95_000, TalkMs = 95_000,
              People = new[] { "+821310002001", "+821310002002", "+821310002003" } });
            // 빈 세션 반복(§4.6 빈 세션 묶기) — 한 사람이 1분마다 호를 열었다 말없이 닫는다(유지 시간 T4 30초). 13:24 의 발언 세션이 묶음을 둘로 끊는다
            for (int i = 0; i < 9; i++)
            {
                var st = at(13, 20 + i); bool spoke = i == 4; int len = spoke ? 46 : 30 + i % 2;
                list.Add(new HistoryEntry($"rep-{i}", st.AddSeconds(len), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002003", "", "tel:g003", len, false, "", spoke ? $"ptt/1/rep/{i}" : "", spoke)
                { State = "ended", SessionKind = "group", StartTime = st, EndTime = st.AddSeconds(len), GroupName = "야간 순찰", MemberCount = 10, TurnCount = spoke ? 1 : 0,
                  SpeakerCount = spoke ? 1 : 0, TotalSpeechMs = spoke ? 3_000 : 0, FloorControl = "on", FloorPolicy = "single" });
            }
            // 서비스 축이 실린 응답의 모양으로 — 음성 세션은 "ptt"
            for (int i = 0; i < list.Count; i++) if (list[i].Service.Length == 0) list[i] = list[i] with { Service = "ptt" };
            for (int i = 0; i < list.Count; i++) list[i] = list[i] with { HasTurnCount = true };   // 세션 인덱스가 실어 준 모양 — 발언 0 인 개별 호(ses-4)가 «발언 없음»
            var v0 = at(15, 5);
            _previewVideoSegments = new[]
            {
                new RecordingSegment(1, "mcvideo", "+821310002002", v0.AddSeconds(6), v0.AddSeconds(66), 60_000, true, "ready", new[] { "+821310002002" }, 1)
                { Tracks = new[] { new SegmentTrack(0, "audio", new[] { new SpeakerSpan("+821310002002", 0, 60_000) }, true, "ready"),
                                   new SegmentTrack(0, "video", new[] { new SpeakerSpan("+821310002002", 0, 60_000) }, true, "ready") } },
                new RecordingSegment(2, "mcvideo", "+821310002003", v0.AddSeconds(120), v0.AddSeconds(155), 35_000, true, "ready", new[] { "+821310002003" }, 1)
                { Tracks = new[] { new SegmentTrack(0, "video", new[] { new SpeakerSpan("+821310002003", 0, 35_000) }, true, "ready") } },
            };
            var s0 = at(9, 10);
            _previewSegments = new[]
            {
                new RecordingSegment(1, "ptt", "+821310002001", s0.AddSeconds(2), s0.AddSeconds(14), 12_000, false, "ready", new[] { "+821310002001" }, 1),
                new RecordingSegment(2, "ptt", "+821310002002", s0.AddSeconds(20), s0.AddSeconds(45), 25_000, false, "ready", new[] { "+821310002002", "+821310002003" }, 2)
                { Tracks = new[] { new SegmentTrack(0, "audio", new[] { new SpeakerSpan("+821310002002", 0, 25_000) }, false, "ready"),
                                   new SegmentTrack(1, "audio", new[] { new SpeakerSpan("+821310002003", 8_000, 10_000) }, false, "ready") } },
                new RecordingSegment(3, "ptt", "+821310002001", s0.AddSeconds(60), s0.AddSeconds(84), 24_000, false, "ready", new[] { "+821310002001" }, 1),
                new RecordingSegment(4, "ptt", "+821310002003", s0.AddSeconds(100), s0.AddSeconds(150), 50_000, false, "transcoding", new[] { "+821310002003" }, 1),
            };
            _previewDetail = new PttSessionDetail(list[0].RecordingId,
                new[] { new PttParticipant("+821310002001", "initiator", s0, s0.AddSeconds(160)), new PttParticipant("+821310002002", "member", s0.AddSeconds(1), s0.AddSeconds(160)),
                        new PttParticipant("+821310002003", "member", s0.AddSeconds(1), null), new PttParticipant("+821310002004", "member", s0.AddSeconds(30), s0.AddSeconds(90)) },
                new[] { new PttEvent(s0, "session_start", "", "", null), new PttEvent(s0, "member_join", "+821310002001", "initiator", null),
                        new PttEvent(s0.AddSeconds(1), "member_join", "+821310002002", "member", null), new PttEvent(s0.AddSeconds(1), "member_join", "+821310002003", "member", null),
                        new PttEvent(s0.AddSeconds(30), "member_join", "+821310002004", "member", null), new PttEvent(s0.AddSeconds(90), "member_leave", "+821310002004", "member", null),
                        new PttEvent(s0.AddSeconds(160), "session_end", "", "", 160) },
                new[] { new PttFloorEvent(s0.AddSeconds(2), "GRANT", "+821310002001", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(14), "RELEASE", "+821310002001", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, 300, ""),
                        new PttFloorEvent(s0.AddSeconds(20), "GRANT", "+821310002002", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(28), "GRANT", "+821310002003", 1, 0, 2, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(31), "DENY", "+821310002004", null, null, null, "", false, "", "recv_only", 3, "+821310002002", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(38), "RELEASE", "+821310002003", 1, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(45), "RELEASE", "+821310002002", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(60), "GRANT", "+821310002001", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(70), "QUEUE", "+821310002003", null, null, null, "", false, "", "", null, "", 1, 1, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(84), "RELEASE", "+821310002001", 0, null, null, "", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(100), "GRANT", "+821310002003", 0, 0, 1, "dual", false, "", "", null, "", null, null, "", null, null, null, ""),
                        new PttFloorEvent(s0.AddSeconds(150), "REVOKE", "+821310002003", 0, null, null, "", false, "", "", null, "", null, null, "", null, 3, null, "") },
                true);
        }
        else
        {
            _suppressQuery = true; KindIndex = 0; _suppressQuery = false;
            list.Add(new HistoryEntry("c1", at(9, 5, 12), HistoryKind.Call, "call.answered", "tel:+821310002001", "tel:+821310009999", "", 42, false, "", "volte/2026/09/08/09/010/01000000001/c1.d", true)
            { State = "ended", CallType = "volte", InviteTime = at(9, 4, 25), AnswerTime = at(9, 4, 30), EndTime = at(9, 5, 12), EndReason = "normal", SipStatus = 200 });
            list.Add(new HistoryEntry("c2", at(9, 31), HistoryKind.Call, "call.missed", "tel:+821310002002", "tel:+821310002001", "", 0, false, "", "volte/2026/09/08/09/010/01000000002/c2.d", false)
            { State = "ended", CallType = "volte", InviteTime = at(9, 30, 40), EndTime = at(9, 31), EndReason = "no_answer", SipStatus = 480 });
            list.Add(new HistoryEntry("c3", at(13, 12, 3), HistoryKind.Call, "call.answered", "tel:+821310002003", "tel:0212345678", "", 305, true, "", "volte/2026/09/08/13/010/01000000003/c3.d", true)
            { State = "ended", CallType = "volte_video", InviteTime = at(13, 6, 50), AnswerTime = at(13, 6, 58), EndTime = at(13, 12, 3), EndReason = "normal", SipStatus = 200 });
            list.Add(new HistoryEntry("c4", at(13, 40), HistoryKind.Call, "call.answered", "tel:+821310002001", "tel:+821310002002", "", 0, false, "", "", false)
            { State = "active", CallType = "volte", InviteTime = at(13, 40), AnswerTime = at(13, 40, 4) });
            list.Add(new HistoryEntry("c5", at(16, 2), HistoryKind.Call, "call.missed", "tel:+821310009999", "tel:+821310002002", "", 0, false, "", "", false)
            { State = "ended", CallType = "volte", InviteTime = at(16, 1, 30), EndTime = at(16, 2), EndReason = "busy", SipStatus = 486 });
            _previewSegments = new[] { new RecordingSegment(1, "volte", "+821310002001", at(9, 4, 30), at(9, 5, 12), 42_000, false, "ready", Array.Empty<string>(), 2) };
        }
        // rows = 하루 상한(1000)까지 찬 날을 흉내 — 한 그룹에 짧은 세션이 종일 이어진 모양(목록 가상화·그리기 시간 점검)
        for (int i = list.Count; i < rows && kind == HistoryKind.Ptt; i++)
        {
            var st = day.AddSeconds(86_340.0 * i / rows);
            list.Add(new HistoryEntry($"gen-{i}", st.AddSeconds(30), HistoryKind.Ptt, "ptt.session.end", "tel:+821310002001", "", "tel:g001", 30, false, "", $"ptt/2/gen/{i}", true)
            { State = "ended", SessionKind = "group", Service = "ptt", StartTime = st, EndTime = st.AddSeconds(30), GroupName = "상황실", MemberCount = 5, TurnCount = 1, SpeakerCount = 1,
              TotalSpeechMs = 4_000, TalkMs = 4_000, FloorControl = "on", FloorPolicy = "single", People = new[] { "+821310002001", "+821310002002" } });
        }
        var swSeed = System.Diagnostics.Stopwatch.StartNew();
        _all = list.OrderByDescending(e => e.Time).ToList();
        _hours = CountHours(_all);
        RebuildHours(); Filter();
        if (rows > 0) LogTimings($"history preview {_all.Count} items", 0, swSeed);
        Selected = (video ? Rows.FirstOrDefault(r => r.IsMcVideo) : null)
                   ?? Rows.FirstOrDefault(r => _previewDetail is { } pd && r.E.RecordingId == pd.RecordingId) ?? Rows.FirstOrDefault(r => r.HasRecording) ?? Rows.FirstOrDefault();
        if (video && Selected is { IsMcVideo: true } && SelectedSegment is { } vs)
        {
            PlayingHasVideo = true; MediaSource = "(표본)";
            RecordingStatus = $"녹취 {Segments.Count}개 중 #{vs.Seq} (표본)";
        }
    }

    /// <summary>임시 오디오 파일 정리(창 닫을 때).</summary>
    public static void CleanupTemp()
    {
        try
        {
            string dir = Path.Combine(Path.GetTempPath(), "CIMS", AppPaths.AppName, "rec");
            if (!Directory.Exists(dir)) return;
            foreach (var f in Directory.EnumerateFiles(dir, "*.mp4"))
                try
                {
                    // 6시간 지난 파일과, 잠긴 정본 대신 새 이름으로 받은 사본(<정본>_<ticks>.mp4 — ManagementClient.FetchSegmentAudioAsync)은 지운다.
                    // 아직 재생기가 물고 있는 파일은 IOException 으로 건너뛰고 다음 정리 때 지워진다.
                    bool copy = System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileNameWithoutExtension(f), @"_\d{15,}$");
                    if (copy || File.GetLastWriteTimeUtc(f) < DateTime.UtcNow.AddHours(-6)) File.Delete(f);
                }
                catch (IOException) { }
                catch (UnauthorizedAccessException) { }
        }
        catch (Exception) { }
    }

    // ── 표시 사전 (콘솔 pttSession.tsx 의 EVENT_ICONS·FLOOR_OPS·DENY_REASON, VoLTE 이력의 종료사유와 같은 문구) ──

    /// <summary>화자 레인 색 — 등장 순서로 배정(콘솔 SPK_COLORS 와 같은 팔레트).</summary>
    internal static readonly string[] SpkColors = { "#2563EB", "#16A34A", "#D97706", "#9333EA", "#DC2626", "#0891B2", "#CA8A04", "#DB2777", "#4F46E5", "#059669", "#E11D48", "#0D9488" };

    internal static string EndReasonText(string reason) => reason switch
    {
        "normal" => "정상종료", "no_answer" => "무응답", "busy" => "통화중", "rejected" => "거절", "error" => "오류",
        "timeout" => "시간초과", "incomplete" => "비정상 종료(기록 없음)", "" => "—", var x => x,
    };

    internal static string FloorLabel(string op) => op switch
    {
        "GRANT" => "발언권 부여", "RELEASE" => "발언 종료", "IDLE" => "유휴", "REVOKE" => "회수 통지", "REVOKE_END" => "회수 확정",
        "QUEUE" => "대기열 등록", "QUEUE_CANCEL" => "대기 취소", "DENY" => "거절", var x => x,
    };

    internal static string FloorColor(string op) => op switch
    {
        "GRANT" => "#16A34A", "REVOKE" => "#D97706", "REVOKE_END" or "DENY" => "#DC2626", "QUEUE" => "#0891B2", _ => "#9CA3AF",
    };

    /// <summary>op 별 부가 정보 — 규격 용어(TS 24.380)로 사유·대기 순번·회수 유예·선점을 펼친다.</summary>
    internal static string FloorDetail(PttFloorEvent f)
    {
        var p = new List<string>();
        switch (f.Op)
        {
            case "GRANT":
                if (f.Preempt) p.Add(f.PreemptedFrom.Length > 0 ? $"선점 — {UserPartConverter.UserPart(f.PreemptedFrom)} 회수" : "선점");
                if (f.Talkers is { } tk && tk > 1) p.Add($"동시 {tk}명");
                if (f.Slot is { } sl && f.Talkers is { } t2 && t2 > 1) p.Add($"슬롯 {sl}");
                if (f.Prio is { } pr && pr > 0) p.Add($"우선순위 {pr}");
                break;
            case "DENY":
                p.Add(f.Reason switch { "recv_only" => "수신전용(ambient)", "only_one" => "참가자 1인", "broadcast" => "broadcast 비개시자", "" => "", var x => x });
                if (f.Owner.Length > 0) p.Add($"발언 중 {UserPartConverter.UserPart(f.Owner)}");
                if (f.Cause is { } c) p.Add($"cause {c}");
                break;
            case "QUEUE":
                if (f.Pos is { } pos) p.Add(f.QSize is { } qs ? $"대기 {pos}/{qs}" : $"대기 {pos}번");
                break;
            case "QUEUE_CANCEL":
                if (f.Revoked.Length > 0) p.Add($"{UserPartConverter.UserPart(f.Revoked)} 대기 해제");
                if (f.Removed is { } rm && rm > 0) p.Add($"{rm}건 제거");
                break;
            case "REVOKE":
                if (f.GraceSec is { } g) p.Add($"{g}초 후 회수");
                if (f.PreemptedBy.Length > 0) p.Add($"선점자 {UserPartConverter.UserPart(f.PreemptedBy)}");
                break;
            case "REVOKE_END":
                if (f.PreemptedBy.Length > 0) p.Add($"선점자 {UserPartConverter.UserPart(f.PreemptedBy)}");
                break;
            case "RELEASE":
            case "IDLE":
                if (f.IdleMs is { } idle && idle > 0) p.Add($"무음 {FmtSpeech(idle)}");
                break;
        }
        return string.Join(" · ", p.Where(x => x.Length > 0));
    }

    internal static (string Label, string Color) EventDisplay(string type) => type switch
    {
        "session_start" => ("세션 시작", "#16A34A"), "session_end" => ("세션 종료", "#DC2626"),
        "member_join" => ("입장", "#2563EB"), "member_leave" => ("퇴장", "#F97316"),
        "floor-grant" => ("발언 시작", "#16A34A"), "floor-release" => ("발언 종료", "#9CA3AF"),
        "config_change" => ("설정 변경", "#9333EA"), "member_invite" => ("초대", "#0891B2"),
        var x => (x, "#9CA3AF"),
    };

    /// <summary>이름표 머리글자 — 이름이면 첫 글자, 번호면 "#"(관제 «기록» 아바타와 같은 규칙).</summary>
    internal static string InitialOf(string label) => label.Trim() is { Length: > 0 } n && !(char.IsDigit(n[0]) || n[0] == '+') ? n[..1] : "#";

    internal static string FmtDur(int sec) { if (sec <= 0) return "—"; int m = sec / 60; return m > 0 ? $"{m}분 {sec % 60}초" : $"{sec}초"; }
    /// <summary>발화 시간(ms) — 1분 이상은 "m분 s초", 미만은 "s.d초".</summary>
    internal static string FmtSpeech(int ms)
    {
        if (ms <= 0) return "0초";
        if (ms >= 60_000) { int s = ms / 1000; return $"{s / 60}분 {s % 60}초"; }
        return ms >= 10_000 ? $"{ms / 1000}초" : $"{ms / 1000.0:0.#}초";
    }

    /// <summary>이름이 있으면 이름, 없으면 표시 번호(홈 국가 로컬 표기).</summary>
    internal static string Who(DispatchSession s, string u)
    {
        string up = UserPartConverter.UserPart(u);
        return s.Directory.NameOf(up) is { Length: > 0 } n ? n : s.Directory.DisplayNumber(up);
    }
    internal static bool SameUser(string a, string b) =>
        a.Length > 0 && b.Length > 0 && DirectoryService.Normalize(UserPartConverter.UserPart(a)) == DirectoryService.Normalize(UserPartConverter.UserPart(b));
}
