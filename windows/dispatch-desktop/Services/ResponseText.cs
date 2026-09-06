// 응답 코드 → 화면 문구 사전 (dispatch_desktop_ui.md §9) — 앱이 프로토콜 코드를 해석하는 유일한 지점. 원문 코드는 토스트 ▸상세.
using DispatchDesktop.Models;

namespace DispatchDesktop.Services;

public static class ResponseText
{
    public enum Area { Pickup, Transfer, Join, PttListen, PttJoin, PttPrivate, PttAdhoc, Emergency, Sds, Sms, Register, Call, Group }

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
        // GMS 그룹 관리(XCAP PUT/DELETE, mcptt_api.md §2) — 본문 `error` 로 세분(GroupError)
        (Area.Group, 401) => "로그인이 만료됐습니다 — 다시 로그인하세요",
        (Area.Group, 403) => "그룹을 만들거나 바꿀 권한이 없습니다 (자격 또는 본인 소유 그룹만)",
        (Area.Group, 404) => "그룹이 없습니다 — 목록을 새로 고칩니다",
        (Area.Group, 409) => "같은 id 의 그룹을 다른 사용자가 소유하고 있습니다",
        (Area.Group, 412) => "다른 곳에서 먼저 바뀐 그룹입니다 — 다시 열어 편집하세요",
        (Area.Group, 400) => "그룹 문서 형식 오류",
        _ => null,
    };

    /// <summary>GMS 오류 본문의 `error` 값 → 문구(mcptt_api.md §2 표). 없으면 null. detail(번호 배열)은 본문에 붙인다.</summary>
    public static string? ForGroupError(string error, string detail) => error switch
    {
        "group_creation_not_allowed" => "그룹 생성 자격이 없습니다 (관리자 부여 필요)",
        "not_group_owner" => "본인이 만든 그룹만 편집·삭제할 수 있습니다",
        "uri_taken" => "같은 id 의 그룹을 다른 사용자가 소유하고 있습니다 — 다른 id 로 다시 시도",
        "unknown_member" => detail.Length > 0 ? $"PTT 미가입 번호가 있습니다: {detail}" : "PTT 미가입 번호가 있습니다",
        "invalid_group_id" or "reserved_prefix" or "invalid_group_document" => "그룹 문서 형식 오류 (앱 결함 — 로그 확인)",
        "etag_mismatch" => "편집 중 다른 곳에서 먼저 바뀐 그룹입니다 — 문서를 다시 읽어 편집하세요",
        "not_found" => "그룹이 없습니다 — 목록을 새로 고칩니다",
        _ => null,
    };

    /// <summary>SDK 실패 사유(`putGroup 403: {"error":"…","detail":[…]}`)에서 `error` 토큰과 `detail` 요약을 뽑는다.</summary>
    public static (string Error, string Detail) GroupError(string reason)
    {
        var m = System.Text.RegularExpressions.Regex.Match(reason, "\"error\"\\s*:\\s*\"([^\"]+)\"");
        if (!m.Success) return ("", "");
        var d = System.Text.RegularExpressions.Regex.Match(reason, "\"detail\"\\s*:\\s*(\\[[^\\]]*\\]|\"[^\"]*\")");
        string detail = d.Success ? d.Groups[1].Value.Trim('[', ']', '"').Replace("\"", "") : "";
        return (m.Groups[1].Value, detail);
    }

    public static string Describe(Area area, int code, string reason)
    {
        if (area == Area.Group)
        {
            var (err, detail) = GroupError(reason);
            if (err.Length > 0 && ForGroupError(err, detail) is { } g) return g;
        }
        string? t = For(area, code);
        if (t is not null) return t;
        if (code >= 100) return $"실패 ({code} {reason})";
        return reason.Length > 0 ? reason : "실패";
    }
}
