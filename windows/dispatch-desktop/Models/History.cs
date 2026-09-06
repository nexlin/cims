// 서버 통합 이력 한 건 — `GET /provisioning/history?kind=call|ptt|message`(CSC 4430, PKCE) 응답 항목의 앱 표현.
// 진행 중(live) 상태는 종전대로 RFC 4235 dialog·RFC 4575 conference 구독이 정본이고, 이력은 "끝난 일"의 수초 지연 사본이다(§13).
namespace DispatchDesktop.Models;

public enum HistoryKind { Call, Ptt, Message }

/// <summary>
/// Id = 서버가 매긴 항목 식별자(중복 제거 키). Event = 서버 이벤트 이름(call.answered·ptt.talk·message.sds …) — 앱은 알려진 값만
/// ActivityKind 로 옮기고 나머지는 Note 로 표시한다. From/To/Group 는 tel:/sip: URI 또는 E.164, Text 는 메시지 본문(kind=message).
/// </summary>
public sealed record HistoryEntry(string Id, DateTime Time, HistoryKind Kind, string Event, string From, string To, string Group,
                                  int DurationSec, bool Emergency, string Text);
