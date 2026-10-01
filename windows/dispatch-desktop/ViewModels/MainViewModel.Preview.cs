// 개발 스위치(§3.4) — --ui-preview-canvas: 서버 없이 관제 두 화면에 표본(멤버 그룹·청취 범위·진행 중 그룹콜/개별/애드혹·일제 통화·긴급·
// VoLTE 통화·대표번호 대기열·진행 중 통화·그룹원·기록·무전 메시지)을 심는다. 코어 세션이 아니라 스냅샷 모델만 채우므로 조작 버튼은 동작하지 않는다.
// --ui-preview-mode=ptt|call · --ui-preview-panel=channel|other|users|group|event|dir · --ui-preview-keypad · --ui-preview-banner=alerts|incoming|none.
using CimsUe;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed partial class MainViewModel
{
    public void SeedCanvasPreview(string banner = "alerts")
    {
        var s = Session;
        CallInfo Ci(int id, CallState st, string remote, bool mcptt, string group, bool priv = false, bool half = true, bool emg = false, bool listen = false,
                    CallDir dir = CallDir.Outgoing, bool bcast = false, string caller = "tel:1001", bool peril = false, string called = "", bool muted = false,
                    string joined = "", MediaSource[]? sources = null) =>
            new(id, 1, dir, st, remote, called, false, true, muted, true, 0, 0, "", sources ?? Array.Empty<MediaSource>(), mcptt, group,
                new McpttInfo(mcptt, priv ? "private" : "prearranged", "", caller, group, emg, peril, priv, !half, bcast), half, listen, joined,
                1f, new McpttCondition(emg, peril, false, false, 0));
        GroupInfo G(string id, string name, int members, bool member, params (string, string)[] roster)
        {
            var g = new GroupInfo(id, "tel:" + id, name, members) { IsMember = member, IsOwner = id == "g-ops" };
            g.Roster = roster.Select(r => new RosterEntry(r.Item1, r.Item2)).ToList();
            return g;
        }
        // 데스크 신원 — 관제 그룹·대표번호(대기열 판정)만. 전화·PTT 계정은 없다(등록 점등 회색)
        s.Profile = new Profile("김관제", "desk1", "82", "", 0, Array.Empty<ServiceProfile>(),
            new DispatchProfile(true, "g-desk", "관제1과", "tel:7000", "all", "all", "hidden", "none", "", Array.Empty<DispatchMember>(), Array.Empty<DispatchTarget>()), true);
        // PTT·전화 전화번호부 표본 — 카드·로스터·사용자 패널·주소록·기록 이름
        s.Directory.CountryCode = "82";
        s.Directory.SeedPreview("ptt", """
            {"orgs":[{"code":"p1","name":"순찰대"},{"code":"hq","name":"상황실"},{"code":"fx","name":"정비과"}],
             "entries":[{"msisdn":"1001","name":"최순경","org":"p1"},{"msisdn":"1003","name":"이순경","org":"p1"},{"msisdn":"1004","name":"정경장","org":"p1"},
                        {"msisdn":"1005","name":"김순경","org":"p1"},{"msisdn":"1008","name":"윤순경","org":"p1"},{"msisdn":"1006","name":"박경장","org":"hq"},
                        {"msisdn":"1007","name":"서상황","org":"hq"},{"msisdn":"1010","name":"한지원","org":"hq"},{"msisdn":"1011","name":"정정비","org":"fx"},
                        {"msisdn":"1013","name":"강순경","org":"p1"},{"msisdn":"1009","name":"오경비","org":"hq"}]}
            """);
        s.Directory.SeedPreview("volte", """
            {"orgs":[{"code":"dc","name":"관제과"},{"code":"hq","name":"상황실"},{"code":"p1","name":"순찰대"},{"code":"fx","name":"정비과"}],
             "entries":[{"msisdn":"7002","name":"이당직","org":"dc"},{"msisdn":"7003","name":"서상황","org":"hq"},{"msisdn":"7004","name":"한지원","org":"hq"},
                        {"msisdn":"1006","name":"박경장","org":"hq"},{"msisdn":"+821031001004","name":"이순경","org":"p1"},{"msisdn":"+821031001008","name":"윤순경","org":"p1"}]}
            """);
        s.Groups.Add(G("g-guard", "경비", 5, true, ("tel:1009", "connected"), ("tel:1011", "connected")));
        s.Groups.Add(G("g-traffic", "교통1", 6, true));
        s.Groups.Add(G("g-ops", "상황실", 8, true, ("tel:1006", "connected"), ("tel:5001", "connected"), ("tel:1010", "connected")));
        s.Groups.Add(G("g-patrol1", "순찰1", 12, true, ("tel:5001", "connected"), ("tel:1001", "connected"), ("tel:1003", "connected"), ("tel:1004", "connected"),
                       ("tel:1005", "connected"), ("tel:1008", "connected"), ("tel:1013", "connected")));
        s.Groups.Add(G("g-patrol2", "순찰2", 9, true, ("tel:1002", "connected"), ("tel:5001", "connected"), ("tel:1004", "connected"), ("tel:1005", "connected"), ("tel:1003", "connected")));
        s.Groups.Add(G("g-night", "야간순찰", 15, false, ("tel:1010", "connected"), ("tel:1011", "connected"), ("tel:1012", "connected")));
        s.Groups.Add(G("g-fix", "정비반", 7, false, ("tel:1013", "connected"), ("tel:1014", "connected")));
        s.Groups.Add(G("g-traffic2", "교통2", 7, false, ("tel:1015", "connected"), ("tel:1016", "connected"), ("tel:1017", "connected"), ("tel:1018", "connected")));
        foreach (var (id, name) in new[] { ("g-outer", "외곽경비"), ("g-gate", "정문경비"), ("g-sup", "지원1조"), ("g-nfix", "야간정비"), ("g-res", "예비대") })
            s.Groups.Add(G(id, name, 6, false));
        var now = DateTime.Now;
        var ops = new SessionItem(Ci(12, CallState.Active, "sip:g-ops@ptt", true, "g-ops", emg: true, caller: "tel:1006"), AccountKind.Ptt, Operation.PttJoin) { Title = "상황실", Speaker = "박경장", SpeakerSince = now.AddSeconds(-41), ConnectedAt = now.AddMinutes(-14) };
        var patrol = new SessionItem(Ci(11, CallState.Active, "sip:g-patrol1@ptt", true, "g-patrol1"), AccountKind.Ptt, Operation.PttJoin) { Title = "순찰1", ConnectedAt = now.AddMinutes(-2) };
        var bcIn = new SessionItem(Ci(19, CallState.Active, "sip:g-patrol2@ptt", true, "g-patrol2", dir: CallDir.Incoming, bcast: true, caller: "tel:1002"), AccountKind.Ptt, Operation.Incoming)
                   { Title = "순찰2", Speaker = "관제2석", SpeakerSince = now.AddSeconds(-6), ConnectedAt = now.AddSeconds(-10) };
        bcIn.Floor = new FloorInfo(FloorState.Listening, new[] { new Talker("tel:1002", 0, false) }, false, FloorIndicator.BroadcastGroup, -1, 0, "", 0, 0, 1, 0);
        var priv = new SessionItem(Ci(14, CallState.Active, "tel:1008", true, "", priv: true, half: false), AccountKind.Ptt, Operation.PttPrivate) { Title = "윤순경", ConnectedAt = now.AddMinutes(-1) };
        var adhoc = new SessionItem(Ci(13, CallState.Active, "sip:adhoc-5001-1@ptt", true, "adhoc-5001-1"), AccountKind.Ptt, Operation.PttAdhoc)
                    { Title = "애드혹", AdhocMembers = new[] { "tel:1003", "tel:1008", "tel:1001" }, Speaker = "최순경", SpeakerSince = now.AddSeconds(-3), ConnectedAt = now.AddSeconds(-25) };
        var night = new SessionItem(Ci(15, CallState.Active, "sip:g-night@ptt", true, "g-night", listen: true), AccountKind.Ptt, Operation.PttListen) { Title = "야간순찰", Speaker = "박현장", SpeakerSince = now.AddSeconds(-14), ConnectedAt = now.AddMinutes(-18) };
        var volte = new SessionItem(Ci(16, CallState.Active, "tel:+82233334444", false, "", dir: CallDir.Incoming, muted: true), AccountKind.Volte, Operation.Incoming) { Title = "02-333-4444", ConnectedAt = now.AddMinutes(-2) };
        var held = new SessionItem(Ci(17, CallState.Held, "tel:1006", false, "", dir: CallDir.Incoming), AccountKind.Volte, Operation.Incoming) { Title = "1006 박경장", ConnectedAt = now.AddMinutes(-5) };
        // VoLTE 감청(INVITE-Join, «진행 중» 행 확장) — 7002 이당직 ↔ 010-5555-1212, 소스 귀속 두 줄(RFC 5576 caller/callee)
        var mon = new SessionItem(Ci(21, CallState.Active, "tel:7002", false, "", listen: true, joined: "cd2",
                                     sources: new[] { new MediaSource(1, "caller", true, 0.62f), new MediaSource(2, "callee", false, 0.25f) }), AccountKind.Volte, Operation.Join)
                  { Title = "7002 이당직 ↔ 010-5555-1212", ConnectedAt = now.AddMinutes(-2) };
        foreach (var x in new[] { ops, patrol, bcIn, priv, adhoc, night, volte, held, mon }) { x.Tick(now); s.Sessions.Add(x); }
        // [무전] 이벤트
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.SessionStart, "경비 세션 시작", "참가 2");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Sds, "순찰1 SDS 이순경", "파일 현장사진_01.jpg");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Error, "순찰1 발언 요청 거부", "대기열이 가득 찼습니다");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.SessionStart, "순찰2 일제 통화", "관제2석");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Member, "순찰2 정경장 합류", "참가 5");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Sds, "순찰1 SDS 박경장", "\"교대 인원 2명 추가 배치 바랍니다\"");
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Emergency, "상황실 긴급 개시", "1006 박경장", emergency: true);
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Emergency, "경비 임박 개시", "1009 오경비", emergency: true);
        s.Activity.Add(ActivityPanel.Ptt, ActivityKind.Talk, "순찰1 박현장 발언 14초");
        // [통화] 기록 — 이 데스크의 착신·발신·부재·전달·감청 + 문자
        s.Activity.Add(new ActivityRow(now.AddMinutes(-78), ActivityPanel.Call, ActivityKind.Outgoing, "발신 → 010-5555-1212", "01:02", Number: "+821055551212"));
        s.Activity.Add(new ActivityRow(now.AddMinutes(-47), ActivityPanel.Call, ActivityKind.ListenStart, "청취 시작 7002 이당직 ↔ 010-1234-5678", "02:40", Number: "7002"));
        s.Activity.Add(new ActivityRow(now.AddMinutes(-36), ActivityPanel.Call, ActivityKind.Transfer, "전달 → 7003 서상황 blind", "→ 7003 서상황 · blind", Number: "+8227771234"));
        s.Activity.Add(new ActivityRow(now.AddMinutes(-27), ActivityPanel.Call, ActivityKind.Outgoing, "발신 → 1006 박경장", "05:12", Number: "1006"));
        s.Activity.Add(new ActivityRow(now.AddMinutes(-14), ActivityPanel.Call, ActivityKind.Incoming, "착신 7000 ← 010-2222-3333", "03:12 · 응답 7004 한지원", Number: "+821022223333", IsPilot: true));
        s.Activity.Add(new ActivityRow(now.AddMinutes(-8), ActivityPanel.Call, ActivityKind.Missed, "부재 7000 ← 010-7777-8888", "넘김 7100", IsMissed: true, Number: "+821077778888", IsPilot: true));
        s.Activity.Add(new ActivityRow(now.AddHours(-5), ActivityPanel.Call, ActivityKind.Outgoing, "발신 → 7003 서상황", "01:05", Number: "7003"));
        s.Activity.Add(new ActivityRow(now.AddHours(-2), ActivityPanel.Call, ActivityKind.Incoming, "착신 ← 7003 서상황", "00:48", Number: "7003"));
        Sms.SeedPreview("7003", (true, "오늘 교대 명단 부탁드려요", 200), (false, "교대 명단 보내 드렸어요", 20),
                        (true, "받았습니다. 야간조 2명이 바뀌었으니 확인 부탁드리고, 변경된 명단은 내일 아침 브리핑 전까지 게시판에도 올려 주세요.", 8));
        Sms.SeedPreview("+821055551212", (false, "네 알겠습니다", 64));
        // 대표번호 대기열(포크 대기 leg) · 진행 중(감시 대상 dialog) · 그룹원 칸
        s.SeedPreviewDialog(new DialogInfo(0, "tel:7000", "q1", "cq1", "", "", "recipient", "early", "tel:+821022223333", true));
        s.SeedPreviewDialog(new DialogInfo(0, "tel:7003", "q1a", "cq1", "", "", "recipient", "early", "tel:+821022223333", true));
        s.SeedPreviewDialog(new DialogInfo(0, "tel:7004", "q1b", "cq1", "", "", "recipient", "early", "tel:+821022223333", true));
        s.SeedPreviewDialog(new DialogInfo(0, "tel:7002", "d2", "cd2", "", "", "initiator", "confirmed", "tel:+821055551212", true));
        s.SeedPreviewDialog(new DialogInfo(0, "tel:7004", "d4", "cd4", "", "", "recipient", "confirmed", "tel:+821090901111", true));
        // 배너 층 — 긴급(세션 조건) + 긴급 경보(TS 24.379 §12.1.1.3). --ui-preview-banner=incoming 이면 대표번호 착신 하나, none 이면 없음
        s.Notify.ShowBanner(new Banner { Kind = BannerKind.Alert, GroupId = "g-guard", AlertUser = "1009", Title = "긴급 경보 — 경비", Subtitle = "1009 오경비", CanCancel = true });
        s.Notify.ShowBanner(new Banner { Kind = BannerKind.Emergency, GroupId = "g-ops", Session = ops, Title = "긴급 — 상황실", Subtitle = "1006 박경장" });
        if (banner == "incoming")
        {
            var ring = new SessionItem(Ci(20, CallState.Incoming, "tel:+821022223333", false, "", dir: CallDir.Incoming, called: "tel:7000"), AccountKind.Volte, Operation.Incoming) { Title = "010-2222-3333" };
            ring.Tick(now); s.Sessions.Add(ring);
            s.Notify.ShowBanner(new Banner { Kind = BannerKind.PilotIncoming, Title = "대표번호 7000 착신", Subtitle = "010-2222-3333", Session = ring });
            foreach (var b in s.Notify.Banners.Where(b => b.IsEmergency).ToList()) s.Notify.RemoveBanner(b);
        }
        else if (banner == "none") foreach (var b in s.Notify.Banners.ToList()) s.Notify.RemoveBanner(b);
        RestoreFromSnapshot();
        patrol.Floor = new FloorInfo(FloorState.Speaking, Array.Empty<Talker>(), true, 0, -1, 0, "", 0, 1, 0, 0);
        patrol.Speaker = "나"; patrol.SpeakerSince = now.AddSeconds(-11); patrol.TalkGauge = 0.6;
        patrol.Tick(now);
        if (PttChannels.Cards.FirstOrDefault(c => c.Id == "g-patrol1") is { } p1) PttChannels.SetSingleTarget(p1);
        if (PttChannels.Cards.FirstOrDefault(c => c.IsAdhoc) is { } ad) PttChannels.ToggleTargetCommand.Execute(ad);
        if (s.Groups.FirstOrDefault(g => g.Id == "g-patrol1") is { } pg) McData.SeedPreview(pg);
        PttChannels.SetUnread(g => g.Id == "g-ops" ? 3 : 0);
        PttChannels.Tick(); TalkBar.Refresh(); Scoped.Rebuild(); CallActivity.Rebuild(); Records.Rebuild();
        Records.Open("7003");
    }

    /// <summary>--ui-preview-screen=groups 와 함께 — [PTT 그룹] 화면에 새 그룹 폼(능력·우선순위·확인 통화·멤버 역할 전부)을 세 멤버로 연다.</summary>
    public void SeedGroupFormPreview()
    {
        var form = new GroupEditViewModel(Session, null) { Name = "3번 게이트 대응" };
        form.AddMembers(Users.Users.Where(u => u.Name is "이순경" or "윤순경" or "최순경").Select(u => (u.Number, u.Name)));
        GroupsScreen.OpenPreview(form);
    }

    /// <summary>표본 위에서 모드·패널·키패드를 골라 그린다(스크린숏 점검).</summary>
    public void ApplyPreview(string? mode, string? panel, bool keypad)
    {
        if (mode is "call") Mode = "call";
        switch (panel)
        {
            case "channel": if (PttChannels.Cards.FirstOrDefault(c => c.Id == "g-patrol1") is { } c) OpenChannel(c, toggle: false); break;
            case "other": if (Scoped.All.FirstOrDefault(o => o.IsListening) is { } o) OpenOther(o, toggle: false); break;
            case "users":
                OpenUsers(toggle: false);
                foreach (var u in Users.Users.Where(u => u.Name is "이순경" or "윤순경" or "최순경").ToList()) Users.ToggleCommand.Execute(u);
                break;
            case "group":
                // 표본에는 PTT 계정이 없어 그룹 생성 자격 판정(CanCreateGroups)이 거짓이다 — 폼만 직접 세운다
                foreach (var u in Users.Users.Where(u => u.Name is "이순경" or "윤순경" or "최순경").ToList()) Users.ToggleCommand.Execute(u);
                var form = new GroupEditViewModel(Session, null);
                form.AddMembers(Users.Picked.Select(u => (u.Number, u.Name)));
                Panel.Group = form;
                Panel.Show(PanelView.Group, fromUsers: true);
                break;
            case "event": if (PttActivity.Rows.FirstOrDefault(r => r.Class == EventClass.Talk) is { } r) OpenEvent(r); break;
            case "dir": Mode = "call"; OpenDirectory(); break;
        }
        if (keypad) { Mode = "call"; KeypadOpen = true; }
    }
}
