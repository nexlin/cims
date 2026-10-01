// 내역 행 — [무전] «이벤트»·[통화] «기록»의 원천. 앱 로컬 링 버퍼(관제사의 작업 메모리), 서버 정본(세션 이력·감사)과 별개(§4.4).
namespace DispatchDesktop.Models;

public enum ActivityPanel { Ptt, Call }

public enum ActivityKind
{
    Talk, Emergency, SessionStart, SessionEnd, Member, Sds, Private, Adhoc, ListenStart, ListenEnd,
    Incoming, Outgoing, Missed, Transfer, Pickup, Sms, Note,
    /// <summary>발언 요청 거부·발언권 회수·요청 시간 초과(floor 사유) — «이벤트» [오류] 칩.</summary>
    Error,
    /// <summary>MCVideo 그룹 영상(§10.3) — 영상 참여·나감 · 송출 시작·끝 · 받기·그만 보기. «이벤트» [영상] 칩.</summary>
    Video,
}

/// <param name="IsOthers">서버 이력에서 온 타인 간 항목(관제 범위 모니터링, §13) — 이 데스크의 응대·부재 집계에 넣지 않는다.</param>
public sealed record ActivityRow(DateTime Time, ActivityPanel Panel, ActivityKind Kind, string Title, string Detail,
                                 bool IsEmergency = false, bool IsMissed = false, string Number = "", bool IsPilot = false, bool IsOthers = false)
{
    public string KindText => Kind switch
    {
        ActivityKind.Talk => "발언", ActivityKind.Emergency => "긴급", ActivityKind.SessionStart => "시작", ActivityKind.SessionEnd => "종료",
        ActivityKind.Member => "멤버", ActivityKind.Sds => "메시지", ActivityKind.Private => "개별", ActivityKind.Adhoc => "애드혹",
        ActivityKind.ListenStart => "청취", ActivityKind.ListenEnd => "청취 종료", ActivityKind.Incoming => "착신", ActivityKind.Outgoing => "발신",
        ActivityKind.Missed => "부재", ActivityKind.Transfer => "전달", ActivityKind.Pickup => "픽업", ActivityKind.Sms => "문자", ActivityKind.Error => "오류", ActivityKind.Video => "영상", _ => "",
    };
    public bool CanRedial => Number.Length > 0 && Kind is ActivityKind.Incoming or ActivityKind.Outgoing or ActivityKind.Missed or ActivityKind.Transfer;
}
