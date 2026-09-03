// ③ 오른쪽 아래 — SMS·LMS: SIP MESSAGE text/plain 1:1 (§4.3). 발신 token 으로 최종 응답 상관. 외부망 번호는 게이트웨이 부재로 전송 비활성(§13).
using CimsUe;
using DispatchDesktop.Converters;
using DispatchDesktop.Models;
using DispatchDesktop.Services;

namespace DispatchDesktop.ViewModels;

public sealed class SmsMessagesViewModel : MessagesViewModelBase
{
    public const int SmsLimit = 70;

    public SmsMessagesViewModel(DispatchSession s) : base(s, MessageKind.Sms)
    {
        s.SipMessageReceived += (_, m) => OnMessage(m);
        s.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(DispatchSession.CanSms)) OnPropertyChanged(nameof(GatewayText)); };
    }

    public string GatewayText => "외부망 게이트웨이 미구성";
    public string CountText => Input.Length > SmsLimit ? $"{Input.Length}자 LMS" : $"{Input.Length}/{SmsLimit} SMS";
    public bool SelectedIsExternal => Selected?.IsExternal == true;

    protected override bool SendAllowed(MessageThread t) => !t.IsExternal && S.CanSms;

    protected override void OnPropertyChanged(System.ComponentModel.PropertyChangedEventArgs e)
    {
        base.OnPropertyChanged(e);
        if (e.PropertyName == nameof(Input)) base.OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs(nameof(CountText)));
        if (e.PropertyName == nameof(Selected)) base.OnPropertyChanged(new System.ComponentModel.PropertyChangedEventArgs(nameof(SelectedIsExternal)));
    }

    private void OnMessage(SipMessage m)
    {
        if (!m.ContentType.StartsWith("text/plain", StringComparison.OrdinalIgnoreCase)) return;
        if (S.Volte is null || m.AccountId != S.Volte.Id) return;
        string key = UserPartConverter.UserPart(m.FromUri);
        Put(new Message { Kind = MessageKind.Sms, ThreadKey = key, Direction = MessageDirection.In, Peer = m.FromUri, PeerName = S.Directory.NameOf(key), Text = m.Body }, persist: true);
        S.Activity.Add(ActivityPanel.Call, ActivityKind.Sms, $"문자 {S.Directory.Label(key)}", m.Body.Length > 40 ? "\"" + m.Body[..39] + "…\"" : "\"" + m.Body + "\"", number: key);
    }

    protected override void SendCore()
    {
        if (!CanSend || Selected is null) return;
        string text = Input.Trim();
        var r = S.SendSms(Selected.Key, text);
        var msg = new Message
        {
            Kind = MessageKind.Sms, ThreadKey = Selected.Key, Direction = MessageDirection.Out, Peer = Selected.Key, Text = text,
            Token = r.Ok ? r.Value : 0, State = r.Ok ? SendState.Pending : SendState.Failed, Read = true,
        };
        Put(msg, persist: true);
        Input = "";
    }

    protected override void ResendCore(Message m)
    {
        if (m.State != SendState.Failed || !m.IsOut) return;
        var r = S.SendSms(m.ThreadKey, m.Text);
        m.Token = r.Ok ? r.Value : 0;
        m.State = r.Ok ? SendState.Pending : SendState.Failed;
        S.Messages.UpdateState(m.Id, m.State);
        S.Messages.UpdateToken(m.Id, m.Token);
    }

    protected override void OnRequestCompleted(RequestResult r)
    {
        if (r.Method != "MESSAGE" || S.Volte is null || r.AccountId != S.Volte.Id) return;
        var m = ThreadMap.Values.SelectMany(t => t.Messages).FirstOrDefault(x => x.IsOut && x.Token == r.Token && x.State == SendState.Pending);
        if (m is null) return;
        bool ok = r.Code is >= 200 and < 300;
        m.State = ok ? SendState.Sent : SendState.Failed;
        S.Messages.UpdateState(m.Id, m.State);
        if (!ok) S.Notify.Error(ResponseText.Describe(ResponseText.Area.Sms, r.Code, r.Reason), $"{r.Code} {r.Reason}");
    }

    public void OpenNumber(string number)
    {
        string key = UserPartConverter.UserPart(number);
        Selected = Thread(key, S.Directory.Label(key), false, S.Directory.IsExternal(key));
    }
}
