// 내역 행 — ②④ "최근" 목록. 앱 로컬 링 버퍼(관제사의 작업 메모리), 서버 정본(세션 이력·감사)과 별개(§4.2·§4.4).
namespace DispatchDesktop.Models;

public enum ActivityPanel { Ptt, Call }

public enum ActivityKind
{
    Talk, Emergency, SessionStart, SessionEnd, Member, Sds, Private, Adhoc, ListenStart, ListenEnd,
    Incoming, Outgoing, Missed, Transfer, Pickup, Sms, Note,
}

public sealed record ActivityRow(DateTime Time, ActivityPanel Panel, ActivityKind Kind, string Title, string Detail,
                                 bool IsEmergency = false, bool IsMissed = false, string Number = "", bool IsPilot = false)
{
    public string KindText => Kind switch
    {
        ActivityKind.Talk => "발언", ActivityKind.Emergency => "긴급", ActivityKind.SessionStart => "시작", ActivityKind.SessionEnd => "종료",
        ActivityKind.Member => "멤버", ActivityKind.Sds => "메시지", ActivityKind.Private => "사설콜", ActivityKind.Adhoc => "애드혹",
        ActivityKind.ListenStart => "청취", ActivityKind.ListenEnd => "청취 종료", ActivityKind.Incoming => "착신", ActivityKind.Outgoing => "발신",
        ActivityKind.Missed => "부재", ActivityKind.Transfer => "전달", ActivityKind.Pickup => "픽업", ActivityKind.Sms => "SMS", _ => "",
    };
    public bool CanRedial => Number.Length > 0 && Kind is ActivityKind.Incoming or ActivityKind.Outgoing or ActivityKind.Missed or ActivityKind.Transfer;
}
