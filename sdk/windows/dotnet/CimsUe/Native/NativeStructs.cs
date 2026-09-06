// CimsUe — cimsue_c.h 의 구조체를 그대로 옮긴 blittable 정의 (필드 이름·순서·형이 헤더와 1:1).
//
// 문자열은 byte*(UTF-8, NUL 종료), 배열은 (포인터, 개수), 참/거짓은 int, 열거형은 int — C 의 enum 은 int 다.
// 레이아웃은 LayoutKind.Sequential 의 자연 정렬이 MSVC x64 와 같다. 어긋남은 CimsUe.Tests 의 ABI 시험이
// cimsue_struct_size() 와 대조해 잡는다. 헤더에 필드를 더하면 여기에도 같은 자리에 더한다.
using System.Runtime.InteropServices;

#pragma warning disable IDE1006 // 이름은 C 헤더를 그대로 따른다
namespace CimsUe.Native;

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_engine_config_t
{
    public byte* user_agent;
    public int log_level;
    public byte* tls_ca_pem;
    public int tls_verify_server;
    public int null_audio_device;
    public int no_vad;
    public int udp_port, tcp_port, tls_port;
    public uint clock_rate;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_account_config_t
{
    public byte* server_host;
    public int server_port;
    public int transport;
    public byte* domain;
    public byte* msisdn;
    public byte* imsi;
    public byte* auth_id;
    public byte* display_name;
    public byte* ha1;
    public byte* password;
    public int auth_scheme;
    public byte* aka_k;
    public byte* aka_opc;
    public byte* aka_amf;
    public byte** sec_mechanisms;
    public int sec_mechanism_count;
    public int media_security;
    public int expires_sec;
    public byte* contact_params;
    public int video_auto_transmit;
    public byte* mcptt_id;
    public int auto_answer_mcptt;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_call_options_t
{
    public int video;
    public int emergency;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_call_options_t
{
    public int emergency;
    public int imminent_peril;
    public int listen_only;
    public int full_duplex;
    public byte** members;
    public int member_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_header_t
{
    public byte* name;
    public byte* value;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_reg_info_t
{
    public int account_id;
    public int state;
    public int code;
    public byte* reason;
    public int expires_sec;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_mcptt_info_t
{
    public int present;
    public byte* session_type;
    public byte* request_uri;
    public byte* calling_user_id;
    public byte* calling_group_id;
    public int emergency;
    public int imminent_peril;
    public int private_call;
    public int no_floor_ctrl;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_media_source_t
{
    public uint ssrc;
    public byte* label;
    public int active;
    public float level;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_call_info_t
{
    public int call_id;
    public int account_id;
    public int dir;
    public int state;
    public byte* remote_uri;
    public byte* called_party;
    public int video;
    public int media_active;
    public int muted;
    public int listen;
    public int playback_route;
    public int last_code;
    public byte* last_reason;
    public cimsue_media_source_t* sources;
    public int source_count;
    public int is_mcptt;
    public byte* group_id;
    public cimsue_mcptt_info_t mcptt;
    public int half_duplex;
    public int listen_only;
    public byte* joined_dialog;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_talker_t
{
    public byte* id;
    public uint ssrc;
    public int self;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_floor_event_t
{
    public int kind;
    public int call_id;
    public int state;
    public int duration_sec;
    public int cause;
    public byte* cause_text;
    public int indicator;
    public int permission;
    public int queue_position;
    public int me_speaking;
    public cimsue_talker_t* talkers;
    public int talker_count;
    public int raw_type;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_floor_info_t
{
    public int state;
    public cimsue_talker_t* talkers;
    public int talker_count;
    public int can_request;
    public int indicator;
    public int queue_position;
    public int local_port;
    public byte* remote_ip;
    public int remote_port;
    public uint granted_count, taken_count, deny_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_request_result_t
{
    public int account_id;
    public long token;
    public byte* method;
    public int code;
    public byte* reason;
    public byte* etag;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dialog_info_t
{
    public int account_id;
    public byte* watched;
    public byte* id;
    public byte* call_id;
    public byte* local_tag;
    public byte* remote_tag;
    public byte* direction;
    public byte* state;
    public byte* remote_identity;
    public int full;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_roster_entry_t
{
    public byte* uri;
    public byte* status;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_sds_message_t
{
    public int account_id;
    public byte* from_uri;
    public byte* group_uri;
    public byte* conv_id;
    public byte* msg_id;
    public long time_sec;
    public int disposition_req;
    public byte* text;
    public int notification;
    public int notif_type;
    public int fd;
    public byte* file_url;
    public byte* file_name;
    public byte* file_type;
    public long file_size;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_stream_stats_t
{
    public uint rx_packets, rx_bytes, rx_loss, rx_discard;
    public uint tx_packets, tx_bytes;
    public int valid;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_audio_device_info_t
{
    public int id;
    public byte* name;
    public byte* driver;
    public uint input_count;
    public uint output_count;
}

/// <summary>Listener 가상함수 1:1 의 함수 포인터 한 벌 + user. 코어 이벤트 스레드에서 호출된다.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_listener_t
{
    public void* user;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, void> on_log;
    public delegate* unmanaged[Cdecl]<void*, cimsue_reg_info_t*, void> on_reg_state;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_incoming_call;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_call_state;
    public delegate* unmanaged[Cdecl]<void*, cimsue_call_info_t*, void> on_call_media;
    public delegate* unmanaged[Cdecl]<void*, cimsue_floor_event_t*, void> on_floor;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, cimsue_roster_entry_t*, int, int, void> on_roster;
    public delegate* unmanaged[Cdecl]<void*, cimsue_dialog_info_t*, void> on_dialog_info;
    public delegate* unmanaged[Cdecl]<void*, cimsue_sds_message_t*, void> on_sds;
    public delegate* unmanaged[Cdecl]<void*, cimsue_request_result_t*, void> on_request_result;
    public delegate* unmanaged[Cdecl]<void*, int, byte*, byte*, byte*, void> on_message;
    public delegate* unmanaged[Cdecl]<void*, void> on_engine_stopped;
}

// ── CSC 설정 평면 (csc.h) ──

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_csc_endpoint_t
{
    public byte* host;
    public int port;
    public byte* client_id;
    public byte* redirect_uri;
    public byte* scope;
    public byte* ca_pem;
    public int verify_server;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_token_set_t
{
    public byte* access_token;
    public byte* token_type;
    public byte* refresh_token;
    public byte* id_token;
    public byte* scope;
    public int expires_in_sec;
}

[StructLayout(LayoutKind.Sequential)]
internal struct cimsue_service_endpoint_t
{
    public int transport;
    public int port;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_service_profile_t
{
    public byte* kind;
    public byte* sip_host;
    public int sip_port;
    public int transport;
    public cimsue_service_endpoint_t* transports;
    public int transport_count;
    public int enforced;
    public int media_security;
    public byte* domain;
    public byte* msisdn;
    public byte* imsi;
    public byte* auth_id;
    public byte* sip_ha1;
    public byte* mcptt_id;
    public int auth_scheme;
    public byte* aka_k;
    public byte* aka_opc;
    public byte* aka_amf;
    public byte** sec_mechanisms;
    public int sec_mechanism_count;
    public int max_payload_sds_cplane_bytes;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_member_t
{
    public byte* user_id;
    public byte* name;
    public byte* volte_aor;
    public byte* ptt_id;
    public byte* extension;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_target_t
{
    public byte* id;
    public byte* uri;
    public byte* name;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_dispatch_profile_t
{
    public int present;
    public byte* group_id;
    public byte* group_name;
    public byte* pilot_id;
    public byte* monitor_scope;
    public byte* ptt_listen;
    public byte* listen_visibility;
    public cimsue_dispatch_member_t* members;
    public int member_count;
    public cimsue_dispatch_target_t* ptt_targets;
    public int ptt_target_count;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_profile_t
{
    public byte* display_name;
    public byte* login_id;
    public byte* country_code;
    public byte* csc_host;
    public int csc_port;
    public cimsue_service_profile_t* services;
    public int service_count;
    public cimsue_dispatch_profile_t dispatch;
    public int allow_group_creation;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_summary_t
{
    public byte* uri;
    public byte* display_name;
    public byte* etag;
    public int member_count;
    public int is_owner;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_xcap_doc_t
{
    public byte* body;
    public byte* etag;
    public int not_modified;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_member_t
{
    public byte* uri;
    public byte* display_name;
    public byte* role;
    public int priority;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct cimsue_group_doc_t
{
    public byte* uri;
    public byte* display_name;
    public byte* etag;
    public cimsue_group_member_t* members;
    public int member_count;
    public byte* session_type;
    public int video_enabled;
    public int encryption;
    public int emergency_call;
    public int emergency_alert;
    public int allow_sds;
    public int allow_fd;
    public int require_affiliation;
    public int priority;
    public int max_participants;
    public byte* org_code;
    public byte* authorized_user;
}

/// <summary>cimsue_struct_id_t — ABI 자기검사용 구조체 id (헤더와 같은 순서).</summary>
internal enum cimsue_struct_id_t
{
    ENGINE_CONFIG = 0, ACCOUNT_CONFIG, CALL_OPTIONS, GROUP_CALL_OPTIONS, HEADER, REG_INFO, MCPTT_INFO, MEDIA_SOURCE, CALL_INFO,
    TALKER, FLOOR_EVENT, FLOOR_INFO, REQUEST_RESULT, DIALOG_INFO, ROSTER_ENTRY, SDS_MESSAGE, STREAM_STATS, AUDIO_DEVICE_INFO,
    LISTENER, CSC_ENDPOINT, TOKEN_SET, SERVICE_ENDPOINT, SERVICE_PROFILE, DISPATCH_PROFILE, PROFILE, GROUP_SUMMARY, XCAP_DOC,
    DISPATCH_MEMBER, DISPATCH_TARGET, GROUP_MEMBER, GROUP_DOC,
    COUNT_,
}
