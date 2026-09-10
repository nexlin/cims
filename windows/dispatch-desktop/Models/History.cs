// 서버 통합 이력 한 건 — `GET /provisioning/history?kind=call|ptt|message`(CSC 4430, PKCE) 응답 항목의 앱 표현.
// 진행 중(live) 상태는 종전대로 RFC 4235 dialog·RFC 4575 conference 구독이 정본이고, 이력은 "끝난 일"의 수초 지연 사본이다(§13).
namespace DispatchDesktop.Models;

public enum HistoryKind { Call, Ptt, Message }

/// <summary>
/// Id = 서버가 매긴 항목 식별자(중복 제거 키). Event = 서버 이벤트 이름(call.answered·ptt.talk·message.sds …) — 앱은 알려진 값만
/// ActivityKind 로 옮기고 나머지는 Note 로 표시한다. From/To/Group 는 tel:/sip: URI 또는 E.164, Text 는 메시지 본문(kind=message).
/// </summary>
/// RecordingId = 녹취 식별자(세션 디렉터리 상대 경로 — `/provisioning/recordings/{id}` 의 키, 없으면 ""), HasRecording = 녹취 파일 존재.
/// 그 아래 init 속성은 [이력] 화면(§4.6)용 종류별 확장 필드(서버 계약 android_ue_provisioning.md §3-2) — 폴링 병합(②④)은 읽지 않는다.
public sealed record HistoryEntry(string Id, DateTime Time, HistoryKind Kind, string Event, string From, string To, string Group,
                                  int DurationSec, bool Emergency, string Text, string RecordingId = "", bool HasRecording = false)
{
    /// <summary>call: ended|active|ringing · ptt: ended|active.</summary>
    public string State { get; init; } = "";
    /// <summary>call: volte | volte_video.</summary>
    public string CallType { get; init; } = "";
    public DateTime? InviteTime { get; init; }
    public DateTime? AnswerTime { get; init; }
    public DateTime? EndTime { get; init; }
    /// <summary>call: normal|no_answer|busy|rejected|error|timeout|incomplete (문구는 앱 사전).</summary>
    public string EndReason { get; init; } = "";
    public int SipStatus { get; init; }
    /// <summary>ptt: group | private | adhoc (TS 24.481 그룹 문서 유무 — 콘솔 PTT 이력의 종류 배지와 같은 축).</summary>
    public string SessionKind { get; init; } = "";
    public DateTime? StartTime { get; init; }
    public string GroupName { get; init; } = "";
    public int MemberCount { get; init; }
    /// <summary>발언 턴 = 화자 구간 수(동시 발언 세그먼트는 턴이 여럿). CMP segments.jsonl 집계(OAM 세션 인덱스) — 스캔 폴백이면 0.</summary>
    public int TurnCount { get; init; }
    public int SpeakerCount { get; init; }
    /// <summary>발화 구간 합(겹침 1회) / 발화 누적(화자별 합).</summary>
    public int TotalSpeechMs { get; init; }
    public int TalkMs { get; init; }
    public int MaxConcurrent { get; init; }
    /// <summary>세션 당시 floor 축 — on(반이중·무전) | off(전이중·통화형) | ""(미기록).</summary>
    public string FloorControl { get; init; } = "";
    /// <summary>single | dual | multi (TS 24.380 동시 발언 정책).</summary>
    public string FloorPolicy { get; init; } = "";
    public int MaxTalkers { get; init; }
    /// <summary>참여자(발언 안 한 참가자 포함) — 1:1·임시는 개시자를 뺀 나머지가 상대.</summary>
    public IReadOnlyList<string> People { get; init; } = Array.Empty<string>();

    /// <summary>시간대 밴드·필터 축 — 통화는 INVITE, PTT 는 세션 시작(서버 hours 와 같은 규칙), 없으면 항목 시각.</summary>
    public DateTime AxisTime => InviteTime ?? StartTime ?? Time;
}

/// <summary>창 조회 한 페이지 — 항목(시각 오름차순) + 다음 커서 + 시간대 분포(HH → 건수, limit 절삭 전 전체).</summary>
public sealed record HistoryPage(IReadOnlyList<HistoryEntry> Items, string Next, IReadOnlyDictionary<string, int> Hours);

/// <summary>PTT 세션 참여자(events.jsonl 입퇴장 기록) — role = initiator|member.</summary>
public sealed record PttParticipant(string Msisdn, string Role, DateTime? Join, DateTime? Leave);

/// <summary>PTT 세션 이벤트(CSP events.jsonl) — session_start/session_end/member_join/member_leave/member_invite/config_change.</summary>
public sealed record PttEvent(DateTime? Ts, string Type, string Member, string Role, int? DurationSec);

/// <summary>floor 중재 이벤트(CMP floor.jsonl, TS 24.380 op 8종) — GRANT/RELEASE/IDLE/REVOKE/REVOKE_END/QUEUE/QUEUE_CANCEL/DENY.
/// 부가 정보는 op 별로 채워진다(없으면 null/"").</summary>
public sealed record PttFloorEvent(DateTime? Ts, string Op, string User, int? Slot, int? Prio, int? Talkers, string Policy, bool Preempt,
                                   string PreemptedFrom, string Reason, int? Cause, string Owner, int? Pos, int? QSize, string Revoked,
                                   int? Removed, int? GraceSec, int? IdleMs, string PreemptedBy);

/// <summary>PTT 세션 상세(GET /provisioning/history/ptt/{recordingId}) — 참여자·이벤트·floor 타임라인. 발언 턴은 녹취 세그먼트에서 만든다.</summary>
public sealed record PttSessionDetail(string RecordingId, IReadOnlyList<PttParticipant> Participants, IReadOnlyList<PttEvent> Events,
                                      IReadOnlyList<PttFloorEvent> Floor, bool HasRecording);
