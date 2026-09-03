// 응답 코드 → 화면 문구 사전 (dispatch_desktop_ui.md §9) — 앱이 프로토콜 코드를 해석하는 유일한 지점. 원문 코드는 토스트 ▸상세.
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public static class ResponseText
{
    public enum Area { Pickup, Transfer, Join, PttListen, PttJoin, PttPrivate, PttAdhoc, Emergency, Sds, Sms, Register, Call }

    public static Area AreaOf(Operation op) => op switch
    {
        Operation.Pickup => Area.Pickup,
        Operation.Transfer => Area.Transfer,
        Operation.Join => Area.Join,
        Operation.PttListen => Area.PttListen,
        Operation.PttJoin => Area.PttJoin,
        Operation.PttPrivate => Area.PttPrivate,
        Operation.PttAdhoc => Area.PttAdhoc,
        Operation.Emergency => Area.Emergency,
        _ => Area.Call,
    };

    /// <summary>문구. 사전에 없으면 null(호출자가 일반 문구를 쓴다).</summary>
    public static string? For(Area area, int code) => (area, code) switch
    {
        (Area.Pickup, 404) => "당겨받을 호가 없습니다",
        (Area.Pickup, 403) => "다른 그룹의 호입니다",
        (Area.Pickup, 489) => "구독 이벤트 미지원(서버)",
        (Area.Transfer, 403) => "이 서비스는 호 전달이 허용되지 않습니다",
        (Area.Transfer, >= 400 and < 500) => "전달 대상이 응답하지 않아 원 통화를 유지합니다",
        (Area.Join, 403) => "청취 권한이 없는 대상입니다",
        (Area.Join, 481) => "통화가 이미 종료되었거나 아직 연결 전입니다",
        (Area.Join, 488) => "미디어 조건 불일치(코덱/SRTP) — 관리자 문의",
        (Area.Join, 486) => "이 통화의 청취 인원이 찼습니다",
        (Area.PttListen, 403) => "청취 자격이 없거나 범위 밖 그룹입니다",
        (Area.PttListen, 480) => "진행 중인 그룹 통화가 없습니다",
        (Area.PttJoin, 403) => "그룹 멤버가 아닙니다",
        (Area.PttPrivate, 403) => "사설콜 자격이 없거나 상대가 허용하지 않습니다",
        (Area.PttPrivate, 404) => "상대를 찾을 수 없음",
        (Area.PttPrivate, 480) => "응답 없음",
        (Area.PttPrivate, 486) => "통화 중",
        (Area.PttAdhoc, 403) => "애드혹 그룹통화 자격이 없거나 시스템에서 꺼져 있습니다",
        (Area.Emergency, 403) => "긴급 호출 자격이 없습니다",
        (Area.Sds, 403) => "그룹 문자 권한 없음",
        (Area.Sds, 413) => "너무 긺(서버 한도)",
        (Area.Sds, 404 or 408 or 503) => "전송 실패 — 재전송",
        (Area.Sms, 404) => "상대가 등록되어 있지 않습니다",
        (Area.Sms, 480) => "응답 없음",
        (Area.Sms, 413) => "문자가 너무 깁니다(서버 한도)",
        (Area.Register, 401 or 403) => "인증 실패 — 다시 로그인",
        (Area.Register, 408 or 503) => "서버 응답 없음 — 재시도 중",
        (Area.Call, 486) => "통화 중",
        (Area.Call, 480) => "응답 없음",
        (Area.Call, 404) => "없는 번호입니다",
        (Area.Call, 403) => "발신이 허용되지 않습니다",
        (Area.Call, 487) => "취소됨",
        (Area.Call, 603) => "거절됨",
        _ => null,
    };

    public static string Describe(Area area, int code, string reason)
    {
        string? t = For(area, code);
        if (t is not null) return t;
        if (code >= 100) return $"실패 ({code} {reason})";
        return reason.Length > 0 ? reason : "실패";
    }
}
