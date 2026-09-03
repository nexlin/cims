// CimsUe.Platform — 오디오 엔드포인트 (CoreAudio IMMDeviceEnumerator COM interop, ue_sdk.md §6.3·§6.4)
//
// 코어(pjmedia WMME)의 장치 목록이 엔진에 줄 id 의 정본이다. 이 클래스는 그 위의 Windows 접점만 맡는다 —
// 재생/캡처 엔드포인트의 친화 이름·기본 장치 표시, IMMNotificationClient 핫플러그 통지(디바운스) → 앱이 Engine.RefreshAudioDevices.
// WMME 장치 이름은 MAXPNAMELEN(32)로 잘리므로 엔드포인트 이름과의 대응은 접두 일치(MatchEngineDevice)로 한다.
using System.Runtime.InteropServices;

namespace CimsUe.Platform;

public enum AudioFlow { Render = 0, Capture = 1 }

public sealed record AudioEndpoint(string Id, string Name, AudioFlow Flow, bool IsDefault);

public sealed class AudioEndpoints : IDisposable
{
    private const uint DEVICE_STATE_ACTIVE = 0x1;
    private const int eConsole = 0;
    private const int eCommunications = 2;
    private static readonly PROPERTYKEY PKEY_Device_FriendlyName = new(new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), 14);

    private readonly IMMDeviceEnumerator _enum;
    private readonly NotificationClient _client;
    private readonly SynchronizationContext? _sync;
    private readonly Timer _debounce;
    private bool _disposed;

    /// <summary>장치 추가/제거/상태/기본 장치 변경(300ms 디바운스). 컨텍스트가 있으면 그 스레드로.</summary>
    public event EventHandler? Changed;

    public AudioEndpoints(SynchronizationContext? context = null)
    {
        _sync = context ?? SynchronizationContext.Current;
        _enum = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
        _debounce = new Timer(_ => Raise(), null, Timeout.Infinite, Timeout.Infinite);
        _client = new NotificationClient(this);
        _enum.RegisterEndpointNotificationCallback(_client);
    }

    /// <summary>활성 엔드포인트 목록(기본 장치 표시 포함).</summary>
    public IReadOnlyList<AudioEndpoint> List(AudioFlow flow)
    {
        var result = new List<AudioEndpoint>();
        string? def = DefaultId(flow);
        _enum.EnumAudioEndpoints((int)flow, DEVICE_STATE_ACTIVE, out IMMDeviceCollection col);
        col.GetCount(out uint n);
        for (uint i = 0; i < n; ++i)
        {
            col.Item(i, out IMMDevice dev);
            try
            {
                dev.GetId(out string id);
                result.Add(new AudioEndpoint(id, FriendlyName(dev), flow, id == def));
            }
            finally { Marshal.FinalReleaseComObject(dev); }
        }
        Marshal.FinalReleaseComObject(col);
        return result;
    }

    /// <summary>기본(콘솔 역할) 엔드포인트 — 없으면 null.</summary>
    public AudioEndpoint? Default(AudioFlow flow)
    {
        try
        {
            _enum.GetDefaultAudioEndpoint((int)flow, eConsole, out IMMDevice dev);
            try
            {
                dev.GetId(out string id);
                return new AudioEndpoint(id, FriendlyName(dev), flow, true);
            }
            finally { Marshal.FinalReleaseComObject(dev); }
        }
        catch (COMException) { return null; }         // 장치 없음(E_NOTFOUND)
    }

    /// <summary>엔드포인트 이름 → 엔진(pjmedia) 장치. WMME 이름은 31자로 잘리므로 접두 일치, 방향(입력/출력 채널 수)으로 거른다.</summary>
    public static AudioDeviceInfo? MatchEngineDevice(IEnumerable<AudioDeviceInfo> devices, string endpointName, AudioFlow flow)
    {
        if (string.IsNullOrEmpty(endpointName)) return null;
        AudioDeviceInfo? best = null;
        foreach (var d in devices)
        {
            bool dir = flow == AudioFlow.Render ? d.OutputCount > 0 : d.InputCount > 0;
            if (!dir || d.Name.Length == 0) continue;
            if (endpointName == d.Name) return d;
            if (endpointName.StartsWith(d.Name, StringComparison.Ordinal) && d.Name.Length >= 31 && best is null) best = d;
        }
        return best;
    }

    private string? DefaultId(AudioFlow flow)
    {
        try
        {
            _enum.GetDefaultAudioEndpoint((int)flow, eConsole, out IMMDevice dev);
            try { dev.GetId(out string id); return id; }
            finally { Marshal.FinalReleaseComObject(dev); }
        }
        catch (COMException) { return null; }
    }

    private static string FriendlyName(IMMDevice dev)
    {
        dev.OpenPropertyStore(0 /* STGM_READ */, out IPropertyStore store);
        try
        {
            PROPERTYKEY key = PKEY_Device_FriendlyName;
            store.GetValue(ref key, out PROPVARIANT pv);
            try { return pv.vt == 31 /* VT_LPWSTR */ && pv.p != IntPtr.Zero ? Marshal.PtrToStringUni(pv.p) ?? "" : ""; }
            finally { PropVariantClear(ref pv); }
        }
        finally { Marshal.FinalReleaseComObject(store); }
    }

    private void Kick() { if (!_disposed) _debounce.Change(300, Timeout.Infinite); }

    private void Raise()
    {
        if (_disposed) return;
        if (_sync is not null) _sync.Post(_ => Changed?.Invoke(this, EventArgs.Empty), null);
        else Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try { _enum.UnregisterEndpointNotificationCallback(_client); } catch { }
        _debounce.Dispose();
        Marshal.FinalReleaseComObject(_enum);
    }

    // ── COM ──

    [DllImport("ole32.dll")] private static extern int PropVariantClear(ref PROPVARIANT pvar);

    [StructLayout(LayoutKind.Sequential)]
    private struct PROPERTYKEY
    {
        public Guid fmtid; public uint pid;
        public PROPERTYKEY(Guid f, uint p) { fmtid = f; pid = p; }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROPVARIANT
    {
        public ushort vt; public ushort r1, r2, r3;
        public IntPtr p; public IntPtr p2;
    }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    private class MMDeviceEnumeratorComObject { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceEnumerator
    {
        void EnumAudioEndpoints(int dataFlow, uint stateMask, out IMMDeviceCollection devices);
        void GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice endpoint);
        void GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        void RegisterEndpointNotificationCallback(IMMNotificationClient client);
        void UnregisterEndpointNotificationCallback(IMMNotificationClient client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDeviceCollection
    {
        void GetCount(out uint count);
        void Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMDevice
    {
        void Activate(ref Guid iid, uint clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        void OpenPropertyStore(uint stgmAccess, out IPropertyStore properties);
        void GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        void GetState(out uint state);
    }

    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore
    {
        void GetCount(out uint props);
        void GetAt(uint prop, out PROPERTYKEY key);
        void GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
        void SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
        void Commit();
    }

    [ComImport, Guid("7991EEC9-7E89-4D85-8390-6C703CEC60C0"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IMMNotificationClient
    {
        void OnDeviceStateChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, uint newState);
        void OnDeviceAdded([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        void OnDeviceRemoved([MarshalAs(UnmanagedType.LPWStr)] string deviceId);
        void OnDefaultDeviceChanged(int flow, int role, [MarshalAs(UnmanagedType.LPWStr)] string? defaultDeviceId);
        void OnPropertyValueChanged([MarshalAs(UnmanagedType.LPWStr)] string deviceId, PROPERTYKEY key);
    }

    /// <summary>통지는 MTA 스레드로 온다 — 디바운스 타이머만 건드린다.</summary>
    [ClassInterface(ClassInterfaceType.None)]
    private sealed class NotificationClient : IMMNotificationClient
    {
        private readonly AudioEndpoints _owner;
        public NotificationClient(AudioEndpoints owner) { _owner = owner; }
        public void OnDeviceStateChanged(string deviceId, uint newState) => _owner.Kick();
        public void OnDeviceAdded(string deviceId) => _owner.Kick();
        public void OnDeviceRemoved(string deviceId) => _owner.Kick();
        public void OnDefaultDeviceChanged(int flow, int role, string? defaultDeviceId) { if (role == eConsole || role == eCommunications) _owner.Kick(); }
        public void OnPropertyValueChanged(string deviceId, PROPERTYKEY key) { }
    }
}
